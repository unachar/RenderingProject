#include "pch.h"
#include "materialmanager.h"
#include "modelmanager.h"
#include "primitivegeometry.h"
#include "world.h"
#include "gpuscene/gpuscenechanges.h"
#include <json/json.h>
#include <fstream>
#include <cmath>

namespace
{
Json::Value WriteVector(const XMFLOAT3& vector)
{
    Json::Value result(Json::arrayValue);
    result.append(vector.x);
    result.append(vector.y);
    result.append(vector.z);
    return result;
}

bool IsFiniteArray(const Json::Value& array, Json::ArrayIndex length)
{
    if (!array.isArray() || array.size() != length)
    {
        return false;
    }
    for (const auto& value : array)
    {
        if (!value.isNumeric() || !std::isfinite(value.asFloat()))
        {
            return false;
        }
    }
    return true;
}

XMFLOAT3 ReadVector(const Json::Value& vector)
{
    return {vector[0].asFloat(), vector[1].asFloat(), vector[2].asFloat()};
}

bool ValidHandle(const Json::Value& reference)
{
    return reference.isString() &&
        (ParseAssetHandle(reference.asString()) != 0 || reference.asString() == FormatAssetHandle(0));
}

bool ValidateRecord(const Json::Value& record)
{
    if (!record.isObject() || !record["entity"].isUInt() || !record["materialSlots"].isArray() ||
        record["materialSlots"].size() > 4096 || (record.isMember("name") && !record["name"].isString()))
    {
        return false;
    }
    for (const auto& reference : record["materialSlots"])
    {
        if (!ValidHandle(reference))
        {
            return false;
        }
    }
    if (record.isMember("identity") && !ValidHandle(record["identity"]))
    {
        return false;
    }
    if (record.isMember("transform"))
    {
        for (const char* property : {"position", "rotation", "scale"})
        {
            if (!IsFiniteArray(record["transform"][property], 3))
            {
                return false;
            }
        }
    }
    if (record.isMember("modelPath") && (!record["modelPath"].isString() || !record["animated"].isBool() ||
        !record["convert"].isBool()))
    {
        return false;
    }
    if (record.isMember("vertices"))
    {
        if (!record["vertices"].isArray() || record["vertices"].size() > 1000000)
        {
            return false;
        }
        for (const auto& vertex : record["vertices"])
        {
            if (!IsFiniteArray(vertex, 12))
            {
                return false;
            }
        }
    }
    return true;
}

bool MatchesSavedSource(const Json::Value& record, EntityID entity)
{
    if (!MaterialManager::IsRenderable(entity) || !record["name"].isString() ||
        !ComponentManager::HasComponent<NameComponent>(entity) ||
        ComponentManager::GetComponentUnchecked<NameComponent>(entity).Name != record["name"].asString())
    {
        return false;
    }
    if (record["modelPath"].isString())
    {
        if (record["animated"].asBool())
        {
            return ComponentManager::HasComponent<AnimationModelComponent>(entity) &&
                ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity).ModelPath == record["modelPath"].asString();
        }
        return ComponentManager::HasComponent<StaticModelComponent>(entity) &&
            ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity).ModelPath == record["modelPath"].asString();
    }
    if (!record["meshSignature"].isString() || !ComponentManager::HasComponent<MeshComponent>(entity))
    {
        return false;
    }
    return FormatAssetHandle(ComponentManager::GetComponentUnchecked<MeshComponent>(entity).GeometryHash) == record["meshSignature"].asString();
}
}

bool MaterialManager::SaveSceneReferences(const std::filesystem::path& path, std::string& error)
{
    Json::Value root;
    root["version"] = 1;
    root["entities"] = Json::Value(Json::arrayValue);
    for (EntityID entity : World::GetView<MaterialComponent>())
    {
        auto& component = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
        if (component.MaterialSlots.empty())
        {
            continue;
        }
        if (component.MaterialSceneIdentity == 0)
        {
            component.MaterialSceneIdentity = GenerateAssetHandle();
        }
        Json::Value record;
        record["entity"] = entity;
        record["identity"] = FormatAssetHandle(component.MaterialSceneIdentity);
        if (ComponentManager::HasComponent<NameComponent>(entity))
        {
            record["name"] = ComponentManager::GetComponentUnchecked<NameComponent>(entity).Name;
        }
        if (ComponentManager::HasComponent<TransformComponent>(entity))
        {
            const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
            record["transform"]["position"] = WriteVector(transform.Position);
            record["transform"]["rotation"] = WriteVector(transform.Rotation);
            record["transform"]["scale"] = WriteVector(transform.Scale);
        }
        if (ComponentManager::HasComponent<StaticModelComponent>(entity))
        {
            const auto& model = ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity);
            record["modelPath"] = model.ModelPath;
            record["animated"] = false;
            record["convert"] = model.IsConvert;
        }
        else if (ComponentManager::HasComponent<AnimationModelComponent>(entity))
        {
            const auto& model = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity);
            record["modelPath"] = model.ModelPath;
            record["animated"] = true;
            record["convert"] = model.IsConvert;
        }
        else if (ComponentManager::HasComponent<MeshComponent>(entity))
        {
            const auto& mesh = ComponentManager::GetComponentUnchecked<MeshComponent>(entity);
            record["meshSignature"] = FormatAssetHandle(mesh.GeometryHash);
            if (mesh.NaniteMesh)
            {
                record["vertices"] = Json::Value(Json::arrayValue);
                for (const auto& vertex : mesh.NaniteMesh->CpuVertices)
                {
                    Json::Value encoded(Json::arrayValue);
                    for (float number : {vertex.Position.x, vertex.Position.y, vertex.Position.z,
                        vertex.Normal.x, vertex.Normal.y, vertex.Normal.z, vertex.TexCoord.x, vertex.TexCoord.y,
                        vertex.Diffuse.x, vertex.Diffuse.y, vertex.Diffuse.z, vertex.Diffuse.w})
                    {
                        encoded.append(number);
                    }
                    record["vertices"].append(encoded);
                }
            }
        }
        record["materialSlots"] = Json::Value(Json::arrayValue);
        for (const auto handle : component.MaterialSlots)
        {
            record["materialSlots"].append(FormatAssetHandle(handle));
        }
        root["entities"].append(record);
    }
    Json::StreamWriterBuilder writer;
    return WriteAssetFile(path, Json::writeString(writer, root), error);
}

bool MaterialManager::LoadSceneReferences(const std::filesystem::path& path, std::string& error)
{
    error.clear();
    std::ifstream stream(path);
    if (!stream)
    {
        return true;
    }
    Json::Value root;
    Json::CharReaderBuilder reader;
    reader["rejectDupKeys"] = true;
    if (!Json::parseFromStream(reader, stream, &root, &error) || root["version"] != 1 ||
        !root["entities"].isArray() || root["entities"].size() > g_kMAX_ENTITIES)
    {
        error = "Invalid scene material reference file: " + error;
        return false;
    }
    // Validate the complete document before changing any entity.
    for (const auto& record : root["entities"])
    {
        if (!ValidateRecord(record))
        {
            error = "Invalid scene material record";
            return false;
        }
    }
    for (const auto& record : root["entities"])
    {
        EntityID entity = g_kINVALID_ENTITY;
        const auto identity = ParseAssetHandle(record["identity"].asString());
        if (identity != 0)
        {
            for (EntityID candidate : World::GetView<MaterialComponent>())
            {
                if (ComponentManager::GetComponentUnchecked<MaterialComponent>(candidate).MaterialSceneIdentity == identity)
                {
                    entity = candidate;
                    break;
                }
            }
        }
        if (entity == g_kINVALID_ENTITY)
        {
            EntityID match = g_kINVALID_ENTITY;
            bool ambiguous = false;
            for (EntityID candidate : World::GetView<MaterialComponent>())
            {
                const auto& existing = ComponentManager::GetComponentUnchecked<MaterialComponent>(candidate);
                if (existing.MaterialSceneIdentity == 0 && MatchesSavedSource(record, candidate))
                {
                    if (match != g_kINVALID_ENTITY)
                    {
                        ambiguous = true;
                    }
                    match = candidate;
                }
            }
            if (!ambiguous)
            {
                entity = match;
            }
        }
        if (entity == g_kINVALID_ENTITY)
        {
            auto restored = World::CreateEntity().Add<TransformComponent>().Add<MaterialComponent>().Add<NameComponent>();
            if (!restored.IsValid())
            {
                error = "Scene entity capacity exceeded";
                return false;
            }
            entity = restored.GetID();
            if (record.isMember("modelPath") && !record["modelPath"].asString().empty())
            {
                const auto modelPath = record["modelPath"].asString();
                const bool convert = record["convert"].asBool();
                if (record["animated"].asBool())
                {
                    restored.Add<AnimationModelComponent>();
                    auto& model = restored.Get<AnimationModelComponent>();
                    model.ModelId = ModelManager::LoadAnimModel(modelPath.c_str(), convert);
                    model.ModelPath = modelPath;
                    model.IsConvert = convert;
                }
                else
                {
                    restored.Add<StaticModelComponent>();
                    auto& model = restored.Get<StaticModelComponent>();
                    model.ModelId = ModelManager::LoadStaticModel(modelPath.c_str(), convert);
                    model.ModelPath = modelPath;
                    model.IsConvert = convert;
                }
            }
            else if (record.isMember("vertices") && !record["vertices"].empty())
            {
                restored.Add<MeshComponent>();
                std::vector<Vertex> vertices;
                for (const auto& encoded : record["vertices"])
                {
                    vertices.push_back({{encoded[0].asFloat(), encoded[1].asFloat(), encoded[2].asFloat()},
                        {encoded[3].asFloat(), encoded[4].asFloat(), encoded[5].asFloat()},
                        {encoded[6].asFloat(), encoded[7].asFloat()},
                        {encoded[8].asFloat(), encoded[9].asFloat(), encoded[10].asFloat(), encoded[11].asFloat()}});
                }
                if (!PrimitiveGeometry::UploadVertices(entity, vertices))
                {
                    error = "Failed to restore scene mesh";
                    World::DestroyEntity(restored);
                    return false;
                }
            }
            else
            {
                World::DestroyEntity(restored);
                error = "Scene material entity is missing its mesh source";
                return false;
            }
        }
        if (record.isMember("name") && ComponentManager::HasComponent<NameComponent>(entity))
        {
            ComponentManager::GetComponentUnchecked<NameComponent>(entity).Name = record["name"].asString();
        }
        if (record.isMember("transform") && ComponentManager::HasComponent<TransformComponent>(entity))
        {
            auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
            transform.Position = ReadVector(record["transform"]["position"]);
            transform.Rotation = ReadVector(record["transform"]["rotation"]);
            transform.Scale = ReadVector(record["transform"]["scale"]);
            transform.IsDirty = true;
        }
        auto& component = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
        component.MaterialSceneIdentity = identity;
        component.MaterialSlots.clear();
        for (const auto& reference : record["materialSlots"])
        {
            component.MaterialSlots.push_back(ParseAssetHandle(reference.asString()));
        }
        GpuSceneChanges::Notify(entity, GpuSceneDirtyFlags::All);
    }
    NotifyAssignmentsChanged();
    return true;
}
