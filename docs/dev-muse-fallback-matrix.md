# Fallback matrix — dev_muse integrations (Stage 9 acceptance)

Every integration on this branch is either behind a default-OFF
flag with the pre-existing path as fallback, or structured so a
failure degrades to the legacy path at runtime. This matrix is
the checklist for the simultaneous-enablement acceptance run.

| Feature | Flag / gate | Fallback when OFF or failed |
| --- | --- | --- |
| D3D12MA allocations | `UseD3D12MemoryAllocator` (OFF) | Committed resources via the same call sites; a failed MA allocation also falls back per-resource at creation time |
| FidelityFX SPD Hi-Z | `UseFidelityFxSpdHiZ` (OFF) | Legacy Hi-Z reduction chain generates the pyramid; SPD only replaces the generator |
| VG dedicated shadows | Traversal settings (ShadowTraversal) | Legacy caster submission for any view without a prepared traversal slot |
| Shadow material binning | Always on inside the dedicated path | Whole dedicated draw returns false -> legacy submission (missing PSO or command signature) |
| Alpha-tested VG shadows | Material shadow mode data | Materials outside modes [1,2] are discarded by the shadow PS; unclassified casters take the opaque bin |
| DBuffer decals | `UseDBufferDecals` (OFF) | No decal composite; the separate LightRenderMode::Decal feature is unaffected |
| SSR | `UseScreenSpaceReflections` (OFF) | Selection state stays env IBL (w=0); DXR reflections (w=1) outrank SSR whenever active |
| SSR checkerboard | `UseSsrCheckerboard` (OFF) | Full half-res trace every frame |
| SSR roughness skip | `SsrRoughnessLimit` = 1.0 | No per-pixel skip (the pre-run behavior) |
| Local reflection probes | `UseLocalReflectionProbes` (OFF) | Global environment texture everywhere |
| Two-probe blending | Compiled in; engages only with 2 framed probes | 1 probe = frame-global SRV override (v1 route); 0 = global env; remainder weight always returns the global env |
| Probe prefilter | `GeneratePrefilter` per probe | The authored texture is sampled directly (mip clamp) |
| Probe CaptureLive | Component flag | Inert until the capture pass lands; authored texture path unchanged |
| Reference captures | Env `DX12_REFERENCE_CAPTURE_TEST` | No capture code runs in normal sessions |
| Tracy (evaluated, not landed) | `TRACY_ENABLE` build opt-in | Macros compile to nothing; CSV/GPU measurement unaffected |

## Acceptance scenario (Stage 9)

Enable ALL flags at once in the heaviest test scene, then:
resize, Alt-Tab, switch scenes, teleport the camera, and soak
10 minutes under VRAM pressure. Expected: no device loss, no
validation errors, every row above degrading independently —
turning any single flag off mid-session must be safe (flags are
read per frame; allocator/SPD switches take effect on resource
recreation).
