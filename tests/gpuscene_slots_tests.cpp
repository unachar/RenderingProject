#include "rendering/gpuscene/gpusceneslots.h"
#include "rendering/gpuscene/gpuscenechanges.h"
#include <iostream>
#include <stdexcept>

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

int main()
{
    try
    {
        GpuSceneSlots slots;
        GPUInstanceData data;
        data.Flags = 1;
        for (uint32_t id = 0; id < 10000; ++id)
        {
            data.Entity = id;
            Check(slots.Register(data) == id, "Stable ID registration");
        }
        Check(slots.ActiveCount() == 10000 && slots.Updates().size() == 10000, "10k registration");
        slots.ClearUpdates();
        slots.SettlePreviousTransforms();
        Check(slots.Updates().empty(), "Unchanged scene: zero updates");
        data = slots.Data()[6789];
        data.World._41 = 1;
        slots.Update(6789, data);
        slots.Update(6789, data);
        Check(slots.Updates().size() == 1, "Exactly one dirty instance");
        slots.ClearUpdates();
        slots.SettlePreviousTransforms();
        Check(slots.Updates().size() == 1 && slots.Data()[6789].PreviousWorld._41 == 1, "Temporal settle");
        slots.ClearUpdates();
        slots.SettlePreviousTransforms();
        Check(slots.Updates().empty(), "Second stationary frame: no upload");
        slots.Remove(6789);
        slots.Remove(6789);
        Check(slots.ActiveCount() == 9999, "Idempotent deletion");
        data.PreviousWorld = data.World;
        Check(slots.Register(data) == 6789 && slots.ActiveCount() == 10000, "Free list reuse");
        Check(slots.Updates().size() == 1, "Delete/recreate coalesces slot upload");
        GpuSceneChanges::Reset();
        GpuSceneChanges::Notify(42, GpuSceneDirtyFlags::Transform);
        GpuSceneChanges::Notify(42, GpuSceneDirtyFlags::Material);
        Check(GpuSceneChanges::Pending().Changed.size() == 1 &&
                  static_cast<uint32_t>(GpuSceneChanges::Pending().Changed.at(42)) == 5,
              "Deduplicate ECS dirty flags");
        GpuSceneChanges::Destroy(42);
        GpuSceneChanges::Notify(42, GpuSceneDirtyFlags::All);
        Check(GpuSceneChanges::Pending().Destroyed.size() == 1 && GpuSceneChanges::Pending().Changed.size() == 1,
              "Destroy/recreate the same ECS ID preserves both events");
        GpuSceneChanges::Reset();
        Check(GpuSceneChanges::Pending().ResetScene && GpuSceneChanges::Pending().Changed.empty(),
              "Scene reset discards obsolete events");
        std::cout << "PASS: GPU scene 10000 slots, dirty updates, temporal settle and ID reuse\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
