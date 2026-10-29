#include "pch.h"
#include "animationmodel.h"

bool AnimationModelResource::LoadPoseInstance(const AnimationModelResource& source, ID3D12Device* device)
{
    if (&source == this || source.m_SourceModelPath.empty()) return false;
    if (!Load(source.m_SourceModelPath.c_str(), device, source.m_SourceConvert)) return false;
    // Reject a source file edited since the original load instead of copying stale palette indices.
    if (m_BoneNames != source.m_BoneNames || m_Meshes.size() != source.m_Meshes.size()) { Uninit(); return false; }
    // Importing the same source produces the same palette indices; copy explicit user mappings too.
    m_ResourceMap = source.m_ResourceMap;
    m_VmdTranslationScale = source.m_VmdTranslationScale;
    m_VmdRetargetTransforms = source.m_VmdRetargetTransforms;
    for (const auto& [name,path] : source.m_AnimationSourcePaths)
    {
        if (!LoadAnimation(path.c_str(), name.c_str())) { Uninit(); return false; }
    }
    vector<int> partIds;
    for (const auto& vertices : source.m_BaseGpuSkinVertices)
        partIds.push_back(vertices.empty() ? -1 : static_cast<int>(vertices.front().Diffuse.w));
    if (!ApplyMeshShadingOverridePartIds(partIds)) { Uninit(); return false; }
    return true;
}

bool AnimationModelResource::SetupLookAtRig(const LookAtSettings& settings)
{
    const array<string,4> overrides={settings.NeckBone,settings.HeadBone,settings.LeftEyeBone,settings.RightEyeBone};
    if (!m_LookAtRigReady || overrides != m_LookAtOverrideNames)
    {
        if (m_PmxRuntimeNodes.empty()) RebuildPmxRuntimeCache();
        m_LookAtPose.Nodes.clear();
        for (const auto& node : m_PmxRuntimeNodes)
        {
            HumanoidIk::Node entry;
            entry.Parent=node.ParentIndex;
            entry.Local=node.BonePtr ? node.BonePtr->BindLocalMatrix : node.Node->mTransformation;
            m_LookAtPose.Nodes.push_back(entry);
        }
        m_LookAtRig={};
        if (!m_LookAtPose.CaptureBindPose()) return false;
        auto resolve = [&](const string& exact,const char* humanoid,const char* vmd) -> int {
            if (!exact.empty())
            {
                const auto found=m_PmxRuntimeNodeIndexMap.find(exact);
                return found!=m_PmxRuntimeNodeIndexMap.end() && m_PmxRuntimeNodes[found->second].BonePtr ?
                       static_cast<int>(found->second) : -1;
            }
            const auto* indices=m_ResourceMap.Find(ModelResourceMap::Humanoid,humanoid);
            if (!indices) indices=m_ResourceMap.Find(ModelResourceMap::Vmd,vmd);
            if (!indices) indices=m_ResourceMap.Find(ModelResourceMap::Bones,vmd);
            if (!indices || indices->size()!=1 || indices->front()>=m_BoneNames.size()) return -1;
            const auto found=m_PmxRuntimeNodeIndexMap.find(m_BoneNames[indices->front()]);
            return found!=m_PmxRuntimeNodeIndexMap.end() && m_PmxRuntimeNodes[found->second].BonePtr ?
                   static_cast<int>(found->second) : -1;
        };
        m_LookAtRig.Bones={resolve(overrides[0],"neck","首"),resolve(overrides[1],"head","頭"),
                           resolve(overrides[2],"leftEye","左目"),resolve(overrides[3],"rightEye","右目")};
        aiVector3D facing;
        for (unsigned side=0;side<2;++side)
        {
            const int foot=resolve("",side ? "rightFoot" : "leftFoot",side ? "右足首" : "左足首");
            const int toe=resolve("",side ? "rightToes" : "leftToes",side ? "右つま先" : "左つま先");
            if (m_LookAtPose.Contains(foot) && m_LookAtPose.Contains(toe))
                facing+=m_LookAtPose.Position(toe)-m_LookAtPose.Position(foot);
        }
        m_HasLookAtForward=HumanoidIk::Finite(facing) && facing.SquareLength()>1e-10f;
        m_LookAtForward=HumanoidIk::Unit(facing);
        m_LookAtOverrideNames=overrides;
        m_LookAtRigReady=true;
    }
    for (int bone : m_LookAtRig.Bones) if (m_LookAtPose.Contains(bone)) return true;
    return false;
}

void AnimationModelResource::RestoreLookAtPose()
{
    if (!m_HasLookAtPose) return;
    for (int part=0;part<4;++part)
    {
        const int bone=m_LookAtSaved.Bindings.Bones[part];
        if (bone<0 || static_cast<size_t>(bone)>=m_PmxRuntimeNodes.size()) continue;
        auto* target=m_PmxRuntimeNodes[static_cast<size_t>(bone)].BonePtr;
        if (target) target->AnimationMatrix=m_LookAtSaved.Local[part];
    }
    m_HasLookAtPose=false;
    CommitPhysicsPose();
}

bool AnimationModelResource::ApplyLookAtPose(LookAtRuntime& runtime,const aiVector3D& modelTarget,float deltaTime)
{
    if (!SetupLookAtRig(runtime.Settings)) return false;
    for (size_t i=0;i<m_PmxRuntimeNodes.size();++i)
    {
        const auto& node=m_PmxRuntimeNodes[i];
        m_LookAtPose.Nodes[i].Local=node.BonePtr ? node.BonePtr->AnimationMatrix : node.Node->mTransformation;
    }
    m_LookAtSaved.Capture(m_LookAtPose,m_LookAtRig);
    auto settings=runtime.Settings;
    if (settings.AutoDetectForward && m_HasLookAtForward)
    {
        const auto up=HumanoidIk::Unit(settings.ModelUp,{0,1,0});
        settings.ModelForward=HumanoidIk::Unit(m_LookAtForward-up*(m_LookAtForward*up),settings.ModelForward);
    }
    if (!Gaze::Solve(m_LookAtPose,m_LookAtRig,runtime.State,modelTarget,settings,runtime.Tracking,deltaTime)) return false;
    for (int bone : m_LookAtRig.Bones)
    {
        if (!m_LookAtPose.Contains(bone)) continue;
        auto* target=m_PmxRuntimeNodes[static_cast<size_t>(bone)].BonePtr;
        if (target) target->AnimationMatrix=m_LookAtPose.Nodes[static_cast<size_t>(bone)].Local;
    }
    m_HasLookAtPose=true;
    CommitPhysicsPose();
    return true;
}
