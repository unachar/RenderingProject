#pragma once
#include <assimp/scene.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace AnimationProbe
{
    inline void Require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }
    template<class Key>
    void ValidateTimes(const Key* keys, unsigned count)
    {
        Require(count == 0 || keys != nullptr, "missing key array");
        for (unsigned index = 0; index < count; ++index)
            Require(std::isfinite(keys[index].mTime) && (index == 0 || keys[index].mTime >= keys[index - 1].mTime),
                    "nonfinite or unsorted key time");
    }
    template<class Key>
    unsigned Previous(const Key* keys, unsigned count, double time)
    {
        const auto* next = std::upper_bound(keys, keys + count, time,
            [](double value, const Key& key) { return value < key.mTime; });
        return next == keys ? 0 : static_cast<unsigned>(next - keys - 1);
    }
    template<class Key>
    bool NonUnitKeyTimes(const Key* keys, unsigned count)
    {
        if (count < 2) return false;
        for (unsigned index = 0; index < count; ++index)
            if (std::abs(keys[index].mTime - double(index)) > 1.0e-6) return true;
        return false;
    }
    template<class Key>
    float Alpha(const Key* keys, unsigned index, unsigned count, double time)
    {
        if (index + 1 >= count) return 0;
        const double interval = keys[index + 1].mTime - keys[index].mTime;
        return interval <= 0 ? 0 : static_cast<float>(std::clamp((time - keys[index].mTime) / interval, 0.0, 1.0));
    }
    inline aiVector3D Position(const aiVectorKey* keys, unsigned count, double time)
    {
        if (!count) return {};
        const unsigned index = Previous(keys, count, time);
        if (index + 1 == count) return keys[index].mValue;
        const float alpha = Alpha(keys, index, count, time);
        return keys[index].mValue * (1 - alpha) + keys[index + 1].mValue * alpha;
    }
    inline aiQuaternion Rotation(const aiQuatKey* keys, unsigned count, double time)
    {
        if (!count) return aiQuaternion(1, 0, 0, 0);
        const unsigned index = Previous(keys, count, time);
        if (index + 1 == count) return keys[index].mValue;
        aiQuaternion result;
        aiQuaternion::Interpolate(result, keys[index].mValue, keys[index + 1].mValue, Alpha(keys, index, count, time));
        return result.Normalize();
    }
    inline double AngleDegrees(const aiQuaternion& a, const aiQuaternion& b)
    {
        const double lengthA = std::sqrt(double(a.w) * a.w + double(a.x) * a.x + double(a.y) * a.y + double(a.z) * a.z);
        const double lengthB = std::sqrt(double(b.w) * b.w + double(b.x) * b.x + double(b.y) * b.y + double(b.z) * b.z);
        Require(lengthA > 0 && lengthB > 0, "zero quaternion in source clip");
        const double dot = std::abs((double(a.w) * b.w + double(a.x) * b.x + double(a.y) * b.y + double(a.z) * b.z) / (lengthA * lengthB));
        return 2 * std::acos(std::clamp(dot, 0.0, 1.0)) * 180.0 / 3.14159265358979323846;
    }
}

