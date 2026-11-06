# Resolution-independent noise materials

Approved in conversation: save procedural definitions as `.noise`, evaluate them on the GPU at shading coordinates, accept asset drag/drop into Base Color, Normal and Height (POM), and retain ordinary texture support per slot. Height/POM remains the existing shared slot. The user approved proceeding with this implementation.

## Approach
Store versioned JSON parameters, not pixels. Reuse the studio's GPU scalar/color evaluator from a resource-free HLSL include. Carry three optional parameter records in material constants and GPU scene records. Ordinary texture handles and `.noise` handles use the existing material slot GUID serialization, so mixed materials and old material files keep working. This is preferred over higher resolution baking (still resolution-limited) or generated per-material shaders (unnecessary pipeline churn).

## Behavior and constraints
- Save/load all noise and pattern parameters, seed, colors, tiling and snapshot time. Materials do not implicitly animate saved definitions.
- Base color evaluates color; Normal evaluates height derivatives; Height evaluates the scalar field for POM, depth offset and self shadow. The slot determines interpretation regardless of the studio preview output selector.
- No generated texture is used as the material source. Preview thumbnails may rasterize a small image.
- Support the existing GPU texture kinds and every current noise basis/fractal type. Mandelbrot/Julia retain their existing CPU/image export and show a clear unsupported `.noise` explanation.
- Use stable, resolution-independent UV derivatives with pixel footprints for normals. Preserve existing opaque-only POM constraints and normal-map orientation.
- Failed/missing/invalid definitions disable only the affected source, report the error, and do not mutate saved handles. Replacements, moves, reverts and reloads must keep source types correct.
- Cover normal raster, forward, deferred, GPU-driven/visibility paths, material/asset previews, and secondary ray reflection material evaluation.
- Windows/D3D12 application; portable serialization/math tests and DXC shader compilation can run on Linux. Full Windows runtime validation must be reported separately if unavailable.
