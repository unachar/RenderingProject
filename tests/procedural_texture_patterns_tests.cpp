#include "procedural/proceduralstudio.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    bool Near(float a, float b)
    {
        return std::abs(a - b) < 0.001f;
    }
}

void RunTests()
{
    using namespace ProcStudio;
    for (int basis = 0; basis < static_cast<int>(Basis::Count); ++basis)
    {
        for (bool tileable : {false, true})
        {
            NoiseParams noise;
            noise.BasisType = static_cast<Basis>(basis);
            noise.Fractal = FractalType::None;
            noise.Frequency = 3.5f;
            for (int sample = -8; sample <= 8; ++sample)
            {
                const float x = sample * 0.137f;
                const float y = sample * 0.219f;
                const float z = sample * 0.071f;
                const int period = tileable ? 4 : 0;
                const float frequency = tileable ? 4.0f : noise.Frequency;
                const float expected =
                    NoiseDetail::Saturate(BasisNoise(noise.BasisType, x * frequency, y * frequency, z * frequency,
                                                     NoiseDetail::Hash(noise.Seed), period) *
                                              0.5f +
                                          0.5f);
                Require(Near(FractalNoise(noise, x, y, z, tileable), expected),
                        "none must render the normalized base noise without octave synthesis");
                NoiseParams changed = noise;
                changed.Octaves = 10;
                changed.Lacunarity = 3.7f;
                changed.Gain = 0.9f;
                Require(Near(FractalNoise(changed, x, y, z, tileable), expected),
                        "none must ignore fractal synthesis settings");
                if (tileable)
                {
                    Require(Near(FractalNoise(noise, x + 1, y, z, true), expected) &&
                                Near(FractalNoise(noise, x, y + 1, z, true), expected) &&
                                Near(FractalNoise(noise, x, y, z + 1, true), expected),
                            "base noise must tile on all three axes");
                }
            }
        }
    }
    // Exercise the public Studio paths, not only the header evaluator.
    for (int basis = 0; basis < static_cast<int>(Basis::Count); ++basis)
    {
        TextureParams texture;
        texture.Noise.BasisType = static_cast<Basis>(basis);
        texture.Noise.Fractal = FractalType::None;
        texture.Noise.Frequency = 3.5f;
        texture.Noise.Phase = 0.7f;
        for (TextureOutput output : {TextureOutput::Color, TextureOutput::Height, TextureOutput::Normal})
        {
            texture.Output = output;
            const auto image = GenerateTexture(texture, 8, 8);
            Require(image.Pixels.size() == 64, "noise texture dimensions");
            for (const auto& pixel : image.Pixels)
            {
                Require(std::isfinite(pixel.X) && std::isfinite(pixel.Y) && std::isfinite(pixel.Z),
                        "noise texture outputs must be finite");
            }
        }
        const auto volume = GenerateVolume(texture, 8);
        Require(volume.Density.size() == 512, "noise volume dimensions");
        for (float value : volume.Density)
        {
            Require(std::isfinite(value) && value >= 0 && value <= 1, "noise volume density range");
        }
        ModelParams terrain;
        terrain.Kind = ModelKind::Terrain;
        terrain.Noise = texture.Noise;
        terrain.Resolution = 8;
        const auto mesh = GenerateModel(terrain);
        Require(!mesh.Positions.empty() && !mesh.Indices.empty(), "noise terrain must generate geometry");
        for (const auto& position : mesh.Positions)
        {
            Require(std::isfinite(position.Y), "noise terrain heights must be finite");
        }
    }
    TextureParams params;
    params.Kind = TextureKind::Geometric;
    Require(TextureScalar(params, 0.125f, 0.0625f, 0, false) > 0.99f,
            "geometric pattern center must be filled by default");
    Require(TextureScalar(params, 0.01f, 0.0625f, 0, false) < 0.01f,
            "geometric pattern background must be empty by default");
    Require(static_cast<int>(TextureKind::Julia) == 10, "existing texture kind values must be preserved");
    params.RepeatX = 4;
    params.RepeatY = 4;
    for (int pattern = 0; pattern < static_cast<int>(GeometricPattern::Count); ++pattern)
    {
        params.Pattern = static_cast<GeometricPattern>(pattern);
        const bool grid = params.Pattern == GeometricPattern::Grid;
        Require(Near(TextureScalar(params, 0.125f, 0.125f, 0, false), grid ? 0.0f : 1.0f),
                "shape centers and grid interiors must match their geometry");
        Require(Near(TextureScalar(params, 0.001f, 0.001f, 0, false), grid ? 1.0f : 0.0f),
                "cell corners must distinguish grid lines from isolated shapes");
        for (int i = -23; i < 79; ++i)
        {
            const float x = i * 0.0173f;
            const float y = i * 0.0317f;
            const float value = TextureScalar(params, x, y, 0, false);
            Require(std::isfinite(value) && value >= 0 && value <= 1, "pattern values must be finite and normalized");
            Require(Near(value, TextureScalar(params, x + 1, y, 0, false)) &&
                        Near(value, TextureScalar(params, x, y + 1, 0, false)),
                    "patterns must repeat along both axes");
            Require(Near(TextureScalar(params, 0, y, 0, false), TextureScalar(params, 1, y, 0, false)) &&
                        Near(TextureScalar(params, x, 0, 0, false), TextureScalar(params, x, 1, 0, false)),
                    "opposite texture boundaries must join");
        }
        params.Output = TextureOutput::Height;
        const auto small = GenerateTexture(params, 48, 48);
        params.PatternSize = 0.8f;
        const auto large = GenerateTexture(params, 48, 48);
        float smallArea = 0;
        float largeArea = 0;
        for (size_t i = 0; i < small.Pixels.size(); ++i)
        {
            smallArea += small.Pixels[i].X;
            largeArea += large.Pixels[i].X;
        }
        Require(largeArea > smallArea, "size control must increase covered area for every pattern");
        params.PatternSize = 0.95f;
        params.PatternSoftness = 0.15f;
        Require(std::abs(TextureScalar(params, 0.125f, -0.00001f, 0, false) -
                         TextureScalar(params, 0.125f, 0.00001f, 0, false)) < 0.005f,
                "large soft patterns must remain continuous across the texture seam");
        for (int sample = 0; sample <= 32; ++sample)
        {
            const float position = sample / 32.0f;
            Require(std::abs(TextureScalar(params, position, -0.00001f, 0, false) -
                             TextureScalar(params, position, 0.00001f, 0, false)) < 0.005f &&
                        std::abs(TextureScalar(params, -0.00001f, position, 0, false) -
                                 TextureScalar(params, 0.00001f, position, 0, false)) < 0.005f,
                    "soft patterns must be continuous along the entire texture boundary");
        }
        params.PatternSize = 0.5f;
        params.PatternSoftness = 0.02f;
        params.ColorA = {0.1f, 0.2f, 0.3f};
        params.ColorB = {0.8f, 0.7f, 0.6f};
        const auto center = TextureColor(params, 0.125f, 0.125f, 0, false);
        Require(Near(center.X, grid ? 0.1f : 0.8f) && Near(center.Z, grid ? 0.3f : 0.6f),
                "background and pattern colors must use the selected colors");
        for (auto output : {TextureOutput::Color, TextureOutput::Height, TextureOutput::Normal})
        {
            params.Output = output;
            const auto image = GenerateTexture(params, 37, 29);
            Require(image.Width == 37 && image.Height == 29 && image.Pixels.size() == 37 * 29,
                    "all output modes must preserve requested dimensions");
            for (const auto& pixel : image.Pixels)
            {
                Require(std::isfinite(pixel.X) && pixel.X >= 0 && pixel.X <= 1 && std::isfinite(pixel.Y) &&
                            pixel.Y >= 0 && pixel.Y <= 1 && std::isfinite(pixel.Z) && pixel.Z >= 0 && pixel.Z <= 1 &&
                            pixel.W == 1,
                        "generated pixels must be finite opaque normalized colors");
                if (output == TextureOutput::Height)
                {
                    Require(Near(pixel.X, pixel.Y) && Near(pixel.Y, pixel.Z), "height output must be grayscale");
                }
                if (output == TextureOutput::Normal)
                {
                    const float nx = pixel.X * 2 - 1;
                    const float ny = pixel.Y * 2 - 1;
                    const float nz = pixel.Z * 2 - 1;
                    Require(Near(nx * nx + ny * ny + nz * nz, 1), "normal output must contain unit vectors");
                }
            }
        }
    }
    params.Pattern = GeometricPattern::Stripes;
    params.PatternSoftness = 0.1f;
    Require(Near(TextureScalar(params, 0.0625f, 0.125f, 0, false), 0.5f), "soft boundary must blend at the edge");
    params.RepeatX = 0;
    params.RepeatY = -1;
    params.PatternSize = 0;
    params.PatternSoftness = 0;
    Require(std::isfinite(TextureScalar(params, -0.5f, -0.5f, 0, false)), "out-of-range settings must be clamped");
    std::cout << "22 noise bases: textures, volumes, terrain; six geometric patterns: all checks passed\n";
}

int main()
{
    try
    {
        RunTests();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
