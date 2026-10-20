#pragma once
// オクタヘドラル・インポスター（ヘッダーのみ。vcxproj への追加不要）。
//
// ・LODComponent::UseImpostor が有効な静的モデルを、切替距離より遠いときにカメラ正対の四角形1枚で描く。
// ・アトラスは (モデル, 代替テクスチャ, 分割数, 解像度, 半球) ごとに1つ。初回要求の次フレームで GPU ベイクする
//   （1フレーム1件まで。ベイク完了まではメッシュのまま描画）。
// ・ピクセルシェーダーは3フレームをブレンドし、焼き込んだ深度で視差補正し、実際の表面深度を
//   SV_DepthGreaterEqual で出力する。GBuffer / Visibility Buffer / フォワードの3経路に対応し、
//   ディファードのライト・影・GI・SSAO はメッシュと同じ扱いになる。
// ・シャドウマップ・レイトレーシング（BLAS）は従来どおり実メッシュを使う（影の形が崩れない）。
// ・置き換え（距離）と可視判定はすべて GPU。CPU はアトラスの準備状態とグループ分けだけを扱う。
//   GPU Driven のモデルは GPUOcclusionCS（結果 w = 4）、インスタンス描画のモデルは GpuInstanceCullLodCS が
//   判定し、インポスターは ExecuteIndirect で描く（インスタンス数は GPU が書く）。
#include "rendererstate.h"
#include "componentmanager.h"
#include "staticmodel.h"
#include "modelmanager.h"
#include "camera.h"
#include "graphicsdevice.h"
#include "shaderpipelines.h"
#include "psomanager.h"
#include "frameconstants.h"
#include "materialbindings.h"
#include "texturemanager.h"
#include "shaderdescription.h"
#include "renderersettings.h"
#include "visibilitybuffer.h"
#include "world.h"
#include "gpuscene/gpuscene.h"
#include "gpudriven/gpudrivenrenderer.h"
#include "visibility/gpuvisibility.h"
#include "instancingsystem.h"
#include "materialsystem.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

class ImpostorRenderer : protected RendererState
{
  public:
    // 実行時の全体設定（エディターの LOD 欄から変更）。
    struct Settings
    {
        bool Enabled = true;        // false でインポスター置き換えを全停止（A/B 比較用）
        float AlphaCutoff = 0.5f;   // 3フレームブレンド後の被覆率しきい値
        float MipBias = 0.0f;       // 正で遠景のちらつきを抑える（ぼける）
        uint32_t EvictFrames = 900; // この間使われなかったアトラスを解放
    };

    struct Statistics
    {
        uint32_t ReadyAtlases = 0;
        uint32_t PendingAtlases = 0;
        uint32_t FailedAtlases = 0;
        uint64_t AtlasBytes = 0;
        uint32_t DrawnInstances = 0; // 直近フレーム
        uint32_t DrawCalls = 0;      // 直近フレーム
    };

    enum class AtlasState : uint8_t
    {
        None,
        Pending,
        Ready,
        Failed
    };

    static constexpr uint32_t kDescriptorCount = RendererState::g_kIMPOSTOR_DESCRIPTOR_COUNT;
    static constexpr uint32_t kScratchDescriptorsPerFrame = 32;
    static constexpr uint32_t kMaxInstancesPerFrame = 32768;
    static constexpr uint32_t kDilateRadius = 6;

    static Settings& GetSettings()
    {
        return S().Config;
    }

    static Statistics GetStatistics()
    {
        auto& s = S();
        Statistics stats{};
        for (const auto& [key, atlas] : s.Atlases)
        {
            (void)key;
            if (atlas.State == AtlasState::Ready)
            {
                ++stats.ReadyAtlases;
                stats.AtlasBytes += AtlasBytes(atlas.Resolution, atlas.MipLevels);
            }
            else if (atlas.State == AtlasState::Pending)
            {
                ++stats.PendingAtlases;
            }
            else if (atlas.State == AtlasState::Failed)
            {
                ++stats.FailedAtlases;
            }
        }
        stats.DrawnInstances = s.LastFrameInstances;
        stats.DrawCalls = s.LastFrameDrawCalls;
        return stats;
    }

    static bool WantsImpostor(EntityID entity)
    {
        if (!ComponentManager::HasComponent<LODComponent>(entity) ||
            !ComponentManager::HasComponent<StaticModelComponent>(entity))
        {
            return false;
        }
        return ComponentManager::GetComponentUnchecked<LODComponent>(entity).UseImpostor;
    }

    // インスタンス描画と同じ規則で、エンティティのテクスチャ（メッシュにテクスチャが無いときの代替）を求める。
    static int ResolveEntityTexture(EntityID entity)
    {
        if (ComponentManager::HasComponent<MaterialComponent>(entity))
        {
            const auto& material = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
            if (material.UseTexture && material.TextureID >= 0)
            {
                return material.TextureID;
            }
        }
        return TextureManager::GetDefaultTextureIndex();
    }

    // エディター表示用。
    static AtlasState QueryState(EntityID entity)
    {
        if (!WantsImpostor(entity))
        {
            return AtlasState::None;
        }
        StaticModelResource* model = ModelManager::GetStaticModel(
            ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity).ModelId);
        if (!model)
        {
            return AtlasState::None;
        }
        const auto key = MakeKey(model, ComponentManager::GetComponentUnchecked<LODComponent>(entity),
                                 ResolveEntityTexture(entity));
        const auto found = S().Atlases.find(key);
        return found == S().Atlases.end() ? AtlasState::None : found->second.State;
    }

    // モデル（全エンティティ共通）のアトラスを破棄し、次の要求で再ベイクさせる。
    static void InvalidateModel(const StaticModelResource* model)
    {
        auto& s = S();
        for (auto it = s.Atlases.begin(); it != s.Atlases.end();)
        {
            if (it->first.Model == model)
            {
                RetireAtlas(it->second);
                it = s.Atlases.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    static void InvalidateAll()
    {
        auto& s = S();
        for (auto& [key, atlas] : s.Atlases)
        {
            (void)key;
            RetireAtlas(atlas);
        }
        s.Atlases.clear();
        if (s.BakeFailed && !s.BakeReady)
        {
            s.BakeResourcesTried = false; // シェーダー修正後の再試行
        }
        s.BakeFailed = false;
        s.PassFailures = 0;
    }

    // ---- 描画（判定はすべて GPU） ----------------------------------------------------------------
    // CPU が行うのは「アトラスが準備済みか」（状態）と、候補の並べ替え・グループ分けだけ。
    // 置き換えの距離判定・視錐台/オクルージョンは GPU で行い、ExecuteIndirect で描く。
    //   ・GPU Driven のモデル    : GPUOcclusionCS が w = 4 を書き、ImpostorCullCS がインスタンスを追加
    //   ・インスタンス描画のモデル: GpuInstanceCullLodCS が直接インスタンスを追加
    static constexpr uint32_t kMaxGroups = 1024;
    static constexpr uint32_t kMaxGpuCandidates = 8192;
    static constexpr uint32_t kMaxCandidateSlots = 32768;

    struct InstancedInfo
    {
        XMFLOAT4 CenterDistance{}; // xyz: モデルのローカル中心, w: 切替距離（0 = なし）
        uint32_t Group = 0;
        uint32_t Base = 0;
        uint32_t Capacity = 0;
        uint32_t Valid = 0;
    };

    // InstancingSystem::Draw（不透明・メイン）の先頭で呼ぶ。PSO を解決し、GPU Driven 側の候補を集める。
    static void BeginPass(bool deferredOpaque, bool receivingPostProcessOnly)
    {
        auto& s = S();
        s.Groups.clear();
        s.GroupLookup.clear();
        s.GpuCandidates.clear();
        s.InstancedLookup.clear();
        s.PassPso = nullptr;
        s.LayoutReady = false;
        s.GpuPrepared = false;
        const UINT frameIndex = GraphicsDevice::GetFrameIndex() % g_kFRAME_COUNT;
        if (s.UploadFrame != frameIndex)
        {
            s.UploadFrame = frameIndex;
            s.CandidateCursor = 0;
            s.SlotCursor = 0;
            s.GroupCursor = 0;
        }
        if (!s.Config.Enabled || s.BakeFailed)
        {
            return;
        }
        const uint32_t passBit = CurrentPassBit();
        if (s.PassFailures & passBit)
        {
            return;
        }
        ShaderDescription resource{};
        resource.vsPath = "shader/hlsl/build/ImpostorVS.cso";
        resource.psPath = "shader/hlsl/build/ImpostorPS.cso";
        resource.isModel = true;
        resource.cullMode = D3D12_CULL_MODE_NONE;
        s.PassPso = PsoManager::GetOrCreateGraphicsPso(resource);
        if (!s.PassPso)
        {
            s.PassFailures |= passBit;
            Debug::Log("Impostor: pipeline creation failed (pass bit %u). Falling back to meshes.\n", passBit);
            return;
        }
        if (!EnsureRuntimeResources())
        {
            s.PassPso = nullptr;
            return;
        }

        // GPU Driven のモデル（UpdateGpuInstances でビット5を立てたもの）。
        if (!GPUDrivenRenderer::IsActive())
        {
            return;
        }
        for (const auto& ready : s.GpuReady)
        {
            const EntityID entity = ready.Entity;
            if (!GPUDrivenRenderer::HandlesEntity(entity) ||
                (!deferredOpaque && MaterialSystem::IsReceivingPostProcess(entity) != receivingPostProcessOnly))
            {
                continue;
            }
            const auto found = s.Atlases.find(ready.Key);
            if (found == s.Atlases.end() || found->second.State != AtlasState::Ready)
            {
                continue;
            }
            const auto& slots = GpuScene::GetEntityInstances(entity);
            if (slots.empty() || s.GpuCandidates.size() >= kMaxGpuCandidates)
            {
                continue;
            }
            const MaterialComponent* material = ComponentManager::HasComponent<MaterialComponent>(entity)
                                                    ? &ComponentManager::GetComponentUnchecked<MaterialComponent>(entity)
                                                    : nullptr;
            const uint32_t group = AddToGroup(found->second, entity, material);
            if (group == UINT32_MAX)
            {
                continue;
            }
            GpuCandidate candidate{};
            candidate.Entity = entity;
            candidate.Group = group;
            s.GpuCandidates.push_back(candidate);
        }
    }

    // インスタンス描画のモデル。アトラスが準備済みなら out にインポスター情報を入れて true。
    // 距離の判定はしない（GpuInstanceCullLodCS が行う）。準備中ならベイクを要求して false。
    static bool RegisterInstanced(EntityID entity, StaticModelResource* model, const MaterialComponent* material,
                                  int entityTexture)
    {
        auto& s = S();
        if (!s.PassPso || !model || !WantsImpostor(entity))
        {
            return false;
        }
        if (ComponentManager::HasComponent<ShaderComponent>(entity) &&
            !ComponentManager::GetComponentUnchecked<ShaderComponent>(entity).PsPath.empty())
        {
            return false; // 独自シェーダーは再現できない
        }
        const auto& lod = ComponentManager::GetComponentUnchecked<LODComponent>(entity);
        // 近距離でもアトラスを要求・保持しておく（切替距離を越えた瞬間にベイク待ちにならないように）。
        Atlas* atlas = RequestAtlas(entity, model, lod, entityTexture);
        if (!atlas || atlas->State != AtlasState::Ready)
        {
            return false;
        }
        const uint32_t group = AddToGroup(*atlas, entity, material);
        if (group == UINT32_MAX)
        {
            return false;
        }
        InstancedInfo info{};
        const XMFLOAT3 center = model->GetAabbCenter();
        info.CenterDistance = {center.x, center.y, center.z, max(lod.ImpostorDistance, 0.0f)};
        info.Group = group;
        info.Valid = 1;
        s.InstancedLookup[entity] = info;
        return true;
    }

    // 候補の登録がすべて終わった後（BuildMainBatches の後）に呼ぶ。グループごとの出力領域を確定する。
    static void FinalizeLayout()
    {
        auto& s = S();
        s.LayoutReady = false;
        if (!s.PassPso || s.Groups.empty())
        {
            return;
        }
        uint32_t base = 0;
        for (auto& group : s.Groups)
        {
            group.Base = base;
            base += group.Capacity;
        }
        if (base > kMaxInstancesPerFrame)
        {
            // 容量超過：このフレームは置き換えない（メッシュのまま）。
            Debug::Log("Impostor: too many candidates (%u). Impostors skipped this frame.\n", base);
            s.Groups.clear();
            s.GpuCandidates.clear();
            s.InstancedLookup.clear();
            return;
        }
        for (auto& [entity, info] : s.InstancedLookup)
        {
            (void)entity;
            info.Base = s.Groups[info.Group].Base;
            info.Capacity = s.Groups[info.Group].Capacity;
        }
        s.LayoutReady = true;
    }

    static InstancedInfo GetInstancedInfo(EntityID entity)
    {
        auto& s = S();
        if (!s.LayoutReady)
        {
            return {};
        }
        const auto found = s.InstancedLookup.find(entity);
        return found == s.InstancedLookup.end() ? InstancedInfo{} : found->second;
    }

    // インスタンス描画の GPU カリングより前に呼ぶ。引数を初期化し、GPU Driven 側の候補を GPU で追加する。
    // 呼び出し後、出力バッファと引数バッファは UAV 状態（GpuInstanceCullLodCS が書き込む）。
    static bool PrepareGpu(ID3D12GraphicsCommandList* commandList)
    {
        auto& s = S();
        s.GpuPrepared = false;
        if (!s.LayoutReady || !commandList)
        {
            return false;
        }
        const uint32_t groupCount = static_cast<uint32_t>(s.Groups.size());
        if (s.GroupCursor + groupCount > kMaxGroups)
        {
            return false;
        }
        const UINT frame = s.UploadFrame;

        // 引数の初期値 {6, 0, 0, 0} とグループ容量をアップロード領域へ。
        const uint32_t groupFirst = frame * kMaxGroups + s.GroupCursor;
        D3D12_DRAW_ARGUMENTS* args = s.MappedArgsInit + groupFirst;
        uint32_t* capacity = s.MappedGroupCapacity + groupFirst;
        for (uint32_t i = 0; i < groupCount; ++i)
        {
            args[i] = {6, 0, 0, 0};
            capacity[i] = s.Groups[i].Capacity;
            s.FrameInstances += s.Groups[i].Capacity;
        }
        s.GroupCursor += groupCount;

        D3D12_RESOURCE_BARRIER toCopy[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(s.Args.Get(), D3D12_RESOURCE_STATE_COMMON,
                                                 D3D12_RESOURCE_STATE_COPY_DEST),
            CD3DX12_RESOURCE_BARRIER::Transition(s.OutInstances.Get(), D3D12_RESOURCE_STATE_COMMON,
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
        commandList->ResourceBarrier(_countof(toCopy), toCopy);
        commandList->CopyBufferRegion(s.Args.Get(), 0, s.ArgsInitUpload.Get(),
                                      static_cast<UINT64>(groupFirst) * sizeof(D3D12_DRAW_ARGUMENTS),
                                      static_cast<UINT64>(groupCount) * sizeof(D3D12_DRAW_ARGUMENTS));
        const auto toUav = CD3DX12_RESOURCE_BARRIER::Transition(s.Args.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commandList->ResourceBarrier(1, &toUav);
        s.GpuPrepared = true;

        // GPU Driven 側：GPUOcclusionCS の結果（w = 4）を読んでインスタンスを追加する。
        const uint32_t candidateCount = static_cast<uint32_t>(s.GpuCandidates.size());
        ID3D12Resource* result = GPUDrivenRenderer::IsActive() ? GPUVisibility::ResultBuffer() : nullptr;
        if (candidateCount == 0 || !result || !GpuScene::InstanceBuffer() || !s.CullPso)
        {
            return true;
        }
        if (s.CandidateCursor + candidateCount > kMaxGpuCandidates)
        {
            return true;
        }
        const uint32_t candidateFirst = frame * kMaxGpuCandidates + s.CandidateCursor;
        const uint32_t slotFirst = frame * kMaxCandidateSlots + s.SlotCursor;
        uint32_t slotCount = 0;
        XMUINT4* candidates = s.MappedCandidates + candidateFirst;
        uint32_t* slots = s.MappedCandidateSlots + slotFirst;
        uint32_t written = 0;
        for (const auto& candidate : s.GpuCandidates)
        {
            const auto& ids = GpuScene::GetEntityInstances(candidate.Entity);
            if (ids.empty() || s.SlotCursor + slotCount + ids.size() > kMaxCandidateSlots)
            {
                continue;
            }
            candidates[written++] = {slotCount, static_cast<uint32_t>(ids.size()), candidate.Group,
                                     s.Groups[candidate.Group].Base};
            for (const auto id : ids)
            {
                slots[slotCount++] = id;
            }
        }
        s.CandidateCursor += written;
        s.SlotCursor += slotCount;
        if (written == 0)
        {
            return true;
        }
        commandList->SetComputeRootSignature(s.CullRoot.Get());
        commandList->SetPipelineState(s.CullPso.Get());
        commandList->SetComputeRootShaderResourceView(0, GpuScene::InstanceBuffer()->GetGPUVirtualAddress());
        commandList->SetComputeRootShaderResourceView(1, result->GetGPUVirtualAddress());
        commandList->SetComputeRootShaderResourceView(
            2, s.CandidateUpload->GetGPUVirtualAddress() + static_cast<UINT64>(candidateFirst) * sizeof(XMUINT4));
        commandList->SetComputeRootShaderResourceView(
            3, s.CandidateSlotUpload->GetGPUVirtualAddress() + static_cast<UINT64>(slotFirst) * sizeof(uint32_t));
        commandList->SetComputeRootShaderResourceView(
            4, s.GroupCapacityUpload->GetGPUVirtualAddress() + static_cast<UINT64>(groupFirst) * sizeof(uint32_t));
        commandList->SetComputeRootUnorderedAccessView(5, s.OutInstances->GetGPUVirtualAddress());
        commandList->SetComputeRootUnorderedAccessView(6, s.Args->GetGPUVirtualAddress());
        const uint32_t constants[4] = {written, static_cast<uint32_t>(GpuScene::Statistics().InstanceSlots), 0, 0};
        commandList->SetComputeRoot32BitConstants(7, 4, constants, 0);
        commandList->Dispatch((written + 63) / 64, 1, 1);
        return true;
    }

    // GpuInstanceCullLodCS に渡す出力先（PrepareGpu が成功したときのみ有効）。
    static bool GetGpuOutputs(D3D12_GPU_VIRTUAL_ADDRESS& instances, D3D12_GPU_VIRTUAL_ADDRESS& args)
    {
        auto& s = S();
        if (!s.GpuPrepared)
        {
            return false;
        }
        instances = s.OutInstances->GetGPUVirtualAddress();
        args = s.Args->GetGPUVirtualAddress();
        return true;
    }

    // GPU で作ったインスタンスを ExecuteIndirect で描く。モデル用ルートシグネチャと現在のパスの
    // レンダーターゲットが設定済みであること。終了時、バッファは COMMON に戻る。
    static void Flush(ID3D12GraphicsCommandList* commandList, UINT frameIndex)
    {
        auto& s = S();
        (void)frameIndex;
        if (!s.GpuPrepared || !commandList)
        {
            s.GpuPrepared = false;
            return;
        }
        s.GpuPrepared = false;
        D3D12_RESOURCE_BARRIER toRead[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(s.Args.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                 D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),
            CD3DX12_RESOURCE_BARRIER::Transition(s.OutInstances.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
        commandList->ResourceBarrier(_countof(toRead), toRead);

        XMMATRIX view = XMMatrixIdentity();
        XMMATRIX projection = XMMatrixIdentity();
        Camera::GetCameraMatrices(Camera::GetCameraEntity(), view, projection);
        XMVECTOR determinant{};
        const XMMATRIX inverseView = XMMatrixInverse(&determinant, view);
        XMFLOAT3 cameraPosition{};
        XMStoreFloat3(&cameraPosition, inverseView.r[3]);
        const XMMATRIX previousViewProjection = XMLoadFloat4x4(&GraphicsDevice::GetPreviousViewMatrix()) *
                                                XMLoadFloat4x4(&GraphicsDevice::GetPreviousProjectionMatrix());

        ID3D12DescriptorHeap* heap = FrameConstants::GetCbvHeap();
        const UINT increment = FrameConstants::GetCbvIncrementSize();
        commandList->SetPipelineState(s.PassPso);
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commandList->IASetVertexBuffers(0, 1, &s.QuadView);

        for (uint32_t groupIndex = 0; groupIndex < s.Groups.size(); ++groupIndex)
        {
            const Group& group = s.Groups[groupIndex];
            const auto found = s.Atlases.find(group.Key);
            if (group.Capacity == 0 || found == s.Atlases.end() || found->second.State != AtlasState::Ready)
            {
                continue;
            }
            const Atlas& atlas = found->second;

            // b0：エンティティの定数をもとに、カメラとマテリアルの値を確定させる（BindAssetSlot と同じ方式）。
            ConstantBuffer3D constants{};
            if (const UINT8* entityConstants = FrameConstants::GetConstantBufferPtr();
                entityConstants && group.Entity < g_kMAX_ENTITIES)
            {
                memcpy(&constants, entityConstants + static_cast<size_t>(group.Entity) * FrameConstants::g_kCB_ALIGNED_SIZE,
                       sizeof(constants));
            }
            constants.World = XMMatrixIdentity();
            constants.PreviousWorld = XMMatrixIdentity();
            constants.View = XMMatrixTranspose(view);
            constants.Projection = XMMatrixTranspose(projection);
            constants.PreviousViewProjection = XMMatrixTranspose(previousViewProjection);
            constants.CameraPos = cameraPosition;
            constants.UseTexture = 1;
            constants.FlipNormal = 0;
            constants.UseNormalMap = 0;
            constants.MaterialAlpha = 1.0f;
            constants.MaterialIsTransparent = 0;
            if (group.Material)
            {
                constants.MaterialMode = static_cast<int>(group.Material->ShaderClassMode);
                constants.ShaderClass = static_cast<int>(group.Material->ShaderClass);
                constants.MaterialMetallic = group.Material->Metallic;
                constants.MaterialRoughness = group.Material->Roughness;
                constants.MaterialFresnel = group.Material->Fresnel;
            }
            commandList->SetGraphicsRootDescriptorTable(0, FrameConstants::AllocateTransientConstantBuffer(constants));
            if (group.Material)
            {
                MaterialBindings::SetMaterial(group.Entity, *group.Material);
            }
            commandList->SetGraphicsRootDescriptorTable(
                1, CD3DX12_GPU_DESCRIPTOR_HANDLE(heap->GetGPUDescriptorHandleForHeapStart(), atlas.SrvIndex, increment));
            commandList->SetGraphicsRootDescriptorTable(
                6, CD3DX12_GPU_DESCRIPTOR_HANDLE(heap->GetGPUDescriptorHandleForHeapStart(), atlas.SrvIndex + 1,
                                                 increment));
            const float rootConstants[16] = {atlas.Center.x,
                                             atlas.Center.y,
                                             atlas.Center.z,
                                             atlas.Radius,
                                             static_cast<float>(atlas.Frames),
                                             atlas.Hemisphere ? 1.0f : 0.0f,
                                             static_cast<float>(atlas.Resolution),
                                             static_cast<float>(atlas.MipLevels - 1),
                                             s.Config.AlphaCutoff,
                                             s.Config.MipBias,
                                             0.0f,
                                             0.0f,
                                             0.0f,
                                             0.0f,
                                             0.0f,
                                             0.0f};
            commandList->SetGraphicsRoot32BitConstants(13, 16, rootConstants, 0);
            commandList->SetGraphicsRootShaderResourceView(
                9, s.OutInstances->GetGPUVirtualAddress() + static_cast<UINT64>(group.Base) * sizeof(XMFLOAT4X4));
            // インスタンス数は GPU が書いた値（0 なら何も描かれない）。
            commandList->ExecuteIndirect(s.DrawSignature.Get(), 1, s.Args.Get(),
                                         static_cast<UINT64>(groupIndex) * sizeof(D3D12_DRAW_ARGUMENTS), nullptr, 0);
            ++s.FrameDrawCalls;
        }
        // 後続のアセットマテリアル描画のため、アセットテクスチャ表を既定に戻す。
        commandList->SetGraphicsRootDescriptorTable(
            18, CD3DX12_GPU_DESCRIPTOR_HANDLE(heap->GetGPUDescriptorHandleForHeapStart(),
                                              FrameConstants::g_kTEXTURE_SRV_START_INDEX, increment));
        D3D12_RESOURCE_BARRIER toCommon[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(s.Args.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                                                 D3D12_RESOURCE_STATE_COMMON),
            CD3DX12_RESOURCE_BARRIER::Transition(s.OutInstances.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                 D3D12_RESOURCE_STATE_COMMON)};
        commandList->ResourceBarrier(_countof(toCommon), toCommon);
    }

    // ---- GPU シーン（GPU Driven）との連携 ----------------------------------------------------------
    // GpuScene::Execute（インスタンス転送の直前）から毎フレーム呼ぶ。アトラスが準備済みのモデルに
    // ビット5と切替距離・モデル中心を書くだけで、距離の判定は行わない（GPUOcclusionCS が判定）。
    static void UpdateGpuInstances()
    {
        auto& s = S();
        s.GpuReady.clear();
        std::vector<EntityID> flagged;
        // PSO 作成に失敗した経路がある間は置き換えない（メッシュが消えるのを防ぐ）。
        // ImpostorCullCS が無いとメッシュだけ消えるので、実行時資源がそろうまで立てない。
        // インポスターの描画は InstancingSystem::Draw（メイン不透明）から行うので、それが使えることも条件。
        const bool usable = s.Config.Enabled && !s.BakeFailed && s.PassFailures == 0 && s.RuntimeReady && s.CullPso != nullptr &&
                            InstancingSystem::IsAvailable();

        for (EntityID entity : World::GetView<LODComponent, StaticModelComponent>())
        {
            if (!WantsImpostor(entity) || !ComponentManager::HasComponent<TransformComponent>(entity) ||
                !GpuScene::EntityUsesGpuOpaque(entity))
            {
                continue;
            }
            if (ComponentManager::HasComponent<ShaderComponent>(entity) &&
                !ComponentManager::GetComponentUnchecked<ShaderComponent>(entity).PsPath.empty())
            {
                continue;
            }
            StaticModelResource* model = ModelManager::GetStaticModel(
                ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity).ModelId);
            if (!model)
            {
                continue;
            }
            const auto& lod = ComponentManager::GetComponentUnchecked<LODComponent>(entity);
            Atlas* atlas = s.Config.Enabled ? RequestAtlas(entity, model, lod, ResolveEntityTexture(entity)) : nullptr;
            const bool ready = usable && atlas && atlas->State == AtlasState::Ready &&
                               s.GpuReady.size() < kMaxGpuCandidates;
            if (ready)
            {
                s.GpuReady.push_back({entity, atlas->Key});
                flagged.push_back(entity);
            }
            GpuScene::SetEntityImpostor(entity, ready, model->GetAabbCenter(), lod.ImpostorDistance);
        }
        // 前フレームに立てたが今回は対象外になったもの（UseImpostor 解除・LOD 削除など）を戻す。
        std::sort(flagged.begin(), flagged.end());
        for (EntityID entity : s.GpuFlagged)
        {
            if (!std::binary_search(flagged.begin(), flagged.end(), entity))
            {
                GpuScene::SetEntityImpostor(entity, false, {}, 0.0f);
            }
        }
        s.GpuFlagged = std::move(flagged);
    }

    // フレームの GBuffer パス開始前（レンダーターゲット設定前）に呼ぶ。保留中のアトラスを1件ベイクし、
    // 使われなくなったアトラスを解放する。
    static void ProcessPendingBakes(ID3D12GraphicsCommandList* commandList)
    {
        auto& s = S();
        ++s.FrameCounter;
        s.LastFrameInstances = s.FrameInstances;
        s.LastFrameDrawCalls = s.FrameDrawCalls;
        s.FrameInstances = 0;
        s.FrameDrawCalls = 0;
        CollectRetired();

        for (auto it = s.Atlases.begin(); it != s.Atlases.end();)
        {
            Atlas& atlas = it->second;
            if (s.FrameCounter > atlas.LastUsedFrame + s.Config.EvictFrames)
            {
                RetireAtlas(atlas);
                it = s.Atlases.erase(it);
                continue;
            }
            ++it;
        }

        if (!commandList || !s.Config.Enabled || s.BakeFailed)
        {
            return;
        }
        Atlas* candidate = nullptr;
        for (auto& [key, atlas] : s.Atlases)
        {
            (void)key;
            if (atlas.State != AtlasState::Pending)
            {
                continue;
            }
            if (!ModelAlive(atlas))
            {
                atlas.State = AtlasState::Failed;
                continue;
            }
            if (!TexturesReady(atlas))
            {
                continue;
            }
            if (!candidate || atlas.RequestedFrame < candidate->RequestedFrame)
            {
                candidate = &atlas;
            }
        }
        if (!candidate)
        {
            return;
        }
        if (!EnsureBakeResources() || !EnsureRuntimeResources())
        {
            s.BakeFailed = true;
            Debug::Log("Impostor: bake pipeline unavailable. Impostors disabled.\n");
            return;
        }
        if (!Bake(commandList, *candidate))
        {
            candidate->State = AtlasState::Failed;
            ReleaseAtlasResources(*candidate);
        }
        // ベイクで変更した状態を、後続の GBuffer パスが期待する状態へ戻す。
        commandList->RSSetViewports(1, &m_Viewport);
        commandList->RSSetScissorRects(1, &m_ScissorRect);
        if (ID3D12RootSignature* modelRoot = ShaderPipelines::GetModelRootSignature())
        {
            commandList->SetGraphicsRootSignature(modelRoot);
        }
    }

    static void Shutdown()
    {
        auto& s = S();
        s.Atlases.clear();
        s.Retired.clear();
        s.RetiredSlots.clear();
        s.FreeSlots.clear();
        s.NextSlot = 0;
        s.Groups.clear();
        s.GroupLookup.clear();
        s.GpuCandidates.clear();
        s.InstancedLookup.clear();
        s.GpuReady.clear();
        s.GpuFlagged.clear();
        s.LayoutReady = false;
        s.GpuPrepared = false;
        auto unmap = [](ComPtr<ID3D12Resource>& resource, auto*& mapped) {
            if (resource && mapped)
            {
                resource->Unmap(0, nullptr);
            }
            mapped = nullptr;
            resource.Reset();
        };
        unmap(s.ArgsInitUpload, s.MappedArgsInit);
        unmap(s.GroupCapacityUpload, s.MappedGroupCapacity);
        unmap(s.CandidateUpload, s.MappedCandidates);
        unmap(s.CandidateSlotUpload, s.MappedCandidateSlots);
        s.OutInstances.Reset();
        s.Args.Reset();
        s.CullRoot.Reset();
        s.CullPso.Reset();
        s.DrawSignature.Reset();
        s.QuadBuffer.Reset();
        s.BakeRoot.Reset();
        s.BakePso.Reset();
        s.BuildRoot.Reset();
        s.DilatePso.Reset();
        s.DownsamplePso.Reset();
        s.RtvHeap.Reset();
        s.DsvHeap.Reset();
        s.BakeResourcesTried = false;
        s.RuntimeResourcesTried = false;
        s.BakeReady = false;
        s.RuntimeReady = false;
        s.BakeFailed = false;
        s.PassFailures = 0;
        s.PassPso = nullptr;
        s.UploadFrame = UINT_MAX;
        s.CandidateCursor = 0;
        s.SlotCursor = 0;
        s.GroupCursor = 0;
    }

  private:
    struct AtlasKey
    {
        const StaticModelResource* Model = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS GeometryAddress = 0; // モデル再読込の検出
        int EntityTexture = -1;
        uint32_t Frames = 8;
        uint32_t Resolution = 1024;
        uint32_t Hemisphere = 0;

        bool operator==(const AtlasKey& other) const
        {
            return Model == other.Model && GeometryAddress == other.GeometryAddress &&
                   EntityTexture == other.EntityTexture && Frames == other.Frames &&
                   Resolution == other.Resolution && Hemisphere == other.Hemisphere;
        }
    };

    struct AtlasKeyHash
    {
        size_t operator()(const AtlasKey& key) const noexcept
        {
            size_t h = std::hash<const void*>{}(key.Model);
            auto combine = [&h](size_t value) { h ^= value + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
            combine(std::hash<uint64_t>{}(key.GeometryAddress));
            combine(std::hash<int>{}(key.EntityTexture));
            combine(key.Frames | (key.Resolution << 8) | (key.Hemisphere << 31));
            return h;
        }
    };

    struct Atlas
    {
        AtlasKey Key{};
        AtlasState State = AtlasState::None;
        uint32_t Resolution = 1024;
        uint32_t Frames = 8;
        uint32_t MipLevels = 1;
        bool Hemisphere = false;
        int EntityTexture = -1;
        int ModelId = -1;
        XMFLOAT3 Center{};
        float Radius = 1.0f;
        ComPtr<ID3D12Resource> Albedo;
        ComPtr<ID3D12Resource> NormalDepth;
        UINT SrvIndex = 0; // ヒープの絶対位置（アルベド、法線の順）
        uint32_t Slot = UINT32_MAX;
        uint64_t RequestedFrame = 0;
        uint64_t LastUsedFrame = 0;
    };

    struct Group
    {
        AtlasKey Key{};
        EntityID Entity = 0; // 定数・マテリアルの代表
        const MaterialComponent* Material = nullptr;
        uint64_t MaterialHash = 0;
        uint32_t Capacity = 0;
        uint32_t Base = 0;
    };

    struct GroupKey
    {
        const void* Atlas = nullptr;
        uint64_t MaterialHash = 0;
        bool operator==(const GroupKey& other) const
        {
            return Atlas == other.Atlas && MaterialHash == other.MaterialHash;
        }
    };

    struct GroupKeyHash
    {
        size_t operator()(const GroupKey& key) const noexcept
        {
            return std::hash<const void*>{}(key.Atlas) ^ (std::hash<uint64_t>{}(key.MaterialHash) * 0x9e3779b97f4a7c15ull);
        }
    };

    struct GpuCandidate
    {
        EntityID Entity = 0;
        uint32_t Group = 0;
    };

    struct GpuReadyEntry
    {
        EntityID Entity = 0;
        AtlasKey Key{};
    };

    struct RetiredResource
    {
        UINT64 Fence = 0;
        ComPtr<ID3D12Resource> Resource;
    };

    struct RetiredSlot
    {
        UINT64 Fence = 0;
        uint32_t Slot = 0;
    };

    struct State
    {
        Settings Config{};
        std::unordered_map<AtlasKey, Atlas, AtlasKeyHash> Atlases;
        std::vector<Group> Groups;
        std::unordered_map<GroupKey, uint32_t, GroupKeyHash> GroupLookup;
        std::vector<GpuCandidate> GpuCandidates;
        std::unordered_map<EntityID, InstancedInfo> InstancedLookup;
        bool LayoutReady = false;
        bool GpuPrepared = false;
        std::vector<RetiredResource> Retired;
        std::vector<RetiredSlot> RetiredSlots;
        std::vector<uint32_t> FreeSlots;
        uint32_t NextSlot = 0;
        uint64_t FrameCounter = 1;
        uint32_t FrameInstances = 0;
        uint32_t FrameDrawCalls = 0;
        uint32_t LastFrameInstances = 0;
        uint32_t LastFrameDrawCalls = 0;
        ID3D12PipelineState* PassPso = nullptr;
        uint32_t PassFailures = 0;
        // GPU Driven 連携（UpdateGpuInstances で準備済みのものを記録）
        std::vector<GpuReadyEntry> GpuReady;
        std::vector<EntityID> GpuFlagged; // 昇順

        bool BakeFailed = false;
        bool BakeResourcesTried = false;
        bool RuntimeResourcesTried = false;
        bool RuntimeReady = false;
        bool BakeReady = false;
        // 実行時
        ComPtr<ID3D12Resource> OutInstances; // GPU が書くインポスターのワールド行列（UAV / SRV）
        ComPtr<ID3D12Resource> Args;         // D3D12_DRAW_ARGUMENTS × グループ（UAV / INDIRECT）
        ComPtr<ID3D12Resource> ArgsInitUpload;
        D3D12_DRAW_ARGUMENTS* MappedArgsInit = nullptr;
        ComPtr<ID3D12Resource> GroupCapacityUpload;
        uint32_t* MappedGroupCapacity = nullptr;
        ComPtr<ID3D12Resource> CandidateUpload;
        XMUINT4* MappedCandidates = nullptr;
        ComPtr<ID3D12Resource> CandidateSlotUpload;
        uint32_t* MappedCandidateSlots = nullptr;
        ComPtr<ID3D12RootSignature> CullRoot;
        ComPtr<ID3D12PipelineState> CullPso;
        ComPtr<ID3D12CommandSignature> DrawSignature;
        UINT UploadFrame = UINT_MAX;
        uint32_t CandidateCursor = 0;
        uint32_t SlotCursor = 0;
        uint32_t GroupCursor = 0;
        ComPtr<ID3D12Resource> QuadBuffer;
        D3D12_VERTEX_BUFFER_VIEW QuadView{};
        // ベイク
        ComPtr<ID3D12RootSignature> BakeRoot;
        ComPtr<ID3D12PipelineState> BakePso;
        ComPtr<ID3D12RootSignature> BuildRoot;
        ComPtr<ID3D12PipelineState> DilatePso;
        ComPtr<ID3D12PipelineState> DownsamplePso;
        ComPtr<ID3D12DescriptorHeap> RtvHeap;
        ComPtr<ID3D12DescriptorHeap> DsvHeap;
    };

    static State& S()
    {
        static State state;
        return state;
    }

    // アトラスを取得（無ければベイクを要求）。使用フレームを更新する。
    static Atlas* RequestAtlas(EntityID entity, StaticModelResource* model, const LODComponent& lod, int entityTexture)
    {
        auto& s = S();
        const AtlasKey key = MakeKey(model, lod, entityTexture);
        auto found = s.Atlases.find(key);
        if (found == s.Atlases.end())
        {
            Atlas atlas{};
            atlas.Key = key;
            atlas.Resolution = key.Resolution;
            atlas.Frames = key.Frames;
            atlas.Hemisphere = key.Hemisphere != 0;
            atlas.EntityTexture = entityTexture;
            atlas.ModelId = ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity).ModelId;
            atlas.State = AtlasState::Pending;
            atlas.RequestedFrame = s.FrameCounter;
            found = s.Atlases.emplace(key, std::move(atlas)).first;
        }
        found->second.LastUsedFrame = s.FrameCounter;
        return &found->second;
    }

    // (アトラス, マテリアル) ごとのグループに1件追加し、グループ番号を返す。
    static uint32_t AddToGroup(Atlas& atlas, EntityID entity, const MaterialComponent* material)
    {
        auto& s = S();
        atlas.LastUsedFrame = s.FrameCounter;
        const uint64_t materialHash = material ? MaterialBindings::GetMaterialBatchHash(*material) : 0;
        const GroupKey key{&atlas, materialHash};
        auto found = s.GroupLookup.find(key);
        if (found == s.GroupLookup.end())
        {
            if (s.Groups.size() >= kMaxGroups)
            {
                return UINT32_MAX;
            }
            Group group{};
            group.Key = atlas.Key;
            group.Entity = entity;
            group.Material = material;
            group.MaterialHash = materialHash;
            found = s.GroupLookup.emplace(key, static_cast<uint32_t>(s.Groups.size())).first;
            s.Groups.push_back(group);
        }
        ++s.Groups[found->second].Capacity;
        return found->second;
    }

    static uint32_t CurrentPassBit()
    {
        const bool deferred = m_RenderMode == RenderMode::DEFERRED && m_IsDeferredGeometryPass;
        const bool visibility = deferred && RendererSettings::GetComputeGBufferEnabled() && VisibilityBuffer::IsAvailable();
        return visibility ? 4u : (deferred ? 2u : 1u);
    }

    static uint32_t SanitizeFrames(uint32_t frames)
    {
        return frames <= 4 ? 4u : (frames <= 8 ? 8u : 16u);
    }

    static uint32_t SanitizeResolution(uint32_t resolution)
    {
        return resolution <= 512 ? 512u : (resolution <= 1024 ? 1024u : 2048u);
    }

    static AtlasKey MakeKey(const StaticModelResource* model, const LODComponent& lod, int entityTexture)
    {
        AtlasKey key{};
        key.Model = model;
        key.GeometryAddress = model && model->GetMeshCount() > 0
                                  ? model->GetMeshData(0).VertexBufferView.BufferLocation
                                  : 0;
        key.EntityTexture = entityTexture;
        key.Frames = SanitizeFrames(lod.ImpostorFrames);
        key.Resolution = SanitizeResolution(lod.ImpostorResolution);
        key.Hemisphere = lod.ImpostorHemisphere ? 1u : 0u;
        return key;
    }

    static uint32_t MipCountFor(uint32_t resolution, uint32_t frames)
    {
        // 1フレームが 8 texel になるまで（フレーム境界をまたいだ平均を作らない）。
        uint32_t frameSize = resolution / frames;
        uint32_t levels = 1;
        while (frameSize > 8 && levels < 8)
        {
            frameSize >>= 1;
            ++levels;
        }
        return levels;
    }

    static uint64_t AtlasBytes(uint32_t resolution, uint32_t mipLevels)
    {
        uint64_t bytes = 0;
        for (uint32_t level = 0; level < mipLevels; ++level)
        {
            const uint64_t size = max(resolution >> level, 1u);
            bytes += size * size * 8ull; // RGBA8 × 2
        }
        return bytes;
    }

    static UINT DescriptorBase()
    {
        // ヒープ末尾 16384 個（VirtualGeometry ページ）の直前に予約した領域。
        ID3D12DescriptorHeap* heap = FrameConstants::GetCbvHeap();
        return heap ? heap->GetDesc().NumDescriptors - 16384u - kDescriptorCount : 0u;
    }

    static D3D12_CPU_DESCRIPTOR_HANDLE CpuDescriptor(UINT index)
    {
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(FrameConstants::GetCbvHeap()->GetCPUDescriptorHandleForHeapStart(), index,
                                             FrameConstants::GetCbvIncrementSize());
    }

    static D3D12_GPU_DESCRIPTOR_HANDLE GpuDescriptor(UINT index)
    {
        return CD3DX12_GPU_DESCRIPTOR_HANDLE(FrameConstants::GetCbvHeap()->GetGPUDescriptorHandleForHeapStart(), index,
                                             FrameConstants::GetCbvIncrementSize());
    }

    static bool AllocateSlot(uint32_t& slot)
    {
        auto& s = S();
        constexpr uint32_t slotCount = (kDescriptorCount - kScratchDescriptorsPerFrame * g_kFRAME_COUNT) / 2;
        if (!s.FreeSlots.empty())
        {
            slot = s.FreeSlots.back();
            s.FreeSlots.pop_back();
            return true;
        }
        if (s.NextSlot < slotCount)
        {
            slot = s.NextSlot++;
            return true;
        }
        return false;
    }

    static UINT SlotDescriptor(uint32_t slot)
    {
        return DescriptorBase() + kScratchDescriptorsPerFrame * g_kFRAME_COUNT + slot * 2;
    }

    static void RetireResource(ComPtr<ID3D12Resource>& resource)
    {
        if (resource)
        {
            S().Retired.push_back({GraphicsDevice::GetPendingSubmissionFence(), resource});
            resource.Reset();
        }
    }

    static void ReleaseAtlasResources(Atlas& atlas)
    {
        RetireResource(atlas.Albedo);
        RetireResource(atlas.NormalDepth);
        if (atlas.Slot != UINT32_MAX)
        {
            S().RetiredSlots.push_back({GraphicsDevice::GetPendingSubmissionFence(), atlas.Slot});
            atlas.Slot = UINT32_MAX;
        }
    }

    static void RetireAtlas(Atlas& atlas)
    {
        ReleaseAtlasResources(atlas);
        atlas.State = AtlasState::None;
    }

    static void CollectRetired()
    {
        auto& s = S();
        const UINT64 completed = GraphicsDevice::GetCompletedFence();
        s.Retired.erase(std::remove_if(s.Retired.begin(), s.Retired.end(),
                                       [completed](const RetiredResource& r) { return r.Fence <= completed; }),
                        s.Retired.end());
        for (auto it = s.RetiredSlots.begin(); it != s.RetiredSlots.end();)
        {
            if (it->Fence <= completed)
            {
                s.FreeSlots.push_back(it->Slot);
                it = s.RetiredSlots.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    // モデルが解放・再読込されていないか（アトラスのキーは生ポインタなので、使う前に必ず確認する）。
    static bool ModelAlive(const Atlas& atlas)
    {
        const StaticModelResource* current = ModelManager::GetStaticModel(atlas.ModelId);
        return current && current == atlas.Key.Model && current->GetMeshCount() > 0 &&
               current->GetMeshData(0).VertexBufferView.BufferLocation == atlas.Key.GeometryAddress;
    }

    static bool TexturesReady(Atlas& atlas)
    {
        // ストリーミング中の低解像度 mip を焼き込まないよう、最詳細 mip が揃うまで待つ（最大 4 秒程度）。
        const auto* model = atlas.Key.Model;
        if (!model)
        {
            return false;
        }
        bool ready = true;
        auto check = [&ready](int index) {
            if (index < 0)
            {
                return;
            }
            TextureManager::TouchTexture(index);
            if (TextureManager::GetTextureResidentMip(index) > 0)
            {
                ready = false;
            }
        };
        for (UINT mesh = 0; mesh < model->GetMeshCount(); ++mesh)
        {
            check(model->GetMeshData(mesh).TextureIndex);
        }
        check(atlas.EntityTexture);
        return ready || S().FrameCounter > atlas.RequestedFrame + 240;
    }

    static ComPtr<ID3DBlob> CompileShader(const wchar_t* file, const char* entry, const char* target)
    {
        ComPtr<ID3DBlob> blob;
        ComPtr<ID3DBlob> errors;
        const HRESULT hr = D3DCompileFromFile(file, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, target,
                                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
        if (FAILED(hr))
        {
            Debug::Log("Impostor: shader compile failed (%s): %s\n", entry,
                       errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
            return nullptr;
        }
        return blob;
    }

    static bool CreateRootSignature(ID3D12Device* device, const CD3DX12_ROOT_SIGNATURE_DESC& description,
                                    ComPtr<ID3D12RootSignature>& rootSignature)
    {
        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3DBlob> errors;
        if (FAILED(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)))
        {
            Debug::Log("Impostor: root signature error: %s\n",
                       errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
            return false;
        }
        return SUCCEEDED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                     IID_PPV_ARGS(&rootSignature)));
    }

    static bool EnsureRuntimeResources()
    {
        auto& s = S();
        if (s.RuntimeResourcesTried)
        {
            return s.RuntimeReady;
        }
        s.RuntimeResourcesTried = true;
        ID3D12Device* device = GraphicsDevice::GetDevice();
        if (!device || !FrameConstants::GetCbvHeap())
        {
            return false;
        }
        const auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        const auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const D3D12_RANGE noRead{0, 0};
        auto createDefault = [&](UINT64 bytes, ComPtr<ID3D12Resource>& resource, const wchar_t* name) {
            const auto description = CD3DX12_RESOURCE_DESC::Buffer(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            if (FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &description,
                                                       D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                       IID_PPV_ARGS(&resource))))
            {
                return false;
            }
            resource->SetName(name);
            return true;
        };
        auto createUpload = [&](UINT64 bytes, ComPtr<ID3D12Resource>& resource, auto*& mapped) {
            const auto description = CD3DX12_RESOURCE_DESC::Buffer(bytes);
            if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &description,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                       IID_PPV_ARGS(&resource))) ||
                FAILED(resource->Map(0, &noRead, reinterpret_cast<void**>(&mapped))))
            {
                resource.Reset();
                mapped = nullptr;
                return false;
            }
            return true;
        };
        if (!createDefault(static_cast<UINT64>(kMaxInstancesPerFrame) * sizeof(XMFLOAT4X4), s.OutInstances,
                           L"Impostor GPU Instances") ||
            !createDefault(static_cast<UINT64>(kMaxGroups) * sizeof(D3D12_DRAW_ARGUMENTS), s.Args,
                           L"Impostor Indirect Arguments") ||
            !createUpload(static_cast<UINT64>(g_kFRAME_COUNT) * kMaxGroups * sizeof(D3D12_DRAW_ARGUMENTS),
                          s.ArgsInitUpload, s.MappedArgsInit) ||
            !createUpload(static_cast<UINT64>(g_kFRAME_COUNT) * kMaxGroups * sizeof(uint32_t), s.GroupCapacityUpload,
                          s.MappedGroupCapacity) ||
            !createUpload(static_cast<UINT64>(g_kFRAME_COUNT) * kMaxGpuCandidates * sizeof(XMUINT4),
                          s.CandidateUpload, s.MappedCandidates) ||
            !createUpload(static_cast<UINT64>(g_kFRAME_COUNT) * kMaxCandidateSlots * sizeof(uint32_t),
                          s.CandidateSlotUpload, s.MappedCandidateSlots))
        {
            return false;
        }

        D3D12_INDIRECT_ARGUMENT_DESC drawArgument{};
        drawArgument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
        D3D12_COMMAND_SIGNATURE_DESC signature{};
        signature.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
        signature.NumArgumentDescs = 1;
        signature.pArgumentDescs = &drawArgument;
        if (FAILED(device->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&s.DrawSignature))))
        {
            return false;
        }

        // GPU Driven 側の候補追加（失敗してもインスタンス描画側のインポスターは使える）。
        if (const auto cull = CompileShader(L"shader/hlsl/ImpostorCullCS.hlsl", "main", "cs_6_5"))
        {
            CD3DX12_ROOT_PARAMETER parameters[8]{};
            for (UINT i = 0; i < 5; ++i)
            {
                parameters[i].InitAsShaderResourceView(i);
            }
            parameters[5].InitAsUnorderedAccessView(0);
            parameters[6].InitAsUnorderedAccessView(1);
            parameters[7].InitAsConstants(4, 0);
            CD3DX12_ROOT_SIGNATURE_DESC rootDescription{};
            rootDescription.Init(_countof(parameters), parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);
            if (CreateRootSignature(device, rootDescription, s.CullRoot))
            {
                D3D12_COMPUTE_PIPELINE_STATE_DESC compute{};
                compute.pRootSignature = s.CullRoot.Get();
                compute.CS = CD3DX12_SHADER_BYTECODE(cull.Get());
                if (FAILED(device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&s.CullPso))))
                {
                    s.CullPso.Reset();
                }
            }
        }
        if (!s.CullPso)
        {
            Debug::Log("Impostor: ImpostorCullCS unavailable. GPU Driven models keep meshes.\n");
        }

        // 6頂点の四角形（Position.xy = 角の符号）。レイアウトは StaticModelVertex と同じ。
        StaticModelVertex quad[6]{};
        const float corners[6][2] = {{-1, -1}, {-1, 1}, {1, 1}, {-1, -1}, {1, 1}, {1, -1}};
        for (int i = 0; i < 6; ++i)
        {
            quad[i].Position = {corners[i][0], corners[i][1], 0.0f};
            quad[i].Normal = {0.0f, 0.0f, 1.0f};
            quad[i].TexCoord = {corners[i][0] * 0.5f + 0.5f, 0.5f - corners[i][1] * 0.5f};
            quad[i].Diffuse = {1.0f, 1.0f, 1.0f, 10.0f};
        }
        const auto quadDescription = CD3DX12_RESOURCE_DESC::Buffer(sizeof(quad));
        if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &quadDescription,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&s.QuadBuffer))))
        {
            return false;
        }
        void* mapped = nullptr;
        if (FAILED(s.QuadBuffer->Map(0, &noRead, &mapped)))
        {
            return false;
        }
        memcpy(mapped, quad, sizeof(quad));
        s.QuadBuffer->Unmap(0, nullptr);
        s.QuadView.BufferLocation = s.QuadBuffer->GetGPUVirtualAddress();
        s.QuadView.SizeInBytes = sizeof(quad);
        s.QuadView.StrideInBytes = sizeof(StaticModelVertex);
        s.RuntimeReady = true;
        return true;
    }

    static bool EnsureBakeResources()
    {
        auto& s = S();
        if (s.BakeResourcesTried)
        {
            return s.BakeReady;
        }
        s.BakeResourcesTried = true;
        ID3D12Device* device = GraphicsDevice::GetDevice();
        if (!device)
        {
            return false;
        }

        // ベイク用：b0 定数 12 個 + t0（メッシュのテクスチャ）
        {
            CD3DX12_DESCRIPTOR_RANGE textureRange;
            textureRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
            CD3DX12_ROOT_PARAMETER parameters[2];
            parameters[0].InitAsConstants(12, 0, 0, D3D12_SHADER_VISIBILITY_ALL);
            parameters[1].InitAsDescriptorTable(1, &textureRange, D3D12_SHADER_VISIBILITY_PIXEL);
            CD3DX12_STATIC_SAMPLER_DESC sampler(0, D3D12_FILTER_ANISOTROPIC);
            sampler.MaxAnisotropy = 8;
            CD3DX12_ROOT_SIGNATURE_DESC description;
            description.Init(_countof(parameters), parameters, 1, &sampler,
                             D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
            if (!CreateRootSignature(device, description, s.BakeRoot))
            {
                return false;
            }
        }
        // 後処理用：b0 定数 4 個 + t0..t1 + u0..u1
        {
            CD3DX12_DESCRIPTOR_RANGE srvRange;
            srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0);
            CD3DX12_DESCRIPTOR_RANGE uavRange;
            uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0);
            CD3DX12_ROOT_PARAMETER parameters[3];
            parameters[0].InitAsConstants(4, 0);
            parameters[1].InitAsDescriptorTable(1, &srvRange);
            parameters[2].InitAsDescriptorTable(1, &uavRange);
            CD3DX12_ROOT_SIGNATURE_DESC description;
            description.Init(_countof(parameters), parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);
            if (!CreateRootSignature(device, description, s.BuildRoot))
            {
                return false;
            }
        }

        const auto bakeVs = CompileShader(L"shader/hlsl/ImpostorBake.hlsl", "BakeVS", "vs_6_5");
        const auto bakePs = CompileShader(L"shader/hlsl/ImpostorBake.hlsl", "BakePS", "ps_6_5");
        const auto dilate = CompileShader(L"shader/hlsl/ImpostorBuildCS.hlsl", "DilateCS", "cs_6_5");
        const auto downsample = CompileShader(L"shader/hlsl/ImpostorBuildCS.hlsl", "DownsampleCS", "cs_6_5");
        if (!bakeVs || !bakePs || !dilate || !downsample)
        {
            return false;
        }

        static const D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{};
        graphics.pRootSignature = s.BakeRoot.Get();
        graphics.VS = CD3DX12_SHADER_BYTECODE(bakeVs.Get());
        graphics.PS = CD3DX12_SHADER_BYTECODE(bakePs.Get());
        graphics.InputLayout = {layout, _countof(layout)};
        graphics.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        graphics.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // 葉などの片面ポリゴンも両方向から写す
        graphics.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        graphics.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        graphics.SampleMask = UINT_MAX;
        graphics.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        graphics.NumRenderTargets = 2;
        graphics.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        graphics.RTVFormats[1] = DXGI_FORMAT_R8G8B8A8_UNORM;
        graphics.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        graphics.SampleDesc.Count = 1;
        if (!PsoManager::CreateGraphicsPipelineState(graphics, "Impostor Bake", s.BakePso))
        {
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC compute{};
        compute.pRootSignature = s.BuildRoot.Get();
        compute.CS = CD3DX12_SHADER_BYTECODE(dilate.Get());
        if (!PsoManager::CreateComputePipelineState(compute, "Impostor Dilate", s.DilatePso))
        {
            return false;
        }
        compute.CS = CD3DX12_SHADER_BYTECODE(downsample.Get());
        if (!PsoManager::CreateComputePipelineState(compute, "Impostor Downsample", s.DownsamplePso))
        {
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC rtv{};
        rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtv.NumDescriptors = 2;
        D3D12_DESCRIPTOR_HEAP_DESC dsv{};
        dsv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsv.NumDescriptors = 1;
        if (FAILED(device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&s.RtvHeap))) ||
            FAILED(device->CreateDescriptorHeap(&dsv, IID_PPV_ARGS(&s.DsvHeap))))
        {
            return false;
        }
        s.BakeReady = true;
        return true;
    }

    static bool CreateTexture(ID3D12Device* device, UINT size, UINT16 mips, DXGI_FORMAT format,
                              D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear,
                              ComPtr<ID3D12Resource>& resource)
    {
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const auto description = CD3DX12_RESOURCE_DESC::Tex2D(format, size, size, 1, mips, 1, 0, flags);
        return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, state, clear,
                                                         IID_PPV_ARGS(&resource)));
    }

    static void WriteSrv(ID3D12Resource* resource, DXGI_FORMAT format, UINT mostDetailedMip, UINT mipLevels,
                         UINT index)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = format;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MostDetailedMip = mostDetailedMip;
        view.Texture2D.MipLevels = mipLevels;
        GraphicsDevice::GetDevice()->CreateShaderResourceView(resource, &view, CpuDescriptor(index));
    }

    static void WriteUav(ID3D12Resource* resource, DXGI_FORMAT format, UINT mip, UINT index)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = format;
        view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipSlice = mip;
        GraphicsDevice::GetDevice()->CreateUnorderedAccessView(resource, nullptr, &view, CpuDescriptor(index));
    }

    static bool Bake(ID3D12GraphicsCommandList* commandList, Atlas& atlas)
    {
        auto& s = S();
        ID3D12Device* device = GraphicsDevice::GetDevice();
        const StaticModelResource* model = atlas.Key.Model;
        if (!device || !model || !FrameConstants::GetCbvHeap())
        {
            return false;
        }
        bool hasGeometry = false;
        for (UINT mesh = 0; mesh < model->GetMeshCount(); ++mesh)
        {
            const auto& data = model->GetMeshData(mesh);
            hasGeometry |= data.VertexBuffer && data.IndexBuffer && data.IndexCount > 0;
        }
        if (!hasGeometry || !AllocateSlot(atlas.Slot))
        {
            return false;
        }

        const UINT resolution = atlas.Resolution;
        const UINT frames = atlas.Frames;
        const UINT frameSize = resolution / frames;
        atlas.MipLevels = MipCountFor(resolution, frames);
        const XMFLOAT3 extents = model->GetAabbExtents();
        atlas.Center = model->GetAabbCenter();
        atlas.Radius = max(sqrtf(extents.x * extents.x + extents.y * extents.y + extents.z * extents.z), 1e-3f);

        ComPtr<ID3D12Resource> bakeAlbedo;
        ComPtr<ID3D12Resource> bakeNormal;
        ComPtr<ID3D12Resource> bakeDepth;
        D3D12_CLEAR_VALUE albedoClear{DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, {0.0f, 0.0f, 0.0f, 0.0f}};
        D3D12_CLEAR_VALUE normalClear{DXGI_FORMAT_R8G8B8A8_UNORM, {0.5f, 0.5f, 1.0f, 0.0f}};
        D3D12_CLEAR_VALUE depthClear{};
        depthClear.Format = DXGI_FORMAT_D32_FLOAT;
        depthClear.DepthStencil.Depth = 1.0f;
        if (!CreateTexture(device, resolution, 1, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                           D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, &albedoClear,
                           bakeAlbedo) ||
            !CreateTexture(device, resolution, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                           D3D12_RESOURCE_STATE_RENDER_TARGET, &normalClear, bakeNormal) ||
            !CreateTexture(device, resolution, 1, DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                           D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear, bakeDepth) ||
            !CreateTexture(device, resolution, static_cast<UINT16>(atlas.MipLevels), DXGI_FORMAT_R8G8B8A8_TYPELESS,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                           atlas.Albedo) ||
            !CreateTexture(device, resolution, static_cast<UINT16>(atlas.MipLevels), DXGI_FORMAT_R8G8B8A8_UNORM,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                           atlas.NormalDepth))
        {
            RetireResource(bakeAlbedo);
            RetireResource(bakeNormal);
            RetireResource(bakeDepth);
            return false;
        }
        atlas.Albedo->SetName(L"Impostor Albedo Atlas");
        atlas.NormalDepth->SetName(L"Impostor NormalDepth Atlas");

        const UINT rtvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        const CD3DX12_CPU_DESCRIPTOR_HANDLE rtv0(s.RtvHeap->GetCPUDescriptorHandleForHeapStart(), 0, rtvIncrement);
        const CD3DX12_CPU_DESCRIPTOR_HANDLE rtv1(s.RtvHeap->GetCPUDescriptorHandleForHeapStart(), 1, rtvIncrement);
        const D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = s.DsvHeap->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(bakeAlbedo.Get(), nullptr, rtv0);
        device->CreateRenderTargetView(bakeNormal.Get(), nullptr, rtv1);
        device->CreateDepthStencilView(bakeDepth.Get(), nullptr, dsvHandle);

        // 1. 各視点を正射影でタイルへ描く。
        const D3D12_CPU_DESCRIPTOR_HANDLE targets[2] = {rtv0, rtv1};
        commandList->ClearRenderTargetView(rtv0, albedoClear.Color, 0, nullptr);
        commandList->ClearRenderTargetView(rtv1, normalClear.Color, 0, nullptr);
        commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        commandList->OMSetRenderTargets(2, targets, FALSE, &dsvHandle);
        commandList->SetGraphicsRootSignature(s.BakeRoot.Get());
        commandList->SetPipelineState(s.BakePso.Get());
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const float bounds[4] = {atlas.Center.x, atlas.Center.y, atlas.Center.z, atlas.Radius};
        commandList->SetGraphicsRoot32BitConstants(0, 4, bounds, 0);
        const int defaultTexture = TextureManager::GetDefaultTextureIndex();
        for (UINT y = 0; y < frames; ++y)
        {
            for (UINT x = 0; x < frames; ++x)
            {
                const D3D12_VIEWPORT viewport{static_cast<float>(x * frameSize), static_cast<float>(y * frameSize),
                                              static_cast<float>(frameSize), static_cast<float>(frameSize), 0.0f,
                                              1.0f};
                const D3D12_RECT scissor{static_cast<LONG>(x * frameSize), static_cast<LONG>(y * frameSize),
                                         static_cast<LONG>((x + 1) * frameSize), static_cast<LONG>((y + 1) * frameSize)};
                commandList->RSSetViewports(1, &viewport);
                commandList->RSSetScissorRects(1, &scissor);
                const float frame[4] = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(frames),
                                        atlas.Hemisphere ? 1.0f : 0.0f};
                commandList->SetGraphicsRoot32BitConstants(0, 4, frame, 4);
                for (UINT mesh = 0; mesh < model->GetMeshCount(); ++mesh)
                {
                    const auto& data = model->GetMeshData(mesh);
                    if (!data.VertexBuffer || !data.IndexBuffer || data.IndexCount == 0)
                    {
                        continue;
                    }
                    int texture = data.TextureIndex >= 0 ? data.TextureIndex : atlas.EntityTexture;
                    const bool useTexture = texture >= 0 && texture != defaultTexture;
                    if (texture < 0)
                    {
                        texture = defaultTexture;
                    }
                    const float flags[4] = {useTexture ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
                    commandList->SetGraphicsRoot32BitConstants(0, 4, flags, 8);
                    commandList->SetGraphicsRootDescriptorTable(1, GpuDescriptor(static_cast<UINT>(texture)));
                    commandList->IASetVertexBuffers(0, 1, &data.VertexBufferView);
                    commandList->IASetIndexBuffer(&data.IndexBufferView);
                    commandList->DrawIndexedInstanced(data.IndexCount, 1, 0, 0, 0);
                }
            }
        }

        // 2. 膨張（ベイク結果 -> アトラス mip0）
        D3D12_RESOURCE_BARRIER toRead[2] = {
            CD3DX12_RESOURCE_BARRIER::Transition(bakeAlbedo.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(bakeNormal.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
        commandList->ResourceBarrier(2, toRead);

        const UINT scratch = DescriptorBase() + (GraphicsDevice::GetFrameIndex() % g_kFRAME_COUNT) * kScratchDescriptorsPerFrame;
        WriteSrv(bakeAlbedo.Get(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 0, 1, scratch + 0);
        WriteSrv(bakeNormal.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1, scratch + 1);
        WriteUav(atlas.Albedo.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 0, scratch + 2);
        WriteUav(atlas.NormalDepth.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 0, scratch + 3);

        commandList->SetComputeRootSignature(s.BuildRoot.Get());
        commandList->SetPipelineState(s.DilatePso.Get());
        const UINT dilateConstants[4] = {resolution, frameSize, kDilateRadius, 0};
        commandList->SetComputeRoot32BitConstants(0, 4, dilateConstants, 0);
        commandList->SetComputeRootDescriptorTable(1, GpuDescriptor(scratch + 0));
        commandList->SetComputeRootDescriptorTable(2, GpuDescriptor(scratch + 2));
        commandList->Dispatch((resolution + 7) / 8, (resolution + 7) / 8, 1);

        // 3. mip 生成（フレーム境界は 2 のべき乗で揃っているので、2x2 平均はフレームをまたがない）
        commandList->SetPipelineState(s.DownsamplePso.Get());
        for (UINT level = 1; level < atlas.MipLevels; ++level)
        {
            D3D12_RESOURCE_BARRIER previous[2] = {
                CD3DX12_RESOURCE_BARRIER::Transition(atlas.Albedo.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, level - 1),
                CD3DX12_RESOURCE_BARRIER::Transition(atlas.NormalDepth.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, level - 1)};
            commandList->ResourceBarrier(2, previous);
            const UINT base = scratch + level * 4;
            WriteSrv(atlas.Albedo.Get(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, level - 1, 1, base + 0);
            WriteSrv(atlas.NormalDepth.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, level - 1, 1, base + 1);
            WriteUav(atlas.Albedo.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, level, base + 2);
            WriteUav(atlas.NormalDepth.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, level, base + 3);
            const UINT size = max(resolution >> level, 1u);
            const UINT levelConstants[4] = {size, max(frameSize >> level, 1u), 0, 0};
            commandList->SetComputeRoot32BitConstants(0, 4, levelConstants, 0);
            commandList->SetComputeRootDescriptorTable(1, GpuDescriptor(base + 0));
            commandList->SetComputeRootDescriptorTable(2, GpuDescriptor(base + 2));
            commandList->Dispatch((size + 7) / 8, (size + 7) / 8, 1);
        }
        const UINT last = atlas.MipLevels - 1;
        D3D12_RESOURCE_BARRIER lastLevel[2] = {
            CD3DX12_RESOURCE_BARRIER::Transition(atlas.Albedo.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, last),
            CD3DX12_RESOURCE_BARRIER::Transition(atlas.NormalDepth.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, last)};
        commandList->ResourceBarrier(2, lastLevel);
        constexpr auto allShader =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        D3D12_RESOURCE_BARRIER finalBarriers[2] = {
            CD3DX12_RESOURCE_BARRIER::Transition(atlas.Albedo.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                 allShader),
            CD3DX12_RESOURCE_BARRIER::Transition(atlas.NormalDepth.Get(),
                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, allShader)};
        commandList->ResourceBarrier(2, finalBarriers);

        // 4. 描画用 SRV（アルベド sRGB、法線・深度）。
        atlas.SrvIndex = SlotDescriptor(atlas.Slot);
        WriteSrv(atlas.Albedo.Get(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 0, atlas.MipLevels, atlas.SrvIndex);
        WriteSrv(atlas.NormalDepth.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 0, atlas.MipLevels, atlas.SrvIndex + 1);

        RetireResource(bakeAlbedo);
        RetireResource(bakeNormal);
        RetireResource(bakeDepth);
        atlas.State = AtlasState::Ready;
        Debug::Log("Impostor: baked %ux%u frames, %u px, %u mips (radius %.2f)\n", frames, frames, resolution,
                   atlas.MipLevels, atlas.Radius);
        return true;
    }
};

