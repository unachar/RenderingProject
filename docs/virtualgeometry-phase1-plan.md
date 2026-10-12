# Virtual Geometry Phase 1 implementation plan

Goal: build a deterministic CPU geometry asset from dev_main LOD0 without changing rendering.
The supplied Phase 1 specification is the implementation brief. Work stays local; no commits.
The user subsequently confirmed this is the local environment and authorized build/executable tests.

1. Inspect StaticModel import, rendering isolation, and bundled meshoptimizer signatures.
2. Add portable flat types/config and retain CPU indices after import optimization.
3. Build leaf meshlets and bounds; implement geometry/range validation.
4. Partition by positional adjacency; merge attributes without merging seams; lock external and non-manifold edges.
5. Simplify with attributes, verify protected edge multisets, recluster, propagate absolute error, and stop on no progress.
6. Add bounded versioned little-endian serialization with checksum and full structural validation.
7. Add statistics/report, CPU-only regression/stress suite and Windows adapter; register required meshoptimizer sources.
8. Review source/project registration; run Debug/Release CPU tests, million-triangle stress/round-trip,
   and the existing rendering validation script for both configurations.

Review focus: seams, mixed terminal root levels, hostile file counts/offsets, non-manifold edges,
overflow/large-coordinate input, non-reducing groups and deterministic output excluding timing.
