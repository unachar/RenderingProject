

cbuffer FsrConstants : register(b0)
{
    uint4 Const0;
    uint4 Const1;
    uint4 Const2;
    uint4 Const3;
    uint4 DispatchInfo;
};

#define A_GPU 1
#define A_HLSL 1
#include "../../External/FidelityFX-FSR/ffx_a.h"

Texture2D<float4> InputTexture : register(t0);
RWTexture2D<float4> OutputTexture : register(u0);

#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) { return InputTexture.Load(int3(ASU2(p), 0)); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}

#include "../../External/FidelityFX-FSR/ffx_fsr1.h"

void RunRcas(AU2 position)
{
    if (all(position < DispatchInfo.xy))
    {
        AF3 color;
        FsrRcasF(color.r, color.g, color.b, position, Const0);
        OutputTexture[position] = float4(color, 1.0f);
    }
}

[numthreads(64, 1, 1)]
void main(uint3 localThreadId : SV_GroupThreadID, uint3 groupId : SV_GroupID)
{
    AU2 position = ARmp8x8(localThreadId.x) + AU2(groupId.x << 4u, groupId.y << 4u);
    RunRcas(position);
    position.x += 8u;
    RunRcas(position);
    position.y += 8u;
    RunRcas(position);
    position.x -= 8u;
    RunRcas(position);
}
