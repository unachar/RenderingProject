#include "terrainediting.h"
#include "vertexdeformationmath.h"
#include "../source/rendering/meshdispatch.h"
#include <cmath>
#include <iostream>

namespace
{
    int g_Checks = 0;
    int g_Failures = 0;

    void Check(bool condition, const char* description)
    {
        ++g_Checks;
        if (!condition)
        {
            ++g_Failures;
            std::cerr << "FAIL: " << description << '\n';
        }
    }

    bool Near(float first, float second)
    {
        return std::abs(first - second) < 1e-4f;
    }
} // 無名名前空間

int main()
{
    using namespace Procedural;
    TerrainEditState edit;
    TerrainSettings first;
    TerrainSettings second;
    first.Noise.Method = NoiseMethod::Composite;
    second.Noise.Seed = 907;
    Check(edit.SynchronizeSelection(17, &first), "load first selected terrain");
    auto individualEdit = first;
    individualEdit.Noise.Layers[1].Noise.Frequency = 0.2f;
    edit.SetSettings(individualEdit);
    Check(edit.HasPendingChanges(), "individual parameter changes request terrain update");
    individualEdit = first;
    individualEdit.Noise.Layers[1].Weight = 0.25f;
    edit.SetSettings(individualEdit);
    Check(edit.HasPendingChanges(), "blend weight changes request terrain update");
    auto changed = first;
    changed.Height = 8;
    edit.SetSettings(changed);
    Check(edit.ShouldApply(0, true), "initial edit is immediately ready");
    edit.MarkApplied(0);
    changed.BaseHeight = 3;
    edit.SetSettings(changed);
    Check(!edit.ShouldApply(0.05, true) && edit.ShouldApply(0.11, true), "drag updates are throttled");
    Check(edit.ShouldApply(0.01, false), "release immediately flushes final edit");
    Check(edit.SynchronizeSelection(19, &second) && !edit.HasPendingChanges() && edit.GetSettings().Noise.Seed == 907,
          "selection discards pending edits and loads correct target");
    auto undone = second;
    undone.Resolution = 32;
    Check(edit.SynchronizeSelection(19, &undone) && edit.GetSettings().Resolution == 32,
          "external undo synchronizes parameters");
    edit.SynchronizeSelection(TerrainEditState::InvalidTarget, nullptr);
    edit.SetSettings(changed);
    Check(!edit.ShouldApply(10, false), "unselected editor changes do not apply");

    VertexMotionSettings motion;
    motion.Amplitude = {0.5f, 1.0f, 0.4f};
    const Vector3 base = {2, 3, 5};
    const Vector3 adjacent = {4, 3, 7};
    for (auto method : {MotionMethod::Wave, MotionMethod::Orbit, MotionMethod::Noise})
    {
        motion.Method = method;
        const auto rest = DeformPosition(base, motion, 0);
        Check(Near(rest.X, base.X) && Near(rest.Y, base.Y) && Near(rest.Z, base.Z), "time zero restores base vertices");
        const auto moved = DeformPosition(base, motion, 0.71);
        const auto repeated = DeformPosition(base, motion, 0.71);
        const auto other = DeformPosition(adjacent, motion, 0.71);
        Check(moved.X == repeated.X && moved.Y == repeated.Y && moved.Z == repeated.Z, "deterministic deformation");
        Check(!Near(moved.Y - base.Y, other.Y - adjacent.Y), "motion varies across mesh vertices");
        const auto padding = GetDeformationBoundsPadding(motion);
        for (int sample = 0; sample < 100; ++sample)
        {
            const auto position = DeformPosition(base, motion, sample * 0.17);
            const auto normal = DeformNormal(base, {0, 1, 0}, motion, sample * 0.17);
            const float length = std::sqrt(normal.X * normal.X + normal.Y * normal.Y + normal.Z * normal.Z);
            Check(std::isfinite(length) && Near(length, 1), "deformed normals are finite unit vectors");
            Check(std::abs(position.X - base.X) <= padding.X + 1e-4f &&
                      std::abs(position.Y - base.Y) <= padding.Y + 1e-4f &&
                      std::abs(position.Z - base.Z) <= padding.Z + 1e-4f,
                  "deformed positions fit conservative bounds");
        }
    }
    for (uint32_t elements : {0u, 2u, 3u, 191u, 192u, 193u, 65535u * 192u, 65535u * 192u + 3u, UINT32_MAX})
    {
        const auto meshlets = MeshDispatch::MeshletCount(elements);
        uint32_t visited = 0;
        while (visited < meshlets)
        {
            const auto count = MeshDispatch::PageSize(meshlets, visited);
            Check(count > 0 && count <= 65535u, "mesh dispatch obeys hardware group limits");
            visited += count;
        }
        Check(visited == meshlets && MeshDispatch::PageSize(meshlets, visited) == 0,
              "huge mesh paging covers every meshlet exactly once without overflow");
    }
    std::cout << "Procedural editor checks=" << g_Checks << " failures=" << g_Failures << '\n';
    return g_Failures != 0;
}
