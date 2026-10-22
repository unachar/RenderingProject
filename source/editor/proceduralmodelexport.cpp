#include "proceduralmodelexport.h"
#include <Exporter.hpp>
#include <cexport.h>
#include <material.h>
#include <scene.h>
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <limits>

namespace ProceduralIO
{
    namespace
    {
        bool ValidateMesh(const ProcStudio::Mesh& mesh, bool vertexColors, std::string& error)
        {
            const size_t vertexCount = mesh.Positions.size();
            if (vertexCount == 0 || vertexCount > std::numeric_limits<unsigned int>::max() || mesh.Indices.empty() ||
                mesh.Indices.size() % 3 != 0 || mesh.Indices.size() / 3 > std::numeric_limits<unsigned int>::max() ||
                mesh.Normals.size() != vertexCount || mesh.UVs.size() != vertexCount ||
                (vertexColors && mesh.Colors.size() != vertexCount))
            {
                error = "モデルの頂点・法線・UV・三角形の数が不正です";
                return false;
            }
            if (std::any_of(mesh.Indices.begin(), mesh.Indices.end(), [vertexCount](uint32_t index) {
                    return index >= vertexCount;
                }))
            {
                error = "モデルに範囲外の頂点インデックスがあります";
                return false;
            }
            return true;
        }

        bool WriteFbx(const std::filesystem::path& path, const ProcStudio::Mesh& mesh, bool vertexColors,
                      const std::string& diffuseTexture, std::string& error)
        {
            // aiScene がノード・メッシュ・マテリアルと配列の所有権を持つ。
            aiScene scene;
            scene.mRootNode = new aiNode("Procedural Studio");
            scene.mRootNode->mNumMeshes = 1;
            scene.mRootNode->mMeshes = new unsigned int[1]{0};
            scene.mNumMaterials = 1;
            scene.mMaterials = new aiMaterial*[1]{new aiMaterial()};
            aiMaterial& material = *scene.mMaterials[0];
            const aiString materialName("procedural");
            const aiColor3D white(1, 1, 1);
            material.AddProperty(&materialName, AI_MATKEY_NAME);
            material.AddProperty(&white, 1, AI_MATKEY_COLOR_DIFFUSE);
            if (!diffuseTexture.empty())
            {
                const aiString texture(diffuseTexture);
                material.AddProperty(&texture, AI_MATKEY_TEXTURE_DIFFUSE(0));
            }

            scene.mNumMeshes = 1;
            scene.mMeshes = new aiMesh*[1]{new aiMesh()};
            aiMesh& target = *scene.mMeshes[0];
            target.mName = aiString("procedural");
            target.mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
            target.mMaterialIndex = 0;
            target.mNumVertices = static_cast<unsigned int>(mesh.Positions.size());
            target.mVertices = new aiVector3D[target.mNumVertices];
            target.mNormals = new aiVector3D[target.mNumVertices];
            target.mTextureCoords[0] = new aiVector3D[target.mNumVertices];
            target.mNumUVComponents[0] = 2;
            if (vertexColors)
            {
                target.mColors[0] = new aiColor4D[target.mNumVertices];
            }
            for (unsigned int i = 0; i < target.mNumVertices; ++i)
            {
                const auto& position = mesh.Positions[i];
                const auto& normal = mesh.Normals[i];
                const auto& uv = mesh.UVs[i];
                target.mVertices[i] = {position.X, position.Y, position.Z};
                target.mNormals[i] = {normal.X, normal.Y, normal.Z};
                // OBJ と同じ UV 原点で外部ツールへ渡す。
                target.mTextureCoords[0][i] = {uv.X, 1 - uv.Y, 0};
                if (vertexColors)
                {
                    const auto& color = mesh.Colors[i];
                    target.mColors[0][i] = {color.X, color.Y, color.Z, 1};
                }
            }
            target.mNumFaces = static_cast<unsigned int>(mesh.Indices.size() / 3);
            target.mFaces = new aiFace[target.mNumFaces];
            for (unsigned int i = 0; i < target.mNumFaces; ++i)
            {
                aiFace& face = target.mFaces[i];
                const size_t first = static_cast<size_t>(i) * 3;
                face.mNumIndices = 3;
                face.mIndices =
                    new unsigned int[3]{mesh.Indices[first], mesh.Indices[first + 1], mesh.Indices[first + 2]};
            }

            // メモリへ出力してから filesystem::path で保存し、日本語の保存先にも対応する。
            Assimp::Exporter exporter;
            // 同梱 Assimp で再読込を確認できた ASCII FBX を出力する。
            const aiExportDataBlob* blob = exporter.ExportToBlob(&scene, "fbxa");
            if (!blob || !blob->data || blob->size == 0)
            {
                error = std::string("FBX の生成に失敗しました: ") + exporter.GetErrorString();
                return false;
            }
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output.write(static_cast<const char*>(blob->data), static_cast<std::streamsize>(blob->size));
            output.close();
            if (!output)
            {
                error = "FBX を書き込めません";
                return false;
            }
            return true;
        }
    }

    bool SaveModelFile(const std::filesystem::path& path, const ProcStudio::Mesh& mesh, bool vertexColors,
                       const std::string& diffuseTexture, std::string& error)
    {
        error.clear();
        auto extension = path.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
        if (extension != L".obj" && extension != L".fbx")
        {
            error = "モデルの保存形式は OBJ または FBX を選んでください";
            return false;
        }
        if (!ValidateMesh(mesh, vertexColors, error))
        {
            return false;
        }
        if (extension == L".obj")
        {
            return ProcStudio::WriteObj(path, mesh, vertexColors, diffuseTexture, error);
        }
        return WriteFbx(path, mesh, vertexColors, diffuseTexture, error);
    }
}
