// FidelityFX FSR 1.0 EASU integration. The filter implementation is provided
// by AMD's official ffx_fsr1.h and distributed under its MIT license.
cbuffer FsrConstants : register(b0)
{
    uint4 Const0;
    uint4 Const1;
    uint4 Const2;
    uint4 Const3;
    uint4 DispatchInfo; // xy = output extent
};

#define A_GPU 1
#define A_HLSL 1
#include "../../External/FidelityFX-FSR/ffx_a.h"

Texture2D<float4> InputTexture : register(t0);
RWTexture2D<float4> OutputTexture : register(u0);
SamplerState LinearClampSampler : register(s0);

#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return InputTexture.GatherRed(LinearClampSampler, p, int2(0, 0)); }
AF4 FsrEasuGF(AF2 p) { return InputTexture.GatherGreen(LinearClampSampler, p, int2(0, 0)); }
AF4 FsrEasuBF(AF2 p) { return InputTexture.GatherBlue(LinearClampSampler, p, int2(0, 0)); }

#include "../../External/FidelityFX-FSR/ffx_fsr1.h"

void RunEasu(AU2 position)
{
    if (all(position < DispatchInfo.xy))
    {
        AF3 color;
        FsrEasuF(color, position, Const0, Const1, Const2, Const3);
        OutputTexture[position] = float4(color, 1.0f);
    }
}

[numthreads(64, 1, 1)]
void main(uint3 localThreadId : SV_GroupThreadID, uint3 groupId : SV_GroupID)
{
    AU2 position = ARmp8x8(localThreadId.x) + AU2(groupId.x << 4u, groupId.y << 4u);
    RunEasu(position);
    position.x += 8u;
    RunEasu(position);
    position.y += 8u;
    RunEasu(position);
    position.x -= 8u;
    RunEasu(position);
}
