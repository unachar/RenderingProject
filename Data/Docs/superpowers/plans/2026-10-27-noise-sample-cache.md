# Procedural base color sample cache implementation plan

**Goal:** Reuse procedural base colors at requested UV positions without baking or limiting close-up detail.

**Architecture:** A bounded GPU cache uses the complete UV bit pattern and a definition identity. Render passes read an immutable four-way cache and reserve miss records atomically. A frame-end compute pass merges completed misses, then resets reservations. No locks, spinning, CPU readbacks, or partial-entry reads occur in rendering.

**Tech stack:** C++20, D3D12, HLSL. Continue in the current checkout to preserve existing changes.

The user approved on-demand caching with bounded storage and continued procedural evaluation on approach. Compared with fixed-resolution baking, exact sample caching preserves the existing image. Compared with interpolated UV tiles, it avoids approximation but provides less reuse while the camera moves or jitters. This limitation must be measured and reported; 150fps is a target, not an assumed result.

## Acceptance and review focus

- Cache hits return exactly the uncached GPU color, including sub-texel and negative UVs.
- Definition edits, seed changes, and new UVs cannot return old values.
- Concurrent misses and hash collisions never produce mixed keys and colors.
- Storage is bounded; eviction changes work, never the resulting color.
- Initialization and frame-end barriers follow the graphics queue. Read-only shader access and miss writes are separate resources.
- Main raster, GPU driven, mesh/virtual geometry, reflection/GI and glass share the cache. Height/normal evaluation remains analytical.
- Null views preserve the uncached path if the cache is unavailable.

## Tasks

- [ ] Add GPU regression/measurement covering cold/warm hits, collisions, edits, close-up positions and all bases; run before implementation.
- [ ] Add independent procedural cache resources and frame-end merge shader; reuse spare parameter words for a 64-bit definition fingerprint.
- [ ] Integrate cache resources into render roots and frame lifetime without exceeding the main root's 64-DWORD limit.
- [ ] Run GPU correctness and performance checks, compile affected shader paths, build application, review diff and document actual limitations.
