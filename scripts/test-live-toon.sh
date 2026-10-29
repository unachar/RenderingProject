#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
validation_dir="$(mktemp -d)"
trap 'rm -rf "$validation_dir"' EXIT
for test in stage_lighting toon_lighting toon_outline_projection skinning_work; do
    "${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror -I source "tests/${test}_tests.cpp" -o "$validation_dir/$test"
    "$validation_dir/$test"
done
if [[ "${1:-}" == "--shaders" ]]; then
    compiler="${DXC:-dxc}"
    "$compiler" -T vs_6_0 -E main -O3 shader/hlsl/toonOutlineVS.hlsl -Fo "$validation_dir/outline.dxil"
    "$compiler" -T cs_6_0 -E main -O3 shader/hlsl/skinning_cs.hlsl -Fo "$validation_dir/skinning.dxil"
    for shader in DeferredLightingPS modelshaderPS; do
        "$compiler" -T ps_6_0 -E main -O3 "shader/hlsl/$shader.hlsl" -Fo "$validation_dir/$shader.dxil"
        "$compiler" -T ps_6_5 -E main -O3 -D SHADER_RAYTRACING=1 "shader/hlsl/$shader.hlsl" -Fo "$validation_dir/$shader-rt.dxil"
    done
    "$compiler" -T ps_6_5 -E MegaLightsShadePS -O3 -D SHADER_RAYTRACING=1 \
        shader/hlsl/DeferredLightingPS.hlsl -Fo "$validation_dir/megalights.dxil"
    for shader in DeferredPS modelshaderPS_MRT GeometryPS GPUDrivenOpaquePS; do
        "$compiler" -T ps_6_0 -E main -O3 "shader/hlsl/$shader.hlsl" -Fo "$validation_dir/$shader.dxil"
    done
    for shader in GBufferFromVisibilityCS GPUDrivenVisibilityResolveCS; do
        "$compiler" -T cs_6_0 -E main -O3 "shader/hlsl/$shader.hlsl" -Fo "$validation_dir/$shader.dxil"
    done
    echo 'Live toon DXC variants: PASS'
fi
