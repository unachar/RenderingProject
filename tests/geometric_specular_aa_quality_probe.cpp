// Numerical quality experiment, not a renderer performance or full-BRDF benchmark.
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include "../shader/hlsl/GeometricSpecularAA.hlsli"

namespace
{
    using Normal = std::array<double, 3>;
    constexpr double Pi = 3.14159265358979323846;

    Normal GeometryNormal(double x, double y, double slope)
    {
        const double length = std::sqrt(1 + slope * slope * (x * x + y * y));
        return {slope * x / length, 1 / length, slope * y / length};
    }

    double DifferenceSquared(const Normal& a, const Normal& b)
    {
        double sum = 0;
        for (unsigned component = 0; component < 3; ++component)
        {
            const double delta = a[component] - b[component];
            sum += delta * delta;
        }
        return sum;
    }

    // GGX NDF for H=(0,1,0), with n=normalize(slope*x,1,slope*y).
    // This stable rearrangement avoids cancellation at low perceptual roughness.
    double Distribution(double x, double y, double slope, double roughness)
    {
        const double alphaSquared = std::pow(roughness, 4);
        const double radius = slope * slope * (x * x + y * y);
        const double denominator = alphaSquared + radius;
        return alphaSquared * (1 + radius) * (1 + radius) / (Pi * denominator * denominator);
    }

    double PixelIntegral(double x, double y, double slope, double roughness, unsigned samples)
    {
        double sum = 0;
        for (unsigned sy = 0; sy < samples; ++sy)
        {
            const double sampleY = y - .5 + (sy + .5) / samples;
            for (unsigned sx = 0; sx < samples; ++sx)
            {
                const double sampleX = x - .5 + (sx + .5) / samples;
                sum += Distribution(sampleX, sampleY, slope, roughness);
            }
        }
        return sum / (double(samples) * samples);
    }
}

int main(int argc, char** argv)
{
    try
    {
        if (argc != 2) throw std::runtime_error("Expected output CSV path");
        std::ofstream output(argv[1]);
        if (!output) throw std::runtime_error("Cannot write quality probe CSV");
        output.precision(12);
        output << "normal_slope,roughness,off_normalized_rmse,on_normalized_rmse,"
                  "off_integrated_ratio,on_integrated_ratio,reference_convergence_relative\n";
        for (double slope : {.02, .06})
        {
            for (double roughness : {.04, .12, .3, .8})
            {
                // Check that a finer quadrature resolves the narrowest central highlight.
                const double reference256 = PixelIntegral(.015625, .013, slope, roughness, 256);
                const double reference512 = PixelIntegral(.015625, .013, slope, roughness, 512);
                const double convergence = std::abs(reference256 - reference512) / reference512;
                if (!std::isfinite(convergence) || convergence > .01)
                    throw std::runtime_error("Pixel reference has not converged within 1% at the central probe");
                double referenceSquared = 0, offError = 0, onError = 0;
                double referenceSum = 0, offSum = 0, onSum = 0;
                for (unsigned frame = 0; frame < 32; ++frame)
                {
                    const double phase = (frame + .5) / 32 - .5;
                    for (int py = -3; py <= 3; ++py)
                    {
                        for (int px = -3; px <= 3; ++px)
                        {
                            const double x = px + phase, y = py + .013;
                            const Normal center = GeometryNormal(x, y, slope);
                            const float gradient = static_cast<float>(
                                DifferenceSquared(center, GeometryNormal(x + 1, y, slope)) +
                                DifferenceSquared(center, GeometryNormal(x, y + 1, slope)));
                            const float filtered = GeometricSpecularAA::FilterRoughness(
                                static_cast<float>(roughness), gradient, .15f, .2f);
                            const double reference = PixelIntegral(x, y, slope, roughness, 256);
                            const double off = Distribution(x, y, slope, roughness);
                            const double on = Distribution(x, y, slope, filtered);
                            if (!std::isfinite(reference + off + on))
                                throw std::runtime_error("Nonfinite GGX distribution");
                            referenceSquared += reference * reference;
                            offError += (off - reference) * (off - reference);
                            onError += (on - reference) * (on - reference);
                            referenceSum += reference;
                            offSum += off;
                            onSum += on;
                        }
                    }
                }
                const double offRmse = std::sqrt(offError / referenceSquared);
                const double onRmse = std::sqrt(onError / referenceSquared);
                output << slope << ',' << roughness << ',' << offRmse << ',' << onRmse << ','
                       << offSum / referenceSum << ',' << onSum / referenceSum << ',' << convergence << '\n';
                std::printf("slope %.2f roughness %.2f: normalized RMSE OFF %.6f / ON %.6f, "
                            "integrated ratio OFF %.6f / ON %.6f, reference delta %.6g\n",
                            slope, roughness, offRmse, onRmse, offSum / referenceSum,
                            onSum / referenceSum, convergence);
            }
        }
        output.flush();
        if (!output) throw std::runtime_error("Quality probe output write failed");
        std::puts("GGX NDF quadrature experiment completed; no full-frame, temporal, or FPS claim.");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
