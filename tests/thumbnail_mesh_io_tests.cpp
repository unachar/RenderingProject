#include "rendering/thumbnailmeshio.h"
#include <cimport.h>
#include <postprocess.h>
#include <scene.h>
#include <cassert>
#include <fstream>
#include <iostream>

int main()
{
    const auto root = std::filesystem::absolute("build/thumbnail-tests/io-fixture");
    std::filesystem::create_directories(root);
    std::ofstream(root / "mesh.obj") << "mtllib arbitrary.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl Color\nf 1 2 3\n";
    std::ofstream(root / "arbitrary.mtl") << "newmtl Color\nKd 0.8 0.2 0.1\n";
    std::vector<std::filesystem::path> files;
    ThumbnailMeshIO io(files);
    auto callbacks = io.Callbacks();
    const auto* scene = aiImportFileEx((root / "mesh.obj").string().c_str(), aiProcess_Triangulate, &callbacks);
    assert(scene && scene->mNumMeshes == 1);
    assert(std::find(files.begin(), files.end(), root / "arbitrary.mtl") != files.end());
    aiReleaseImport(scene);
    const float positions[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const uint16_t indices[] = {0, 1, 2};
    std::ofstream buffer(root / "geometry.bin", std::ios::binary);
    buffer.write(reinterpret_cast<const char*>(positions), sizeof(positions));
    buffer.write(reinterpret_cast<const char*>(indices), sizeof(indices));
    buffer.close();
    std::ofstream(root / "mesh.gltf")
        << R"({"asset":{"version":"2.0"},"buffers":[{"uri":"geometry.bin","byteLength":42}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";
    files.clear();
    scene = aiImportFileEx((root / "mesh.gltf").string().c_str(), aiProcess_Triangulate, &callbacks);
    assert(scene && scene->mNumMeshes == 1);
    assert(std::find(files.begin(), files.end(), root / "geometry.bin") != files.end());
    aiReleaseImport(scene);
    std::filesystem::remove(root / "mesh.obj");
    std::filesystem::remove(root / "arbitrary.mtl");
    std::filesystem::remove(root / "mesh.gltf");
    std::filesystem::remove(root / "geometry.bin");
    std::filesystem::remove(root);
    std::cout << "Thumbnail mesh I/O tests passed\n";
}
