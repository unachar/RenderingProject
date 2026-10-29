"""Execute the production common-light resolver with a minimal C++ HLSL math shim."""
from pathlib import Path
import re
import subprocess
import tempfile
import os
ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'shader/hlsl/common.hlsl').read_text(encoding='utf-8-sig')
start = source.index('void ResolveSingleLightCommon(', source.index('void ResolveSingleLightCommon(') + 1)
# Skip declarations; the definition has a body immediately after its argument list.
for match in re.finditer(r'void ResolveSingleLightCommon\([^;]+?\)\s*\{', source):
    start = match.start()
    break
end = source.index('\nvoid ResolveLightCommon(', start)
body = source[start:end]
body = re.sub(r'out (float3|float) (\w+)', r'\1& \2', body)
body = re.sub(r'(\w+)\.xyz', r'xyz(\1)', body)
component_source = (ROOT / 'source/ecs/componentmanager.h').read_text(encoding='utf-8-sig')
component = component_source[component_source.index('struct LightComponent\n'):component_source.index('struct SunComponent')]
light_header = (ROOT / 'source/scene/light.h').read_text(encoding='utf-8-sig')
enums = light_header[light_header.index('enum class LightType'):light_header.index('class Light\n')]
program = r'''
#include "tests/toon_hlsl_math_shim.h"
#include "shader/hlsl/StageLighting.hlsli"
#include <cassert>
#include <cstdio>
#include <string>
using std::string;
using XMFLOAT3 = float3;
using XMFLOAT4 = float4;
constexpr float XM_PI = 3.14159265358979323846f;
COMPONENT
inline float3 xyz(float4 a) { return {a.x,a.y,a.z}; }
inline float3 operator-(float3 a) { return {-a.x,-a.y,-a.z}; }
inline float length(float3 a) { return sqrt(dot(a,a)); }
inline float step(float a,float b) { return b>=a ? 1.0f : 0.0f; }
using std::exp;
using std::round;
inline float3 SafeNormalizeCommon(float3 v,float3 fallback)
{ float l=length(v); return l>1e-6f ? v/l : fallback; }
RESOLVER
float Shade(float3 p, int type, float range, float4 extra)
{
    float3 dir{0,0,0}; float atten=0,volume=0;
    ResolveSingleLightCommon(p,{0,1,0,range},{0,0,0,float(type)},extra,dir,atten,volume);
    assert(std::isfinite(atten) && std::isfinite(dir.x));
    return atten;
}
int main()
{
    LightComponent light;
    assert(light.PhysicallyBased); // Old scene defaults retain legacy mode.
    light.VolumeDensity=0.35f;
    assert(PackLightExtra(light).z==0.35f);
    light.PhysicallyBased=false;
    assert(std::abs(StageVolumeDensity(PackLightExtra(light).z)-0.35f)<1e-6f);
    light.Type=LightType::Capsule;
    light.Range=2; light.CapsuleLength=20; light.CapsuleRadius=0.1f;
    auto packed=PackLightExtra(light);
    assert(packed.x==20 && packed.y==0.1f && packed.z<0);
    assert(std::abs(LightInfluenceRadius(light)-12.1f)<1e-5f);
    assert(Shade({0,2,0},1,8,{0,0,-1,0})==1);
    assert(Shade({0,2,0},1,80,{0,0,-1,0})==1);
    assert(std::abs(Shade({0,2,0},1,8,{0,0,0,0})-0.5625f)<1e-6f);
    // Nearest point is on the segment, not its centre: endpoint remains fully lit.
    assert(Shade({0,10,0},5,2,{20,0.1f,-1,0})==1);
    assert(Shade({0,12.2f,0},5,2,{20,0.1f,-1,0})==0);
    assert(Shade({0,0,0},5,2,{20,0.1f,-1,0})==1);
    // A zero-length capsule exactly matches a point when radius is zero.
    assert(Shade({0,1.5f,0},5,2,{0,0,-1,0})==Shade({0,1.5f,0},1,2,{0,0,-1,0}));
    // Rect stage intensity does not scale with area, front/back mask is retained.
    assert(Shade({0,2,0},4,8,{1,1,-1,0})==Shade({0,2,0},4,8,{10,10,-1,0}));
    assert(Shade({0,-2,0},4,8,{1,1,-1,0})==0);
    // Spot keeps its cone mask in both attenuation modes.
    assert(Shade({0,2,0},2,8,{0.95f,0.8f,-1,0})==1);
    assert(Shade({2,0,0},2,8,{0.95f,0.8f,-1,0})==0);
    std::puts("Stage common resolver: PASS (point, capsule endpoints, degenerate segment, rect, spot)");
}
'''.replace('COMPONENT', enums + component).replace('RESOLVER', body)
with tempfile.TemporaryDirectory() as work:
    cpp = Path(work)/'resolve.cpp'
    cpp.write_text(program)
    binary=Path(work)/'resolve'
    subprocess.run([os.environ.get('CXX','g++'),'-std=c++17','-Wall','-Wextra','-Werror',
                    '-Wno-unused-variable','-I',str(ROOT),str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
