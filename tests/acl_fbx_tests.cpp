#include "../source/models/fbxaclclip.h"
#include <assimp/scene.h>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    void Populate(aiAnimation& animation)
    {
        animation.mDuration = 11.0;
        animation.mTicksPerSecond = 30.0;
        animation.mNumChannels = 3;
        animation.mChannels = new aiNodeAnim*[3]{};
        for (unsigned channel = 0; channel < 3; ++channel)
        {
            auto* node = new aiNodeAnim;
            animation.mChannels[channel] = node;
            node->mNodeName.Set("bone" + std::to_string(channel));
            node->mNumPositionKeys = channel == 0 ? 12 : 3;
            node->mNumRotationKeys = channel == 0 ? 12 : 4;
            node->mPositionKeys = new aiVectorKey[node->mNumPositionKeys];
            node->mRotationKeys = new aiQuatKey[node->mNumRotationKeys];
            for (unsigned key = 0; key < node->mNumPositionKeys; ++key)
            {
                // Nonunit timestamps intentionally exercise the existing index-based playback contract.
                node->mPositionKeys[key] = aiVectorKey(key * 2.0, aiVector3D(float(key) * .2f, float(channel), .5f));
            }
            for (unsigned key = 0; key < node->mNumRotationKeys; ++key)
            {
                node->mRotationKeys[key] = aiQuatKey(key * 2.0, aiQuaternion(aiVector3D(0, 1, 0), .01f * key));
            }
        }
    }

    void Compare(const aiAnimation& animation, AnimationCompression::FbxAclClip& clip, float seconds)
    {
        std::vector<AnimationCompression::LocalPose> pose;
        Require(clip.Sample(seconds, pose), "sample accepted time");
        Require(pose.size() == animation.mNumChannels, "all channels decoded");
        const float wrapped = std::fmod(seconds * float(animation.mTicksPerSecond), float(animation.mDuration));
        const unsigned tick = static_cast<unsigned>((std::max)(0.0f, wrapped));
        for (unsigned channel = 0; channel < animation.mNumChannels; ++channel)
        {
            const auto& node = *animation.mChannels[channel];
            Require(clip.FindChannel(&node) == channel, "channel mapping");
            const auto position = node.mPositionKeys[tick % node.mNumPositionKeys].mValue;
            const auto rotation = node.mRotationKeys[tick % node.mNumRotationKeys].mValue;
            Require((pose[channel].Position - position).Length() < 0.0001f, "translation matches stepped source");
            const double dot =
                std::abs(double(pose[channel].Rotation.w) * rotation.w + double(pose[channel].Rotation.x) * rotation.x +
                         double(pose[channel].Rotation.y) * rotation.y + double(pose[channel].Rotation.z) * rotation.z);
            Require(dot > .999999, "rotation matches nonidentity source");
        }
    }
}

int main(int argc, char** argv)
{
    try
    {
#if !RENDERINGPROJECT_USE_ACL
        AnimationCompression::FbxAclClip disabled;
        aiAnimation source;
        std::vector<AnimationCompression::LocalPose> output;
        Require(!disabled.Build(source) && !disabled.Sample(0, output), "disabled SDK keeps legacy fallback");
        std::cout << "PASS ACL-disabled build without SDK headers\n";
        return 0;
#else
        aiAnimation animation;
        Populate(animation);
        if (argc == 1 || std::string(argv[1]) != "allocator")
        {
            auto* saved = animation.mChannels[1];
            animation.mChannels[1] = animation.mChannels[0];
            AnimationCompression::FbxAclClip duplicate;
            const bool accepted = duplicate.Build(animation);
            animation.mChannels[1] = saved;
            Require(!accepted && duplicate.LastError() == "Duplicate FBX channel pointer",
                    "reject duplicate before decoding");
        }
        const int beforeAllocations = AclAllocationProbe::LiveBlocks;
        unsigned injected = 0;
        for (int failure : {0, 1, 8, 32})
        {
            {
                AnimationCompression::FbxAclClip failureClip;
                AclAllocationProbe::FailAfter = failure;
                AclAllocationProbe::Consumed = false;
                const bool built = failureClip.Build(animation);
                AclAllocationProbe::FailAfter = -1;
                if (AclAllocationProbe::Consumed)
                {
                    ++injected;
                    Require(!built, "native allocation failure must return legacy fallback");
                }
            }
            Require(AclAllocationProbe::LiveBlocks == beforeAllocations, "no leaked SDK allocations on failure");
        }
        Require(injected >= 3, "nested SDK allocation failures exercised");
        AnimationCompression::FbxAclClip clip;
        Require(clip.Build(animation), clip.LastError().c_str());
        Require(clip.GetStatistics().CompressedBytes > 0, "actual ACL payload");
        Require(clip.GetStatistics().Samples == 12, "repeating key periods retained");
        for (float time : {-2.0f, 0.0f, .017f, .1f, .25f, .3666667f, .4f, 10.0f})
        {
            Compare(animation, clip, time);
        }
        std::vector<AnimationCompression::LocalPose> first, second;
        Require(clip.Sample(0, first) && clip.Sample(.1f, second), "two blend times");
        Require((first[0].Position - second[0].Position).Length() > .1f, "independent blend outputs");
        Require(!clip.Sample(std::numeric_limits<float>::quiet_NaN(), second), "reject nonfinite time");
        aiNodeAnim missing;
        Require(clip.FindChannel(&missing) == AnimationCompression::FbxAclClip::MissingChannel, "missing channel");
        auto moved = std::move(clip);
        Compare(animation, moved, .2f);
        Require(!clip.Sample(0, second), "moved-from codec is empty");
        aiAnimation defaults;
        defaults.mDuration = 0;
        defaults.mNumChannels = 1;
        defaults.mChannels = new aiNodeAnim*[1]{new aiNodeAnim};
        AnimationCompression::FbxAclClip identity;
        Require(identity.Build(defaults), "empty subtracks retain identity");
        second.assign(5, AnimationCompression::LocalPose{aiQuaternion(0, 1, 0, 0), aiVector3D(3, 4, 5)});
        Require(identity.Sample(1.0f, second) && second.size() == 1 && second[0].Position.Length() == 0 &&
                    second[0].Rotation.w == 1,
                "default tracks clear prior pose values");
        defaults.mDuration = std::numeric_limits<double>::infinity();
        Require(!identity.Build(defaults), "reject nonfinite duration");
        animation.mChannels[1]->mPositionKeys[0].mValue.x = std::numeric_limits<float>::infinity();
        Require(!clip.Build(animation), "reject nonfinite transforms");
        animation.mChannels[1]->mPositionKeys[0].mValue.x = 0;
        animation.mChannels[1]->mNumPositionKeys = 2;
        animation.mChannels[0]->mNumPositionKeys = 11;
        animation.mChannels[0]->mNumRotationKeys = 11;
        Require(!clip.Build(animation), "unsupported repeating periods fall back");
        std::cout << "PASS ACL FBX poses, looping, ownership and invalid input\n";
        return 0;
#endif
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
