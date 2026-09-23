# DLSS SDK Setup

## Download
**Source**: [NVIDIA Developer - DLSS SDK](https://developer.nvidia.com/nvidia-dlss-sdk)

**Required Version**: **DLSS 2.x** (latest 2.x version)
- RTX 3050 Laptop **does NOT support DLSS 3 Frame Generation** (requires RTX 40 series Optical Flow Accelerator)
- DLSS 2.x Super Resolution IS supported

## Files to Place

### `include/`
```
nvsdk_ngx.h
nvsdk_ngx_helpers.h
nvsdk_ngx_params.h
```

### `lib/x64/`
```
nvsdk_ngx.lib          (Release)
nvsdk_ngx_d.lib        (Debug - if available)
```

### `dll/x64/`
```
nvsdk_ngx.dll          (Release)
nvsdk_ngx_d.dll        (Debug - if available)
```

## Notes
- DLSS SDK requires NVIDIA Developer Program registration
- The SDK includes both headers and prebuilt binaries
- For Debug builds, you may need to rename `nvsdk_ngx.lib` to `nvsdk_ngx_d.lib` or link against release lib in debug mode
- Minimum driver version: 470.00+