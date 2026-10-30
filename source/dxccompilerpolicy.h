#pragma once

#include <windows.h>
#include <d3dcompiler.h>
#include <string>
#include <vector>

namespace DxcCompilerPolicy
{
    inline bool KeepReflectionRequested()
    {
        wchar_t value[2]{};
        const DWORD length = GetEnvironmentVariableW(L"DX12_KEEP_SHADER_REFLECTION", value, 2);
        return length == 1 && value[0] == L'1';
    }

    inline bool ShouldStripReflection(unsigned flags, bool keepReflection)
    {
        return (flags & D3DCOMPILE_DEBUG) == 0 && !keepReflection;
    }

    inline void AppendReflectionArguments(std::vector<std::wstring>& arguments, unsigned flags,
                                          bool keepReflection)
    {
        if (ShouldStripReflection(flags, keepReflection))
        {
            // Keep executable DXIL, validation data, signatures and embedded root signatures.
            // CachePath hashes this argument, separating retained and stripped objects.
            arguments.emplace_back(L"-Qstrip_reflect");
        }
    }
}
