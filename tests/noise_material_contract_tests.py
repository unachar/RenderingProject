"""Execute the production shader gates against the actual CPU material constant layout.

Extracts their small C++-compatible expressions instead of restating the conditions.
No graphics mock or copied noise implementation is used.
"""
from pathlib import Path
import os
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/noise-tests'
BUILD.mkdir(parents=True, exist_ok=True)
binding = (ROOT / 'source/rendering/materialbindings.cpp').read_text()
start = binding.index('    struct MaterialPartShaderConstants')
end = binding.index('    static_assert', start)
layout = binding[start:end]
cull = (ROOT / 'shader/hlsl/GpuSceneCullCS.hlsl').read_text()
cull_gate = re.search(r'if \((!mesh.VertexCount[^\n]+)\)', cull)[1]
reflection = (ROOT / 'shader/hlsl/RayReflectionsCS.hlsl').read_text()
reflection = reflection[reflection.index('float3 ShadeReflectionHit('):]
reflection_gate = re.search(r'\[branch\] if \(([^\n]+)\)', reflection)[1]
visibility = (ROOT / 'shader/hlsl/GPUDrivenVisibilityResolveCS.hlsl').read_text()
share_gate = next(m[1] for m in re.finditer(r'share = ([^;]+);', visibility) if 'metadata.z' in m[1])
shader_types = (ROOT / 'shader/hlsl/GpuSceneCommon.hlsli').read_text()
macros = '\n'.join(re.findall(r'^#define GPU_SCENE_[A-Z_]+ \d+u?$', shader_types, re.M))
capacity = re.search(r'GpuSceneMaterialParameterVectors = (\d+)',
                     (ROOT / 'source/rendering/gpuscene/gpuscenetypes.h').read_text())[1]
program = r'''
#include "procedural/proceduraltextureparams.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
struct XMFLOAT3 { float x, y, z; };
struct XMFLOAT4 { float x, y, z, w; };
struct XMUINT4 { uint32_t x, y, z, w; };
constexpr int kMaterialPartParamCount = 15;
''' + macros + '\n' + layout + r'''
bool culled(unsigned parameterCount)
{
    struct { unsigned VertexCount = 3; } mesh;
    return CULL_GATE;
}
bool needsFrame(unsigned height, unsigned normalNoise)
{
    struct Mesh {
        XMUINT4 Surface{};
        std::array<ProcStudio::TextureGpuParams, 3> Noise{};
    } mesh;
    mesh.Surface.z = height;
    mesh.Noise[1].Width = normalNoise;
    return REFLECTION_GATE;
}
bool all(bool value) { return value; }
bool shared(unsigned colorNoise, unsigned normalNoise, unsigned heightNoise)
{
    struct XYZ { bool operator>=(unsigned) const { return true; } };
    struct Asset {
        struct { XYZ xyz; } Textures;
        struct { float z = 1; } Surface;
        struct { float x = .05f, z = -1; } Parallax;
        std::array<ProcStudio::TextureGpuParams, 3> Noise{};
    } asset;
    XMUINT4 metadata{0, UINT32_MAX, 0, 0};
    asset.Noise[0].Width = colorNoise;
    asset.Noise[1].Width = normalNoise;
    asset.Noise[2].Width = heightNoise;
    return SHARE_GATE;
}
int main()
{
    const unsigned vectors = (sizeof(PBRConstants) + 15) / 16;
    assert(!culled(vectors) && "valid material constants must survive GPU culling");
    assert(culled(CAPACITY + 1) && "out-of-bounds constants must be rejected");
    assert(needsFrame(0, 1) && "normal-only .noise needs a reflection tangent frame");
    assert(needsFrame(1, 0) && needsFrame(1, 1));
    assert(!needsFrame(0, 0));
    assert(shared(0, 0, 0));
    assert(!shared(1, 0, 0) && "procedural color must be shaded at each pixel");
    assert(!shared(0, 1, 0) && "procedural normals must be shaded at each pixel");
    assert(!shared(0, 0, 1) && "procedural POM must preserve per-pixel depth");
    std::cout << "Material shader contracts: " << vectors << " vectors survive culling; reflection and per-pixel noise gates PASS\n";
}
'''
program = program.replace('CULL_GATE', cull_gate).replace('REFLECTION_GATE', reflection_gate).replace('CAPACITY', capacity).replace('SHARE_GATE', share_gate)
source = BUILD / 'material-contracts.cpp'
source.write_text(program)
binary = BUILD / 'material-contracts'
subprocess.run([os.environ.get('CXX', 'g++'), '-std=c++20', '-I', str(ROOT / 'source'), str(source), '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True)
