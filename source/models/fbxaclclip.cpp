#include "fbxaclclip.h"
#include "scene.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <malloc.h>
#include <unordered_map>

#if RENDERINGPROJECT_USE_ACL
#include <acl/compression/compress.h>
#include <acl/compression/track_array.h>
#include <acl/compression/transform_error_metrics.h>
#include <acl/core/iallocator.h>
#include <acl/core/track_writer.h>
#include <acl/decompression/decompress.h>
#endif

namespace AnimationCompression
{
    namespace
    {
        bool Finite(const aiVector3D& value)
        {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }

        bool ValidRotation(const aiQuaternion& value)
        {
            const double length = double(value.w) * value.w + double(value.x) * value.x + double(value.y) * value.y +
                                  double(value.z) * value.z;
            return std::isfinite(length) && std::abs(length - 1.0) <= 0.0001;
        }

        double RotationError(const aiQuaternion& first, const aiQuaternion& second)
        {
            const double dot = double(first.w) * second.w + double(first.x) * second.x + double(first.y) * second.y +
                               double(first.z) * second.z;
            const double a = double(first.w) * first.w + double(first.x) * first.x + double(first.y) * first.y +
                             double(first.z) * first.z;
            const double b = double(second.w) * second.w + double(second.x) * second.x + double(second.y) * second.y +
                             double(second.z) * second.z;
            return 2.0 * std::acos((std::min)(1.0, std::abs(dot) / std::sqrt(a * b))) * 180.0 / 3.141592653589793;
        }

#if RENDERINGPROJECT_USE_ACL
        // SDK allocation helpers assume a non-null pointer. Throw before they can dereference one,
        // and reclaim any SDK temporaries that were not exception-safe during partial compression.
        class ClipAllocator : public acl::iallocator
        {
          public:
            ~ClipAllocator() override
            {
                for (const auto& allocation : m_Allocations)
                {
                    _aligned_free(allocation.first);
                }
            }

            void* allocate(size_t bytes, size_t alignment) override
            {
                constexpr size_t MaxBytes = 256ull * 1024 * 1024;
                if (bytes > MaxBytes - m_Bytes)
                {
                    throw std::bad_alloc();
                }
                void* memory = _aligned_malloc(bytes, (std::max)(alignment, sizeof(void*)));
                if (!memory)
                {
                    throw std::bad_alloc();
                }
                try
                {
                    m_Allocations.emplace(memory, bytes);
                }
                catch (...)
                {
                    _aligned_free(memory);
                    throw;
                }
                m_Bytes += bytes;
                return memory;
            }

            void deallocate(void* memory, size_t) override
            {
                const auto found = m_Allocations.find(memory);
                if (found != m_Allocations.end())
                {
                    m_Bytes -= found->second;
                    _aligned_free(memory);
                    m_Allocations.erase(found);
                }
            }

          private:
            std::unordered_map<void*, size_t> m_Allocations;
            size_t m_Bytes = 0;
        };

        struct DecompressionSettings : acl::default_transform_decompression_settings
        {
            static constexpr bool disable_fp_exeptions()
            {
                return true;
            }
        };

        struct PoseWriter : acl::track_writer
        {
            std::vector<LocalPose>& Output;

            explicit PoseWriter(std::vector<LocalPose>& output) : Output(output) {}

            static constexpr bool skip_all_scales()
            {
                return true;
            }

            void RTM_SIMD_CALL write_rotation(uint32_t index, rtm::quatf_arg0 rotation)
            {
                Output[index].Rotation = aiQuaternion(rtm::quat_get_w(rotation), rtm::quat_get_x(rotation),
                                                      rtm::quat_get_y(rotation), rtm::quat_get_z(rotation));
            }

            void RTM_SIMD_CALL write_translation(uint32_t index, rtm::vector4f_arg0 position)
            {
                Output[index].Position =
                    aiVector3D(rtm::vector_get_x(position), rtm::vector_get_y(position), rtm::vector_get_z(position));
            }
        };
#endif
    }

    struct FbxAclClip::Impl
    {
        ClipStatistics Statistics;
        std::unordered_map<const aiNodeAnim*, uint32_t> Channels;
        float Duration = 0;
        float TickRate = 25;
#if RENDERINGPROJECT_USE_ACL
        ClipAllocator Allocator;
        acl::compressed_tracks* Tracks = nullptr;
        acl::decompression_context<DecompressionSettings> Context;

        ~Impl()
        {
            if (Tracks)
            {
                Allocator.deallocate(Tracks, 0);
            }
        }

        void Decode(uint32_t sample, std::vector<LocalPose>& output)
        {
            output.assign(Channels.size(), LocalPose{});
            Context.seek(float(sample), acl::sample_rounding_policy::floor);
            PoseWriter writer(output);
            Context.decompress_tracks(writer);
        }
#endif
    };

    FbxAclClip::FbxAclClip() = default;
    FbxAclClip::~FbxAclClip() = default;
    FbxAclClip::FbxAclClip(FbxAclClip&&) noexcept = default;
    FbxAclClip& FbxAclClip::operator=(FbxAclClip&&) noexcept = default;

    bool FbxAclClip::Build(const aiAnimation& animation)
    {
        m_Impl.reset();
        m_Error = "Invalid or unsupported FBX clip";
#if RENDERINGPROJECT_USE_ACL
        if (!animation.mChannels || animation.mNumChannels == 0 || animation.mNumChannels > 4096 ||
            !std::isfinite(animation.mDuration) || animation.mDuration < 0 ||
            animation.mDuration > std::numeric_limits<float>::max() || !std::isfinite(animation.mTicksPerSecond) ||
            animation.mTicksPerSecond < 0 || animation.mTicksPerSecond > std::numeric_limits<float>::max())
        {
            return false;
        }
        try
        {
            auto candidate = std::make_unique<Impl>();
            candidate->Duration = float(animation.mDuration);
            candidate->TickRate = animation.mTicksPerSecond != 0 ? float(animation.mTicksPerSecond) : 25.0f;
            uint32_t samples = 1;
            for (unsigned index = 0; index < animation.mNumChannels; ++index)
            {
                const auto* channel = animation.mChannels[index];
                if (!channel || (channel->mNumPositionKeys && !channel->mPositionKeys) ||
                    (channel->mNumRotationKeys && !channel->mRotationKeys) || channel->mNumPositionKeys > 65536 ||
                    channel->mNumRotationKeys > 65536)
                {
                    return false;
                }
                if (!candidate->Channels.emplace(channel, index).second)
                {
                    m_Error = "Duplicate FBX channel pointer";
                    return false;
                }
                samples = (std::max)({samples, channel->mNumPositionKeys, channel->mNumRotationKeys});
                candidate->Statistics.SourceKeyBytes += size_t(channel->mNumPositionKeys) * sizeof(aiVectorKey) +
                                                        size_t(channel->mNumRotationKeys) * sizeof(aiQuatKey);
            }
            if (uint64_t(samples) * animation.mNumChannels > 1000000)
            {
                m_Error = "ACL raw sample budget exceeded";
                return false;
            }
            acl::track_array_qvvf raw(candidate->Allocator, animation.mNumChannels);
            for (unsigned index = 0; index < animation.mNumChannels; ++index)
            {
                const auto& channel = *animation.mChannels[index];
                if ((channel.mNumPositionKeys && samples % channel.mNumPositionKeys != 0) ||
                    (channel.mNumRotationKeys && samples % channel.mNumRotationKeys != 0))
                {
                    m_Error = "Incompatible repeating FBX key counts";
                    return false;
                }
                acl::track_desc_transformf description;
                description.output_index = index;
                description.parent_index = acl::k_invalid_track_index;
                description.precision = 0.00001f;
                description.shell_distance = 1.0f;
                auto track = acl::track_qvvf::make_reserve(description, candidate->Allocator, samples, 1.0f);
                for (uint32_t sample = 0; sample < samples; ++sample)
                {
                    LocalPose pose;
                    if (channel.mNumPositionKeys)
                    {
                        pose.Position = channel.mPositionKeys[sample % channel.mNumPositionKeys].mValue;
                    }
                    if (channel.mNumRotationKeys)
                    {
                        pose.Rotation = channel.mRotationKeys[sample % channel.mNumRotationKeys].mValue;
                    }
                    if (!Finite(pose.Position) || !ValidRotation(pose.Rotation))
                    {
                        m_Error = "Nonfinite position or nonunit FBX rotation";
                        return false;
                    }
                    track[sample] = rtm::qvv_set(rtm::quat_normalize(rtm::quat_set(pose.Rotation.x, pose.Rotation.y,
                                                                                   pose.Rotation.z, pose.Rotation.w)),
                                                 rtm::vector_set(pose.Position.x, pose.Position.y, pose.Position.z),
                                                 rtm::vector_set(1.0f));
                }
                raw[index] = std::move(track);
            }
            raw.set_looping_policy(acl::sample_looping_policy::clamp);
            auto settings = acl::get_default_compression_settings();
            settings.optimize_loops = false;
            acl::qvvf_transform_error_metric metric;
            settings.error_metric = &metric;
            acl::output_stats statistics;
            const auto error =
                acl::compress_track_list(candidate->Allocator, raw, settings, candidate->Tracks, statistics);
            if (error.any())
            {
                m_Error = error.c_str();
                return false;
            }
            if (!candidate->Tracks || candidate->Tracks->is_valid(true).any() ||
                !candidate->Context.initialize(*candidate->Tracks))
            {
                m_Error = "Invalid ACL compression output";
                return false;
            }
            candidate->Context.set_looping_policy(acl::sample_looping_policy::clamp);
            candidate->Statistics.Samples = samples;
            candidate->Statistics.CompressedBytes = candidate->Tracks->get_size();
            std::vector<LocalPose> decoded;
            for (uint32_t sample = 0; sample < samples; ++sample)
            {
                candidate->Decode(sample, decoded);
                for (unsigned index = 0; index < animation.mNumChannels; ++index)
                {
                    const auto& channel = *animation.mChannels[index];
                    const auto position = channel.mNumPositionKeys
                                              ? channel.mPositionKeys[sample % channel.mNumPositionKeys].mValue
                                              : aiVector3D{};
                    const auto rotation = channel.mNumRotationKeys
                                              ? channel.mRotationKeys[sample % channel.mNumRotationKeys].mValue
                                              : aiQuaternion(1, 0, 0, 0);
                    const double positionError = (decoded[index].Position - position).Length();
                    const double rotationError = RotationError(decoded[index].Rotation, rotation);
                    if (!Finite(decoded[index].Position) || !ValidRotation(decoded[index].Rotation) ||
                        !std::isfinite(positionError) || !std::isfinite(rotationError) || positionError > 0.0001 ||
                        rotationError > 0.05)
                    {
                        m_Error = "ACL local pose error exceeds acceptance limits";
                        return false;
                    }
                    candidate->Statistics.MaxPositionError =
                        (std::max)(candidate->Statistics.MaxPositionError, positionError);
                    candidate->Statistics.MaxRotationErrorDegrees =
                        (std::max)(candidate->Statistics.MaxRotationErrorDegrees, rotationError);
                }
            }
            m_Impl = std::move(candidate);
            m_Error.clear();
            return true;
        }
        catch (const std::bad_alloc&)
        {
            m_Error = "ACL clip allocation failed";
            return false;
        }
#else
        (void)animation;
        m_Error = "ACL is disabled in this build";
        return false;
#endif
    }

    bool FbxAclClip::Sample(float seconds, std::vector<LocalPose>& output)
    {
#if RENDERINGPROJECT_USE_ACL
        if (!m_Impl || !std::isfinite(seconds))
        {
            return false;
        }
        float ticks = seconds * m_Impl->TickRate;
        if (m_Impl->Duration > 0)
        {
            ticks = std::fmod(ticks, m_Impl->Duration);
        }
        if (!std::isfinite(ticks) || double(ticks) >= double(UINT32_MAX))
        {
            return false;
        }
        const auto sample = static_cast<uint32_t>((std::max)(0.0f, ticks)) % m_Impl->Statistics.Samples;
        try
        {
            m_Impl->Decode(sample, output);
            return true;
        }
        catch (const std::bad_alloc&)
        {
            return false;
        }
#else
        (void)seconds;
        (void)output;
        return false;
#endif
    }

    uint32_t FbxAclClip::FindChannel(const aiNodeAnim* channel) const
    {
        if (!m_Impl)
        {
            return MissingChannel;
        }
        const auto found = m_Impl->Channels.find(channel);
        return found == m_Impl->Channels.end() ? MissingChannel : found->second;
    }

    ClipStatistics FbxAclClip::GetStatistics() const
    {
        return m_Impl ? m_Impl->Statistics : ClipStatistics{};
    }
}
