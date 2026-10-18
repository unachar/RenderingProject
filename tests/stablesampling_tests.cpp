#include "rendering/stablesampling.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
int main()
{
    try
    {
        for (unsigned pixel = 0; pixel < 64; ++pixel)
        {
            double sumX = 0, sumY = 0;
            for (unsigned index = 0; index < 256; ++index)
            {
                const auto sample = StableSampling::SampleSequence(pixel, pixel * 7, index, 3);
                const auto replay = StableSampling::SampleSequence(pixel, pixel * 7, index, 3);
                Check(sample.x >= 0 && sample.x < 1 && sample.y >= 0 && sample.y < 1, "sample domain");
                Check(sample.x == replay.x && sample.y == replay.y, "exact replay");
                sumX += sample.x;
                sumY += sample.y;
                Check(StableSampling::PixelSampleIndex(index * 2, true) == index &&
                    StableSampling::PixelSampleIndex(index * 2 + 1, true) == index, "both checkerboard parities cover the whole sequence");
            }
            Check(std::abs(sumX / 256 - .5) < .015 && std::abs(sumY / 256 - .5) < .015, "no hemisphere-domain bias");
            const auto first = StableSampling::SampleSequence(pixel, pixel * 7, 0, 3);
            const auto nextDomain = StableSampling::SampleSequence(pixel, pixel * 7, 0, 4);
            Check(first.x != nextDomain.x || first.y != nextDomain.y, "independent sampling domains");
        }
        for (unsigned pixel = 0; pixel < 8; ++pixel)
        {
            double sumU = 0, sumV = 0;
            unsigned countU = 0, countV = 0;
            for (unsigned index = 0; index < 4096; ++index)
            {
                const auto selection = StableSampling::SampleSequence(pixel, 9, index, 1);
                const auto coordinates = StableSampling::SampleSequence(pixel, 9, index, 2);
                if (selection.x < .5f) { sumU += coordinates.x; ++countU; }
                if (selection.y < .5f) { sumV += coordinates.y; ++countV; }
            }
            Check(countU > 1900 && countV > 1900, "selection domain coverage");
            Check(std::abs(sumU / countU - .5) < .03 && std::abs(sumV / countV - .5) < .03,
                "emitter and triangle selection must not bias barycentric coordinates");
        }
        std::cout << "stable sampling tests: PASS\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
