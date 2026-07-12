# DXC integration

The x64 renderer now restores and links the Microsoft DirectX Shader Compiler through the native NuGet package:

- Package: `Microsoft.Direct3D.DXC`
- Version: `1.9.2602.24`
- API: `IDxcUtils` + `IDxcCompiler3`
- Runtime output: DXIL
- Current runtime target promotion: legacy `*_5_x` profiles become matching `*_6_0` profiles

## Build flow

1. Visual Studio restores `packages.config`.
2. `Directory.Build.props` imports `Microsoft.Direct3D.DXC.props` for x64 builds.
3. The project explicitly links `dxcompiler.lib`.
4. `Directory.Build.targets` imports `Microsoft.Direct3D.DXC.targets`.
5. The package targets deploy the matching DXC runtime files to `$(OutDir)`.
6. `source/pch.h` routes runtime `D3DCompileFromFile` calls to `DxcCompileFromFileCompat`.
7. `source/dxccompiler.h` invokes `DxcCreateInstance`, compiles with `IDxcCompiler3`, and returns the DXIL container through the existing `ID3DBlob` interface.

## Compatibility

- Existing Visual Studio `FxCompile` items remain on FXC/SM5.x for now.
- Runtime shader compilation uses DXC on x64.
- Win32 configurations keep the legacy FXC path because the native DXC package is configured for the project's x64 renderer path.
- Existing root signatures and PSO creation code do not need to change because the adapter copies the DXIL object into an `ID3DBlob`.

## First build

Use **Restore NuGet Packages**, then build `Debug | x64` or `Release | x64`.

The output directory should contain `dxcompiler.dll` after a successful build. The exact companion files are controlled by the selected DXC NuGet package version.

## Current migrated shader

`shader/hlsl/GpuDrivenCullCS.hlsl` is compiled at runtime. Its existing `cs_5_1` request is promoted by the compatibility adapter to `cs_6_0`, so the GPU-driven culling pipeline now consumes DXIL.

## Next migration step

Build-time HLSL files can be moved one-by-one from Visual Studio `FxCompile` to DXC custom-build items. Mesh and amplification shaders should use `ms_6_5` and `as_6_5` after runtime feature checks confirm Shader Model 6.5 and Mesh Shader Tier support.
