#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <nvsdk_ngx.h>
#include <cstdio>
#include <filesystem>

using Microsoft::WRL::ComPtr;

int main()
{
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        return 1;
    }
    for (UINT index = 0; ; ++index)
    {
        adapter.Reset();
        if (FAILED(factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter))))
        {
            break;
        }
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
        {
            break;
        }
    }
    if (!device)
    {
        return 1;
    }
    const auto dllDirectory = std::filesystem::absolute("External/DLSS/sdk/lib/Windows_x86_64/rel").wstring();
    const auto logDirectory = std::filesystem::absolute("bin/validation/ngx").wstring();
    std::filesystem::create_directories(logDirectory);
    const wchar_t* paths[] = { dllDirectory.c_str() };
    NVSDK_NGX_FeatureCommonInfo featureInfo{};
    featureInfo.PathListInfo.Path = paths;
    featureInfo.PathListInfo.Length = 1;
    const auto initialized = NVSDK_NGX_D3D12_Init_with_ProjectID(
        "ddaac575-3a1b-4406-94ce-e13d43222fe2", NVSDK_NGX_ENGINE_TYPE_CUSTOM,
        "1.0", logDirectory.c_str(), device.Get(), &featureInfo);
    printf("NGX initialization: 0x%08x\n", static_cast<unsigned>(initialized));
    if (NVSDK_NGX_FAILED(initialized))
    {
        return 2;
    }
    NVSDK_NGX_Parameter* parameters = nullptr;
    const auto capabilities = NVSDK_NGX_D3D12_GetCapabilityParameters(&parameters);
    int available = 0;
    int driverUpdate = 0;
    if (NVSDK_NGX_SUCCEED(capabilities))
    {
        parameters->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
        parameters->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &driverUpdate);
        NVSDK_NGX_D3D12_DestroyParameters(parameters);
    }
    printf("DLSS Super Resolution: available=%d driverUpdate=%d capabilities=0x%08x\n", available, driverUpdate, static_cast<unsigned>(capabilities));
    NVSDK_NGX_D3D12_Shutdown1(device.Get());
    return available && !driverUpdate ? 0 : 3;
}
