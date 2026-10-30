#include "pch.h"
#include "referenceimage.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ReferenceImageIO
{
    namespace
    {
        std::FILE* OpenBinary(const char* path)
        {
            std::FILE* file = nullptr;
#ifdef _MSC_VER
            fopen_s(&file, path, "wb");
#else
            file = std::fopen(path, "wb");
#endif
            return file;
        }
    } // namespace

    bool WritePfm(const char* path, const ReferenceImage& image)
    {
        if (path == nullptr || image.Pixels.empty() || image.Width == 0 || image.Height == 0)
        {
            return false;
        }
        std::FILE* file = OpenBinary(path);
        if (file == nullptr)
        {
            return false;
        }
        std::fprintf(file, "PF\n%u %u\n-1.0\n", image.Width, image.Height);
        for (uint32_t y = image.Height; y-- > 0;)
        {
            const float* row = &image.Pixels[static_cast<size_t>(y) * image.Width * 4];
            for (uint32_t x = 0; x < image.Width; ++x)
            {
                const float rgb[3] = {row[x * 4 + 0], row[x * 4 + 1], row[x * 4 + 2]};
                std::fwrite(rgb, sizeof(float), 3, file);
            }
        }
        const bool ok = std::ferror(file) == 0;
        std::fclose(file);
        return ok;
    }

    bool WritePpmPreview(const char* path, const ReferenceImage& image, float exposure)
    {
        if (path == nullptr || image.Pixels.empty() || image.Width == 0 || image.Height == 0)
        {
            return false;
        }
        std::FILE* file = OpenBinary(path);
        if (file == nullptr)
        {
            return false;
        }
        std::fprintf(file, "P6\n%u %u\n255\n", image.Width, image.Height);
        for (size_t i = 0; i < static_cast<size_t>(image.Width) * image.Height; ++i)
        {
            uint8_t rgb[3];
            for (int c = 0; c < 3; ++c)
            {
                const float linear = std::max(image.Pixels[i * 4 + c], 0.0f) * exposure;
                const float srgb = std::pow(linear / (1.0f + linear), 1.0f / 2.2f);
                rgb[c] = static_cast<uint8_t>(std::min(srgb, 1.0f) * 255.0f + 0.5f);
            }
            std::fwrite(rgb, 1, 3, file);
        }
        const bool ok = std::ferror(file) == 0;
        std::fclose(file);
        return ok;
    }
} // namespace ReferenceImageIO
