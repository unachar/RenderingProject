"""Portable .noise serialization, mixed-source binding and procedural regression tests.

Run with Python 3 and a C++20 compiler (CXX, default g++). The Windows-only PCH
include is removed from a build-directory copy of the CPU generator, never source.
Use scripts/test-procedural-gpu.ps1 on Windows for real D3D12 evaluation parity.
"""
from pathlib import Path
import os
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/noise-tests'
BUILD.mkdir(parents=True, exist_ok=True)
JSON = ROOT / 'External/RTXNS/external/donut/thirdparty/jsoncpp'
compiler = os.environ.get('CXX', 'g++')
flags = [compiler, '-std=c++20', '-O1', '-pthread', '-I', str(ROOT / 'source'),
         '-I', str(ROOT / 'source/procedural'), '-I', str(JSON / 'include'),
         '-include', 'array', '-include', 'algorithm', '-include', 'functional']
source = (ROOT / 'source/procedural/proceduralstudio.cpp').read_text(encoding='utf-8')
portable = BUILD / 'proceduralstudio.cpp'
portable.write_text(source.replace('#include "pch.h"', '', 1), encoding='utf-8')

def run_test(name, files):
    binary = BUILD / name
    subprocess.run(flags + [str(ROOT / path) for path in files] + ['-o', str(binary)], check=True, cwd=ROOT)
    subprocess.run([str(binary)], check=True, cwd=ROOT)

run_test('noise-assets', ['tests/noise_asset_tests.cpp', 'source/assets/noiseasset.cpp',
                         'source/assets/materialasset.cpp', 'source/assets/assetregistry.cpp', str(portable),
                         *[str(path) for path in (JSON / 'src/lib_json').glob('*.cpp')]])
for name in ['procedural_noise', 'procedural_texture_patterns', 'procedural_tree_normals']:
    files = [f'tests/{name}_tests.cpp']
    if name != 'procedural_noise': files.append(str(portable))
    run_test(name, files)
# Execute the existing material suite with only its OS process-id helper adapted.
material_test = (ROOT / 'tests/material_asset_tests.cpp').read_text(encoding='utf-8')
material_portable = BUILD / 'material_asset_tests.cpp'
material_portable.write_text(material_test.replace('#include <windows.h>', '#include <unistd.h>')
                             .replace('GetCurrentProcessId()', 'getpid()'), encoding='utf-8')
run_test('material-assets', [str(material_portable), 'source/assets/materialasset.cpp',
                           'source/assets/assetregistry.cpp',
                           *[str(path) for path in (JSON / 'src/lib_json').glob('*.cpp')]])
subprocess.run([sys.executable, str(ROOT / 'tests/noise_material_contract_tests.py')], check=True, cwd=ROOT)
print('Noise material portable regression suite: PASS', flush=True)
