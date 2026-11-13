#ifndef GEOMETRIC_SPECULAR_AA
#define GEOMETRIC_SPECULAR_AA 0
#endif
#define SHADER_3D
#ifndef SHADER_VISIBILITY_RESOLVE
#define POM_PIXEL_DEPTH_OUTPUT
#endif
#include "common.hlsl"

#ifdef SHADER_GPU_DRIVEN
#include "GPUDrivenOpaque.hlsli"
#else
Texture2D g_Texture : register(t0);
Texture2D g_NormalTexture : register(t2);
#endif
SamplerState g_SamplerState : register(s0);

#include "MaterialSurface.hlsli"
#include "NormalMapDecode.hlsli"
#include "ToonLighting.hlsli"
#include "LiveToonMaterial.hlsli"
#if GEOMETRIC_SPECULAR_AA
#include "GeometricSpecularAA.hlsli"
#endif

GBufferOutput ShadeGBuffer(PSInput3D In, bool frontFace)
{
#if GEOMETRIC_SPECULAR_AA
    // Evaluate derivatives before any alpha/backface discard or material-dependent branch.
    // Geometry normals deliberately exclude normal-map and height-map frequencies.
#ifdef SHADER_VISIBILITY_RESOLVE
    float3 geometryNormalDx = VisibilityNormalDx;
    float3 geometryNormalDy = VisibilityNormalDy;
#else
    float3 geometryNormal = GeometricSpecularAA::GeometryNormal(In.Normal.xyz);
    float3 geometryNormalDx = ddx(geometryNormal);
    float3 geometryNormalDy = ddy(geometryNormal);
#endif
    float geometryGradientSquared = GeometricSpecularAA::FiniteGradientSquared(
        dot(geometryNormalDx, geometryNormalDx), dot(geometryNormalDy, geometryNormalDy));
#endif
#ifdef SHADER_GPU_DRIVEN
    uint materialID = gSceneInstances[In.InstanceID].MaterialIndex;
    uint address = materialID * GPU_SCENE_MATERIAL_STRIDE;
    uint4 metadata = gSceneMaterials.Load4(address + 32u);
    uint normalTextureIndex = metadata.y;
#ifndef SHADER_VISIBILITY_RESOLVE
    if ((metadata.z & 4u) == 0 && !frontFace)
    {
        discard;
    }
#endif
    int UseTexture = (metadata.z & 1u) != 0;
    int FlipNormal = 0;
    int ShaderClass = metadata.w;
    int MaterialMode = gSceneMaterials.Load(address + 52u);
    float4 surface = asfloat(gSceneMaterials.Load4(address + 16u));
    float mMetallic = surface.x;
    float mRoughness = surface.y;
    float mFresnel = surface.z;
    float ShadowThreshold = LoadSceneMaterialVector(gSceneMaterials, materialID, 1u).w;
    float ShadowSoftness = LoadSceneMaterialVector(gSceneMaterials, materialID, 2u).x;
    float ShadowStrength = LoadSceneMaterialVector(gSceneMaterials, materialID, 2u).y;
#endif
#ifdef SHADER_GPU_DRIVEN
    AssetMaterialParameters assetMaterial = LoadAssetMaterial(materialID);
#else
    AssetMaterialParameters assetMaterial = LoadAssetMaterial(0);
#endif
    AssetParallaxResult pom =
        ApplyAssetParallaxEx(assetMaterial, In.TexCoord, In.WorldPos, In.Normal, In.CameraPos, g_SamplerState);
    float2 materialUv = pom.Uv;
    float materialPartId = (MaterialMode == 1) ? (float)ShaderClass : max(In.Diffuse.a, 0.0f);
    // SNI ヘッドマスク: materialPartId + 100.0f でエンコードされている
    float headMask = step(15.0f, materialPartId);
    materialPartId = materialPartId - headMask * 100.0f;
    float4 baseColor = float4(In.Diffuse.rgb, 1.0f);
    if (assetMaterial.Surface.z > 0.5f)
    {
        baseColor = float4(1, 1, 1, 1);
    }
    if (UseTexture != 0)
    {
#ifdef SHADER_GPU_DRIVEN
#ifdef SHADER_VISIBILITY_RESOLVE
        baseColor *= SampleMaterialGradCommon(LoadMaterialTexture(SceneTextureIndex(metadata.x)), g_SamplerState, materialUv, VisibilityUvDx, VisibilityUvDy);
#else
        baseColor *= SampleMaterialBiasCommon(LoadMaterialTexture(SceneTextureIndex(metadata.x)), g_SamplerState, materialUv);
#endif
#else
        baseColor *= SampleMaterialBiasCommon(g_Texture, g_SamplerState, materialUv);
#endif
    }

    float assetMetallic = mMetallic;
    float assetRoughness = mRoughness;
    float4 assetEmissionAO;
    ApplyAssetMaterial(assetMaterial, materialUv, g_SamplerState, baseColor, assetMetallic, assetRoughness,
                       assetEmissionAO);
    // マテリアルグラフの描画時評価（3D 位置で評価。GPU 駆動 / Visibility 解決でも同じ）
    float3 graphGeometryNormal = In.Normal.xyz * ((FlipNormal != 0) ? -1.0f : 1.0f);
    MaterialGraphVmOutput materialGraph = (MaterialGraphVmOutput)0;
    [branch] if (MaterialGraphProgramSlot(assetMaterial) != 0u)
    {
        MaterialGraphVmInput graphInput;
        graphInput.WorldPos = In.WorldPos;
        graphInput.PixelPos = In.Position.xy;
#ifdef SHADER_GPU_DRIVEN
        GPUInstanceData graphInstance = gSceneInstances[In.InstanceID];
        MaterialGraphSetObject(graphInput, graphInstance.World, graphInstance.LocalCenter.xyz,
                               graphInstance.LocalExtents.xyz, graphInstance.Entity, graphInstance.Entity != 0xFFFFFFFFu);
#else
        MaterialGraphSetObject(graphInput, World, ObjectBoundsCenter.xyz,
                               ObjectBoundsExtents.w > 0.5f ? ObjectBoundsExtents.xyz : float3(0, 0, 0),
                               (uint)ObjectBoundsCenter.w, ObjectBoundsExtents.w > 0.5f || ObjectBoundsCenter.w > 0.5f);
#endif
        graphInput.Normal = graphGeometryNormal;
        graphInput.CameraPos = In.CameraPos;
        graphInput.UV = materialUv;
        graphInput.VertexColor = In.Diffuse;
        materialGraph = EvaluateMaterialGraphVm(MaterialGraphProgramSlot(assetMaterial), graphInput, g_SamplerState);
        ApplyMaterialGraphSurface(materialGraph, assetMaterial, baseColor, assetMetallic, assetRoughness,
                                  assetEmissionAO);
    }
#ifndef SHADER_VISIBILITY_RESOLVE
    clip(baseColor.a - 0.01f);
#endif

    float3 normal = In.Normal.xyz * ((FlipNormal != 0) ? -1.0f : 1.0f);

#ifdef SHADER_GPU_DRIVEN
    bool hasNormalMap = normalTextureIndex != 0xffffffffu && normalTextureIndex >= TextureBase &&
                        normalTextureIndex - TextureBase < 512u;

    // シーン既定のテクスチャは平坦な法線マップではないため、法線としてサンプリングしない。
    [branch] if (hasNormalMap)
    {
        uint textureIndex = SceneTextureIndex(normalTextureIndex);
#ifdef SHADER_VISIBILITY_RESOLVE
        float3 tangentNormal = DecodeTangentNormalSample(
            SampleMaterialGradCommon(LoadMaterialTexture(textureIndex), g_SamplerState, materialUv, VisibilityUvDx, VisibilityUvDy));
#else
        float3 tangentNormal = DecodeTangentNormalSample(
            SampleMaterialBiasCommon(LoadMaterialTexture(textureIndex), g_SamplerState, materialUv));
#endif
        normal = ApplyNormalMapCommon(normal, In.WorldPos, In.TexCoord, ScaleAssetNormal(tangentNormal, assetMaterial));
    }

#else
    if (UseNormalMap != 0)
    {
        float3 tangentNormal = DecodeTangentNormalSample(SampleMaterialBiasCommon(g_NormalTexture, g_SamplerState, materialUv));
        normal = ApplyNormalMapCommon(normal, In.WorldPos, In.TexCoord, ScaleAssetNormal(tangentNormal, assetMaterial));
    }
#endif
    // 法線マップが無いときは POM の高さマップから法線を作る（UV シフトだけでは見た目が変わらないため）。
#ifdef SHADER_GPU_DRIVEN
    if (!hasNormalMap)
#else
    if (UseNormalMap == 0)
#endif
    {
        normal = ApplyAssetHeightNormal(assetMaterial, normal, In.WorldPos, In.TexCoord, materialUv, g_SamplerState);
    }
    normal = SafeNormalizeCommon(normal, float3(0.0f, 1.0f, 0.0f));
    normal = ApplyMaterialGraphNormal(materialGraph, assetMaterial, normal, graphGeometryNormal, In.WorldPos, In.TexCoord);

    // 球面法線補間 (SNI): ヘッドマスク (materialPartId + 100 でエンコード) を使用
    float3 sniNormal = SafeNormalizeCommon(In.SniVector, normal);
    normal = lerp(normal, sniNormal, headMask);

    GBufferOutput output;
    output.EmissionAO = assetEmissionAO;
    output.Color = baseColor;
    output.Normal = MakeGBufferNormal(normal);
    output.Normal.a = -AssetParallaxRayDistance(pom);

    // ピクセル深度オフセット：GBuffer 深度（ライティング・影・SSAO・反射レイの始点）を凹凸の奥へ。
    output.Depth = AssetParallaxPixelDepth(pom, In.WorldPos, In.CameraPos, saturate(In.Position.z));

#ifdef SHADER_GPU_DRIVEN
    MaterialPartShaderParams partParams = LoadGpuDrivenPart(materialID, materialPartId);
#else
    MaterialPartShaderParams partParams = ResolveMaterialPartParams(materialPartId);
#endif
    bool usePartParams = MaterialMode != 1;
    output.Material.r = usePartParams ? partParams.Basic.x : assetMetallic;
    output.Material.g = usePartParams ? partParams.Basic.y : assetRoughness;
    output.Material.b = usePartParams ? partParams.Basic.z : mFresnel;
    output.Material.a = materialPartId;
#if GEOMETRIC_SPECULAR_AA
    output.Material.g = GeometricSpecularAA::FilterMaterialRoughness(
        output.Material.g, geometryGradientSquared, materialPartId);
#endif
    output.Shadow.r = usePartParams ? partParams.Shadow0.x : ShadowThreshold;
    output.Shadow.g = usePartParams ? partParams.Shadow0.y : ShadowSoftness;
    output.Shadow.b = usePartParams ? partParams.Shadow0.z : ShadowStrength;
    output.Shadow.a = EncodeShadingModel(assetMaterial, 0.0f);
    // ライブトゥーンのマテリアルテクスチャ（Def / 顔陰影反転 / 質感ランプ）
    {
#ifdef SHADER_GPU_DRIVEN
        uint4 liveToonTextures = asuint(LoadSceneMaterialVector(gSceneMaterials, materialID, 190u));
        float4 liveToonParams = LoadSceneMaterialVector(gSceneMaterials, materialID, 191u);
#else
        uint4 liveToonTextures = LiveToonTextures;
        float4 liveToonParams = LiveToonMaterialParams;
#endif
        LiveToonMaterialSurface toonSurface;
        toonSurface.Albedo = output.Color.rgb;
        toonSurface.ShadowThreshold = output.Shadow.r;
        toonSurface.ShadowSoftness = output.Shadow.g;
        toonSurface.Roughness = output.Material.g;
        ApplyLiveToonMaterialTextures(liveToonTextures, liveToonParams, materialUv, normal,
                                      SurfaceViewDirectionCommon(In.WorldPos, In.CameraPos), materialPartId,
                                      g_SamplerState, toonSurface);
        output.Color.rgb = toonSurface.Albedo;
        output.Shadow.r = toonSurface.ShadowThreshold;
        output.Shadow.g = toonSurface.ShadowSoftness;
        output.Material.g = toonSurface.Roughness;
    }
    // POM セルフシャドウ（主光源）。
    WriteAssetParallaxSelfShadow(pom, In.WorldPos, g_SamplerState, output.Shadow, output.Material.a);
    output.Color.a = 1.0f;
#ifdef SHADER_GPU_DRIVEN
    if (VisibilityDebugMode != 0)
    {
        uint4 visibility = gVisibilityDebug[In.InstanceID];
        const float3 palette[8] = {float3(0.1, 1, 0.2), float3(0.1, 0.5, 1),  float3(1, 0.8, 0.1),
                                   float3(1, 0.3, 0.1), float3(0.7, 0.2, 1),  float3(0.2, 1, 1),
                                   float3(1, 0.2, 0.7), float3(0.8, 0.8, 0.8)};
        output.Color.rgb = VisibilityDebugMode == 2 ? palette[min(visibility.z, 7u)] : float3(0.1, 1, 0.2);
    }
#endif
    return output;
}

#ifdef SHADER_VISIBILITY_RESOLVE
GBufferOutput main(PSInput3D In, bool frontFace : SV_IsFrontFace)
{
    return ShadeGBuffer(In, frontFace);
}
#else
GBufferDepthOutput main(PSInput3D In, bool frontFace : SV_IsFrontFace)
{
    GBufferOutput shaded = ShadeGBuffer(In, frontFace);
    GBufferDepthOutput output;
    output.Color = shaded.Color;
    output.Normal = shaded.Normal;
    output.Depth = shaded.Depth;
    output.Material = shaded.Material;
    output.Shadow = shaded.Shadow;
    output.EmissionAO = shaded.EmissionAO;
    output.HardwareDepth = min(shaded.Depth.x, In.Position.z);
    return output;
}
#endif



