#define RENDERINGPROJECT_DISABLE_RENDER_CALL_INSTRUMENTATION
#define NOMINMAX
#include "pch.h"
#include "scenefile.h"
#include "animator.h"
#include "modelmanager.h"
#include "timelinesystem.h"
#include "vmdanimationimpoter.h"
#include <cassert>
#include <fstream>

#include "component-type-registry.inc"

namespace
{
    AnimationModelComponent character;
    int loadCount = 0;
}

template <> bool ComponentManager::HasComponent<AnimationModelComponent>(EntityID entity)
{
    return entity == 9;
}

template <> AnimationModelComponent& ComponentManager::GetComponentUnchecked<AnimationModelComponent>(EntityID)
{
    return character;
}

bool ModelManager::LoadAnimation(int, const char* path, const char*)
{
    ++loadCount;
    VmdAnimation clip;
    return LoadVmdAnimationFile(path, clip);
}

// Exercise the production selection/cache code without starting the GPU renderer.
#include "timeline-animation-selection.inc"
#include "animator-model-play.inc"
#include "animator-model-update.inc"

int main()
{
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _CALL_REPORTFAULT);
    const vector<string> paths = {"asset/model/animation/kasou.vmd", "asset/model/animation/kasou_kao.vmd"};
    character.ModelId = 3;
    character.CurrentAnimation = "original";
    TimelineTrackData track;
    track.Target = 9;
    track.Property = TimelineProperty::AnimationTime;
    track.AnimationPaths = paths;
    assert(TimeLineSystem::PrepareTrackAnimations(track));
    assert(character.ActiveAnimationLayers.size() == 2);
    assert(character.ActiveAnimationLayers[1].AnimationName == "timeline:" + paths[1]);
    assert(loadCount == 2);
    assert(character.AnimationPaths == paths);
    assert(character.Animations == track.LoadedAnimationNames);
    character.ActiveAnimationLayers[0].CurrentTime = 3.0f;
    character.ActiveAnimationLayers[1].CurrentTime = 3.0f;
    assert(TimeLineSystem::PrepareTrackAnimations(track));
    assert(loadCount == 2);
    assert(character.ActiveAnimationLayers[1].CurrentTime == 3.0f);
    character.TimelineDriven = true;
    Animator::Update(character, 1.0f);
    assert(character.ActiveAnimationLayers[0].CurrentTime == 3.0f);
    assert(character.ActiveAnimationLayers[1].CurrentTime == 3.0f);

    track.AnimationPaths.pop_back();
    assert(TimeLineSystem::PrepareTrackAnimations(track));
    assert(character.ActiveAnimationLayers.empty());
    assert(character.CurrentAnimation == "timeline:" + paths[0]);
    track.AnimationPaths.clear();
    assert(TimeLineSystem::PrepareTrackAnimations(track));
    assert(character.CurrentAnimation == "original");

    track.AnimationPaths = {"missing-animation.vmd"};
    assert(!TimeLineSystem::PrepareTrackAnimations(track));
    const int failedLoadCount = loadCount;
    assert(!TimeLineSystem::PrepareTrackAnimations(track));
    assert(loadCount == failedLoadCount);
    assert(!track.AnimationError.empty());
    assert(character.CurrentAnimation == "original");
    track.AnimationPaths = paths;
    assert(TimeLineSystem::PrepareTrackAnimations(track));
    assert(track.AnimationError.empty());

    character.Animations = {"dance", "face"};
    character.CurrentAnimation = "dance";
    character.ActiveAnimationLayers = {{"dance", 0.0f}, {"face", 0.0f}};
    track.PreviousAnimationNames.clear();
    track.LoadedAnimationModelId = -1;
    assert(TimeLineSystem::PrepareTrackAnimations(track));
    assert(character.CurrentAnimation == "dance");
    assert(character.ActiveAnimationLayers[1].AnimationName == "face");
    assert(character.AnimationPaths.size() == 2);
    track.Target = 8;
    assert(!TimeLineSystem::PrepareTrackAnimations(track));
    track.Target = 9;
    assert(TimeLineSystem::PrepareTrackAnimations(track));
    assert(track.AnimationError.empty());

    TimelineComponent original;
    original.Tracks.push_back(track);
    Json::Value record;
    SceneFile::Archive writer(SceneFile::Mode::Write, record);
    SceneFile::Visit(writer, original);
    assert(record["tracks"][0]["animationPaths"].size() == 2);
    assert(!record["tracks"][0].isMember("loadedAnimationModelId"));
    TimelineComponent restored;
    SceneFile::Archive reader(SceneFile::Mode::Read, record);
    SceneFile::Visit(reader, restored);
    assert(restored.Tracks[0].AnimationPaths == paths);
    assert(restored.Tracks[0].LoadedAnimationModelId == -1);
    record["tracks"][0].removeMember("animationPaths");
    TimelineComponent legacy;
    SceneFile::Archive legacyReader(SceneFile::Mode::Read, record);
    SceneFile::Visit(legacyReader, legacy);
    assert(legacy.Tracks[0].AnimationPaths.empty());

    VmdAnimation face;
    assert(LoadVmdAnimationFile(paths[1].c_str(), face));
    assert(!face.MorphTracks.empty());
    bool hasExpression = false;
    for (const auto& [name, keys] : face.MorphTracks)
    {
        for (const auto& key : keys)
        {
            hasExpression |= key.Value > 0.0f;
        }
    }
    assert(hasExpression);

    for (const char* path : {"asset/Scenes/LiveStage.scene", "asset/Scenes/LiveStage_LED_UI_Preserved_20261030.scene",
                             "asset/Prefabs/LiveStage.prefab"})
    {
        ifstream file(path);
        Json::Value scene;
        file >> scene;
        bool foundCharacter = false;
        bool foundTrack = false;
        for (const auto& entity : scene["entities"])
        {
            if (entity["id"].asUInt() == 9)
            {
                const auto& animation = entity["animationModel"];
                assert(animation["animationPaths"].size() == 2);
                assert(animation["layers"].size() == 2);
                foundCharacter = true;
            }
            for (const auto& savedTrack : entity["timeline"]["tracks"])
            {
                if (savedTrack["property"].asInt() == 19 && savedTrack["target"].asInt() == 9)
                {
                    assert(savedTrack["animationPaths"].size() == 2);
                    foundTrack = true;
                }
            }
        }
        assert(foundCharacter && foundTrack);
    }
    cout << "Timeline animation layering, caching, removal, failure recovery, scene persistence and face VMD passed\n";
}
