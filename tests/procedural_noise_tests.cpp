#include "../source/procedural/noise.h"
#include <cstdlib>
#include <iostream>

using namespace ProcStudio;

static void Require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

int main()
{
    static_assert(static_cast<int>(Basis::CellEdge) == 3);
    static_assert(static_cast<int>(FractalType::None) == 6);
    static_assert(static_cast<int>(Basis::Count) == 22);
    static_assert(static_cast<int>(FractalType::Count) == 9);
    const Basis added[] = {Basis::White,       Basis::Simplex,       Basis::Voronoi,
                           Basis::Cell,        Basis::Gabor,         Basis::Wavelet,
                           Basis::Curl,        Basis::ValueGradient, Basis::SparseConvolution,
                           Basis::Spot,        Basis::LocalRandom,   Basis::Chevron,
                           Basis::LatticeFree, Basis::Phasor,        Basis::AnisotropicGabor,
                           Basis::Harmonic,    Basis::Flow,          Basis::Ridged};
    for (Basis basis : added)
    {
        NoiseParams p;
        p.BasisType = basis;
        p.Fractal = FractalType::None;
        p.Frequency = 3;
        bool varying = false, seeded = false;
        float first = FractalNoise(p, 0.123f, -0.321f, 0.42f, false);
        for (int i = 0; i < 32; ++i)
        {
            const float x = -0.83f + i * 0.071f, y = 0.31f + i * 0.037f, z = -0.27f + i * 0.019f;
            const float a = FractalNoise(p, x, y, z, false);
            Require(std::isfinite(a) && a >= 0 && a <= 1, "Noise range/finite regression");
            Require(a == FractalNoise(p, x, y, z, false), "Noise must be deterministic");
            varying |= std::abs(a - first) > 1e-4f;
            NoiseParams q = p;
            q.Seed++;
            seeded |= std::abs(a - FractalNoise(q, x, y, z, false)) > 1e-4f;
            // Binary fractions avoid float rounding across white/cell discontinuities.
            const float tx = -0.375f + i * 0.03125f, ty = 0.1875f, tz = -0.0625f;
            const float t = FractalNoise(p, tx, ty, tz, true);
            for (int axis = 0; axis < 3; ++axis)
            {
                Require(std::abs(t - FractalNoise(p, tx + (axis == 0), ty + (axis == 1), tz + (axis == 2), true)) <
                            0.003f,
                        "Tileable noise must repeat on all three axes");
            }
            if (basis != Basis::White && basis != Basis::Cell && basis != Basis::Voronoi)
            {
                Require(std::abs(FractalNoise(p, -1e-5f, y, z, true) - FractalNoise(p, 1e-5f, y, z, true)) < 0.02f,
                        "Continuous basis must not introduce a tile seam");
            }
        }
        Require(varying, "Added basis must not be constant");
        Require(seeded, "Added basis must depend on the seed");
    }
    for (FractalType type : {FractalType::Multifractal, FractalType::HeterogeneousMultifractal})
    {
        NoiseParams p;
        p.Fractal = type;
        for (int i = 0; i < 20; ++i)
        {
            const float x = i * 0.037f;
            const float a = FractalNoise(p, x, 0.24f, -0.32f, true);
            Require(std::isfinite(a) && a >= 0 && a <= 1, "Multifractal range");
            Require(std::abs(a - FractalNoise(p, x + 1, 0.24f, -0.32f, true)) < 1e-4f, "Multifractal tile periodicity");
        }
        NoiseParams q = p;
        q.Fractal = FractalType::FBm;
        Require(std::abs(FractalNoise(p, .123f, .456f, .789f, false) - FractalNoise(q, .123f, .456f, .789f, false)) >
                    1e-4f,
                "Multifractal must differ from fBm");
    }
    // Divergence of a curl must vanish (within finite difference error).
    const float e = 0.002f;
    float divergence = 0;
    for (int axis = 0; axis < 3; ++axis)
    {
        auto a = NoiseDetail::CurlVector(.37f + (axis == 0) * e, .29f + (axis == 1) * e, .61f + (axis == 2) * e, 17, 0);
        auto b = NoiseDetail::CurlVector(.37f - (axis == 0) * e, .29f - (axis == 1) * e, .61f - (axis == 2) * e, 17, 0);
        divergence += (a[axis] - b[axis]) / (2 * e);
    }
    Require(std::abs(divergence) < 0.02f, "Curl must be divergence-free");
    NoiseParams p;
    p.Fractal = FractalType::None;
    for (Basis basis : {Basis::Gabor, Basis::AnisotropicGabor, Basis::Phasor, Basis::LocalRandom, Basis::Flow})
    {
        p.BasisType = basis;
        const float a = FractalNoise(p, .132f, .267f, .323f, false);
        p.Phase = .7f;
        Require(std::abs(a - FractalNoise(p, .132f, .267f, .323f, false)) > 1e-5f,
                "Phase control must affect phase-based noises");
        p.Phase = 0;
    }
    std::cout << "Procedural noise tests passed (22 bases, 9 fractal modes).\n";
}
