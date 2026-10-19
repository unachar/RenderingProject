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
        vector<uint8_t> Bytes;
    };

    bool ReadTexture(ID3D12Resource* texture, TextureData& output);
    bool ReadTextureMip(ID3D12Resource* texture, TextureData& output, UINT mip, D3D12_RESOURCE_STATES state);
    bool ReadTextureSlice(ID3D12Resource* texture, TextureData& output, UINT slice);
    bool ReadBuffer(ID3D12Resource* buffer, D3D12_RESOURCE_STATES state, UINT64 bytes, vector<uint8_t>& output);
    bool SaveScenePng(const wchar_t* path);
    bool SaveTexturePng(ID3D12Resource* resource, const wchar_t* path);
    float HalfToFloat(uint16_t value);
    UINT ReportGraphicsErrors();
} // 名前空間 RenderCapture
