#pragma once
#include <stdlib.h>
#ifdef __cplusplus
extern "C" {
#endif
void TextureProbeFailNextZstdAllocation(void);
int TextureProbeZstdFailurePending(void);
void* TextureProbeZstdMalloc(size_t size);
void* TextureProbeZstdCalloc(size_t count, size_t size);
void TextureProbeZstdFree(void* memory);
#ifdef __cplusplus
}
#endif

// Only the dedicated test build of zstd.c defines this; application/benchmark builds do not.
#ifdef TEXTURE_ZSTD_ALLOCATOR_SHIM
#define malloc TextureProbeZstdMalloc
#define calloc TextureProbeZstdCalloc
#define free TextureProbeZstdFree
#endif
