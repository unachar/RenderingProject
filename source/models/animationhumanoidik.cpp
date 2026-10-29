#include "pch.h"
#include "animationmodel.h"
#include "vmdikbinding.h"

void AnimationModelResource::BuildHumanoidIkChannel(const VmdAnimation* animation, size_t layerIndex,
                                                     unsigned side, VmdHumanoidIkChannel& channel)
{
    if (m_IsPmxModel || !animation) return;
    VmdIkBinding::UpdateChannel(animation->BoneTracks, animation->IkTracks, layerIndex, side, channel);
}

void AnimationModelResource::SetupHumanoidIkRig()
{
    if (m_IsPmxModel) return;
    if (m_PmxRuntimeNodes.empty()) RebuildPmxRuntimeCache();
    m_HumanoidIkPose.Nodes.clear();
    m_HumanoidIkPose.Nodes.reserve(m_PmxRuntimeNodes.size());
    for (const auto& source : m_PmxRuntimeNodes)
    {
        HumanoidIk::Node node;
        node.Parent = source.ParentIndex;
        node.Local = source.BonePtr ? source.BonePtr->BindLocalMatrix : source.Node->mTransformation;
        m_HumanoidIkPose.Nodes.push_back(node);
    }
    m_HumanoidIkLegs = {};
    m_HumanoidIkRigDirty = false;
    if (!m_HumanoidIkPose.CaptureBindPose()) return;
    auto resolve = [&](const char* vmd, const char* human) -> int {
        const auto* indices = m_ResourceMap.Find(ModelResourceMap::Vmd, vmd);
        if (!indices) indices = m_ResourceMap.Find(ModelResourceMap::Humanoid, human);
        if (!indices) indices = m_ResourceMap.Find(ModelResourceMap::Bones, vmd);
        if (!indices || indices->size() != 1 || indices->front() >= m_BoneNames.size()) return -1;
        const auto found = m_PmxRuntimeNodeIndexMap.find(m_BoneNames[indices->front()]);
        return found == m_PmxRuntimeNodeIndexMap.end() ? -1 : static_cast<int>(found->second);
    };
    for (unsigned side = 0; side < 2; ++side)
    {
        auto& leg = m_HumanoidIkLegs[side];
        leg.Hip = resolve(side ? "右足" : "左足", side ? "rightUpperLeg" : "leftUpperLeg");
        leg.Knee = resolve(side ? "右ひざ" : "左ひざ", side ? "rightLowerLeg" : "leftLowerLeg");
        leg.Foot = resolve(side ? "右足首" : "左足首", side ? "rightFoot" : "leftFoot");
        leg.Toe = resolve(side ? "右つま先" : "左つま先", side ? "rightToes" : "leftToes");
        if (!HumanoidIk::ValidLeg(m_HumanoidIkPose, leg))
        {
            Debug::Log("VMD humanoid %s leg IK unavailable: missing chain or unsupported joint scale.\n", side ? "right" : "left");
            leg = {};
            continue;
        }
        if (!m_HumanoidIkPose.Contains(leg.Toe) || !m_HumanoidIkPose.DescendsFrom(leg.Toe, leg.Foot)) leg.Toe = -1;
        const auto hip = m_HumanoidIkPose.Position(leg.Hip);
        const auto knee = m_HumanoidIkPose.Position(leg.Knee);
        const auto foot = m_HumanoidIkPose.Position(leg.Foot);
        const auto axis = HumanoidIk::Unit(foot - hip, {0, -1, 0});
        auto bend = knee - hip - axis * ((knee - hip) * axis);
        if (bend.SquareLength() < (foot - hip).SquareLength() * 0.000001f && leg.Toe >= 0)
        {
            bend = m_HumanoidIkPose.Position(leg.Toe) - foot;
            bend -= axis * (bend * axis);
        }
        leg.Pole = HumanoidIk::Unit(bend, HumanoidIk::Perpendicular(axis));
    }
}

HumanoidIk::Target AnimationModelResource::SampleHumanoidIkTarget(VmdHumanoidIkChannel& channel,
    const HumanoidIk::Leg& leg, float frame, const vector<float>* layerFrames)
{
    auto sampleFrame = [&](size_t layer) {
        return layerFrames && layer < layerFrames->size() ? (*layerFrames)[layer] : frame;
    };
    auto sample = [&](VmdRootMotionBinding& binding, aiQuaternion& rotation, aiVector3D& position) {
        SampleVmdBoneTrackCached(binding.Track, sampleFrame(binding.LayerIndex), binding.Cursor, rotation, position);
    };
    auto enabled = [&](VmdLayeredIkBinding& binding) {
        return SampleVmdIkTrackCached(binding.Track, sampleFrame(binding.LayerIndex), binding.Cursor);
    };
    HumanoidIk::Offsets offsets;
    offsets.FootEnabled = channel.Foot.Track && enabled(channel.FootSwitch);
    offsets.ToeEnabled = channel.Toe.Track && leg.Toe >= 0 && enabled(channel.ToeSwitch);
    if (!offsets.FootEnabled && !offsets.ToeEnabled) return {};
    sample(channel.Foot, offsets.FootRotation, offsets.FootPosition);
    aiQuaternion unused;
    sample(channel.Toe, unused, offsets.ToePosition);
    sample(channel.Root, offsets.RootRotation, offsets.RootPosition);
    sample(channel.Parent, offsets.ParentRotation, offsets.ParentPosition);
    const auto& foot = m_HumanoidIkPose.Nodes[static_cast<size_t>(leg.Foot)].BindGlobal;
    const auto toe = leg.Toe >= 0 ? HumanoidIk::Translation(m_HumanoidIkPose.Nodes[static_cast<size_t>(leg.Toe)].BindGlobal) :
                                 HumanoidIk::Translation(foot);
    return HumanoidIk::FromOffsets(HumanoidIk::Translation(foot), HumanoidIk::Rotation(foot), toe,
                                   offsets, m_VmdTranslationScale);
}

void AnimationModelResource::ApplyVmdHumanoidIk(float frame1, float frame2, float blendRate, bool layered)
{
    if (m_IsPmxModel || !m_AiScene) return;
    const auto& first = layered ? m_HumanoidIkLayered : m_HumanoidIkPrimary;
    bool hasTargets = false;
    for (unsigned side = 0; side < 2; ++side)
        hasTargets |= first[side].Foot.Track || first[side].Toe.Track ||
                      (!layered && (m_HumanoidIkSecondary[side].Foot.Track || m_HumanoidIkSecondary[side].Toe.Track));
    if (!hasTargets) return;
    if (m_HumanoidIkRigDirty) SetupHumanoidIkRig();
    if (m_HumanoidIkPose.Nodes.size() != m_PmxRuntimeNodes.size()) return;
    for (size_t i = 0; i < m_PmxRuntimeNodes.size(); ++i)
    {
        const auto& source = m_PmxRuntimeNodes[i];
        m_HumanoidIkPose.Nodes[i].Local = source.BonePtr ? source.BonePtr->AnimationMatrix : source.Node->mTransformation;
    }
    if (!m_HumanoidIkPose.UpdateGlobals()) return;
    for (unsigned side = 0; side < 2; ++side)
    {
        auto leg = m_HumanoidIkLegs[side];
        if (!HumanoidIk::ValidLeg(m_HumanoidIkPose, leg)) continue;
        HumanoidIk::Target fk;
        fk.FootPosition = m_HumanoidIkPose.Position(leg.Foot);
        fk.FootRotation = HumanoidIk::Rotation(m_HumanoidIkPose.Nodes[static_cast<size_t>(leg.Foot)].Global);
        fk.ToePosition = leg.Toe >= 0 ? m_HumanoidIkPose.Position(leg.Toe) : fk.FootPosition;
        const auto axis = HumanoidIk::Unit(fk.FootPosition - m_HumanoidIkPose.Position(leg.Hip), {0, -1, 0});
        const auto toKnee = m_HumanoidIkPose.Position(leg.Knee) - m_HumanoidIkPose.Position(leg.Hip);
        const auto bend = toKnee - axis * (toKnee * axis);
        if (bend.SquareLength() > toKnee.SquareLength() * 0.000001f) leg.Pole = HumanoidIk::Unit(bend);
        else
        {
            auto inverseBind = HumanoidIk::Rotation(m_HumanoidIkPose.Nodes[static_cast<size_t>(leg.Hip)].BindGlobal);
            inverseBind.Conjugate();
            leg.Pole = (HumanoidIk::Rotation(m_HumanoidIkPose.Nodes[static_cast<size_t>(leg.Hip)].Global) * inverseBind).Rotate(leg.Pole);
        }
        HumanoidIk::Target target;
        if (layered) target = SampleHumanoidIkTarget(m_HumanoidIkLayered[side], leg, 0, &m_VmdLayerFramesScratch);
        else
        {
            const auto a = SampleHumanoidIkTarget(m_HumanoidIkPrimary[side], leg, frame1, nullptr);
            const auto b = SampleHumanoidIkTarget(m_HumanoidIkSecondary[side], leg, frame2, nullptr);
            target = HumanoidIk::Blend(a, b, blendRate, fk);
        }
        if (!HumanoidIk::Apply(m_HumanoidIkPose, leg, target)) continue;
        for (int index : {leg.Hip, leg.Knee, leg.Foot})
        {
            auto* bone = m_PmxRuntimeNodes[static_cast<size_t>(index)].BonePtr;
            if (bone) bone->AnimationMatrix = m_HumanoidIkPose.Nodes[static_cast<size_t>(index)].Local;
        }
    }
}
