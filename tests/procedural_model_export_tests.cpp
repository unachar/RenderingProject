#include "editor/proceduralmodelexport.h"
#include <Importer.hpp>
#include <material.h>
#include <scene.h>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace
{
    void Require(bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    void CheckTerrainMap(int mapsX, int mapsZ)
    {
        ProcStudio::ModelParams parameters;
        parameters.Kind = ProcStudio::ModelKind::Terrain;
        parameters.Resolution = 8;
        Require(parameters.TerrainMapsX == 1 && parameters.TerrainMapsZ == 1, "Existing terrain must default to 1x1");
        parameters.TerrainMapsX = mapsX;
        parameters.TerrainMapsZ = mapsZ;
        const auto mesh = ProcStudio::GenerateModel(parameters);
        const int stepsX = parameters.Resolution * mapsX;
        const int stepsZ = parameters.Resolution * mapsZ;
        Require(mesh.Positions.size() == static_cast<size_t>(stepsX + 1) * (stepsZ + 1),
                "Terrain borders must share vertices");
        Require(mesh.Indices.size() == static_cast<size_t>(stepsX) * stepsZ * 6,
                "Terrain map triangle count is incorrect");
        auto noise = parameters.Noise;
        noise.Seed ^= parameters.Seed;
        for (int z = 0; z <= stepsZ; ++z)
        {
            for (int x = 0; x <= stepsX; ++x)
            {
                const size_t index = static_cast<size_t>(z) * (stepsX + 1) + x;
                const float u = static_cast<float>(x) / parameters.Resolution;
                const float v = static_cast<float>(z) / parameters.Resolution;
                const auto& position = mesh.Positions[index];
                Require(std::abs(position.X - (u - mapsX * 0.5f) * parameters.Size) < 0.0001f &&
                            std::abs(position.Z - (v - mapsZ * 0.5f) * parameters.Size) < 0.0001f,
                        "Terrain maps must retain cell size and be centered");
                Require(std::abs(position.Y - ProcStudio::FractalNoise(noise, u, v, 0.37f, false) * parameters.Height) <
                            0.0001f,
                        "Terrain maps must sample continuous noise without stretching or repeating");
                Require(std::abs(mesh.UVs[index].X - u) < 0.0001f && std::abs(mesh.UVs[index].Y - v) < 0.0001f,
                        "Terrain texture density must stay constant across maps");
                const auto& normal = mesh.Normals[index];
                Require(std::isfinite(normal.X) && std::isfinite(normal.Y) && std::isfinite(normal.Z) && normal.Y > 0,
                        "Terrain normals must be finite and face upward");
            }
        }
        for (int z = 0; z < stepsZ; ++z)
        {
            for (int x = 0; x < stepsX; ++x)
            {
                const uint32_t a = z * (stepsX + 1) + x;
                const uint32_t b = a + stepsX + 1;
                const size_t offset = (static_cast<size_t>(z) * stepsX + x) * 6;
                const uint32_t expected[] = {a, b, a + 1, a + 1, b, b + 1};
                for (size_t corner = 0; corner < 6; ++corner)
                {
                    Require(mesh.Indices[offset + corner] == expected[corner],
                            "Terrain topology must stay connected across map boundaries");
                }
            }
        }
    }

    void CheckFbx(const std::filesystem::path& path, const ProcStudio::Mesh& mesh, bool vertexColors)
    {
        std::string error;
        const bool saved =
            ProceduralIO::SaveModelFile(path, mesh, vertexColors, vertexColors ? "" : "albedo.png", error);
        Require(saved, error);
        std::ifstream input(path, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        Require(bytes.rfind("; FBX", 0) == 0, "Expected ASCII FBX header");
        Assimp::Importer importer;
        const auto* scene = importer.ReadFileFromMemory(bytes.data(), bytes.size(), 0, "fbx");
        Require(scene && scene->mNumMeshes == 1, importer.GetErrorString());
        const auto& imported = *scene->mMeshes[0];
        Require(imported.mNumFaces == mesh.Indices.size() / 3, "Triangle count changed");
        Require(imported.HasNormals() && imported.HasTextureCoords(0), "Missing normals or UVs");
        Require(imported.HasVertexColors(0) == vertexColors, "Incorrect vertex color mode");
        // FBX の再読込で頂点が分割されても、三角形の順序と各頂点の属性が一致すること。
        for (size_t corner = 0; corner < mesh.Indices.size(); ++corner)
        {
            const auto source = mesh.Indices[corner];
            const auto target = imported.mFaces[corner / 3].mIndices[corner % 3];
            const auto& p = imported.mVertices[target];
            const auto& n = imported.mNormals[target];
            const auto& uv = imported.mTextureCoords[0][target];
            const auto near = [](float a, float b) {
                return std::abs(a - b) < 0.0001f;
            };
            Require(near(p.x, mesh.Positions[source].X) && near(p.y, mesh.Positions[source].Y) &&
                        near(p.z, mesh.Positions[source].Z),
                    "Position or winding changed");
            Require(near(n.x, mesh.Normals[source].X) && near(n.y, mesh.Normals[source].Y) &&
                        near(n.z, mesh.Normals[source].Z),
                    "Normal changed");
            Require(near(uv.x, mesh.UVs[source].X) && near(uv.y, 1 - mesh.UVs[source].Y), "UV changed");
            if (vertexColors)
            {
                const auto& color = imported.mColors[0][target];
                Require(near(color.r, mesh.Colors[source].X) && near(color.g, mesh.Colors[source].Y) &&
                            near(color.b, mesh.Colors[source].Z),
                        "Vertex color changed");
            }
        }
        if (!vertexColors)
        {
            aiString texture;
            Require(scene->mMaterials[imported.mMaterialIndex]->GetTexture(aiTextureType_DIFFUSE, 0, &texture) ==
                            AI_SUCCESS &&
                        std::string(texture.C_Str()) == "albedo.png",
                    "Texture reference lost");
        }
    }
}

int main()
{
    try
    {
        const std::filesystem::path root = "build/procedural-export-tests/output";
        std::filesystem::create_directories(root);
        CheckTerrainMap(1, 1);
        CheckTerrainMap(3, 3);
        CheckTerrainMap(3, 2);
        CheckTerrainMap(1, 3);
        CheckTerrainMap(4, 4);
        ProcStudio::Mesh mesh;
        mesh.Positions = {{0, 0, 0}, {2, 0, 0}, {0, 3, 0}};
        mesh.Normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
        mesh.Colors = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        mesh.UVs = {{0, 0.2f}, {2, 0.3f}, {0, 3}};
        mesh.Indices = {0, 1, 2};
        CheckFbx(root / L"頂点カラー.FBX", mesh, true);
        CheckFbx(root / "texture.fbx", mesh, false);
        for (auto kind :
             {ProcStudio::ModelKind::FractalTree, ProcStudio::ModelKind::Terrain, ProcStudio::ModelKind::Sphere})
        {
            ProcStudio::ModelParams parameters;
            parameters.Kind = kind;
            parameters.Depth = 2;
            parameters.Leaves = false;
            parameters.Resolution = 8;
            if (kind == ProcStudio::ModelKind::Terrain)
            {
                parameters.TerrainMapsX = 3;
                parameters.TerrainMapsZ = 3;
            }
            CheckFbx(root / "cpu-generated.fbx", ProcStudio::GenerateModel(parameters), true);
        }
        ProcStudio::ModelParams terrain;
        terrain.Kind = ProcStudio::ModelKind::Terrain;
        terrain.Resolution = 8;
        terrain.TerrainMapsX = 3;
        terrain.TerrainMapsZ = 3;
        const auto terrainMesh = ProcStudio::GenerateModel(terrain);
        CheckFbx(root / "terrain-3x3.fbx", terrainMesh, true);
        CheckFbx(root / "terrain-3x3-texture.fbx", terrainMesh, false);
        std::string terrainError;
        Require(ProceduralIO::SaveModelFile(root / "terrain-3x3.obj", terrainMesh, true, "", terrainError),
                terrainError);
        Assimp::Importer terrainImporter;
        const auto* importedTerrain = terrainImporter.ReadFile((root / "terrain-3x3.obj").string(), 0);
        Require(importedTerrain && importedTerrain->mNumMeshes == 1, "OBJ terrain must export as one mesh");
        Require(importedTerrain->mMeshes[0]->mNumFaces == terrainMesh.Indices.size() / 3,
                "OBJ terrain triangle count changed");
        const auto& importedMesh = *importedTerrain->mMeshes[0];
        for (size_t corner = 0; corner < terrainMesh.Indices.size(); ++corner)
        {
            const auto& position = terrainMesh.Positions[terrainMesh.Indices[corner]];
            const auto& importedPosition = importedMesh.mVertices[importedMesh.mFaces[corner / 3].mIndices[corner % 3]];
            Require(std::abs(position.X - importedPosition.x) < 0.0001f &&
                        std::abs(position.Y - importedPosition.y) < 0.0001f &&
                        std::abs(position.Z - importedPosition.z) < 0.0001f,
                    "OBJ terrain shape or winding changed");
        }
        terrain.TerrainMapsX = -1;
        terrain.TerrainMapsZ = 0;
        Require(ProcStudio::GenerateModel(terrain).Positions.size() == 81, "Invalid map counts must clamp to 1x1");
        terrain.TerrainMapsX = 1000000;
        terrain.TerrainMapsZ = 1000000;
        const size_t maxSteps = terrain.Resolution * ProcStudio::kMaxTerrainMaps;
        Require(ProcStudio::GenerateModel(terrain).Positions.size() == (maxSteps + 1) * (maxSteps + 1),
                "Terrain map counts must be bounded before allocation");
        terrain.TerrainMapsX = 3;
        terrain.TerrainMapsZ = 3;
        terrain.Resolution = 96;
        const auto fullTerrain = ProcStudio::GenerateModel(terrain);
        Require(fullTerrain.Positions.size() == 83521 && fullTerrain.Indices.size() / 3 == 165888,
                "Default resolution 3x3 terrain counts changed");
        std::string error;
        Require(ProceduralIO::SaveModelFile(root / "legacy.obj", mesh, true, "", error), error);
        Require(ProcStudio::IsProceduralObj(root / "legacy.obj"), "OBJ marker lost");
        Require(std::filesystem::exists(root / "legacy.mtl"), "OBJ material lost");
        Require(!ProceduralIO::SaveModelFile(root / "unsupported.xyz", mesh, true, "", error) && !error.empty(),
                "Unsupported extension must fail");
        Require(!ProceduralIO::SaveModelFile(root / "missing/file.fbx", mesh, true, "", error) && !error.empty(),
                "Unwritable output must fail");
        Require(!ProceduralIO::SaveModelFile(root / "empty.fbx", {}, true, "", error) && !error.empty(),
                "Empty geometry must fail");
        mesh.Indices[2] = 99;
        Require(!ProceduralIO::SaveModelFile(root / "invalid.fbx", mesh, true, "", error) && !error.empty(),
                "Invalid geometry must fail");
        std::cout << "Procedural CPU model OBJ/FBX export tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
