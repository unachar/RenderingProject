#ifndef GPU_SCENE_COMMON_INCLUDED
#define GPU_SCENE_COMMON_INCLUDED
#define GPU_SCENE_MATERIAL_PARAMETER_VECTORS 192u

// バイト単位のストライドとオフセットは gpuscenetypes.h で検証する。全行列は
// StructuredBuffer のメンバーも含め、DirectXMath の行ベクトル規約を使う。
struct GPUInstanceData
{
    row_major float4x4 World;
    row_major float4x4 PreviousWorld;
    uint MeshIndex;
    uint MaterialIndex;
    uint Flags;
    uint Entity;
    float4 BoundsCenter;
    float4 BoundsExtents;
    float4 LocalCenter;
    float4 LocalExtents;
    float4 LodDistances;
    uint LODGroupIndex;
    uint VirtualGeometryAssetIndex;
    uint2 VisibilityPadding;
};

struct GPUMeshData
{
    uint VertexAddressLow;
    uint VertexAddressHigh;
    uint IndexAddressLow;
    uint IndexAddressHigh;
    uint VertexCount;
    uint IndexCount;
    uint StartIndex;
    int BaseVertex;
    float4 BoundsCenter;
    float4 BoundsExtents;
};

struct GPUMaterialData
{
    float4 BaseColor;
    float4 Surface;
    uint AlbedoTextureIndex;
    uint NormalTextureIndex;
    uint Flags;
    uint ShaderClass;
    uint ParameterVectorCount;
    uint ShaderMode;
    uint2 Padding;
    float4 ShadingParameters[GPU_SCENE_MATERIAL_PARAMETER_VECTORS];
};

// SM 5.1 の StructuredBuffer 要素は 2048 バイトまで。既存のマテリアルには
// 15 個のパーツ上書きがあり、3136 バイトのキャッシュを生の SRV アクセスで扱う。
#define GPU_SCENE_MATERIAL_STRIDE 3136u

// インポスター置き換え（Flags ビット5 = アトラス準備済み。CPU は距離判定を行わない）。
//   LodDistances.w      : 切替距離
//   BoundsExtents.w / LocalExtents.w / asfloat(VisibilityPadding.x) : モデルのローカル AABB 中心
// 全セクションが同じ中心・距離を持つので、エンティティ内で判定が揃う。
#define GPU_INSTANCE_IMPOSTOR_READY 32u
float3 GpuInstanceImpostorCenter(GPUInstanceData instance)
{
    float3 local = float3(instance.BoundsExtents.w, instance.LocalExtents.w, asfloat(instance.VisibilityPadding.x));
    return mul(float4(local, 1.0f), instance.World).xyz;
}

float4 LoadSceneMaterialVector(ByteAddressBuffer materials, uint materialId, uint vectorIndex)
{
    return asfloat(materials.Load4(materialId * GPU_SCENE_MATERIAL_STRIDE + 64u + vectorIndex * 16u));
}
#endif

