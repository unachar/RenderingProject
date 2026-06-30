#pragma once

#include "main.h"
#include "Vector.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <array>

#include "cimport.h"
#include "scene.h"
#include "postprocess.h"
#include "matrix4x4.h"
#include "toonoutlinebuilder.h"
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
	int      BoneIndices[4]{};
	float    BoneWeights[4]{};
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

struct MeshData
{
	ComPtr<ID3D12Resource> VertexBuffer{};
	D3D12_VERTEX_BUFFER_VIEW VertexBufferView{};
	ComPtr<ID3D12Resource> TeoVertexBuffer{};
	D3D12_VERTEX_BUFFER_VIEW TeoVertexBufferView{};
	std::array<ComPtr<ID3D12Resource>, ToonOutlineBuilder::kModeCount> TeoVertexBuffers{};
	std::array<D3D12_VERTEX_BUFFER_VIEW, ToonOutlineBuilder::kModeCount> TeoVertexBufferViews{};

	ComPtr<ID3D12Resource> InputVertexBuffer{};
	ComPtr<ID3D12Resource> TeoInputVertexBuffer{};
	std::array<ComPtr<ID3D12Resource>, ToonOutlineBuilder::kModeCount> TeoInputVertexBuffers{};

	ComPtr<ID3D12Resource> IndexBuffer{};
	D3D12_INDEX_BUFFER_VIEW IndexBufferView{};
	ComPtr<ID3D12Resource> TeoIndexBuffer{};
	D3D12_INDEX_BUFFER_VIEW TeoIndexBufferView{};
	std::array<ComPtr<ID3D12Resource>, ToonOutlineBuilder::kModeCount> TeoIndexBuffers{};
	std::array<D3D12_INDEX_BUFFER_VIEW, ToonOutlineBuilder::kModeCount> TeoIndexBufferViews{};

	UINT IndexCount = 0;
	UINT VertexCount = 0;
	UINT TeoIndexCount = 0;
	UINT TeoVertexCount = 0;
	std::array<UINT, ToonOutlineBuilder::kModeCount> TeoIndexCounts{};
	std::array<UINT, ToonOutlineBuilder::kModeCount> TeoVertexCounts{};
	int TextureIndex = -1;

	UINT SrvInputVertexIndex = 0;
	UINT UavOutputVertexIndex = 0;
	UINT SrvTeoInputVertexIndex = 0;
	UINT UavTeoOutputVertexIndex = 0;
	std::array<UINT, ToonOutlineBuilder::kModeCount> SrvTeoInputVertexIndices{};
	std::array<UINT, ToonOutlineBuilder::kModeCount> UavTeoOutputVertexIndices{};
	string MeshName{};
	string MaterialName{};
	float MaterialPartId = 10.0f;
	bool DefaultToonOutlineEnabled = true;
};

class AnimationModelResource
{
private:

	static constexpr UINT m_kMAX_BONES = 2096;
	static constexpr int m_kMAX_BONE_INFLUENCES = 4;
	static constexpr UINT m_kSKINNING_DESCRIPTORS_PER_MESH = 3 + (ToonOutlineBuilder::kModeCount * 2);
	static constexpr UINT m_kINPUT_VERTEX_SRV_OFFSET = 0;
	static constexpr UINT m_kBONE_SRV_OFFSET = 1;
	static constexpr UINT m_kOUTPUT_VERTEX_UAV_OFFSET = 2;
	static constexpr UINT m_kTEO_DESCRIPTOR_OFFSET = 3;

	XMFLOAT3 m_AabbCenter{};
	XMFLOAT3 m_AabbExtents{};


	const aiScene* m_AiScene = nullptr;

	vector<MeshData> m_Meshes{};
	vector<string> m_BoneNames{};
	vector<vector<DeformVertex>> m_DeformVertex{};
	vector<vector<GpuSkinVertex>> m_GpuSkinVertices{};
	vector<vector<GpuSkinVertex>> m_TeoGpuSkinVertices{};
	vector<array<vector<GpuSkinVertex>, ToonOutlineBuilder::kModeCount>> m_TeoGpuSkinVerticesByMode{};
	

	unordered_map<string, const aiScene*> m_Animation{};
	unordered_map<string, uint32_t> m_BoneIndexMap{};
	unordered_map<string, Bone> m_Bone{};
	vector<XMFLOAT4X4> m_BoneMatricesScratch{};


	ComPtr<ID3D12Resource> m_BoneBuffer{};
	void* m_pBoneBufferMapped = nullptr;
	

	ComPtr<ID3D12DescriptorHeap> m_SkinningDescHeap{};
	UINT m_SkinningDescHeapStart = 0;

	ID3D12Device* m_pDevice = nullptr;

	bool TryLoadEmbeddedTextureByIndex(const aiString& texPath, const char* modelName, int& outTexIndex) const;
	bool TryLoadEmbeddedTextureByName(const aiString& texPath, const char* modelName, int& outTexIndex) const;
	int ResolveMeshTextureIndex(const aiMesh* mesh, const char* fileName, const string& dirPath) const;
	aiNodeAnim* FindNodeAnimChannel(aiAnimation* animation, const string& boneName) const;
	void SampleNodeAnimation(aiAnimation* animation, aiNodeAnim* channel, float timeSeconds,
		aiQuaternion& outRotation, aiVector3D& outPosition, aiVector3D& outScale) const;

	void CreateBone(aiNode* node);
	void UpdateBoneMatrix(aiNode* node, aiMatrix4x4 matrix);
	void UpdateBindPoseBoneMatrix(aiNode* node, aiMatrix4x4 matrix);
	void WriteBoneMatricesToBuffer();

	bool CreateGpuSkinningBuffers(ID3D12Device* device);
public:
	AnimationModelResource() = default;
	~AnimationModelResource() { Uninit(); }

	bool Load(const char* fileName, ID3D12Device* device, bool isConvert = true);
	bool LoadAnimation(const char* fileName, const char* name);

	void UpdateBoneMatrices(const char* animName1, float frame1,
		const char* animName2, float frame2, float blendRate);

	bool ApplyMeshShadingOverridePartIds(const vector<int>& overridePartIds);
	void DispatchGpuSkinning(ID3D12GraphicsCommandList* pCommandList);

	void Uninit();

	UINT GetMeshCount() const { return (UINT)m_Meshes.size(); }
	aiAnimation* GetAnimation(const string& name);
	const MeshData& GetMeshData(UINT index) const { return m_Meshes[index]; }
	const string& GetMaterialPath() const { static string empty; return empty; }


	XMFLOAT3 GetAabbCenter() const { return m_AabbCenter; }
	XMFLOAT3 GetAabbExtents() const { return m_AabbExtents; }
};

