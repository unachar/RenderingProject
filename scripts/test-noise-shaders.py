"""Compile procedural image and material paths with DXC; DXC environment variable selects the compiler."""
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
import argparse
import os
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/noise-tests'
TARGETS = {
    'definition': ('tests/noise_material_shader.hlsl', 'cs_6_0', 'main', []),
    'image': ('shader/hlsl/ProceduralTextureCS.hlsl', 'cs_6_0', 'Generate', []),
    'image-normal': ('shader/hlsl/ProceduralTextureCS.hlsl', 'cs_6_0', 'Normals', []),
    'image-preview': ('shader/hlsl/ProceduralTextureCS.hlsl', 'cs_6_0', 'Preview', []),
    'deferred': ('shader/hlsl/DeferredPS.hlsl', 'ps_6_0', 'main', []),
    'geometry': ('shader/hlsl/GeometryPS.hlsl', 'ps_6_0', 'main', []),
    'forward': ('shader/hlsl/modelshaderPS.hlsl', 'ps_6_0', 'main', []),
    'mrt': ('shader/hlsl/modelshaderPS_MRT.hlsl', 'ps_6_0', 'main', []),
    'gpu': ('shader/hlsl/GPUDrivenOpaquePS.hlsl', 'ps_6_0', 'main', []),
    'bindless': ('shader/hlsl/GPUDrivenOpaquePS.hlsl', 'ps_6_6', 'main',
                 ['-D', 'RP_BINDLESS=1', '-D', 'RP_TEXTURE_HEAP_BASE=512']),
    'visibility': ('shader/hlsl/GPUDrivenVisibilityResolveCS.hlsl', 'cs_6_0', 'main', []),
    'virtual': ('shader/hlsl/VirtualGeometryOpaquePS.hlsl', 'ps_6_0', 'main', []),
    'reflection': ('shader/hlsl/RayReflectionsCS.hlsl', 'cs_6_5', 'TraceDiffuse', ['-D', 'USE_NRD=0']),
    'gi-card': ('shader/hlsl/RayReflectionsCS.hlsl', 'cs_6_5', 'LumenCapture', ['-D', 'USE_NRD=0']),
    'glass': ('shader/hlsl/modelshaderPS.hlsl', 'ps_6_5', 'main', ['-D', 'SHADER_RAYTRACING=1']),
    'scene-cull': ('shader/hlsl/GpuSceneCullCS.hlsl', 'cs_6_0', 'main', []),
    'shadow': ('shader/hlsl/ShadowMapPS.hlsl', 'ps_6_0', 'main', []),
}
parser = argparse.ArgumentParser()
parser.add_argument('targets', nargs='*', choices=list(TARGETS))
parser.add_argument('--jobs', type=int, default=2)
args = parser.parse_args()
dxc = os.environ.get('DXC') or shutil.which('dxc')
if not dxc: raise SystemExit('Set DXC to a DirectX Shader Compiler executable.')
BUILD.mkdir(parents=True, exist_ok=True)

def compile_shader(name):
    source, profile, entry, defines = TARGETS[name]
    log = BUILD / f'compile-{name}.log'
    with log.open('w') as output:
        result = subprocess.run([dxc, '-T', profile, '-E', entry, '-O3', *defines, source,
                                 '-Fo', str(BUILD / f'{name}.dxil')], stdout=output, stderr=output, cwd=ROOT)
    if result.returncode: raise RuntimeError(f'{name}: {log.read_text()[-6000:]}')
    return f'{name}: PASS'

with ThreadPoolExecutor(max_workers=args.jobs) as executor:
    tasks = [executor.submit(compile_shader, name) for name in (args.targets or TARGETS)]
    for task in as_completed(tasks): print(task.result(), flush=True)
print('Noise material shader compilation: PASS', flush=True)
