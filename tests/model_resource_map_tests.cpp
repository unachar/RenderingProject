#include "models/modelresourcemap.h"
#include <cassert>
#include <sstream>
#include <iostream>
#include <chrono>

int main()
{
    ModelResourceMap map;
    map.Add("bone", "hips", 7);
    map.Add("mesh", "body", 0);
    map.Add("mesh", "body", 2);
    map.Add("mesh", "body", 2);
    map.Add("material", "", 3);
    map.Add("socket", "右手", 5);
    assert(map.Find("bone", "hips")->front() == 7);
    assert(*map.Find("mesh", "body") == (std::vector<uint32_t>{0, 2}));
    assert(map.Find("material", "")->front() == 3);
    assert(!map.Find("bone", "missing"));
    assert(*map.NamesForIndex("bone", 7)->front() == "hips");
    assert(!map.NamesForIndex("bone", 999));
    map.Set("bone", "hips", 8);
    assert(!map.NamesForIndex("bone", 7));
    assert(*map.NamesForIndex("bone", 8)->front() == "hips");
    map.Set("bone", "hips", 7);
    std::vector<uint32_t> visited;
    map.Add(ModelResourceMap::MaterialMeshes, "4", 2);
    map.Add(ModelResourceMap::MaterialMeshes, "4", 200); // Ignore invalid/stale drawable indices.
    map.ForEachMaterialMesh(4, 10, [&](uint32_t index) { visited.push_back(index); });
    assert(visited == (std::vector<uint32_t>{2}));
    visited.clear();
    map.ForEachMaterialMesh(9, 10, [&](uint32_t index) { visited.push_back(index); });
    assert(visited.empty());
    map.ForEachMaterialMesh(-1, 3, [&](uint32_t index) { visited.push_back(index); });
    assert(visited == (std::vector<uint32_t>{0,1,2}));
    ModelResourceMap multiline;
    assert(!multiline.HasMultilineNames(ModelResourceMap::Meshes));
    multiline.Add(ModelResourceMap::Meshes, "two\nlines", 0);
    assert(multiline.HasMultilineNames(ModelResourceMap::Meshes));
    std::ostringstream multilineBytes(std::ios::binary);
    assert(multiline.Write(multilineBytes, 1));
    ModelResourceMap restoredMultiline;
    std::istringstream multilineInput(multilineBytes.str(), std::ios::binary);
    assert(restoredMultiline.Read(multilineInput));
    assert(restoredMultiline.HasMultilineNames(ModelResourceMap::Meshes));
    assert(*restoredMultiline.NamesForIndex(ModelResourceMap::Meshes, 0)->front() == "two\nlines");
    multiline.Clear();
    assert(!multiline.HasMultilineNames(ModelResourceMap::Meshes));
    ModelResourceMap many;
    for (uint32_t i=0;i<10000;++i) many.Add(ModelResourceMap::MaterialMeshes, std::to_string(i%1000), i);
    size_t work=0;
    many.ForEachMaterialMesh(17, 10000, [&](uint32_t index) {
        assert(index%1000==17);
        ++work;
    });
    assert(work==10); // The previous material morph loop examined all 10000 meshes.
    ModelResourceMap copied = map;
    ModelResourceMap assigned;
    assigned = copied;
    copied.Clear();
    assert(*assigned.NamesForIndex("bone", 7)->front() == "hips");
    ModelResourceMap moved = std::move(assigned);
    assert(*moved.NamesForIndex("bone", 7)->front() == "hips");
    moved = moved;
    assert(*moved.NamesForIndex("bone", 7)->front() == "hips");
    auto hash = map.Fingerprint("animated|left-handed");
    std::ostringstream bytes(std::ios::binary);
    assert(map.Write(bytes, hash));
    ModelResourceMap loaded;
    std::istringstream input(bytes.str(), std::ios::binary);
    assert(loaded.Read(input, hash));
    assert(loaded.Categories() == map.Categories());
    assert(*loaded.NamesForIndex("bone", 7)->front() == "hips");
    assert(loaded.Find("socket", "右手")->front() == 5);
    ModelResourceMap reordered;
    reordered.Add(ModelResourceMap::MaterialMeshes, "4", 2);
    reordered.Add(ModelResourceMap::MaterialMeshes, "4", 200);
    reordered.Add("socket", "右手", 5);
    reordered.Add("material", "", 3);
    reordered.Add("mesh", "body", 0);
    reordered.Add("mesh", "body", 2);
    reordered.Add("bone", "hips", 7);
    std::ostringstream same(std::ios::binary);
    assert(reordered.Write(same, hash));
    assert(same.str() == bytes.str());
    assert(hash != map.Fingerprint("animated|raw"));
    auto original = loaded.Categories();
    std::istringstream stale(bytes.str(), std::ios::binary);
    assert(!loaded.Read(stale, hash + 1));
    assert(loaded.Categories() == original);
    for (size_t length = 0; length < bytes.str().size(); ++length)
    {
        std::istringstream truncated(bytes.str().substr(0, length), std::ios::binary);
        assert(!loaded.Read(truncated, hash));
        assert(loaded.Categories() == original);
    }
    auto bad = bytes.str();
    bad[0] = '!';
    std::istringstream corrupt(bad, std::ios::binary);
    assert(!loaded.Read(corrupt, hash));
    // Header offsets: magic(8), version(4), fingerprint(8), payload size(4).
    bad = bytes.str();
    for (int i=20; i<24; ++i) bad[i] = char(0xff);
    std::istringstream oversized(bad, std::ios::binary);
    assert(!loaded.Read(oversized, hash));
    bad = bytes.str(); bad.back() ^= 1;
    std::istringstream changed(bad, std::ios::binary);
    assert(!loaded.Read(changed, hash));
    const auto directory = std::filesystem::temp_directory_path() /
        ("model-map-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    const auto source = directory / "model.fbx";
    { std::ofstream file(source); file << "fixture"; }
    const auto cache = directory / "cache";
    assert(map.Persist(source, "left-handed", cache));
    assert(map.Persist(source, "left-handed", cache));
    assert(map.Persist(source, "raw", cache));
    assert(std::distance(std::filesystem::directory_iterator(cache), std::filesystem::directory_iterator()) == 2);
    map.Add("mesh", "new mesh", 4);
    assert(map.Persist(source, "left-handed", cache));
    bool foundUpdated = false;
    for (const auto& file : std::filesystem::directory_iterator(cache))
    {
        ModelResourceMap offline;
        std::ifstream fileStream(file.path(), std::ios::binary);
        assert(offline.Read(fileStream)); // Offline readers need no importer fingerprint.
        foundUpdated |= offline.Find("mesh", "new mesh") != nullptr;
    }
    assert(foundUpdated);
    std::filesystem::remove_all(directory);
    std::cout << "Model resource maps: PASS\n";
}
