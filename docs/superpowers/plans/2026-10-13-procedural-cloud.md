# Procedural cloud MVP implementation plan

Implement the supplied RenderingProject/dev_main specification in the existing D3D12 renderer. Preserve unrelated working-tree changes. Execute sequentially in this workspace.

Architecture: `CloudRenderer` owns cloud resources and pipelines, uses the engine SRV/UAV and RTV heaps, existing postprocess vertex shader, camera constant buffer and light constant buffer. Draw after deferred lighting and before postprocess. RGB is scattering; alpha is transmittance. Composite uses ONE/SRC_ALPHA blending into the HDR scene without sampling that render target.

- [ ] Add settings validation and quality presets; verify invalid inputs and clear/zero-density behavior.
- [ ] Generate periodic Perlin-Worley base (128 cubed) and Worley detail (32 cubed) textures with compute shaders; regenerate only for changed seed or explicit request.
- [ ] Add half/full-resolution per-flight-frame color and depth render targets, fullscreen raymarch and bilateral composite. Handle inside/outside/parallel slab rays, scene depth, bounded tracing and early termination.
- [ ] Bind shared camera/light buffers, Beer-Lambert attenuation, HG phase, ambient and optional powder. Add UI and existing profiler scopes.
- [ ] Integrate initialization, resize, shutdown and shader compilation. Build Debug and Release.
- [ ] Add GPU validation scenarios for parameter extremes, camera movement, seed repeatability, resize, quality and light changes. Capture output; report actual validation and performance limits.

No temporal history, weather map, ground shadow or extra cloud layers. Default cloud rendering is opt-in to preserve existing scenes. Resources use explicit transitions and per-frame descriptors. Geometry-edge mismatches fall back to full-resolution evaluation where bilateral neighbors cannot represent the pixel.
