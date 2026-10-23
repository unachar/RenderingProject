#pragma once
#include <algorithm>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace EntityHierarchy
{
    using Entity = uint32_t;
    inline constexpr Entity None = UINT32_MAX;

    template <typename ParentLookup> bool IsDescendant(Entity entity, Entity ancestor, ParentLookup parent)
    {
        std::unordered_set<Entity> visited;
        for (Entity current = parent(entity); current != None; current = parent(current))
        {
            if (!visited.insert(current).second)
            {
                return false;
            }
            if (current == ancestor)
            {
                return true;
            }
        }
        return false;
    }

    template <typename ParentLookup> bool CanReparent(Entity entity, Entity candidate, ParentLookup parent)
    {
        std::unordered_set<Entity> visited;
        for (Entity current = candidate; current != None; current = parent(current))
        {
            if (current == entity || !visited.insert(current).second)
            {
                return false;
            }
        }
        return true;
    }

    template <typename ParentLookup>
    std::vector<Entity> SelectionRoots(const std::vector<Entity>& selected, ParentLookup parent)
    {
        std::vector<Entity> roots;
        const std::unordered_set<Entity> selectedSet(selected.begin(), selected.end());
        for (Entity entity : selected)
        {
            bool ancestorSelected = false;
            std::unordered_set<Entity> visited;
            for (Entity ancestor = parent(entity); ancestor != None && visited.insert(ancestor).second;
                 ancestor = parent(ancestor))
            {
                if (selectedSet.count(ancestor))
                {
                    ancestorSelected = true;
                    break;
                }
            }
            if (!ancestorSelected)
            {
                roots.push_back(entity);
            }
        }
        return roots;
    }

    template <typename ParentLookup>
    std::vector<Entity> WithDescendants(const std::vector<Entity>& selected, const std::vector<Entity>& all,
                                        ParentLookup parent)
    {
        std::vector<Entity> result;
        const std::unordered_set<Entity> selectedSet(selected.begin(), selected.end());
        for (Entity entity : all)
        {
            std::unordered_set<Entity> visited;
            for (Entity ancestor = entity; ancestor != None && visited.insert(ancestor).second;
                 ancestor = parent(ancestor))
            {
                if (selectedSet.count(ancestor))
                {
                    result.push_back(entity);
                    break;
                }
            }
        }
        return result;
    }
}
