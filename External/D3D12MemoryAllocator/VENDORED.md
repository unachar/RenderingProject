# Vendored: D3D12MemoryAllocator

- Upstream: https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator
- Commit: 1753823e86d274ae1a2a55e6b9f9d206c3c30945 (2026-10-26, master)
- License: MIT (see LICENSE.txt, NOTICES.txt)
- Files taken verbatim: include/D3D12MemAlloc.h, src/D3D12MemAlloc.cpp
  (placed side by side; the .cpp includes the header by bare name).
- Local modifications: none.
- Integration: used only through source/rendering/memoryallocator.{h,cpp},
  behind RendererSettings::UseD3D12MemoryAllocator (default off). The
  existing transient arena is intentionally not routed through D3D12MA.
