#include "d3d12_compute_fixture.h"
#include <cmath>

int main(int argc, char* argv[])
{
    try
    {
        const bool useHardware = argc > 1 && std::string(argv[1]) == "--hardware";
        D3D12Test::Fixture gpu(!useHardware);
        auto code = D3D12Test::Compile(L"build/pom-tests/intersection-under-test.hlsl");
        auto height = gpu.MakeTexture(4096, 1, DXGI_FORMAT_R32_FLOAT, false);
        auto output = gpu.MakeTexture(256, 50, DXGI_FORMAT_R32G32B32A32_FLOAT, true);
        const unsigned constants[4] = {};
        double squaredError = 0;
        float maxError = 0;
        unsigned cases = 0;
        for (unsigned profile = 0; profile < 4; ++profile)
        {
            std::vector<float> heights(4096);
            for (unsigned i = 0; i < heights.size(); ++i)
            {
                const float uv = (i + 0.5f) / heights.size();
                heights[i] = 1.0f - std::clamp((uv - 0.45f) * 40.0f, 0.0f, 1.0f);
                if (profile == 1)
                {
                    heights[i] = 1;
                }
                if (profile == 2)
                {
                    heights[i] = 0;
                }
                if (profile == 3)
                {
                    heights[i] = 0.63f;
                }
            }
            gpu.Upload(height, heights.data(), static_cast<UINT>(heights.size() * sizeof(float)));
            gpu.Dispatch(code.Get(), constants, sizeof(constants), {&height}, {&output}, 256, 50);
            auto bytes = gpu.Readback(output);
            const auto* pixels = reinterpret_cast<const float*>(bytes.data());
            for (unsigned row = profile * 12; row < (profile + 1) * 12; ++row)
            {
                const float scale = row % 12 < 6 ? 0.02f : 0.2f;
                const float offset = 1.7320508f * scale;
                for (unsigned x = 0; x < 256; ++x)
                {
                    const float uv = 0.4f + 0.45f * x / 255.0f;
                    float expected = std::clamp((uv - 0.45f) * 40 / (1 + offset * 40), 0.0f, 1.0f);
                    if (profile == 1)
                    {
                        expected = 0;
                    }
                    if (profile == 2)
                    {
                        expected = 1;
                    }
                    if (profile == 3)
                    {
                        expected = 0.37f;
                    }
                    const auto* pixel = pixels + (row * 256 + x) * 4;
                    const float error = std::abs(pixel[0] - expected);
                    if (!std::isfinite(pixel[0]) || error > 0.012f || pixel[3] != 1 ||
                        std::abs(pixel[1] - (uv - offset * pixel[0])) > 0.00001f)
                    {
                        std::fprintf(
                            stderr,
                            "POM intersection profile %u row %u pixel %u: depth %.6f expected %.6f error %.6f\n",
                            profile, row, x, pixel[0], expected, error);
                        return 1;
                    }
                    maxError = std::max(maxError, error);
                    squaredError += error * error;
                    ++cases;
                }
            }
            for (unsigned row = 48; row < 50; ++row)
            {
                for (unsigned x = 0; x < 256; ++x)
                {
                    const auto* pixel = pixels + (row * 256 + x) * 4;
                    const float uv = 0.4f + 0.45f * x / 255.0f;
                    if (pixel[0] != 0 || std::abs(pixel[1] - uv) > 0.000001f || pixel[3] != 0)
                    {
                        throw std::runtime_error("Disabled/back-facing POM changed UV/depth");
                    }
                }
            }
        }
        const double rmsError = std::sqrt(squaredError / cases);
        std::printf("POM intersection (%s): %u cases, max depth error %.6f, RMS %.6f\n",
                    useHardware ? "hardware" : "WARP", cases, maxError, rmsError);
        if (rmsError > 0.0025)
        {
            throw std::runtime_error("Low-step POM intersection error is too large");
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
