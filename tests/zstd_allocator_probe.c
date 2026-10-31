#include "zstd_allocator_probe.h"
#include <windows.h>
static volatile LONG failNextAllocation = 0;
void TextureProbeFailNextZstdAllocation(void) { InterlockedExchange(&failNextAllocation, 1); }
int TextureProbeZstdFailurePending(void) { return InterlockedCompareExchange(&failNextAllocation, 0, 0) != 0; }
void* TextureProbeZstdMalloc(size_t size)
{
    return InterlockedExchange(&failNextAllocation, 0) ? NULL : malloc(size);
}
void* TextureProbeZstdCalloc(size_t count, size_t size)
{
    return InterlockedExchange(&failNextAllocation, 0) ? NULL : calloc(count, size);
}
void TextureProbeZstdFree(void* memory) { free(memory); }
