#include "pch.h"
#include "animationmodel.h"
#include "animationmath.h"

using namespace AnimationMath;

void AnimationModelResource::InvalidateVmdRuntimeCache()
{

    m_HumanoidIkPrimary = {};
    m_HumanoidIkSecondary = {};
    m_HumanoidIkLayered = {};
    m_HumanoidIkRigDirty = true;
    m_LookAtRigReady = false;
    m_HasCachedPose = false;
    m_HasCachedLayeredPose = false;
    m_UseLayeredVmdIk = false;
    m_CachedVmdPrimaryAnimation = nullptr;
    m_CachedVmdSecondaryAnimation = nullptr;
    m_VmdBoneBindings.clear();
    m_VmdMorphBindings.clear();
    m_CachedVmdLayerAnimations.clear();
    m_VmdLayerFramesScratch.clear();
    m_LastLayerPoseTimes.clear();
    m_VmdLayeredBoneBindings.clear();
    m_VmdLayeredMorphBindings.clear();
    m_VmdLayeredIkBindings.clear();
    m_VmdIkTrackCache.clear();
    m_VmdIkTrackCursors.clear();
    m_VmdActiveMorphsScratch.clear();
    m_VmdMorphPositionOffsetsScratch.clear();
    m_VmdMorphUvOffsetsScratch.clear();
}

void AnimationModelResource::RebuildVmdRuntimeCache(const VmdAnimation* primaryAnimation,
                                                    const VmdAnimation* secondaryAnimation)
{
    m_UseLayeredVmdIk = false;
    m_CachedVmdPrimaryAnimation = primaryAnimation;
    m_CachedVmdSecondaryAnimation = secondaryAnimation;
    m_VmdBoneBindings.clear();
    m_VmdMorphBindings.clear();
    m_VmdIkTrackCache.clear();
    m_VmdIkTrackCursors.clear();
    m_VmdActiveMorphsScratch.clear();

    if (!primaryAnimation)
    {
        return;
    }

    const VmdAnimation* secondary;
    if (secondaryAnimation)
    {
        secondary = secondaryAnimation;
    }
    else
    {
        secondary = primaryAnimation;
    }
    m_HumanoidIkPrimary = {};
    m_HumanoidIkSecondary = {};
    for (unsigned side = 0; side < 2; ++side)
    {
        BuildHumanoidIkChannel(primaryAnimation, 0, side, m_HumanoidIkPrimary[side]);
        BuildHumanoidIkChannel(secondary, 0, side, m_HumanoidIkSecondary[side]);
    }
    m_VmdBoneBindings.reserve(m_Bone.size());
    for (auto& pair : m_Bone)
    {
        VmdBoneBinding binding{};
        binding.BonePtr = &pair.second;
        binding.BonePtr->BindLocalMatrix.Decompose(binding.BaseScale, binding.BaseRotation, binding.BasePosition);

        const uint32_t boneIndex = m_BoneIndexMap.at(pair.first);
        binding.PrimaryTrack = VmdRetarget::FindTrack(primaryAnimation->BoneTracks, pair.first, boneIndex, m_ResourceMap);
        binding.SecondaryTrack = VmdRetarget::FindTrack(secondary->BoneTracks, pair.first, boneIndex, m_ResourceMap);
        const auto transform = m_VmdRetargetTransforms.find(pair.first);
        if (transform != m_VmdRetargetTransforms.end()) binding.Retarget = transform->second;
        BuildVmdTorsoCompensation(boneIndex, primaryAnimation, 0, binding.PrimaryTorsoCompensation);
        BuildVmdTorsoCompensation(boneIndex, secondary, 0, binding.SecondaryTorsoCompensation);
        BuildVmdRootMotionBindings(boneIndex, primaryAnimation, 0, binding.PrimaryRootMotion);
        BuildVmdRootMotionBindings(boneIndex, secondary, 0, binding.SecondaryRootMotion);

        m_VmdBoneBindings.push_back(binding);
    }

    m_VmdMorphBindings.reserve(primaryAnimation->MorphTracks.size());
    for (const auto& track : primaryAnimation->MorphTracks)
    {
        auto morphIt = m_PmxMorphIndexMap.find(track.first);
        if (morphIt == m_PmxMorphIndexMap.end())
        {
            continue;
        }
        m_VmdMorphBindings.push_back({morphIt->second, &track.second});
    }

    m_VmdIkTrackCache.reserve(primaryAnimation->IkTracks.size());
    m_VmdIkTrackCursors.reserve(primaryAnimation->IkTracks.size());
    for (const auto& track : primaryAnimation->IkTracks)
    {
        m_VmdIkTrackCache.emplace(track.first, &track.second);
        m_VmdIkTrackCursors.emplace(track.first, VmdTrackSampleCursor{});
    }

    if (m_VmdMorphPositionOffsetsScratch.size() != m_PmxBaseVertices.size())
    {
        m_VmdMorphPositionOffsetsScratch.resize(m_PmxBaseVertices.size());
        m_VmdMorphUvOffsetsScratch.resize(m_PmxBaseVertices.size());
    }
}

bool AnimationModelResource::IsVmdLayeredRuntimeCacheValid(const vector<AnimationPlaybackLayer>& animationLayers) const
{
    if (m_CachedVmdLayerAnimations.size() != animationLayers.size())
    {
        return false;
    }

    for (size_t layerIndex = 0; layerIndex < animationLayers.size(); ++layerIndex)
    {
        const auto animationIt = m_VmdAnimations.find(animationLayers[layerIndex].AnimationName);
        const VmdAnimation* animation;
        if (animationIt != m_VmdAnimations.end())
        {
            animation = &animationIt->second;
        }
        else
        {
            animation = nullptr;
        }
        if (m_CachedVmdLayerAnimations[layerIndex] != animation)
        {
            return false;
        }
    }
    return true;
}

void AnimationModelResource::RebuildVmdLayeredRuntimeCache(const vector<AnimationPlaybackLayer>& animationLayers)
{
    m_HasCachedLayeredPose = false;
    m_UseLayeredVmdIk = true;
    m_CachedVmdLayerAnimations.clear();
    m_CachedVmdLayerAnimations.reserve(animationLayers.size());
    for (const AnimationPlaybackLayer& layer : animationLayers)
    {
        const auto animationIt = m_VmdAnimations.find(layer.AnimationName);
        const VmdAnimation* layerAnimation;
        if (animationIt != m_VmdAnimations.end())
        {
            layerAnimation = &animationIt->second;
        }
        else
        {
            layerAnimation = nullptr;
        }
        m_CachedVmdLayerAnimations.push_back(layerAnimation);
    }

    m_HumanoidIkLayered = {};
    for (size_t layer = 0; layer < m_CachedVmdLayerAnimations.size(); ++layer)
        for (unsigned side = 0; side < 2; ++side)
            BuildHumanoidIkChannel(m_CachedVmdLayerAnimations[layer], layer, side, m_HumanoidIkLayered[side]);
    m_VmdLayerFramesScratch.resize(animationLayers.size());
    m_LastLayerPoseTimes.resize(animationLayers.size());
    m_VmdLayeredBoneBindings.clear();
    m_VmdLayeredMorphBindings.clear();
    m_VmdLayeredIkBindings.clear();
    m_VmdActiveMorphsScratch.clear();

    m_VmdLayeredBoneBindings.reserve(m_Bone.size());
    for (auto& bonePair : m_Bone)
    {
        VmdLayeredBoneBinding binding{};
        binding.BonePtr = &bonePair.second;
        binding.BonePtr->BindLocalMatrix.Decompose(binding.BaseScale, binding.BaseRotation, binding.BasePosition);
        const uint32_t boneIndex = m_BoneIndexMap.at(bonePair.first);
        const auto transform = m_VmdRetargetTransforms.find(bonePair.first);
        if (transform != m_VmdRetargetTransforms.end()) binding.Retarget = transform->second;

        for (size_t layerIndex = 0; layerIndex < m_CachedVmdLayerAnimations.size(); ++layerIndex)
        {
            const VmdAnimation* animation = m_CachedVmdLayerAnimations[layerIndex];
            if (!animation)
            {
                continue;
            }
            BuildVmdTorsoCompensation(boneIndex, animation, layerIndex, binding.TorsoCompensation);
            BuildVmdRootMotionBindings(boneIndex, animation, layerIndex, binding.RootMotion);
            const auto* track = VmdRetarget::FindTrack(animation->BoneTracks, bonePair.first, boneIndex, m_ResourceMap);
            if (track)
            {
                binding.Track = track;
                binding.LayerIndex = layerIndex;
            }
        }
        m_VmdLayeredBoneBindings.push_back(binding);
    }

    size_t morphTrackCount = 0;
    size_t ikTrackCount = 0;
    for (const VmdAnimation* animation : m_CachedVmdLayerAnimations)
    {
        if (animation)
        {
            morphTrackCount += animation->MorphTracks.size();
            ikTrackCount += animation->IkTracks.size();
        }
    }
    m_VmdLayeredMorphBindings.reserve(min(morphTrackCount, m_PmxMorphs.size()));
    m_VmdLayeredIkBindings.reserve(ikTrackCount);
    unordered_map<uint32_t, size_t> morphBindingIndices;
    morphBindingIndices.reserve(min(morphTrackCount, m_PmxMorphs.size()));

    for (size_t layerIndex = 0; layerIndex < m_CachedVmdLayerAnimations.size(); ++layerIndex)
    {
        const VmdAnimation* animation = m_CachedVmdLayerAnimations[layerIndex];
        if (!animation)
        {
            continue;
        }

        for (const auto& trackPair : animation->MorphTracks)
        {
            const auto morphIt = m_PmxMorphIndexMap.find(trackPair.first);
            if (morphIt == m_PmxMorphIndexMap.end())
            {
                continue;
            }

            const uint32_t morphIndex = morphIt->second;
            const auto bindingIt = morphBindingIndices.find(morphIndex);
            if (bindingIt == morphBindingIndices.end())
            {
                morphBindingIndices.emplace(morphIndex, m_VmdLayeredMorphBindings.size());
                m_VmdLayeredMorphBindings.push_back({morphIndex, &trackPair.second, layerIndex, {}});
            }
            else
            {
                VmdLayeredMorphBinding& binding = m_VmdLayeredMorphBindings[bindingIt->second];
                binding.Track = &trackPair.second;
                binding.LayerIndex = layerIndex;
                binding.Cursor = {};
            }
        }

        for (const auto& trackPair : animation->IkTracks)
        {
            VmdLayeredIkBinding& binding = m_VmdLayeredIkBindings[trackPair.first];
            binding.Track = &trackPair.second;
            binding.LayerIndex = layerIndex;
            binding.Cursor = {};
        }
    }

    m_VmdActiveMorphsScratch.reserve(m_VmdLayeredMorphBindings.size());
    if (m_VmdMorphPositionOffsetsScratch.size() != m_PmxBaseVertices.size())
    {
        m_VmdMorphPositionOffsetsScratch.resize(m_PmxBaseVertices.size());
        m_VmdMorphUvOffsetsScratch.resize(m_PmxBaseVertices.size());
    }
}

void AnimationModelResource::InvalidatePmxRuntimeCache()
{
    m_HumanoidIkPose.Nodes.clear();
    m_HumanoidIkLegs = {};
    m_HumanoidIkRigDirty = true;
    m_PmxRuntimeNodes.clear();
    m_PmxRuntimeNodeIndexMap.clear();
    m_PmxGlobalMatricesScratch.clear();
    m_PmxOrderedTransformSteps.clear();
    m_PmxAppendResultsScratch.clear();
}

void AnimationModelResource::RebuildPmxRuntimeCache()
{
    InvalidatePmxRuntimeCache();
    if (!m_AiScene)
    {
        return;
    }

    auto buildFast = [&](auto&& self, aiNode* node, int parent) -> void {
        if (!node)
        {
            return;
        }

        const int myIndex = static_cast<int>(m_PmxRuntimeNodes.size());
        const string nodeName = node->mName.C_Str();
        Bone* bonePtr = nullptr;
        auto boneIt = m_Bone.find(nodeName);
        if (boneIt != m_Bone.end())
        {
            bonePtr = &boneIt->second;
        }

        m_PmxRuntimeNodeIndexMap[nodeName] = static_cast<size_t>(myIndex);
        m_PmxRuntimeNodes.push_back({node, nodeName, parent, bonePtr});

        for (unsigned int i = 0; i < node->mNumChildren; ++i)
        {
            self(self, node->mChildren[i], myIndex);
        }
    };
    buildFast(buildFast, m_AiScene->mRootNode, -1);
    m_PmxGlobalMatricesScratch.resize(m_PmxRuntimeNodes.size());

    m_PmxOrderedTransformSteps.reserve(m_PmxAppendConstraints.size() + m_PmxIkConstraints.size());
    for (size_t i = 0; i < m_PmxAppendConstraints.size(); ++i)
    {
        const auto& c = m_PmxAppendConstraints[i];
        m_PmxOrderedTransformSteps.push_back({false, i, c.DeformDepth, c.BoneOrder});
    }
    for (size_t i = 0; i < m_PmxIkConstraints.size(); ++i)
    {
        const auto& c = m_PmxIkConstraints[i];
        m_PmxOrderedTransformSteps.push_back({true, i, c.DeformDepth, c.BoneOrder});
    }
    sort(m_PmxOrderedTransformSteps.begin(), m_PmxOrderedTransformSteps.end(),
         [](const PmxOrderedTransformStep& lhs, const PmxOrderedTransformStep& rhs) {
             if (lhs.DeformDepth != rhs.DeformDepth)
             {
                 return lhs.DeformDepth < rhs.DeformDepth;
             }
             if (lhs.BoneOrder != rhs.BoneOrder)
             {
                 return lhs.BoneOrder < rhs.BoneOrder;
             }
             return !lhs.IsIk && rhs.IsIk;
         });

    m_PmxAppendResultsScratch.reserve(m_PmxAppendConstraints.size());
}

void AnimationModelResource::InvalidateAnimationPoseCache()
{
    m_HasCachedPose = false;
    m_HasCachedLayeredPose = false;
    m_UseLayeredVmdIk = false;
}

void AnimationModelResource::ConfigureVmdRetargeting(const char* fileName)
{
    m_VmdRetargetTransforms.clear();
    if (m_IsPmxModel || !m_AiScene || !m_AiScene->mRootNode) return;
    VmdRetarget::AddVrmHumanoidBones(std::filesystem::u8path(fileName), m_ResourceMap);
    VmdRetarget::AddCommonHumanoidBones(m_ResourceMap);
    VmdRetarget::AddVmdAliases(m_ResourceMap);

    unordered_map<uint32_t, aiMatrix4x4> globals;
    auto visit = [&](auto&& self, const aiNode* node, const aiMatrix4x4& parent) -> void {
        const aiMatrix4x4 global = parent * node->mTransformation;
        const string name = node->mName.C_Str();
        const auto index = m_BoneIndexMap.find(name);
        if (index != m_BoneIndexMap.end())
        {
            globals[index->second] = global;
            VmdRetarget::Transform transform;
            aiVector3D scale, position;
            global.Decompose(scale, transform.GlobalBindRotation, position);
            transform.GlobalBindRotation.Normalize();
            transform.ParentBindLinear = aiMatrix3x3(parent);
            transform.ParentInverse = transform.ParentBindLinear;
            if (fabsf(transform.ParentInverse.Determinant()) > 0.00000001f)
            {
                transform.ParentInverse.Inverse();
                transform.Enabled = true;
            }
            m_VmdRetargetTransforms.emplace(name, transform);
        }
        for (unsigned i = 0; i < node->mNumChildren; ++i) self(self, node->mChildren[i], global);
    };
    visit(visit, m_AiScene->mRootNode, aiMatrix4x4());
    // Typical MMD head-to-hips distance is about 7.5 model units.
    // An explicit scale is available for differently proportioned source motions.
    float motionScale = 1.0f;
    const auto* hips = m_ResourceMap.Find(ModelResourceMap::Humanoid, "hips");
    const auto* head = m_ResourceMap.Find(ModelResourceMap::Humanoid, "head");
    if (hips && head && hips->size() == 1 && head->size() == 1 &&
        globals.count(hips->front()) && globals.count(head->front()))
    {
        const auto& h = globals.at(hips->front());
        const auto& t = globals.at(head->front());
        const float distance = aiVector3D(t.a4 - h.a4, t.b4 - h.b4, t.c4 - h.c4).Length();
        if (std::isfinite(distance) && distance > 0.0001f) motionScale = distance / 7.5f;
    }
    SetVmdTranslationScale(motionScale);
}

bool AnimationModelResource::SetVmdBoneMapping(const string& sourceBoneName, const string& targetBoneName)
{
    const auto* target = m_ResourceMap.Find(ModelResourceMap::Bones, targetBoneName);
    if (sourceBoneName.empty() || !target || target->size() != 1 || m_IsPmxModel) return false;
    if (const auto* names = m_ResourceMap.NamesForIndex(ModelResourceMap::Vmd, target->front()))
    {
        for (const auto* name : *names)
        {
            const auto* indices = m_ResourceMap.Find(ModelResourceMap::Vmd, *name);
            if (*name != sourceBoneName && indices && indices->size() == 1) return false;
        }
    }
    m_ResourceMap.Set(ModelResourceMap::Vmd, sourceBoneName, target->front());
    InvalidateVmdRuntimeCache();
    return true;
}

bool AnimationModelResource::SetVmdTranslationScale(float modelUnitsPerVmdUnit)
{
    if (!std::isfinite(modelUnitsPerVmdUnit) || modelUnitsPerVmdUnit <= 0.0f || m_IsPmxModel) return false;
    m_VmdTranslationScale = modelUnitsPerVmdUnit;
    for (auto& transform : m_VmdRetargetTransforms) transform.second.TranslationScale = modelUnitsPerVmdUnit;
    InvalidateVmdRuntimeCache();
    return true;
}

void AnimationModelResource::BuildVmdRootMotionBindings(uint32_t boneIndex, const VmdAnimation* animation,
                                                        size_t layerIndex, vector<VmdRootMotionBinding>& bindings)
{
    if (m_IsPmxModel || !animation) return;
    const auto* hips = VmdRetarget::RootMotionTarget(m_ResourceMap);
    if (!hips || hips->size() != 1 || hips->front() != boneIndex) return;
    for (const char* name : {"全ての親", "センター", "グルーブ", "腰"})
    {
        // A native MMD hierarchy already propagates these ancestor tracks.
        if (m_Bone.count(name) || m_ResourceMap.Find(ModelResourceMap::Vmd, name)) continue;
        const auto track = animation->BoneTracks.find(name);
        if (track == animation->BoneTracks.end()) continue;
        const auto existing = std::find_if(bindings.begin(), bindings.end(), [&](const auto& binding) {
            return binding.Name == name;
        });
        VmdRootMotionBinding binding{name, &track->second, layerIndex, {}};
        if (existing == bindings.end()) bindings.push_back(binding);
        else *existing = binding; // Last layer wins independently for each root-motion source.
    }
}

void AnimationModelResource::BuildVmdTorsoCompensation(uint32_t boneIndex, const VmdAnimation* animation,
                                                       size_t layerIndex, VmdRootMotionBinding& binding)
{
    if (m_IsPmxModel || !animation || m_Bone.count("上半身")) return;
    const auto* torso = m_ResourceMap.Find(ModelResourceMap::Vmd, "上半身");
    if (!torso || torso->size() != 1 || torso->front() != boneIndex) return;
    const auto lower = animation->BoneTracks.find("下半身");
    if (lower != animation->BoneTracks.end()) binding = {"下半身", &lower->second, layerIndex, {}};
}
