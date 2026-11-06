# ACL FBX integration

Goal: Implement actual ACL compression/decompression and compare against current FBX playback, preserving its stepped key-index semantics. VMD Bezier/IK/morph playback remains a distinct implementation task.

Design: A move-only, owning FbxAclClip uses SDK transform tracks, all sampled at integer key indices. Constant/short repeating key arrays are expanded only if their lengths divide the longest array; otherwise the clip is rejected for legacy fallback. Scale remains unit scale, matching current playback. Clamp ACL looping and perform legacy time wrapping explicitly. Validate finite transforms, limits, compressed size, and every decoded sample's local translation/rotation error before accepting a clip. Keep all SDK implementation types in one translation unit.

Production: opt-in DX12_USE_ACL_ANIMATION=1 selects an in-memory compressed clip per aiAnimation, with independent pose scratch buffers for blending two times of the same clip. Clear caches before imported scenes are released/replaced. Keep source arrays for fallback and existing ownership contracts: compressed payload savings are not total runtime memory savings. Do not enable by default without a favorable measured tradeoff. Existing VMD placeholder must not be used as a real codec.

- [x] Add failing tests covering nonidentity poses, differing key counts, wrap/negative/fractional times, two-time sampling, invalid inputs and ownership.
- [x] Implement SDK codec, validate errors and byte counts, run synthetic tests.
- [x] Benchmark real FBX clips with identical stepped reference semantics; record compression time, payload size and sampling cost.
- [x] Integrate opt-in resource playback and lifetime invalidation; build and run relevant existing animation tests.
- [x] Review, document evidence and default decision. Remaining installed libraries and VMD ACL support remain open.

Evidence and outstanding adoption conditions: docs/acl-fbx-comparison-2026-11-01.md. Default remains raw; this is not a claim that compressed storage or VMD integration is complete.
