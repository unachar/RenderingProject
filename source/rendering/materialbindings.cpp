#include "pch.h"
#include "transmittanceshadows.h"
#include "materialbindings.h"
#include "graphicsdevice.h"
#include "componentmanager.h"
#include "world.h"
#include "materialsystem.h"
#include "materialmanager.h"
#include "frameconstants.h"
#include "texturemanager.h"

namespace
{
uint64_t HashGeometry(const void* data, size_t size)
{
    uint64_t hash = 1469598103934665603ull;
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i)
    {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

struct MaterialPartShaderConstants
{
    XMFLOAT4 Basic{};
    XMFLOAT4 Base{};
    XMFLOAT4 Shadow0{};
    XMFLOAT4 Shadow1{};
    XMFLOAT4 Highlight{};
    XMFLOAT4 RimStyle{};
    XMFLOAT4 RimLight{};
    XMFLOAT4 Skin0{};
    XMFLOAT4 Skin1{};
};

struct PBRConstants
{
    float Metallic = 0.0f;
    float Roughness = 0.5f;
    float Fresnel = 0.04f;
    float NormalBlend = 0.0f;
    float NormalBias = 0.0f;
    float BaseSaturation = 1.2f;
    float BaseBrightness = 1.0f;
    float ShadowThreshold = 0.44f;
    float ShadowSoftness = 0.045f;
    float ShadowStrength = 1.0f;
    float MidStrength = 1.0f;
    float LitStrength = 1.0f;
    float RimStrength = 0.45f;
    float RimThreshold = 0.70f;
    float RimSoftness = 0.055f;
    float RimPower = 1.0f;
    XMFLOAT3 RimColor = {0.38f, 0.48f, 0.80f};
    float RimAlbedoBlend = 0.20f;
    float RimLightBlend = 0.35f;
    float SpecularStrength = 0.35f;
    float SpecularThreshold = 0.35f;
    float KawaiiBlend = 1.0f;
    float SkinScatterStrength = 1.0f;
    float SkinScatterWrap = 0.42f;
    float SkinBacklightStrength = 1.0f;
    float SkinRimScatterStrength = 1.0f;
    float SkinOilSpecularStrength = 1.0f;
    float SkinShadowScatter = 0.54f;
    float CastShadowThreshold = 0.28f;
    float CastShadowSoftness = 0.10f;
    // HLSL starts the next float4 at a 16-byte boundary after float3 PBRPadding.
    float Padding[4]{};
    XMFLOAT4 Transparent0 = {1.50f, 0.98f, 0.02f, 0.035f};
    XMFLOAT4 Transparent1 = {0.10f, 0.02f, 0.01f, 0.005f};
    MaterialPartShaderConstants PartParams[kMaterialPartParamCount]{};
    XMFLOAT4 AssetBaseColor{1, 1, 1, 1};
    XMFLOAT4 AssetSurface{1, 1, 0, 0.5f}; // Normal scale, AO strength, enabled, alpha cutoff.
    XMFLOAT4 AssetEmission{};
    XMUINT4 AssetTextures{UINT32_MAX, UINT32_MAX, UINT32_MAX, 0}; // MR, AO, emissive, surface/cull flags.
};

void WriteMaterialPartConstants(PBRConstants& constants, int index, const MaterialPartParams& params)
{
    if (index < 0 || index >= kMaterialPartParamCount)
    {
        return;
    }

    constants.PartParams[index].Basic = XMFLOAT4(params.Metallic, params.Roughness, params.Fresnel, params.NormalBlend);
    constants.PartParams[index].Base =
        XMFLOAT4(params.NormalBias, params.BaseSaturation, params.BaseBrightness, params.KawaiiBlend);
    constants.PartParams[index].Shadow0 =
        XMFLOAT4(params.ShadowThreshold, params.ShadowSoftness, params.ShadowStrength, params.MidStrength);
    constants.PartParams[index].Shadow1 =
        XMFLOAT4(params.LitStrength, params.CastShadowThreshold, params.CastShadowSoftness, 0.0f);
    constants.PartParams[index].Highlight =
        XMFLOAT4(params.RimStrength, params.RimThreshold, params.SpecularStrength, params.SpecularThreshold);
    constants.PartParams[index].RimStyle =
        XMFLOAT4(params.RimSoftness, params.RimPower, params.RimAlbedoBlend, params.RimLightBlend);
    constants.PartParams[index].RimLight = XMFLOAT4(params.RimColor.x, params.RimColor.y, params.RimColor.z, 1.0f);
    constants.PartParams[index].Skin0 = XMFLOAT4(params.SkinScatterStrength, params.SkinScatterWrap,
                                                 params.SkinBacklightStrength, params.SkinRimScatterStrength);
    constants.PartParams[index].Skin1 = XMFLOAT4(params.SkinOilSpecularStrength, params.SkinShadowScatter, 0.0f, 0.0f);
}

PBRConstants BuildPBRConstantsFromMaterial(const MaterialComponent& material)
{
    PBRConstants constants{};
    constants.Metallic = material.Metallic;
    constants.Roughness = material.Roughness;
    constants.Fresnel = material.Fresnel;
    constants.NormalBlend = material.NormalBlend;
    constants.NormalBias = material.NormalBias;
    constants.BaseSaturation = material.BaseSaturation;
    constants.BaseBrightness = material.BaseBrightness;
    constants.ShadowThreshold = material.ShadowThreshold;
    constants.ShadowSoftness = material.ShadowSoftness;
    constants.ShadowStrength = material.ShadowStrength;
    constants.MidStrength = material.MidStrength;
    constants.LitStrength = material.LitStrength;
    constants.RimStrength = material.RimStrength;
    constants.RimThreshold = material.RimThreshold;
    constants.RimSoftness = material.RimSoftness;
    constants.RimPower = material.RimPower;
    constants.RimColor = material.RimColor;
    constants.RimAlbedoBlend = material.RimAlbedoBlend;
    constants.RimLightBlend = material.RimLightBlend;
    constants.SpecularStrength = material.SpecularStrength;
    constants.SpecularThreshold = material.SpecularThreshold;
    constants.KawaiiBlend = material.KawaiiBlend;
    constants.SkinScatterStrength = material.SkinScatterStrength;
    constants.SkinScatterWrap = material.SkinScatterWrap;
    constants.SkinBacklightStrength = material.SkinBacklightStrength;
    constants.SkinRimScatterStrength = material.SkinRimScatterStrength;
    constants.SkinOilSpecularStrength = material.SkinOilSpecularStrength;
    constants.SkinShadowScatter = material.SkinShadowScatter;
    constants.CastShadowThreshold = material.CastShadowThreshold;
    constants.CastShadowSoftness = material.CastShadowSoftness;
    constants.Padding[1] = static_cast<float>(MaterialSystem::ResolveShadowMode(material));
    constants.Padding[2] = material.ShadowCutoff;
    constants.Transparent0 =
        XMFLOAT4(max(material.IOR, 1.0f), clamp(material.Transmission, 0.0f, 1.0f),
                 clamp(material.TransmissionRoughness, 0.0f, 1.0f), max(material.RefractionStrength, 0.0f));
    constants.Transparent1 =
        XMFLOAT4(max(material.Thickness, 0.0f), max(material.AbsorptionCoefficient.x, 0.0f),
                 max(material.AbsorptionCoefficient.y, 0.0f), max(material.AbsorptionCoefficient.z, 0.0f));
    for (int i = 0; i < kMaterialPartParamCount; ++i)
    {
        WriteMaterialPartConstants(constants, i, material.PartParams[i]);
    }
    constants.AssetBaseColor = material.BaseColor;
    constants.AssetSurface = {material.NormalScale, material.AOStrength, 0, material.AlphaCutoff};
    if (material.RuntimeHandle != 0)
    {
        constants.AssetSurface.z = 1.0f;
    }
    constants.AssetEmission = {material.EmissiveColor.x, material.EmissiveColor.y, material.EmissiveColor.z,
                               material.EmissiveIntensity};
    auto textureOffset = [](int descriptor) {
        if (descriptor < static_cast<int>(FrameConstants::g_kTEXTURE_SRV_START_INDEX))
        {
            return UINT32_MAX;
        }
        return static_cast<uint32_t>(descriptor) - FrameConstants::g_kTEXTURE_SRV_START_INDEX;
    };
    constants.AssetTextures = {
        textureOffset(material.MetallicRoughnessID), textureOffset(material.AOTextureID),
        textureOffset(material.EmissiveTextureID),
        static_cast<uint32_t>(material.AssetSurface) |
            (static_cast<uint32_t>(material.DoubleSided ? MaterialCull::None : material.AssetCull) << 2) |
            // Extended shading model (MaterialSurface.hlsli: EncodeShadingModel): bits 4-6 model, 8-15 strength.
            ((min(material.ShadingModel, 7u) & 7u) << 4) |
            (static_cast<uint32_t>(clamp(material.ShadingStrength, 0.0f, 1.0f) * 255.0f + 0.5f) << 8)};
    return constants;
}

const MaterialComponent& GetDeferredLightingMaterial()
{
    static MaterialComponent defaultMaterial{};
    EntityID fallbackEntity = g_kINVALID_ENTITY;
    EntityID litEntity = g_kINVALID_ENTITY;
    EntityID selfShadowEntity = g_kINVALID_ENTITY;
    EntityID toonEntity = g_kINVALID_ENTITY;

    for (EntityID entity : World::GetView<MaterialComponent>())
    {
        const auto& material = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
        if (material.ShaderClassMode != MaterialMode::Manual)
        {
            continue;
        }

        if (fallbackEntity == g_kINVALID_ENTITY)
        {
            fallbackEntity = entity;
        }
        if (material.ShaderClass == ShaderClass::Lit && litEntity == g_kINVALID_ENTITY)
        {
            litEntity = entity;
        }
        if (material.ShaderClass == ShaderClass::SelfShadow && selfShadowEntity == g_kINVALID_ENTITY)
        {
            selfShadowEntity = entity;
        }
        if (material.ShaderClass == ShaderClass::Toon && toonEntity == g_kINVALID_ENTITY)
        {
            toonEntity = entity;
        }
    }

    if (litEntity != g_kINVALID_ENTITY)
    {
        return ComponentManager::GetComponentUnchecked<MaterialComponent>(litEntity);
    }

    if (selfShadowEntity != g_kINVALID_ENTITY)
    {
        return ComponentManager::GetComponentUnchecked<MaterialComponent>(selfShadowEntity);
    }

    if (toonEntity != g_kINVALID_ENTITY)
    {
        return ComponentManager::GetComponentUnchecked<MaterialComponent>(toonEntity);
    }

    if (fallbackEntity != g_kINVALID_ENTITY)
    {
        return ComponentManager::GetComponentUnchecked<MaterialComponent>(fallbackEntity);
    }

    return defaultMaterial;
}

} // namespace

GPUMaterialData MaterialBindings::BuildGpuSceneMaterial(const MaterialComponent& material)
{
    GPUMaterialData result{};
    result.BaseColor = material.BaseColor;
    result.BaseColor.w *= material.Alpha;
    result.Surface = XMFLOAT4(material.Metallic, material.Roughness, material.Fresnel, material.NormalBlend);
    result.AlbedoTextureIndex = material.TextureID >= 0 ? static_cast<uint32_t>(material.TextureID) : UINT32_MAX;
    result.NormalTextureIndex = material.NormalMapID >= 0 ? static_cast<uint32_t>(material.NormalMapID) : UINT32_MAX;
    result.Flags = (material.UseTexture ? 1u : 0u) | (MaterialSystem::IsTransparentMaterial(material) ? 2u : 0u) |
                   (material.DoubleSided ? 4u : 0u);
    result.ShaderClass = static_cast<uint32_t>(material.ShaderClass);
    result.ShaderMode = static_cast<uint32_t>(material.ShaderClassMode);
    const auto constants = BuildPBRConstantsFromMaterial(material);
    static_assert(sizeof(constants) <= sizeof(result.ShadingParameters));
    result.ParameterVectorCount = (sizeof(constants) + 15) / 16;
    memcpy(result.ShadingParameters.data(), &constants, sizeof(constants));
    return result;
}

D3D12_GPU_VIRTUAL_ADDRESS MaterialBindings::GetPBRConstantBufferAddress(UINT slot)
{
    if (!m_PBRConstantBuffer)
    {
        return 0;
    }
    UINT safeSlot;
    if (slot < g_kPBR_CB_SLOT_COUNT)
    {
        safeSlot = slot;
    }
    else
    {
        safeSlot = 0;
    }
    const UINT frameSlot = GraphicsDevice::GetFrameIndex() * g_kPBR_CB_SLOT_COUNT + safeSlot;
    return m_PBRConstantBuffer->GetGPUVirtualAddress() + frameSlot * g_kPBR_CB_ALIGNED_SIZE;
}

void MaterialBindings::SetMaterial(const EntityID entityID, const MaterialComponent& material)
{
    if (!m_pPBRCbvDataBegin || !m_PBRConstantBuffer || !m_CommandList)
    {
        return;
    }

    UINT slot;
    if (entityID < g_kMAX_ENTITIES)
    {
        slot = entityID + 1;
    }
    else
    {
        slot = 0;
    }
    PBRConstants constants = BuildPBRConstantsFromMaterial(material);
    static_assert(sizeof(constants) <= g_kPBR_CB_ALIGNED_SIZE,
                  "Material constants must fit within one per-entity frame slot");
    static_assert(offsetof(PBRConstants, AssetBaseColor) == 146 * 16);
    const auto textureHeap = FrameConstants::GetCbvHeap();
    if (textureHeap)
    {
        m_CommandList->SetGraphicsRootDescriptorTable(
            18, CD3DX12_GPU_DESCRIPTOR_HANDLE(textureHeap->GetGPUDescriptorHandleForHeapStart(),
                                              FrameConstants::g_kTEXTURE_SRV_START_INDEX,
                                              FrameConstants::GetCbvIncrementSize()));
    }
    if (material.RuntimeHandle != 0)
    {
        auto& runtime = MaterialManager::GetRuntime(material.RuntimeHandle);
        if (!runtime.Constants)
        {
            const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
            const auto description = CD3DX12_RESOURCE_DESC::Buffer(g_kPBR_CB_ALIGNED_SIZE * g_kFRAME_COUNT);
            if (FAILED(m_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&runtime.Constants))) ||
                FAILED(runtime.Constants->Map(0, nullptr, reinterpret_cast<void**>(&runtime.MappedConstants))))
            {
                runtime.Error = "Failed to create material constant buffer";
                runtime.Constants.Reset();
                return;
            }
        }
        const auto frame = GraphicsDevice::GetFrameIndex();
        if (runtime.UploadedRevision[frame] != runtime.Revision)
        {
            memcpy(runtime.MappedConstants + frame * g_kPBR_CB_ALIGNED_SIZE, &constants, sizeof(constants));
            runtime.UploadedRevision[frame] = runtime.Revision;
        }
        m_CommandList->SetGraphicsRootConstantBufferView(3, runtime.Constants->GetGPUVirtualAddress() +
                                                                frame * g_kPBR_CB_ALIGNED_SIZE);
        return;
    }

    const UINT frameSlot = GraphicsDevice::GetFrameIndex() * g_kPBR_CB_SLOT_COUNT + slot;
    auto* dst = static_cast<UINT8*>(m_pPBRCbvDataBegin) + frameSlot * g_kPBR_CB_ALIGNED_SIZE;
    memcpy(dst, &constants, sizeof(constants));
    m_CommandList->SetGraphicsRootConstantBufferView(3, GetPBRConstantBufferAddress(slot));
}

bool MaterialBindings::SetPreviewMaterial(const MaterialComponent& material, ID3D12Resource* buffer, UINT textureBase,
                                          UINT partIndex, UINT partsPerFrame)
{
    if (!buffer)
    {
        return false;
    }
    auto constants = BuildPBRConstantsFromMaterial(material);
    constants.AssetSurface.z = 1.0f;
    auto offset = [textureBase](int descriptor) {
        return descriptor >= static_cast<int>(textureBase) ? static_cast<uint32_t>(descriptor) - textureBase
                                                           : UINT32_MAX;
    };
    constants.AssetTextures.x = offset(material.MetallicRoughnessID);
    constants.AssetTextures.y = offset(material.AOTextureID);
    constants.AssetTextures.z = offset(material.EmissiveTextureID);
    uint8_t* mapped = nullptr;
    if (FAILED(buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped))))
    {
        return false;
    }
    const auto frameOffset = (GraphicsDevice::GetFrameIndex() * partsPerFrame + partIndex) * g_kPBR_CB_ALIGNED_SIZE;
    memcpy(mapped + frameOffset, &constants, sizeof(constants));
    buffer->Unmap(0, nullptr);
    m_CommandList->SetGraphicsRootConstantBufferView(3, buffer->GetGPUVirtualAddress() + frameOffset);
    m_CommandList->SetGraphicsRootDescriptorTable(
        18, CD3DX12_GPU_DESCRIPTOR_HANDLE(FrameConstants::GetCbvHeap()->GetGPUDescriptorHandleForHeapStart(),
                                          textureBase, FrameConstants::GetCbvIncrementSize()));
    return true;
}

uint64_t MaterialBindings::GetMaterialBatchHash(const MaterialComponent& material)
{
    const PBRConstants constants = BuildPBRConstantsFromMaterial(material);
    uint64_t hash = HashGeometry(&constants, sizeof(constants));

    struct BatchMaterialState
    {
        int ShaderMode = 0;
        int ShaderClassValue = 0;
        float Alpha = 1.0f;
        UINT IsTransparent = 0;
        UINT UseTexture = 0;
        UINT UseNormalMap = 0;
    } state{};

    state.ShaderMode = static_cast<int>(material.ShaderClassMode);
    state.ShaderClassValue = static_cast<int>(material.ShaderClass);
    state.Alpha = material.Alpha;
    state.IsTransparent = static_cast<UINT>(material.IsTransparent);
    state.UseTexture = static_cast<UINT>(material.UseTexture);
    state.UseNormalMap = static_cast<UINT>(material.NormalMapID >= 0);
    const uint64_t stateHash = HashGeometry(&state, sizeof(state));

    hash ^= stateHash + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    return hash;
}

void MaterialBindings::BindAssetSlot(EntityID entity, const MaterialComponent& material, int modelTexture)
{
    ConstantBuffer3D constants;
    memcpy(&constants, FrameConstants::GetConstantBufferPtr() + entity * FrameConstants::g_kCB_ALIGNED_SIZE,
           sizeof(constants));
    constants.UseTexture = static_cast<int>(material.UseTexture);
    const bool useModelTexture = material.RuntimeHandle == 0 && modelTexture >= 0;
    if (useModelTexture)
    {
        constants.UseTexture = 1;
    }
    constants.UseNormalMap = static_cast<int>(material.NormalMapID >= 0);
    constants.MaterialMode = static_cast<int>(material.ShaderClassMode);
    constants.ShaderClass = static_cast<int>(material.ShaderClass);
    constants.MaterialMetallic = material.Metallic;
    constants.MaterialRoughness = material.Roughness;
    constants.MaterialFresnel = material.Fresnel;
    constants.MaterialAlpha = material.Alpha;
    constants.MaterialIsTransparent = static_cast<int>(material.IsTransparent);
    if (TransmittanceShadows::IsActive())
    {
        bool thinSheet = false;
        if (ComponentManager::HasComponent<SpriteComponent>(entity))
        {
            thinSheet = ComponentManager::GetComponentUnchecked<SpriteComponent>(entity).Is3D;
        }
        if (ComponentManager::HasComponent<MeshComponent>(entity))
        {
            const auto& mesh = ComponentManager::GetComponentUnchecked<MeshComponent>(entity);
            const auto& extents = mesh.LocalBoundsExtents;
            thinSheet |= mesh.HasLocalBounds && min(extents.x, min(extents.y, extents.z)) < 0.000001f;
        }
        if (thinSheet)
        {
            constants.MaterialIsTransparent |= 2;
        }
    }
    m_CommandList->SetGraphicsRootDescriptorTable(0, FrameConstants::AllocateTransientConstantBuffer(constants));
    SetMaterial(entity, material);
    int base = TextureManager::GetDefaultTextureIndex();
    int normal = base;
    if (material.TextureID >= 0)
    {
        base = material.TextureID;
    }
    if (useModelTexture)
    {
        base = modelTexture;
    }
    if (material.NormalMapID >= 0)
    {
        normal = material.NormalMapID;
    }
    const auto heap = FrameConstants::GetCbvHeap()->GetGPUDescriptorHandleForHeapStart();
    m_CommandList->SetGraphicsRootDescriptorTable(
        1, CD3DX12_GPU_DESCRIPTOR_HANDLE(heap, base, FrameConstants::GetCbvIncrementSize()));
    m_CommandList->SetGraphicsRootDescriptorTable(
        6, CD3DX12_GPU_DESCRIPTOR_HANDLE(heap, normal, FrameConstants::GetCbvIncrementSize()));
    for (int descriptor :
         {base, normal, material.MetallicRoughnessID, material.AOTextureID, material.EmissiveTextureID})
    {
        if (descriptor >= 0)
        {
            TextureManager::TouchTexture(descriptor);
        }
    }
}

void MaterialBindings::UpdateDeferredLightingMaterial()
{
    if (m_pPBRCbvDataBegin)
    {
        PBRConstants pbrConstants = BuildPBRConstantsFromMaterial(GetDeferredLightingMaterial());
        auto* pbrDst = static_cast<UINT8*>(m_pPBRCbvDataBegin) +
                       GraphicsDevice::GetFrameIndex() * g_kPBR_CB_SLOT_COUNT * g_kPBR_CB_ALIGNED_SIZE;
        memcpy(pbrDst, &pbrConstants, sizeof(pbrConstants));
    }
}
