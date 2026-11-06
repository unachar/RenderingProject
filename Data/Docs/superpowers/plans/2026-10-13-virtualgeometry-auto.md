# VirtualGeometry Auto Registration Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task in the current chat.

**Goal:** Automatically register static geometry, render VirtualGeometry leaf clusters, and expose LOD and cluster debugging.

**Architecture:** A portable runtime builder owns shared, cached hierarchy assets and converts leaf clusters to the existing Mesh Shader contract. Source vertex matching preserves the existing geometry arena, Discrete LOD, shadows and DXR. Existing editor debugging is extended with viewport overlays and Nanite asset inspection.

**Tech Stack:** C++20, meshoptimizer, DirectX 12, HLSL, ImGui, CMake CPU tests.

**Spec:** docs/virtualgeometry-auto-registration-design.md (approved 2026-10-13).

## Global Constraints

- Preserve existing uncommitted changes; do not commit.
- First integration renders finest clusters; continuous hierarchy LOD and streaming are outside scope.
- Automatically inspect all objects; retain traditional rendering for unsupported materials, deformation and hardware.
- No per-frame rebuild; permit import-time builds in Debug and Release.
- Debug values reflect actual GPU selection; no debug readback when disabled.

## Review Focus

- UV/normal seams and repeated positions: exact complete-attribute vertex matching.
- Material changes: cache key changes and rebuilt shader vertex attributes.
- Repeated terrain edits: replace registered meshlet ranges without unlimited append growth.
- Corrupt cache: reject and rebuild, preserving fallback on errors.
- Missing alternate LOD or unsupported GPU: no missing or duplicated geometry.

### Task 1: Portable build/cache/leaf conversion

Files: source/virtualgeometry/virtualgeometryruntime.h/.cpp, tests/virtualgeometry_runtime_tests.cpp, tests/virtualgeometry/CMakeLists.txt.

Interfaces: VirtualGeometryRuntime::Acquire(vertices, indices, materialId, materialPartId, cacheDirectory) returns a shared RuntimeAsset containing Hierarchy and Meshlets. RuntimeAsset retains cache/build state and errors; stats expose cache hits, builds and failures.

- [x] Add tests for source triangle preservation, seam attributes, sharing, geometry/material key changes and corrupt-cache recovery; run to see missing API failure.
- [x] Implement compatible 64-vertex/124-triangle clusters, full-attribute source remap, bounded cache loading and shared ownership.
- [x] Run Debug and Release VirtualGeometry suites.

### Task 2: Automatic import/update/GPU registration

Files: source/models/staticmodel.h/.cpp, source/rendering/meshlets/meshletmanager.h/.cpp, source/rendering/gpudriven/gpudrivenrenderer.cpp, DirectX12.vcxproj/.filters.

- [x] Retain shared runtime assets per source LOD and invoke Acquire from the common import/generation entry.
- [x] Register leaf Meshlets using exact original vertex remaps. Rebuild material edits and enable supported Mesh Shader rendering by default.
- [x] Replace invalidated registered data by reusing free ranges; ensure bounds/uploads reflect edited geometry.
- [x] Build Debug/Release and run existing meshlet and GPU validation paths; record the broader GPU Scene stress hang rather than reporting it as passing.

### Task 3: LOD/cluster editor debugging

Files: source/rendering/meshshader/meshshaderrenderer.h/.cpp, source/rendering/visibility/gpuvisibility.h/.cpp, source/editor/editorscene.cpp, source/virtualgeometry/virtualgeometrydebug.h/.cpp.

- [x] Reuse actual GPU LOD shading with a fixed legend and forced LOD controls; add camera freeze with conservative occlusion handling.
- [x] Show shared hierarchy levels, cluster/triangle/error stats, import cache/build state and instance fallback reasons.
- [x] Draw candidate bounds/cones and selected hierarchy bounds over the actual Scene image, filtered by entity and capped; acquire debug records only on demand.
- [x] Verify shading, force/auto, camera freeze, transformed bounds, traditional fallback and debug-off behavior.

### Task 4: Review and validation record

- [x] Review index/attribute contracts, updated ranges, GPU lifetime, partial build cleanup, viewport clipping and existing changes.
- [x] Record CPU/build/runtime evidence and any untested editor operations in docs/virtualgeometry-auto-registration.md.
