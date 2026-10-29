#include "toon_hlsl_math_shim.h"
#if __has_include("../shader/hlsl/ToonLighting.hlsli")
#include "../shader/hlsl/ToonLighting.hlsli"
#else
#error Live toon lighting helper has not been implemented
#endif
#include <cassert>
#include <cstdio>

static float3 Shade(float3 albedo, float3 radiance, float type=2, float3 light={0,0,1}, float3 normal={0,0,1})
{
    return EvaluateLiveToonLight(albedo,normal,{0,0,1},light,radiance,type,0.5f,0.44f,0.045f,1.0f);
}
static void Near(float3 a, float3 b)
{
    assert(std::abs(a.x-b.x)<1e-5f && std::abs(a.y-b.y)<1e-5f && std::abs(a.z-b.z)<1e-5f);
}
int main()
{
    for (int i=0;i<=14;++i) assert(IsLiveToonClass(float(i)) == (i>=1 && i<=4));
    const float3 black{0,0,0}, white{1,1,1};
    Near(Shade(white,black),black); // A disabled or fully shadowed light emits no direct term.
    Near(Shade(black,white),black); // No invented material color.
    const auto full=Shade({0.8f,0.4f,0.2f},white);
    Near(Shade({0.4f,0.2f,0.1f},white),full*0.5f); // Albedo must not be squared.
    Near(Shade({0.8f,0.4f,0.2f},{0.5f,0.5f,0.5f}),full*0.5f);
    const auto red=Shade(white,{1,0,0},2,{1,0,0});
    const auto blue=Shade(white,{0,0,1},2,{-1,0,0});
    assert(red.x>0 && blue.z>0 && (red+blue).x>0 && (red+blue).z>0);
    assert(red.z==0 && blue.x==0); // Opposing stage lights keep their distinct tint.
    const auto skin=Shade(white,{0,0,1},3);
    const auto cloth=Shade(white,{0,0,1},2);
    assert(skin.x>cloth.x && skin.y>cloth.y && skin.z>skin.x);
    const auto skinBack=Shade(white,white,3,{0,0,-1});
    const auto clothBack=Shade(white,white,2,{0,0,-1});
    assert(skinBack.x>clothBack.x);
    for(int i=1;i<=4;++i)
    {
        auto c=Shade({0.3f,0.5f,0.7f},white,float(i),{0,0,-1});
        assert(std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z));
        assert(c.x>=0 && c.y>=0 && c.z>=0);
    }
    assert(LiveToonBand(0.2f,0.44f,0)==0);
    assert(LiveToonBand(0.8f,0.44f,0)==1);
    Near(EvaluateLiveToonAmbient(black,white,3),black);
    const auto ambient=EvaluateLiveToonAmbient(white,black,3);
    assert(ambient.x>0 && ambient.x<0.35f);
    // Toon uses all RGB shadow channels for authored bands. POM occlusion must
    // survive the UNORM GBuffer without replacing threshold or shadow strength.
    const float4 authored{0.44f,0.045f,0.8f,0.0f};
    for (float visibility : {0.0f,0.4f,1.0f})
    {
        const auto packed=PackLiveToonPomShadow(authored,visibility);
        assert(packed.x==authored.x && packed.y==authored.y && packed.z==authored.z);
        assert(packed.w>=0 && packed.w<=1);
        const float alpha8=std::round(packed.w*255.0f)/255.0f;
        assert(std::abs(LiveToonPomVisibility(alpha8)-visibility)<1.0f/255.0f);
    }
    assert(LiveToonPomVisibility(0)==1); // Default non-POM / primitive writer.
    std::puts("Live toon lighting: PASS (classification, albedo, lights, skin, bands)");
}
