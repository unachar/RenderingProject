#pragma once
#include <windows.h>
#include <d3d12.h>
#include <cstring>

#if defined(_DEBUG) && (defined(_M_X64) || defined(_M_ARM64))
#define RENDERINGPROJECT_PIX_GPU_MARKERS 1
#include "../../External/PIXEvents/include/pix3.h"
#else
#define RENDERINGPROJECT_PIX_GPU_MARKERS 0
#endif

namespace GpuMarkers
{
#if RENDERINGPROJECT_PIX_GPU_MARKERS
    struct Record
    {
        UINT64 Words[PixEventsLegacy::PIXEventsGraphicsRecordSpaceQwords]{};
        UINT Bytes = 0;
    };

    inline Record Encode(const char* name)
    {
        Record record;
        if (!name || !name[0])
        {
            return record;
        }
        // Use the pinned GPU-only encoder. The public PIX API also emits CPU events
        // on Windows and would require an additional runtime DLL.
        const UINT64* end = PixEventsLegacy::EncodeSetMarkerForContext(record.Words, UINT64(0), name);
        record.Bytes = static_cast<UINT>((end - record.Words) * sizeof(UINT64));
        return record;
    }
#endif

    inline void Write(ID3D12GraphicsCommandList* commands, const char* name)
    {
        if (!commands || !name || !name[0])
        {
            return;
        }
#if RENDERINGPROJECT_PIX_GPU_MARKERS
        const Record record = Encode(name);
        commands->SetMarker(D3D12_EVENT_METADATA, record.Words, record.Bytes);
#else
        // Basic annotations: metadata 1 is ANSI, and the size includes the NUL.
        commands->SetMarker(1, name, static_cast<UINT>(std::strlen(name) + 1));
#endif
    }
}

#undef RENDERINGPROJECT_PIX_GPU_MARKERS
