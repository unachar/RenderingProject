# Dual Quaternion Skinning Implementation Plan

**Goal:** Apply DQS to the existing animation GPU skinning pipeline.
**Architecture:** Shared HLSL math, rigid-only DQS before the existing LBS fallback; SDEF keeps precedence.
**Tech Stack:** HLSL SM6, C++ portable tests, existing D3D12 compute fixture.
**Spec:** docs/superpowers/specs/2026-10-29-dual-quaternion-skinning-design.md

## Tasks
- [x] Add tests that execute the actual HLSL math under a test-only C++ shim; observe failure before implementing.
- [x] Extract existing quaternion functions and add rigid validation, dual quaternion construction, hemisphere blending, normalization and transforms.
- [x] Integrate DQS after SDEF and before LBS without changing buffer ABI. Register include in project.
- [x] Add reproducible portable and Windows GPU test runners, run available regressions, review diff, review and commit; update dev_main with a normal fast-forward push after verifying its head.

## Review focus
Matrix transpose convention; exact 180-degree rotations; translation under antipodal correction; unsupported scale/shear/reflection; matching outlines and previous-frame positions. Preserve valid single-bone output and invalid-index handling.

## Verification and review
- Shared production HLSL math compiled as C++17 and passed: identity, seven rotation cases, translation, hemisphere/dual sign, twist radius, four influences, degenerate normalization, scaled/sheared/reflected/nonfinite matrices.
- Five existing portable suites passed: skinning_work, renderfeature, temporalhistory, rendermeasurement, stablesampling.
- Material shader contract and thumbnail material contract scripts passed; project XML and git diff whitespace checks passed.
- Linux DXC 1.9.2609.5 compiled actual cs_6_0/HV2021 shader with -WX in optimized and debug configurations.
- Independent review confirmed production math, transpose conventions and buffer ABI. Corrected GPU SDEF pivot fixture and DXC import-library search path in Windows runner.
- Windows GPU tests, full application builds, and visual/performance checks were not run. Existing dxc_build_contract_tests.py cannot execute here because it requires Windows vswhere.exe. An initial unittest discovery invocation was unsuitable for that standalone script; portable scripts were rerun directly.
- Ruling: use shader-local conversion rather than introducing another bone palette, preserving existing import/cache/buffer layouts; costs extra per-vertex GPU work, to profile on Windows.
