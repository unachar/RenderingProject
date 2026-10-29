#include <windows.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <array>
#include <cstdio>
#include "../source/d3dx12.h"
#include "imgui.h"
#include "imgui_impl_dx12.h"

using Microsoft::WRL::ComPtr;
#include "../source/editor/gbufferpreview.h"

int main()
{
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
    {
        debug->EnableDebugLayer();
    }
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<ID3D12Device> device;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))) ||
        FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) ||
        !GBufferPreview::Initialize(device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_D32_FLOAT))
    {
        std::fputs("GBuffer preview D3D12 pipeline initialization failed\n", stderr);
        return 1;
    }
    GBufferPreview::Shutdown();
    std::puts("GBuffer preview native DXC / D3D12 pipeline: PASS");
    return 0;
}
