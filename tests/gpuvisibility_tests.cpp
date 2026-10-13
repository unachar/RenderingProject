#include "rendering/visibility/visibilitytypes.h"
#include <cassert>
#include <cstdio>
#include <sstream>

int main()
{
    using namespace Visibility;
    static_assert(sizeof(VisibleInstanceData) == 16);
    static_assert(sizeof(GPULODLevel) == 16);
    static_assert(sizeof(GPULODGroup) == 16);
    // The last destination texel must retain the odd source edge.
    assert(ReductionRange(0, 5, 2).first == 0);
    assert(ReductionRange(1, 5, 2).second == 5);
    assert(ReductionRange(0, 3, 1).second == 3);
    assert(ReductionRange(0, 1, 1).second == 1);
    for (uint32_t size = 1; size <= 257; ++size)
    {
        const uint32_t destination = (std::max)(size / 2, 1u);
        uint32_t end = 0;
        for (uint32_t i = 0; i < destination; ++i)
        {
            const auto range = ReductionRange(i, size, destination);
            assert(range.first <= end && range.second > range.first);
            end = range.second;
        }
        assert(end == size);
    }
    LODMetadata metadata;
    metadata.Count = 5;
    metadata.Thresholds = {0.30f, 0.15f, 0.05f, 0.01f, 0, 0, 0, 0};
    metadata.Hysteresis = 0.1f;
    assert(SelectLOD(0.4f, metadata, InvalidID) == 0);
    assert(SelectLOD(0.2f, metadata, InvalidID) == 1);
    assert(SelectLOD(0.08f, metadata, InvalidID) == 2);
    assert(SelectLOD(0.001f, metadata, InvalidID) == 4);
    assert(SelectLOD(0.28f, metadata, 0) == 0);
    assert(SelectLOD(0.26f, metadata, 0) == 1);
    assert(SelectLOD(0.32f, metadata, 1) == 1);
    assert(SelectLOD(0.34f, metadata, 1) == 0);
    assert(SelectLOD(0.31f, metadata, 2) == 1); // A large step still crosses the intermediate boundary.
    metadata.MeshPaths[2] = "asset/Tree LOD2.obj";
    std::array<uint32_t, 8> meshes{4, 5, InvalidID, InvalidID, InvalidID, InvalidID, InvalidID, InvalidID};
    assert(FallbackLOD(4, meshes) == 1);
    assert(FallbackLOD(0, meshes) == 0);
    meshes.fill(InvalidID);
    assert(FallbackLOD(4, meshes) == InvalidID);
    std::stringstream file;
    assert(SaveLOD(file, metadata));
    LODMetadata loaded;
    assert(LoadLOD(file, loaded));
    assert(loaded.Count == 5 && loaded.Thresholds == metadata.Thresholds);
    assert(loaded.MeshPaths == metadata.MeshPaths);
    std::stringstream invalid("LOD 1 9 0.1");
    assert(!LoadLOD(invalid, loaded));
    assert(loaded.Count == 5); // Failed loads are transactional.
    std::puts("GPU visibility footprint/LOD/metadata tests: PASS");
}
