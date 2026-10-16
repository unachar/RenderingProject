#pragma once
#include "gpuscenetypes.h"
#include <algorithm>
#include <utility>
#include <vector>

// ECS mutation notifications. No D3D or component dependencies in this header.
// 以前は unordered_map だったため、1回の通知でハッシュ検索が2回走り、処理順もハッシュ表の状態に依存していた。
// エンティティ番号で引く配列＋変更リストにして、通知を O(1)・処理順を EntityID 昇順に固定する。
namespace GpuSceneChanges
{
using ChangeList = std::vector<std::pair<uint32_t, GpuSceneDirtyFlags>>;
struct PendingChanges
{
    ChangeList Changed;            // 取り出し後は EntityID 昇順
    vector<uint32_t> Destroyed;
    bool ResetScene = false;
};
inline PendingChanges &Pending()
{
    static PendingChanges changes;
    return changes;
}
// entity -> Changed内の位置+1（0 = 未登録）
inline std::vector<uint32_t> &Slots()
{
    static std::vector<uint32_t> slots;
    return slots;
}
inline void Notify(uint32_t entity, GpuSceneDirtyFlags flags = GpuSceneDirtyFlags::All)
{
    if (entity == UINT32_MAX)
    {
        return;
    }
    auto &slots = Slots();
    if (entity >= slots.size())
    {
        slots.resize((std::max)(static_cast<size_t>(entity) + 1, slots.size() * 2 + 64), 0u);
    }
    auto &changed = Pending().Changed;
    uint32_t &slot = slots[entity];
    if (slot == 0)
    {
        changed.emplace_back(entity, flags);
        slot = static_cast<uint32_t>(changed.size());
    }
    else
    {
        changed[slot - 1].second = changed[slot - 1].second | flags;
    }
}
inline void Destroy(uint32_t entity)
{
    Pending().Destroyed.push_back(entity);
    auto &slots = Slots();
    if (entity < slots.size() && slots[entity] != 0)
    {
        // 削除前の変更は捨てる（同じフレーム内で再利用された場合は、その後の通知で再び立つ）
        Pending().Changed[slots[entity] - 1].second = GpuSceneDirtyFlags::None;
    }
}
inline void ClearSlots(const ChangeList &changed)
{
    auto &slots = Slots();
    for (const auto &change : changed)
    {
        if (change.first < slots.size()) slots[change.first] = 0;
    }
}
inline void Reset()
{
    ClearSlots(Pending().Changed);
    Pending() = {};
    Pending().ResetScene = true;
}
// 保留中の変更を取り出して空にする。変更なしの項目を除き、EntityID 昇順に並べる。
inline PendingChanges Take()
{
    PendingChanges result = std::move(Pending());
    Pending() = {};
    ClearSlots(result.Changed);
    auto &changed = result.Changed;
    changed.erase(std::remove_if(changed.begin(), changed.end(),
        [](const auto &change) { return change.second == GpuSceneDirtyFlags::None; }), changed.end());
    std::sort(changed.begin(), changed.end(),
        [](const auto &left, const auto &right) { return left.first < right.first; });
    return result;
}
} // namespace GpuSceneChanges

