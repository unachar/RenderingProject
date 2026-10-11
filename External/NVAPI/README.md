# NVAPI SDK Setup

## Download
**Source**: [NVIDIA Developer - NVAPI SDK](https://developer.nvidia.com/nvapi)

## Files to Place

### `include/`
```
nvapi.h
nvapi_lite.h
nvapi_lite_common.h
```

### `lib/x64/`
```
nvapi64.lib          (Release)
nvapi64_d.lib        (Debug - if available)
```

### `dll/x64/`
```
nvapi64.dll          (Release - usually in System32, but copy for local deployment)
nvapi64_d.dll        (Debug - if available)
```

## Notes
- NVAPI is used for GPU feature detection, DLSS initialization, and display configuration
- `nvapi64.dll` is typically installed with NVIDIA drivers in System32
- For local deployment, copy the DLL to output directory
- Required for querying DLSS support and optimal settings