#pragma once
#include <malloc.h>
#include <cstddef>

namespace AclAllocationProbe
{
    inline thread_local int FailAfter = -1;
    inline thread_local bool Consumed = false;
    inline thread_local int LiveBlocks = 0;

    inline void* Allocate(size_t bytes, size_t alignment)
    {
        if (FailAfter == 0)
        {
            FailAfter = -1;
            Consumed = true;
            return nullptr;
        }
        if (FailAfter > 0)
        {
            --FailAfter;
        }
        void* memory = _aligned_malloc(bytes, alignment);
        if (memory)
        {
            ++LiveBlocks;
        }
        return memory;
    }

    inline void Free(void* memory)
    {
        if (memory)
        {
            --LiveBlocks;
        }
        _aligned_free(memory);
    }
}

// Used only by the dedicated failure-injection build, after CRT declarations are included.
#define _aligned_malloc AclAllocationProbe::Allocate
#define _aligned_free AclAllocationProbe::Free
