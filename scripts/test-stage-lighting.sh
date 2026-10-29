#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
validation_dir="$(mktemp -d)"
trap 'rm -rf "$validation_dir"' EXIT
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror tests/stage_lighting_tests.cpp -o "$validation_dir/stage"
"$validation_dir/stage"
python3 tests/stage_light_resolve_tests.py
if [[ "${1:-}" == "--shaders" ]]; then
    compiler="${DXC:-dxc}"
    # Forward model variants are covered by test-live-toon.sh --shaders.
    for shader in DeferredLightingPS AtmosphereGBufferPS; do
        "$compiler" -T ps_6_5 -E main -O3 -D SHADER_RAYTRACING=1 "shader/hlsl/$shader.hlsl" -Fo "$validation_dir/$shader.dxil"
    done
    "$compiler" -T cs_6_5 -E FogInject -O3 shader/hlsl/AdvancedEffectsCS.hlsl -Fo "$validation_dir/fog.dxil"
    "$compiler" -T cs_6_5 -E main -O3 shader/hlsl/RayReflectionsCS.hlsl -Fo "$validation_dir/reflections.dxil"
    echo 'Stage lighting DXC variants: PASS'
fi
