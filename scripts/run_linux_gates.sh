#!/usr/bin/env bash
# All dev_muse gates that run on Linux, in one command:
# the Python test suites under tests/ (excluding the Windows-only
# dxc_build_contract_tests.py and the pre-existing
# stage_light_resolve failure - see the verification checklist)
# and, when DXC is set, the HLSL compile sweep.
#
# Usage: [DXC=/path/to/dxc LD_LIBRARY_PATH=/path/to/lib] ./run_linux_gates.sh
set -u
cd "$(dirname "$0")/.."
fail=0
for t in tests/shader_scan_tests.py tests/pfm_compare_tests.py \
         tests/traversal_binning_contract_tests.py tests/ssr_constants_contract_tests.py \
         tests/reference_integrator_contract_tests.py tests/postprocess_probe_contract_tests.py \
         tests/shader_build_targets_contract_tests.py tests/decal_layout_contract_tests.py \
         tests/spd_layout_contract_tests.py \
         tests/frame_lighting_order_tests.py tests/megalights_compaction_tests.py \
         tests/noise_material_contract_tests.py tests/render_capture_comparison_tests.py \
         tests/thumbnail_material_contract_tests.py tests/tsr_tile_tests.py; do
    if python3 "$t" >/dev/null 2>&1; then
        echo "PASS $t"
    else
        echo "FAIL $t"; fail=1
    fi
done
if [ -n "${DXC:-}" ]; then
    if DXC="$DXC" bash scripts/compile_check_linux.sh >/dev/null 2>&1; then
        echo "PASS compile sweep"
    else
        echo "FAIL compile sweep"; fail=1
    fi
else
    echo "SKIP compile sweep (DXC not set)"
fi
exit $fail
