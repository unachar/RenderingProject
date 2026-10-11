# NVIDIA Real-Time Denoiser (NRD) Setup

## Download / Build
**Source**: [GitHub - NVIDIA/Real-Time-Denoiser](https://github.com/NVIDIA/Real-Time-Denoiser)

### Option 1: Build from Source (Recommended)
```bash
git clone https://github.com/NVIDIA/Real-Time-Denoiser.git
cd Real-Time-Denoiser
git submodule update --init --recursive
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64 -DNRD_BACKEND_API=D3D12
cmake --build . --config Release
cmake --build . --config Debug
```

### Option 2: Download Prebuilt (if available)
Check Releases page for prebuilt binaries.

## Files to Place

### `include/`
```
NRD.h
NRDIntegration.h
NRDDescs.h
```

### `lib/x64/`
```
nrd.lib              (Release)
nrd_d.lib            (Debug)
```

### `dll/x64/`
```
nrd.dll              (Release)
nrd_d.dll            (Debug)
```

## Integration Notes
- NRD supports D3D12 backend (set `NRD_BACKEND_API=D3D12` at build time)
- Requires shader compilation for denoising passes (included in repo)
- Works with DLSS output (temporal + spatial denoising)
- Key denoisers for RT:
  - `NRD_DENOISER_REBLUR` - for diffuse/specular (ray traced)
  - `NRD_DENOISER_SIGMA` - for shadows
  - `NRD_DENOISER_RELAX` - for specular reflections

## Dependencies
- DirectX 12 (Agility SDK or Windows 10 SDK 10.0.19041+)
- SPIRV-Cross (included as submodule)
- CMake 3.20+

## License
MIT License - suitable for commercial use