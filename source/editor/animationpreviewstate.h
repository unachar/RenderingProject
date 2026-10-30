#pragma once

#include <algorithm>
#include <cmath>
#include <cctype>
#include <string>
#include <unordered_set>
#include <vector>

namespace AnimationPreview
{
    struct Clip
    {
        std::string Name;
        std::string Path;
        bool Enabled = true;
        float Seconds = 0.0f;
    };

    inline bool ValidateClips(const std::vector<Clip>& clips)
    {
        std::unordered_set<std::string> names;
        for (const auto& clip : clips)
        {
            if (clip.Name.empty() || clip.Path.empty() || !names.insert(clip.Name).second)
            {
                return false;
            }
        }
        return true;
    }

    inline bool IsVmd(const Clip& clip)
    {
        std::string extension = clip.Path.substr(clip.Path.find_last_of('.') + 1);
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char letter) {
            return static_cast<char>(std::tolower(letter));
        });
        return extension == "vmd";
    }

    inline void SetClipEnabled(std::vector<Clip>& clips, size_t selected, bool enabled)
    {
        if (selected >= clips.size())
        {
            return;
        }
        clips[selected].Enabled = enabled;
        if (!enabled)
        {
            return;
        }
        for (size_t index = 0; index < clips.size(); ++index)
        {
            if (index != selected && (!IsVmd(clips[selected]) || !IsVmd(clips[index])))
            {
                clips[index].Enabled = false;
            }
        }
    }

    struct Selection
    {
        std::vector<std::string> Names;
        std::vector<std::string> Paths;
        std::vector<std::string> PlayingNames;
    };

    inline Selection BuildSelection(const std::vector<Clip>& clips)
    {
        Selection selection;
        for (const auto& clip : clips)
        {
            selection.Names.push_back(clip.Name);
            selection.Paths.push_back(clip.Path);
            if (clip.Enabled)
            {
                selection.PlayingNames.push_back(clip.Name);
            }
        }
        return selection;
    }

    inline float Duration(const std::vector<Clip>& clips)
    {
        float duration = 0.0f;
        for (const auto& clip : clips)
        {
            if (clip.Enabled && std::isfinite(clip.Seconds))
            {
                duration = std::max(duration, clip.Seconds);
            }
        }
        return duration;
    }

    inline float SampleTime(float time, float duration)
    {
        // Assimp playback wraps at exactly duration; hold the final pose while scrubbing the end.
        const float lastSample = std::max(0.0f, duration - 0.0001f);
        return std::clamp(time, 0.0f, lastSample);
    }

    inline float LayerSampleTime(float time, float clipDuration, float totalDuration, bool loop)
    {
        // Let the model's sampler wrap each clip at its own native period during loop playback.
        return SampleTime(time, loop ? totalDuration : clipDuration);
    }

    struct Playback
    {
        float Time = 0.0f;
        float Speed = 1.0f;
        bool Playing = false;
        bool Loop = true;

        void Seek(float time, float duration)
        {
            Time = std::isfinite(time) ? std::clamp(time, 0.0f, std::max(duration, 0.0f)) : 0.0f;
        }

        void Advance(float deltaTime, float duration)
        {
            if (!(duration > 0.0f) || !std::isfinite(duration))
            {
                Time = 0.0f;
                Playing = false;
                return;
            }
            if (!Playing || !std::isfinite(deltaTime) || deltaTime < 0.0f || !std::isfinite(Speed) || Speed < 0.0f)
            {
                return;
            }
            Time += deltaTime * Speed;
            if (Time >= duration)
            {
                if (Loop)
                {
                    Time = std::fmod(Time, duration);
                }
                else
                {
                    Time = duration;
                    Playing = false;
                }
            }
        }
    };
}
