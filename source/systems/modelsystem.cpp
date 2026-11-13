#include "pch.h"
#include "editorvisibility.h"
#include "noisecachebindings.h"
#include "envflags.h"
#include "transmittanceshadows.h"
#include "materialmanager.h"
#include "meshshader/meshshaderrenderer.h"
#include "modelsystem.h"
#include "gpudriven/gpudrivenrenderer.h"
#include "raytracing.h"
#include "vertexdeformation.h"
#include "atmosphere.h"
#include "gpudrivenindirect.h"
#include "instancingsystem.h"
#include "componentmanager.h"
#include "modelmanager.h"
#include "texturemanager.h"
#include "systemmanager.h"
#include "graphicsdevice.h"
#include "shaderpipelines.h"
#include "psomanager.h"
#include "camera.h"
#include "imguimanager.h"
#include "materialsystem.h"
#include "toonoutlinebuilder.h"
#include "meshshaderpipeline.h"
#include <vector>
#include <algorithm>
#include "world.h"
#include "shaderdescription.h"
#include "scenepasses.h"
#include "materialbindings.h"
#include "frameconstants.h"
#include "lightingresources.h"
#include "gpuobjectculling.h"
#include "renderconfiguration.h"
#include "materialpartresolver.h"
#include "mekakure.h"
#include "livetoonstyle.h"
#include <type_traits>

static const MaterialComponent& DefaultMaterial()
{
    static const MaterialComponent material{};
    return material;
}

static const MaterialComponent& LegacyMaterialForEntity(EntityID entity)
{
    if (ComponentManager::HasComponent<MaterialComponent>(entity))
    {
        return ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
    }
    return DefaultMaterial();
}

static bool IsSkyEntity(EntityID entity)
{
    return ComponentManager::HasComponent<NameComponent>(entity) &&
           ComponentManager::GetComponentUnchecked<NameComponent>(entity).Name == "Sky";
}

static bool ShouldCastShadow(EntityID entity, bool transmission = false)
{
    // 環境メッシュは背景放射輝度を供給するため、太陽を遮蔽させない。
    if (ComponentManager::HasComponent<MeshComponent>(entity) &&
        ComponentManager::GetComponentUnchecked<MeshComponent>(entity).IsEnvironment)
    {
        return false;
    }

    if (IsSkyEntity(entity) || ComponentManager::HasComponent<LightComponent>(entity))
    {
        return false;
    }
    if (!Registry::HasComponent(entity, ComponentType::MATERIAL))
    {
        return true;
    }

    const auto& material = MaterialManager::Resolve(ComponentManager::GetComponentUnchecked<MaterialComponent>(entity));
    const auto& source = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
    for (size_t slot = 0; slot < source.MaterialSlots.size(); ++slot)
    {
        if (MaterialSystem::CastsShadowInPass(MaterialManager::Resolve(source, slot), transmission))
        {
            return true;
        }
    }
    return MaterialSystem::CastsShadowInPass(material, transmission);
}

static bool ShouldDrawToonOutline(EntityID entity)
{
    if (!Registry::HasComponent(entity, ComponentType::MATERIAL))
    {
        return false;
    }

    const auto& material = MaterialManager::Resolve(ComponentManager::GetComponentUnchecked<MaterialComponent>(entity));
    return material.ShaderClassMode == MaterialMode::Auto ||
           (material.ShaderClassMode == MaterialMode::Manual && material.ShaderClass == ShaderClass::Toon);
}

void ApplyToonOutlineConstants(ConstantBuffer3D& cb, const MaterialComponent& material, float widthScale = 1.0f)
{
    cb.ToonOutlineColor = {material.ToonOutlineColor.x, material.ToonOutlineColor.y, material.ToonOutlineColor.z, 0.0f};
    cb.ToonOutlineWidth = material.ToonOutlineWidth * widthScale;
    cb.ToonOutlineScreenWidth = material.ToonOutlineScreenWidth * widthScale;
    cb.ViewportSize = {max(static_cast<float>(GraphicsDevice::GetSceneWidth()), 1.0f),
                       max(static_cast<float>(GraphicsDevice::GetSceneHeight()), 1.0f)};
    cb.ToonOutlineUseScreenSpace =
        static_cast<int>(material.ToonOutlineWidthModeSetting == ToonOutlineWidthMode::ScreenPixels);
}

D3D12_GPU_DESCRIPTOR_HANDLE CreateToonOutlineCbv(UINT8* cbvDataBegin, EntityID entity,
                                                 const MaterialComponent& material, float widthScale = 1.0f)
{
    if (!cbvDataBegin)
    {
        return {};
    }

    const auto* source =
        reinterpret_cast<const ConstantBuffer3D*>(cbvDataBegin + (entity * FrameConstants::g_kCB_ALIGNED_SIZE));
    ConstantBuffer3D outlineConstants = *source;
    ApplyToonOutlineConstants(outlineConstants, material, widthScale);
    return FrameConstants::AllocateTransientConstantBuffer(outlineConstants);
}

int GetTeoModeIndex(const MaterialComponent& material)
{
    return clamp(static_cast<int>(material.ToonTeoRenderMode), 0, kToonOutlineModeCount - 1);
}

template <class TMeshData>
bool ShouldDrawMeshToonOutline(const MaterialComponent& material, UINT meshIndex, const TMeshData& meshData)
{
    if (meshIndex < material.ToonMeshOutlineOverrides.size())
    {
        switch (material.ToonMeshOutlineOverrides[meshIndex])
        {
            case MeshOutlineOverride::ForceOn:
                return true;
            case MeshOutlineOverride::ForceOff:
                return false;
            default:
                break;
        }
    }
    return meshData.DefaultToonOutlineEnabled;
}

float GetMeshToonOutlineWidthScale(const MaterialComponent& material, UINT meshIndex)
{
    if (meshIndex < material.ToonMeshOutlineWidthScales.size())
    {
        return max(material.ToonMeshOutlineWidthScales[meshIndex], 0.0f);
    }
    return 1.0f;
}

bool HasVisibleToonOutlineWidth(const MaterialComponent& material, float scale = 1.0f)
{
    const float width = material.ToonOutlineWidthModeSetting == ToonOutlineWidthMode::ScreenPixels
                            ? material.ToonOutlineScreenWidth
                            : material.ToonOutlineWidth;
    return width > 0.0f && scale > 0.0f;
}

static float GetCameraDepth(EntityID entity, const XMMATRIX& view)
{
    if (!Registry::HasComponent(entity, ComponentType::TRANSFORM))
    {
        return 0.0f;
    }
    const XMFLOAT3& pos = ComponentManager::GetComponentUnchecked<TransformComponent>(entity).Position;
    const XMVECTOR viewPos = XMVector3TransformCoord(XMLoadFloat3(&pos), view);
    return XMVectorGetZ(viewPos);
}

const char* GetModelVsPath(EntityID entity)
{
    if (ComponentManager::HasComponent<ShaderComponent>(entity))
    {
        const auto& shader = ComponentManager::GetComponentUnchecked<ShaderComponent>(entity);
        if (!shader.VsPath.empty())
        {
            return shader.VsPath.c_str();
        }
    }
    return "shader/hlsl/build/modelshaderVS.cso";
}

const char* GetModelPsPath(EntityID entity)
{
    if (ComponentManager::HasComponent<ShaderComponent>(entity))
    {
        const auto& shader = ComponentManager::GetComponentUnchecked<ShaderComponent>(entity);
        if (!shader.PsPath.empty())
        {
            return shader.PsPath.c_str();
        }
    }
    return "shader/hlsl/build/modelshaderPS.cso";
}

// ---------------------------------------------------------------------------------------------
// メカクレ（前髪越しに目・眉を透かす）
// HDR 半透明パスの最後に、対象メッシュ（読み込み時に分類した目 / 眉 / まつ毛）だけを
// 「深度をカメラ側へ寄せた」状態でアルファ合成する（MekakureVS / MekakurePS）。
//
// 透過は「髪だけ」に限定する（プロセカ 3 周年アップデートの考え方：
// 『目の手前にあるのが自キャラの髪のときだけ透かす』をキャラ単位で判定）。
// ステンシルの代わりに、キャラごとに R32G32 のマスクを作る（DSV は D32 でステンシルが無いため）：
//   R = 自キャラの「髪以外」（顔・瞼・肌・体）の最も手前の距離（-距離を MAX 合成）
//       → 目より手前に皮膚があるピクセル（瞼・横顔の頬や鼻）は透かさない
//   G = 画面上で一番手前に見えている自キャラの髪の被覆（0: 髪なし / 1〜2: 髪あり、2 - G = 透け具合）
//       → 手前に見えているのが髪のときだけ透かす（他キャラの髪・背景越しには透けない）
//  - 深度は書かない・ルートシグネチャ変更なし・既存パラメーターの意味は変更なし
//  - 対象メッシュの判定は読み込み時の SemanticFlags（ビット演算のみ）。名前フィルターが空なら
//    毎フレームの文字列処理は発生しない
//  - 髪メッシュが分類できないモデルでは髪の被覆判定を省略し、皮膚の遮蔽判定だけを行う
// ---------------------------------------------------------------------------------------------
template <typename TMeshData> static bool IsMekakureTargetMesh(const MaterialComponent& material, const TMeshData& mesh)
{
    if (!material.MekakureExcludeNames.empty() &&
        MeshSemanticKeyMatchesFilter(mesh.SemanticKey, material.MekakureExcludeNames))
    {
        return false;
    }
    if ((mesh.SemanticFlags & static_cast<uint32_t>(material.MekakureTargetMask)) != 0)
    {
        return true;
    }
    return !material.MekakureIncludeNames.empty() &&
           MeshSemanticKeyMatchesFilter(mesh.SemanticKey, material.MekakureIncludeNames);
}

namespace
{
    // ---- メカクレ用の GPU リソース（PSO・キャラ単位マスク） ----
    constexpr UINT kMekakureMaskSrvIndex = RendererState::g_kQUALITY_DESCRIPTOR_START + 12;
    constexpr float kMekakureHairPush = 0.002f; // 髪の被覆：深度の誤差で自分の髪に負けないよう少しだけ手前へ
    constexpr DXGI_FORMAT kMekakureMaskFormat = DXGI_FORMAT_R32G32_FLOAT;
    constexpr float kMekakureMaskClear[4] = {-1.0e30f, 0.0f, 0.0f, 0.0f}; // R: 遮蔽なし, G: 髪なし

    // MekakureMaskPS のモード（ConstantBuffer3D::MaterialPadding）
    constexpr float kMaskModeHairAlpha = 0.0f;   // 髪テクスチャのアルファで透け具合
    constexpr float kMaskModeHairRed = 1.0f;     // 専用マスク画像の R で透け具合
    constexpr float kMaskModeHairCoverage = 2.0f; // 透け具合の制限なし（被覆だけ）
    constexpr float kMaskModeOccluder = 3.0f;    // 髪以外（皮膚など）の距離

    enum class MekakurePsoKind
    {
        Overlay,  // 目・眉・まつ毛をシーンへ合成
        Hair,     // 髪の被覆（深度テストあり：手前に見えている髪だけ）
        Occluder, // 髪以外の距離（深度テストなし：自キャラ内の前後関係だけを見る）
    };

    struct MekakureGpu
    {
        ComPtr<ID3D12PipelineState> OverlayPso;
        ComPtr<ID3D12PipelineState> HairPso;
        ComPtr<ID3D12PipelineState> OccluderPso;
        DXGI_FORMAT OverlayFormat = DXGI_FORMAT_UNKNOWN;
        ComPtr<ID3D12Resource> Mask;
        ComPtr<ID3D12Resource> Retired[RendererState::g_kFRAME_COUNT + 1];
        UINT RetiredCursor = 0;
        ComPtr<ID3D12DescriptorHeap> RtvHeap;
        UINT Width = 0;
        UINT Height = 0;
        bool PsoFailed = false;
    };

    MekakureGpu& GetMekakureGpu()
    {
        static MekakureGpu gpu;
        return gpu;
    }

    ID3D12PipelineState* CreateMekakurePso(const char* vsPath, const char* psPath, DXGI_FORMAT format,
                                           MekakurePsoKind kind)
    {
        ComPtr<ID3DBlob> vs;
        ComPtr<ID3DBlob> ps;
        ShaderDescription vsDesc{};
        vsDesc.csoPath = vsPath;
        vsDesc.ppBlob = vs.GetAddressOf();
        ShaderDescription psDesc{};
        psDesc.csoPath = psPath;
        psDesc.ppBlob = ps.GetAddressOf();
        if (!ShaderPipelines::LoadShaderBlob(vsDesc) || !ShaderPipelines::LoadShaderBlob(psDesc))
        {
            return nullptr;
        }
        static D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
        const bool overlay = kind == MekakurePsoKind::Overlay;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = ShaderPipelines::GetModelRootSignature();
        desc.VS = CD3DX12_SHADER_BYTECODE(vs.Get());
        desc.PS = CD3DX12_SHADER_BYTECODE(ps.Get());
        desc.InputLayout = {layout, _countof(layout)};
        desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        desc.RasterizerState.CullMode = overlay ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
        desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        auto& blend = desc.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        if (overlay)
        {
            // 乗算済みアルファ
            blend.SrcBlend = D3D12_BLEND_ONE;
            blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            blend.BlendOp = D3D12_BLEND_OP_ADD;
            blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
            blend.DestBlendAlpha = D3D12_BLEND_ONE;
            blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        }
        else
        {
            // R: 一番手前の皮膚（-距離の最大）, G: 髪が重なったら一番「透かさない」値（2 - 透け具合の最大）
            blend.SrcBlend = D3D12_BLEND_ONE;
            blend.DestBlend = D3D12_BLEND_ONE;
            blend.BlendOp = D3D12_BLEND_OP_MAX;
            blend.SrcBlendAlpha = D3D12_BLEND_ONE;
            blend.DestBlendAlpha = D3D12_BLEND_ONE;
            blend.BlendOpAlpha = D3D12_BLEND_OP_MAX;
        }
        blend.RenderTargetWriteMask = overlay ? D3D12_COLOR_WRITE_ENABLE_ALL
                                              : (D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN);
        desc.SampleMask = UINT_MAX;
        desc.DepthStencilState.DepthEnable = kind == MekakurePsoKind::Occluder ? FALSE : TRUE;
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL; // 逆 Z
        desc.DepthStencilState.StencilEnable = FALSE;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = format;
        desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        desc.SampleDesc.Count = 1;
        ID3D12PipelineState* pso = nullptr;
        if (FAILED(GraphicsDevice::GetDevice()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso))))
        {
            Debug::Log("ERROR: Mekakure PSO creation failed (%s)\n", psPath);
            return nullptr;
        }
        return pso;
    }

    bool EnsureMekakureResources(UINT width, UINT height)
    {
        MekakureGpu& gpu = GetMekakureGpu();
        ID3D12Device* device = GraphicsDevice::GetDevice();
        const DXGI_FORMAT sceneFormat = RendererState::GetSceneColorFormat();
        if (!gpu.PsoFailed && (!gpu.OverlayPso || gpu.OverlayFormat != sceneFormat))
        {
            gpu.OverlayPso.Attach(CreateMekakurePso("shader/hlsl/build/MekakureVS.cso",
                                                    "shader/hlsl/build/MekakurePS.cso", sceneFormat,
                                                    MekakurePsoKind::Overlay));
            if (!gpu.HairPso)
            {
                gpu.HairPso.Attach(CreateMekakurePso("shader/hlsl/build/MekakureVS.cso",
                                                     "shader/hlsl/build/MekakureMaskPS.cso", kMekakureMaskFormat,
                                                     MekakurePsoKind::Hair));
            }
            if (!gpu.OccluderPso)
            {
                gpu.OccluderPso.Attach(CreateMekakurePso("shader/hlsl/build/MekakureVS.cso",
                                                         "shader/hlsl/build/MekakureMaskPS.cso", kMekakureMaskFormat,
                                                         MekakurePsoKind::Occluder));
            }
            gpu.OverlayFormat = sceneFormat;
            gpu.PsoFailed = !gpu.OverlayPso || !gpu.HairPso || !gpu.OccluderPso;
        }
        if (gpu.PsoFailed)
        {
            return false;
        }
        if (!gpu.RtvHeap)
        {
            D3D12_DESCRIPTOR_HEAP_DESC heap{};
            heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            heap.NumDescriptors = 1;
            if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&gpu.RtvHeap))))
            {
                return false;
            }
        }
        if (!gpu.Mask || gpu.Width != width || gpu.Height != height)
        {
            if (gpu.Mask)
            {
                // GPU がまだ使っている可能性があるので数フレーム保持してから解放する。
                gpu.Retired[gpu.RetiredCursor] = gpu.Mask;
                gpu.RetiredCursor = (gpu.RetiredCursor + 1) % _countof(gpu.Retired);
                gpu.Mask.Reset();
            }
            const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(kMekakureMaskFormat, width, height, 1, 1, 1, 0,
                                                           D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
            const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
            D3D12_CLEAR_VALUE clear{};
            clear.Format = kMekakureMaskFormat;
            clear.Color[0] = kMekakureMaskClear[0];
            clear.Color[1] = kMekakureMaskClear[1];
            clear.Color[2] = kMekakureMaskClear[2];
            clear.Color[3] = kMekakureMaskClear[3];
            if (FAILED(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
                                                       IID_PPV_ARGS(&gpu.Mask))))
            {
                return false;
            }
            gpu.Mask->SetName(L"MekakureHairMask");
            device->CreateRenderTargetView(gpu.Mask.Get(), nullptr, gpu.RtvHeap->GetCPUDescriptorHandleForHeapStart());
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = kMekakureMaskFormat;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(
                gpu.Mask.Get(), &srv,
                CD3DX12_CPU_DESCRIPTOR_HANDLE(FrameConstants::GetCbvHeap()->GetCPUDescriptorHandleForHeapStart(),
                                              kMekakureMaskSrvIndex, FrameConstants::GetCbvIncrementSize()));
            gpu.Width = width;
            gpu.Height = height;
        }
        return true;
    }

    struct MekakureEntityDraw
    {
        EntityID Entity = g_kINVALID_ENTITY;
        const MaterialComponent* Source = nullptr;
        AnimationModelResource* Animated = nullptr;
        StaticModelResource* Static = nullptr;
        vector<UINT> EyeMeshes;
        vector<int> EyeParts; // EyeMeshes と同じ並び。MekakurePart の番号
        vector<UINT> HairMeshes;
        vector<UINT> OccluderMeshes; // 髪・透過対象以外（顔・瞼・肌・体）。目より手前なら透かさない
    };
} // namespace

void Mekakure::Release()
{
    GetMekakureGpu() = MekakureGpu{};
}

void Mekakure::Render(ID3D12GraphicsCommandList* commandList, D3D12_CPU_DESCRIPTOR_HANDLE sceneRtv,
                      D3D12_CPU_DESCRIPTOR_HANDLE depthDsv, const D3D12_VIEWPORT& viewport, const D3D12_RECT& scissor)
{
    if (!commandList)
    {
        return;
    }

    // ---- 対象の収集（ビット演算のみ。名前フィルターが空なら文字列処理なし） ----
    static vector<MekakureEntityDraw> draws;
    draws.clear();
    auto collect = [&](EntityID entity, auto* model) {
        if (!model || !ComponentManager::HasComponent<MaterialComponent>(entity))
        {
            return;
        }
        const auto& source = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
        if (!source.MekakureEnabled)
        {
            return;
        }
        bool anyVisiblePart = false;
        for (const auto& part : source.MekakureParts)
        {
            anyVisiblePart |= (part.Initialized ? part.Opacity : source.MekakureOpacity) > 0.001f;
        }
        if (!anyVisiblePart)
        {
            return;
        }
        MekakureEntityDraw draw;
        draw.Entity = entity;
        draw.Source = &source;
        for (UINT m = 0; m < model->GetMeshCount(); ++m)
        {
            const auto& mesh = model->GetMeshData(m);
            if (!mesh.VertexBuffer || !mesh.IndexBuffer || mesh.IndexCount == 0)
            {
                continue;
            }
            if (IsMekakureTargetMesh(source, mesh))
            {
                draw.EyeMeshes.push_back(m);
                draw.EyeParts.push_back(
                    ResolveMekakurePartIndex(mesh.SemanticFlags, static_cast<uint32_t>(source.MekakureTargetMask)));
            }
            else if ((mesh.SemanticFlags & MeshSemantic_Hair) != 0)
            {
                draw.HairMeshes.push_back(m); // 透け具合のマスクが無くても「髪の被覆」として常に使う
            }
            else if ((mesh.SemanticFlags & MeshSemantic_Accessory) == 0)
            {
                // 半透明スロット（頬染め・涙などの重ね板）は目を隠さない
                if (!source.MaterialSlots.empty() &&
                    MaterialSystem::IsTransparentMaterial(MaterialManager::Resolve(source, m)))
                {
                    continue;
                }
                draw.OccluderMeshes.push_back(m);
            }
        }
        if (draw.EyeMeshes.empty())
        {
            return;
        }
        if constexpr (std::is_same_v<std::decay_t<decltype(*model)>, AnimationModelResource>)
        {
            draw.Animated = model;
        }
        else
        {
            draw.Static = model;
        }
        draws.push_back(std::move(draw));
    };
    for (EntityID entity : World::GetView<AnimationModelComponent, TransformComponent>())
    {
        if (EditorVisibility::IsHidden(entity))
        {
            continue; // #5 アウトライナーで非表示
        }
        const auto& component = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity);
        collect(entity, component.ModelId >= 0 ? ModelManager::GetAnimModel(component.ModelId) : nullptr);
    }
    for (EntityID entity : World::GetView<StaticModelComponent, TransformComponent>())
    {
        if (EditorVisibility::IsHidden(entity))
        {
            continue; // #5 アウトライナーで非表示
        }
        const auto& component = ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity);
        collect(entity, component.ModelId >= 0 ? ModelManager::GetStaticModel(component.ModelId) : nullptr);
    }
    if (draws.empty())
    {
        return;
    }
    const UINT width = static_cast<UINT>(max(viewport.Width, 1.0f));
    const UINT height = static_cast<UINT>(max(viewport.Height, 1.0f));
    if (!EnsureMekakureResources(width, height))
    {
        return;
    }
    MekakureGpu& gpu = GetMekakureGpu();

    // ---- スキニング済み頂点を頂点バッファの状態へ ----
    static vector<D3D12_RESOURCE_BARRIER> barriers;
    barriers.clear();
    for (auto& draw : draws)
    {
        if (!draw.Animated)
        {
            continue;
        }
        draw.Animated->DispatchGpuSkinning(commandList, 0); // 姿勢が同じなら内部でスキップ
        auto addBarrier = [&](UINT m) {
            ID3D12Resource* vb = draw.Animated->GetMeshData(m).VertexBuffer.Get();
            if (!Raytracing::IsReadingSkinnedVerticesForGlass(vb))
            {
                barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
                    vb, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER));
            }
        };
        for (UINT m : draw.EyeMeshes)
        {
            addBarrier(m);
        }
        for (UINT m : draw.HairMeshes)
        {
            addBarrier(m);
        }
        for (UINT m : draw.OccluderMeshes)
        {
            addBarrier(m);
        }
    }
    if (!barriers.empty())
    {
        commandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    }

    ScenePasses::BeginModelPass(); // ルートシグネチャとライト・影・GI のバインド
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);

    XMMATRIX view = XMMatrixIdentity();
    XMMATRIX projection = XMMatrixIdentity();
    Camera::GetCameraMatrices(Camera::GetCameraEntity(), view, projection);
    XMFLOAT3 eye{};
    {
        XMVECTOR determinant{};
        XMStoreFloat3(&eye, XMMatrixInverse(&determinant, view).r[3]);
    }
    const XMMATRIX transposedView = XMMatrixTranspose(view);
    const XMMATRIX transposedProjection = XMMatrixTranspose(projection);
    const auto heapStart = FrameConstants::GetCbvHeap()->GetGPUDescriptorHandleForHeapStart();
    const UINT increment = FrameConstants::GetCbvIncrementSize();
    const int defaultTexture = TextureManager::GetDefaultTextureIndex();
    const auto handle = [&](int index) { return CD3DX12_GPU_DESCRIPTOR_HANDLE(heapStart, index, increment); };
    const D3D12_CPU_DESCRIPTOR_HANDLE maskRtv = gpu.RtvHeap->GetCPUDescriptorHandleForHeapStart();

    auto baseConstants = [&](EntityID entity) {
        const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
        ConstantBuffer3D cb{};
        cb.World = XMMatrixTranspose(XMLoadFloat4x4(&transform.WorldMatrix));
        cb.View = transposedView;
        cb.Projection = transposedProjection;
        cb.CameraPos = eye;
        cb.ViewportSize = {viewport.Width, viewport.Height};
        return cb;
    };
    auto meshGeometry = [&](const MekakureEntityDraw& draw, UINT m, D3D12_VERTEX_BUFFER_VIEW& vb,
                            D3D12_INDEX_BUFFER_VIEW& ib, UINT& count, int& texture) {
        if (draw.Animated)
        {
            const MeshData& mesh = draw.Animated->GetMeshData(m);
            vb = mesh.VertexBufferView;
            ib = mesh.IndexBufferView;
            count = mesh.IndexCount;
            texture = mesh.TextureIndex;
        }
        else
        {
            const StaticMeshData& mesh = draw.Static->GetMeshData(m);
            vb = mesh.VertexBufferView;
            ib = mesh.IndexBufferView;
            count = mesh.IndexCount;
            texture = mesh.TextureIndex;
        }
    };
    auto drawMesh = [&](const MekakureEntityDraw& draw, UINT m, int textureOverride, int fallbackTexture) {
        D3D12_VERTEX_BUFFER_VIEW vb{};
        D3D12_INDEX_BUFFER_VIEW ib{};
        UINT count = 0;
        int texture = -1;
        meshGeometry(draw, m, vb, ib, count, texture);
        texture = textureOverride >= 0 ? textureOverride : (texture >= 0 ? texture : fallbackTexture);
        TextureManager::TouchTexture(texture);
        commandList->SetGraphicsRootDescriptorTable(1, handle(texture));
        commandList->IASetVertexBuffers(0, 1, &vb);
        commandList->IASetIndexBuffer(&ib);
        commandList->DrawIndexedInstanced(count, 1, 0, 0, 0);
    };

    // ---- キャラ単位：1. マスク（皮膚の距離 + 髪の被覆） → 2. 目・眉・まつ毛の重ね描き ----
    // 他キャラの髪や顔が手前にあっても、自キャラのマスクには載らないので透けない。
    for (const auto& draw : draws)
    {
        const MaterialComponent& source = *draw.Source;
        const MaterialComponent& material = MaterialManager::Resolve(source);
        const int entityTexture = (material.UseTexture && material.TextureID >= 0) ? material.TextureID : defaultTexture;
        const bool hairCoverage = !draw.HairMeshes.empty();

        // 1. マスク
        {
            auto toRt = CD3DX12_RESOURCE_BARRIER::Transition(
                gpu.Mask.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            commandList->ResourceBarrier(1, &toRt);
            commandList->ClearRenderTargetView(maskRtv, kMekakureMaskClear, 0, nullptr);
            commandList->OMSetRenderTargets(1, &maskRtv, FALSE, &depthDsv);

            // 1-a. 髪以外（顔・瞼・肌・体）の最も手前の距離。深度テストなし（自キャラ内の前後だけを見る）
            if (!draw.OccluderMeshes.empty())
            {
                commandList->SetPipelineState(gpu.OccluderPso.Get());
                ConstantBuffer3D cb = baseConstants(draw.Entity);
                cb.ToonOutlineWidth = 0.0f; // 押し出さない（実際の位置で比較する）
                cb.MaterialPadding = kMaskModeOccluder;
                commandList->SetGraphicsRootDescriptorTable(0, FrameConstants::AllocateTransientConstantBuffer(cb));
                for (UINT m : draw.OccluderMeshes)
                {
                    drawMesh(draw, m, -1, entityTexture);
                }
            }

            // 1-b. 手前に見えている自キャラの髪（シーン深度でテスト）。透け具合のマスク設定もここで反映
            if (hairCoverage)
            {
                commandList->SetPipelineState(gpu.HairPso.Get());
                const bool useMask = source.MekakureMaskSource != 0 && source.MekakureMaskStrength > 0.001f;
                ConstantBuffer3D cb = baseConstants(draw.Entity);
                cb.ToonOutlineWidth = kMekakureHairPush;
                cb.MaterialPadding = !useMask                         ? kMaskModeHairCoverage
                                     : source.MekakureMaskSource == 2 ? kMaskModeHairRed
                                                                      : kMaskModeHairAlpha;
                cb.ConstantPadding = source.MekakureMaskInvert ? 1.0f : 0.0f;
                cb.MaterialAlpha = useMask ? 1.0f : 0.0f; // 効きはパーツ別に重ね描きで掛ける
                commandList->SetGraphicsRootDescriptorTable(0, FrameConstants::AllocateTransientConstantBuffer(cb));
                int maskTexture = -1;
                if (useMask && source.MekakureMaskSource == 2)
                {
                    maskTexture = LiveToonTextures::ResolveDescriptor(source.MekakureMaskPath);
                    if (maskTexture < 0)
                    {
                        // マスク画像が読めない場合は制限なし（被覆だけ）で描く
                        cb.MaterialPadding = kMaskModeHairCoverage;
                        cb.MaterialAlpha = 0.0f;
                        commandList->SetGraphicsRootDescriptorTable(
                            0, FrameConstants::AllocateTransientConstantBuffer(cb));
                    }
                }
                for (UINT m : draw.HairMeshes)
                {
                    drawMesh(draw, m, maskTexture, entityTexture);
                }
            }

            auto toSrv = CD3DX12_RESOURCE_BARRIER::Transition(
                gpu.Mask.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            commandList->ResourceBarrier(1, &toSrv);
        }

        // 2. 目・眉・まつ毛の重ね描き（パーツごとに定数を切り替える）
        commandList->OMSetRenderTargets(1, &sceneRtv, FALSE, &depthDsv);
        commandList->SetPipelineState(gpu.OverlayPso.Get());
        commandList->SetGraphicsRootDescriptorTable(8, handle(kMekakureMaskSrvIndex)); // t3 をマスクとして使う
        const bool useMask = source.MekakureMaskSource != 0 && source.MekakureMaskStrength > 0.001f;
        const float globalMaskStrength = useMask ? std::clamp(source.MekakureMaskStrength, 0.0f, 1.0f) : 0.0f;
        int normalTexture = defaultTexture;
        if (material.NormalMapID >= 0)
        {
            normalTexture = material.NormalMapID;
        }
        commandList->SetGraphicsRootDescriptorTable(6, handle(normalTexture));
        MaterialBindings::SetMaterial(draw.Entity, material);

        // パーツの定数はそのパーツの最初のメッシュで 1 回だけ作る
        D3D12_GPU_DESCRIPTOR_HANDLE partCbv[MekakurePart_Count]{};
        bool partReady[MekakurePart_Count]{};
        bool partVisible[MekakurePart_Count]{};
        auto partConstants = [&](int part) {
            if (partReady[part])
            {
                return partVisible[part];
            }
            partReady[part] = true;
            const MekakurePartParams& params = source.MekakureParts[part];
            // パーツ個別の値。未移行（古いシーン）のパーツだけ旧共通設定を使う
            const bool own = params.Initialized;
            const float opacity = own ? params.Opacity : source.MekakureOpacity;
            const float depth = own ? params.Depth : source.MekakureDepth;
            const float fadeStart = own ? params.FadeStart : source.MekakureFadeStart;
            const float fadeEnd = own ? params.FadeEnd : source.MekakureFadeEnd;
            const float maskStrength = std::clamp(params.MaskStrength, 0.0f, 1.0f);
            const bool skinOcclusion = params.SkinOcclusion;
            if (opacity <= 0.001f)
            {
                return false;
            }
            ConstantBuffer3D cb = baseConstants(draw.Entity);
            cb.UseTexture = static_cast<int>(material.UseTexture);
            cb.UseNormalMap = material.NormalMapID >= 0 ? 1 : 0;
            cb.MaterialMetallic = material.Metallic;
            cb.MaterialRoughness = material.Roughness;
            cb.MaterialFresnel = material.Fresnel;
            cb.MaterialMode = static_cast<int>(material.ShaderClassMode);
            cb.ShaderClass = static_cast<int>(material.ShaderClass);
            cb.MaterialAlpha = std::clamp(opacity, 0.0f, 1.0f) * material.Alpha;
            cb.ToonOutlineWidth = max(depth, 0.0f);
            cb.ToonOutlineScreenWidth = fadeStart;
            cb.ConstantPadding = fadeEnd;
            // 0: 髪の被覆なし、1〜2: 髪の被覆がある所だけ透かす（小数部 = 透過マスクの効き）
            cb.MaterialPadding = hairCoverage ? 1.0f + globalMaskStrength * maskStrength : 0.0f;
            cb.ToonOutlineUseScreenSpace = skinOcclusion ? 1 : 0; // bit0: 皮膚で隠す
            partCbv[part] = FrameConstants::AllocateTransientConstantBuffer(cb);
            partVisible[part] = true;
            return true;
        };
        for (size_t e = 0; e < draw.EyeMeshes.size(); ++e)
        {
            const UINT m = draw.EyeMeshes[e];
            const int part = std::clamp(draw.EyeParts[e], 0, static_cast<int>(MekakurePart_Count) - 1);
            if (!partConstants(part))
            {
                continue;
            }
            if (!source.MaterialSlots.empty())
            {
                const auto& slotMaterial = MaterialManager::Resolve(source, m);
                if (MaterialSystem::IsTransparentMaterial(slotMaterial))
                {
                    continue;
                }
                MaterialBindings::BindAssetSlot(draw.Entity, slotMaterial);
            }
            commandList->SetGraphicsRootDescriptorTable(0, partCbv[part]);
            drawMesh(draw, m, -1, entityTexture);
        }
    }

    for (auto& barrier : barriers)
    {
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    }
    if (!barriers.empty())
    {
        commandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    }
}

void ModelDrawBackend::Draw(RenderPass renderPass, bool receivingPostProcessOnly)
{
    if (renderPass == RenderPass::OverlayScene)
    {
        return;
    }
    if (renderPass == RenderPass::ShadowMap || renderPass == RenderPass::TransmittanceShadow)
    {
        ID3D12GraphicsCommandList* pCommandList = GraphicsDevice::GetCommandList();
        if (!pCommandList)
        {
            return;
        }

        const bool transmission = renderPass == RenderPass::TransmittanceShadow;
        const bool referenceDraw = IsShadowReferenceDrawEnabled();
        ID3D12PipelineState* shadowPso =
            transmission ? TransmittanceShadows::GetPipelineState() : PsoManager::GetOrCreateShadowMapPso();
        UINT8* pCbvDataBegin = FrameConstants::GetConstantBufferPtr();
        ID3D12DescriptorHeap* cbvHeap = FrameConstants::GetCbvHeap();
        if (!shadowPso || !pCbvDataBegin || !cbvHeap)
        {
            return;
        }

        // 影パスの可視判定は GPU（GpuObjectCullCS）。PSO を設定する前に取得する。
        const int cullView = GpuObjectCulling::ShadowView(pCommandList);
        pCommandList->SetPipelineState(shadowPso);
        auto writeShadowCb = [&](EntityID entity) {
            XMMATRIX world =
                XMLoadFloat4x4(&ComponentManager::GetComponentUnchecked<TransformComponent>(entity).WorldMatrix);
            ConstantBuffer3D cb{};
            cb.World = XMMatrixTranspose(world);
            cb.UseTexture = 0;
            memcpy(pCbvDataBegin + (entity * FrameConstants::g_kCB_ALIGNED_SIZE), &cb, sizeof(cb));
            pCommandList->SetGraphicsRootDescriptorTable(0, FrameConstants::GetConstantBufferHandle(entity));
            const auto& material =
                MaterialManager::Resolve(ComponentManager::HasComponent<MaterialComponent>(entity)
                                             ? ComponentManager::GetComponentUnchecked<MaterialComponent>(entity)
                                             : DefaultMaterial());
            MaterialBindings::BindAssetSlot(entity, material);
        };

        for (EntityID i : World::GetView<AnimationModelComponent>())
        {
            if (EditorVisibility::IsHidden(i))
            {
                continue; // #5 アウトライナーで非表示
            }
            auto& animComp = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(i);
            if (animComp.ModelId < 0 || !ShouldCastShadow(i, transmission) ||
                !LightingResources::ShouldDrawEntityInCurrentShadowSubset(i) || InstancingSystem::CanInstance(i))
            {
                continue;
            }

            AnimationModelResource* model = ModelManager::GetAnimModel(animComp.ModelId);
            if (!model)
            {
                continue;
            }
            model->DispatchGpuSkinning(pCommandList, 0); // Shadow draws never consume TEO vertices.
            for (UINT m = 0; m < model->GetMeshCount(); ++m)
            {
                const MeshData& meshData = model->GetMeshData(m);
                if (!meshData.VertexBuffer)
                {
                    continue;
                }
                D3D12_RESOURCE_BARRIER vbBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
                    meshData.VertexBuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
                pCommandList->ResourceBarrier(1, &vbBarrier);
            }

            pCommandList->SetGraphicsRootSignature(ShaderPipelines::GetModelRootSignature());
            NoiseCacheBindings::BindMain(pCommandList);
            if (LightingResources::GetShadowCB())
            {
                pCommandList->SetGraphicsRootConstantBufferView(
                    5, LightingResources::GetCurrentShadowConstantBufferAddress());
            }
            TransmittanceShadows::BindCurrentPass(pCommandList);
            pCommandList->SetPipelineState(shadowPso);
            // 以降の描画・間接描画は GPU の判定で破棄される（スキニングは他のパスと共有のため対象外）。
            GpuObjectCulling::Begin(pCommandList, cullView, i);
            writeShadowCb(i);
            const auto& shadowMaterial = MaterialManager::Resolve(LegacyMaterialForEntity(i));
            const bool indirectShadow =
                !transmission && m_IndirectDraws && !referenceDraw &&
                LightingResources::IsCurrentShadowPassVirtualPage() &&
                LegacyMaterialForEntity(i).MaterialSlots.empty() &&
                MaterialSystem::ResolveShadowMode(shadowMaterial) == MaterialShadowMode::Opaque &&
                m_IndirectDraws->ExecuteShadow(
                    pCommandList, model, i,
                    XMLoadFloat4x4(&ComponentManager::GetComponentUnchecked<TransformComponent>(i).WorldMatrix),
                    LightingResources::GetCurrentShadowViewProjection(), shadowPso, true);
            for (UINT m = 0; m < model->GetMeshCount(); ++m)
            {
                const MeshData& meshData = model->GetMeshData(m);
                const auto& slotMaterial = MaterialManager::Resolve(LegacyMaterialForEntity(i), m);
                if (!MaterialSystem::CastsShadowInPass(slotMaterial, transmission))
                {
                    const auto restore = CD3DX12_RESOURCE_BARRIER::Transition(
                        meshData.VertexBuffer.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    pCommandList->ResourceBarrier(1, &restore);
                    continue;
                }
                MaterialBindings::BindAssetSlot(i, slotMaterial, meshData.TextureIndex);
                pCommandList->IASetVertexBuffers(0, 1, &meshData.VertexBufferView);
                pCommandList->IASetIndexBuffer(&meshData.IndexBufferView);
                if (!indirectShadow)
                {
                    pCommandList->DrawIndexedInstanced(meshData.IndexCount, 1, 0, 0, 0);
                }

                D3D12_RESOURCE_BARRIER backBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
                    meshData.VertexBuffer.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                pCommandList->ResourceBarrier(1, &backBarrier);
            }
            GpuObjectCulling::End(pCommandList);
        }

        for (EntityID i : World::GetView<StaticModelComponent>())
        {
            if (EditorVisibility::IsHidden(i))
            {
                continue; // #5 アウトライナーで非表示
            }
            auto& staticComp = ComponentManager::GetComponentUnchecked<StaticModelComponent>(i);
            if (staticComp.ModelId < 0 || !ShouldCastShadow(i, transmission) ||
                !LightingResources::ShouldDrawEntityInCurrentShadowSubset(i) || InstancingSystem::CanInstance(i))
            {
                continue;
            }

            StaticModelResource* model = ModelManager::GetStaticModel(staticComp.ModelId);
            if (!model)
            {
                continue;
            }

            GpuObjectCulling::Begin(pCommandList, cullView, i);
            writeShadowCb(i);
            const auto& shadowMaterial = MaterialManager::Resolve(LegacyMaterialForEntity(i));
            const bool indirectShadow =
                !transmission && m_IndirectDraws && !referenceDraw &&
                LightingResources::IsCurrentShadowPassVirtualPage() && !VertexDeformation::IsRequested(i) &&
                LegacyMaterialForEntity(i).MaterialSlots.empty() &&
                MaterialSystem::ResolveShadowMode(shadowMaterial) == MaterialShadowMode::Opaque &&
                m_IndirectDraws->ExecuteShadow(
                    pCommandList, model, i,
                    XMLoadFloat4x4(&ComponentManager::GetComponentUnchecked<TransformComponent>(i).WorldMatrix),
                    LightingResources::GetCurrentShadowViewProjection(), shadowPso, true);
            for (UINT m = 0; m < model->GetMeshCount(); ++m)
            {
                const StaticMeshData& meshData = model->GetMeshData(m);
                VertexDeformationView deformation;
                const auto vertexView = VertexDeformation::TryResolveStaticMesh(i, m, deformation)
                                            ? deformation.CurrentView
                                            : meshData.VertexBufferView;
                const auto& slotMaterial = MaterialManager::Resolve(LegacyMaterialForEntity(i), m);
                if (!MaterialSystem::CastsShadowInPass(slotMaterial, transmission))
                {
                    continue;
                }
                MaterialBindings::BindAssetSlot(i, slotMaterial, meshData.TextureIndex);
                pCommandList->IASetVertexBuffers(0, 1, &vertexView);
                pCommandList->IASetIndexBuffer(&meshData.IndexBufferView);
                if (!indirectShadow)
                {
                    pCommandList->DrawIndexedInstanced(meshData.IndexCount, 1, 0, 0, 0);
                }
            }
            GpuObjectCulling::End(pCommandList);
        }
        return;
    }

    if (renderPass == RenderPass::Velocity)
    {
        if (RenderConfiguration::GetRenderMode() != RenderMode::DEFERRED)
        {
            return;
        }
        ID3D12GraphicsCommandList* commandList = GraphicsDevice::GetCommandList();
        ID3D12PipelineState* pso = PsoManager::GetVelocityGeometryPso();
        if (!commandList || !pso)
        {
            return;
        }
        XMMATRIX view = XMMatrixIdentity(), projection = XMMatrixIdentity();
        Camera::GetCameraMatrices(Camera::GetCameraEntity(), view, projection);
        XMMATRIX previousViewProjection = XMLoadFloat4x4(&GraphicsDevice::GetPreviousViewMatrix()) *
                                          XMLoadFloat4x4(&GraphicsDevice::GetPreviousProjectionMatrix());
        commandList->SetGraphicsRootSignature(ShaderPipelines::GetModelRootSignature());
        NoiseCacheBindings::BindMain(commandList);
        commandList->SetPipelineState(pso);
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        auto setConstants = [&](EntityID entity) {
            const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
            ConstantBuffer3D cb{};
            cb.World = XMMatrixTranspose(XMLoadFloat4x4(&transform.WorldMatrix));
            cb.View = XMMatrixTranspose(view);
            cb.Projection = XMMatrixTranspose(projection);
            cb.PreviousWorld = XMMatrixTranspose(XMLoadFloat4x4(&transform.PreviousWorldMatrix));
            cb.PreviousViewProjection = XMMatrixTranspose(previousViewProjection);
            commandList->SetGraphicsRootDescriptorTable(0, FrameConstants::AllocateTransientConstantBuffer(cb));
        };

        for (EntityID entity : World::GetView<AnimationModelComponent>())
        {
            if (EditorVisibility::IsHidden(entity))
            {
                continue; // #5 アウトライナーで非表示
            }
            auto& component = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity);
            if (MaterialSystem::IsTransparent(entity))
            {
                continue;
            }
            AnimationModelResource* model = ModelManager::GetAnimModel(component.ModelId);
            if (!model || !ComponentManager::HasComponent<TransformComponent>(entity))
            {
                continue;
            }

            model->DispatchGpuSkinning(commandList, 0);
            commandList->SetGraphicsRootSignature(ShaderPipelines::GetModelRootSignature());
            NoiseCacheBindings::BindMain(commandList);
            commandList->SetPipelineState(pso);
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            setConstants(entity);
            for (UINT mesh = 0; mesh < model->GetMeshCount(); ++mesh)
            {
                const MeshData& data = model->GetMeshData(mesh);
                if (!data.VertexBuffer || !data.PreviousVertexValid)
                {
                    continue;
                }
                D3D12_RESOURCE_BARRIER currentReady =
                    CD3DX12_RESOURCE_BARRIER::Transition(data.VertexBuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                         D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
                commandList->ResourceBarrier(1, &currentReady);
                D3D12_VERTEX_BUFFER_VIEW views[2] = {data.VertexBufferView, data.PreviousVertexBufferView};
                commandList->IASetVertexBuffers(0, 2, views);
                commandList->IASetIndexBuffer(&data.IndexBufferView);
                commandList->DrawIndexedInstanced(data.IndexCount, 1, 0, 0, 0);
                D3D12_RESOURCE_BARRIER currentBack = CD3DX12_RESOURCE_BARRIER::Transition(
                    data.VertexBuffer.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                commandList->ResourceBarrier(1, &currentBack);
            }
        }

        for (EntityID entity : World::GetView<StaticModelComponent>())
        {
            if (EditorVisibility::IsHidden(entity))
            {
                continue; // #5 アウトライナーで非表示
            }
            if (MeshShaderRenderer::IsActive() && GPUDrivenRenderer::HandlesEntity(entity))
            {
                continue;
            }
            auto& component = ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity);
            if (MaterialSystem::IsTransparent(entity))
            {
                continue;
            }
            if (ComponentManager::HasComponent<MeshComponent>(entity) &&
                ComponentManager::GetComponentUnchecked<MeshComponent>(entity).IsEnvironment &&
                Atmosphere::GetParameters().Enabled && Atmosphere::GetParameters().PhysicalSky)
            {
                continue;
            }
            StaticModelResource* model = ModelManager::GetStaticModel(component.ModelId);
            if (!model || !ComponentManager::HasComponent<TransformComponent>(entity))
            {
                continue;
            }
            setConstants(entity);
            for (UINT mesh = 0; mesh < model->GetMeshCount(); ++mesh)
            {
                const StaticMeshData& data = model->GetMeshData(mesh);
                D3D12_VERTEX_BUFFER_VIEW views[2] = {data.VertexBufferView, data.VertexBufferView};
                VertexDeformationView deformation;
                if (VertexDeformation::TryResolveStaticMesh(entity, mesh, deformation))
                {
                    views[0] = deformation.CurrentView;
                    views[1] = deformation.PreviousView;
                }
                commandList->IASetVertexBuffers(0, 2, views);
                commandList->IASetIndexBuffer(&data.IndexBufferView);
                commandList->DrawIndexedInstanced(data.IndexCount, 1, 0, 0, 0);
            }
        }
        return;
    }

    const bool drawTransparent = (renderPass == RenderPass::TransparentScene);
    const bool occlusionPhaseTwo = renderPass == RenderPass::OcclusionPhase2;
    ID3D12GraphicsCommandList* pCommandList = GraphicsDevice::GetCommandList();
    if (!pCommandList)
    {
        return;
    }

    ScenePasses::BeginModelPass();
    UINT8* pCbvDataBegin = FrameConstants::GetConstantBufferPtr();
    UINT cbvIncrement = FrameConstants::GetCbvIncrementSize();
    auto heapStart = FrameConstants::GetCbvHeap()->GetGPUDescriptorHandleForHeapStart();
    const int defaultTextureIndex = TextureManager::GetDefaultTextureIndex();
    const int defaultNormalIndex = TextureManager::GetDefaultTextureIndex();

    XMMATRIX viewMat;
    XMMATRIX projMat;
    Camera::GetCameraMatrices(Camera::GetCameraEntity(), viewMat, projMat);
    const XMMATRIX transposedView = XMMatrixTranspose(viewMat);
    const XMMATRIX transposedProj = XMMatrixTranspose(projMat);
    const XMMATRIX viewProjection = viewMat * projMat;

    {
        m_AnimDrawCalls.clear();
        m_AnimDrawCalls.reserve(World::GetView<AnimationModelComponent>().size());

        auto animEntities = World::GetView<AnimationModelComponent>();
        for (EntityID i : animEntities)
        {
            if (EditorVisibility::IsHidden(i))
            {
                continue; // #5 アウトライナーで非表示
            }
            auto& animComp = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(i);
            if (animComp.ModelId < 0)
            {
                continue;
            }

            bool isReceiving = MaterialSystem::IsReceivingPostProcess(i);
            const MaterialComponent* material;
            if (Registry::HasComponent(i, ComponentType::MATERIAL))
            {
                material = &MaterialManager::Resolve(ComponentManager::GetComponentUnchecked<MaterialComponent>(i));
            }
            else
            {
                material = nullptr;
            }
            if (!MaterialManager::HasSurfaceForPass(i, drawTransparent))
            {
                continue;
            }
            if (receivingPostProcessOnly && !isReceiving)
            {
                continue;
            }
            AnimationModelResource* model = ModelManager::GetAnimModel(animComp.ModelId);
            if (!model)
            {
                continue;
            }
            if (InstancingSystem::CanInstance(i))
            {
                continue;
            }

            ShaderDescription psoResource{};
            psoResource.vsPath = GetModelVsPath(i);
            psoResource.psPath = GetModelPsPath(i);
            psoResource.isModel = true;
            psoResource.enableAlphaBlend = drawTransparent;
            ID3D12PipelineState* pso = PsoManager::GetOrCreateGraphicsPso(psoResource);
            if (!pso)
            {
                continue;
            }

            XMMATRIX world =
                XMLoadFloat4x4(&ComponentManager::GetComponentUnchecked<TransformComponent>(i).WorldMatrix);

            ConstantBuffer3D cb{};
            cb.World = XMMatrixTranspose(world);
            cb.View = transposedView;
            cb.Projection = transposedProj;
            cb.UseTexture = 0;
            MaterialBindings::FillObjectConstants(cb, i);

            int srvIndex = defaultTextureIndex;
            int normalSrvIndex = defaultNormalIndex;
            if (material)
            {
                const auto& mat = *material;
                cb.UseTexture = static_cast<int>(mat.UseTexture);
                cb.MaterialMetallic = mat.Metallic;
                cb.MaterialRoughness = mat.Roughness;
                cb.MaterialFresnel = mat.Fresnel;
                cb.MaterialAlpha = mat.Alpha;
                cb.MaterialIsTransparent = static_cast<int>(mat.IsTransparent);
                cb.MaterialMode = static_cast<int>(mat.ShaderClassMode);
                cb.ShaderClass = static_cast<int>(mat.ShaderClass);
                ApplyToonOutlineConstants(cb, mat);
                cb.MaterialPadding = static_cast<float>(IsSkyEntity(i));
                cb.FlipNormal = static_cast<int>(IsSkyEntity(i));
                if (cb.UseTexture != 0 && mat.TextureID >= 0)
                {
                    srvIndex = mat.TextureID;
                }
                if (mat.NormalMapID >= 0)
                {
                    cb.UseNormalMap = 1;
                    normalSrvIndex = mat.NormalMapID;
                }
            }

            memcpy(pCbvDataBegin + (i * FrameConstants::g_kCB_ALIGNED_SIZE), &cb, sizeof(cb));
            float cameraDepth;
            if (drawTransparent)
            {
                cameraDepth = GetCameraDepth(i, viewMat);
            }
            else
            {
                cameraDepth = 0.0f;
            }
            m_AnimDrawCalls.push_back({i, pso, srvIndex, normalSrvIndex, model, material, cameraDepth});
        }

        sort(m_AnimDrawCalls.begin(), m_AnimDrawCalls.end(),
             [drawTransparent](const AnimDrawCall& a, const AnimDrawCall& b) {
                 if (drawTransparent)
                 {
                     if (fabsf(a.cameraDepth - b.cameraDepth) > 0.0001f)
                     {
                         return a.cameraDepth > b.cameraDepth;
                     }
                 }
                 else
                 {
                     if (fabsf(a.cameraDepth - b.cameraDepth) > 0.0001f)
                     {
                         return a.cameraDepth < b.cameraDepth;
                     }
                 }
                 if (a.pso != b.pso)
                 {
                     return a.pso < b.pso;
                 }
                 if (a.srvIndex != b.srvIndex)
                 {
                     return a.srvIndex < b.srvIndex;
                 }
                 return a.model < b.model;
             });

        ID3D12PipelineState* lastPso = nullptr;
        ID3D12PipelineState* outlinePso = PsoManager::GetOrCreateToonOutlinePso(drawTransparent);

        vector<D3D12_RESOURCE_BARRIER> barriers;
        barriers.reserve(128);

        for (const auto& dc : m_AnimDrawCalls)
        {
            TextureManager::TouchTexture(dc.srvIndex);
            TextureManager::TouchTexture(dc.normalSrvIndex);
            if (InstancingSystem::CanInstance(dc.EntityID))
            {
                continue;
            }
            const auto& outlineMaterial = dc.material ? *dc.material : DefaultMaterial();
            const bool useToonOutlines = !occlusionPhaseTwo && ShouldDrawToonOutline(dc.EntityID) &&
                                         HasVisibleToonOutlineWidth(outlineMaterial);
            const int activeTeoMode = GetTeoModeIndex(outlineMaterial);
            const bool useTeoOutlines = useToonOutlines &&
                                        outlineMaterial.ToonOutlineRenderMode != ToonOutlineMode::Extrude &&
                                        outlineMaterial.ToonOutlineTeoWidthScale > 0.0f;
            dc.model->DispatchGpuSkinning(pCommandList, useTeoOutlines ? (1u << activeTeoMode) : 0u);
            lastPso = nullptr;

            barriers.clear();
            for (UINT m = 0; m < dc.model->GetMeshCount(); m++)
            {
                const MeshData& meshData = dc.model->GetMeshData(m);

                if (!meshData.VertexBuffer)
                {
                    continue;
                }

                if (!Raytracing::IsReadingSkinnedVerticesForGlass(meshData.VertexBuffer.Get()))
                {
                    barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
                        meshData.VertexBuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER));
                }

                for (int teoMode = 0; useToonOutlines && teoMode < kToonOutlineModeCount; ++teoMode)
                {
                    if (!useTeoOutlines || teoMode != activeTeoMode || !meshData.TeoVertexBuffers[teoMode])
                    {
                        continue;
                    }
                    barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
                        meshData.TeoVertexBuffers[teoMode].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER));
                }
            }
            if (!barriers.empty())
            {
                pCommandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
            }

            ScenePasses::BeginModelPass();
            const MaterialComponent* materialSelection = nullptr;
            if (dc.material)
            {
                materialSelection = &(*dc.material);
            }
            else
            {
                materialSelection = &(DefaultMaterial());
            }
            const MaterialComponent& material = *materialSelection;

            // SNI 球面法線補間: 頭ボーン位置を球中心として毎フレーム更新
            auto& srcMaterial = const_cast<MaterialComponent&>(ComponentManager::GetComponentUnchecked<MaterialComponent>(dc.EntityID));
            if (srcMaterial.SniEnabled > 0.5f && dc.model)
            {
                XMFLOAT4X4 headTransform;
                // 一般的な頭ボーン名を試行
                static const char* headBoneNames[] = {"Head", "head", "J_Head", "J_head", "Head_Bone", "head_bone", "Bip01_Head", "Bip01_head"};
                bool found = false;
                for (const char* name : headBoneNames)
                {
                    if (dc.model->GetBoneGlobalTransform(name, headTransform))
                    {
                        srcMaterial.SniSphereCenter = {headTransform._41, headTransform._42, headTransform._43};
                        // 半径を頭ボーンからの推定値に設定（Y軸のエクステントを使用）
                        srcMaterial.SniSphereRadius.x = 0.2f; // デフォルト半径
                        found = true;
                        break;
                    }
                }
                // 頭ボーンが見つからない場合は、モデルの AABB 中心 + 上方向オフセットを使用
                if (!found)
                {
                    XMFLOAT3 center = dc.model->GetAabbCenter();
                    XMFLOAT3 extents = dc.model->GetAabbExtents();
                    srcMaterial.SniSphereCenter = {center.x, center.y + extents.y * 0.5f, center.z};
                    srcMaterial.SniSphereRadius.x = extents.y * 0.5f;
                }
            }

            MaterialBindings::SetMaterial(dc.EntityID, material);

            if (dc.pso != lastPso)
            {
                pCommandList->SetPipelineState(dc.pso);
                lastPso = dc.pso;
            }

            D3D12_GPU_DESCRIPTOR_HANDLE cbvHandle = FrameConstants::GetConstantBufferHandle(dc.EntityID);
            pCommandList->SetGraphicsRootDescriptorTable(0, cbvHandle);

            CD3DX12_GPU_DESCRIPTOR_HANDLE srvHandle(heapStart, dc.srvIndex, cbvIncrement);
            pCommandList->SetGraphicsRootDescriptorTable(1, srvHandle);
            CD3DX12_GPU_DESCRIPTOR_HANDLE normalSrvHandle(heapStart, dc.normalSrvIndex, cbvIncrement);
            pCommandList->SetGraphicsRootDescriptorTable(6, normalSrvHandle);

            const XMMATRIX world =
                XMLoadFloat4x4(&ComponentManager::GetComponentUnchecked<TransformComponent>(dc.EntityID).WorldMatrix);
            const bool indirectBaseDrawn =
                LegacyMaterialForEntity(dc.EntityID).MaterialSlots.empty() && !material.LiveSurfaceOverride &&
                m_IndirectDraws &&
                m_IndirectDraws->ExecutePrimary(pCommandList, dc.model, dc.EntityID, world, viewProjection, dc.srvIndex,
                                                heapStart, cbvIncrement, dc.pso);

            for (UINT m = 0; m < dc.model->GetMeshCount(); m++)
            {
                const MeshData& meshData = dc.model->GetMeshData(m);
                const auto& slotMaterial = MaterialManager::Resolve(LegacyMaterialForEntity(dc.EntityID), m);
                ID3D12PipelineState* sectionPso = dc.pso;
                if (MaterialSystem::IsTransparentMaterial(slotMaterial) != drawTransparent)
                {
                    continue;
                }

                if (!LegacyMaterialForEntity(dc.EntityID).MaterialSlots.empty() || slotMaterial.LiveSurfaceOverride)
                {
                    MaterialBindings::BindAssetSlot(dc.EntityID, slotMaterial);
                }
                if (slotMaterial.RuntimeHandle != 0)
                {
                    ShaderDescription sectionDescription{};
                    sectionDescription.vsPath = GetModelVsPath(dc.EntityID);
                    sectionDescription.psPath = GetModelPsPath(dc.EntityID);
                    sectionDescription.isModel = true;
                    sectionDescription.enableAlphaBlend = drawTransparent;
                    sectionDescription.cullMode = D3D12_CULL_MODE_BACK;
                    if (slotMaterial.AssetCull == MaterialCull::None)
                    {
                        sectionDescription.cullMode = D3D12_CULL_MODE_NONE;
                    }
                    else if (slotMaterial.AssetCull == MaterialCull::Front)
                    {
                        sectionDescription.cullMode = D3D12_CULL_MODE_FRONT;
                    }
                    sectionDescription.depthWrite = slotMaterial.AssetDepthWrite;
                    auto* assetPso = PsoManager::GetOrCreateGraphicsPso(sectionDescription);
                    if (assetPso)
                    {
                        sectionPso = assetPso;
                    }
                    pCommandList->SetPipelineState(sectionPso);
                    lastPso = nullptr;
                }

                if (slotMaterial.RuntimeHandle == 0 && !slotMaterial.LiveSurfaceOverride && meshData.TextureIndex >= 0)
                {
                    TextureManager::TouchTexture(meshData.TextureIndex);
                    CD3DX12_GPU_DESCRIPTOR_HANDLE meshSrvHandle(heapStart, meshData.TextureIndex, cbvIncrement);
                    pCommandList->SetGraphicsRootDescriptorTable(1, meshSrvHandle);
                }
                else if (slotMaterial.RuntimeHandle == 0)
                {
                    CD3DX12_GPU_DESCRIPTOR_HANDLE entitySrvHandle(heapStart, dc.srvIndex, cbvIncrement);
                    pCommandList->SetGraphicsRootDescriptorTable(1, entitySrvHandle);
                }
                if (!indirectBaseDrawn && !occlusionPhaseTwo)
                {
                    pCommandList->SetPipelineState(sectionPso);
                    pCommandList->IASetVertexBuffers(0, 1, &meshData.VertexBufferView);
                    pCommandList->IASetIndexBuffer(&meshData.IndexBufferView);
                    pCommandList->DrawIndexedInstanced(meshData.IndexCount, 1, 0, 0, 0);
                }

                if (!occlusionPhaseTwo && outlinePso && useToonOutlines &&
                    ShouldDrawMeshToonOutline(material, m, meshData) &&
                    HasVisibleToonOutlineWidth(material, GetMeshToonOutlineWidthScale(material, m)))
                {
                    const float meshWidthScale = GetMeshToonOutlineWidthScale(material, m);
                    const int teoMode = GetTeoModeIndex(material);
                    const bool hasTeoMesh = meshData.TeoVertexBuffers[teoMode] && meshData.TeoIndexBuffers[teoMode] &&
                                            meshData.TeoIndexCounts[teoMode] > 0;
                    const bool drawExtrude = material.ToonOutlineRenderMode == ToonOutlineMode::Extrude ||
                                             material.ToonOutlineRenderMode == ToonOutlineMode::Mix ||
                                             (material.ToonOutlineRenderMode == ToonOutlineMode::TEO && !hasTeoMesh);
                    const bool drawTeo = hasTeoMesh && material.ToonOutlineTeoWidthScale > 0.0f &&
                                         (material.ToonOutlineRenderMode == ToonOutlineMode::TEO ||
                                          material.ToonOutlineRenderMode == ToonOutlineMode::Mix);

                    pCommandList->SetPipelineState(outlinePso);
                    if (drawExtrude)
                    {
                        const D3D12_GPU_DESCRIPTOR_HANDLE outlineCbvHandle =
                            CreateToonOutlineCbv(pCbvDataBegin, dc.EntityID, material, meshWidthScale);
                        if (outlineCbvHandle.ptr != 0)
                        {
                            pCommandList->SetGraphicsRootDescriptorTable(0, outlineCbvHandle);
                        }
                        else
                        {
                            pCommandList->SetGraphicsRootDescriptorTable(0, cbvHandle);
                        }
                        pCommandList->IASetVertexBuffers(0, 1, &meshData.VertexBufferView);
                        pCommandList->IASetIndexBuffer(&meshData.IndexBufferView);
                        pCommandList->DrawIndexedInstanced(meshData.IndexCount, 1, 0, 0, 0);
                    }
                    if (drawTeo)
                    {
                        const D3D12_GPU_DESCRIPTOR_HANDLE outlineCbvHandle = CreateToonOutlineCbv(
                            pCbvDataBegin, dc.EntityID, material, meshWidthScale * material.ToonOutlineTeoWidthScale);
                        if (outlineCbvHandle.ptr != 0)
                        {
                            pCommandList->SetGraphicsRootDescriptorTable(0, outlineCbvHandle);
                        }
                        else
                        {
                            pCommandList->SetGraphicsRootDescriptorTable(0, cbvHandle);
                        }
                        pCommandList->IASetVertexBuffers(0, 1, &meshData.TeoVertexBufferViews[teoMode]);
                        pCommandList->IASetIndexBuffer(&meshData.TeoIndexBufferViews[teoMode]);
                        pCommandList->DrawIndexedInstanced(meshData.TeoIndexCounts[teoMode], 1, 0, 0, 0);
                    }
                    pCommandList->SetGraphicsRootDescriptorTable(0, cbvHandle);
                    // The next base draw sets its section PSO explicitly. Avoid an
                    // unused restore after every outline (especially the last mesh).
                    lastPso = nullptr;
                }
            }

            barriers.clear();
            for (UINT m = 0; m < dc.model->GetMeshCount(); m++)
            {
                const MeshData& meshData = dc.model->GetMeshData(m);

                if (!meshData.VertexBuffer)
                {
                    continue;
                }

                if (!Raytracing::IsReadingSkinnedVerticesForGlass(meshData.VertexBuffer.Get()))
                {
                    barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
                        meshData.VertexBuffer.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
                }

                for (int teoMode = 0; useToonOutlines && teoMode < kToonOutlineModeCount; ++teoMode)
                {
                    if (!useTeoOutlines || teoMode != activeTeoMode || !meshData.TeoVertexBuffers[teoMode])
                    {
                        continue;
                    }
                    barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
                        meshData.TeoVertexBuffers[teoMode].Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
                }
            }
            if (!barriers.empty())
            {
                pCommandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
            }
        }
    }

    {
        m_StaticDrawCalls.clear();
        auto staticEntities = !drawTransparent && GPUDrivenRenderer::IsActive()
                                  ? EntityView(&GpuScene::CpuOpaqueStaticEntities())
                                  : World::GetView<StaticModelComponent>();
        m_StaticDrawCalls.reserve(staticEntities.size());
        for (EntityID i : staticEntities)
        {
            if (EditorVisibility::IsHidden(i))
            {
                continue; // #5 アウトライナーで非表示
            }
            const bool gpuBaseDrawn = !drawTransparent && GPUDrivenRenderer::HandlesEntity(i);
            if (gpuBaseDrawn && !ShouldDrawToonOutline(i))
            {
                continue;
            }
            if (ComponentManager::HasComponent<MeshComponent>(i) &&
                ComponentManager::GetComponentUnchecked<MeshComponent>(i).IsEnvironment &&
                Atmosphere::GetParameters().Enabled && Atmosphere::GetParameters().PhysicalSky)
            {
                continue;
            }
            if (InstancingSystem::CanInstance(i))
            {
                continue;
            }
            auto& staticComp = ComponentManager::GetComponentUnchecked<StaticModelComponent>(i);
            if (staticComp.ModelId < 0)
            {
                continue;
            }

            bool isReceiving = MaterialSystem::IsReceivingPostProcess(i);
            const MaterialComponent* material;
            if (Registry::HasComponent(i, ComponentType::MATERIAL))
            {
                material = &MaterialManager::Resolve(ComponentManager::GetComponentUnchecked<MaterialComponent>(i));
            }
            else
            {
                material = nullptr;
            }
            if (!MaterialManager::HasSurfaceForPass(i, drawTransparent))
            {
                continue;
            }
            if (receivingPostProcessOnly && !isReceiving)
            {
                continue;
            }
            StaticModelResource* model = ModelManager::GetStaticModel(staticComp.ModelId);
            if (!model)
            {
                continue;
            }

            ShaderDescription psoResource{};
            psoResource.vsPath = GetModelVsPath(i);
            psoResource.psPath = GetModelPsPath(i);
            psoResource.isModel = true;
            psoResource.enableAlphaBlend = drawTransparent;
            ID3D12PipelineState* pso = PsoManager::GetOrCreateGraphicsPso(psoResource);
            if (!pso)
            {
                continue;
            }

            XMMATRIX world =
                XMLoadFloat4x4(&ComponentManager::GetComponentUnchecked<TransformComponent>(i).WorldMatrix);

            ConstantBuffer3D cb{};
            cb.World = XMMatrixTranspose(world);
            cb.View = transposedView;
            cb.Projection = transposedProj;
            cb.UseTexture = 0;
            MaterialBindings::FillObjectConstants(cb, i);

            int srvIndex = defaultTextureIndex;
            int normalSrvIndex = defaultNormalIndex;
            if (material)
            {
                const auto& mat = *material;
                cb.UseTexture = static_cast<int>(mat.UseTexture);
                cb.MaterialMetallic = mat.Metallic;
                cb.MaterialRoughness = mat.Roughness;
                cb.MaterialFresnel = mat.Fresnel;
                cb.MaterialAlpha = mat.Alpha;
                cb.MaterialIsTransparent = static_cast<int>(mat.IsTransparent);
                cb.MaterialMode = static_cast<int>(mat.ShaderClassMode);
                cb.ShaderClass = static_cast<int>(mat.ShaderClass);
                ApplyToonOutlineConstants(cb, mat);
                cb.MaterialPadding = static_cast<float>(IsSkyEntity(i));
                cb.FlipNormal = static_cast<int>(IsSkyEntity(i));
                if (cb.UseTexture != 0 && mat.TextureID >= 0)
                {
                    srvIndex = mat.TextureID;
                }
                if (mat.NormalMapID >= 0)
                {
                    cb.UseNormalMap = 1;
                    normalSrvIndex = mat.NormalMapID;
                }
            }

            memcpy(pCbvDataBegin + (i * FrameConstants::g_kCB_ALIGNED_SIZE), &cb, sizeof(cb));
            float cameraDepth;
            if (drawTransparent)
            {
                cameraDepth = GetCameraDepth(i, viewMat);
            }
            else
            {
                cameraDepth = 0.0f;
            }
            m_StaticDrawCalls.push_back({i, pso, srvIndex, normalSrvIndex, model, material, cameraDepth});
        }

        sort(m_StaticDrawCalls.begin(), m_StaticDrawCalls.end(),
             [drawTransparent](const StaticDrawCall& a, const StaticDrawCall& b) {
                 if (drawTransparent)
                 {
                     if (fabsf(a.cameraDepth - b.cameraDepth) > 0.0001f)
                     {
                         return a.cameraDepth > b.cameraDepth;
                     }
                 }
                 else
                 {
                     if (fabsf(a.cameraDepth - b.cameraDepth) > 0.0001f)
                     {
                         return a.cameraDepth < b.cameraDepth;
                     }
                 }
                 if (a.pso != b.pso)
                 {
                     return a.pso < b.pso;
                 }
                 if (a.srvIndex != b.srvIndex)
                 {
                     return a.srvIndex < b.srvIndex;
                 }
                 return a.model < b.model;
             });

        ID3D12PipelineState* lastPso = nullptr;
        ID3D12PipelineState* outlinePso = PsoManager::GetOrCreateToonOutlinePso(drawTransparent);
        const bool useMeshShaders = !drawTransparent && RenderConfiguration::GetRenderMode() == RenderMode::DEFERRED &&
                                    MeshShaderPipeline::IsSupported();

        for (const auto& dc : m_StaticDrawCalls)
        {
            TextureManager::TouchTexture(dc.srvIndex);
            TextureManager::TouchTexture(dc.normalSrvIndex);
            const MaterialComponent* materialSelection = nullptr;
            if (dc.material)
            {
                materialSelection = &(*dc.material);
            }
            else
            {
                materialSelection = &(DefaultMaterial());
            }
            const MaterialComponent& material = *materialSelection;

            // SNI 球面法線補間 (StaticModel): 頭部メッシュの境界球 (RayCenter/RayRadius) を使用
            auto& srcMaterial = const_cast<MaterialComponent&>(ComponentManager::GetComponentUnchecked<MaterialComponent>(dc.EntityID));
            if (srcMaterial.SniEnabled > 0.5f && dc.model)
            {
                XMFLOAT3 sphereCenter{0, 0, 0};
                float sphereRadius = 1.0f;
                bool found = false;
                for (UINT m = 0; m < dc.model->GetMeshCount(); ++m)
                {
                    const StaticMeshData& meshData = dc.model->GetMeshData(m);
                    if ((meshData.SemanticFlags & MeshSemantic_Head) != 0)
                    {
                        // ワールド空間に変換
                        XMMATRIX world = XMLoadFloat4x4(&ComponentManager::GetComponentUnchecked<TransformComponent>(dc.EntityID).WorldMatrix);
                        XMVECTOR localCenter = XMLoadFloat3(&meshData.RayCenter);
                        XMVECTOR worldCenter = XMVector3TransformCoord(localCenter, world);
                        XMStoreFloat3(&sphereCenter, worldCenter);
                        sphereRadius = meshData.RayRadius;
                        found = true;
                        break;
                    }
                }
                if (found)
                {
                    srcMaterial.SniSphereCenter = sphereCenter;
                    srcMaterial.SniSphereRadius.x = sphereRadius;
                }
                else
                {
                    // フォールバック: モデル全体の AABB 上部中心
                    XMFLOAT3 center = dc.model->GetAabbCenter();
                    XMFLOAT3 extents = dc.model->GetAabbExtents();
                    srcMaterial.SniSphereCenter = {center.x, center.y + extents.y * 0.5f, center.z};
                    srcMaterial.SniSphereRadius.x = extents.y * 0.5f;
                }
            }

            MaterialBindings::SetMaterial(dc.EntityID, material);

            if (dc.pso != lastPso)
            {
                pCommandList->SetPipelineState(dc.pso);
                lastPso = dc.pso;
            }

            D3D12_GPU_DESCRIPTOR_HANDLE cbvHandle = FrameConstants::GetConstantBufferHandle(dc.EntityID);
            pCommandList->SetGraphicsRootDescriptorTable(0, cbvHandle);

            CD3DX12_GPU_DESCRIPTOR_HANDLE srvHandle(heapStart, dc.srvIndex, cbvIncrement);
            pCommandList->SetGraphicsRootDescriptorTable(1, srvHandle);
            CD3DX12_GPU_DESCRIPTOR_HANDLE normalSrvHandle(heapStart, dc.normalSrvIndex, cbvIncrement);
            pCommandList->SetGraphicsRootDescriptorTable(6, normalSrvHandle);

            const XMMATRIX world =
                XMLoadFloat4x4(&ComponentManager::GetComponentUnchecked<TransformComponent>(dc.EntityID).WorldMatrix);
            const bool deforming = VertexDeformation::IsRequested(dc.EntityID);
            const bool gpuBaseDrawn = !drawTransparent && GPUDrivenRenderer::HandlesEntity(dc.EntityID);
            const bool indirectBaseDrawn =
                gpuBaseDrawn ||
                (!deforming && LegacyMaterialForEntity(dc.EntityID).MaterialSlots.empty() &&
                 !material.LiveSurfaceOverride && !useMeshShaders &&
                 m_IndirectDraws &&
                 m_IndirectDraws->ExecutePrimary(pCommandList, dc.model, dc.EntityID, world, viewProjection,
                                                 dc.srvIndex, heapStart, cbvIncrement, dc.pso));

            for (UINT m = 0; m < dc.model->GetMeshCount(); m++)
            {
                const StaticMeshData& meshData = dc.model->GetMeshData(m);
                const auto& slotMaterial = MaterialManager::Resolve(LegacyMaterialForEntity(dc.EntityID), m);
                ID3D12PipelineState* sectionPso = dc.pso;
                if (MaterialSystem::IsTransparentMaterial(slotMaterial) != drawTransparent)
                {
                    continue;
                }

                if (!LegacyMaterialForEntity(dc.EntityID).MaterialSlots.empty() || slotMaterial.LiveSurfaceOverride)
                {
                    MaterialBindings::BindAssetSlot(dc.EntityID, slotMaterial);
                }
                if (slotMaterial.RuntimeHandle != 0)
                {
                    ShaderDescription sectionDescription{};
                    sectionDescription.vsPath = GetModelVsPath(dc.EntityID);
                    sectionDescription.psPath = GetModelPsPath(dc.EntityID);
                    sectionDescription.isModel = true;
                    sectionDescription.enableAlphaBlend = drawTransparent;
                    sectionDescription.cullMode = D3D12_CULL_MODE_BACK;
                    if (slotMaterial.AssetCull == MaterialCull::None)
                    {
                        sectionDescription.cullMode = D3D12_CULL_MODE_NONE;
                    }
                    else if (slotMaterial.AssetCull == MaterialCull::Front)
                    {
                        sectionDescription.cullMode = D3D12_CULL_MODE_FRONT;
                    }
                    sectionDescription.depthWrite = slotMaterial.AssetDepthWrite;
                    auto* assetPso = PsoManager::GetOrCreateGraphicsPso(sectionDescription);
                    if (assetPso)
                    {
                        sectionPso = assetPso;
                    }
                    pCommandList->SetPipelineState(sectionPso);
                    lastPso = nullptr;
                }

                VertexDeformationView deformation;
                const auto vertexView = VertexDeformation::TryResolveStaticMesh(dc.EntityID, m, deformation)
                                            ? deformation.CurrentView
                                            : meshData.VertexBufferView;

                if (slotMaterial.RuntimeHandle == 0 && !slotMaterial.LiveSurfaceOverride && meshData.TextureIndex >= 0)
                {
                    TextureManager::TouchTexture(meshData.TextureIndex);
                    CD3DX12_GPU_DESCRIPTOR_HANDLE meshSrvHandle(heapStart, meshData.TextureIndex, cbvIncrement);
                    pCommandList->SetGraphicsRootDescriptorTable(1, meshSrvHandle);
                }
                else if (slotMaterial.RuntimeHandle == 0)
                {
                    CD3DX12_GPU_DESCRIPTOR_HANDLE entitySrvHandle(heapStart, dc.srvIndex, cbvIncrement);
                    pCommandList->SetGraphicsRootDescriptorTable(1, entitySrvHandle);
                }
                MeshShaderPipeline::Geometry geometry;
                geometry.Vertices =
                    deformation.CurrentResource ? deformation.CurrentResource : meshData.VertexBuffer.Get();
                geometry.Indices = meshData.IndexBuffer.Get();
                geometry.VertexView = vertexView;
                geometry.IndexView = meshData.IndexBufferView;
                geometry.ElementCount = meshData.IndexCount;
                XMFLOAT3 localCenter = dc.model->GetAabbCenter(), localExtents = dc.model->GetAabbExtents();
                VertexDeformation::TryGetLocalBounds(dc.EntityID, localCenter, localExtents);
                const bool meshShaderDrawn =
                    !gpuBaseDrawn && slotMaterial.RuntimeHandle == 0 && useMeshShaders &&
                    MeshShaderPipeline::Draw(pCommandList, geometry, localCenter, localExtents);

                if (!meshShaderDrawn && !indirectBaseDrawn && !occlusionPhaseTwo)
                {
                    pCommandList->SetPipelineState(sectionPso);
                    pCommandList->IASetVertexBuffers(0, 1, &vertexView);
                    pCommandList->IASetIndexBuffer(&meshData.IndexBufferView);
                    pCommandList->DrawIndexedInstanced(meshData.IndexCount, 1, 0, 0, 0);
                }

                if (!occlusionPhaseTwo && outlinePso && ShouldDrawToonOutline(dc.EntityID) &&
                    ShouldDrawMeshToonOutline(material, m, meshData) &&
                    HasVisibleToonOutlineWidth(material, GetMeshToonOutlineWidthScale(material, m)))
                {
                    const float meshWidthScale = GetMeshToonOutlineWidthScale(material, m);
                    const int teoMode = GetTeoModeIndex(material);
                    const bool hasTeoMesh = meshData.TeoVertexBuffers[teoMode] && meshData.TeoIndexBuffers[teoMode] &&
                                            meshData.TeoIndexCounts[teoMode] > 0;
                    const bool drawExtrude = material.ToonOutlineRenderMode == ToonOutlineMode::Extrude ||
                                             material.ToonOutlineRenderMode == ToonOutlineMode::Mix ||
                                             (material.ToonOutlineRenderMode == ToonOutlineMode::TEO && !hasTeoMesh);
                    const bool drawTeo = hasTeoMesh && material.ToonOutlineTeoWidthScale > 0.0f &&
                                         (material.ToonOutlineRenderMode == ToonOutlineMode::TEO ||
                                          material.ToonOutlineRenderMode == ToonOutlineMode::Mix);

                    pCommandList->SetPipelineState(outlinePso);
                    if (drawExtrude)
                    {
                        const D3D12_GPU_DESCRIPTOR_HANDLE outlineCbvHandle =
                            CreateToonOutlineCbv(pCbvDataBegin, dc.EntityID, material, meshWidthScale);
                        if (outlineCbvHandle.ptr != 0)
                        {
                            pCommandList->SetGraphicsRootDescriptorTable(0, outlineCbvHandle);
                        }
                        else
                        {
                            pCommandList->SetGraphicsRootDescriptorTable(0, cbvHandle);
                        }
                        pCommandList->IASetVertexBuffers(0, 1, &vertexView);
                        pCommandList->IASetIndexBuffer(&meshData.IndexBufferView);
                        pCommandList->DrawIndexedInstanced(meshData.IndexCount, 1, 0, 0, 0);
                    }
                    if (drawTeo)
                    {
                        const D3D12_GPU_DESCRIPTOR_HANDLE outlineCbvHandle = CreateToonOutlineCbv(
                            pCbvDataBegin, dc.EntityID, material, meshWidthScale * material.ToonOutlineTeoWidthScale);
                        if (outlineCbvHandle.ptr != 0)
                        {
                            pCommandList->SetGraphicsRootDescriptorTable(0, outlineCbvHandle);
                        }
                        else
                        {
                            pCommandList->SetGraphicsRootDescriptorTable(0, cbvHandle);
                        }
                        VertexDeformationView outlineDeformation;
                        const auto outlineView =
                            VertexDeformation::TryResolveStaticMesh(dc.EntityID, m, outlineDeformation, teoMode)
                                ? outlineDeformation.CurrentView
                                : meshData.TeoVertexBufferViews[teoMode];
                        pCommandList->IASetVertexBuffers(0, 1, &outlineView);
                        pCommandList->IASetIndexBuffer(&meshData.TeoIndexBufferViews[teoMode]);
                        pCommandList->DrawIndexedInstanced(meshData.TeoIndexCounts[teoMode], 1, 0, 0, 0);
                    }
                    pCommandList->SetGraphicsRootDescriptorTable(0, cbvHandle);
                    // The next base draw sets its section PSO explicitly. Avoid an
                    // unused restore after every outline (especially the last mesh).
                    lastPso = nullptr;
                }
            }
        }
    }

}



