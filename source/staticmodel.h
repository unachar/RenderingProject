#pragma once
#include "main.h"
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <array>
#include "toonoutlinebuilder.h"

struct StaticModelVertex
{
	XMFLOAT3 Position{};
	XMFLOAT3 Normal{};
	XMFLOAT2 TexCoord{};
	XMFLOAT4 Diffuse{};
};

struct StaticMeshData
{
	static constexpr UINT LodCount = 3;
	ComPtr<ID3D12Resource> VertexBuffer{};
	ComPtr<ID3D12Resource> IndexBuffer{};
	std::array<ComPtr<ID3D12Resource>, LodCount - 1> LodIndexBuffers{};
	ComPtr<ID3D12Resource> TeoVertexBuffer{};
	ComPtr<ID3D12Resource> TeoIndexBuffer{};
	std::array<ComPtr<ID3D12Resource>, ToonOutlineBuilder::kModeCount> TeoVertexBuffers{};
	std::array<ComPtr<ID3D12Resource>, ToonOutlineBuilder::kModeCount> TeoIndexBuffers{};
	D3D12_VERTEX_BUFFER_VIEW VertexBufferView{};
	D3D12_INDEX_BUFFER_VIEW IndexBufferView{};
	std::array<D3D12_INDEX_BUFFER_VIEW, LodCount - 1> LodIndexBufferViews{};
	D3D12_VERTEX_BUFFER_VIEW TeoVertexBufferView{};
	D3D12_INDEX_BUFFER_VIEW TeoIndexBufferView{};
	std::array<D3D12_VERTEX_BUFFER_VIEW, ToonOutlineBuilder::kModeCount> TeoVertexBufferViews{};
	std::array<D3D12_INDEX_BUFFER_VIEW, ToonOutlineBuilder::kModeCount> TeoIndexBufferViews{};
	UINT IndexCount = 0;
	std::array<UINT, LodCount - 1> LodIndexCounts{};
	UINT VertexCount = 0;
	UINT TeoIndexCount = 0;
	UINT TeoVertexCount = 0;
	std::array<UINT, ToonOutlineBuilder::kModeCount> TeoIndexCounts{};
	std::array<UINT, ToonOutlineBuilder::kModeCount> TeoVertexCounts{};
	int TextureIndex = -1;
	string MeshName{};
	string MaterialName{};
	float MaterialPartId = 10.0f;
	float AppliedMaterialPartId = 10.0f;
	bool DefaultToonOutlineEnabled = true;
	vector<StaticModelVertex> CpuVertices{};
	std::array<vector<StaticModelVertex>, ToonOutlineBuilder::kModeCount> CpuTeoVerticesByMode{};

	const D3D12_INDEX_BUFFER_VIEW& GetLodIndexBufferView(UINT lod) const
	{
		return (lod == 0 || lod >= LodCount || LodIndexCounts[lod - 1] == 0)
			? IndexBufferView : LodIndexBufferViews[lod - 1];
	}
	UINT GetLodIndexCount(UINT lod) const
	{
		return (lod == 0 || lod >= LodCount || LodIndexCounts[lod - 1] == 0)
			? IndexCount : LodIndexCounts[lod - 1];
	}
};

class StaticModelResource
{
private:
	vector<StaticMeshData> m_Meshes{};
	string m_MaterialPath{};

	vector<XMFLOAT3> m_Positions{};
	vector<XMFLOAT3> m_Normals{};
	vector<XMFLOAT2> m_TexCoords{};
	vector<StaticModelVertex> m_Vertices{};
	vector<unsigned int> m_Indices{};

	XMFLOAT3 m_AabbCenter = { 0.0f, 0.0f, 0.0f };
	XMFLOAT3 m_AabbExtents = { 0.5f, 0.5f, 0.5f };

	void Clear()
	{
		m_Positions.clear();
		m_Normals.clear();
		m_TexCoords.clear();
		m_Vertices.clear();
		m_Indices.clear();
		m_AabbCenter = { 0.0f, 0.0f, 0.0f };
		m_AabbExtents = { 0.5f, 0.5f, 0.5f };
	}

	void Reserve()
	{
		m_Positions.reserve(1024);
		m_Normals.reserve(1024);
		m_TexCoords.reserve(512);
		m_Vertices.reserve(4096);
		m_Indices.reserve(4096);
	}

	bool UploadBufferData(
		ID3D12Device* device,
		ID3D12Resource* dstResource,
		const void* srcData,
		UINT sizeInBytes,
		D3D12_RESOURCE_STATES finalState,
		const char* logTag);
	bool UploadBufferData(
		ID3D12Device* device,
		ID3D12Resource* dstResource,
		const void* srcData,
		UINT sizeInBytes,
		D3D12_RESOURCE_STATES beforeState,
		D3D12_RESOURCE_STATES finalState,
		const char* logTag);

	bool CreateDefaultBufferAndUpload(
		ID3D12Device* device,
		UINT sizeInBytes,
		const void* srcData,
		D3D12_RESOURCE_STATES finalState,
		const char* createErrorLog,
		const char* uploadLogTag,
		ComPtr<ID3D12Resource>& outResource);
	bool BuildLodIndexBuffers(
		ID3D12Device* device,
		const vector<StaticModelVertex>& vertices,
		const vector<unsigned int>& indices,
		StaticMeshData& meshData);

public:
	bool LoadObj(const char* fileName, ID3D12Device* device);
	bool LoadAssimpModel(const char* fileName, ID3D12Device* device, bool isConvert = true);
	bool LoadFBX(const char* fileName, ID3D12Device* device, bool isConvert = true);
	bool ApplyMeshShadingOverridePartIds(ID3D12Device* device, const vector<int>& overridePartIds);
	void Uninit();

	UINT GetMeshCount() const { return (UINT)m_Meshes.size(); }
	const StaticMeshData& GetMeshData(UINT index) const { return m_Meshes[index]; }
	const string& GetMaterialPath() const { return m_MaterialPath; }

	XMFLOAT3 GetAabbCenter() const { return m_AabbCenter; }
	XMFLOAT3 GetAabbExtents() const { return m_AabbExtents; }
};

