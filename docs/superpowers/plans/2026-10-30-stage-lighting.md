# Stage Lighting Implementation Plan

Goal: independent range/intensity controls and capsule stage light on dev_main.
Architecture: shared CPU packing and scalar HLSL math, unchanged GPU layout.
Tech Stack: C++, HLSL, ImGui.
Spec: docs/superpowers/specs/2026-10-30-stage-lighting-design.md

## Tasks
- [x] Add numerical tests for unchanged legacy attenuation, constant stage brightness, smooth zero boundary, density decoding and capsule closest point; run RED.
- [x] Implement StageLighting.hlsli and shared CPU packing; expose fields in factory, inspector and scene serialization. Add Capsule enum, creation menus, debug display and conservative culling. Wire common shader and exact Rect dispatch. Run GREEN.
- [x] Run existing toon tests, Python contracts and available shader compilation. Review diff, document limitations, commit and push dev_main if available.

Review focus: old scene defaults, zero range, capsule endcaps, Rect-only LTC, all upload paths.

Validation ledger: scalar math, extracted common resolver and actual CPU LightComponent/packing helpers PASS. Existing toon/outline/skinning tests PASS. Eight portable Python regression programs PASS; Windows-only MSBuild contract is unavailable on Linux. Independent final code review found no actionable issues. DXC -O3: 13 live-toon variants + 4 stage variants PASS (17 total). Windows app build and actual D3D12 rendering remain unverified.
