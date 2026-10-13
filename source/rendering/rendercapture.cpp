#include "pch.h"
#include "rendercapture.h"
#include "graphicsdevice.h"
#include "graphicslog.h"
#include "rendertargets.h"
#include <d3d12sdklayers.h>
#include <wincodec.h>
#include <cmath>
#include <limits>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

bool RenderCapture::ReadBuffer(ID3D12Resource* source, D3D12_RESOURCE_STATES state,
    UINT64 bytes, std::vector<uint8_t>& output)
{
    auto* device = GraphicsDevice::GetDevice();
    if (!device || !source || !bytes || source->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        bytes > source->GetDesc().Width || !GraphicsDevice::WaitForGpuIdle()) return false;
    ComPtr<ID3D12Resource> readback;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    const auto description = CD3DX12_RESOURCE_DESC::Buffer(bytes);
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
            IID_PPV_ARGS(&commands)))) return false;
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(source, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commands->ResourceBarrier(1, &barrier);
    commands->CopyBufferRegion(readback.Get(), 0, source, 0, bytes);
    barrier = CD3DX12_RESOURCE_BARRIER::Transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    commands->ResourceBarrier(1, &barrier);
    if (FAILED(commands->Close())) return false;
    ID3D12CommandList* lists[] = { commands.Get() };
    GraphicsDevice::GetCommandQueue()->ExecuteCommandLists(1, lists);
    if (!GraphicsDevice::WaitForGpuIdle()) return false;
    void* mapped = nullptr;
    const CD3DX12_RANGE range(0, static_cast<SIZE_T>(bytes));
    if (FAILED(readback->Map(0, &range, &mapped))) return false;
    output.resize(static_cast<size_t>(bytes));
    memcpy(output.data(), mapped, output.size());
    const CD3DX12_RANGE noWrite(0, 0);
    readback->Unmap(0, &noWrite);
    return true;
}

bool RenderCapture::ReadTexture(ID3D12Resource* texture, TextureData& output)
{
    return ReadTextureMip(texture, output, 0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

bool RenderCapture::ReadTextureMip(ID3D12Resource* texture, TextureData& output, UINT mip, D3D12_RESOURCE_STATES state)
{
    ID3D12Device* device = GraphicsDevice::GetDevice();
    if (!device || !texture || !GraphicsDevice::WaitForGpuIdle())
    {
        return false;
    }
    const auto description = texture->GetDesc();
    if (mip >= description.MipLevels) return false;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&description, mip, 1, 0, &footprint, nullptr, nullptr, &bytes);
    ComPtr<ID3D12Resource> readback;
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    const auto buffer = CD3DX12_RESOURCE_DESC::Buffer(bytes);
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands))))
    {
        return false;
    }
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(texture,
        state, D3D12_RESOURCE_STATE_COPY_SOURCE, mip);
    commands->ResourceBarrier(1, &barrier);
    const CD3DX12_TEXTURE_COPY_LOCATION destination(readback.Get(), footprint);
    const CD3DX12_TEXTURE_COPY_LOCATION source(texture, mip);
    commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    barrier = CD3DX12_RESOURCE_BARRIER::Transition(texture,
        D3D12_RESOURCE_STATE_COPY_SOURCE, state, mip);
    commands->ResourceBarrier(1, &barrier);
    if (FAILED(commands->Close()))
    {
        return false;
    }
    ID3D12CommandList* lists[] = { commands.Get() };
    GraphicsDevice::GetCommandQueue()->ExecuteCommandLists(1, lists);
    if (!GraphicsDevice::WaitForGpuIdle())
    {
        return false;
    }
    void* mapped = nullptr;
    const CD3DX12_RANGE readRange(0, static_cast<SIZE_T>(bytes));
    if (FAILED(readback->Map(0, &readRange, &mapped)))
    {
        return false;
    }
    output.Width = footprint.Footprint.Width;
    output.Height = footprint.Footprint.Height;
    output.RowPitch = footprint.Footprint.RowPitch;
    output.Format = description.Format;
    output.Bytes.resize(static_cast<size_t>(bytes));
    memcpy(output.Bytes.data(), mapped, static_cast<size_t>(bytes));
    const CD3DX12_RANGE noWrite(0, 0);
    readback->Unmap(0, &noWrite);
    return true;
}

float RenderCapture::HalfToFloat(uint16_t value)
{
    const float sign = (value & 0x8000u) != 0 ? -1.0f : 1.0f;
    const int exponent = (value >> 10) & 31;
    const int mantissa = value & 1023;
    if (exponent == 0)
    {
        return sign * std::ldexp(static_cast<float>(mantissa), -24);
    }
    if (exponent == 31)
    {
        return std::numeric_limits<float>::infinity();
    }
    return sign * std::ldexp(static_cast<float>(mantissa + 1024), exponent - 25);
}

bool RenderCapture::SaveScenePng(const wchar_t* path)
{
    TextureData texture;
    if (!ReadTexture(RenderTargets::GetEditorSceneResource(), texture) ||
        (texture.Format != DXGI_FORMAT_R8G8B8A8_UNORM && texture.Format != DXGI_FORMAT_R16G16B16A16_FLOAT))
    {
        return false;
    }
    std::vector<BYTE> pixels(static_cast<size_t>(texture.Width) * texture.Height * 4);
    for (UINT row = 0; row < texture.Height; ++row)
    {
        for (UINT column = 0; column < texture.Width; ++column)
        {
            BYTE* destination = pixels.data() + (static_cast<size_t>(row) * texture.Width + column) * 4;
            const BYTE* source = texture.Bytes.data() + static_cast<size_t>(row) * texture.RowPitch;
            if (texture.Format == DXGI_FORMAT_R8G8B8A8_UNORM)
            {
                source += column * 4;
                destination[0] = source[2];
                destination[1] = source[1];
                destination[2] = source[0];
            }
            else
            {
                const auto* components = reinterpret_cast<const uint16_t*>(source) + column * 4;
                for (UINT channel = 0; channel < 3; ++channel)
                {
                    const float color = HalfToFloat(components[2 - channel]);
                    if (!std::isfinite(color))
                    {
                        return false;
                    }
                    destination[channel] = static_cast<BYTE>(std::clamp(color, 0.0f, 1.0f) * 255.0f + 0.5f);
                }
            }
            destination[3] = 255;
        }
    }
    const HRESULT apartmentResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    const bool saved =
        SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE)) &&
        SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
        SUCCEEDED(frame->SetSize(texture.Width, texture.Height)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
        SUCCEEDED(frame->WritePixels(texture.Height, texture.Width * 4, static_cast<UINT>(pixels.size()), pixels.data())) &&
        SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    frame.Reset();
    encoder.Reset();
    stream.Reset();
    factory.Reset();
    if (SUCCEEDED(apartmentResult))
    {
        CoUninitialize();
    }
    return saved;
}

UINT RenderCapture::ReportGraphicsErrors()
{
    ComPtr<ID3D12InfoQueue> info;
    if (FAILED(GraphicsDevice::GetDevice()->QueryInterface(IID_PPV_ARGS(&info))))
    {
        WriteGraphicsLog("Validation: D3D12 debug queue unavailable\n");
        return 0;
    }
    UINT errorCount = 0;
    for (UINT64 index = 0; index < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++index)
    {
        SIZE_T bytes = 0;
        info->GetMessage(index, nullptr, &bytes);
        std::vector<BYTE> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (FAILED(info->GetMessage(index, message, &bytes))) continue;
        if (message->ID == D3D12_MESSAGE_ID_GPU_BASED_VALIDATION_RESOURCE_STATE_IMPRECISE)
            WriteGraphicsLog("D3D12 validation tracking limitation id=%u: %s\n", static_cast<UINT>(message->ID), message->pDescription);
        if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR || message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)
        {
            ++errorCount;
            WriteGraphicsLog("D3D12 validation error: %s\n", message->pDescription);
        }
    }
    WriteGraphicsLog("Validation: D3D12 errors = %u\n", errorCount);
    return errorCount;
}
