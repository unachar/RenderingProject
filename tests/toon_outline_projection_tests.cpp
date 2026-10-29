#include "toon_hlsl_math_shim.h"
#if __has_include("../shader/hlsl/ToonOutlineProjection.hlsli")
#include "../shader/hlsl/ToonOutlineProjection.hlsli"
#include "../shader/hlsl/SkinningNormal.hlsli"
#else
#error Pixel-stable outline projection has not been implemented
#endif
#include <cassert>
#include <cstdio>
int main()
{
    for (float2 extent : {float2(1920,1080),float2(1080,1920),float2(4096,512)})
        for (float4 direction : {float4(1,0,0,0),float4(0,1,0,0),float4(1,1,0,0),float4(1,-1,0,0)})
            for (float w : {0.1f,1.0f,100.0f})
            {
                const auto offset=ToonOutlineClipOffset({0,0,0,w},direction,extent,3);
                const float x=offset.x/w*extent.x*0.5f, y=offset.y/w*extent.y*0.5f;
                assert(std::abs(std::sqrt(x*x+y*y)-3)<1e-4f);
            }
    const float4 origin{0.5f,0.1f,0.2f,1};
    for (float width : {0.0f,-1.0f})
    {
        const auto p=ToonOutlineClipOffset(origin,{1,1,0,0},{1920,1080},width);
        assert(p.x==0 && p.y==0);
    }
    const auto anchor=ToonOutlineClipOffset(origin,{0,0,0,0},{1920,1080},3);
    assert(anchor.x==0 && anchor.y==0); // TEO base normal must not move.
    const auto skinnedAnchor = NormalizeSkinnedNormal({0,0,0}, {0,0,0});
    assert(skinnedAnchor.x==0 && skinnedAnchor.y==0 && skinnedAnchor.z==0);
    const auto rotated = NormalizeSkinnedNormal({1,0,0}, {0,2,0});
    assert(rotated.x==0 && rotated.y==1 && rotated.z==0);
    const auto degenerate = NormalizeSkinnedNormal({1,0,0}, {0,0,0});
    assert(degenerate.x==0 && degenerate.y==1 && degenerate.z==0);
    for (float w : {-1.0f,0.0f,1e-8f})
    {
        const auto p=ToonOutlineClipOffset({1,1,0,w},{1,1,0,0},{1920,1080},3);
        assert(p.x==0 && p.y==0);
    }
    // A projected normal parallel to the position is a zero screen direction.
    const auto parallel=ToonOutlineClipOffset({1,2,0,2},{0.5f,1,0,1},{1920,1080},3);
    assert(parallel.x==0 && parallel.y==0);
    std::puts("Toon outline projection: PASS (aspect, depth, zero width, TEO anchors)");
}
