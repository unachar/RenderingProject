#pragma once

#include "quaternion.h"
#include "vector3.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct aiAnimation;
struct aiNodeAnim;

namespace AnimationCompression
{
    struct LocalPose
    {
        aiQuaternion Rotation{1, 0, 0, 0};
        aiVector3D Position{};
    };

    struct ClipStatistics
    {
        size_t SourceKeyBytes = 0;
        size_t CompressedBytes = 0;
        uint32_t Samples = 0;
        double MaxPositionError = 0;
        double MaxRotationErrorDegrees = 0;
    };

    // Preserves the renderer's stepped FBX key-index playback, including short repeating tracks.
    // The compressed payload is owned; source channel pointers serve only as lookup identifiers.
    class FbxAclClip
    {
      public:
        static constexpr uint32_t MissingChannel = UINT32_MAX;
        FbxAclClip();
        ~FbxAclClip();
        FbxAclClip(FbxAclClip&&) noexcept;
        FbxAclClip& operator=(FbxAclClip&&) noexcept;
        FbxAclClip(const FbxAclClip&) = delete;
        FbxAclClip& operator=(const FbxAclClip&) = delete;

        bool Build(const aiAnimation& animation);
        bool Sample(float seconds, std::vector<LocalPose>& output);
        uint32_t FindChannel(const aiNodeAnim* channel) const;
        ClipStatistics GetStatistics() const;

        const std::string& LastError() const
        {
            return m_Error;
        }

      private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
        std::string m_Error;
    };
}
