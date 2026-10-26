# Noise Materials Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Resolution-independent `.noise` assets alongside texture material inputs.
**Architecture:** Versioned JSON definitions, shared CPU/GPU parameter layout and HLSL evaluation, three optional material channels propagated through all material paths.
**Tech Stack:** C++20, JsonCpp, HLSL, D3D12, ImGui.
**Spec:** docs/superpowers/specs/2026-10-26-noise-material-design.md

## Global Constraints
- Work on dev_main; retain existing texture/material GUID formats and opaque-only POM behavior.
- Noise must be evaluated per shading coordinate, without baking a material texture.
- Keep all existing GPU noise algorithms and parameters; escape fractals retain CPU image export.

## Review Focus
- Invalid enums, huge/NaN parameters, malformed/versioned JSON must not change the previous valid definition.
- Switching texture/noise/empty sources must clear stale state; moves and reload must preserve GUIDs.
- Preview, GPU scene and shader constants must agree on offsets/stride.
- Normal orientation and POM height/self-shadow must agree across raster and reflection paths.
- Existing image export and ordinary texture materials must continue to compile and pass regression tests.

### Task 1: Definitions and parameter layout
Files: source/assets/noiseasset.{h,cpp}, source/procedural/proceduraltextureparams.h, tests/noise_asset_tests.cpp, source/assets/{materialasset.cpp,assetregistry.cpp,materialasset.h}.
Interfaces: NoiseSerializer::{Validate,Load,Save}(TextureParams); ProcStudio::MakeTextureGpuParams(TextureParams), TextureGpuParams (208 bytes).
- [x] Add/run failing round-trip, corrupt/version rejection, finite validation, classification and layout tests.
- [x] Implement serialization, atomic saving, registry type and shared parameter packing.
- [x] Run portable C++ tests including existing material/noise tests.

### Task 2: Shared GPU evaluation and material runtime
Files: shader/hlsl/{ProceduralTextureCS.hlsl,ProceduralTexture.hlsli,MaterialSurface.hlsli,common.hlsl,DeferredPS.hlsl,GeometryPS.hlsl,modelshaderPS*.hlsl,GpuSceneCommon.hlsli}; source/{assets/materialmanager.*,ecs/componentmanager.h,rendering/materialbindings.cpp,rendering/rendererstate.h,rendering/gpuscene/gpuscenetypes.h,procedural/proceduraltexturegpu.cpp}.
Interfaces: three optional TextureGpuParams on MaterialComponent; shared ProceduralTexture::Scalar/Color/Normal; LoadAssetMaterial loads records at vector 151.
- [x] Add failing shader evaluation/layout coverage, compile with DXC.
- [x] Share evaluator, extend constants, load definitions per source, preserve texture branch, route POM/normal/color evaluation.
- [x] Compile affected shaders (descriptor tables and bindless); run procedural regression tests.

### Task 3: Editor, previews and reflections
Files: source/editor/{proceduralstudiowindow.cpp,editorassets.cpp,editormaterialassets.cpp,assetthumbnailmanager.cpp}; source/rendering/{thumbnailrenderer.*,raytracingscene.*,rayreflections.cpp}; shader/hlsl/RayReflectionsCS.hlsl; tests and scripts.
Interfaces: `.noise` asset payload accepted only by Base Color, Normal, Height; existing texture handles serialize both types. Shader parameters reused by ray reflection records and thumbnail bindings.
- [x] Add failing mixed-source and end-to-end shader checks.
- [x] Add save/load UI and drop support, source diagnostics, preview handling and ray reflection sampling.
- [x] Run portable tests, shader compilation, diff review; document Windows-only validation limitation.

## Pre-flight
Task 1→2: TextureGpuParams is exactly 13 vectors / 208 bytes; all consumers use this layout.
Task 2→3: Runtime channel order is BaseColor, Normal, Height. Serialized references remain MaterialAssetData::Textures[0,1,5].
Execution: native in this session, as authorized by the user's approval to proceed. No further design approval required.


## Execution evidence
- Task 1 complete: versioned definitions, 208-byte shared parameters, GUID classification and atomic save. `python scripts/test-noise-materials.py` passed after final source edits.
- Task 2 complete: image/material evaluator sharing and 190-vector material constants, with GPU Scene capacity 192 / stride 3136. Raster, GPU-driven, bindless and visibility shaders compiled with DXC `-O3`.
- Task 3 complete: editor save/load/drop, mixed-source bindings, thumbnails, secondary reflection/GI/glass color propagation. All 17 targets listed in `scripts/test-noise-shaders.py` compiled; GPUDrivenOpaqueVS additionally compiled.
- DXC reflection confirms PBR constants 3040 bytes / noise offset 2416, reflection records 848 bytes and glass records 448 bytes.
- Independent code review identified the GPU Scene culling capacity and normal-only secondary-reflection gate. Both were reproduced by `tests/noise_material_contract_tests.py`, fixed and passed.
- Final verification identified coarse visibility shading incorrectly sharing procedural pixels and missing-source diagnostics after registry refresh. Both were reproduced by regression tests and fixed. Visibility material shading now has one evaluator call site instead of five unrolled copies.
- Final review findings: no deferred issues. No product-scope changes beyond the approved design.
- Windows application build, GUI drag/drop and D3D12 hardware/WARP execution were not available in this Linux environment. Windows test entry points are included; compilation is not claimed as runtime rendering validation.
