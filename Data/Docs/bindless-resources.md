# Bindless material resources

SM6.6 and Resource Binding Tier 3 enable `ResourceDescriptorHeap` automatically.
`DX12_DISABLE_BINDLESS=1` selects the existing descriptor-table path. The startup
log records hardware support, the selected path, and the texture heap base.

`MaterialTextureHeap.hlsli` resolves material texture indices directly into the
existing shader-visible CBV/SRV/UAV heap. GPU-driven albedo/normal sampling and
shared metallic/roughness, AO, emission, and POM sampling use this helper. This
includes visibility resolve, Meshlet, VirtualGeometry, and material previews.
Ordinary per-draw albedo/normal SRVs, lighting resources, and other passes retain
their existing bindings. The texture allocator still has 512 asset slots.

Material indices, streaming lifetimes, and fence-based resource retirement keep
their existing contracts. Regular materials carry their texture-region base in
`AssetParallax.w`; previews supply their own region. GPU-driven shaders receive
the engine texture-region base as a compile definition.

The DXC adapter selects SM6.6 and adds Bindless definitions only while the device
has selected this path. Compiler arguments distinguish the disk cache entries;
the in-memory shader cache also distinguishes Bindless and DXR variants. Main
scene and GPU-driven root signatures allow direct resource heap indexing, with
the resource heap bound before the root signature. Static samplers remain bound
through the root signature.

Verification uses the existing application's render benchmark and saved PNGs;
no new test source files are required.

## Hardware verification (2026-10-20)

Release x64 ran on an NVIDIA GeForce RTX 3050 Laptop GPU, with SM6.6 and Binding
Tier 3 reported at startup. The existing POM material fixture completed 12 frames
in each mode with zero failures. CPU and GPU-driven GBuffer captures matched
exactly both across rendering paths and across Bindless enabled/disabled modes.
Results are in `build/bindless-fixture/`, including logs, PNGs, and comparison JSON.

The existing render benchmark also completed in both modes for the saved city
and POM scenes (8 warmup + 8 measured frames per run). City captures had SSIM
0.99999847 and maximum 8-bit channel difference 6; saved POM scene captures
matched exactly. This short run establishes rendering parity, not performance.
Captures are in `bin/validation/render-benchmark/bindless-*-20261020/`.

The Release run did not have a D3D12 debug queue; GPU-based validation was not
performed. Editor settings and scene contents were preserved. The link step used
`PreferredToolArchitecture=x64` to avoid the installed x86-hosted PDB DLL mismatch.
