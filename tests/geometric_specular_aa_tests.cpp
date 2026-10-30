#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include "../shader/hlsl/GeometricSpecularAA.hlsli"

int main()
{
    using namespace GeometricSpecularAA;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    unsigned cases = 0;
    for (int step = 0; step <= 100; ++step)
    {
        const float roughness = float(step) / 100.0f;
        assert(FilterRoughness(roughness, 0, 0.15f, 0.2f) == roughness);
        assert(FilterRoughness(roughness, 1, 0, 0.2f) == roughness);
        assert(FilterRoughness(roughness, 1, 0.15f, 0) == roughness);
        assert(FilterRoughness(roughness, nan, 0.15f, 0.2f) == roughness);
        assert(FilterRoughness(roughness, infinity, 0.15f, 0.2f) == roughness);
        float previous = roughness;
        for (int gradient = 0; gradient <= 100; ++gradient)
        {
            const float filtered = FilterRoughness(roughness, float(gradient) / 100.0f, 0.15f, 0.2f);
            const double expected = std::pow(std::fmin(1.0, std::pow(double(roughness), 4.0) +
                std::fmin(0.3 * double(gradient) / 100.0, 0.2)), 0.25);
            assert(std::isfinite(filtered) && filtered >= roughness && filtered <= 1);
            assert(filtered + 1e-6f >= previous);
            assert(std::abs(double(filtered) - expected) < 2e-6);
            previous = filtered;
            ++cases;
        }
    }
    assert(FilterRoughness(1, 100, 0.15f, 0.2f) == 1);
    assert(FilterRoughness(0.5f, 1, nan, 0.2f) == 0.5f);
    assert(FilterRoughness(0.5f, 1, 0.15f, infinity) == 0.5f);
    assert(FilterRoughness(0.5f, 1, -1, 0.2f) == 0.5f);
    assert(FilterRoughness(nan, 1, 0.15f, 0.2f) == 0);
    assert(FiniteGradientSquared(nan, 0.25f) == 0.25f);
    assert(FiniteGradientSquared(0.25f, infinity) == 0.25f);
    assert(FiniteGradientSquared(-1, 0.25f) == 0.25f);
    for (int material = 0; material <= 14; ++material)
    {
        assert(EligibleMaterial(float(material)) == (material == 11));
    }
    printf("Geometric specular AA: %u reference/monotonicity cases and boundary checks PASS\n", cases);
}
