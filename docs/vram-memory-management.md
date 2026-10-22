# VRAM management

The renderer uses its existing tiled texture, render graph and DXGI facilities. No allocator dependency or test source files are added.

## Runtime policy

`source/rendering/gpumemorybudget.h` queries the adapter belonging to the D3D12 device. The local usage and OS budget are process values, rather than total machine GPU usage. Maintenance runs before recording frame commands.

The editor's GPU Memory controls and serialized project settings expose:

| Setting | Default | Meaning |
| --- | --- | --- |
| `memory.Enabled` | 1 | Enable residency budget and memory reclamation |
| `memory.TextureMiB` | 512 | Upper target for managed texture backing storage |
| `memory.TargetPercent` | 85 | Target percentage of the current OS local-memory budget |
| `memory.IdleTextureFrames` | 300 | Retire high-resolution mips after this many unused frames |
| `memory.UploadMiBPerFrame` | 8 | Streaming upload window in MiB; one indivisible larger mip is allowed when no upload is pending |
| `memory.ShadowResolution` | 1024 | Conventional shadow-map resolution, normalized to 1024 or 2048 |
| `memory.ReleaseInactive` | 1 | Release resources belonging to disabled rendering features |
| `memory.TransientAliasing` | 1 | Share non-overlapping temporary resources |

The effective texture target is the smaller of the configured texture target and the remaining process target after other local allocations. Coarse texture tails remain pinned. Non-streamable textures and other rendering allocations can exceed the target; this is not a hard cap on total process VRAM.

## Texture residency and compression

- Coarse tails stay resident; each promoted standard mip owns an independently releasable heap.
- Every 16 frames, unused mips and least recently touched high-resolution mips above the texture budget are unmapped.
- A 120-frame cooldown after eviction avoids immediate eviction/promotion oscillation. New textures are not subject to this cooldown.
- Global texture SRVs clamp sampling to the resident mip. Ray, glass and thumbnail descriptor paths use the same resident-mip information.
- Shared descriptor changes wait for previously submitted GPU work. Unmapped heaps and streaming upload buffers survive until the actual frame submission fence completes; elapsed frame counts are not used as proof of completion.
- A reserved resource retains its backing-storage owner through D3D12 private interface data. Borrowed resources remain valid when the manager publishes a compressed replacement.
- Completed BC cache entries are published in place, one texture per 60 frames. Both file textures and embedded model textures retain their compression cache key. Stable descriptor indices preserve material references.
- The reusable CPU upload-heap pool is capped at 32 MiB, including returns from batch loading. This reduces retained system memory on discrete GPUs, rather than being counted as a direct VRAM saving.

## Rendering allocations

- Reflection, GI, ReSTIR, radiance-cache, Lumen and inactive NRD/OptiX resources are released after GPU completion. The actual available denoiser backend determines which history is retained.
- Radiance cache allocation is deferred until the cache is used. Shadow and AO denoisers are created independently, only when their effect needs them. Disabling all DXR consumers releases scene BLAS/TLAS, frame resources and guides; refraction keeps the scene alive when required.
- Lumen screen probes shrink as well as grow when their grid changes.
- Conventional shadow maps use the selected resolution. Shadow array layers grow immediately and shrink after 240 frames of spare capacity, or immediately under memory pressure. A minimum one-layer resource keeps bindings valid. VSM retains its logical 2048 page grid. Reallocation invalidates page caches and rebinds the frame graph's imported resource.
- The opaque-scene refraction copy and pre-upscale AA target share backing storage when supported. Aliasing barriers activate the copy before its full copy and the AA target before clearing and rendering it. Scene output and temporal history keep independent storage.
- Existing histogram/lens-output aliasing is enabled by the memory policy, with committed-resource fallback. Releasing the arena also releases placed resources that refer to it.
- Froxel fog volumes are allocated only when fog is enabled. Inactive fog, bloom, half-resolution lens targets and stationary VSM cache are reclaimed.

Existing BC7/BC5/BC4 import compression, static BLAS compaction, quantized BLAS input and virtual geometry page budgeting remain in use. History buffers cannot be indiscriminately aliased across frames, and reducing their precision would require separate image-quality acceptance criteria.

## Verification

Use a Release build and the existing rendering/GI validation scripts. They exercise render resizing, feature fallback, GI off/on transitions and reflection coexistence. `init_log.txt` includes `VRAM:` records for process usage, texture residency, eviction, BC replacement, shadow allocation and alias savings.

For memory comparison, keep the scene, render extent and shadow resolution fixed and compare `memory.Enabled=0` against `1`. The existing rendering benchmark records `ProcessLocalUsageBytes` per frame. Memory savings and frame time are separate metrics; changing shadow resolution also changes quality. Generated logs and captures are validation outputs, not added test implementations.
