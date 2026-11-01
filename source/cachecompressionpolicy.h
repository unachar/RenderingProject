#pragma once

#include <windows.h>

namespace CacheCompressionPolicy
{
    inline bool PreferSmallerFiles()
    {
        // Balance measured size savings against decode cost; zero explicitly favors warm-load speed.
        char value[2]{};
        return !(GetEnvironmentVariableA("DX12_COMPRESS_CACHES", value, sizeof(value)) == 1 && value[0] == '0');
    }
}
