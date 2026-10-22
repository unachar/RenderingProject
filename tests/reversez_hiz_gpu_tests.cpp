#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace
{
    void Check(HRESULT result)
    {
        if (FAILED(result))
        {
            throw std::runtime_error("Hi-Z GPU setup or readback failed");
        }
    }
}

int main()
{
    try
    {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                               D3D11_SDK_VERSION, &device, nullptr, &context));
        ComPtr<ID3DBlob> code;
        ComPtr<ID3DBlob> errors;
        const HRESULT compiled = D3DCompileFromFile(L"shader/hlsl/HiZReduceCS.hlsl", nullptr,
                                                    D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_0",
                                                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (FAILED(compiled) && errors)
        {
            std::fputs(static_cast<const char*>(errors->GetBufferPointer()), stderr);
        }
        Check(compiled);
        ComPtr<ID3D11ComputeShader> shader;
        Check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader));
        context->CSSetShader(shader.Get(), nullptr, 0);

        D3D11_TEXTURE2D_DESC description{};
        description.Width = 5;
        description.Height = 3;
        description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
        description.Format = DXGI_FORMAT_R32_FLOAT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> source;
        Check(device->CreateTexture2D(&description, nullptr, &source));
        ComPtr<ID3D11ShaderResourceView> srv;
        Check(device->CreateShaderResourceView(source.Get(), nullptr, &srv));
        description.Width = 2;
        description.Height = 1;
        description.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        ComPtr<ID3D11Texture2D> output;
        Check(device->CreateTexture2D(&description, nullptr, &output));
        ComPtr<ID3D11UnorderedAccessView> uav;
        Check(device->CreateUnorderedAccessView(output.Get(), nullptr, &uav));
        description.BindFlags = 0;
        description.Usage = D3D11_USAGE_STAGING;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        Check(device->CreateTexture2D(&description, nullptr, &staging));
        const UINT constants[] = {2, 1, 0, 0};
        D3D11_BUFFER_DESC bufferDescription{};
        bufferDescription.ByteWidth = sizeof(constants);
        bufferDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = constants;
        ComPtr<ID3D11Buffer> buffer;
        Check(device->CreateBuffer(&bufferDescription, &data, &buffer));
        context->CSSetConstantBuffers(0, 1, buffer.GetAddressOf());

        auto verify = [&](const std::array<float, 15>& depths, float expectedLeft, float expectedRight) {
            context->UpdateSubresource(source.Get(), 0, nullptr, depths.data(), 5 * sizeof(float), 0);
            context->CSSetShaderResources(0, 1, srv.GetAddressOf());
            context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
            context->Dispatch(1, 1, 1);
            ID3D11UnorderedAccessView* noUav = nullptr;
            ID3D11ShaderResourceView* noSrv = nullptr;
            context->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
            context->CSSetShaderResources(0, 1, &noSrv);
            context->CopyResource(staging.Get(), output.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
            const float* values = static_cast<const float*>(mapped.pData);
            const bool matches = values[0] == expectedLeft && values[1] == expectedRight;
            context->Unmap(staging.Get(), 0);
            if (!matches)
            {
                throw std::runtime_error("Reverse-Z Hi-Z must preserve the farthest depth and clear holes");
            }
        };

        std::array<float, 15> depths;
        depths.fill(0.8f);
        depths[0] = 0.1f;
        depths[14] = 0.0f;
        verify(depths, 0.1f, 0.0f); // 端の背景を含め、奇数サイズ全体を覆う。
        depths[7] = 0.0f;
        verify(depths, 0.0f, 0.0f); // 境界をまたぐ穴で隠蔽しない。
        depths.fill(1.0e-9f);
        verify(depths, 1.0e-9f, 1.0e-9f); // 遠景の小さな正の深度を失わない。
        std::puts("Reverse-Z production Hi-Z shader GPU tests: PASS");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
