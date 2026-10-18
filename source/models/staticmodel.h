#pragma once
#include "main.h"
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <array>
#include "toonoutlinebuilder.h"
#include "visibility/visibilitytypes.h"
#include "meshlets/meshletbuilder.h"
#include "virtualgeometry/virtualgeometryruntime.h"

struct StaticModelVertex
{
	XMFLOAT3 Position{};
	XMFLOAT3 Normal{};
	XMFLOAT2 TexCoord{};
	XMFLOAT4 Diffuse{};
};

struct StaticMeshData
{
	static constexpr UINT LodCount = 5;
    static constexpr UINT RayLodCount=3;
    array<ComPtr<ID3D12Resource>,RayLodCount> RayLodBuffers{};
    array<D3D12_INDEX_BUFFER_VIEW,RayLodCount> RayLodViews{};
    array<UINT,RayLodCount> RayLodCounts{};
    ComPtr<ID3D12Resource> RayProxyVertices,RayProxyIndices;
    D3D12_VERTEX_BUFFER_VIEW RayProxyVertexView{};
    D3D12_INDEX_BUFFER_VIEW RayProxyIndexView{};
    XMFLOAT3 RayCenter{}; float RayRadius=0;
	ComPtr<ID3D12Resource> VertexBuffer{};
	ComPtr<ID3D12Resource> IndexBuffer{};
	array<ComPtr<ID3D12Resource>, LodCount - 1> LodIndexBuffers{};
	ComPtr<ID3D12Resource> TeoVertexBuffer{};
	ComPtr<ID3D12Resource> TeoIndexBuffer{};
	array<ComPtr<ID3D12Resource>, kToonOutlineModeCount> TeoVertexBuffers{};
	array<ComPtr<ID3D12Resource>, kToonOutlineModeCount> TeoIndexBuffers{};
	D3D12_VERTEX_BUFFER_VIEW VertexBufferView{};
	D3D12_INDEX_BUFFER_VIEW IndexBufferView{};
	array<D3D12_INDEX_BUFFER_VIEW, LodCount - 1> LodIndexBufferViews{};
	D3D12_VERTEX_BUFFER_VIEW TeoVertexBufferView{};
	D3D12_INDEX_BUFFER_VIEW TeoIndexBufferView{};
	array<D3D12_VERTEX_BUFFER_VIEW, kToonOutlineModeCount> TeoVertexBufferViews{};
	array<D3D12_INDEX_BUFFER_VIEW, kToonOutlineModeCount> TeoIndexBufferViews{};
	UINT IndexCount = 0;
	array<UINT, LodCount - 1> LodIndexCounts{};
	UINT VertexCount = 0;
	UINT TeoIndexCount = 0;
	UINT TeoVertexCount = 0;
	array<UINT, kToonOutlineModeCount> TeoIndexCounts{};
	array<UINT, kToonOutlineModeCount> TeoVertexCounts{};
	int TextureIndex = -1;
	string MeshName{};
	string MaterialName{};
	float MaterialPartId = 10.0f;
	float AppliedMaterialPartId = 10.0f;
	bool DefaultToonOutlineEnabled = true;
	vector<StaticModelVertex> CpuVertices{};
	array<Meshlets::Asset, LodCount> MeshletLODs{};
	array<shared_ptr<const VirtualGeometryRuntime::RuntimeAsset>, LodCount> VirtualGeometryLODs{};
    VGImportSettings VirtualGeometrySettings;
    array<shared_future<shared_ptr<const VirtualGeometryRuntime::RuntimeAsset>>, LodCount> VirtualGeometryBuilds{};
    string VirtualGeometryBuildError;
    bool VirtualGeometryRootFallback = false;
	vector<uint32_t> CpuIndices{}; // Exact optimized LOD0 upload data; never reconstructed by GPU readback.
	array<vector<uint32_t>, LodCount - 1> CpuLodIndices{};
	array<vector<StaticModelVertex>, kToonOutlineModeCount> CpuTeoVerticesByMode{};

	const D3D12_INDEX_BUFFER_VIEW& GetLodIndexBufferView(UINT lod) const
	{
		if ((lod == 0 || lod >= LodCount || LodIndexCounts[lod - 1] == 0))
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

class StaticModelResource
{
private:
	vector<StaticMeshData> m_Meshes{};
	string m_MaterialPath{};
	string m_SourcePath{};
	Visibility::LODMetadata m_LODMetadata{};

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
    bool LoadVirtualGeometry(const char* fileName, ID3D12Device* device);
    bool LoadDerivedVirtualGeometry(const char* fileName, ID3D12Device* device, bool convert);
    void PublishDerivedVirtualGeometry(bool convert);
    bool UsesDerivedVirtualGeometry() const { return m_UsesDerivedVG; }
    void CaptureSourceIdentity(const std::filesystem::path& path);
    bool SourceDependenciesChanged() const;
	bool LoadAssimpModel(const char* fileName, ID3D12Device* device, bool isConvert = true);
	bool ApplyMeshShadingOverridePartIds(ID3D12Device* device, const vector<int>& overridePartIds);
	void Uninit();

	UINT GetMeshCount() const { return (UINT)m_Meshes.size(); }
	const StaticMeshData& GetMeshData(UINT index) const { return m_Meshes[index]; }
	const string& GetMaterialPath() const { return m_MaterialPath; }
	const string& GetSourcePath() const { return m_SourcePath; }
	const Visibility::LODMetadata& GetLODMetadata() const { return m_LODMetadata; }
	void SetLODMetadata(const Visibility::LODMetadata& value) { if (Visibility::ValidLOD(value)) m_LODMetadata = value; }
	void LoadLODMetadata(const char* path);
    bool PollVirtualGeometry();
    void RebuildVirtualGeometry(const VGImportSettings& settings);
    const VGImportSettings& GetVirtualGeometrySettings() const { return m_VGSettings; }

	XMFLOAT3 GetAabbCenter() const { return m_AabbCenter; }
	XMFLOAT3 GetAabbExtents() const { return m_AabbExtents; }
private:
    VGImportSettings m_VGSettings;
    bool m_UsesDerivedVG = false, m_DerivedVGPublished = false;
    vector<std::filesystem::path> m_VGDependencies;
    vector<uint64_t> m_VGDependencyHashes;
    vector<std::filesystem::file_time_type> m_VGDependencyTimes;
};


