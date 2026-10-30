#define RENDERINGPROJECT_DISABLE_RENDER_CALL_INSTRUMENTATION
#define NOMINMAX
#include "../source/pch.h"
#include "../source/scene/scenefile.h"
#include <cassert>
#include <iostream>
#include <sstream>

#include "component-type-registry.inc"

int main()
{
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _CALL_REPORTFAULT);
    AnimationModelComponent original;
    original.ModelPath = "model.pmx";
    original.UseIndependentPose = true;
    original.AnimationPaths = {"body.vmd", "face.vmd", "idle.vmd"};
    original.Animations = {"body", "face", "idle"};
    original.CurrentAnimation = "body";
    original.ActiveAnimationLayers = {{"body", 3.0f}, {"face", 3.0f}};
    original.Speed = 0.75f;
    Json::Value record;
    SceneFile::Archive writer(SceneFile::Mode::Write, record);
    SceneFile::Visit(writer, original);
    assert(record["independentPose"].asBool());
    Json::StreamWriterBuilder builder;
    std::istringstream serialized(Json::writeString(builder, record));
    Json::CharReaderBuilder parser;
    Json::Value saved;
    string error;
    assert(Json::parseFromStream(parser, serialized, &saved, &error));
    AnimationModelComponent restored;
    SceneFile::Archive reader(SceneFile::Mode::Read, saved);
    SceneFile::Visit(reader, restored);
    assert(restored.ModelPath == original.ModelPath && restored.UseIndependentPose);
    assert(restored.AnimationPaths == original.AnimationPaths);
    assert(restored.Animations == original.Animations);
    assert(restored.CurrentAnimation == "body" && restored.Speed == 0.75f);
    assert(restored.ActiveAnimationLayers.size() == 2);
    assert(restored.ActiveAnimationLayers[1].AnimationName == "face");
    assert(restored.ActiveAnimationLayers[0].CurrentTime == 0.0f);
    record.removeMember("independentPose");
    AnimationModelComponent legacy;
    SceneFile::Archive legacyReader(SceneFile::Mode::Read, record);
    SceneFile::Visit(legacyReader, legacy);
    assert(!legacy.UseIndependentPose);
    std::cout << "Animation scene settings round-trip and backward compatibility passed\n";
}
