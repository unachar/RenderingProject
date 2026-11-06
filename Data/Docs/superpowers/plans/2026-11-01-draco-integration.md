# Draco integration

Implement the installed Draco SDK in the model import adapter. Decode KHR_draco_mesh_compression using extension unique attribute IDs and mapped point attributes; retain declared component types/normalization and triangle topology. Expand into independent owning accessors before existing fastgltf conversion and Assimp postprocessing. Reject invalid mappings/counts/buffer ranges rather than invent missing data. Keep the SDK in the existing adapter DLL so application Debug STL remains isolated.

- [x] Build pinned SDK and add disabled-path/runtime/license handling.
- [x] Add failing real-asset and malformed-input tests, implement glTF mesh decoding, and validate geometry/attributes/ownership through the production route.
- [x] Compare full compressed/uncompressed asset size, load time and decoded quality; compare codec encoding/storage choices with existing mesh compression where equivalent.
- [x] Validate Debug/Release/disabled consumers, product build and actual rendering; document adoption and unresolved limits.

Do not automatically replace existing geometry caches or quantize original assets. Codec payload, complete glTF size, CPU import, GPU memory and visible quality are separate measurements. A successfully decoded compressed sample alone does not prove a cache replacement is suitable.

Results: `docs/draco-comparison-2026-11-01.md`. Tests include real compressed Box, SDK-encoded UV/color/uint8 skin mappings with nonsequential unique IDs, malformed inputs, SDK-disabled and ordinary Debug consumers. The resident codec comparison uses one 16,641-vertex synthetic surface, and full import uses the paired Box files. These do not prove general real-asset compression or visible quality. Final product build and 3-frame runtime smoke pass.

Ruling: prefer the new backend for supported glTF inputs by default, with Assimp fallback for unsupported features. A valid omitted inverse-bind field crashes the bundled Assimp; the new backend correctly supplies identity. Default selection has a red/green test and live smoke includes that plain glTF input. Existing non-glTF formats and explicit `DX12_USE_FASTGLTF=0` remain on native imports. Large animation import still measures slower in the new backend and needs optimization; it is not described as a universal speed improvement.

Draco input is adopted. Standalone .drc, product cooking/export, broader mixed-attribute/extension inputs, memory and visual quantization comparisons remain follow-up work within the active External integration goal.
