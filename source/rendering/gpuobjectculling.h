#pragma once
// =============================================================================================
// GpuObjectCulling : エンティティ単位の可視判定を GPU で行う（ヘッダーのみ。.vcxproj への追加不要）
//
//   CPU の個別描画経路（3D スプライト・メッシュ・影・Velocity）で使っていた CPU の視錐台 / 画面サイズ /
//   描画距離カリングを置き換える。CPU は境界（ワールド行列とローカル AABB・影用の境界球）を並べて
//   アップロードするだけで、見えるかどうかは判定しない。
//
//   ・ビュー（メイン カメラ / 各シャドウパス）ごとに GpuObjectCullCS を1回実行し、エンティティごとの
//     述語（uint64、0 = 描かない）を書く。同じフレーム・同じ行列のビューは再利用する。
//   ・描画側は Begin(entity) / End() で SetPredication を張り、GPU が 0 を書いた物の描画・ディスパッチを
//     GPU 側で破棄する（CPU は結果を待たない）。
//   ・アニメーションの更新間引き（CPU が知る必要がある物）は、メイン カメラの結果を読み戻した値を使う
//     （数フレーム遅れ。結果が無いときは可視扱い）。
// =============================================================================================
#include "pch.h"
#include "cpuculling.h"
#include "camera.h"
#include "componentmanager.h"
#include "graphicsdevice.h"
#include "lightingresources.h"
#include "modelmanager.h"
#include "staticmodel.h"
#include "animationmodel.h"
#include "vertexdeformation.h"
#include "world.h"
#include "workgraphruntime.h"
#include <cstring>
#include <vector>

class GpuObjectCulling final
{
  public:
    static constexpr int kNoView = -1;

    // メイン カメラのビュー。必ず PSO を設定する前に呼ぶ（初回はコンピュートを実行するため）。
    static int CameraView(ID3D12GraphicsCommandList* commandList)
    {
        XMMATRIX view = XMMatrixIdentity(), projection = XMMatrixIdentity();
        Camera::GetCameraMatrices(Camera::GetCameraEntity(), view, projection);
        const XMMATRIX viewProjection = view * projection;
        XMFLOAT4X4 p;
        XMStoreFloat4x4(&p, projection);
        XMFLOAT3 cameraPosition{};
        XMStoreFloat3(&cameraPosition, XMMatrixInverse(nullptr, view).r[3]);
        const float height = static_cast<float>((std::max)(GraphicsDevice::GetSceneHeight(), 1u));

        ViewConstants constants{};
        ExtractPlanes(viewProjection, constants.Planes);
        constants.CameraPositionPixelScale = {cameraPosition.x, cameraPosition.y, cameraPosition.z,
                                              0.5f * height * fabsf(p._22)};
        constants.Mode = 0;
        const auto& settings = RenderCulling::GetSettings();
        constants.Flags = (settings.ObjectFrustumCulling ? 1u : 0u) | (settings.ObjectSizeCulling ? 2u : 0u);
        constants.MinPixels = settings.MinObjectPixels;
        constants.MaxDistance = settings.MaxDrawDistance;
        return AcquireView(commandList, constants, true);
    }

    // 現在のシャドウパスのビュー（パス外・カリング不要なら kNoView）。PSO を設定する前に呼ぶ。
    static int ShadowView(ID3D12GraphicsCommandList* commandList)
    {
        XMMATRIX transposedViewProjection{};
        if (!LightingResources::GetCurrentShadowCullMatrix(transposedViewProjection))
        {
            return kNoView;
        }
        ViewConstants constants{};
        // 転置行列の各行 = 元の行列の各列（clip = v * M）。IsShadowCasterVisible と同じ平面。
        const XMVECTOR c0 = transposedViewProjection.r[0];
        const XMVECTOR c1 = transposedViewProjection.r[1];
        const XMVECTOR c2 = transposedViewProjection.r[2];
        const XMVECTOR c3 = transposedViewProjection.r[3];
        const XMVECTOR planes[6] = {XMVectorAdd(c3, c0), XMVectorSubtract(c3, c0), XMVectorAdd(c3, c1),
                                    XMVectorSubtract(c3, c1), c2, XMVectorSubtract(c3, c2)};
        for (int i = 0; i < 6; ++i)
        {
            XMStoreFloat4(&constants.Planes[i], XMPlaneNormalize(planes[i]));
        }
        constants.Mode = 1;
        constants.Flags = 1;
        return AcquireView(commandList, constants, false);
    }

    // entity の描画を GPU の判定結果で条件付きにする。End() まで有効。
    static void Begin(ID3D12GraphicsCommandList* commandList, int view, EntityID entity)
    {
        auto& s = S();
        if (view < 0 || !commandList || !s.Predicates || entity >= s.ObjectCount ||
            static_cast<uint32_t>(view) >= s.Views.size() || s.ViewFrame != CurrentFrame())
        {
            return;
        }
        EnsurePredicationState(commandList);
        const UINT64 offset = (static_cast<UINT64>(s.Views[view].Offset) + entity) * sizeof(uint64_t);
        commandList->SetPredication(s.Predicates.Get(), offset, D3D12_PREDICATION_OP_EQUAL_ZERO);
        s.PredicationActive = true;
    }

    static void End(ID3D12GraphicsCommandList* commandList)
    {
        auto& s = S();
        if (s.PredicationActive && commandList)
        {
            commandList->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
        }
        s.PredicationActive = false;
    }

    // CPU 側で可視性が必要な処理（アニメーションの更新間引き）用。GPU の結果を読み戻した値で、
    // 数フレーム遅れる。結果が無い・範囲外は可視扱い。
    static bool WasVisibleOnGpu(EntityID entity)
    {
        auto& s = S();
        RefreshReadback();
        if (entity >= s.CpuVisible.size())
        {
            return true;
        }
        return s.CpuVisible[entity] != 0;
    }

    static void Shutdown()
    {
        auto& s = S();
        for (auto& frame : s.Frames)
        {
            if (frame.Upload && frame.Mapped)
            {
                frame.Upload->Unmap(0, nullptr);
            }
            frame.Mapped = nullptr;
            frame.Upload.Reset();
            frame.Readback.Reset();
            frame.ReadbackFence = 0;
        }
        s.Predicates.Reset();
        s.Root.Reset();
        s.Graph.Reset();
        s.Tried = false;
        s.Ready = false;
        s.Views.clear();
        s.CpuVisible.clear();
    }

  private:
    static constexpr uint32_t kMaxEntities = g_kMAX_ENTITIES;
    static constexpr UINT64 kPredicateBytes = 8ull * 1024ull * 1024ull; // 全ビュー分（8 MB）

    struct ObjectRecord
    {
        XMFLOAT4X4 World{};
        XMFLOAT4 LocalCenter{}; // w: asfloat(フラグ)
        XMFLOAT4 LocalExtents{};
        XMFLOAT4 ShadowSphere{};
    };
    static_assert(sizeof(ObjectRecord) == 112);

    struct ViewConstants
    {
        XMFLOAT4 Planes[6]{};
        XMFLOAT4 CameraPositionPixelScale{};
        uint32_t ObjectCount = 0;
        uint32_t OutputOffset = 0;
        uint32_t Mode = 0;
        uint32_t Flags = 0;
        float MinPixels = 0.0f;
        float MaxDistance = 0.0f;
        float Padding[2]{};
    };
    static_assert(sizeof(ViewConstants) == 144);

    struct ViewEntry
    {
        ViewConstants Constants{};
        uint32_t Offset = 0; // uint2 単位
    };

    struct FrameResources
    {
        ComPtr<ID3D12Resource> Upload;
        ObjectRecord* Mapped = nullptr;
        ComPtr<ID3D12Resource> Readback;
        UINT64 ReadbackFence = 0;
        uint32_t ReadbackCount = 0;
    };

    struct State
    {
        bool Tried = false;
        bool Ready = false;
        ComPtr<ID3D12RootSignature> Root;
        WorkGraph::ComputeGraph Graph;
        ComPtr<ID3D12Resource> Predicates;
        D3D12_RESOURCE_STATES PredicateState = D3D12_RESOURCE_STATE_COMMON;
        UINT64 PredicateStateFence = 0; // この値の送信までは PredicateState が有効（送信後は COMMON に戻る）
        array<FrameResources, RendererState::g_kFRAME_COUNT> Frames;
        uint64_t ViewFrame = UINT64_MAX;
        uint32_t ObjectCount = 0;
        uint32_t Stride = 0; // ビュー1つの要素数（uint2）
        vector<ViewEntry> Views;
        bool PredicationActive = false;
        bool CameraReadbackQueued = false;
        UINT64 LatestReadbackFence = 0;
        vector<uint8_t> CpuVisible;
    };

    static State& S()
    {
        static State state;
        return state;
    }

    static uint64_t CurrentFrame()
    {
        return RenderCulling::FrameSerial();
    }

    static void ExtractPlanes(const XMMATRIX& viewProjection, XMFLOAT4* outPlanes)
    {
        XMFLOAT4X4 m;
        XMStoreFloat4x4(&m, viewProjection);
        const XMFLOAT4 planes[6] = {{m._14 + m._11, m._24 + m._21, m._34 + m._31, m._44 + m._41},
                                    {m._14 - m._11, m._24 - m._21, m._34 - m._31, m._44 - m._41},
                                    {m._14 + m._12, m._24 + m._22, m._34 + m._32, m._44 + m._42},
                                    {m._14 - m._12, m._24 - m._22, m._34 - m._32, m._44 - m._42},
                                    {m._13, m._23, m._33, m._43},
                                    {m._14 - m._13, m._24 - m._23, m._34 - m._33, m._44 - m._43}};
        for (int index = 0; index < 6; ++index)
        {
            XMStoreFloat4(&outPlanes[index], XMPlaneNormalize(XMLoadFloat4(&planes[index])));
        }
    }

    static bool EnsureResources()
    {
        auto& s = S();
        if (s.Tried)
        {
            return s.Ready;
        }
        s.Tried = true;
        ID3D12Device* device = GraphicsDevice::GetDevice();
        if (!device)
        {
            return false;
        }
        ComPtr<ID3DBlob> errors;
        CD3DX12_ROOT_PARAMETER parameters[3]{};
        parameters[0].InitAsShaderResourceView(0);
        parameters[1].InitAsUnorderedAccessView(0);
        parameters[2].InitAsConstants(sizeof(ViewConstants) / 4, 0);
        CD3DX12_ROOT_SIGNATURE_DESC rootDescription{};
        rootDescription.Init(_countof(parameters), parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        ComPtr<ID3DBlob> serialized;
        if (FAILED(D3D12SerializeRootSignature(&rootDescription, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)) ||
            FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                               IID_PPV_ARGS(&s.Root))))
        {
            return false;
        }
        if (!s.Graph.Initialize(device, s.Root.Get(), L"shader/hlsl/GpuObjectCullWorkGraph.hlsl", L"CullObjects"))
        {
            return false;
        }
        const auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const auto predicateDescription =
            CD3DX12_RESOURCE_DESC::Buffer(kPredicateBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &predicateDescription,
                                                   D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                   IID_PPV_ARGS(&s.Predicates))))
        {
            return false;
        }
        s.Predicates->SetName(L"GPU Object Culling Predicates");

        const auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        const auto readbackHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
        const auto uploadDescription = CD3DX12_RESOURCE_DESC::Buffer(sizeof(ObjectRecord) * kMaxEntities);
        const auto readbackDescription = CD3DX12_RESOURCE_DESC::Buffer(sizeof(uint64_t) * kMaxEntities);
        const D3D12_RANGE noRead{0, 0};
        for (auto& frame : s.Frames)
        {
            if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDescription,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                       IID_PPV_ARGS(&frame.Upload))) ||
                FAILED(frame.Upload->Map(0, &noRead, reinterpret_cast<void**>(&frame.Mapped))) ||
                FAILED(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDescription,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&frame.Readback))))
            {
                return false;
            }
        }
        s.Ready = true;
        return true;
    }

    // バッファの状態は ExecuteCommandLists の終わりに COMMON へ戻る（バッファの減衰）。
    // 送信をまたいだら COMMON とみなす。
    static D3D12_RESOURCE_STATES CurrentPredicateState()
    {
        auto& s = S();
        if (s.PredicateStateFence != GraphicsDevice::GetPendingSubmissionFence())
        {
            s.PredicateState = D3D12_RESOURCE_STATE_COMMON;
            s.PredicateStateFence = GraphicsDevice::GetPendingSubmissionFence();
        }
        return s.PredicateState;
    }

    static void TransitionPredicates(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES after)
    {
        auto& s = S();
        const D3D12_RESOURCE_STATES before = CurrentPredicateState();
        if (before == after)
        {
            if (after == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
            {
                const auto uav = CD3DX12_RESOURCE_BARRIER::UAV(s.Predicates.Get());
                commandList->ResourceBarrier(1, &uav);
            }
            return;
        }
        const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(s.Predicates.Get(), before, after);
        commandList->ResourceBarrier(1, &barrier);
        s.PredicateState = after;
    }

    static void EnsurePredicationState(ID3D12GraphicsCommandList* commandList)
    {
        if (CurrentPredicateState() != D3D12_RESOURCE_STATE_PREDICATION)
        {
            TransitionPredicates(commandList, D3D12_RESOURCE_STATE_PREDICATION);
        }
    }

    // フレームの最初のビュー要求で、全エンティティの境界をアップロードする（判定はしない）。
    static void BeginFrameIfNeeded()
    {
        auto& s = S();
        const uint64_t frame = CurrentFrame();
        if (s.ViewFrame == frame)
        {
            return;
        }
        s.ViewFrame = frame;
        s.Views.clear();
        s.CameraReadbackQueued = false;

        FrameResources& resources = s.Frames[GraphicsDevice::GetFrameIndex() % RendererState::g_kFRAME_COUNT];
        uint32_t count = 0;
        for (EntityID entity : World::GetView<TransformComponent>())
        {
            count = (std::max)(count, static_cast<uint32_t>(entity) + 1u);
        }
        count = (std::min)(count, kMaxEntities);
        s.ObjectCount = count;
        s.Stride = (std::max)(count, 1u);
        if (count == 0)
        {
            return;
        }
        memset(resources.Mapped, 0, sizeof(ObjectRecord) * count);
        for (EntityID entity : World::GetView<TransformComponent>())
        {
            if (entity >= count)
            {
                continue;
            }
            WriteRecord(entity, resources.Mapped[entity]);
        }
    }

    static void WriteRecord(EntityID entity, ObjectRecord& record)
    {
        const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
        record.World = transform.WorldMatrix;
        uint32_t flags = 0;

        // カメラ用のローカル境界（以前の RenderCulling / InstancingSystem と同じ優先順位）。
        XMFLOAT3 center{}, extents{};
        bool hasBounds = false;
        const bool cullingDisabled =
            ComponentManager::HasComponent<InstancingComponent>(entity) &&
            !ComponentManager::GetComponentUnchecked<InstancingComponent>(entity).EnableFrustumCulling;
        if (!cullingDisabled)
        {
            if (VertexDeformation::TryGetLocalBounds(entity, center, extents))
            {
                hasBounds = true;
            }
            else if (ComponentManager::HasComponent<MeshComponent>(entity) &&
                     ComponentManager::GetComponentUnchecked<MeshComponent>(entity).HasLocalBounds)
            {
                const auto& mesh = ComponentManager::GetComponentUnchecked<MeshComponent>(entity);
                center = mesh.LocalBoundsCenter;
                extents = mesh.LocalBoundsExtents;
                hasBounds = true;
            }
            else if (ComponentManager::HasComponent<AABBComponent>(entity))
            {
                const auto& bounds = ComponentManager::GetComponentUnchecked<AABBComponent>(entity);
                if (bounds.Extents.x > 0.0f || bounds.Extents.y > 0.0f || bounds.Extents.z > 0.0f)
                {
                    center = bounds.Center;
                    extents = bounds.Extents;
                    hasBounds = true;
                }
            }
            if (!hasBounds && ComponentManager::HasComponent<StaticModelComponent>(entity))
            {
                if (StaticModelResource* model = ModelManager::GetStaticModel(
                        ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity).ModelId))
                {
                    center = model->GetAabbCenter();
                    extents = model->GetAabbExtents();
                    hasBounds = true;
                }
            }
            if (!hasBounds && ComponentManager::HasComponent<AnimationModelComponent>(entity))
            {
                if (AnimationModelResource* model = ModelManager::GetAnimModel(
                        ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity).ModelId))
                {
                    center = model->GetAabbCenter();
                    extents = model->GetAabbExtents();
                    hasBounds = true;
                }
            }
            if (!hasBounds && ComponentManager::HasComponent<SpriteComponent>(entity))
            {
                const auto& sprite = ComponentManager::GetComponentUnchecked<SpriteComponent>(entity);
                if (sprite.Is3D && sprite.HasLocalBounds)
                {
                    center = sprite.LocalBoundsCenter;
                    extents = sprite.LocalBoundsExtents;
                    hasBounds = true;
                }
            }
        }
        if (hasBounds)
        {
            flags |= 1u;
            record.LocalCenter = {center.x, center.y, center.z, 0.0f};
            record.LocalExtents = {extents.x, extents.y, extents.z, 0.0f};
        }
        XMFLOAT4 sphere{};
        if (LightingResources::GetShadowCasterSphere(entity, sphere))
        {
            flags |= 2u;
            record.ShadowSphere = sphere;
        }
        memcpy(&record.LocalCenter.w, &flags, sizeof(flags));
    }

    static int AcquireView(ID3D12GraphicsCommandList* commandList, ViewConstants constants, bool camera)
    {
        auto& s = S();
        if (!commandList || !EnsureResources())
        {
            return kNoView;
        }
        BeginFrameIfNeeded();
        if (s.ObjectCount == 0)
        {
            return kNoView;
        }
        constants.ObjectCount = s.ObjectCount;
        for (size_t i = 0; i < s.Views.size(); ++i)
        {
            ViewConstants cached = s.Views[i].Constants;
            cached.OutputOffset = constants.OutputOffset;
            if (memcmp(&cached, &constants, sizeof(constants)) == 0)
            {
                return static_cast<int>(i);
            }
        }
        const UINT64 capacity = kPredicateBytes / sizeof(uint64_t);
        const UINT64 offset = static_cast<UINT64>(s.Views.size()) * s.Stride;
        if (offset + s.Stride > capacity)
        {
            return kNoView; // ビュー数が多すぎる：このビューは判定なし（すべて描く）
        }
        constants.OutputOffset = static_cast<uint32_t>(offset);
        if (s.PredicationActive)
        {
            End(commandList);
        }

        FrameResources& resources = s.Frames[GraphicsDevice::GetFrameIndex() % RendererState::g_kFRAME_COUNT];
        TransitionPredicates(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commandList->SetComputeRootSignature(s.Root.Get());
        commandList->SetComputeRootShaderResourceView(0, resources.Upload->GetGPUVirtualAddress());
        commandList->SetComputeRootUnorderedAccessView(1, s.Predicates->GetGPUVirtualAddress());
        commandList->SetComputeRoot32BitConstants(2, sizeof(ViewConstants) / 4, &constants, 0);
        const uint32_t dispatchGrid[3]{(s.ObjectCount + 63) / 64, 1, 1};
        if (!s.Graph.DispatchCpu(commandList, GraphicsDevice::GetFrameIndex(), dispatchGrid, sizeof(dispatchGrid)))
        {
            throw std::runtime_error("GPU object culling Work Graph dispatch failed");
        }

        // メイン カメラの結果を CPU 用に読み戻す（アニメーションの更新間引き用）。
        if (camera && !s.CameraReadbackQueued && resources.Readback)
        {
            TransitionPredicates(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);
            commandList->CopyBufferRegion(resources.Readback.Get(), 0, s.Predicates.Get(), offset * sizeof(uint64_t),
                                          static_cast<UINT64>(s.ObjectCount) * sizeof(uint64_t));
            resources.ReadbackFence = GraphicsDevice::GetPendingSubmissionFence();
            resources.ReadbackCount = s.ObjectCount;
            s.CameraReadbackQueued = true;
        }
        TransitionPredicates(commandList, D3D12_RESOURCE_STATE_PREDICATION);

        ViewEntry entry{};
        entry.Constants = constants;
        entry.Offset = constants.OutputOffset;
        s.Views.push_back(entry);
        return static_cast<int>(s.Views.size() - 1);
    }

    static void RefreshReadback()
    {
        auto& s = S();
        if (!s.Ready)
        {
            return;
        }
        const UINT64 completed = GraphicsDevice::GetCompletedFence();
        FrameResources* newest = nullptr;
        for (auto& frame : s.Frames)
        {
            if (frame.ReadbackFence != 0 && frame.ReadbackFence <= completed &&
                frame.ReadbackFence > s.LatestReadbackFence && (!newest || frame.ReadbackFence > newest->ReadbackFence))
            {
                newest = &frame;
            }
        }
        if (!newest)
        {
            return;
        }
        const D3D12_RANGE range{0, static_cast<SIZE_T>(newest->ReadbackCount) * sizeof(uint64_t)};
        void* mapped = nullptr;
        if (FAILED(newest->Readback->Map(0, &range, &mapped)))
        {
            return;
        }
        const auto* values = static_cast<const uint64_t*>(mapped);
        s.CpuVisible.assign(newest->ReadbackCount, 1);
        for (uint32_t i = 0; i < newest->ReadbackCount; ++i)
        {
            s.CpuVisible[i] = values[i] != 0 ? 1 : 0;
        }
        const D3D12_RANGE noWrite{0, 0};
        newest->Readback->Unmap(0, &noWrite);
        s.LatestReadbackFence = newest->ReadbackFence;
    }
};

