#include "../source/models/fbxaclclip.h"
#include <assimp/cimport.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace
{
    using Clock = std::chrono::steady_clock;
    using Pose = std::vector<AnimationCompression::LocalPose>;
    volatile float checksum = 0;

    void RawSample(const aiAnimation& animation, float seconds, Pose& output)
    {
        const float rate = animation.mTicksPerSecond != 0 ? float(animation.mTicksPerSecond) : 25.0f;
        float ticks = seconds * rate;
        if (animation.mDuration > 0)
        {
            ticks = std::fmod(ticks, float(animation.mDuration));
        }
        const auto key = static_cast<unsigned>((std::max)(0.0f, ticks));
        output.assign(animation.mNumChannels, AnimationCompression::LocalPose{});
        for (unsigned index = 0; index < animation.mNumChannels; ++index)
        {
            const auto& channel = *animation.mChannels[index];
            if (channel.mNumPositionKeys)
            {
                output[index].Position = channel.mPositionKeys[key % channel.mNumPositionKeys].mValue;
            }
            if (channel.mNumRotationKeys)
            {
                output[index].Rotation = channel.mRotationKeys[key % channel.mNumRotationKeys].mValue;
            }
        }
    }

    double Milliseconds(Clock::time_point start)
    {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }
}

int main()
{
    try
    {
        unsigned tested = 0;
        std::cout
            << "{\"scope\":\"Optimized CPU all-channel stepped sampling; 5 alternating-order passes of 2000 samples. Payload bytes exclude names, source scene and retained fallback arrays. No renderer FPS or total-memory claim.\",\"clips\":[\n";
        bool first = true;
        for (const char* file : {"asset/model/XBot.fbx", "asset/model/idle.fbx", "asset/model/standup.fbx"})
        {
            const aiScene* scene = aiImportFile(file, aiProcess_ValidateDataStructure);
            if (!scene || scene->mNumAnimations == 0)
            {
                throw std::runtime_error("FBX benchmark import failed");
            }
            for (unsigned clipIndex = 0; clipIndex < scene->mNumAnimations; ++clipIndex)
            {
                if (!first)
                {
                    std::cout << ",\n";
                }
                first = false;
                const auto& animation = *scene->mAnimations[clipIndex];
                AnimationCompression::FbxAclClip clip;
                const auto buildStart = Clock::now();
                const bool built = clip.Build(animation);
                const double buildMs = Milliseconds(buildStart);
                std::cout << "{\"file\":" << std::quoted(file) << ",\"clip\":" << clipIndex;
                if (!built)
                {
                    std::cout << ",\"accepted\":false,\"reason\":" << std::quoted(clip.LastError()) << '}';
                    continue;
                }
                Pose reference, decoded;
                const float rate = animation.mTicksPerSecond != 0 ? float(animation.mTicksPerSecond) : 25;
                for (unsigned sample = 0; sample < 512; ++sample)
                {
                    const float seconds = float(sample) * .371f / rate;
                    RawSample(animation, seconds, reference);
                    if (!clip.Sample(seconds, decoded) || reference.size() != decoded.size())
                    {
                        throw std::runtime_error("ACL sample failed");
                    }
                    for (size_t index = 0; index < reference.size(); ++index)
                    {
                        if ((reference[index].Position - decoded[index].Position).Length() > .0001f)
                        {
                            throw std::runtime_error("ACL fractional-time pose mismatch");
                        }
                    }
                }
                double rawPass[5]{}, aclPass[5]{};
                for (unsigned pass = 0; pass < 5; ++pass)
                {
                    for (unsigned phase = 0; phase < 2; ++phase)
                    {
                        const bool useAcl = (pass + phase) % 2 != 0;
                        const auto start = Clock::now();
                        for (unsigned sample = 0; sample < 2000; ++sample)
                        {
                            const float seconds = float(sample) * .371f / rate;
                            if (useAcl)
                            {
                                if (!clip.Sample(seconds, decoded))
                                {
                                    throw std::runtime_error("timed ACL sample failed");
                                }
                            }
                            else
                            {
                                RawSample(animation, seconds, decoded);
                            }
                            checksum += decoded[0].Position.x + decoded[0].Rotation.w;
                        }
                        (useAcl ? aclPass[pass] : rawPass[pass]) = Milliseconds(start);
                    }
                }
                const auto statistics = clip.GetStatistics();
                std::cout << ",\"accepted\":true,\"channels\":" << animation.mNumChannels
                          << ",\"samples\":" << statistics.Samples << ",\"rawKeyBytes\":" << statistics.SourceKeyBytes
                          << ",\"aclBytes\":" << statistics.CompressedBytes << ",\"buildAndValidationMs\":" << buildMs
                          << ",\"maxPositionError\":" << statistics.MaxPositionError
                          << ",\"maxRotationErrorDegrees\":" << statistics.MaxRotationErrorDegrees << ",\"passes\":[";
                for (unsigned pass = 0; pass < 5; ++pass)
                {
                    if (pass)
                    {
                        std::cout << ',';
                    }
                    std::cout << "{\"rawMs\":" << rawPass[pass] << ",\"aclMs\":" << aclPass[pass] << '}';
                }
                std::cout << "]}";
                ++tested;
            }
            aiReleaseImport(scene);
        }
        std::cout << "\n],\"tested\":" << tested << ",\"checksum\":" << checksum << "}\n";
        if (tested == 0)
        {
            throw std::runtime_error("no supported FBX clips tested");
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
