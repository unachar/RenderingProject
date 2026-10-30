#pragma once

#include "animationmodel.h"
#include "staticmodel.h"
#include <vector>
#include <memory>
#include <string>
#include <unordered_map>

class ModelManager
{
  private:
    static vector<unique_ptr<AnimationModelResource>> m_AnimModels;
    static vector<unique_ptr<StaticModelResource>> m_StaticModels;
    static unordered_map<string, int> m_AnimModelCache;
    static unordered_map<string, int> m_StaticModelCache;
    static unordered_map<int, weak_ptr<AnimationModelResource>> m_OwnedAnimModels;

    struct LoadModelResult
    {
        int ModelId = -1;
        bool Succeeded = false;
        double ElapsedMs = 0.0;
        string Error;
    };

    static LoadModelResult LoadAnimModelDetailed(const char* fileName, bool isConvert = true);
    static LoadModelResult LoadStaticModelDetailed(const char* fileName, bool isConvert = true);

  public:
    static int LoadAnimModel(const char* fileName, bool isConvert = true);
    // Fresh mutable pose/GPU buffers; not inserted into the file cache.
    static int CreateAnimModelInstance(int sourceId);
    // Transfers an independently loaded pose into scene ownership, without entering the file cache.
    static int RegisterAnimModelInstance(unique_ptr<AnimationModelResource> model,
                                         shared_ptr<AnimationModelResource>& owner);
    static void CollectRetiredAnimModels();
    static int LoadStaticModel(const char* fileName, bool isConvert = true);

    static bool LoadAnimation(int modelId, const char* fileName, const char* name);

    static AnimationModelResource* GetAnimModel(int modelId);
    static StaticModelResource* GetStaticModel(int modelId);
    // Resource-local indices; pointer lifetime ends on unload or hot reload.
    static const ModelResourceMap* GetResourceMap(int modelId, bool animated);
    static void UpdateVirtualGeometry();
    static StaticModelResource* FindLoadedStaticModel(const std::filesystem::path& path);
    static bool SaveVirtualGeometrySettings(const std::filesystem::path& path, const VGImportSettings& settings,
                                            std::string& error);

    static void Uninit();
};
