#define SHADER_3D
#define SHADER_GPU_DRIVEN
#include "common.hlsl"
#include "GpuSceneCommon.hlsli"
StructuredBuffer<GPUInstanceData> Instances:register(t30);
ByteAddressBuffer Materials:register(t31);
cbuffer IndirectInstance:register(b4) { uint InstanceID; }
struct VelocityOutput { float4 Position:SV_POSITION; float4 CurrentClip:TEXCOORD0; float4 PreviousClip:TEXCOORD1; nointerpolation uint MaterialFlags:TEXCOORD2; };
VelocityOutput main(VSInput3D input)
{
    GPUInstanceData instance=Instances[InstanceID];
    VelocityOutput output;
    output.CurrentClip=mul(mul(mul(float4(input.Position,1),instance.World),View),Projection);
    output.Position=output.CurrentClip;
    output.MaterialFlags=Materials.Load(instance.MaterialIndex*GPU_SCENE_MATERIAL_STRIDE+40);
    output.PreviousClip=mul(mul(float4(input.Position,1),instance.PreviousWorld),PreviousViewProjection);
    return output;
}
