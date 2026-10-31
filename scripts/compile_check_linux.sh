#!/usr/bin/env bash
# Linux DXC compile sweep for the shaders the dev_muse integrations
# touch. This is a smoke gate, not the build: the Windows build
# (Directory.Build.targets) remains authoritative. It catches HLSL
# syntax/register errors on any machine with a Linux DXC.
#
# Usage: DXC=/path/to/dxc LD_LIBRARY_PATH=/path/to/lib ./compile_check_linux.sh
set -u
DXC="${DXC:-dxc}"
cd "$(dirname "$0")/.."
ok=0; fail=0
check() { # profile entry file [extra args...]
    local profile="$1" entry="$2" file="$3"; shift 3
    if "$DXC" -T "$profile" -E "$entry" -O3 "$@" -Fo "/tmp/cc_$(basename "$file")_$entry.cso" "$file" 2>/tmp/cc_err.txt; then
        ok=$((ok+1))
    else
        fail=$((fail+1)); echo "FAIL $file ($entry)"; head -3 /tmp/cc_err.txt
    fi
}
for e in Reset Traverse BuildArguments Partition Retest BuildTemporalArguments; do
    check cs_6_5 "$e" shader/hlsl/VirtualGeometryTraversalCS.hlsl
done
check ps_6_0 main shader/hlsl/DeferredLightingPS.hlsl -I shader/hlsl
check ps_6_0 main shader/hlsl/modelshaderPS.hlsl -I shader/hlsl
check ms_6_5 main shader/hlsl/VirtualGeometryShadowBinnedMS.hlsl -I shader/hlsl
check cs_6_0 main shader/hlsl/ScreenSpaceReflectionsTraceCS.hlsl
check cs_6_0 main shader/hlsl/ScreenSpaceReflectionsResolveCS.hlsl
check cs_6_0 main shader/hlsl/ScreenSpaceReflectionsDownsampleCS.hlsl
check cs_6_5 main shader/hlsl/ReferencePathTraceCS.hlsl
check cs_6_5 AovMain shader/hlsl/ReferencePathTraceAovCS.hlsl
check cs_6_0 main shader/hlsl/EnvironmentCubeToEquirectCS.hlsl
echo "compile sweep: ok=$ok fail=$fail"
[ "$fail" -eq 0 ]
