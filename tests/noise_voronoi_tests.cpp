#include "procedural/noise.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace Shader
{
    using uint = uint32_t;
    using std::floor;
    using std::max;
    using std::min;
    using namespace ProcStudio::NoiseDetail;
    size_t featureQueries = 0;

    float Unit(uint hash)
    {
        ++featureQueries;
        return ProcStudio::NoiseDetail::Unit(hash);
    }

    uint HashXYZ(int x, int y, int z, uint seed)
    {
        return ProcStudio::NoiseDetail::Hash3(x, y, z, seed);
    }

#include "voronoi-under-test.h"
}

int main()
try
{
    uint32_t random = 1337;
    const auto coordinate = [&]() {
        random = ProcStudio::NoiseDetail::Hash(random);
        return (static_cast<int>(random % 200000) - 100000) / 1024.0f;
    };
    size_t count = 0;
    for (int period : {0, 1, 2, 7, 64})
    {
        for (int index = 0; index < 20000; ++index)
        {
            float x = coordinate();
            float y = coordinate();
            float z = coordinate();
            if (index % 16 == 1)
            {
                x *= 4096;
                y *= 4096;
                z *= 4096;
            }
            if (index % 4 == 0)
            {
                z = 0; // Material UVs are evaluated on this plane.
            }
            if (index % 8 == 0)
            {
                x = std::floor(x);
                y = std::nextafter(std::floor(y), y);
            }
            const auto seed = random;
            const float expected = ProcStudio::NoiseDetail::VoronoiCell(x, y, z, seed, period);
            const float actual = Shader::VoronoiCell(x, y, z, seed, period);
            if (expected != actual)
            {
                throw std::runtime_error("Voronoi nearest feature changed");
            }
            ++count;
        }
    }
    // Three random coordinates per feature, plus the final cell-color lookup.
    const double average = static_cast<double>(Shader::featureQueries - count) / (3 * count);
    std::cout << "Voronoi exact parity: " << count << " samples; feature queries/sample = " << average << std::endl;
    if (average > 32)
    {
        throw std::runtime_error("Voronoi still evaluates distant cells that cannot be nearest");
    }
}
catch (const std::exception& error)
{
    std::cerr << error.what() << '\n';
    return 1;
}
