#pragma once
// =============================================================================================
// RenderCulling : カリングと負荷削減の設定（ヘッダーのみ。.vcxproj への追加不要）
//   ※ オブジェクトの可視判定は GPU（GpuObjectCulling / GpuInstanceCullLodCS / GPU Driven）へ移した。
//     ここの IsEntityVisible は描画経路からは使わない。設定値（視錐台・画面サイズ・描画距離）は GPU 側が参照する。
//   ・オブジェクト：視錐台カリング（ワールド AABB × 6 平面）、画面サイズカリング（投影半径が小さいもの）、
//     描画距離カリング。結果はフレームごとにエンティティ単位でキャッシュ（同じフレームの複数パスで再計算しない）
//   ・ライト：視錐台カリング（球 × 6 平面）、距離・画面サイズによるライトの LOD（境界でフェードして飛ばない）
//   ・GPU 駆動の経路（GPUVisibility）はもともと GPU で視錐台 / オクルージョン / LOD を行う。
//     ここは GPU 駆動で扱えない物（半透明・Forward・スプライト・GPU 駆動オフ時）と Velocity・ライトが対象
// =============================================================================================
#include "pch.h"
#include "rendering/scenedepth.h"
#include "camera.h"
#include "componentmanager.h"
#include "graphicsdevice.h"
#include <algorithm>
#include <cmath>
#include <vector>

class RenderCulling final
{
  public:
    struct Settings
    {
        bool ObjectFrustumCulling = true;
        bool ObjectSizeCulling = true;
        float MinObjectPixels = 1.5f; // 投影した境界球の半径（ピクセル）がこれ未満なら描かない
        float MaxDrawDistance = 0.0f; // 0 = 無制限
        bool StaticVelocitySkip =
            true; // 動いていない物は Velocity のジオメトリパスを省く（カメラの動きは全画面パスで出ている）
        bool AutoLod =
            false; // LOD コンポーネントの無いモデルにも自動生成 LOD を使う（GPU 駆動）。悪化の切り分け中のため既定 OFF
        bool SharePrimitiveGeometry = false; // 同じ形のプリミティブで頂点 / LOD を共有。悪化の切り分け中のため既定 OFF
        // ライト
        bool LightFrustumCulling = true;
        bool LightDistanceCulling = true;
        float LightMaxDistance = 250.0f;     // これより遠いローカルライトは使わない（手前 15% でフェード）
        float LightMinPixels = 2.0f;         // 影響範囲の投影半径（ピクセル）がこれ未満のローカルライトは使わない
        bool LightOcclusionCulling = true;   // MegaLights の GPU 結果で、見えている面に届かないライトを後回しにする
        bool SkipRayTracedShadowMaps = true; // DXR 影モードでは使われないローカルライトのシャドウマップを描かない
        // GPU（反射 / GI）
        float ReflectionRoughnessCutoff = 0.5f; // これより粗い面は遮蔽確認のみ行い、見えている空で近似
        int RayHitLights = 32;
        bool ReflectionCheckerboard = true; // 反射レイを半分のピクセルだけ飛ばし NRD で復元（NRD 使用時） // 反射 / GI
                                            // のヒット点で評価するライト数（カメラに近い順）
    };

    struct Stats
    {
        UINT ObjectsTested = 0, ObjectsFrustumCulled = 0, ObjectsSizeCulled = 0;
        UINT VelocitySkipped = 0;
        UINT LightsTested = 0, LightsFrustumCulled = 0, LightsDistanceCulled = 0, LightsOccluded = 0;
        UINT ShadowMapsSkipped = 0;
    };

    static Settings& GetSettings()
    {
        return s_Settings;
    }

    static const Stats& GetStats()
    {
        return s_LastStats;
    }

    static Stats& MutableStats()
    {
        return s_Stats;
    }

    // フレームの最初（カメラ確定後）に 1 回。2 回目以降は同じフレームなら何もしない
    static void BeginFrame(uint64_t frameSerial)
    {
        if (s_HasFrame && frameSerial == s_FrameSerial)
        {
            return;
        }
        s_HasFrame = true;
        s_FrameSerial = frameSerial;
        ++s_Stamp;
        s_LastStats = s_Stats;
        s_Stats = {};
        XMMATRIX view = XMMatrixIdentity(), projection = XMMatrixIdentity();
        Camera::GetCameraMatrices(Camera::GetCameraEntity(), view, projection);
        XMFLOAT4X4 p;
        XMStoreFloat4x4(&p, projection);
        const XMMATRIX inverseView = XMMatrixInverse(nullptr, view);
        XMStoreFloat3(&s_CameraPosition, inverseView.r[3]);
        ExtractPlanes(view * projection);
        // 投影半径（ピクセル）= radius * scale / distance
        const float height = static_cast<float>((std::max)(GraphicsDevice::GetSceneHeight(), 1u));
        s_PixelScale = 0.5f * height * fabsf(p._22);
    }

    // ローカル AABB（center / extents）とワールド行列 → 見えるか（フレーム内キャッシュ）
    static bool IsObjectVisible(EntityID entity, const XMFLOAT4X4& world, const XMFLOAT3& center,
                                const XMFLOAT3& extents)
    {
        if (entity >= s_ObjectStamp.size())
        {
            s_ObjectStamp.resize(static_cast<size_t>(entity) + 1, 0);
            s_ObjectVisible.resize(static_cast<size_t>(entity) + 1, 1);
        }
        if (s_ObjectStamp[entity] == s_Stamp)
        {
            return s_ObjectVisible[entity] != 0;
        }
        s_ObjectStamp[entity] = s_Stamp;
        const bool visible = TestObject(world, center, extents);
        s_ObjectVisible[entity] = visible ? 1 : 0;
        return visible;
    }

    // メッシュ / スプライトの境界（MeshComponent のローカル境界 → AABBComponent → 無ければ常に可視）
    static bool IsEntityVisible(EntityID entity)
    {
        if (!ComponentManager::HasComponent<TransformComponent>(entity))
        {
            return true;
        }
        const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
        if (ComponentManager::HasComponent<MeshComponent>(entity))
        {
            const auto& mesh = ComponentManager::GetComponentUnchecked<MeshComponent>(entity);
            if (mesh.HasLocalBounds)
            {
                return IsObjectVisible(entity, transform.WorldMatrix, mesh.LocalBoundsCenter, mesh.LocalBoundsExtents);
            }
        }
        if (ComponentManager::HasComponent<AABBComponent>(entity))
        {
            const auto& bounds = ComponentManager::GetComponentUnchecked<AABBComponent>(entity);
            if (bounds.Extents.x > 0.0f || bounds.Extents.y > 0.0f || bounds.Extents.z > 0.0f)
            {
                return IsObjectVisible(entity, transform.WorldMatrix, bounds.Center, bounds.Extents);
            }
        }
        return true;
    }

    // ライト：0 = 使わない、(0, 1] = 強度に掛ける係数（距離フェード）
    static float LightWeight(const XMFLOAT3& position, float range, bool directional)
    {
        if (directional)
        {
            return 1.0f;
        }
        ++s_Stats.LightsTested;
        const float radius = (std::max)(range, 0.01f);
        const XMVECTOR center = XMLoadFloat3(&position);
        if (s_Settings.LightFrustumCulling)
        {
            for (const XMFLOAT4& plane : s_Planes)
            {
                if (XMVectorGetX(XMPlaneDotCoord(XMLoadFloat4(&plane), center)) < -radius)
                {
                    ++s_Stats.LightsFrustumCulled;
                    return 0.0f;
                }
            }
        }
        if (!s_Settings.LightDistanceCulling)
        {
            return 1.0f;
        }
        const float distance = XMVectorGetX(XMVector3Length(XMVectorSubtract(center, XMLoadFloat3(&s_CameraPosition))));
        const float surface = (std::max)(distance - radius, 0.0f); // 影響範囲の表面までの距離
        if (surface > 0.0f && radius * s_PixelScale / (std::max)(distance, 0.01f) < s_Settings.LightMinPixels)
        {
            ++s_Stats.LightsDistanceCulled;
            return 0.0f;
        }
        const float maxDistance = s_Settings.LightMaxDistance;
        if (maxDistance <= 0.0f)
        {
            return 1.0f;
        }
        if (surface >= maxDistance)
        {
            ++s_Stats.LightsDistanceCulled;
            return 0.0f;
        }
        const float fadeStart = maxDistance * 0.85f;
        return surface <= fadeStart ? 1.0f : 1.0f - (surface - fadeStart) / (maxDistance - fadeStart);
    }

    // LightingResources::BeginFrame ごとに増える。GpuObjectCulling のフレーム判定に使う。
    static uint64_t FrameSerial()
    {
        return s_FrameSerial;
    }

    static const XMFLOAT3& CameraPosition()
    {
        return s_CameraPosition;
    }

  private:
    static void ExtractPlanes(const XMMATRIX& viewProjection)
    {
        XMFLOAT4X4 m;
        XMStoreFloat4x4(&m, viewProjection);
        // 行ベクトル（DirectXMath）用の Gribb-Hartmann。z は 0..1（逆 Z でも near / far の順が入れ替わるだけ）
        const XMFLOAT4 planes[6] = {{m._14 + m._11, m._24 + m._21, m._34 + m._31, m._44 + m._41},
                                    {m._14 - m._11, m._24 - m._21, m._34 - m._31, m._44 - m._41},
                                    {m._14 + m._12, m._24 + m._22, m._34 + m._32, m._44 + m._42},
                                    {m._14 - m._12, m._24 - m._22, m._34 - m._32, m._44 - m._42},
                                    {m._13, m._23, m._33, m._43},
                                    {m._14 - m._13, m._24 - m._23, m._34 - m._33, m._44 - m._43}};
        for (int index = 0; index < 6; ++index)
        {
            XMStoreFloat4(&s_Planes[index], SceneDepth::NormalizeFrustumPlane(XMLoadFloat4(&planes[index])));
        }
    }

    static bool TestObject(const XMFLOAT4X4& worldMatrix, const XMFLOAT3& center, const XMFLOAT3& extents)
    {
        ++s_Stats.ObjectsTested;
        // ローカル AABB → ワールド AABB（中心と |M| × extents）
        const XMMATRIX world = XMLoadFloat4x4(&worldMatrix);
        const XMVECTOR worldCenter = XMVector3TransformCoord(XMLoadFloat3(&center), world);
        XMFLOAT4X4 m;
        XMStoreFloat4x4(&m, world);
        const XMFLOAT3 e = extents;
        const XMFLOAT3 worldExtents = {fabsf(m._11) * e.x + fabsf(m._21) * e.y + fabsf(m._31) * e.z,
                                       fabsf(m._12) * e.x + fabsf(m._22) * e.y + fabsf(m._32) * e.z,
                                       fabsf(m._13) * e.x + fabsf(m._23) * e.y + fabsf(m._33) * e.z};
        if (s_Settings.ObjectFrustumCulling)
        {
            for (const XMFLOAT4& plane : s_Planes)
            {
                const float distance = XMVectorGetX(XMPlaneDotCoord(XMLoadFloat4(&plane), worldCenter));
                const float projected =
                    fabsf(plane.x) * worldExtents.x + fabsf(plane.y) * worldExtents.y + fabsf(plane.z) * worldExtents.z;
                if (distance < -projected)
                {
                    ++s_Stats.ObjectsFrustumCulled;
                    return false;
                }
            }
        }
        if (s_Settings.ObjectSizeCulling || s_Settings.MaxDrawDistance > 0.0f)
        {
            const float radius = sqrtf(worldExtents.x * worldExtents.x + worldExtents.y * worldExtents.y +
                                       worldExtents.z * worldExtents.z);
            const float distance =
                XMVectorGetX(XMVector3Length(XMVectorSubtract(worldCenter, XMLoadFloat3(&s_CameraPosition))));
            if (distance > radius)
            {
                if (s_Settings.MaxDrawDistance > 0.0f && distance - radius > s_Settings.MaxDrawDistance)
                {
                    ++s_Stats.ObjectsSizeCulled;
                    return false;
                }
                if (s_Settings.ObjectSizeCulling && radius * s_PixelScale / distance < s_Settings.MinObjectPixels)
                {
                    ++s_Stats.ObjectsSizeCulled;
                    return false;
                }
            }
        }
        return true;
    }

    static inline Settings s_Settings{};
    static inline Stats s_Stats{}, s_LastStats{};
    static inline XMFLOAT4 s_Planes[6]{};
    static inline XMFLOAT3 s_CameraPosition{};
    static inline float s_PixelScale = 500.0f;
    static inline uint32_t s_Stamp = 0;
    static inline uint64_t s_FrameSerial = 0;
    static inline bool s_HasFrame = false;
    static inline std::vector<uint32_t> s_ObjectStamp;
    static inline std::vector<uint8_t> s_ObjectVisible;
};

