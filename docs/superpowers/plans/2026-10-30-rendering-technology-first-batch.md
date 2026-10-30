# Rendering Technology First Batch Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** Reduce runtime DXIL cache size and add an opt-in PBR geometric specular antialiasing evaluation path.

**Architecture:** Keep the current DX12 renderer and cache. Add explicit compiler-policy helper and a shared scalar roughness filter. Preserve default rendering and Debug shader metadata; compile all affected material paths in both modes.

**Tech Stack:** C++17/20, HLSL 2021, existing DXC 1.9 package, PowerShell, D3D12.

**Spec:** docs/superpowers/specs/2026-10-30-rendering-technology-integration-design.md (approved by user).

## Global Constraints

- No new external runtime library or wholesale renderer replacement.
- Debug reflection remains; override DX12_KEEP_SHADER_REFLECTION=1 retains original Release reflection.
- Geometric specular AA defaults OFF, variance scale 0.15 and threshold 0.2.
- Toon/unlit/packed impostor and Forward transparent stay unchanged.
- Compare actual byte counts, executable DXIL, signatures and required container parts; never infer FPS from bytes.
- No automatic commit/push/PR required. Execute inline following user approval to implement.

## Review Focus

- Environment override and Debug combinations: test production compiler argument policy and distinct cache inputs.
- Reflect-strip must not remove root signature/PSV/signatures: compare actual DXIL containers.
- Geometric derivatives must precede discard; compile PS and compute variants, test derivative source ordering.
- Invalid reconstructed normals must not inject NaNs; test finite rejection and failed direction fallback.
- OFF path/Toon classes/zero gradients must preserve original roughness: numerical and GPU checks.

### Task A: Runtime DXIL reflection stripping

**Files:** source/dxccompilerpolicy.h (new), source/dxccompiler.h; tests/dxccompilerpolicy_tests.cpp; scripts/test-dxccompilerpolicy.ps1; scripts/test-dxil-reflection.ps1; docs/rendering-technology-integration-validation.md.

**Interfaces:** DxcCompilerPolicy::ShouldStripReflection(unsigned flags, bool keepReflection) -> bool. Uses D3DCOMPILE_DEBUG supplied from existing DXC API header. The helper appends -Qstrip_reflect before CachePath is calculated.

- [ ] Write/run failing tests for Debug, Release, environment override and cache argument separation.
- [ ] Add helper and runtime compile option using existing flags and environment-query conventions.
- [ ] Run policy tests; compile PS/VS/CS with real DXC and compare execution-relevant container parts and sizes.
- [ ] Preserve Debug shader info; verify user override; record totals and limitations.

### Task B: Opt-in geometric specular AA

**Files:** shader/hlsl/GeometricSpecularAA.hlsli (new), shader/hlsl/common.hlsl, shader/hlsl/DeferredPS.hlsl, shader/hlsl/GPUDrivenVisibilityResolveCS.hlsl; tests/geometric_specular_aa_tests.cpp; scripts/test-geometric-specular-aa.ps1; affected build dependencies in Directory.Build.targets/MaterialShaders.targets only where required.

**Interfaces:** shared scalar FilterGeometricRoughness(float roughness, float normalGradientSquared, float varianceScale, float threshold) -> float. HLSL wrapper derives gradients in PS or receives same-triangle gradients in Compute. GEOMETRIC_SPECULAR_AA defaults 0.

- [ ] Write/run failing production-math tests for zero gradients/settings, r=0/1, monotonicity, large gradients and finite checks.
- [ ] Implement scalar filter and guarded derivative inputs with finite fallback, preserving OFF calculation.
- [ ] Attach final roughness filter only to eligible PBR GBuffer classes, after part/asset selection. Compute interpolated geometry normals before normal-map changes.
- [ ] Test production math, compile PS/GPU Driven/Visibility CS ON/OFF and validate dependency tracking.
- [ ] Add GPU fixture using real production shader function for roughness outputs and disabled/Toon behavior; run full renderer smoke if available.
- [ ] Record actual image/timing coverage and any pending temporal-quality benchmark; keep feature OFF unless evidence justifies enabling.

### Final validation and review

- [ ] Build affected code and shaders; run focused regression checks and renderer smoke.
- [ ] Get independent review, address actionable findings, rerun only affected checks.
- [ ] Update checked steps, survey and validation evidence; report unfinished parts of the broad goal accurately.

