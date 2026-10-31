# Tests

Standalone scripts; run each with python3 from the repo root.
None of them need a GPU or a Windows build.

| Script | Gates |
| --- | --- |
| `shader_scan_tests.py` | HLSL conventions across the shader tree (96 cases). |
| `pfm_compare_tests.py` | The Stage 2H PFM metrics tooling (RMSE / SSIM / regions / exit codes). |
| `traversal_binning_contract_tests.py` | Stage 1C: 272-byte traversal constants, Statistics bytes 28/96, dual shadow commands, binned PSOs, light-LAYER view keying, per-view Counters ownership. |
| `ssr_constants_contract_tests.py` | SSR: trace 56 / resolve 28 root constants on both sides of the host/shader boundary, heap-slot bindings, ping-pong slots. |
| `reference_integrator_contract_tests.py` | Stage 2H: 30-DWORD constants, UAV range, scene-buffer register order (host enum vs shader t1..t8). |
| `postprocess_probe_contract_tests.py` | Stage 2J: PostProcessParams probe tail at byte 400 in both layout descriptions, 480-byte CB. |
| `shader_build_targets_contract_tests.py` | Linux port of `scripts/test-shader-build-targets.ps1`: 45 tracked DXC outputs, no duplicates, dependency existence. |
| `dxc_build_contract_tests.py` | Windows-only (vswhere): DXC package / project wiring. |
| `geometric_specular_aa_tests.cpp` | Stage 1B: CPU-side checks for the geometric specular AA math. |
| `virtual_geometry_streaming_tests.cpp` | VG streaming unit checks. |

The contract tests exist because these agreements (constant
sizes, register orders, byte offsets) are invisible to both
compilers: HLSL and C++ each compile happily when the other
side drifts. When one fails, fix the side that moved - or, if
the move was deliberate, update both sides and the test in the
same commit.
