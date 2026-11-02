#pragma once
// ============================================================================
// TimelineCurve : TimelineComponent のキー補間・トラック評価・キー編集の共通実装（ヘッダーのみ）。
//
//  ・ランタイム（TimeLineSystem::EvaluateComponent）とエディター（Sequencer の Curve 表示 / 編集）が
//    同じ関数を使うため、表示される曲線と実際の再生結果が一致する。
//  ・ECS / D3D / ImGui に依存しない（回帰テストを単体でビルドできる）。
//
//  キー重複ルール（同じクリップ内）
//    ・時刻差が kKeyTimeEpsilon 未満のキーは「同一時刻」とみなし、1 つしか存在しない。
//    ・InsertKey：同一時刻のキーがあれば値を上書きする（補間設定は既存キーを維持）。
//    ・MoveKeys ：移動したキーが移動していないキーと同一時刻になった場合、移動したキーが残る
//                （移動先にあったキーは削除）。選択キー同士は同じ量だけ動くため衝突しない。
// ============================================================================
#include "timelinecomponent.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace TimelineCurve
{
    inline constexpr float kKeyTimeEpsilon = 1.0e-4f;
    inline constexpr float kMinClipDuration = 1.0e-3f;

    // ---------------------------------------------------------------- 値の次元・単位
    inline int ValueDimension(TimelineProperty property)
    {
        switch (property)
        {
            case TimelineProperty::TransformPosition:
            case TimelineProperty::TransformRotation:
            case TimelineProperty::TransformScale:
            case TimelineProperty::CameraTarget:
            case TimelineProperty::CameraLockOnOffset:
            case TimelineProperty::LightDirection:
            case TimelineProperty::LightColor:
            case TimelineProperty::FogColor:
            case TimelineProperty::EmissiveColor:
                return 3;
            case TimelineProperty::CameraCut:
                return 0;
            default:
                return 1;
        }
    }

    inline bool IsColorProperty(TimelineProperty property)
    {
        return property == TimelineProperty::LightColor || property == TimelineProperty::FogColor ||
               property == TimelineProperty::EmissiveColor;
    }

    // 内部値がラジアンで、UI では度で表示するプロパティ。
    inline bool IsRadianProperty(TimelineProperty property)
    {
        return property == TimelineProperty::TransformRotation || property == TimelineProperty::CameraFov;
    }

    inline float GetComponent(const DirectX::XMFLOAT4& value, int channel)
    {
        switch (channel)
        {
            case 0:
                return value.x;
            case 1:
                return value.y;
            case 2:
                return value.z;
            default:
                return value.w;
        }
    }

    inline void SetComponent(DirectX::XMFLOAT4& value, int channel, float component)
    {
        switch (channel)
        {
            case 0:
                value.x = component;
                break;
            case 1:
                value.y = component;
                break;
            case 2:
                value.z = component;
                break;
            default:
                value.w = component;
                break;
        }
    }

    inline DirectX::XMFLOAT4 Lerp(const DirectX::XMFLOAT4& a, const DirectX::XMFLOAT4& b, float t)
    {
        return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
    }

    // ---------------------------------------------------------------- イージング
    inline float BezierAxis(float a1, float a2, float t)
    {
        // B(t) = 3(1-t)^2 t a1 + 3(1-t) t^2 a2 + t^3 （端点 0, 1）
        const float u = 1.0f - t;
        return 3.0f * u * u * t * a1 + 3.0f * u * t * t * a2 + t * t * t;
    }

    inline float BezierAxisDerivative(float a1, float a2, float t)
    {
        const float u = 1.0f - t;
        return 3.0f * u * u * a1 + 6.0f * u * t * (a2 - a1) + 3.0f * t * t * (1.0f - a2);
    }

    // CSS の cubic-bezier(x1, y1, x2, y2) と同じ定義。x から y を求める。
    inline float CubicBezierEase(float x, float x1, float y1, float x2, float y2)
    {
        x = std::clamp(x, 0.0f, 1.0f);
        x1 = std::clamp(x1, 0.0f, 1.0f);
        x2 = std::clamp(x2, 0.0f, 1.0f);
        if (x <= 0.0f)
        {
            return 0.0f;
        }
        if (x >= 1.0f)
        {
            return 1.0f;
        }
        // Newton 法（x1, x2 ∈ [0,1] なので x(t) は単調）。収束しなければ二分法で確定する。
        float t = x;
        for (int iteration = 0; iteration < 8; ++iteration)
        {
            const float error = BezierAxis(x1, x2, t) - x;
            if (std::fabs(error) < 1.0e-6f)
            {
                return BezierAxis(y1, y2, t);
            }
            const float slope = BezierAxisDerivative(x1, x2, t);
            if (std::fabs(slope) < 1.0e-6f)
            {
                break;
            }
            t = std::clamp(t - error / slope, 0.0f, 1.0f);
        }
        float low = 0.0f;
        float high = 1.0f;
        t = x;
        for (int iteration = 0; iteration < 32; ++iteration)
        {
            const float value = BezierAxis(x1, x2, t);
            if (std::fabs(value - x) < 1.0e-6f)
            {
                break;
            }
            if (value < x)
            {
                low = t;
            }
            else
            {
                high = t;
            }
            t = (low + high) * 0.5f;
        }
        return BezierAxis(y1, y2, t);
    }

    inline float ApplyEase(float t, TimelineInterpolation interpolation, const DirectX::XMFLOAT4& bezier)
    {
        t = std::clamp(t, 0.0f, 1.0f);
        switch (interpolation)
        {
            case TimelineInterpolation::Step:
                return 0.0f;
            case TimelineInterpolation::EaseIn:
                return t * t;
            case TimelineInterpolation::EaseOut:
                return 1.0f - (1.0f - t) * (1.0f - t);
            case TimelineInterpolation::EaseInOut:
                if (t < 0.5f)
                {
                    return 2.0f * t * t;
                }
                return 1.0f - std::pow(-2.0f * t + 2.0f, 2.0f) * 0.5f;
            case TimelineInterpolation::CubicBezier:
                return CubicBezierEase(t, bezier.x, bezier.y, bezier.z, bezier.w);
            case TimelineInterpolation::Linear:
            default:
                return t;
        }
    }

    // ---------------------------------------------------------------- サンプリング
    // 既存の SampleClip と同じ境界規則（左キー以上・右キー未満の区間で補間）。
    inline DirectX::XMFLOAT4 SampleKeys(const std::vector<TimelineKeyframe>& keys, float localTime)
    {
        if (keys.empty())
        {
            return {};
        }
        if (localTime <= keys.front().Time)
        {
            return keys.front().Value;
        }
        // 既存実装と同じ線形走査（未ソートの手書きデータでも結果が変わらない）。
        for (size_t index = 1; index < keys.size(); ++index)
        {
            const auto& rightKey = keys[index];
            if (localTime < rightKey.Time)
            {
                const auto& leftKey = keys[index - 1];
                const float span = std::max(0.00001f, rightKey.Time - leftKey.Time);
                const float t = ApplyEase((localTime - leftKey.Time) / span, leftKey.Interpolation, leftKey.Bezier);
                return Lerp(leftKey.Value, rightKey.Value, t);
            }
        }
        return keys.back().Value;
    }

    inline DirectX::XMFLOAT4 SampleClip(const TimelineClipData& clip, float localTime)
    {
        return SampleKeys(clip.Keys, localTime);
    }

    inline bool ClipContains(const TimelineClipData& clip, float time)
    {
        return clip.Enabled && clip.Duration > 0.0f && time >= clip.StartTime && time <= clip.StartTime + clip.Duration;
    }

    inline float ClipWeight(const TimelineClipData& clip, float localTime)
    {
        float weight = 1.0f;
        if (clip.BlendIn > 0.0f)
        {
            weight = std::min(weight, localTime / clip.BlendIn);
        }
        if (clip.BlendOut > 0.0f)
        {
            weight = std::min(weight, (clip.Duration - localTime) / clip.BlendOut);
        }
        return std::clamp(weight, 0.0f, 1.0f);
    }

    enum class TrackSample
    {
        None,    // 何も適用しない
        Default, // track.DefaultValue を適用（クリップ外）
        Value,   // value を適用
    };

    // EvaluateComponent のトラック評価（クリップ重み付け・既定値ブレンド・HoldOutside）。
    inline TrackSample EvaluateTrack(const TimelineTrackData& track, float time, DirectX::XMFLOAT4& value)
    {
        DirectX::XMFLOAT4 accumulated{};
        float totalWeight = 0.0f;
        for (const auto& clip : track.Clips)
        {
            if (!ClipContains(clip, time) || clip.Keys.empty())
            {
                continue;
            }
            const float local = time - clip.StartTime;
            const float weight = ClipWeight(clip, local);
            const DirectX::XMFLOAT4 sample = SampleClip(clip, local);
            accumulated.x += sample.x * weight;
            accumulated.y += sample.y * weight;
            accumulated.z += sample.z * weight;
            accumulated.w += sample.w * weight;
            totalWeight += weight;
        }
        if (totalWeight <= 0.0f)
        {
            if (track.HoldOutside)
            {
                // 直前に終わったクリップの終端値、なければ最初のクリップの先頭値を保持する。
                const TimelineClipData* before = nullptr;
                const TimelineClipData* first = nullptr;
                for (const auto& clip : track.Clips)
                {
                    if (!clip.Enabled || clip.Keys.empty())
                    {
                        continue;
                    }
                    const float end = clip.StartTime + std::max(0.0f, clip.Duration);
                    if (end <= time && (!before || end >= before->StartTime + before->Duration))
                    {
                        before = &clip;
                    }
                    if (!first || clip.StartTime < first->StartTime)
                    {
                        first = &clip;
                    }
                }
                if (before)
                {
                    value = SampleClip(*before, std::max(0.0f, before->Duration));
                    return TrackSample::Value;
                }
                if (first)
                {
                    value = SampleClip(*first, 0.0f);
                    return TrackSample::Value;
                }
            }
            if (track.HasDefaultValue)
            {
                value = track.DefaultValue;
                return TrackSample::Default;
            }
            return TrackSample::None;
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
        const float inverse = 1.0f / totalWeight;
        value = {accumulated.x * inverse, accumulated.y * inverse, accumulated.z * inverse, accumulated.w * inverse};
        return TrackSample::Value;
    }

    // CameraCut トラック群から、time に有効なカメラを返す（後から始まるクリップが優先）。
    inline EntityID FindCameraCut(const TimelineComponent& timeline, float time)
    {
        EntityID result = g_kINVALID_ENTITY;
        float bestStart = -1.0e30f;
        for (const auto& track : timeline.Tracks)
        {
            if (!track.Enabled || track.Property != TimelineProperty::CameraCut || track.Target == g_kINVALID_ENTITY)
            {
                continue;
            }
            for (const auto& clip : track.Clips)
            {
                if (ClipContains(clip, time) && clip.StartTime >= bestStart)
                {
                    bestStart = clip.StartTime;
                    result = track.Target;
                }
            }
        }
        return result;
    }

    // ---------------------------------------------------------------- 映像（LiveMonitor）・音声の区間
    inline float MonitorSequenceLength(const LiveMonitorBinding& monitor)
    {
        return static_cast<float>(monitor.Frames.size()) / std::max(0.001f, monitor.FramesPerSecond);
    }

    inline float MonitorEndTime(const LiveMonitorBinding& monitor, float timelineDuration)
    {
        if (monitor.EndTime > monitor.StartTime)
        {
            return monitor.EndTime;
        }
        if (monitor.Loop || monitor.Frames.empty())
        {
            return std::max(monitor.StartTime, timelineDuration);
        }
        return monitor.StartTime + std::max(0.0f, MonitorSequenceLength(monitor) - monitor.Offset);
    }

    // 表示するフレーム番号。更新しない時刻（無効・終了後）は false。
    inline bool MonitorFrame(const LiveMonitorBinding& monitor, float time, size_t& frame)
    {
        if (!monitor.Enabled || monitor.Frames.empty())
        {
            return false;
        }
        if (monitor.EndTime > monitor.StartTime && time > monitor.EndTime)
        {
            return false;
        }
        const double frameTime = std::max(0.0, static_cast<double>(time - monitor.StartTime + monitor.Offset)) *
                                 std::max(0.001f, monitor.FramesPerSecond);
        const double count = static_cast<double>(monitor.Frames.size());
        frame = monitor.Loop ? static_cast<size_t>(std::fmod(frameTime, count))
                             : static_cast<size_t>(std::min(frameTime, count - 1.0));
        return true;
    }

    // 音声はタイムライン 0 秒 = ファイルの MusicOffset 秒（既存の同期方式）。
    inline void AudioRange(const TimelineComponent& timeline, float& start, float& end)
    {
        start = -timeline.MusicOffset;
        end = timeline.MusicDuration > 0.0f ? timeline.MusicDuration - timeline.MusicOffset : timeline.Duration;
    }

    // ---------------------------------------------------------------- キー編集
    inline void SortKeys(std::vector<TimelineKeyframe>& keys)
    {
        std::stable_sort(keys.begin(), keys.end(),
                         [](const TimelineKeyframe& a, const TimelineKeyframe& b) { return a.Time < b.Time; });
    }

    inline int FindKey(const std::vector<TimelineKeyframe>& keys, float localTime)
    {
        for (size_t index = 0; index < keys.size(); ++index)
        {
            if (std::fabs(keys[index].Time - localTime) < kKeyTimeEpsilon)
            {
                return static_cast<int>(index);
            }
        }
        return -1;
    }

    // 同一時刻のキーがあれば値を上書き、なければ挿入。挿入（上書き）したキーの index を返す。
    inline int InsertKey(TimelineClipData& clip, const TimelineKeyframe& key)
    {
        const int existing = FindKey(clip.Keys, key.Time);
        if (existing >= 0)
        {
            clip.Keys[existing].Value = key.Value;
            return existing;
        }
        const auto position = std::upper_bound(clip.Keys.begin(), clip.Keys.end(), key.Time,
                                               [](float time, const TimelineKeyframe& k) { return time < k.Time; });
        // insert は再確保しうるため、begin() は挿入後に取得する（同一式で評価すると順序未規定）。
        const auto inserted = clip.Keys.insert(position, key);
        return static_cast<int>(inserted - clip.Keys.begin());
    }

    // クリップの開始時刻を変えても、キーの絶対時刻（= 内容）は動かさない。
    inline void SetClipStartKeepContent(TimelineClipData& clip, float newStart)
    {
        const float delta = newStart - clip.StartTime;
        clip.StartTime = newStart;
        for (auto& key : clip.Keys)
        {
            key.Time -= delta;
        }
    }

    inline void ClipNeighbours(const TimelineTrackData& track, int clipIndex, float& minStart, float& maxEnd)
    {
        minStart = -1.0e30f;
        maxEnd = 1.0e30f;
        const auto& clip = track.Clips[clipIndex];
        for (int index = 0; index < static_cast<int>(track.Clips.size()); ++index)
        {
            if (index == clipIndex)
            {
                continue;
            }
            const auto& other = track.Clips[index];
            const float otherEnd = other.StartTime + other.Duration;
            if (otherEnd <= clip.StartTime + kKeyTimeEpsilon)
            {
                minStart = std::max(minStart, otherEnd);
            }
            if (other.StartTime >= clip.StartTime + clip.Duration - kKeyTimeEpsilon)
            {
                maxEnd = std::min(maxEnd, other.StartTime);
            }
        }
    }

    // 絶対時刻 time を含むクリップの index。なければ最寄りのクリップを（隣接クリップと重ならない範囲で）伸ばし、
    // クリップが 1 つもなければ新規作成する。
    inline int EnsureClipForTime(TimelineTrackData& track, float time, const char* newClipName = "Keys")
    {
        int nearest = -1;
        float nearestDistance = 1.0e30f;
        for (int index = 0; index < static_cast<int>(track.Clips.size()); ++index)
        {
            const auto& clip = track.Clips[index];
            const float start = clip.StartTime;
            const float end = clip.StartTime + clip.Duration;
            if (time >= start - kKeyTimeEpsilon && time <= end + kKeyTimeEpsilon)
            {
                return index;
            }
            const float distance = time < start ? start - time : time - end;
            if (distance < nearestDistance)
            {
                nearestDistance = distance;
                nearest = index;
            }
        }
        if (nearest >= 0)
        {
            auto& clip = track.Clips[nearest];
            if (time < clip.StartTime)
            {
                const float end = clip.StartTime + clip.Duration;
                SetClipStartKeepContent(clip, time);
                clip.Duration = end - time;
            }
            else
            {
                clip.Duration = time - clip.StartTime;
            }
            return nearest;
        }
        TimelineClipData clip{};
        clip.Name = newClipName;
        clip.StartTime = std::max(0.0f, time);
        clip.Duration = kMinClipDuration;
        track.Clips.push_back(clip);
        return static_cast<int>(track.Clips.size()) - 1;
    }

    // 絶対時刻にキーを追加（同一時刻は上書き）。clipIndex / keyIndex を返す。
    inline std::pair<int, int> AddKeyAt(TimelineTrackData& track, float time, const DirectX::XMFLOAT4& value,
                                        TimelineInterpolation interpolation = TimelineInterpolation::Linear)
    {
        time = std::max(0.0f, time);
        const int clipIndex = EnsureClipForTime(track, time);
        auto& clip = track.Clips[clipIndex];
        TimelineKeyframe key{};
        key.Time = time - clip.StartTime;
        key.Value = value;
        key.Interpolation = interpolation;
        return {clipIndex, InsertKey(clip, key)};
    }

    struct KeyRef
    {
        int Track = -1;
        int Clip = -1;
        int Key = -1;

        bool operator==(const KeyRef& other) const
        {
            return Track == other.Track && Clip == other.Clip && Key == other.Key;
        }

        bool operator<(const KeyRef& other) const
        {
            if (Track != other.Track)
            {
                return Track < other.Track;
            }
            if (Clip != other.Clip)
            {
                return Clip < other.Clip;
            }
            return Key < other.Key;
        }
    };

    inline bool IsValid(const TimelineComponent& timeline, const KeyRef& ref)
    {
        return ref.Track >= 0 && ref.Track < static_cast<int>(timeline.Tracks.size()) && ref.Clip >= 0 &&
               ref.Clip < static_cast<int>(timeline.Tracks[ref.Track].Clips.size()) && ref.Key >= 0 &&
               ref.Key < static_cast<int>(timeline.Tracks[ref.Track].Clips[ref.Clip].Keys.size());
    }

    inline float KeyAbsoluteTime(const TimelineComponent& timeline, const KeyRef& ref)
    {
        const auto& clip = timeline.Tracks[ref.Track].Clips[ref.Clip];
        return clip.StartTime + clip.Keys[ref.Key].Time;
    }

    struct KeyOrigin
    {
        KeyRef Ref;
        float AbsoluteTime = 0.0f;
    };

    // 選択キーを delta 秒移動する。origins はドラッグ開始時の位置（ドラッグ中は毎フレーム同じ origins を渡す）。
    // 移動後のキーが所属クリップの外に出る場合、隣のクリップと重ならない範囲でクリップを伸ばす。
    // 衝突規則：移動したキーと同時刻の非選択キーは削除される。selection は新しい index に更新される。
    inline void MoveKeys(TimelineComponent& timeline, const std::vector<KeyOrigin>& origins, float delta,
                         std::vector<KeyRef>& selection)
    {
        selection.clear();
        // クリップ単位で処理する。
        std::vector<KeyOrigin> sorted = origins;
        std::sort(sorted.begin(), sorted.end(),
                  [](const KeyOrigin& a, const KeyOrigin& b) { return a.Ref < b.Ref; });
        size_t begin = 0;
        while (begin < sorted.size())
        {
            size_t end = begin;
            const int trackIndex = sorted[begin].Ref.Track;
            const int clipIndex = sorted[begin].Ref.Clip;
            while (end < sorted.size() && sorted[end].Ref.Track == trackIndex && sorted[end].Ref.Clip == clipIndex)
            {
                ++end;
            }
            if (trackIndex < 0 || trackIndex >= static_cast<int>(timeline.Tracks.size()) || clipIndex < 0 ||
                clipIndex >= static_cast<int>(timeline.Tracks[trackIndex].Clips.size()))
            {
                begin = end;
                continue;
            }
            auto& track = timeline.Tracks[trackIndex];
            auto& clip = track.Clips[clipIndex];
            float minStart = 0.0f;
            float maxEnd = 0.0f;
            ClipNeighbours(track, clipIndex, minStart, maxEnd);
            minStart = std::max(0.0f, minStart);

            // 目標の絶対時刻（隣接クリップの範囲に制限）
            struct Moved
            {
                int Key;
                float Time;
            };
            std::vector<Moved> moved;
            moved.reserve(end - begin);
            float lowest = 1.0e30f;
            float highest = -1.0e30f;
            for (size_t index = begin; index < end; ++index)
            {
                const int key = sorted[index].Ref.Key;
                if (key < 0 || key >= static_cast<int>(clip.Keys.size()))
                {
                    continue;
                }
                const float target = std::clamp(sorted[index].AbsoluteTime + delta, minStart, maxEnd);
                moved.push_back({key, target});
                lowest = std::min(lowest, target);
                highest = std::max(highest, target);
            }
            if (moved.empty())
            {
                begin = end;
                continue;
            }
            // クリップ範囲を必要なだけ広げる（内容の絶対時刻は保つ）。
            const float clipEnd = clip.StartTime + clip.Duration;
            if (lowest < clip.StartTime)
            {
                SetClipStartKeepContent(clip, lowest);
                clip.Duration = clipEnd - lowest;
            }
            if (highest > clip.StartTime + clip.Duration)
            {
                clip.Duration = highest - clip.StartTime;
            }
            // 衝突する非選択キーを除いて再構築する。
            std::vector<char> isMoved(clip.Keys.size(), 0);
            for (const auto& m : moved)
            {
                isMoved[m.Key] = 1;
            }
            std::vector<std::pair<TimelineKeyframe, char>> rebuilt;
            rebuilt.reserve(clip.Keys.size());
            for (size_t key = 0; key < clip.Keys.size(); ++key)
            {
                if (isMoved[key])
                {
                    continue;
                }
                const float absolute = clip.StartTime + clip.Keys[key].Time;
                bool collides = false;
                for (const auto& m : moved)
                {
                    if (std::fabs(m.Time - absolute) < kKeyTimeEpsilon)
                    {
                        collides = true;
                        break;
                    }
                }
                if (!collides)
                {
                    rebuilt.push_back({clip.Keys[key], 0});
                }
            }
            for (const auto& m : moved)
            {
                TimelineKeyframe key = clip.Keys[m.Key];
                key.Time = m.Time - clip.StartTime;
                rebuilt.push_back({key, 1});
            }
            std::stable_sort(rebuilt.begin(), rebuilt.end(),
                             [](const auto& a, const auto& b) { return a.first.Time < b.first.Time; });
            clip.Keys.clear();
            for (size_t key = 0; key < rebuilt.size(); ++key)
            {
                clip.Keys.push_back(rebuilt[key].first);
                if (rebuilt[key].second)
                {
                    selection.push_back({trackIndex, clipIndex, static_cast<int>(key)});
                }
            }
            begin = end;
        }
    }

    // 選択キーを削除する。
    inline void DeleteKeys(TimelineComponent& timeline, std::vector<KeyRef> refs)
    {
        // 後ろの index から消す
        std::sort(refs.begin(), refs.end());
        refs.erase(std::unique(refs.begin(), refs.end()), refs.end());
        for (auto it = refs.rbegin(); it != refs.rend(); ++it)
        {
            if (IsValid(timeline, *it))
            {
                auto& keys = timeline.Tracks[it->Track].Clips[it->Clip].Keys;
                keys.erase(keys.begin() + it->Key);
            }
        }
    }

    // Euler 角（ラジアン）を前のキーとの差が ±π 以内になるよう 2π 単位で補正する（最短回転）。
    inline bool UnwrapRotationKeys(TimelineTrackData& track)
    {
        constexpr float kTwoPi = 6.28318530718f;
        bool changed = false;
        bool hasPrevious = false;
        DirectX::XMFLOAT4 previous{};
        // クリップ開始順・キー順に並べた全キーについて処理する。
        std::vector<std::pair<float, TimelineKeyframe*>> ordered;
        for (auto& clip : track.Clips)
        {
            for (auto& key : clip.Keys)
            {
                ordered.push_back({clip.StartTime + key.Time, &key});
            }
        }
        std::stable_sort(ordered.begin(), ordered.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        for (auto& entry : ordered)
        {
            auto& value = entry.second->Value;
            if (hasPrevious)
            {
                for (int channel = 0; channel < 3; ++channel)
                {
                    float component = GetComponent(value, channel);
                    const float reference = GetComponent(previous, channel);
                    const float wrapped = component - kTwoPi * std::round((component - reference) / kTwoPi);
                    if (std::fabs(wrapped - component) > 1.0e-6f)
                    {
                        SetComponent(value, channel, wrapped);
                        changed = true;
                    }
                }
            }
            previous = value;
            hasPrevious = true;
        }
        return changed;
    }

    // ---------------------------------------------------------------- 変更検知用チェックサム（割り当てなし）
    inline void HashBytes(uint64_t& hash, const void* data, size_t size)
    {
        const auto* bytes = static_cast<const uint8_t*>(data);
        for (size_t index = 0; index < size; ++index)
        {
            hash ^= bytes[index];
            hash *= 1099511628211ull;
        }
    }

    inline uint64_t ComputeChecksum(const TimelineComponent& timeline)
    {
        uint64_t hash = 1469598103934665603ull;
        HashBytes(hash, &timeline.Duration, sizeof(float));
        HashBytes(hash, &timeline.MusicOffset, sizeof(float));
        HashBytes(hash, &timeline.MusicDuration, sizeof(float));
        const size_t trackCount = timeline.Tracks.size();
        HashBytes(hash, &trackCount, sizeof(trackCount));
        for (const auto& track : timeline.Tracks)
        {
            HashBytes(hash, &track.Target, sizeof(track.Target));
            HashBytes(hash, &track.Property, sizeof(track.Property));
            HashBytes(hash, &track.Enabled, sizeof(bool));
            HashBytes(hash, &track.HoldOutside, sizeof(bool));
            HashBytes(hash, &track.HasDefaultValue, sizeof(bool));
            HashBytes(hash, &track.DefaultValue, sizeof(track.DefaultValue));
            const size_t clipCount = track.Clips.size();
            HashBytes(hash, &clipCount, sizeof(clipCount));
            for (const auto& clip : track.Clips)
            {
                HashBytes(hash, &clip.StartTime, sizeof(float));
                HashBytes(hash, &clip.Duration, sizeof(float));
                HashBytes(hash, &clip.BlendIn, sizeof(float));
                HashBytes(hash, &clip.BlendOut, sizeof(float));
                HashBytes(hash, &clip.Enabled, sizeof(bool));
                const size_t keyCount = clip.Keys.size();
                HashBytes(hash, &keyCount, sizeof(keyCount));
                for (const auto& key : clip.Keys)
                {
                    HashBytes(hash, &key.Time, sizeof(float));
                    HashBytes(hash, &key.Value, sizeof(key.Value));
                    HashBytes(hash, &key.Interpolation, sizeof(key.Interpolation));
                    HashBytes(hash, &key.Bezier, sizeof(key.Bezier));
                }
            }
        }
        const size_t monitorCount = timeline.Monitors.size();
        HashBytes(hash, &monitorCount, sizeof(monitorCount));
        for (const auto& monitor : timeline.Monitors)
        {
            HashBytes(hash, &monitor.StartTime, sizeof(float));
            HashBytes(hash, &monitor.EndTime, sizeof(float));
            HashBytes(hash, &monitor.Offset, sizeof(float));
            const size_t frames = monitor.Frames.size();
            HashBytes(hash, &frames, sizeof(frames));
        }
        return hash;
    }
} // namespace TimelineCurve
