#include "source/models/fastgltfimport.h"
#include "assimp/cimport.h"
#include "assimp/scene.h"
#include "assimp/postprocess.h"
#include "assimp/material.h"
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <iostream>
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

static void CompareMatrix(const aiMatrix4x4& a, const aiMatrix4x4& e)
{
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
        {
            Check(std::abs(a[row][col] - e[row][col]) < 0.00001f, "matrix parity");
        }
    }
}

static void CompareNodes(const aiNode* a, const aiNode* e)
{
    Check(a && e, "node presence");
    Check(a->mName == e->mName, "node name");
    CompareMatrix(a->mTransformation, e->mTransformation);
    Check(a->mNumMeshes == e->mNumMeshes && a->mNumChildren == e->mNumChildren, "node topology");
    for (unsigned int i = 0; i < a->mNumMeshes; ++i)
    {
        Check(a->mMeshes[i] == e->mMeshes[i], "node mesh assignment");
    }
    for (unsigned int i = 0; i < a->mNumChildren; ++i)
    {
        Check(a->mChildren[i]->mParent == a, "owned parent pointer");
        CompareNodes(a->mChildren[i], e->mChildren[i]);
    }
}

int main()
{
#if RENDERINGPROJECT_USE_FASTGLTF
    Check(FastGltfImport::Available(), "SDK enabled");
    const char* paths[] = {"External/RTXNS/external/donut/thirdparty/cgltf/fuzz/data/Box.glb",
                           "External/RTXNS/external/donut/thirdparty/cgltf/fuzz/data/TriangleWithoutIndices.gltf",
                           "bin/validation/fastgltf/material.gltf",
                           "bin/validation/fastgltf/animated.gltf",
                           "bin/validation/fastgltf/sparse.gltf",
                           "bin/validation/fastgltf/sparse-only.gltf",
                           "bin/validation/fastgltf/external.gltf",
                           "External/Draco/upstream/testdata/CesiumMan/glTF_Binary/CesiumMan.glb"};
    for (const auto path : paths)
    {
        for (const auto flags :
             {0u,
              static_cast<unsigned int>(aiProcess_Triangulate | aiProcess_JoinIdenticalVertices |
                                        aiProcess_ImproveCacheLocality | aiProcess_GenSmoothNormals |
                                        aiProcess_ConvertToLeftHanded),
              static_cast<unsigned int>((aiProcessPreset_TargetRealtime_MaxQuality &
                                         ~(aiProcess_CalcTangentSpace | aiProcess_ValidateDataStructure)) |
                                        aiProcess_ConvertToLeftHanded)})
        {
            std::cout << "Compare " << path << " flags=" << flags << '\n';
            char error[512]{};
            const auto actual = FastGltfImport::Load(path, flags, error, sizeof(error));
            if (!actual)
            {
                std::cerr << error << '\n';
            }
            Check(actual != nullptr, "actual adapter import");
            // The bundled Assimp misreads interleaved sparse values; this fixture deliberately
            // replaces a position with the same known value. Compare against its dense equivalent.
            const auto referencePath = (std::strstr(path, "sparse") || std::strstr(path, "external.gltf"))
                                           ? "bin/validation/fastgltf/material.gltf"
                                           : path;
            const auto expected = aiImportFile(referencePath, flags);
            Check(expected != nullptr, "Assimp reference import");
            CompareNodes(actual->mRootNode, expected->mRootNode);
            Check(actual->mNumTextures == expected->mNumTextures, "embedded image count");
            for (unsigned int i = 0; i < actual->mNumTextures; ++i)
            {
                const auto a = actual->mTextures[i];
                const auto e = expected->mTextures[i];
                Check(a->mWidth == e->mWidth && a->mHeight == e->mHeight && a->mHeight == 0, "compressed image shape");
                Check(std::memcmp(a->pcData, e->pcData, a->mWidth) == 0, "embedded image bytes");
            }
            Check(actual->mNumMeshes == expected->mNumMeshes, "mesh count");
            for (unsigned int m = 0; m < actual->mNumMeshes; ++m)
            {
                const auto a = actual->mMeshes[m];
                const auto e = expected->mMeshes[m];
                Check(a->mNumVertices == e->mNumVertices, "vertex count");
                Check(a->mNumFaces == e->mNumFaces, "face count");
                for (unsigned int v = 0; v < a->mNumVertices; ++v)
                {
                    if ((a->mVertices[v] - e->mVertices[v]).Length() >= 0.00001f)
                    {
                        std::cerr << "vertex " << v << " actual=" << a->mVertices[v].x << ',' << a->mVertices[v].y
                                  << ',' << a->mVertices[v].z << " expected=" << e->mVertices[v].x << ','
                                  << e->mVertices[v].y << ',' << e->mVertices[v].z << '\n';
                    }
                    Check((a->mVertices[v] - e->mVertices[v]).Length() < 0.00001f, "position parity");
                    Check(a->HasNormals() == e->HasNormals(), "normal presence");
                    if (a->HasNormals())
                    {
                        Check((a->mNormals[v] - e->mNormals[v]).Length() < 0.00001f, "normal parity");
                    }
                    Check(a->HasTangentsAndBitangents() == e->HasTangentsAndBitangents(), "tangent presence");
                    if (a->HasTangentsAndBitangents())
                    {
                        Check((a->mTangents[v] - e->mTangents[v]).Length() < 0.00001f, "tangent parity");
                        Check((a->mBitangents[v] - e->mBitangents[v]).Length() < 0.00001f, "bitangent parity");
                    }
                    for (unsigned int set = 0; set < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++set)
                    {
                        Check(a->HasTextureCoords(set) == e->HasTextureCoords(set), "UV presence");
                        if (a->HasTextureCoords(set))
                        {
                            Check((a->mTextureCoords[set][v] - e->mTextureCoords[set][v]).Length() < 0.00001f,
                                  "UV parity");
                        }
                    }
                    for (unsigned int set = 0; set < AI_MAX_NUMBER_OF_COLOR_SETS; ++set)
                    {
                        Check(a->HasVertexColors(set) == e->HasVertexColors(set), "color presence");
                        if (a->HasVertexColors(set))
                        {
                            for (unsigned int component = 0; component < 4; ++component)
                            {
                                Check(std::abs(a->mColors[set][v][component] - e->mColors[set][v][component]) <
                                          0.00001f,
                                      "color parity");
                            }
                        }
                    }
                }
                for (unsigned int f = 0; f < a->mNumFaces; ++f)
                {
                    Check(a->mFaces[f].mNumIndices == e->mFaces[f].mNumIndices, "face index count");
                    for (unsigned int i = 0; i < a->mFaces[f].mNumIndices; ++i)
                    {
                        Check(a->mFaces[f].mIndices[i] == e->mFaces[f].mIndices[i], "index parity");
                    }
                }
                Check(a->mNumAnimMeshes == e->mNumAnimMeshes, "morph count");
                for (unsigned int i = 0; i < a->mNumAnimMeshes; ++i)
                {
                    const auto am = a->mAnimMeshes[i];
                    const auto em = e->mAnimMeshes[i];
                    Check(am->mNumVertices == em->mNumVertices && std::abs(am->mWeight - em->mWeight) < 0.00001f,
                          "morph metadata");
                    for (unsigned int v = 0; v < am->mNumVertices; ++v)
                    {
                        Check((am->mVertices[v] - em->mVertices[v]).Length() < 0.00001f, "morph position");
                    }
                }
                Check(a->mNumBones == e->mNumBones, "bone count");
                for (unsigned int i = 0; i < a->mNumBones; ++i)
                {
                    const auto ab = a->mBones[i];
                    const auto eb = e->mBones[i];
                    Check(ab->mName == eb->mName && ab->mNumWeights == eb->mNumWeights, "bone metadata");
                    CompareMatrix(ab->mOffsetMatrix, eb->mOffsetMatrix);
                    for (unsigned int w = 0; w < ab->mNumWeights; ++w)
                    {
                        Check(ab->mWeights[w].mVertexId == eb->mWeights[w].mVertexId &&
                                  std::abs(ab->mWeights[w].mWeight - eb->mWeights[w].mWeight) < 0.00001f,
                              "skin weight");
                    }
                }
                const auto am = actual->mMaterials[a->mMaterialIndex];
                const auto em = expected->mMaterials[e->mMaterialIndex];
                aiColor4D ac, ec;
                Check(am->Get(AI_MATKEY_COLOR_DIFFUSE, ac) == AI_SUCCESS &&
                          em->Get(AI_MATKEY_COLOR_DIFFUSE, ec) == AI_SUCCESS,
                      "base color property");
                for (unsigned int component = 0; component < 4; ++component)
                {
                    Check(std::abs(ac[component] - ec[component]) < 0.00001f, "base color parity");
                }
                for (const auto type :
                     {aiTextureType_DIFFUSE, aiTextureType_BASE_COLOR, aiTextureType_NORMALS, aiTextureType_METALNESS,
                      aiTextureType_DIFFUSE_ROUGHNESS, aiTextureType_EMISSIVE})
                {
                    Check(am->GetTextureCount(type) == em->GetTextureCount(type), "texture role parity");
                    if (am->GetTextureCount(type))
                    {
                        aiString ap, ep;
                        am->GetTexture(type, 0, &ap);
                        em->GetTexture(type, 0, &ep);
                        Check(ap == ep, "texture path parity");
                    }
                }
            }
            Check(actual->mNumAnimations == expected->mNumAnimations, "animation count");
            for (unsigned int i = 0; i < actual->mNumAnimations; ++i)
            {
                const auto a = actual->mAnimations[i];
                const auto e = expected->mAnimations[i];
                Check(a->mNumChannels == e->mNumChannels && a->mName == e->mName, "animation metadata");
                Check(std::abs(a->mDuration - e->mDuration) < 0.0001 && a->mTicksPerSecond == e->mTicksPerSecond,
                      "animation time base");
                for (unsigned int c = 0; c < a->mNumChannels; ++c)
                {
                    const auto ac = a->mChannels[c];
                    const aiNodeAnim* ec = nullptr;
                    for (unsigned int candidate = 0; candidate < e->mNumChannels; ++candidate)
                    {
                        if (ac->mNodeName == e->mChannels[candidate]->mNodeName)
                        {
                            ec = e->mChannels[candidate];
                        }
                    }
                    Check(ec != nullptr, "animation target by name");
                    Check(ac->mNumPositionKeys == ec->mNumPositionKeys &&
                              ac->mNumRotationKeys == ec->mNumRotationKeys &&
                              ac->mNumScalingKeys == ec->mNumScalingKeys,
                          "animation key counts");
                    for (unsigned int k = 0; k < ac->mNumPositionKeys; ++k)
                    {
                        Check(std::abs(ac->mPositionKeys[k].mTime - ec->mPositionKeys[k].mTime) < 0.0001 &&
                                  (ac->mPositionKeys[k].mValue - ec->mPositionKeys[k].mValue).Length() < 0.00001f,
                              "animation position key");
                    }
                    for (unsigned int k = 0; k < ac->mNumScalingKeys; ++k)
                    {
                        Check(std::abs(ac->mScalingKeys[k].mTime - ec->mScalingKeys[k].mTime) < 0.0001 &&
                                  (ac->mScalingKeys[k].mValue - ec->mScalingKeys[k].mValue).Length() < 0.00001f,
                              "animation scale key");
                    }
                    for (unsigned int k = 0; k < ac->mNumRotationKeys; ++k)
                    {
                        const auto aq = ac->mRotationKeys[k].mValue;
                        const auto eq = ec->mRotationKeys[k].mValue;
                        Check(std::abs(ac->mRotationKeys[k].mTime - ec->mRotationKeys[k].mTime) < 0.0001 &&
                                  std::abs(std::abs(aq.w * eq.w + aq.x * eq.x + aq.y * eq.y + aq.z * eq.z) - 1) <
                                      0.00001f,
                              "animation rotation key");
                    }
                }
            }
            Check(!FastGltfImport::Release(expected), "native scene not owned by adapter");
            aiReleaseImport(expected);
            Check(FastGltfImport::Release(actual), "adapter owns and releases scene");
        }
    }
    char error[128]{};
    Check(!FastGltfImport::Load("missing.gltf", 0, error, sizeof(error)), "missing input fallback");
    Check(error[0] != 0, "failure explanation");
    Check(!FastGltfImport::Release(nullptr), "null release");
    for (int attempt = 0; attempt < 32; ++attempt)
    {
        Check(!FastGltfImport::Load("bin/validation/fastgltf/bad-morph-nan.gltf", 0, error, sizeof(error)),
              "failure after temporary face allocation");
        Check(std::strstr(error, "Nonfinite vertex data") != nullptr, "partial mesh cleanup reaches morph validation");
    }
    for (const auto invalid : {"bad-accessor", "bad-sparse", "bad-cycle", "unsupported"})
    {
        const auto path = std::string("bin/validation/fastgltf/") + invalid + ".gltf";
        Check(!FastGltfImport::Load(path.c_str(), 0, error, sizeof(error)), "invalid or unsupported input rejected");
    }
    std::cout << "PASS fastgltf owned scene, geometry and postprocess parity\n";
#else
    Check(!FastGltfImport::Available(), "SDK disabled");
    char error[128]{};
    Check(!FastGltfImport::Load("missing.gltf", 0, error, sizeof(error)) && error[0] != 0, "SDK unavailable fallback");
#endif
    for (const auto enabled : {"0", "1"})
    {
        _putenv_s("DX12_USE_FASTGLTF", enabled);
        const auto scene = ImportModelScene("bin/validation/fastgltf/animated.gltf", aiProcess_ConvertToLeftHanded);
        Check(scene && scene->mNumAnimations == 1, "production model import route");
        const bool owned = FastGltfImport::Release(scene);
        Check(owned == (RENDERINGPROJECT_USE_FASTGLTF && enabled[0] == '1'), "production backend selection");
        if (!owned)
        {
            ReleaseModelScene(scene);
        }
    }
    _putenv_s("DX12_USE_FASTGLTF", "1");
    const auto fallback = ImportModelScene("bin/validation/fastgltf/unsupported.gltf", 0);
    Check(fallback != nullptr && !FastGltfImport::Release(fallback), "unsupported feature uses native fallback");
    ReleaseModelScene(fallback);
    _putenv_s("DX12_USE_FASTGLTF", "");
#if RENDERINGPROJECT_USE_FASTGLTF
    const auto preferred = ImportModelScene("bin/validation/fastgltf/material.gltf", 0);
    Check(preferred != nullptr && FastGltfImport::Release(preferred),
          "default core glTF backend preserves new format correctness");
    const auto recovered = ImportModelScene("bin/validation/fastgltf/external.gltf", 0);
    Check(recovered != nullptr && FastGltfImport::Release(recovered),
          "automatic recovery of Unicode external buffer input");
#endif
    std::cout << "PASS production model import selection and release\n";
}
