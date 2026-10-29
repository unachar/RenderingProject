#pragma once
#include "modelresourcemap.h"
// Bundled Assimp headers use unqualified min in material.inl.
using std::min;
#include "scene.h"
#include "material.h"

namespace ModelMapImport
{
inline void AddNodes(ModelResourceMap& map, const aiNode* node, uint32_t& next,
                     const std::unordered_map<std::string, uint32_t>* palette = nullptr)
{
    if (!node) return;
    const std::string name = node->mName.C_Str();
    if (palette)
    {
        const auto it = palette->find(name);
        if (it != palette->end()) map.Add(ModelResourceMap::Bones, name, it->second);
    }
    else map.Add(ModelResourceMap::Bones, name, next++);
    for (unsigned i = 0; i < node->mNumChildren; ++i) AddNodes(map, node->mChildren[i], next, palette);
}
inline void AddMaterials(ModelResourceMap& map, const aiScene* scene)
{
    if (!scene) return;
    for (unsigned i = 0; i < scene->mNumMaterials; ++i)
    {
        aiString name;
        scene->mMaterials[i]->Get(AI_MATKEY_NAME, name);
        map.Add(ModelResourceMap::Materials, name.C_Str(), i);
    }
}
template<class MeshRange> inline void AddMeshes(ModelResourceMap& map, const MeshRange& meshes)
{
    for (uint32_t i = 0; i < meshes.size(); ++i)
    {
        const auto& mesh = meshes[i];
        map.Add(ModelResourceMap::Meshes, mesh.MeshName, i);
        if (mesh.MaterialIndex >= 0)
        {
            map.Add(ModelResourceMap::Materials, mesh.MaterialName, static_cast<uint32_t>(mesh.MaterialIndex));
            map.Add(ModelResourceMap::MaterialMeshes, std::to_string(mesh.MaterialIndex), i);
        }
    }
}
}
