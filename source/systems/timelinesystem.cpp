#include "pch.h"
#include "timelinesystem.h"
#include "world.h"
#include "componentmanager.h"
#include "projectmanager.h"
#include "animator.h"
#include "camera.h"
#include "materialmanager.h"
#include "modelmanager.h"
#include "texturemanager.h"
#include "localheightfog.h"
#include "gpuscene/gpuscenechanges.h"
#include <filesystem>
#include "../audio/xaudiomusicplayer.h"
#include "../audio/livemusicclock.h"
#include <algorithm>

namespace
{
    struct MusicPlayback
    {
        LiveMusic::XAudioMusicPlayer Player;
        string Path;
        uint64_t Session = 0;
        bool Playing = false;
        float Offset = 0.0f;
    };

    unordered_map<EntityID, unique_ptr<MusicPlayback>> g_Music;

    bool AudioError(TimelineComponent& timeline, const char* operation)
    {
        timeline.PlaybackError = string("XAudio2: ") + operation + ": " + timeline.MusicPath;
        timeline.IsPlaying = false;
        timeline.SeekRequested = false;
        return false;
    }

    bool UpdateMusic(EntityID entity, TimelineComponent& timeline)
    {
        auto& entry = g_Music[entity];
        const uint64_t session = ProjectManager::GetPlaySession();
        if (entry && (entry->Path != timeline.MusicPath || entry->Session != session))
        {
            entry.reset();
        }
        if (!entry)
        {
            entry = make_unique<MusicPlayback>();
            entry->Path = timeline.MusicPath;
            entry->Session = session;
            if (!entry->Player.OpenWav(timeline.MusicPath))
            {
                entry.reset();
                return AudioError(timeline, "Cannot open PCM16 / float32 WAV or audio device");
            }
            timeline.SeekRequested = true;
        }
        auto& music = *entry;
        timeline.MusicDuration = static_cast<float>(music.Player.DurationSeconds());
        const float end = min(timeline.Duration, max(0.0f, timeline.MusicDuration - timeline.MusicOffset));
        if (timeline.SeekRequested || music.Offset != timeline.MusicOffset)
        {
            music.Player.Pause();
            music.Playing = false;
            if (!music.Player.Seek(timeline.CurrentTime + timeline.MusicOffset))
            {
                return AudioError(timeline, "Seek failed");
            }
            timeline.SeekRequested = false;
            music.Offset = timeline.MusicOffset;
        }
        timeline.CurrentTime = clamp(static_cast<float>(music.Player.TimeSeconds()) - timeline.MusicOffset, 0.0f, end);
        if (timeline.IsPlaying && timeline.CurrentTime >= end)
        {
            music.Player.Pause();
            music.Playing = false;
            if (timeline.Loop && end > 0.0f)
            {
                Camera::NotifyCut();
                if (!music.Player.Seek(timeline.MusicOffset))
                {
                    return AudioError(timeline, "Loop seek failed");
                }
                timeline.CurrentTime = 0.0f;
            }
            else
            {
                timeline.IsPlaying = false;
            }
        }
        if (timeline.IsPlaying != music.Playing)
        {
            if (timeline.IsPlaying)
            {
                if (!music.Player.Play())
                {
                    return AudioError(timeline, "Playback failed");
                }
            }
            else
            {
                music.Player.Pause();
            }
            music.Playing = timeline.IsPlaying;
        }
        timeline.PlaybackError.clear();
        return true;
    }
}

float ApplyEase(float value, TimelineInterpolation interpolation)
{
    value = clamp(value, 0.0f, 1.0f);
    switch (interpolation)
    {
        case TimelineInterpolation::Step:
            return 0.0f;
        case TimelineInterpolation::EaseIn:
            return value * value;
        case TimelineInterpolation::EaseOut:
            return 1.0f - (1.0f - value) * (1.0f - value);
        case TimelineInterpolation::EaseInOut:
            if (value < 0.5f)
            {
                return 2.0f * value * value;
            }
            return 1.0f - powf(-2.0f * value + 2.0f, 2.0f) * 0.5f;
        default:
            return value;
    }
}

XMFLOAT4 LerpValue(const XMFLOAT4& a, const XMFLOAT4& b, float t)
{
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
}

XMFLOAT4 SampleClip(const TimelineClipData& clip, float localTime)
{
    if (clip.Keys.empty())
    {
        return {};
    }
    if (localTime <= clip.Keys.front().Time)
    {
        return clip.Keys.front().Value;
    }
    for (size_t i = 1; i < clip.Keys.size(); ++i)
    {
        const auto& right = clip.Keys[i];
        if (localTime < right.Time)
        {
            const auto& left = clip.Keys[i - 1];
            const float span = max(0.00001f, right.Time - left.Time);
            const float t = ApplyEase((localTime - left.Time) / span, left.Interpolation);
            return LerpValue(left.Value, right.Value, t);
        }
    }
    return clip.Keys.back().Value;
}

void ApplyProperty(EntityID target, TimelineProperty property, const XMFLOAT4& value, int fogVolume)
{
    if (!Registry::IsAlive(target))
    {
        return;
    }
    if (property <= TimelineProperty::TransformScale && ComponentManager::HasComponent<TransformComponent>(target))
    {
        auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(target);
        XMFLOAT3* destination;
        if (property == TimelineProperty::TransformPosition)
        {
            destination = &transform.Position;
        }
        else
        {
            if (property == TimelineProperty::TransformRotation)
            {
                destination = &transform.Rotation;
            }
            else
            {
                destination = &transform.Scale;
            }
        }
        *destination = {value.x, value.y, value.z};
        transform.IsDirty = true;
        return;
    }
    if (property >= TimelineProperty::CameraTarget && property <= TimelineProperty::CameraLockOnOffset &&
        ComponentManager::HasComponent<CameraComponent>(target))
    {
        auto& camera = ComponentManager::GetComponentUnchecked<CameraComponent>(target);
        switch (property)
        {
            case TimelineProperty::CameraTarget:
                camera.Target = {value.x, value.y, value.z};
                break;
            case TimelineProperty::CameraFov:
                camera.Fov = clamp(value.x, XMConvertToRadians(1.0f), XMConvertToRadians(179.0f));
                break;
            case TimelineProperty::CameraNearClip:
                camera.NearClip = max(0.001f, value.x);
                break;
            case TimelineProperty::CameraFarClip:
                camera.FarClip = max(camera.NearClip + 0.001f, value.x);
                break;
            case TimelineProperty::CameraLockOnOffset:
                camera.LockOnOffset = {value.x, value.y, value.z};
                break;
            default:
                break;
        }
        return;
    }
    if (property == TimelineProperty::PostProcessIntensity &&
        ComponentManager::HasComponent<PostProcessComponent>(target))
    {
        ComponentManager::GetComponentUnchecked<PostProcessComponent>(target).Intensity = clamp(value.x, 0.0f, 1.0f);
    }
    if (property >= TimelineProperty::LightIntensity && property <= TimelineProperty::LightBeamDensity &&
        ComponentManager::HasComponent<LightComponent>(target))
    {
        auto& light = ComponentManager::GetComponentUnchecked<LightComponent>(target);
        switch (property)
        {
            case TimelineProperty::LightIntensity:
                light.Intensity = max(0.0f, value.x);
                break;
            case TimelineProperty::LightColor:
                light.Color = value;
                break;
            case TimelineProperty::LightDirection:
                if (XMVectorGetX(XMVector3LengthSq(XMLoadFloat4(&value))) > 0.000001f)
                {
                    XMStoreFloat3(&light.Direction, XMVector3Normalize(XMLoadFloat4(&value)));
                }
                break;
            case TimelineProperty::LightRange:
                light.Range = max(0.001f, value.x);
                break;
            case TimelineProperty::LightBeamDensity:
                light.VolumeDensity = max(0.0f, value.x);
                light.AffectsVolumetrics = light.VolumeDensity > 0.0f;
                break;
        }
    }
    if (property >= TimelineProperty::BloomThreshold && property <= TimelineProperty::BloomRadius &&
        ComponentManager::HasComponent<PostProcessComponent>(target))
    {
        auto& post = ComponentManager::GetComponentUnchecked<PostProcessComponent>(target);
        if (property == TimelineProperty::BloomThreshold)
        {
            post.BloomThreshold = max(0.0f, value.x);
        }
        if (property == TimelineProperty::BloomSoftKnee)
        {
            post.BloomSoftKnee = clamp(value.x, 0.0f, 1.0f);
        }
        if (property == TimelineProperty::BloomRadius)
        {
            post.BloomRadius = max(0.0f, value.x);
        }
    }
    auto& fog = LocalHeightFog::GetMutableVolumes();
    if (fogVolume >= 0 && static_cast<size_t>(fogVolume) < fog.size())
    {
        if (property == TimelineProperty::FogDensity)
        {
            fog[fogVolume].Density = max(0.0f, value.x);
        }
        if (property == TimelineProperty::FogColor)
        {
            fog[fogVolume].Color = {value.x, value.y, value.z};
        }
    }
    if (property == TimelineProperty::AnimationTime)
    {
        if (ComponentManager::HasComponent<AnimationModelComponent>(target))
        {
            auto& animation = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(target);
            if (animation.CurrentAnimation.empty() && !animation.Animations.empty())
            {
                Animator::Play(animation, animation.Animations.front());
            }
            animation.TimelineDriven = true;
            animation.CurrentTime = animation.NextTime = max(0.0f, value.x);
            for (auto& layer : animation.ActiveAnimationLayers)
            {
                layer.CurrentTime = animation.CurrentTime;
            }
            if (!ProjectManager::IsSimulationRunning())
            {
                if (auto* model = ModelManager::GetAnimModel(animation.ModelId))
                {
                    if (animation.ActiveAnimationLayers.size() > 1)
                    {
                        model->UpdateBoneMatrices(animation.ActiveAnimationLayers);
                    }
                    else if (!animation.CurrentAnimation.empty())
                    {
                        model->UpdateBoneMatrices(animation.CurrentAnimation.c_str(), animation.CurrentTime,
                                                  animation.CurrentAnimation.c_str(), animation.CurrentTime, 0.0f);
                    }
                }
            }
        }
        if (ComponentManager::HasComponent<CameraComponent>(target) &&
            ComponentManager::HasComponent<TransformComponent>(target))
        {
            auto& camera = ComponentManager::GetComponentUnchecked<CameraComponent>(target);
            if (!camera.Animation.Path.empty() &&
                (!camera.Animation.Clip || camera.Animation.LoadedPath != camera.Animation.Path))
            {
                Animator::Play(camera, false);
            }
            camera.Animation.TimelineDriven = true;
            camera.Animation.CurrentTime = max(0.0f, value.x);
            Animator::Update(camera, ComponentManager::GetComponentUnchecked<TransformComponent>(target), 0.0f);
        }
    }
    if ((property == TimelineProperty::EmissiveIntensity || property == TimelineProperty::EmissiveColor) &&
        ComponentManager::HasComponent<MaterialComponent>(target))
    {
        auto& material = ComponentManager::GetComponentUnchecked<MaterialComponent>(target);
        if (!material.MaterialSlots.empty() || material.RuntimeHandle)
        {
            MaterialComponent binding = MaterialManager::Resolve(material);
            binding.MaterialSlots.clear();
            binding.RuntimeHandle = 0;
            material = move(binding);
        }
        if (property == TimelineProperty::EmissiveIntensity)
        {
            material.EmissiveIntensity = max(0.0f, value.x);
        }
        else
        {
            material.EmissiveColor = {value.x, value.y, value.z};
        }
        material.LiveSurfaceOverride = true;
        GpuSceneChanges::Notify(target, GpuSceneDirtyFlags::Material);
    }
}

vector<unique_ptr<TimeLineDirector>> TimeLineSystem::m_Directors;

void TimeLineSystem::Update()
{
    ComponentManager::ForEachComponent<AnimationModelComponent>([](EntityID, auto& animation) {
        animation.TimelineDriven = false;
    });
    ComponentManager::ForEachComponent<CameraComponent>([](EntityID, auto& camera) {
        camera.Animation.TimelineDriven = false;
    });
    const float deltaTime = World::GetDeltaTime();
    for (auto& director : m_Directors)
    {
        if (director)
        {
            director->Update(deltaTime);
        }
    }

    for (auto it = g_Music.begin(); it != g_Music.end();)
    {
        if (!Registry::IsAlive(it->first) || !ComponentManager::HasComponent<TimelineComponent>(it->first) ||
            ComponentManager::GetComponentUnchecked<TimelineComponent>(it->first).MusicPath.empty())
        {
            it = g_Music.erase(it);
        }
        else
        {
            ++it;
        }
    }
    ComponentManager::ForEachComponent<TimelineComponent>([&](EntityID entity, TimelineComponent& timeline) {
        const uint64_t session = ProjectManager::GetPlaySession();
        if (timeline.LastPlaySession != session)
        {
            timeline.LastPlaySession = session;
            timeline.CurrentTime = 0.0f;
            timeline.IsPlaying = timeline.PlayOnAwake;
            timeline.PlaybackError.clear();
        }
        if (!timeline.MusicPath.empty())
        {
            if (timeline.PlaybackError.empty() || timeline.IsPlaying || timeline.SeekRequested)
            {
                UpdateMusic(entity, timeline);
            }
        }
        else if (timeline.IsPlaying)
        {
            timeline.CurrentTime += deltaTime * timeline.Speed;
            if (timeline.CurrentTime >= timeline.Duration || timeline.CurrentTime < 0.0f)
            {
                if (timeline.Loop && timeline.Duration > 0.0f)
                {
                    Camera::NotifyCut();
                    timeline.CurrentTime =
                        fmodf(fmodf(timeline.CurrentTime, timeline.Duration) + timeline.Duration, timeline.Duration);
                }
                else
                {
                    timeline.CurrentTime = clamp(timeline.CurrentTime, 0.0f, timeline.Duration);
                    timeline.IsPlaying = false;
                }
            }
        }
        EvaluateComponent(timeline);
    });
}

void TimeLineSystem::Uninit()
{
    Clear();
}

TimeLineDirector& TimeLineSystem::CreateDirector()
{
    auto director = make_unique<TimeLineDirector>();
    TimeLineDirector& ref = *director;
    m_Directors.push_back(move(director));
    return ref;
}

void TimeLineSystem::Clear()
{
    SuspendAudio(true);
    m_Directors.clear();
}

void TimeLineSystem::SuspendAudio(bool close)
{
    if (close)
    {
        g_Music.clear();
        return;
    }
    for (auto& [entity, music] : g_Music)
    {
        if (music && music->Playing)
        {
            music->Player.Pause();
            music->Playing = false;
        }
    }
}

XMFLOAT4 TimeLineSystem::ReadProperty(EntityID target, TimelineProperty property, bool* valid, int fogVolume)
{
    if (valid)
    {
        *valid = false;
    }
    if (!Registry::IsAlive(target))
    {
        return {};
    }
    if (property <= TimelineProperty::TransformScale && ComponentManager::HasComponent<TransformComponent>(target))
    {
        const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(target);
        const XMFLOAT3* valueSelection = nullptr;
        if (property == TimelineProperty::TransformPosition)
        {
            valueSelection = &(transform.Position);
        }
        else if (property == TimelineProperty::TransformRotation)
        {
            valueSelection = &transform.Rotation;
        }
        else
        {
            valueSelection = &transform.Scale;
        }
        const XMFLOAT3& value = *valueSelection;
        if (valid)
        {
            *valid = true;
        }
        return {value.x, value.y, value.z, 0.0f};
    }
    if (property >= TimelineProperty::CameraTarget && property <= TimelineProperty::CameraLockOnOffset &&
        ComponentManager::HasComponent<CameraComponent>(target))
    {
        const auto& camera = ComponentManager::GetComponentUnchecked<CameraComponent>(target);
        if (valid)
        {
            *valid = true;
        }
        switch (property)
        {
            case TimelineProperty::CameraTarget:
                return {camera.Target.x, camera.Target.y, camera.Target.z, 0};
            case TimelineProperty::CameraFov:
                return {camera.Fov, 0, 0, 0};
            case TimelineProperty::CameraNearClip:
                return {camera.NearClip, 0, 0, 0};
            case TimelineProperty::CameraFarClip:
                return {camera.FarClip, 0, 0, 0};
            case TimelineProperty::CameraLockOnOffset:
                return {camera.LockOnOffset.x, camera.LockOnOffset.y, camera.LockOnOffset.z, 0};
            default:
                break;
        }
    }
    if (property == TimelineProperty::PostProcessIntensity &&
        ComponentManager::HasComponent<PostProcessComponent>(target))
    {
        if (valid)
        {
            *valid = true;
        }
        return {ComponentManager::GetComponentUnchecked<PostProcessComponent>(target).Intensity, 0, 0, 0};
    }
    XMFLOAT4 result{};
    bool found = false;
    if (property >= TimelineProperty::LightIntensity && property <= TimelineProperty::LightBeamDensity &&
        ComponentManager::HasComponent<LightComponent>(target))
    {
        const auto& light = ComponentManager::GetComponentUnchecked<LightComponent>(target);
        found = true;
        switch (property)
        {
            case TimelineProperty::LightIntensity:
                result.x = light.Intensity;
                break;
            case TimelineProperty::LightColor:
                result = light.Color;
                break;
            case TimelineProperty::LightDirection:
                result = {light.Direction.x, light.Direction.y, light.Direction.z, 0};
                break;
            case TimelineProperty::LightRange:
                result.x = light.Range;
                break;
            case TimelineProperty::LightBeamDensity:
                result.x = light.VolumeDensity;
                break;
        }
    }
    if (property >= TimelineProperty::BloomThreshold && property <= TimelineProperty::BloomRadius &&
        ComponentManager::HasComponent<PostProcessComponent>(target))
    {
        const auto& post = ComponentManager::GetComponentUnchecked<PostProcessComponent>(target);
        found = true;
        if (property == TimelineProperty::BloomThreshold)
        {
            result.x = post.BloomThreshold;
        }
        if (property == TimelineProperty::BloomSoftKnee)
        {
            result.x = post.BloomSoftKnee;
        }
        if (property == TimelineProperty::BloomRadius)
        {
            result.x = post.BloomRadius;
        }
    }
    const auto& fog = LocalHeightFog::GetVolumes();
    if ((property == TimelineProperty::FogDensity || property == TimelineProperty::FogColor) && fogVolume >= 0 &&
        static_cast<size_t>(fogVolume) < fog.size())
    {
        found = true;
        const auto& volume = fog[fogVolume];
        result = property == TimelineProperty::FogDensity ? XMFLOAT4{volume.Density, 0, 0, 0}
                                                          : XMFLOAT4{volume.Color.x, volume.Color.y, volume.Color.z, 1};
    }
    if (property == TimelineProperty::AnimationTime)
    {
        if (ComponentManager::HasComponent<AnimationModelComponent>(target))
        {
            found = true;
            result.x = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(target).CurrentTime;
        }
        else if (ComponentManager::HasComponent<CameraComponent>(target))
        {
            found = true;
            result.x = ComponentManager::GetComponentUnchecked<CameraComponent>(target).Animation.CurrentTime;
        }
    }
    if ((property == TimelineProperty::EmissiveIntensity || property == TimelineProperty::EmissiveColor) &&
        ComponentManager::HasComponent<MaterialComponent>(target))
    {
        const auto& material =
            MaterialManager::Resolve(ComponentManager::GetComponentUnchecked<MaterialComponent>(target));
        found = true;
        result = property == TimelineProperty::EmissiveIntensity
                     ? XMFLOAT4{material.EmissiveIntensity, 0, 0, 0}
                     : XMFLOAT4{material.EmissiveColor.x, material.EmissiveColor.y, material.EmissiveColor.z, 1};
    }
    if (valid)
    {
        *valid = found;
    }
    return result;
}

bool TimeLineSystem::PrepareTrackAnimations(TimelineTrackData& track)
{
    if (track.AnimationPaths.empty())
    {
        if (ComponentManager::HasComponent<AnimationModelComponent>(track.Target))
        {
            auto& animation = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(track.Target);
            if (!track.PreviousAnimationNames.empty())
            {
                Animator::Play(animation, track.PreviousAnimationNames, false);
            }
        }
        track.PreviousAnimationNames.clear();
        track.LoadedAnimationPaths.clear();
        track.LoadedAnimationNames.clear();
        track.LoadedAnimationModelId = -1;
        track.AnimationError.clear();
        return true;
    }
    if (!ComponentManager::HasComponent<AnimationModelComponent>(track.Target))
    {
        track.LoadedAnimationModelId = -1;
        track.AnimationError = "アニメーションモデルを対象Entityに指定してください。";
        return false;
    }
    auto& animation = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(track.Target);
    if (animation.ModelId < 0)
    {
        track.LoadedAnimationModelId = -1;
        track.AnimationError = "対象のモデルが読み込まれていません。";
        return false;
    }
    if (track.PreviousAnimationNames.empty())
    {
        for (const auto& layer : animation.ActiveAnimationLayers)
        {
            track.PreviousAnimationNames.push_back(layer.AnimationName);
        }
        if (track.PreviousAnimationNames.empty() && !animation.CurrentAnimation.empty())
        {
            track.PreviousAnimationNames.push_back(animation.CurrentAnimation);
        }
    }
    if (track.LoadedAnimationModelId != animation.ModelId || track.LoadedAnimationPaths != track.AnimationPaths)
    {
        track.LoadedAnimationModelId = animation.ModelId;
        track.LoadedAnimationPaths = track.AnimationPaths;
        track.LoadedAnimationNames.clear();
        track.AnimationError.clear();
        for (const auto& path : track.AnimationPaths)
        {
            // Reuse registered names so saving the selected animation remains reloadable.
            // New files use the full path to avoid collisions between equal filenames.
            string name = "timeline:" + path;
            bool registered = false;
            const size_t count = min(animation.AnimationPaths.size(), animation.Animations.size());
            for (size_t index = 0; index < count; ++index)
            {
                if (filesystem::u8path(animation.AnimationPaths[index]).lexically_normal() ==
                    filesystem::u8path(path).lexically_normal())
                {
                    name = animation.Animations[index];
                    registered = true;
                    break;
                }
            }
            if (!ModelManager::LoadAnimation(animation.ModelId, path.c_str(), name.c_str()))
            {
                track.AnimationError = "アニメーションを読み込めません: " + path;
                break;
            }
            if (!registered)
            {
                animation.AnimationPaths.push_back(path);
                animation.Animations.push_back(name);
            }
            track.LoadedAnimationNames.push_back(name);
        }
    }
    if (!track.AnimationError.empty())
    {
        return false;
    }
    Animator::Play(animation, track.LoadedAnimationNames, false);
    return true;
}

void TimeLineSystem::EvaluateComponent(TimelineComponent& timeline)
{
    const LiveMusic::TempoMap tempo(max(1.0f, timeline.BeatsPerMinute));
    const float beat = static_cast<float>(tempo.SecondsToBeats(timeline.CurrentTime - timeline.BeatOffset));
    for (const auto& group : timeline.LightGroups)
    {
        if (!group.Enabled)
        {
            continue;
        }
        for (const auto& fixture : group.Fixtures)
        {
            if (!ComponentManager::HasComponent<LightComponent>(fixture.Target))
            {
                continue;
            }
            auto& light = ComponentManager::GetComponentUnchecked<LightComponent>(fixture.Target);
            const float phase = beat / max(0.001f, group.SweepBeats) + fixture.Phase;
            const float pan = group.Pan + sinf(phase * XM_2PI) * group.PanSweep;
            const float tilt = group.Tilt + cosf(phase * XM_2PI) * group.TiltSweep;
            XMVECTOR direction = XMVectorSet(sinf(pan) * cosf(tilt), -sinf(tilt), cosf(pan) * cosf(tilt), 0);
            if (ComponentManager::HasComponent<TransformComponent>(fixture.Target))
            {
                const auto& rotation =
                    ComponentManager::GetComponentUnchecked<TransformComponent>(fixture.Target).Rotation;
                direction = XMVector3TransformNormal(direction,
                                                     XMMatrixRotationRollPitchYaw(rotation.x, rotation.y, rotation.z));
            }
            XMStoreFloat3(&light.Direction, XMVector3Normalize(direction));
            const float flash = beat * max(0.0f, group.StrobesPerBeat) + fixture.Phase;
            const bool illuminated =
                group.StrobesPerBeat <= 0.0f || flash - floorf(flash) < clamp(group.StrobeDuty, 0.0f, 1.0f);
            light.Intensity = illuminated ? max(0.0f, group.Intensity) : 0.0f;
            const bool alternate = group.AlternateOnBeat && static_cast<int>(floorf(beat + fixture.Phase)) % 2 != 0;
            light.Color = alternate ? group.AlternateColor : group.Color;
            light.VolumeDensity = max(0.0f, group.BeamDensity);
            light.AffectsVolumetrics = group.BeamDensity > 0.0f;
        }
    }
    for (const auto& monitor : timeline.Monitors)
    {
        if (monitor.Frames.empty() || !ComponentManager::HasComponent<MaterialComponent>(monitor.Target))
        {
            continue;
        }
        const double frameTime = max(0.0, static_cast<double>(timeline.CurrentTime - monitor.StartTime)) *
                                 max(0.001f, monitor.FramesPerSecond);
        const size_t frame = monitor.Loop
                                 ? static_cast<size_t>(fmod(frameTime, static_cast<double>(monitor.Frames.size())))
                                 : static_cast<size_t>(min(frameTime, static_cast<double>(monitor.Frames.size() - 1)));
        const string& path = monitor.Frames[frame];
        const int texture = TextureManager::LoadTexture(filesystem::u8path(path));
        if (texture < 0)
        {
            continue;
        }
        auto& material = ComponentManager::GetComponentUnchecked<MaterialComponent>(monitor.Target);
        if (!material.MaterialSlots.empty() || material.RuntimeHandle)
        {
            MaterialComponent binding = MaterialManager::Resolve(material);
            binding.MaterialSlots.clear();
            binding.RuntimeHandle = 0;
            material = move(binding);
        }
        material.TextureID = material.EmissiveTextureID = texture;
        material.LiveSurfaceOverride = true;
        material.UseTexture = true;
        material.TexturePath = path;
        material.EmissiveColor = {1, 1, 1};
        material.EmissiveIntensity = max(0.0f, monitor.EmissiveIntensity);
        GpuSceneChanges::Notify(monitor.Target, GpuSceneDirtyFlags::Material);
        if (ComponentManager::HasComponent<LightComponent>(monitor.SurroundLight))
        {
            auto& light = ComponentManager::GetComponentUnchecked<LightComponent>(monitor.SurroundLight);
            if (light.Type == LightType::Rect)
            {
                light.AreaTexturePath = path;
                light.Intensity = max(0.0f, monitor.LightIntensity);
            }
        }
    }
    for (auto& track : timeline.Tracks)
    {
        if (!track.Enabled || !Registry::IsAlive(track.Target))
        {
            continue;
        }
        XMFLOAT4 accumulated{};
        float totalWeight = 0.0f;
        for (auto& clip : track.Clips)
        {
            if (!clip.Enabled || clip.Duration <= 0.0f || timeline.CurrentTime < clip.StartTime ||
                timeline.CurrentTime > clip.StartTime + clip.Duration || clip.Keys.empty())
            {
                continue;
            }
            const float local = timeline.CurrentTime - clip.StartTime;
            float weight = 1.0f;
            if (clip.BlendIn > 0.0f)
            {
                weight = min(weight, local / clip.BlendIn);
            }
            if (clip.BlendOut > 0.0f)
            {
                weight = min(weight, (clip.Duration - local) / clip.BlendOut);
            }
            weight = clamp(weight, 0.0f, 1.0f);
            const XMFLOAT4 value = SampleClip(clip, local);
            accumulated.x += value.x * weight;
            accumulated.y += value.y * weight;
            accumulated.z += value.z * weight;
            accumulated.w += value.w * weight;
            totalWeight += weight;
        }
        if (totalWeight <= 0.0f)
        {
            if (track.HasDefaultValue)
            {
                if (track.Property == TimelineProperty::AnimationTime && !PrepareTrackAnimations(track))
                {
                    continue;
                }
                ApplyProperty(track.Target, track.Property, track.DefaultValue, track.FogVolumeIndex);
            }
            continue;
        }
        if (totalWeight < 1.0f && track.HasDefaultValue)
        {
            const float baseWeight = 1.0f - totalWeight;
            accumulated.x += track.DefaultValue.x * baseWeight;
            accumulated.y += track.DefaultValue.y * baseWeight;
            accumulated.z += track.DefaultValue.z * baseWeight;
            accumulated.w += track.DefaultValue.w * baseWeight;
            totalWeight = 1.0f;
        }
        const float invWeight = 1.0f / totalWeight;
        accumulated.x *= invWeight;
        accumulated.y *= invWeight;
        accumulated.z *= invWeight;
        accumulated.w *= invWeight;
        if (track.Property == TimelineProperty::AnimationTime && !PrepareTrackAnimations(track))
        {
            continue;
        }
        ApplyProperty(track.Target, track.Property, accumulated, track.FogVolumeIndex);
    }
}
