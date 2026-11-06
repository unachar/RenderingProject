# Phase 6: GPU hierarchy rendering

Target: `unachar/RenderingProject`, `dev_main`.

## Outcome and scope

Render the existing VirtualGeometry hierarchy through native Mesh Shaders. Select detail per cluster region using projected error while switching each simplification group's complete parent/child representation together. Preserve the builder, cache format v1, legacy meshlets, traditional rendering, and selectable discrete LOD rendering.

Phase 7 streaming and phases 8–11 remain subsequent work. Phase 6 uploads all geometry; page fields reserve their future contract but do not imply streaming support. Unsupported settings must be disabled or clearly identified in the editor.

## Existing contracts confirmed

`VirtualGeometryAsset` already owns vertices, byte triangle indices, clusters, groups, hierarchy indices and root clusters. `GroupIndex` identifies the consuming group; `GeneratingGroupIndex` identifies the group that produced a parent cluster. Group child/parent ranges refer to `HierarchyIndices`, not contiguous cluster records. `VirtualGeometryVertex` is 48 bytes and includes position, normal, UV and diffuse color. Runtime `ConvertLeaves` exposes only leaf geometry through `Meshlets::Asset`.

## GPU representation and registration

Add global cluster, group, hierarchy-index, root, vertex, triangle and asset buffers. Register shared runtime assets once, retain stable asset indices, and publish readiness only after valid resources exist. Keep asset-relative indices explicit; resolve them through asset offsets in shaders. Triangle offsets are byte offsets into a padded byte-address buffer. Validate buffer arithmetic, range limits and C++/HLSL layout agreement.

Upload all levels without mapping simplified vertices back to source vertices. Preserve cone apex as well as axis/cutoff, or use a proven conservative sphere/cone test; the proposed axis/cutoff-only GPU structure must not silently discard information required by existing culling. Nonuniform, reflected or singular transforms need conservative handling.

## Selection and traversal

Seed LOD0 VirtualGeometry assets from eligible GPU Scene instances. Bypass mesh-wide discrete LOD selection only for the native path. Use ping-pong queues and bounded level dispatches initially, with GPU-generated work counts and indirect dispatch/draw arguments.

Treat the hierarchy as a shared group graph, not independent root-to-leaf trees. A group can generate multiple parents which can later belong to different consuming groups. Independent per-parent descent would duplicate children or mix parent and child coverage. Resolve group decisions across all incoming representations before replacing a complete parent set with its child set. Deduplicate group expansion per instance and ensure the selected cut covers source geometry exactly once. This invariant must be established in a CPU reference selector and graph tests before the GPU algorithm is finalized.

Use conservative group bounds and accumulated group error for replacement decisions. Project error using viewport height and projection scale, conservative transformed error, and distance to bounds with a near-plane guard. Use a documented pixel convention consistently. Leaves terminate refinement. Force Level requests a valid group-consistent cut; irregular terminal branches may stop earlier than the requested level.

Frustum and previous Hi-Z tests may reject a subtree only with conservative bounds and valid temporal projection. Cone culling applies only where the stored cone actually bounds the tested geometry; do not derive subtree cones from a single simplified parent. Invalid temporal data disables occlusion for that frame.

Queue/output overflow must be detected. Capacity must support the validated worst-case cut or route the entire affected instance to fallback without rendering a partial native mesh. Never silently drop work or render both native and legacy representations.

## Raster integration and debugging

`VisibleVirtualCluster` supplies instance, cluster and material indices. Native Mesh Shaders decode the original 48-byte vertices and three-byte triangles, apply existing instance/material conventions, and preserve GBuffer and velocity behavior. Keep `MeshletMS` and traditional pipelines as fallbacks. Native eligibility initially allows supported static opaque PBR/Toon geometry with no skinning or arbitrary deformation.

Expose enabled, target pixel error, force level, culling toggles and actual selected-level coloring. Add selected cluster/triangle counts, overflow and fallback diagnostics, plus traversal and native raster profiler events. Do not present hierarchy bounds preview as evidence of selected geometry rendering.

## Validation and acceptance

CPU tests cover shared group graphs, mixed-depth cuts, duplicate prevention, complete replacement, projected error, forced levels, invalid indices and capacity failure. Validate upload packing and byte decoding, compile all affected C++ and HLSL configurations, and test fallback selection.

On supported Mesh Shader hardware, inspect near/far and mixed-distance scenes: nonleaf geometry is actually rasterized, detail responds to camera movement, selected colors match output levels, and transitions show no visible cracks. Check traditional and legacy/discrete modes and velocity rendering. Record hardware and executed checks; compilation alone cannot establish visual acceptance. Phase 6 is complete only when all ten user-specified acceptance criteria have evidence.
