#define SHADER_3D
#define SHADER_GPU_DRIVEN
#include "common.hlsl"
#include "GPUDrivenOpaque.hlsli"
struct Output { float4 Position:SV_POSITION; float4 CurrentClip:TEXCOORD0; float4 PreviousClip:TEXCOORD1; };
Output main(VSInput3D input)
{
    GPUInstanceData instance=gSceneInstances[GPUInstanceID]; Output output;
    output.CurrentClip=mul(mul(mul(float4(input.Position,1),instance.World),View),Projection);
    output.PreviousClip=mul(mul(float4(input.Position,1),instance.PreviousWorld),PreviousViewProjection); output.Position=output.CurrentClip; return output;
}
