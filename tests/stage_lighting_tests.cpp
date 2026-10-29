#include "toon_hlsl_math_shim.h"
#include "../shader/hlsl/StageLighting.hlsli"
#include <cassert>
#include <cstdio>
int main()
{
    assert(std::abs(StageRangeAttenuation(2,8,0)-0.5625f)<1e-6f);
    assert(StageRangeAttenuation(2,8,-1)==1);
    assert(StageRangeAttenuation(2,80,-1)==1);
    assert(StageRangeAttenuation(8,8,-1)==0);
    assert(StageRangeAttenuation(9,8,-1)==0);
    assert(std::abs(StageRangeAttenuation(7.5f,8,-1)-0.5f)<1e-6f);
    assert(StageRangeAttenuation(0.01f,0,-1)==0);
    assert(std::abs(StageVolumeDensity(-1.35f)-0.35f)<1e-6f);
    assert(StageVolumeDensity(0.35f)==0.35f);
    assert(StageCapsuleAxial(10,4)==2);
    assert(StageCapsuleAxial(-10,4)==-2);
    assert(StageCapsuleAxial(1,4)==1);
    assert(StageCapsuleAxial(10,0)==0);
    for (int i = 0; i <= 100; ++i)
    {
        float d = i * 0.1f;
        float a = StageRangeAttenuation(d, 8, -1);
        assert(std::isfinite(a) && a >= 0 && a <= 1);
        if (i > 0) assert(a <= StageRangeAttenuation(d - 0.1f, 8, -1) + 1e-6f);
    }
    std::puts("Stage lighting math: PASS");
}
