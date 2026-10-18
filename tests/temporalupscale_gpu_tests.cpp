#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace
{
constexpr UINT InputWidth = 32, InputHeight = 24;
constexpr UINT OutputWidth = 160, OutputHeight = 120;
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
void Check(HRESULT result, const char* message) { Require(SUCCEEDED(result), message); }
struct Texture
{
    ComPtr<ID3D11Texture2D> Resource;
    ComPtr<ID3D11ShaderResourceView> Srv;
    ComPtr<ID3D11UnorderedAccessView> Uav;
};
Texture MakeTexture(ID3D11Device* device, UINT width, UINT height, bool scalar, bool writable, bool pair = false)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = pair ? DXGI_FORMAT_R16G16_FLOAT : scalar ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (writable ? D3D11_BIND_UNORDERED_ACCESS : 0);
    Texture texture;
    Check(device->CreateTexture2D(&desc, nullptr, &texture.Resource), "create texture");
    Check(device->CreateShaderResourceView(texture.Resource.Get(), nullptr, &texture.Srv), "create SRV");
    if (writable) Check(device->CreateUnorderedAccessView(texture.Resource.Get(), nullptr, &texture.Uav), "create UAV");
    return texture;
}
float Halton(UINT phase, UINT base)
{
    float value = 0, fraction = 1;
    while (phase)
    {
        fraction /= base;
        value += fraction * (phase % base);
        phase /= base;
    }
    return value - .5f;
}
struct Constants
{
    float Extent[4] = {InputWidth, InputHeight, OutputWidth, OutputHeight};
    float Jitter[4]{};
    float Parameters[4]{};
    float InvCurrent[16]{};
    float Previous[16]{};
    float Reconstruction[4]{};
    Constants()
    {
        for (UINT i = 0; i < 4; ++i) InvCurrent[i * 5] = Previous[i * 5] = 1;
    }
};
}
int main(int argc, char** argv)
{
    try
    {
        const bool giFlow = argc > 1 && std::string(argv[1]).find("gi-flow") != std::string::npos;
        const bool slowFlow = giFlow && std::string(argv[1]).find("slow") != std::string::npos;
        const bool planarFixture = argc > 1 && std::string(argv[1]).find("plane") != std::string::npos;
        const bool thinGeometry = argc > 1 && std::string(argv[1]).find("geometry") != std::string::npos;
        const bool slowCamera = thinGeometry && std::string(argv[1]).find("moving") != std::string::npos;
        const bool thinSafety = thinGeometry && std::string(argv[1]).find("safety") != std::string::npos;
        const bool fineDetail = argc > 1 && (std::string(argv[1]).find("--detail") == 0 || planarFixture || thinGeometry);
        const bool dimDetail = fineDetail && std::string(argv[1]).find("dim") != std::string::npos;
        const bool stabilizeDetail = argc == 1 || (giFlow && std::string(argv[1]).find("stable") != std::string::npos) || (fineDetail && (planarFixture || thinGeometry || std::string(argv[1]).find("stable") != std::string::npos));
        const float detailScale = dimDetail ? .08f / .98f : 1.f;
        const bool subpixelAA = argc == 1 || fineDetail || giFlow;
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &device, nullptr, &context), "create fixture device");
        ComPtr<ID3DBlob> bytecode, errors;
        const auto compiled = D3DCompileFromFile(L"shader/hlsl/TemporalUpscaleCS.hlsl", nullptr,
            D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &bytecode, &errors);
        if (FAILED(compiled) && errors) std::cerr << static_cast<const char*>(errors->GetBufferPointer());
        Check(compiled, "compile production TSR shader");
        ComPtr<ID3D11ComputeShader> shader;
        Check(device->CreateComputeShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, &shader), "create shader");
        context->CSSetShader(shader.Get(), nullptr, 0);
        auto color = MakeTexture(device.Get(), InputWidth, InputHeight, false, false);
        auto depth = MakeTexture(device.Get(), InputWidth, InputHeight, true, false);
        auto velocity = MakeTexture(device.Get(), InputWidth, InputHeight, false, false);
        auto reveal = MakeTexture(device.Get(), InputWidth, InputHeight, true, false);
        std::array<Texture, 2> historyColor, historyDepth, historyDetail;
        for (UINT i = 0; i < 2; ++i)
        {
            historyColor[i] = MakeTexture(device.Get(), OutputWidth, OutputHeight, false, true);
            historyDepth[i] = MakeTexture(device.Get(), OutputWidth, OutputHeight, true, true);
            historyDetail[i] = MakeTexture(device.Get(), OutputWidth, OutputHeight, false, true, true);
        }
        D3D11_BUFFER_DESC bufferDesc{};
        bufferDesc.ByteWidth = sizeof(Constants);
        bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ComPtr<ID3D11Buffer> buffer;
        Check(device->CreateBuffer(&bufferDesc, nullptr, &buffer), "create constants");
        context->CSSetConstantBuffers(0, 1, buffer.GetAddressOf());
        D3D11_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> sampler;
        Check(device->CreateSamplerState(&samplerDesc, &sampler), "create sampler");
        context->CSSetSamplers(0, 1, sampler.GetAddressOf());
        D3D11_TEXTURE2D_DESC stagingDesc{};
        historyColor[0].Resource->GetDesc(&stagingDesc);
        stagingDesc.BindFlags = 0;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        Check(device->CreateTexture2D(&stagingDesc, nullptr, &staging), "create readback");
        std::vector<float> inputColor(InputWidth * InputHeight * 4), inputDepth(InputWidth * InputHeight);
        std::vector<float> inputVelocity(inputColor.size()), revealage(inputDepth.size(), 1);
        std::vector<float> last(OutputWidth * OutputHeight), resolved(last.size());
        float previousX = 0, previousY = 0;
        double error = 0, shimmer = 0, detailEnergy = 0;
        UINT samples = 0;
        for (UINT frame = 0; frame < (fineDetail ? 176u : 179u); ++frame)
        {
            Constants constants;
            const float cameraOffset = slowCamera ? frame * .1f / OutputWidth : 0;
            const float jitterX = Halton(frame % 128 + 1, 2) / InputWidth;
            const float jitterY = Halton(frame % 128 + 1, 3) / InputHeight;
            constants.Jitter[0] = jitterX;
            constants.Jitter[1] = jitterY;
            constants.Jitter[2] = previousX;
            constants.Jitter[3] = previousY;
            // Last frames exercise object removal, a camera cut, then transparency.
            constants.Parameters[0] = frame > 0 && frame != 161 && frame != 163 ? 1.f : 0.f;
            constants.Parameters[1] = frame == 162 ? 1.f : 0.f;
            if (fineDetail)
            {
                constants.Parameters[0] = frame > 0 ? 1.f : 0.f;
                constants.Parameters[1] = 0;
            }
            if (thinSafety && frame == 160) constants.Parameters[1] = 1;
            if (thinSafety && frame == 161) constants.Parameters[0] = 0;
            if (giFlow && frame == 163) constants.Parameters[0] = 1;
            constants.Parameters[2] = subpixelAA ? 1.f : 0.f;
            constants.Parameters[3] = stabilizeDetail ? 1.f : 0.f;
            constants.Reconstruction[0] = !planarFixture || std::string(argv[1]).find("stable") != std::string::npos ? 1.f : 0.f;
            constants.Reconstruction[1] = !thinGeometry || std::string(argv[1]).find("stable") != std::string::npos ? 1.f : 0.f;
            for (UINT y = 0; y < InputHeight; ++y)
            for (UINT x = 0; x < InputWidth; ++x)
            {
                const UINT pixel = y * InputWidth + x;
                const float worldX = (x + .5f) / InputWidth - jitterX - cameraOffset;
                const float worldY = (y + .5f) / InputHeight - jitterY;
                const float objectOffset = frame >= 163 ? float(frame - 163) * 2 / OutputHeight : 0;
                const bool foreground = (frame < 160 || frame >= 163) && worldY > .3f * worldX + .31f + objectOffset;
                inputDepth[pixel] = foreground ? .5f : 1.f;
                float value = frame == 161 ? .7f : frame == 162 ? .4f : foreground ? 1.f : .02f;
                if (fineDetail)
                {
                    // One output-pixel-wide slanted strand on a continuous surface.
                    value = frame < 160 && std::abs(worldX - (.27f * worldY + .37f)) < .5f / OutputWidth ? .02f + .98f * detailScale : .02f;
                    inputDepth[pixel] = planarFixture ? .3f + .4f * worldY : .5f;
                    if (thinGeometry) inputDepth[pixel] = value > .02f ? .5f : 1.f;
                }
                for (UINT channel = 0; channel < 3; ++channel) inputColor[pixel * 4 + channel] = value;
                inputColor[pixel * 4 + 3] = 1;
                inputVelocity[pixel * 4 + 2] = jitterX - previousX;
                if (slowCamera && frame > 0) inputVelocity[pixel * 4 + 2] += .1f / OutputWidth;
                inputVelocity[pixel * 4 + 3] = jitterY - previousY;
                if (foreground && frame > 163 && !fineDetail) inputVelocity[pixel * 4 + 3] += 2.f / OutputHeight;
                if (giFlow)
                {
                    const float red = frame < 160 ? .15f + .002f * std::sin(frame * (slowFlow ? .17f : 1.7f)) :
                        frame == 160 ? .35f : frame == 161 ? .07f : frame == 162 ? .11f : .21f;
                    inputColor[pixel * 4] = red;
                    inputColor[pixel * 4 + 1] = .03f;
                    inputColor[pixel * 4 + 2] = .02f;
                    inputDepth[pixel] = .5f;
                    inputVelocity[pixel * 4 + 3] = jitterY - previousY;
                    if (frame == 163) inputVelocity[pixel * 4 + 3] += 2.f / OutputHeight;
                }
            }
            std::fill(revealage.begin(), revealage.end(), frame == 162 || (thinSafety && frame == 160) ? 0.f : 1.f);
            context->UpdateSubresource(color.Resource.Get(), 0, nullptr, inputColor.data(), InputWidth * 16, 0);
            context->UpdateSubresource(depth.Resource.Get(), 0, nullptr, inputDepth.data(), InputWidth * 4, 0);
            context->UpdateSubresource(velocity.Resource.Get(), 0, nullptr, inputVelocity.data(), InputWidth * 16, 0);
            context->UpdateSubresource(reveal.Resource.Get(), 0, nullptr, revealage.data(), InputWidth * 4, 0);
            context->UpdateSubresource(buffer.Get(), 0, nullptr, &constants, 0, 0);
            const UINT write = frame % 2, read = write ^ 1;
            ID3D11ShaderResourceView* srvs[] = {color.Srv.Get(), depth.Srv.Get(), velocity.Srv.Get(),
                historyColor[read].Srv.Get(), historyDepth[read].Srv.Get(), reveal.Srv.Get(), historyDetail[read].Srv.Get()};
            ID3D11UnorderedAccessView* uavs[] = {historyColor[write].Uav.Get(), historyDepth[write].Uav.Get(), historyDetail[write].Uav.Get()};
            context->CSSetShaderResources(0, 7, srvs);
            context->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
            context->Dispatch(OutputWidth / 8, OutputHeight / 8, 1);
            ID3D11ShaderResourceView* noSrvs[7]{};
            ID3D11UnorderedAccessView* noUavs[3]{};
            context->CSSetShaderResources(0, 7, noSrvs);
            context->CSSetUnorderedAccessViews(0, 3, noUavs, nullptr);
            context->CopyResource(staging.Get(), historyColor[write].Resource.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "map output");
            for (UINT y = 0; y < OutputHeight; ++y)
            {
                const auto* row = reinterpret_cast<const float*>(static_cast<const BYTE*>(mapped.pData) + y * mapped.RowPitch);
                for (UINT x = 0; x < OutputWidth; ++x) resolved[y * OutputWidth + x] = row[x * 4];
            }
            context->Unmap(staging.Get(), 0);
            if (giFlow)
            {
                const UINT center = OutputWidth * (OutputHeight / 2) + OutputWidth / 2;
                if (frame >= 128 && frame < 160)
                {
                    const double delta = resolved[center] - last[center];
                    shimmer += delta * delta;
                    error += (resolved[center] - .15) * (resolved[center] - .15);
                    ++samples;
                }
                if (frame >= 160 && frame <= 163)
                {
                    const float expected = frame == 160 ? .35f : frame == 161 ? .07f : frame == 162 ? .11f : .21f;
                    Require(std::abs(resolved[center] - expected) < (frame == 160 ? .006f : .0001f),
                        "GI stability retains lighting changes, resets, reactive pixels or motion");
                }
            }
            else if (fineDetail && frame >= 160)
            {
                if (thinSafety && (frame == 160 || frame == 161))
                {
                    float maximumError = 0;
                    for (float value : resolved) maximumError = (std::max)(maximumError, std::abs(value - .02f));
                    Require(maximumError < .0001f, "thin geometry history survives reactive rejection or reset");
                }
                // A persistent signal change at unchanged depth must release the lock.
                if (frame == 175)
                {
                    float maximumError = 0;
                    for (float value : resolved) maximumError = (std::max)(maximumError, std::abs(value - .02f));
                    std::cout << "Detail removal residual=" << maximumError << '\n';
                    Require(maximumError < .02f, "detail lock retains a removed texture strand");
                }
            }
            else if (frame >= 163)
            {
                float maximumError = 0;
                for (UINT y = 5; y < OutputHeight - 5; ++y)
                for (UINT x = 5; x < OutputWidth - 5; ++x)
                {
                    const float edge = (.3f * (x + .5f) / OutputWidth + .31f) * OutputHeight + float(frame - 163) * 2;
                    // Bicubic fallback spans two input texels (10 output pixels),
                    // plus a jittered input sample's half-texel displacement.
                    if (std::abs(y + .5f - edge) <= 13) continue;
                    const float expected = y + .5f > edge ? 1.f : .02f;
                    maximumError = (std::max)(maximumError, std::abs(resolved[y * OutputWidth + x] - expected));
                }
                if (maximumError >= .02f) std::cerr << "moving frame=" << frame << " maximum error=" << maximumError << '\n';
                Require(maximumError < .02f, "moving silhouette leaves a trail outside its reconstruction footprint");
            }
            else if (frame >= 160)
            {
                const float expected = frame == 160 ? .02f : frame == 161 ? .7f : .4f;
                float maximumError = 0;
                // Newly exposed background must not retain the removed object's color.
                for (UINT y = OutputHeight - 20; y < OutputHeight - 5; ++y)
                for (UINT x = 5; x < OutputWidth - 5; ++x)
                    maximumError = (std::max)(maximumError, std::abs(resolved[y * OutputWidth + x] - expected));
                Require(maximumError < .0001f, "disocclusion, reset or reactive mask retained stale history");
            }
            else if (frame >= 128)
            {
                for (UINT y = 5; y < OutputHeight - 5; ++y)
                for (UINT x = 5; x < OutputWidth - 5; ++x)
                {
                    const float edge = .3f * (x + .5f) / OutputWidth + .31f;
                    const float distance = (y + .5f) / OutputHeight - edge;
                    const float strandX = (.27f * (y + .5f) / OutputHeight + .37f + cameraOffset) * OutputWidth;
                    if (fineDetail ? std::abs(x + .5f - strandX) > 4 : std::abs(distance * OutputHeight) > 4) continue;
                    // Box-filtered analytic coverage, integrated over 32 horizontal samples.
                    float coverage = 0;
                    for (UINT sub = 0; sub < 32; ++sub)
                    {
                        const float boundary = (.3f * (x + (sub + .5f) / 32) / OutputWidth + .31f) * OutputHeight;
                        coverage += std::clamp(y + 1.f - boundary, 0.f, 1.f) / 32;
                    }
                    const UINT index = y * OutputWidth + x;
                    if (fineDetail)
                        coverage = std::clamp((std::min)(x + 1.f, strandX + .5f) - (std::max)(float(x), strandX - .5f), 0.f, 1.f);
                    const double difference = resolved[index] - (.02f + .98f * coverage * (fineDetail ? detailScale : 1.f));
                    error += difference * difference;
                    const double change = resolved[index] - last[index];
                    shimmer += change * change;
                    detailEnergy += resolved[index] - .02f;
                    ++samples;
                }
            }
            last = resolved;
            previousX = jitterX;
            previousY = jitterY;
        }
        const double rmse = std::sqrt(error / samples), temporal = std::sqrt(shimmer / samples);
        if (giFlow)
        {
            std::cout << "GI flow: temporal RMS=" << temporal << "; bias RMSE=" << rmse << '\n';
            if (stabilizeDetail) Require(temporal < (slowFlow ? .00008 : .0004) && rmse < .001, "weak GI changes bypass temporal accumulation");
            else Require(temporal > (slowFlow ? .00015 : .001), "baseline must reproduce weak GI flicker");
            return 0;
        }
        std::cout << (dimDetail ? "Dim " : "") << (fineDetail ? (stabilizeDetail ? "Stable detail" : "Baseline detail") : subpixelAA ? "Subpixel AA" : "Legacy TSR")
            << " 20% diagonal: coverage RMSE=" << rmse << "; temporal RMS=" << temporal << '\n';
        if (fineDetail)
        {
            std::cout << "Detail mean contrast=" << detailEnergy / samples << '\n';
            if (thinGeometry)
            {
                if (std::string(argv[1]).find("stable") != std::string::npos)
                {
                    Require(temporal < .04, "distant thin geometry flickers");
                    Require(rmse < .28, "distant thin geometry coverage is lost");
                    Require(detailEnergy / samples > .035, "stability erases distant thin geometry");
                }
                else Require(temporal > .15, "baseline must reproduce thin geometry flicker");
            }
            else if (planarFixture)
            {
                const bool planarDepth = std::string(argv[1]).find("stable") != std::string::npos;
                if (planarDepth)
                {
                    Require(temporal < .04, "grazing planar detail flickers");
                    Require(rmse < .25, "grazing stabilization loses detail coverage");
                }
                else Require(temporal > .1, "baseline must reproduce grazing depth rejection");
            }
            else if (stabilizeDetail)
            {
                Require(temporal < .015 * detailScale, "subpixel strand flickers");
                Require(detailEnergy / samples > .0174 * detailScale, "stability must not erase the strand");
                Require(rmse < .2682 * detailScale, "stability must not increase strand coverage error");
            }
        }
        else if (subpixelAA)
        {
            Require(rmse < .20, "diagonal reconstruction error exceeds limit");
            Require(temporal < .04, "stationary silhouette flickers");
        }
        else Require(rmse > .20, "legacy fixture must reproduce the reported edge problem");
        std::cout << "temporal upscale shader GPU tests: PASS\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
