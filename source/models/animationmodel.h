#pragma once

#include "main.h"
#include "vector.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <array>
#include <cstdint>

#include "cimport.h"
#include "scene.h"
#include "postprocess.h"
#include "matrix4x4.h"
#include "rendererstate.h"
#include "toonoutlinebuilder.h"
#include "vmdanimationimpoter.h"
#include "animationplayback.h"
#include "pmxphysicsdata.h"
#include "modelresourcemap.h"
#include "vmdretarget.h"
#include "humanoidik.h"
#include "lookatsolver.h"
#pragma comment(lib, "assimp-vc143-mt.lib")

struct ModelVertex
{
    XMFLOAT3 Position{};
    XMFLOAT3 Normal{};
    XMFLOAT2 TexCoord{};
    XMFLOAT4 Diffuse{};
};

struct GpuSkinVertex
{
    XMFLOAT3 Position{};
    XMFLOAT3 Normal{};
    XMFLOAT2 TexCoord{};
    XMFLOAT4 Diffuse{};
    int BoneIndices[4]{};
    float BoneWeights[4]{};
    int DeformType = 0;
    int DeformPadding[3]{};
    XMFLOAT4 SdefC{};
    XMFLOAT4 SdefR0{};
    XMFLOAT4 SdefR1{};
};

struct DeformVertex
{
    aiVector3D Position{};
    aiVector3D Normal{};
    int BoneNum = 0;
    string BoneName[4];
    float BoneWeight[4]{};
};

struct Bone
{
    aiMatrix4x4 Matrix{};
    aiMatrix4x4 AnimationMatrix{};
    aiMatrix4x4 BindLocalMatrix{};
    aiMatrix4x4 OffsetMatrix{};
};

struct PmxIkLink
{
    string BoneName{};
    bool HasLimit = false;
    aiVector3D LimitMin{};
    aiVector3D LimitMax{};
};

struct PmxIkConstraint
{
    string BoneName{};
    string TargetBoneName{};
    uint32_t IterationCount = 0;
    float LimitAngle = 0.0f;
    int32_t DeformDepth = 0;
    uint32_t BoneOrder = 0;
    vector<PmxIkLink> Links{};
};

struct PmxAppendConstraint
{
    string BoneName{};
    string AppendBoneName{};
    float Weight = 0.0f;
    bool InheritRotation = false;
    bool InheritTranslation = false;
    bool Local = false;
    int32_t DeformDepth = 0;
    uint32_t BoneOrder = 0;
};

struct PmxPositionMorphOffset
{
    uint32_t VertexIndex = 0;
    aiVector3D Position{};
};

struct PmxUvMorphOffset
{
    uint32_t VertexIndex = 0;
    XMFLOAT4 Uv{};
};

struct PmxBoneMorphOffset
{
    string BoneName{};
    aiVector3D Position{};
    aiQuaternion Rotation{};
};

struct PmxMaterialMorphOffset
{
    int32_t MaterialIndex = -1;
    uint8_t Operation = 0;
    XMFLOAT4 Diffuse{};
};

struct PmxMorph
{
    string Name{};
    uint8_t Type = 0;
    vector<PmxPositionMorphOffset> PositionOffsets{};
    vector<PmxUvMorphOffset> UvOffsets{};
    vector<PmxBoneMorphOffset> BoneOffsets{};
    vector<PmxMaterialMorphOffset> MaterialOffsets{};
    vector<pair<uint32_t, float>> GroupOffsets{};
};

struct MeshData
{
    static constexpr UINT LodCount = 3;
    ComPtr<ID3D12Resource> VertexBuffer{};
    D3D12_VERTEX_BUFFER_VIEW VertexBufferView{};
    ComPtr<ID3D12Resource> PreviousVertexBuffer{};
    D3D12_VERTEX_BUFFER_VIEW PreviousVertexBufferView{};
    bool PreviousVertexValid = false;
    ComPtr<ID3D12Resource> TeoVertexBuffer{};
    D3D12_VERTEX_BUFFER_VIEW TeoVertexBufferView{};
    array<ComPtr<ID3D12Resource>, kToonOutlineModeCount> TeoVertexBuffers{};
    array<D3D12_VERTEX_BUFFER_VIEW, kToonOutlineModeCount> TeoVertexBufferViews{};

    ComPtr<ID3D12Resource> InputVertexBuffer{};
    ComPtr<ID3D12Resource> TeoInputVertexBuffer{};
    array<ComPtr<ID3D12Resource>, kToonOutlineModeCount> TeoInputVertexBuffers{};

    ComPtr<ID3D12Resource> IndexBuffer{};
    D3D12_INDEX_BUFFER_VIEW IndexBufferView{};
    array<ComPtr<ID3D12Resource>, LodCount - 1> LodIndexBuffers{};
    array<D3D12_INDEX_BUFFER_VIEW, LodCount - 1> LodIndexBufferViews{};
    array<UINT, LodCount - 1> LodIndexCounts{};
    ComPtr<ID3D12Resource> TeoIndexBuffer{};
    D3D12_INDEX_BUFFER_VIEW TeoIndexBufferView{};
    array<ComPtr<ID3D12Resource>, kToonOutlineModeCount> TeoIndexBuffers{};
    array<D3D12_INDEX_BUFFER_VIEW, kToonOutlineModeCount> TeoIndexBufferViews{};

    UINT IndexCount = 0;
    UINT VertexCount = 0;
    UINT TeoIndexCount = 0;
    UINT TeoVertexCount = 0;
    array<UINT, kToonOutlineModeCount> TeoIndexCounts{};
    array<UINT, kToonOutlineModeCount> TeoVertexCounts{};
    int TextureIndex = -1;
    int MaterialIndex = -1;

    UINT SrvInputVertexIndex = 0;
    UINT UavOutputVertexIndex = 0;
    UINT SrvTeoInputVertexIndex = 0;
    UINT UavTeoOutputVertexIndex = 0;
    array<UINT, kToonOutlineModeCount> SrvTeoInputVertexIndices{};
    array<UINT, kToonOutlineModeCount> UavTeoOutputVertexIndices{};
    string MeshName{};
    string MaterialName{};
    float MaterialPartId = 10.0f;
    bool DefaultToonOutlineEnabled = true;

    const D3D12_INDEX_BUFFER_VIEW& GetLodIndexBufferView(UINT lod) const
    {
        if (lod == 0 || lod >= LodCount || LodIndexCounts[lod - 1] == 0)
        {
            return IndexBufferView;
        }
        return LodIndexBufferViews[lod - 1];
    }

    UINT GetLodIndexCount(UINT lod) const
    {
        if ((lod == 0 || lod >= LodCount || LodIndexCounts[lod - 1] == 0))
        {
            return IndexCount;
        }
        return LodIndexCounts[lod - 1];
    }
};

class AnimationModelResource
{
  private:
    static constexpr UINT m_kMAX_BONES = 2096;
    static constexpr int m_kMAX_BONE_INFLUENCES = 4;
    static constexpr UINT m_kSKINNING_DESCRIPTORS_PER_MESH =
        2 + RendererState::g_kFRAME_COUNT + (kToonOutlineModeCount * 2);
    static constexpr UINT m_kINPUT_VERTEX_SRV_OFFSET = 0;
    static constexpr UINT m_kBONE_SRV_OFFSET = 1;
    static constexpr UINT m_kOUTPUT_VERTEX_UAV_OFFSET = 1 + RendererState::g_kFRAME_COUNT;
    static constexpr UINT m_kTEO_DESCRIPTOR_OFFSET = 2 + RendererState::g_kFRAME_COUNT;

    struct VmdRootMotionBinding
    {
        string Name;
        const vector<VmdKeyframe>* Track = nullptr;
        size_t LayerIndex = 0;
        VmdTrackSampleCursor Cursor{};
    };

    struct VmdBoneBinding
    {
        Bone* BonePtr = nullptr;
        VmdRetarget::Transform Retarget{};
        VmdRootMotionBinding PrimaryTorsoCompensation;
        VmdRootMotionBinding SecondaryTorsoCompensation;
        vector<VmdRootMotionBinding> PrimaryRootMotion;
        vector<VmdRootMotionBinding> SecondaryRootMotion;
        const vector<VmdKeyframe>* PrimaryTrack = nullptr;
        const vector<VmdKeyframe>* SecondaryTrack = nullptr;
        VmdTrackSampleCursor PrimaryCursor{};
        VmdTrackSampleCursor SecondaryCursor{};
        aiVector3D BaseScale{1.0f, 1.0f, 1.0f};
        aiQuaternion BaseRotation{1.0f, 0.0f, 0.0f, 0.0f};
        aiVector3D BasePosition{0.0f, 0.0f, 0.0f};
    };

    struct VmdMorphBinding
    {
        uint32_t MorphIndex = 0;
        const vector<VmdScalarKeyframe>* Track = nullptr;
        VmdTrackSampleCursor Cursor{};
    };

    struct VmdLayeredBoneBinding
    {
        Bone* BonePtr = nullptr;
        VmdRetarget::Transform Retarget{};
        VmdRootMotionBinding TorsoCompensation;
        vector<VmdRootMotionBinding> RootMotion;
        const vector<VmdKeyframe>* Track = nullptr;
        size_t LayerIndex = 0;
        VmdTrackSampleCursor Cursor{};
        aiVector3D BaseScale{1.0f, 1.0f, 1.0f};
        aiQuaternion BaseRotation{1.0f, 0.0f, 0.0f, 0.0f};
        aiVector3D BasePosition{0.0f, 0.0f, 0.0f};
    };

    struct VmdLayeredMorphBinding
    {
        uint32_t MorphIndex = 0;
        const vector<VmdScalarKeyframe>* Track = nullptr;
        size_t LayerIndex = 0;
        VmdTrackSampleCursor Cursor{};
    };

    struct VmdLayeredIkBinding
    {
        const vector<VmdIkKeyframe>* Track = nullptr;
        size_t LayerIndex = 0;
        VmdTrackSampleCursor Cursor{};
    };

    struct VmdHumanoidIkChannel
    {
        VmdRootMotionBinding Foot, Toe, Root, Parent;
        VmdLayeredIkBinding FootSwitch, ToeSwitch;
    };

    struct PmxRuntimeNode
    {
        aiNode* Node = nullptr;
        string Name{};
        int ParentIndex = -1;
        Bone* BonePtr = nullptr;
    };

    struct PmxOrderedTransformStep
    {
        bool IsIk = false;
        size_t Index = 0;
        int32_t DeformDepth = 0;
        uint32_t BoneOrder = 0;
    };

    struct PmxAppendResult
    {
        aiQuaternion Rotation{1.0f, 0.0f, 0.0f, 0.0f};
        aiVector3D Translation{0.0f, 0.0f, 0.0f};
    };

    XMFLOAT3 m_AabbCenter{};
    XMFLOAT3 m_AabbExtents{};

    const aiScene* m_AiScene = nullptr;
    bool m_OwnsGeneratedAiScene = false;

    string m_SourceModelPath;
    bool m_SourceConvert = true;
    unordered_map<string, string> m_AnimationSourcePaths;
    HumanoidIk::Pose m_LookAtPose;
    Gaze::Rig m_LookAtRig;
    Gaze::SavedLocals m_LookAtSaved;
    array<string,4> m_LookAtOverrideNames;
    bool m_LookAtRigReady = false;
    bool m_HasLookAtPose = false;
    bool m_HasLookAtForward = false;
    aiVector3D m_LookAtForward{0,0,1};
    bool SetupLookAtRig(const LookAtSettings& settings);
    ModelResourceMap m_ResourceMap;
    bool m_IsPmxModel = false;
    float m_VmdTranslationScale = 1.0f;
    bool m_HumanoidIkRigDirty = true;
    HumanoidIk::Pose m_HumanoidIkPose;
    array<HumanoidIk::Leg, 2> m_HumanoidIkLegs{};
    array<VmdHumanoidIkChannel, 2> m_HumanoidIkPrimary{}, m_HumanoidIkSecondary{}, m_HumanoidIkLayered{};
    unordered_map<string, VmdRetarget::Transform> m_VmdRetargetTransforms;
    vector<MeshData> m_Meshes{};
    vector<string> m_BoneNames{};
    vector<vector<DeformVertex>> m_DeformVertex{};
    vector<vector<GpuSkinVertex>> m_GpuSkinVertices{};
    vector<vector<GpuSkinVertex>> m_BaseGpuSkinVertices{};
    vector<vector<GpuSkinVertex>> m_TeoGpuSkinVertices{};
    vector<array<vector<GpuSkinVertex>, kToonOutlineModeCount>> m_TeoGpuSkinVerticesByMode{};

    unordered_map<string, const aiScene*> m_Animation{};
    unordered_map<string, VmdAnimation> m_VmdAnimations{};
    unordered_map<string, uint32_t> m_BoneIndexMap{};
    unordered_map<string, Bone> m_Bone{};
    unordered_map<string, string> m_BoneParentMap{};
    vector<PmxAppendConstraint> m_PmxAppendConstraints{};
    vector<PmxIkConstraint> m_PmxIkConstraints{};
    vector<aiVector3D> m_PmxBaseVertices{};
    vector<aiVector3D> m_PmxBaseNormals{};
    vector<XMFLOAT2> m_PmxBaseTexCoords{};
    vector<PmxMorph> m_PmxMorphs{};
    vector<PmxRigidBodyData> m_PmxRigidBodies{};
    vector<PmxJointData> m_PmxJoints{};
    unordered_map<string, uint32_t> m_PmxMorphIndexMap{};
    vector<vector<pair<uint32_t, uint32_t>>> m_PmxVertexToMeshVertices{};
    vector<XMFLOAT4X4> m_BoneMatricesScratch{};
    uint64_t m_SkinningVersion = 0;
    uint64_t m_DispatchedSkinningVersion = UINT64_MAX;
    array<uint64_t, kToonOutlineModeCount> m_DispatchedOutlineSkinningVersions = {
        UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX};
    string m_LastPoseAnimation1{};
    string m_LastPoseAnimation2{};
    float m_LastPoseFrame1 = 0.0f;
    float m_LastPoseFrame2 = 0.0f;
    float m_LastPoseBlendRate = 0.0f;
    bool m_HasCachedPose = false;
    bool m_HasCachedLayeredPose = false;
    bool m_HasAppliedVmdMorphs = false;
    bool m_UseLayeredVmdIk = false;
    const VmdAnimation* m_CachedVmdPrimaryAnimation = nullptr;
    const VmdAnimation* m_CachedVmdSecondaryAnimation = nullptr;
    vector<VmdBoneBinding> m_VmdBoneBindings{};
    vector<VmdMorphBinding> m_VmdMorphBindings{};
    vector<const VmdAnimation*> m_CachedVmdLayerAnimations{};
    vector<float> m_VmdLayerFramesScratch{};
    vector<float> m_LastLayerPoseTimes{};
    vector<VmdLayeredBoneBinding> m_VmdLayeredBoneBindings{};
    vector<VmdLayeredMorphBinding> m_VmdLayeredMorphBindings{};
    unordered_map<string, VmdLayeredIkBinding> m_VmdLayeredIkBindings{};
    unordered_map<string, const vector<VmdIkKeyframe>*> m_VmdIkTrackCache{};
    unordered_map<string, VmdTrackSampleCursor> m_VmdIkTrackCursors{};
    vector<pair<uint32_t, float>> m_VmdActiveMorphsScratch{};
    vector<aiVector3D> m_VmdMorphPositionOffsetsScratch{};
    vector<XMFLOAT2> m_VmdMorphUvOffsetsScratch{};
    vector<PmxRuntimeNode> m_PmxRuntimeNodes{};
    unordered_map<string, size_t> m_PmxRuntimeNodeIndexMap{};
    vector<XMFLOAT4X4> m_PmxGlobalMatricesScratch{};
    vector<PmxOrderedTransformStep> m_PmxOrderedTransformSteps{};
    unordered_map<string, PmxAppendResult> m_PmxAppendResultsScratch{};

    array<ComPtr<ID3D12Resource>, RendererState::g_kFRAME_COUNT> m_BoneBuffers{};
    array<void*, RendererState::g_kFRAME_COUNT> m_pBoneBufferMapped{};

    ComPtr<ID3D12DescriptorHeap> m_SkinningDescHeap{};
    UINT m_SkinningDescHeapStart = 0;

    ID3D12Device* m_pDevice = nullptr;

    bool TryLoadEmbeddedTextureByIndex(const aiString& texPath, const char* modelName, int& outTexIndex) const;
    bool TryLoadEmbeddedTextureByName(const aiString& texPath, const char* modelName, int& outTexIndex) const;
    int ResolveMeshTextureIndex(const aiMesh* mesh, const char* fileName, const string& dirPath) const;
    aiNodeAnim* FindNodeAnimChannelCached(aiAnimation* animation, const string& boneName);
    mutable unordered_map<const aiAnimation*, unordered_map<string, aiNodeAnim*>> m_NodeAnimChannelCache{};
    uint32_t m_SkinningMatrixCount = 0;
    bool LoadVmdAnimation(const char* fileName, const char* name);
    void UpdateVmdBoneMatrices(const VmdAnimation* animation1, float frame1, const VmdAnimation* animation2,
                               float frame2, float blendRate);
    void UpdateVmdBoneMatrices(const vector<AnimationPlaybackLayer>& animationLayers);
    void ConfigureVmdRetargeting(const char* fileName);
    void BuildHumanoidIkChannel(const VmdAnimation* animation, size_t layerIndex, unsigned side,
                                VmdHumanoidIkChannel& channel);
    void SetupHumanoidIkRig();
    HumanoidIk::Target SampleHumanoidIkTarget(VmdHumanoidIkChannel& channel, const HumanoidIk::Leg& leg,
                                             float frame, const vector<float>* layerFrames);
    void ApplyVmdHumanoidIk(float frame1, float frame2, float blendRate, bool layered);
    void BuildVmdRootMotionBindings(uint32_t boneIndex, const VmdAnimation* animation, size_t layerIndex,
                                    vector<VmdRootMotionBinding>& bindings);
    void BuildVmdTorsoCompensation(uint32_t boneIndex, const VmdAnimation* animation, size_t layerIndex,
                                   VmdRootMotionBinding& binding);
    void SampleVmdTorsoCompensation(VmdRootMotionBinding& binding, float frame, const vector<float>* layerFrames,
                                    const VmdRetarget::Transform& transform, const aiVector3D& bindPosition,
                                    aiQuaternion& rotation, aiVector3D& position);
    void SampleVmdRootMotion(vector<VmdRootMotionBinding>& bindings, float frame,
                             const vector<float>* layerFrames, aiQuaternion& rotation, aiVector3D& position);
    void InvalidateVmdRuntimeCache();
    void RebuildVmdRuntimeCache(const VmdAnimation* primaryAnimation, const VmdAnimation* secondaryAnimation);
    bool IsVmdLayeredRuntimeCacheValid(const vector<AnimationPlaybackLayer>& animationLayers) const;
    void RebuildVmdLayeredRuntimeCache(const vector<AnimationPlaybackLayer>& animationLayers);
    void InvalidatePmxRuntimeCache();
    void RebuildPmxRuntimeCache();
    void BuildPmxVertexMeshMap();
    void ApplyVmdMorphs(const VmdAnimation* animation, float currentFrame);
    void ApplyVmdMorphLayers();
    void ApplyActiveVmdMorphs();
    void ApplyPmxOrderedTransforms(const VmdAnimation* animation, float currentFrame);
    bool IsVmdIkEnabled(const VmdAnimation* animation, const string& ikBoneName, float currentFrame);

    void CreateBone(aiNode* node);
    void UpdateBoneMatrix(aiNode* node, aiMatrix4x4 matrix);
    void UpdateBindPoseBoneMatrix(aiNode* node, aiMatrix4x4 matrix);
    void WriteBoneMatricesToBuffer();

    bool CreateGpuSkinningBuffers(ID3D12Device* device);

  public:
    AnimationModelResource() = default;

    ~AnimationModelResource()
    {
        Uninit();
    }

    bool Load(const char* fileName, ID3D12Device* device, bool isConvert = true);
    bool LoadAnimation(const char* fileName, const char* name);
    bool LoadPoseInstance(const AnimationModelResource& source, ID3D12Device* device);
    bool HasLookAtBones(const LookAtSettings& settings) { return SetupLookAtRig(settings); }
    void RestoreLookAtPose();
    bool ApplyLookAtPose(LookAtRuntime& runtime, const aiVector3D& modelTarget, float deltaTime);
    // Explicit mappings for rigs not recognized by VRM metadata / common aliases.
    bool SetVmdBoneMapping(const string& sourceBoneName, const string& targetBoneName);
    bool SetVmdTranslationScale(float modelUnitsPerVmdUnit);

    void UpdateBoneMatrices(const char* animName1, float frame1, const char* animName2, float frame2, float blendRate);
    void UpdateBoneMatrices(const vector<AnimationPlaybackLayer>& animationLayers);
    void InvalidateAnimationPoseCache();
    void ResetBoneMatricesToBindPose();
    bool GetBoneGlobalTransform(const string& boneName, XMFLOAT4X4& transform);
    bool GetBoneBindGlobalTransform(const string& boneName, XMFLOAT4X4& transform);
    bool SetBoneGlobalTransform(const string& boneName, const XMFLOAT4X4& transform, bool preserveTranslation);
    void CommitPhysicsPose();

    bool ApplyMeshShadingOverridePartIds(const vector<int>& overridePartIds);
    // outlineModes: bit mask of requested ToonOutlineMeshMode presets; zero skins only the base mesh.
    void DispatchGpuSkinning(ID3D12GraphicsCommandList* pCommandList, uint32_t outlineModes = 0);

    void Uninit();

    const ModelResourceMap& GetResourceMap() const { return m_ResourceMap; }

    UINT GetMeshCount() const
    {
        return (UINT)m_Meshes.size();
    }

    aiAnimation* GetAnimation(const string& name);

    const MeshData& GetMeshData(UINT index) const
    {
        return m_Meshes[index];
    }

    const string& GetMaterialPath() const
    {
        static string empty;
        return empty;
    }

    XMFLOAT3 GetAabbCenter() const
    {
        return m_AabbCenter;
    }

    XMFLOAT3 GetAabbExtents() const
    {
        return m_AabbExtents;
    }

    const vector<PmxRigidBodyData>& GetPmxRigidBodies() const
    {
        return m_PmxRigidBodies;
    }

    const vector<PmxJointData>& GetPmxJoints() const
    {
        return m_PmxJoints;
    }

    bool HasPmxPhysics() const
    {
        return !m_PmxRigidBodies.empty();
    }
};
