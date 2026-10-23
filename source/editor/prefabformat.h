#pragma once
#include "ecs/entityhierarchy.h"
#include <json/json.h>
#include <string>
#include <unordered_map>

namespace PrefabFormat
{
    inline bool Validate(const Json::Value& prefab, std::string& error)
    {
        using PrefabEntity = EntityHierarchy::Entity;
        constexpr auto None = EntityHierarchy::None;
        if (!prefab.isObject() || !prefab["format"].isString() || prefab["format"].asString() != "DX12Prefab" ||
            !prefab["version"].isInt() || prefab["version"].asInt() != 1 || !prefab["root"].isUInt() ||
            !prefab["entities"].isArray() || prefab["entities"].empty())
        {
            error = "プレハブの形式またはバージョンが不正です";
            return false;
        }
        std::unordered_map<PrefabEntity, PrefabEntity> parents;
        for (const auto& record : prefab["entities"])
        {
            if (!record.isObject() || !record["id"].isUInt() || record["id"].asUInt() == None ||
                !record["transform"].isObject() ||
                !(record["transform"]["parent"].isUInt() ||
                  (record["transform"]["parent"].isInt() && record["transform"]["parent"].asInt() == -1)))
            {
                error = "プレハブのオブジェクト情報が不正です";
                return false;
            }
            const auto& parentValue = record["transform"]["parent"];
            PrefabEntity parentId = None;
            if (parentValue.isUInt())
            {
                parentId = parentValue.asUInt();
            }
            if (!parents.emplace(record["id"].asUInt(), parentId).second)
            {
                error = "プレハブ内の ID が重複しています";
                return false;
            }
        }
        const PrefabEntity root = prefab["root"].asUInt();
        if (!parents.count(root) || parents.at(root) != None)
        {
            error = "プレハブのルートが不正です";
            return false;
        }
        const auto parent = [&](PrefabEntity entity) {
            const auto found = parents.find(entity);
            if (found == parents.end())
            {
                return None;
            }
            return found->second;
        };
        for (const auto& [entity, ancestor] : parents)
        {
            if (entity != root &&
                (!parents.count(ancestor) || !EntityHierarchy::CanReparent(entity, ancestor, parent) ||
                 !EntityHierarchy::IsDescendant(entity, root, parent)))
            {
                error = "プレハブの親子関係が不正です";
                return false;
            }
        }
        return true;
    }
}
