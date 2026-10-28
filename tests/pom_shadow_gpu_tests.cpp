#include "d3d12_compute_fixture.h"
#include <cmath>

int main()
{
    try
    {
        D3D12Test::Fixture gpu;
        auto code = D3D12Test::Compile(L"build/pom-tests/shadow-under-test.hlsl");
        auto output = gpu.MakeTexture(12, 1, DXGI_FORMAT_R32_FLOAT, true);
        const unsigned constants[4] = {};
        gpu.Dispatch(code.Get(), constants, sizeof(constants), {}, {&output}, 12, 1);
        auto bytes = gpu.Readback(output);
        const auto* visibility = reinterpret_cast<const float*>(bytes.data());
        for (unsigned i = 0; i < 12; ++i)
        {
            const bool blocker = i >= 8;
            if (!std::isfinite(visibility[i]) || visibility[i] < 0 || visibility[i] > 1 ||
                (!blocker && visibility[i] < 0.999f) || (blocker && visibility[i] > 0.9f))
            {
                std::fprintf(stderr, "POM shadow case %u: visibility %.6f\n", i, visibility[i]);
                return 1;
            }
        }
        std::puts("POM GPU: flat heights, interpolation error, shallow/deep relief and real blockers: PASS (12 cases)");
        auto receiverCode = D3D12Test::Compile(L"build/pom-tests/receiver-under-test.hlsl");
        // Exact half-float powers of two cover shallow and deep POM without payload rounding.
        unsigned short normals[24][4] = {};
        for (unsigned i = 0; i < 24; ++i)
        {
            normals[i][0] = normals[i][1] = 0x3800;
            normals[i][2] = 0x3c00;
            normals[i][3] = static_cast<unsigned short>(0x8000 | ((5 + i % 12) << 10));
        }
        auto normalTexture = gpu.MakeTexture(24, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, false);
        auto receiverOutput = gpu.MakeTexture(24, 1, DXGI_FORMAT_R32_FLOAT, true);
        gpu.Upload(normalTexture, normals, sizeof(normals));
        gpu.Dispatch(receiverCode.Get(), constants, sizeof(constants), {&normalTexture}, {&receiverOutput}, 24, 1);
        bytes = gpu.Readback(receiverOutput);
        const auto* receiverZ = reinterpret_cast<const float*>(bytes.data());
        for (unsigned i = 0; i < 24; ++i)
        {
            if (!std::isfinite(receiverZ[i]) || std::abs(receiverZ[i]) > 0.00001f)
            {
                std::fprintf(stderr, "POM mesh receiver case %u: z %.6f\n", i, receiverZ[i]);
                return 1;
            }
        }
        std::puts("POM GPU: Plane shadow/AO receivers, 12 depths, orthographic/perspective: PASS (24 cases)");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
