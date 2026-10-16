#define SHADER_3D
#define SHADER_GPU_DRIVEN
#include "common.hlsl"
#include "GpuSceneCommon.hlsli"
StructuredBuffer<GPUInstanceData> gInstances : register(t30);
ByteAddressBuffer gMaterials : register(t31);
void main(PSInput3D input)
{
    GPUInstanceData instance = gInstances[input.InstanceID];
    uint flags = gMaterials.Load(instance.MaterialIndex * GPU_SCENE_MATERIAL_STRIDE + 40u);
    // Textured meshes may contain alpha-tested coverage. Dynamic objects cannot be temporal occluders.
    if ((flags & 1u) != 0 || (instance.Flags & 8u) != 0) discard;
}

