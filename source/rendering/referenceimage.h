#pragma once
#include <cstdint>
#include <vector>

// Stage 2 H: a finished reference image (linear HDR mean radiance)
// plus its file writers. Pure CPU so the formats can be unit-tested
// without a GPU.
struct ReferenceImage
{
    std::vector<float> Pixels; // RGBA32F, row-major, row 0 = image top
    uint32_t Width = 0;
    uint32_t Height = 0;
    uint32_t Samples = 0;
};

namespace ReferenceImageIO
{
    // PFM (PF, little-endian) keeping linear HDR; rows are written
    // bottom-first per the PFM spec.
    bool WritePfm(const char* path, const ReferenceImage& image);
    // Binary PPM preview: Reinhard + gamma 2.2 at the given exposure.
    bool WritePpmPreview(const char* path, const ReferenceImage& image, float exposure = 1.0f);
} // namespace ReferenceImageIO
