#ifndef GPU_SCENE_COMMON_INCLUDED
#define GPU_SCENE_COMMON_INCLUDED
// Byte strides and offsets are asserted in gpuscenetypes.h. All matrices use
// DirectXMath's row-vector convention, including StructuredBuffer members.
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
    float4 ShadingParameters[160];
};
// SM 5.1 limits a StructuredBuffer element to 2048 bytes. The existing
// material has 15 part overrides, so its 2624-byte cache uses raw SRV access.
#define GPU_SCENE_MATERIAL_STRIDE 2624u
float4 LoadSceneMaterialVector(ByteAddressBuffer materials, uint materialId, uint vectorIndex)
{
    return asfloat(materials.Load4(materialId * GPU_SCENE_MATERIAL_STRIDE + 64u + vectorIndex * 16u));
}
#endif

