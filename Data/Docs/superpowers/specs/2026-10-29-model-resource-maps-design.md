# Shared model maps and VMD humanoid binding

Implement on dev_main, as requested. Existing VMD binding only finds identical bone names; Moca is also loaded as a static model.

Use a CPU-only ModelResourceMap owned by each model resource. A named category contains an unordered_map<string, vector<uint32_t>>; built-in categories are bone, mesh, material, mesh-material, humanoid and vmd. Indices belong to the loaded model, never process pointers. Duplicate/empty names remain queryable. Additional categories need no binary format change. Bone indices match the animation skinning palette; mesh indices match GetMeshData; material indices match Assimp materials. mesh-material maps a decimal material index to drawable mesh indices.

Serialize sorted entries in a bounded, versioned little-endian .modelmap file. Reconstruct unordered_maps after loading. The cache is validated against a deterministic fingerprint of the current imported layout, including conversion mode; invalid/stale caches are replaced and cannot change runtime index order. Persistence is best effort and never makes a valid model unloadable. Lookup is average O(1); import and deserialization remain linear.

For non-PMX models, build humanoid bindings from VRM 0/1 metadata or unambiguous Mixamo/common bone aliases. VMD Japanese names map to target humanoid bones; exact names win. Expose an override API. Resolve tracks when building the existing runtime cache, keeping string searches out of per-frame sampling. Retarget rotation through the target global bind orientation and scale motion translations into parent space; PMX retains its existing math/IK/morph path. Center motion supplements humanoid hips. Unknown rigs require explicit mappings. Generic PMX IK/append chains and PMX morphs are not fabricated for FBX/VRM; FK track playback is supported, with diagnostics for unmatched tracks.

Expose maps through model resources and ModelManager so material tools can query without importer or GPU ownership. Cover static imported models as well, including cache/derived geometry paths. Keep existing model test scene defaults; add a documented opt-in VMD playback path for XBot and Moca.

Alternatives: importer-private maps prevent editor reuse; serializing unordered_map memory is invalid; global name maps collide across models. Resource-owned indices and portable records avoid these problems.

Validation: portable C++ tests for map round trips, deterministic bytes, duplicates, malformed/stale data, category extension, alias precedence, rest-pose math, scale and layered binding. Windows/DX12 visual playback cannot be run in the Linux worker and must be reported separately.
