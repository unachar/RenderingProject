#include "models/modelmapimport.h"
#include <cassert>
#include <iostream>
struct Mesh
{
    std::string MeshName, MaterialName;
    int MaterialIndex;
};
int main()
{
    ModelResourceMap map;
    // Source mesh 1 was empty and omitted. The returned index must be drawable index 1, not source index 2.
    const std::vector<Mesh> drawable{{"Body", "Skin", 4}, {"Body", "Skin", 4}, {"", "", 7}};
    ModelMapImport::AddMeshes(map, drawable);
    assert(*map.Find(ModelResourceMap::Meshes, "Body") == (std::vector<uint32_t>{0,1}));
    assert(*map.Find(ModelResourceMap::Materials, "Skin") == (std::vector<uint32_t>{4}));
    assert(*map.Find(ModelResourceMap::MaterialMeshes, "4") == (std::vector<uint32_t>{0,1}));
    assert(map.Find(ModelResourceMap::Meshes, "")->front() == 2);
    assert(!map.Find(ModelResourceMap::MaterialMeshes, "0"));
    std::cout << "Model map importer: PASS (drawable indices, shared materials, unnamed meshes)\n";
}
