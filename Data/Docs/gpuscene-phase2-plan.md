# GPU Scene Phase 2 plan

Keep existing rendering and the uncommitted VirtualGeometry work. No commits.

1. Reuse ECS change boundaries, the existing three frame fences, Resource helpers and RenderGraph.
2. Define matching C++/HLSL row-major instance, mesh and material records with layout assertions.
3. Add shared resource registration, stable instance slots, free-list reuse and a deduplicated dirty queue.
4. Add DEFAULT-resident buffers and per-frame staging; grow geometrically and retire replaced resources through the existing frame ring.
5. Connect structural changes, transform calculation, material setters and editor history changes to an ECS bridge.
6. Add a resident-scene frustum compute pass, and let existing per-model GPU culling read the same scene records.
7. Order update/culling through RenderGraph, retain all existing draw paths, and add profiler/UI diagnostics.
8. Compile incrementally. Validate 1/1000/10000 slots, one-instance updates, temporal settle, ID reuse,
   buffer growth, GPU readback/culling and existing Debug/Release rendering smoke tests.

Direct mutable component references remain supported. Known engine mutation boundaries notify the cache;
new code holding an unchecked reference must explicitly notify after editing. No per-frame scene scan.
