#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include "../source/editor/gbufferpreviewshader.h"

using Microsoft::WRL::ComPtr;

namespace
{
    void Check(HRESULT result)
    {
        if (FAILED(result))
        {
            throw std::runtime_error("GBuffer preview GPU setup failed");
        }
    }

    ComPtr<ID3DBlob> Compile(const char* source, const char* profile)
    {
        ComPtr<ID3DBlob> code, errors;
        HRESULT result =
            D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, "main", profile, 0, 0, &code, &errors);
        if (FAILED(result) && errors)
        {
            std::fputs(static_cast<const char*>(errors->GetBufferPointer()), stderr);
        }
        Check(result);
        return code;
    }
}

int main()
{
    try
    {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                nullptr, &context));
        const char* vertexSource = R"hlsl(
struct Output { float4 Position : SV_POSITION; float4 Color : COLOR0; float2 Uv : TEXCOORD0; };
Output main(uint id : SV_VertexID)
{
    Output output;
    output.Uv = float2((id << 1) & 2, id & 2);
    output.Position = float4(output.Uv * float2(2, -2) + float2(-1, 1), 0, 1);
    output.Color = 1;
    return output;
}
)hlsl";
        auto vsCode = Compile(vertexSource, "vs_5_0");
        auto psCode = Compile(GBufferPreviewShader::Pixel, "ps_5_0");
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        Check(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs));
        Check(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps));
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->PSSetShader(ps.Get(), nullptr, 0);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D11_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
        context->RSSetViewports(1, &viewport);

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> source, target, staging;
        Check(device->CreateTexture2D(&desc, nullptr, &source));
        ComPtr<ID3D11ShaderResourceView> srv;
        Check(device->CreateShaderResourceView(source.Get(), nullptr, &srv));
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        Check(device->CreateTexture2D(&desc, nullptr, &target));
        ComPtr<ID3D11RenderTargetView> rtv;
        Check(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
        desc.BindFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Check(device->CreateTexture2D(&desc, nullptr, &staging));
        D3D11_BUFFER_DESC bufferDesc{};
        bufferDesc.ByteWidth = 16;
        bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ComPtr<ID3D11Buffer> settings;
        Check(device->CreateBuffer(&bufferDesc, nullptr, &settings));
        context->PSSetConstantBuffers(1, 1, settings.GetAddressOf());

        auto verify = [&](const char* name, unsigned type, std::array<float, 4> sample, std::array<float, 4> expected) {
            const unsigned constants[4] = {type, 0, 0, 0};
            context->UpdateSubresource(settings.Get(), 0, nullptr, constants, 0, 0);
            context->UpdateSubresource(source.Get(), 0, nullptr, sample.data(), sizeof(sample), 0);
            context->PSSetShaderResources(0, 1, srv.GetAddressOf());
            context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
            context->Draw(3, 0);
            context->CopyResource(staging.Get(), target.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
            std::array<float, 4> actual;
            std::memcpy(actual.data(), mapped.pData, sizeof(actual));
            context->Unmap(staging.Get(), 0);
            for (size_t channel = 0; channel < actual.size(); ++channel)
            {
                if (std::abs(actual[channel] - expected[channel]) > 0.0001f)
                {
                    std::fprintf(stderr, "%s: channel %zu expected %g, got %g\n", name, channel, expected[channel],
                                 actual[channel]);
                    throw std::runtime_error("GBuffer preview regression");
                }
            }
        };
        verify("normal with negative POM alpha", 1, {0.5f, 1, 0.5f, -0.2f}, {0.5f, 1, 0.5f, 1});
        verify("material ID is not opacity", 3, {0.1f, 0.8f, 0.04f, 0}, {0.1f, 0.8f, 0.04f, 1});
        verify("shadow model is not opacity", 4, {0.5f, 0.1f, 0.8f, 0}, {0.5f, 0.1f, 0.8f, 1});
        verify("reverse Z far geometry", 2, {0.00001f, 0, 0, 1}, {0.1f, 0.1f, 0.1f, 1});
        verify("reverse Z background", 2, {0, 0, 0, 0}, {0, 0, 0, 1});
        verify("reverse Z near plane", 2, {1, 0, 0, 0}, {1, 1, 1, 1});
        verify("stationary velocity", 7, {0.5f, 0.5f, 0, 0}, {0.5f, 0.5f, 0.5f, 1});
        verify("signed velocity is not opacity", 7, {1, 0, 0.2f, -0.2f}, {1, 0, 0.5f, 1});
        verify("HDR atmosphere", 6, {4, 1, 0, 0}, {0.8f, 0.5f, 0, 1});
        verify("HDR bloom", 9, {4, 1, 0, 0}, {0.8f, 0.5f, 0, 1});
        verify("HDR emission", 5, {4, 1, 0, 0.3f}, {0.8f, 0.5f, 0, 1});
        verify("material AO", 10, {4, 1, 0, 0.3f}, {0.3f, 0.3f, 0.3f, 1});
        std::puts("GBuffer preview production pixel shader GPU tests: 12 PASS");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
