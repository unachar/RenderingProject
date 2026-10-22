#include "preview-under-test.h"
#include <fstream>
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

    void SavePreview(const char* path, const std::vector<uint32_t>& pixels)
    {
        std::ofstream output(path, std::ios::binary);
        output << "P6\n512 512\n255\n";
        for (const auto pixel : pixels)
        {
            const char rgb[] = {static_cast<char>(pixel), static_cast<char>(pixel >> 8),
                                static_cast<char>(pixel >> 16)};
            output.write(rgb, sizeof(rgb));
        }
        Require(output.good(), "Preview image could not be saved");
    }
}

int main()
{
    try
    {
        ProcStudio::ModelParams parameters;
        parameters.Kind = ProcStudio::ModelKind::Terrain;
        parameters.TerrainMapsX = 3;
        parameters.TerrainMapsZ = 3;
        parameters.Noise.Frequency = 3;
        const auto mesh = ProcStudio::GenerateModel(parameters);
        TerrainPreviewTest::RenderInput input;
        input.Geometry = &mesh;
        input.View.Pitch = 0.8f;
        std::vector<uint32_t> clean;
        TerrainPreviewTest::Rasterize(input, clean);
        input.TerrainGridSize = parameters.Size;
        std::vector<uint32_t> grid;
        TerrainPreviewTest::Rasterize(input, grid);
        Require(clean.size() == 512 * 512 && grid.size() == clean.size(), "Preview dimensions changed");
        size_t changed = 0;
        for (size_t pixel = 0; pixel < clean.size(); ++pixel)
        {
            changed += clean[pixel] != grid[pixel];
        }
        Require(changed > 500 && changed < clean.size() / 4, "Grid must show visible lines without covering terrain");
        SavePreview("build/terrain-preview-tests/terrain-3x3.ppm", clean);
        SavePreview("build/terrain-preview-tests/terrain-3x3-grid.ppm", grid);

        parameters.Height = 0;
        const auto flat = ProcStudio::GenerateModel(parameters);
        input.Geometry = &flat;
        input.View.Yaw = 0;
        input.View.Pitch = 1.57079632679f;
        input.TerrainGridSize = 0;
        TerrainPreviewTest::Rasterize(input, clean);
        input.TerrainGridSize = parameters.Size;
        TerrainPreviewTest::Rasterize(input, grid);
        int lineRuns = 0;
        bool onLine = false;
        // 上面から見た平地で、左右の外周と2本の内部境界だけが描かれること。
        for (int x = 0; x < 512; ++x)
        {
            const bool changedPixel = clean[240 * 512 + x] != grid[240 * 512 + x];
            if (changedPixel && !onLine)
            {
                ++lineRuns;
            }
            onLine = changedPixel;
        }
        Require(lineRuns == 4, "3x3 terrain must show two internal X boundaries and two outer edges");
        input.TerrainGridSize = 0;
        TerrainPreviewTest::Rasterize(input, grid);
        Require(clean == grid, "Disabling the grid must restore the original preview");
        std::cout << "Terrain preview grid tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
