#include "source/models/fastgltfimport.h"
#include "assimp/cimport.h"
#include "assimp/scene.h"
#include "assimp/postprocess.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>
using namespace std;
#include "source/models/modelimportutils.h"

static void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL " << message << '\n';
        std::exit(1);
    }
}

struct Corner
{
    aiVector3D position, normal;
    aiVector3D uv{};
    aiColor4D color{};
    bool hasUv = false, hasColor = false;
};

using Triangle = std::array<Corner, 3>;

static std::vector<Triangle> Triangles(const aiScene* scene)
{
    std::vector<Triangle> triangles;
    for (unsigned int m = 0; m < scene->mNumMeshes; ++m)
    {
        const auto mesh = scene->mMeshes[m];
        Check(mesh->HasNormals(), "decoded normals");
        for (unsigned int f = 0; f < mesh->mNumFaces; ++f)
        {
            Check(mesh->mFaces[f].mNumIndices == 3, "triangle topology");
            Triangle triangle;
            for (int c = 0; c < 3; ++c)
            {
                const auto index = mesh->mFaces[f].mIndices[c];
                Check(index < mesh->mNumVertices, "decoded index range");
                triangle[c] = {mesh->mVertices[index], mesh->mNormals[index]};
                if (mesh->HasTextureCoords(0))
                {
                    triangle[c].hasUv = true;
                    triangle[c].uv = mesh->mTextureCoords[0][index];
                }
                if (mesh->HasVertexColors(0))
                {
                    triangle[c].hasColor = true;
                    triangle[c].color = mesh->mColors[0][index];
                }
            }
            triangles.push_back(triangle);
        }
    }
    return triangles;
}

int main(int argc, char**)
{
    const char* box = "External/Draco/upstream/testdata/Box/glTF_Binary/Box_Draco.glb";
    if (argc > 1)
    {
        char error[512]{};
        Check(!FastGltfImport::Load(box, 0, error, sizeof(error)), "Draco-disabled import rejects compressed input");
        const auto plain =
            FastGltfImport::Load("External/Draco/upstream/testdata/Box/glTF_Binary/Box.glb", 0, error, sizeof(error));
        Check(plain && FastGltfImport::Release(plain), "Draco-disabled adapter still loads plain glTF");
        std::cout << "PASS Draco-disabled adapter without Draco SDK linkage\n";
        return 0;
    }
    const std::array<const char*, 2> pairs[]{{box, "External/Draco/upstream/testdata/Box/glTF_Binary/Box.glb"},
                                             {"bin/validation/draco/fixture.gltf", "bin/validation/draco/dense.gltf"}};
    for (const auto& pair : pairs)
    {
        for (const auto flags : {0u, static_cast<unsigned int>(aiProcess_Triangulate | aiProcess_ConvertToLeftHanded)})
        {
            std::cout << "Compare " << pair[0] << " flags=" << flags << std::endl;
            char error[512]{};
            const auto actual = FastGltfImport::Load(pair[0], flags, error, sizeof(error));
            if (!actual)
            {
                std::cerr << error << '\n';
            }
            Check(actual != nullptr, "real Draco glTF import");
            std::cout << "Decoded compressed input; importing dense reference" << std::endl;
            const auto expected = aiImportFile(pair[1], flags);
            Check(expected != nullptr, "original reference import");
            std::cout << "Dense reference imported" << std::endl;
            const auto a = Triangles(actual), e = Triangles(expected);
            Check(a.size() == e.size() && !a.empty(), "face count preservation");
            std::vector<bool> used(e.size());
            for (const auto& triangle : a)
            {
                bool found = false;
                for (std::size_t i = 0; i < e.size() && !found; ++i)
                {
                    if (!used[i])
                    {
                        for (int shift = 0; shift < 3 && !found; ++shift)
                        {
                            bool matches = true;
                            for (int c = 0; c < 3; ++c)
                            {
                                const auto& ac = triangle[c];
                                const auto& ec = e[i][(c + shift) % 3];
                                matches = matches &&
                                          (triangle[c].position - e[i][(c + shift) % 3].position).Length() < 0.0001f &&
                                          (triangle[c].normal - e[i][(c + shift) % 3].normal).Length() < 0.001f;
                                matches = matches && ac.hasUv == ec.hasUv && ac.hasColor == ec.hasColor;
                                if (ac.hasUv)
                                {
                                    matches = matches && (ac.uv - ec.uv).Length() < 0.0001f;
                                }
                                if (ac.hasColor)
                                {
                                    for (unsigned int component = 0; component < 4; ++component)
                                    {
                                        matches =
                                            matches && std::abs(ac.color[component] - ec.color[component]) < 0.0001f;
                                    }
                                }
                            }
                            if (matches)
                            {
                                found = used[i] = true;
                            }
                        }
                    }
                }
                Check(found, "oriented triangle position/normal parity");
            }
            for (unsigned int m = 0; m < actual->mNumMeshes; ++m)
            {
                if (expected->mMeshes[m]->HasBones())
                {
                    const auto mesh = actual->mMeshes[m];
                    Check(mesh->mNumBones == 1 && std::string(mesh->mBones[0]->mName.C_Str()) == "Joint",
                          "skin target");
                    const auto bone = mesh->mBones[0];
                    Check(bone->mNumWeights == mesh->mNumVertices, "mapped skin vertex count");
                    for (unsigned int i = 0; i < bone->mNumWeights; ++i)
                    {
                        Check(bone->mWeights[i].mVertexId < mesh->mNumVertices && bone->mWeights[i].mWeight == 1.0f,
                              "normalized uint8 weight is applied once");
                    }
                }
            }
            aiReleaseImport(expected);
            Check(FastGltfImport::Release(actual), "adapter scene ownership");
        }
    }
    char error[512]{};
    for (const auto name : {"bad-id", "bad-count", "bad-type", "bad-stream", "bad-duplicate-id"})
    {
        const auto path = std::string("bin/validation/draco/") + name + ".gltf";
        Check(!FastGltfImport::Load(path.c_str(), 0, error, sizeof(error)), "invalid compressed input rejected");
    }
    const auto native = aiImportFile(box, 0);
    std::cout << "Bundled Assimp Draco support: " << (native ? "available" : "unavailable") << '\n';
    aiReleaseImport(native);
    _putenv_s("DX12_USE_FASTGLTF", "");
    const auto selected = ImportModelScene(box, 0);
    Check(selected != nullptr, "production compressed glTF input");
    const bool adapterOwned = FastGltfImport::Release(selected);
    Check(adapterOwned, "production default selects Draco adapter directly");
    if (!adapterOwned)
    {
        ReleaseModelScene(selected);
        const auto optionalBind = ImportModelScene("bin/validation/draco/dense-optional-bind.gltf", 0);
        Check(optionalBind && optionalBind->mNumMeshes == 1 && optionalBind->mMeshes[0]->mNumBones == 1,
              "default plain glTF with omitted inverse bind matrices");
        const auto& inverse = optionalBind->mMeshes[0]->mBones[0]->mOffsetMatrix;
        Check(inverse == aiMatrix4x4(), "omitted inverse bind matrix is identity");
        Check(FastGltfImport::Release(optionalBind), "safe default importer owns optional-bind scene");
    }
    std::cout << "PASS real Draco glTF geometry, normal, winding and ownership\n";
}
