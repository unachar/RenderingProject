#pragma once
#include "pch.h"

namespace RenderCapture
{
struct TextureData
{
    UINT Width = 0;
    UINT Height = 0;
    UINT RowPitch = 0;
    DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
    std::vector<uint8_t> Bytes;
};

bool ReadTexture(ID3D12Resource* texture, TextureData& output);
bool ReadBuffer(ID3D12Resource* buffer, D3D12_RESOURCE_STATES state, UINT64 bytes,
    std::vector<uint8_t>& output);
bool SaveScenePng(const wchar_t* path);
float HalfToFloat(uint16_t value);
UINT ReportGraphicsErrors();
}
