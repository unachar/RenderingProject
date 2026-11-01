#define NOMINMAX
#include <windows.h>
#include "../source/models/fbxaclclip.h"
#include <assimp/cimport.h>
#include <assimp/scene.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <unordered_map>
using namespace std;

struct VmdAnimation
{
};

struct Bone
{
    aiMatrix4x4 AnimationMatrix;
};

struct Debug
{
    static void Log(const char*, ...) {}
};

aiMatrix4x4 MakeAiIdentityMatrix()
{
    return {};
}

class ResourceFixture
{
  public:
    const aiScene* m_AiScene = nullptr;
    aiAnimation* Animation = nullptr;
    bool m_HasCachedLayeredPose = false, m_UseLayeredVmdIk = false, m_HasCachedPose = false;
    string m_LastPoseAnimation1, m_LastPoseAnimation2;
    float m_LastPoseFrame1 = 0, m_LastPoseFrame2 = 0, m_LastPoseBlendRate = 0;
    unordered_map<string, VmdAnimation> m_VmdAnimations;
    unordered_map<string, Bone> m_Bone;
    unordered_map<const aiAnimation*, AnimationCompression::FbxAclClip> m_AclClips;
    vector<AnimationCompression::LocalPose> m_AclPoseScratch1, m_AclPoseScratch2;
    unsigned VmdCalls = 0;

    aiAnimation* GetAnimation(const string&)
    {
        return Animation;
    }

    aiNodeAnim* FindNodeAnimChannelCached(aiAnimation* animation, const string& name)
    {
        if (animation && name == "bone")
        {
            return animation->mChannels[0];
        }
        return nullptr;
    }

    void UpdateVmdBoneMatrices(const VmdAnimation*, float, const VmdAnimation*, float, float)
    {
        ++VmdCalls;
    }

    void UpdateBoneMatrix(aiNode*, aiMatrix4x4) {}

    void WriteBoneMatricesToBuffer() {}

    bool TrySampleAclClip(aiAnimation*, float, vector<AnimationCompression::LocalPose>&);
    void UpdateBoneMatrices(const char*, float, const char*, float, float);
};

// The runner extracts these two complete methods from the production source.
#include "acl-fbx-playback.inc"

int main(int argc, char** argv)
{
    const bool useAcl = argc > 1 && string(argv[1]) == "acl";
    SetEnvironmentVariableA("DX12_USE_ACL_ANIMATION", useAcl ? "1" : "0");
    aiAnimation animation;
    animation.mDuration = 4;
    animation.mTicksPerSecond = 30;
    animation.mNumChannels = 1;
    animation.mChannels = new aiNodeAnim*[1]{new aiNodeAnim};
    auto& channel = *animation.mChannels[0];
    channel.mNodeName.Set("bone");
    channel.mNumPositionKeys = channel.mNumRotationKeys = 4;
    channel.mPositionKeys = new aiVectorKey[4];
    channel.mRotationKeys = new aiQuatKey[4];
    for (unsigned index = 0; index < 4; ++index)
    {
        channel.mPositionKeys[index] = aiVectorKey(index, aiVector3D(float(index), .25f, -.5f));
        channel.mRotationKeys[index] = aiQuatKey(index, aiQuaternion(aiVector3D(0, 1, 0), .1f * index));
    }
    ResourceFixture resource;
    // The production method only passes the root to the GPU/scene integration stand-in.
    const aiScene* scene = aiImportFile("asset/model/XBot.fbx", 0);
    if (!scene)
    {
        throw runtime_error("fixture scene import failed");
    }
    resource.m_AiScene = scene;
    resource.Animation = &animation;
    resource.m_Bone.emplace("bone", Bone{});
    resource.m_Bone.emplace("missing", Bone{});
    for (const auto& times : {pair<float, float>{0.0f, 0.0f}, {0.0f, .1f}, {.1f, 0.0f}, {.14f, .077f}, {-.1f, .031f}})
    {
        resource.UpdateBoneMatrices("same", times.first, "same", times.second, .25f);
        auto sample = [](float seconds) {
            return unsigned((max)(0.0f, fmod(seconds * 30, 4.0f))) % 4;
        };
        const unsigned first = sample(times.first), second = sample(times.second);
        aiQuaternion rotation;
        aiQuaternion::Interpolate(rotation, channel.mRotationKeys[first].mValue, channel.mRotationKeys[second].mValue,
                                  .25f);
        const auto position = channel.mPositionKeys[first].mValue * .75f + channel.mPositionKeys[second].mValue * .25f;
        const aiMatrix4x4 expected(aiVector3D(1, 1, 1), rotation, position);
        const auto& actual = resource.m_Bone.at("bone").AnimationMatrix;
        if (abs(actual.a1 - expected.a1) > .0001f || abs(actual.a3 - expected.a3) > .0001f ||
            abs(actual.a4 - expected.a4) > .0001f || abs(actual.b4 - expected.b4) > .0001f)
        {
            throw runtime_error("production two-time FBX blend differs");
        }
        if (resource.m_Bone.at("missing").AnimationMatrix.a1 != 1)
        {
            throw runtime_error("missing-channel identity changed");
        }
    }
    if ((resource.m_AclClips.size() == 1) != useAcl)
    {
        throw runtime_error("production codec selection not exercised");
    }
    if (useAcl && resource.m_AclClips.at(&animation).GetStatistics().CompressedBytes == 0)
    {
        throw runtime_error("production test did not decode an actual ACL payload");
    }
    resource.m_VmdAnimations.emplace("vmd", VmdAnimation{});
    resource.UpdateBoneMatrices("vmd", .17f, "same", .2f, .5f);
    if (resource.VmdCalls != 1)
    {
        throw runtime_error("VMD must keep its dedicated playback");
    }
    cout << "PASS production FBX selection/blending and VMD routing, ACL=" << useAcl << '\n';
    aiReleaseImport(scene);
}
