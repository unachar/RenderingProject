#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include "rendering/scenedepth.h"
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace
{
    void Check(HRESULT result)
    {
        if (FAILED(result))
        {
            throw std::runtime_error("Camera projection GPU test setup/readback failed");
        }
    }

    bool Near(float actual, float expected)
    {
        return std::abs(actual - expected) < 0.001f;
    }

    struct CameraConstants
    {
        XMFLOAT4X4 Inverse;
        XMFLOAT4 CameraAndDepth;
    };

    struct MegaLightConstants
    {
        XMFLOAT4X4 WorldToView;
        XMFLOAT4 Projection, Depth, Depth2;
        XMUINT4 Grid, Counts, Screen, Stochastic;
        XMFLOAT4 StochasticParams;
        XMFLOAT4X4 ViewToWorld;
        XMFLOAT4 ScreenTrace;
        XMUINT4 SampleGrid;
    };
    static_assert(sizeof(MegaLightConstants) == 288);
}

int main()
{
    try
    {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                               D3D11_SDK_VERSION, &device, nullptr, &context));
        ComPtr<ID3DBlob> code, errors;
        const HRESULT compiled = D3DCompileFromFile(L"tests/camera_projection_tests.hlsl", nullptr,
            D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (FAILED(compiled) && errors)
        {
            std::fputs(static_cast<const char*>(errors->GetBufferPointer()), stderr);
        }
        Check(compiled);
        ComPtr<ID3D11ComputeShader> shader;
        Check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader));
        context->CSSetShader(shader.Get(), nullptr, 0);
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = 6 * sizeof(XMFLOAT4);
        description.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        description.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        description.StructureByteStride = sizeof(XMFLOAT4);
        ComPtr<ID3D11Buffer> output, staging, cameraBuffer, lightBuffer;
        Check(device->CreateBuffer(&description, nullptr, &output));
        ComPtr<ID3D11UnorderedAccessView> uav;
        Check(device->CreateUnorderedAccessView(output.Get(), nullptr, &uav));
        description.BindFlags = 0;
        description.Usage = D3D11_USAGE_STAGING;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Check(device->CreateBuffer(&description, nullptr, &staging));
        description = {};
        description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        description.ByteWidth = sizeof(CameraConstants);
        Check(device->CreateBuffer(&description, nullptr, &cameraBuffer));
        description.ByteWidth = sizeof(MegaLightConstants);
        Check(device->CreateBuffer(&description, nullptr, &lightBuffer));

        for (bool orthographic : {false, true})
        {
            const auto view = XMMatrixTranslation(-10, -20, 30);
            auto projection = orthographic ? SceneDepth::ReverseZOrthographicLH(10, 2, 0.1f, 100)
                                           : SceneDepth::InfiniteReverseZPerspectiveLH(XM_PIDIV4, 2, 0.1f);
            XMFLOAT4X4 values;
            XMStoreFloat4x4(&values, projection);
            SceneDepth::SetProjectionJitter(values, {0.01f, -0.02f});
            projection = XMLoadFloat4x4(&values);
            const auto inverse = XMMatrixInverse(nullptr, view * projection);
            const auto clip = XMVector4Transform(XMVectorSet(0, 0, 6, 1), projection);
            const float depth = XMVectorGetZ(clip) / XMVectorGetW(clip);
            CameraConstants camera{};
            XMStoreFloat4x4(&camera.Inverse, XMMatrixTranspose(inverse));
            camera.CameraAndDepth = {10, 20, -30, depth};
            MegaLightConstants lights{};
            XMStoreFloat4x4(&lights.WorldToView, XMMatrixTranspose(view));
            XMStoreFloat4x4(&lights.ViewToWorld, XMMatrixTranspose(XMMatrixInverse(nullptr, view)));
            lights.Projection = {values._11, values._22, 0.01f, -0.02f};
            lights.Depth = {values._33, values._43, 0.1f, 1};
            lights.Grid.w = 1u | (orthographic ? (1u << 18) : 0u);
            lights.Screen = {100, 50, 1, 1};
            context->UpdateSubresource(cameraBuffer.Get(), 0, nullptr, &camera, 0, 0);
            context->UpdateSubresource(lightBuffer.Get(), 0, nullptr, &lights, 0, 0);
            ID3D11Buffer* buffers[] = {cameraBuffer.Get(), lightBuffer.Get()};
            context->CSSetConstantBuffers(0, 2, buffers);
            context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
            context->Dispatch(1, 1, 1);
            ID3D11UnorderedAccessView* noUav = nullptr;
            context->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
            context->CopyResource(staging.Get(), output.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
            std::array<XMFLOAT4, 6> result;
            std::memcpy(result.data(), mapped.pData, sizeof(result));
            context->Unmap(staging.Get(), 0);
            assert(Near(result[0].z, 1));
            if (orthographic)
            {
                assert(Near(result[1].x, 0) && Near(result[1].y, 0) && Near(result[1].z, 1));
                assert(Near(result[3].x - result[2].x, 8));
                assert(Near(result[3].y - result[2].y, 3));
            }
            else
            {
                assert(std::abs(result[1].x - result[0].x) > 0.1f);
                assert(Near(result[2].x, 10) && Near(result[3].x, 10));
            }
            const auto world = XMVector4Transform(XMVectorSet(0.01f, -0.02f, depth, 1), inverse);
            assert(Near(result[4].x, XMVectorGetX(world) / XMVectorGetW(world)));
            assert(Near(result[4].y, XMVectorGetY(world) / XMVectorGetW(world)));
            assert(Near(result[4].z, XMVectorGetZ(world) / XMVectorGetW(world)));
            assert(Near(result[4].w, 6));
            assert(Near(result[5].x, 50.5f) && Near(result[5].y, 25.5f));
        }
        std::puts("Perspective/orthographic production camera rays and MegaLights depth GPU tests: PASS");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
