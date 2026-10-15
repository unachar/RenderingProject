# Execution ledger — docs/superpowers/plans/2026-10-15-virtualgeometry-phase7.md

Execution: Native, approved 2026-10-15. Tasks 1–8 authorized; no intermediate approval gates.

Ruling: preserve the existing dev_main checkout and uncommitted changes; do not commit, push, or create another checkout, as specified by the approved plan.
Ruling: use this persistent ledger instead of shell-only SDD bookkeeping scripts; the Windows workspace and read-only .git require no commits and the ledger must retain evidence for uncommitted work.

Pre-flight: Task 1 PageManifest feeds Tasks 4/5; Page locations feed Tasks 2/6/7. Task 3 LoadTicket and fence ownership feed Tasks 4/5. Task 5 frame snapshots and GPU feedback feed Tasks 6/7. Task 7 statistics feed Task 8. Existing frame fence accessors are available; submission order must be verified during Task 5.

Tasks 1–7: implementation integrated; final regression verification in progress.

Continuation: user committed the earlier work externally (HEAD 532a37df); retain it and continue without resets.

Task 1/2/3/4 core evidence: Debug and Release standalone Page cache/resident selector/residency/SSD I/O tests passed, including deterministic byte packing, checksum rejection, truncated headers, all 16 shared-parent residency combinations, old completion generations, both-fence slot retirement, and cancellation of delayed worker I/O. New generated-cache and procedural stress checks are being added.
Task 5 evidence: RTX 3050 Laptop GPU production Copy Queue copied 65536 bytes with exact readback; publication before fence completion rejected; D3D12 debug errors zero.
Task 6/7 evidence: production traversal GPU harness passed 46 cases (18 streaming cases added, including partial residency, request overflow, absent root, nonzero Page base). Production Mesh Shader rasterized both contiguous and paged Level 3 parents with nonzero byte offsets.
Task 7 regression evidence: fresh Debug runtime passed all 36 acceptance stages after streaming warmup, exact native/traditional GBuffer comparison, material fallback, and zero D3D12 errors. Startup window extended to 180 frames to permit asynchronous prewarming; actual test stages remain 36 and their completion is explicitly asserted.

Ruling: preserve the 48-byte GPU asset layout; streaming flag bit 1 changes VertexOffset/TriangleOffset to PageBase/PageCount. Legacy GPU harness cases still use the old contiguous interpretation.
Ruling: replace a physical Page resource after safe retirement rather than copying into an old shader-read buffer. Each frame declares resident Page reads and returns their buffers to COMMON. Both queues' lifetime checks still gate descriptor/resource replacement.
Ruling: initial native cache preparation runs in at most two worker futures; render-thread registration itself performs no Page disk I/O. Manifest-only stress assets bypass full CPU hierarchy/leaf geometry retention.
Ruling: account actual committed Page resources separately from pending reservations, metadata, upload heap and staging; free Page resources only after both fences complete. Use usage feedback for intermediate hierarchy dependencies as well as rasterized Pages to prevent avoidable eviction/refinement cycles.

Task 8: implementing disk-generated, sharded planar scenes. Each asset contains 32768 leaf clusters × 50 triangles = 1,638,400 source triangles, with a complete 8-way parent hierarchy. Distinct disk assets have distinct Page identities; source counts and instance counts are reported separately. Payload generation uses a bounded 64 KiB buffer, not a multi-GB RAM copy. A VRAM metadata admission check precedes billion-triangle attempts to avoid allocating resident hierarchy buffers beyond device capacity.
Task 8 preliminary runs passed at 31,129,600 and 101,580,800 source triangles. These earlier runs enabled per-frame debug readback and therefore do not establish overlapping graphics execution. Final runs explicitly disable it and assert graphics work remains in flight.

Independent final review completed (read-only phase7_review). No critical findings. Addressed: preflight pinned admission before metadata append, rollback rejected registrations, cached reservation/pinned counts and stop full-budget request scans, queue up to 128 SSD jobs across four workers, publication grace before eviction, and metadata accounting including feedback/readback buffers. Normal imported assets still retain their existing CPU hierarchy/geometry for compatibility; manifest-only stress assets do not.

Additional verification: hardware Copy Queue test now holds a graphics fence incomplete, proves the old slot/resource cannot be reused, releases it, and checks replacement bytes plus stale generation rejection. Moving velocity output is tested for both contiguous and paged native geometry. Runtime samples inspect selected cluster uniqueness, ancestor/descendant exclusion and native/legacy/fallback routing. Repeated rejected registrations must leave all aggregate counts unchanged.

Active final validation: Release 100m runtime, full Debug/Release CPU suite including million-triangle stress. Copy/retirement GPU test passed with zero D3D12 errors. Shader dependency check passed after accounting for the existing two-output ShadowCache shader target.

Final evidence: full CPU Debug 1/1 and Release 2/2 passed (including million). Fresh production GPU 46 traversal cases plus paged/contiguous parent raster and moving velocity passed. Release 101,580,800 source-triangle runtime passed 240 frames with 18 coverage/routing samples, 194 evictions, 24,733 GPU requests, 20,774,912 Copy bytes and one injected/recovered I/O failure. Ordinary frames retained pending graphics work; synchronous counters were disabled. Rejected registrations left aggregate counts unchanged.

Billion-triangle attempt: hardware admission returned 78 / SKIP, metadata 3,623,931,904 bytes plus reserve needs 4,469,280,768, available DXGI VRAM budget 3,533,963,264. No billion geometry generation/render verification was attempted after admission failed. Reproducible 100m generated files were removed after successful validation (4,735,315,410 bytes); logs remain.

Final Debug 31,129,600 source-triangle runtime passed: 240 frames, 15 coverage/routing samples, 190 evictions, 24,931 GPU requests, 18,481,152 SSD/Copy bytes, one injected/recovered I/O failure, D3D12 errors 0, pending graphics work observed. The script removed its generated 30m files and restored settings. Tasks 1–7 implemented and verified; Task 8 completed through 100m and billion admission, with billion rendering explicitly unverified due hardware capacity. See `docs/virtualgeometry-phase7-validation.md` for evidence, limitations and resume commands.
