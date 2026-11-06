# Execution ledger — docs/superpowers/plans/2026-10-15-virtualgeometry-phase6.md

Execution: native, user approved 2026-10-15. Scope: Phase 6, then subsequent phases in order.

Ruling: work in the requested dev_main checkout — the user supplied this branch and selected implementation in this session; retain the current location and do not create another checkout.
Ruling: preserve working changes for review rather than automatically commit each task — .git is read-only in this workspace permission profile; implementation does not depend on commits.

Contracts reviewed: Task 1 asset-relative group selection -> Task 4 GPU group decisions; Task 2 packing -> Task 3 upload -> Task 4/5 resource consumption; Task 4 native success -> Task 5 exclusive fallback; Task 5 settings/selected records -> Task 6 diagnostics. HierarchyIndexOffset must be included in the GPU asset table. Cone apex must remain available.

Task 1: selector implemented; Debug/Release CPU suite passed. Existing validator already checks level/link monotonicity, so no duplicate validator change.
Task 2: packing implemented; Debug/Release tests passed for native bytes, padding, offsets and rejection.
Task 3: shared registration and global upload implemented; registration CPU tests passed. Native resources integrated in MeshShaderRenderer Prepare.
Tasks 4–6: implementation connected; first complete C++/HLSL Debug build passed. Runtime verification pending.
Task 7: in progress.

Ruling: use a persistent per-instance workgroup queue initially — group demand closure requires evaluating group metadata before root traversal, and this avoids CPU dispatch per hierarchy level. Geometry traversal uses roots and deduplicated group expansion. Metadata closure currently scans all groups per level; performance optimization remains necessary before claiming pixel-proportional metadata cost.
Ruling: native raster PSOs and draw integrate directly into the existing MeshShaderRenderer — reuse its GBuffer/velocity root and pixel shader contracts instead of introducing a duplicate renderer. Native resources and traversal retain dedicated files.
Ruling: the general rendering validation exposed pre-existing unqualified clamp calls in solarcycle.h — qualify only those three calls with std:: to allow the existing standalone simulation tests to compile; preserve behavior.
Evidence: test-shader-build-targets failed with the expected missing independently tracked outputs, then passed after adding five incremental targets. Native mesh shader's missing GPUInstanceData include and reserved triangle identifier were corrected from compiler diagnostics.

Evidence: RTX 3050 production shader/reference comparison passed 22 cases, including a 1024-leaf hierarchy. Production VirtualGeometryMS rasterized a Level 3 parent with nonzero byte/vertex offsets and verified its yellow output by texture readback.
Evidence: full Release CPU suite passed including the million-triangle stress case (26.57 seconds). General rendering smoke hit a DXR/DLSS GPU timeout both with native enabled and with DX12_VIRTUAL_GEOMETRY_ENABLED=0; isolate native acceptance from that feature-toggle sequence.
Ruling: the legacy suite's sole remaining assertion expected Auto normal-mapped models to be rejected, while existing SupportsEntity and DeferredPS both support normal mapping — update that assertion to match current supported behavior; do not regress production material support to satisfy an obsolete expectation.
Ruling: allocate frame scratch from actual asset size times object capacity, capped by configured budgets — avoid unconditional maximum buffers on small scenes. Bound dependency propagation to the asset's actual hierarchy height encoded in asset flags.

Continuation 2026-10-15: resumed Task 7 from the working tree, without repeating implemented Tasks 1–6.
Evidence: fresh production GPU suite passed 28 reference/routing/culling cases and Level 3 native raster readback on NVIDIA GeForce RTX 3050 Laptop GPU.
Evidence: fresh Debug and Release 24-frame native acceptance passed near/far/return selection, mixed levels, forced parent/leaf, and exact parent/mixed raster coverage versus leaves. Debug D3D12 error count was zero.
Evidence: fresh Debug/Release non-million CPU tests and incremental shader dependency tests passed.
Review: fresh read-only whole-change reviewer found no critical/important correctness issues in offsets, reservations, group traversal, frame fences or exclusive routing; full-scene material/velocity visual coverage remains a limitation.
Ruling: extend acceptance from 24 to 36 frames to compare native leaf GBuffer with traditional rendering and verify an unsupported Manual Unlit material still writes center depth. These cases close material/fallback evidence gaps; they do not change production rendering.
Evidence: extended 36-frame Debug acceptance passed. Native/traditional base color, normal, material and emission/AO comparison checked 4,320,000 texel bytes with zero differences beyond tolerance. Unsupported Manual Unlit correctly used traditional rendering and wrote center depth. D3D12 errors and acceptance failures were zero.
Evidence: full fresh CPU validation passed Debug and Release, including the Release million-triangle stress test.
Evidence: extended 36-frame Release acceptance also passed, including zero differing material-reference bytes and visible unsupported-material fallback depth. No Release debug-layer claim is made.
Task 7: all ten core Phase 6 criteria now have tested evidence in docs/virtualgeometry-phase6-validation.md. Broader imported-material seams and moving-native velocity comparison remain unperformed; generic DXR/DLSS feature smoke timeout remains unresolved. Do not interpret the isolated acceptance as an all-feature rendering validation pass.
Review follow-up: no important regression in 36-frame continuation; reviewer identified a possible blank-image equality false positive. Added visible center-depth assertions to both native-reference and traditional-comparison frames, reusing the same helper for unsupported fallback. Reverification follows. Material comparison covers untextured PBR only.
Final evidence: after the blank-image guard fix, rebuilt and reran both Debug and Release 36-frame suites. Both passed; both native and traditional reference frames have visible center depth, and the GBuffer comparison again reported zero differing bytes out of 4,320,000. Debug D3D12 errors remained zero. No production rendering change was needed during this continuation.
Ruling: keep current dev_main checkout and uncommitted changes as already recorded; do not prompt again for integration or create/push/merge a branch. Phase 7 streaming remains the next implementation phase, not an implemented feature.
