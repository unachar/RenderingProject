#include "pch.h"
#include "envflags.h"
#include <unordered_set>
#include "gpudriven/gpudrivenrenderer.h"
#include "../../External/meshoptimizer/src/meshoptimizer.h"
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <string.h>
#include <shlwapi.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cctype>

#pragma comment(lib, "shlwapi.lib")

#include "main.h"
#include "staticmodel.h"
#include "virtualgeometry/virtualgeometrypagecache.h"
#include "virtualgeometry/virtualgeometryresources.h"
#include "materialmanager.h"
#include <json/json.h>
#include <iomanip>
#include "meshlets/meshletmanager.h"
#include "texturemanager.h"
#include "materialpartresolver.h"
#include "cimport.h"
#include "scene.h"
#include "material.h"
#include "modelimportutils.h"
#include "pmxloader.h"
#include "postprocess.h"
#include "toonoutlinebuilder.h"

#pragma comment(lib, "assimp-vc143-mt.lib")

struct MtlData
{
	XMFLOAT4 Kd = { 1.0f, 1.0f, 1.0f, 1.0f };
	string MapKd = "";
};

static const char* GetAssimpTextureTypeName(aiTextureType type)
{
	switch (type)
	{
	case aiTextureType_DIFFUSE: return "DIFFUSE";
	case aiTextureType_SPECULAR: return "SPECULAR";
	case aiTextureType_AMBIENT: return "AMBIENT";
	case aiTextureType_EMISSIVE: return "EMISSIVE";
	case aiTextureType_HEIGHT: return "HEIGHT";
	case aiTextureType_NORMALS: return "NORMALS";
	case aiTextureType_SHININESS: return "SHININESS";
	case aiTextureType_OPACITY: return "OPACITY";
	case aiTextureType_DISPLACEMENT: return "DISPLACEMENT";
	case aiTextureType_LIGHTMAP: return "LIGHTMAP";
	case aiTextureType_REFLECTION: return "REFLECTION";
	case aiTextureType_BASE_COLOR: return "BASE_COLOR";
	case aiTextureType_NORMAL_CAMERA: return "NORMAL_CAMERA";
	case aiTextureType_EMISSION_COLOR: return "EMISSION_COLOR";
	case aiTextureType_METALNESS: return "METALNESS";
	case aiTextureType_DIFFUSE_ROUGHNESS: return "DIFFUSE_ROUGHNESS";
	case aiTextureType_AMBIENT_OCCLUSION: return "AMBIENT_OCCLUSION";
	default: return "UNKNOWN";
	}
}

static void LogAssimpMaterialTexture(const aiMaterial* material, aiTextureType type)
{
	const unsigned int textureCount = material->GetTextureCount(type);
	for (unsigned int i = 0; i < textureCount; ++i)
	{
		aiString path;
		if (material->GetTexture(type, i, &path) == AI_SUCCESS)
		{
			Debug::Log("    Texture[%s][%u]: %s\n", GetAssimpTextureTypeName(type), i, path.C_Str());
		}
	}
}

static void LogAssimpModelInfo(const aiScene* scene, const char* fileName, const char* modelKind)
{
	if (!scene)
	{
		return;
	}

	Debug::Log("==== %s model import info: %s ====\n", modelKind, fileName);
	Debug::Log("Scene: meshes=%u, materials=%u, textures=%u, animations=%u\n",
		scene->mNumMeshes, scene->mNumMaterials, scene->mNumTextures, scene->mNumAnimations);

	for (unsigned int i = 0; i < scene->mNumMaterials; ++i)
	{
		const aiMaterial* material = scene->mMaterials[i];
		aiString name;
		aiColor3D diffuse(1.0f, 1.0f, 1.0f);
		aiColor3D specular(0.0f, 0.0f, 0.0f);
		aiColor3D ambient(0.0f, 0.0f, 0.0f);
		aiColor3D emissive(0.0f, 0.0f, 0.0f);
		aiColor4D baseColor(1.0f, 1.0f, 1.0f, 1.0f);
		float opacity = 1.0f;
		float shininess = 0.0f;
		float metallic = 0.0f;
		float roughness = 0.0f;
		int twoSided = 0;

		material->Get(AI_MATKEY_NAME, name);
		material->Get(AI_MATKEY_COLOR_DIFFUSE, diffuse);
		material->Get(AI_MATKEY_COLOR_SPECULAR, specular);
		material->Get(AI_MATKEY_COLOR_AMBIENT, ambient);
		material->Get(AI_MATKEY_COLOR_EMISSIVE, emissive);
		material->Get(AI_MATKEY_BASE_COLOR, baseColor);
		material->Get(AI_MATKEY_OPACITY, opacity);
		material->Get(AI_MATKEY_SHININESS, shininess);
		material->Get(AI_MATKEY_METALLIC_FACTOR, metallic);
		material->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness);
		material->Get(AI_MATKEY_TWOSIDED, twoSided);

		Debug::Log("  Material[%u]: name='%s', properties=%u\n", i, name.C_Str(), material->mNumProperties);
		Debug::Log("    Diffuse=(%.3f, %.3f, %.3f), Specular=(%.3f, %.3f, %.3f), Ambient=(%.3f, %.3f, %.3f), Emissive=(%.3f, %.3f, %.3f)\n",
			diffuse.r, diffuse.g, diffuse.b, specular.r, specular.g, specular.b,
			ambient.r, ambient.g, ambient.b, emissive.r, emissive.g, emissive.b);
		Debug::Log("    BaseColor=(%.3f, %.3f, %.3f, %.3f), Opacity=%.3f, Shininess=%.3f, Metallic=%.3f, Roughness=%.3f, TwoSided=%d\n",
			baseColor.r, baseColor.g, baseColor.b, baseColor.a, opacity, shininess, metallic, roughness, twoSided);

		const aiTextureType textureTypes[] =
		{
			aiTextureType_DIFFUSE,
			aiTextureType_BASE_COLOR,
			aiTextureType_SPECULAR,
			aiTextureType_NORMALS,
			aiTextureType_NORMAL_CAMERA,
			aiTextureType_METALNESS,
			aiTextureType_DIFFUSE_ROUGHNESS,
			aiTextureType_AMBIENT_OCCLUSION,
			aiTextureType_EMISSIVE,
			aiTextureType_OPACITY,
		};
		for (aiTextureType type : textureTypes)
		{
			LogAssimpMaterialTexture(material, type);
		}
	}

	for (unsigned int i = 0; i < scene->mNumMeshes; ++i)
	{
		const aiMesh* mesh = scene->mMeshes[i];
		if (!mesh)
		{
			Debug::Log("  Mesh[%u]: <null>\n", i);
			continue;
		}

		Debug::Log("  Mesh[%u]: name='%s', material=%u, vertices=%u, faces=%u, bones=%u, normals=%d, texcoord0=%d\n",
			i, mesh->mName.C_Str(), mesh->mMaterialIndex, mesh->mNumVertices, mesh->mNumFaces,
			mesh->mNumBones, static_cast<int>(mesh->HasNormals()), static_cast<int>(mesh->HasTextureCoords(0)));
		Debug::Log("    MaterialPartId=%.0f\n", ResolveMaterialPartId(scene, mesh));
	}
	Debug::Log("==== end %s model import info ====\n", modelKind);
}

static unordered_map<string, MtlData> ParseMtl(const string& mtlFilePath)
{
	unordered_map<string, MtlData> materials;
	ifstream ifs(mtlFilePath);
	if (!ifs) return materials;

	string line;
	string currentMtl;

	while (getline(ifs, line))
	{
		if (line.rfind("newmtl ", 0) == 0)
		{
			istringstream iss(line.substr(7));
			string name;
			iss >> name;
			if (!name.empty())
			{
				currentMtl = name;
				materials[currentMtl] = MtlData();
				materials[currentMtl].Kd = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
			}
		}
		else if (!currentMtl.empty() && line.rfind("Kd ", 0) == 0)
		{
			istringstream iss(line.substr(3));
			float r, g, b;
			if (iss >> r >> g >> b)
			{
				materials[currentMtl].Kd = XMFLOAT4(r, g, b, 1.0f);
			}
		}
		else if (!currentMtl.empty() && line.rfind("map_Kd ", 0) == 0)
		{
			istringstream iss(line.substr(7));
			string path;
			if (iss >> path)
			{
				materials[currentMtl].MapKd = path;
			}
		}
	}

	return materials;
}

static bool TryLoadStaticEmbeddedTextureByIndex(const aiScene* scene, const aiString& texPath, const char* modelName, int& outTexIndex)
{
	if (!scene || texPath.length == 0 || texPath.data[0] != '*')
	{
		return false;
	}

	const int texIdx = atoi(&texPath.data[1]);
	if (texIdx < 0 || texIdx >= static_cast<int>(scene->mNumTextures))
	{
		return false;
	}

	aiTexture* aiTex = scene->mTextures[texIdx];
	string uniqueName = string(modelName) + "_" + texPath.C_Str();
	outTexIndex = TextureManager::LoadTextureFromMemory(
		uniqueName.c_str(),
		reinterpret_cast<const uint8_t*>(aiTex->pcData),
		aiTex->mWidth);
	return true;
}

static bool TryLoadStaticEmbeddedTextureByName(const aiScene* scene, const aiString& texPath, const char* modelName, int& outTexIndex)
{
	if (!scene)
	{
		return false;
	}

	for (unsigned int i = 0; i < scene->mNumTextures; ++i)
	{
		if (strcmp(scene->mTextures[i]->mFilename.C_Str(), texPath.C_Str()) != 0)
		{
			continue;
		}

		aiTexture* aiTex = scene->mTextures[i];
		string uniqueName = string(modelName) + "_" + texPath.C_Str();
		outTexIndex = TextureManager::LoadTextureFromMemory(
			uniqueName.c_str(),
			reinterpret_cast<const uint8_t*>(aiTex->pcData),
			aiTex->mWidth);
		return true;
	}
	return false;
}

static int ResolveStaticMeshTextureIndex(const aiScene* scene, const aiMesh* mesh, const char* fileName, const string& dirPath)
{
	int textureIndex = TextureManager::GetDefaultTextureIndex();
	if (!scene || !mesh || mesh->mMaterialIndex >= scene->mNumMaterials)
	{
		return textureIndex;
	}

	aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];
	aiString texPath;
	const bool hasTexture =
		(material->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) == AI_SUCCESS) ||
		(material->GetTexture(aiTextureType_BASE_COLOR, 0, &texPath) == AI_SUCCESS);
	if (!hasTexture)
	{
		return textureIndex;
	}

	if (TryLoadStaticEmbeddedTextureByIndex(scene, texPath, fileName, textureIndex) ||
		TryLoadStaticEmbeddedTextureByName(scene, texPath, fileName, textureIndex))
	{
		return textureIndex;
	}

	auto toLower = [](string value)
		{
			return MaterialPartToLowerString(value);
		};
	auto isSupportedTextureExtension = [&](const filesystem::path& path)
		{
			const string ext = toLower(path.extension().string());
			return ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
				ext == ".bmp" || ext == ".tga" || ext == ".tif" || ext == ".tiff" ||
				ext == ".dds" || ext == ".sph" || ext == ".spa";
		};
	auto pushCandidate = [](vector<filesystem::path>& paths, const filesystem::path& path)
		{
			if (find(paths.begin(), paths.end(), path) == paths.end())
			{
				paths.push_back(path);
			}
		};

	filesystem::path texFilePath = ModelPathFromUtf8(texPath.C_Str());
	vector<filesystem::path> candidates;
	if (texFilePath.extension() != ".tga" && texFilePath.extension() != ".TGA")
	{
		filesystem::path tgaPath = texFilePath;
		tgaPath.replace_extension(".tga");
		pushCandidate(candidates, ModelPathFromUtf8(dirPath.c_str()) / tgaPath);
		pushCandidate(candidates, ModelPathFromUtf8(dirPath.c_str()) / tgaPath.filename());
	}
	if (isSupportedTextureExtension(texFilePath))
	{
		if (texFilePath.is_absolute())
		{
			pushCandidate(candidates, texFilePath);
		}
		pushCandidate(candidates, ModelPathFromUtf8(dirPath.c_str()) / texFilePath);
		pushCandidate(candidates, ModelPathFromUtf8(dirPath.c_str()) / texFilePath.filename());
	}

	for (const auto& candidate : candidates)
	{
		if (filesystem::exists(candidate))
		{
			return TextureManager::LoadTexture(candidate);
		}
	}

	Debug::Log("WARNING: Static model texture not found: %s\n", texPath.C_Str());
	return textureIndex;
}

bool StaticModelResource::UploadBufferData(
	ID3D12Device* device,
	ID3D12Resource* dstResource,
	const void* srcData,
	UINT sizeInBytes,
	D3D12_RESOURCE_STATES finalState,
	const char* logTag)
{
	return UploadBufferData(device, dstResource, srcData, sizeInBytes,
		D3D12_RESOURCE_STATE_COMMON, finalState, logTag);
}

bool StaticModelResource::UploadBufferData(
	ID3D12Device* device,
	ID3D12Resource* dstResource,
	const void* srcData,
	UINT sizeInBytes,
	D3D12_RESOURCE_STATES beforeState,
	D3D12_RESOURCE_STATES finalState,
	const char* logTag)
{
	if (!device || !dstResource || !srcData || sizeInBytes == 0)
	{
		Debug::Log("ERROR: Invalid upload args (%s)\n", logTag);
		return false;
	}

	D3D12_RESOURCE_DESC desc = dstResource->GetDesc();
	UINT64 uploadBufferSize = 0;
	device->GetCopyableFootprints(&desc, 0, 1, 0, nullptr, nullptr, nullptr, &uploadBufferSize);
	ComPtr<ID3D12Resource> uploadBuffer = TextureManager::AcquireUploadBuffer(device, uploadBufferSize);
	if (!uploadBuffer)
	{
		Debug::Log("ERROR: Failed to acquire upload buffer (%s)\n", logTag);
		return false;
	}

	UINT8* pDest = nullptr;
	CD3DX12_RANGE readRange(0, 0);
	HRESULT hr = uploadBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pDest));
	if (FAILED(hr) || !pDest)
	{
		TextureManager::ReleaseUploadBuffer(uploadBuffer, uploadBufferSize);
		Debug::Log("ERROR: Failed to map upload buffer (%s)\n", logTag);
		return false;
	}
	memcpy(pDest, srcData, sizeInBytes);
	uploadBuffer->Unmap(0, nullptr);

	const bool batchMode = TextureManager::IsBatchLoading();
	ComPtr<ID3D12CommandAllocator> cmdAlloc;
	ComPtr<ID3D12GraphicsCommandList> cmdList;

	if (batchMode)
	{
		cmdList = TextureManager::GetBatchCommandList();
		if (!cmdList)
		{
			TextureManager::ReleaseUploadBuffer(uploadBuffer, uploadBufferSize);
			Debug::Log("ERROR: Batch command list not initialized (%s)\n", logTag);
			return false;
		}
	}
	else
	{
		hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc));
		if (FAILED(hr))
		{
			TextureManager::ReleaseUploadBuffer(uploadBuffer, uploadBufferSize);
			Debug::Log("ERROR: CreateCommandAllocator failed (%s)\n", logTag);
			return false;
		}
		hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc.Get(), nullptr, IID_PPV_ARGS(&cmdList));
		if (FAILED(hr))
		{
			TextureManager::ReleaseUploadBuffer(uploadBuffer, uploadBufferSize);
			Debug::Log("ERROR: CreateCommandList failed (%s)\n", logTag);
			return false;
		}
	}

	D3D12_SUBRESOURCE_DATA data {};
	data.pData = srcData;
	data.RowPitch = sizeInBytes;
	data.SlicePitch = data.RowPitch;

	if (beforeState != D3D12_RESOURCE_STATE_COPY_DEST)
	{
		auto toCopyDest = CD3DX12_RESOURCE_BARRIER::Transition(dstResource,
			beforeState, D3D12_RESOURCE_STATE_COPY_DEST);
		cmdList->ResourceBarrier(1, &toCopyDest);
	}

	UpdateSubresources(cmdList.Get(), dstResource, uploadBuffer.Get(), 0, 0, 1, &data);

	if (finalState != D3D12_RESOURCE_STATE_COPY_DEST)
	{
		auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(dstResource,
			D3D12_RESOURCE_STATE_COPY_DEST, finalState);
		cmdList->ResourceBarrier(1, &barrier);
	}

	if (batchMode)
	{
		TextureManager::ReleaseUploadBuffer(uploadBuffer, uploadBufferSize, true);
	}
	else
	{
		if (!TextureManager::ExecuteCommandListAndSync(cmdList.Get()))
		{
			TextureManager::ReleaseUploadBuffer(uploadBuffer, uploadBufferSize);
			Debug::Log("ERROR: ExecuteCommandListAndSync failed (%s)\n", logTag);
			return false;
		}
		TextureManager::ReleaseUploadBuffer(uploadBuffer, uploadBufferSize);
	}

	return true;
}

bool StaticModelResource::CreateDefaultBufferAndUpload(
	ID3D12Device* device,
	UINT sizeInBytes,
	const void* srcData,
	D3D12_RESOURCE_STATES finalState,
	const char* createErrorLog,
	const char* uploadLogTag,
	ComPtr<ID3D12Resource>& outResource)
{
	if (!device || !srcData || sizeInBytes == 0)
	{
		Debug::Log("ERROR: Invalid buffer creation args (%s)\n", uploadLogTag);
		return false;
	}

	CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
	D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(sizeInBytes);
	HRESULT hr = device->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_NONE,
		&desc,
		D3D12_RESOURCE_STATE_COMMON,
		nullptr,
		IID_PPV_ARGS(&outResource));
	if (FAILED(hr) || !outResource)
	{
		Debug::Log("%s", createErrorLog);
		return false;
	}

	return UploadBufferData(device, outResource.Get(), srcData, sizeInBytes, finalState, uploadLogTag);
}

void StaticModelResource::LoadLODMetadata(const char* path)
{
	m_SourcePath = path;
    std::string error;
    const auto sourcePath = ModelPathFromUtf8(path);
    if (!VirtualGeometryImport::Load(sourcePath, m_VGSettings, error))
    {
        m_VGSettings.Enabled = false;
        Debug::Log("VirtualGeometry settings fallback: %s\n", error.c_str());
    }
    auto lodPath = sourcePath;
    lodPath += ".lod";
	ifstream file(lodPath);
	if (file && !Visibility::LoadLOD(file, m_LODMetadata)) Debug::Log("Invalid LOD metadata: %s.lod\n", path);
}

bool StaticModelResource::BuildLodIndexBuffers(
	ID3D12Device* device,
	const vector<StaticModelVertex>& vertices,
	const vector<unsigned int>& indices,
	StaticMeshData& meshData)
{
	if (!device || vertices.empty() || indices.size() < 6)
	{
		return true;
	}

	constexpr float ratios[StaticMeshData::LodCount - 1] = { 0.50f, 0.20f, 0.08f, 0.03f };
	constexpr float errors[StaticMeshData::LodCount - 1] = { 0.01f, 0.035f, 0.08f, 0.15f };
	for (UINT lod = 0; lod < StaticMeshData::LodCount - 1; ++lod)
	{
		const size_t targetCount = max<size_t>(
			3, (static_cast<size_t>(indices.size() * ratios[lod]) / 3) * 3);
		vector<unsigned int> lodIndices(indices.size());
		const size_t resultCount = meshopt_simplify(
			lodIndices.data(), indices.data(), indices.size(),
			reinterpret_cast<const float*>(vertices.data()), vertices.size(),
			sizeof(StaticModelVertex), targetCount, errors[lod],
			meshopt_SimplifyRegularize);
		if (resultCount < 3 || resultCount >= indices.size())
		{
			continue;
		}
		lodIndices.resize(resultCount);
		// Perf: LODインデックスにも頂点キャッシュ最適化を適用。
		meshopt_optimizeVertexCache(
			lodIndices.data(), lodIndices.data(), lodIndices.size(), vertices.size());
		const UINT byteSize = static_cast<UINT>(resultCount * sizeof(unsigned int));
		if (!CreateDefaultBufferAndUpload(
			device, byteSize, lodIndices.data(), D3D12_RESOURCE_STATE_INDEX_BUFFER,
			"ERROR: Failed to create static LOD index buffer\n",
			"static LOD index upload", meshData.LodIndexBuffers[lod]))
		{
			return false;
		}
		meshData.LodIndexBufferViews[lod].BufferLocation =
			meshData.LodIndexBuffers[lod]->GetGPUVirtualAddress();
		meshData.LodIndexBufferViews[lod].Format = DXGI_FORMAT_R32_UINT;
		meshData.LodIndexBufferViews[lod].SizeInBytes = byteSize;
		meshData.LodIndexCounts[lod] = static_cast<UINT>(resultCount);
		meshData.CpuLodIndices[lod].assign(lodIndices.begin(), lodIndices.end());
	}

    static const bool rayLod=ReadEnvironmentFlagOnce("DX12_DXR_LOD"),rayProxy=ReadEnvironmentFlagOnce("DX12_DXR_PROXY");
    if(rayLod||rayProxy)
    {
        constexpr float ratios[]={.65f,.3f,.1f},errors[]={.005f,.015f,.04f};
        for(UINT lod=0;lod<StaticMeshData::RayLodCount;++lod)
        {
            vector<unsigned int> simplified(indices.size());
            const size_t target=(std::max)(size_t(3),size_t(indices.size()*ratios[lod])/3*3);
            const size_t count=meshopt_simplify(simplified.data(),indices.data(),indices.size(),reinterpret_cast<const float*>(vertices.data()),vertices.size(),sizeof(StaticModelVertex),target,errors[lod],meshopt_SimplifyRegularize);
            if(count<3||count>=indices.size()) continue;
            simplified.resize(count); meshopt_optimizeVertexCache(simplified.data(),simplified.data(),count,vertices.size());
            const UINT bytes=static_cast<UINT>(count*sizeof(unsigned int));
            if(!CreateDefaultBufferAndUpload(device,bytes,simplified.data(),D3D12_RESOURCE_STATE_INDEX_BUFFER,"Ray LOD allocation failed\n","ray LOD",meshData.RayLodBuffers[lod])) continue;
            meshData.RayLodViews[lod]={meshData.RayLodBuffers[lod]->GetGPUVirtualAddress(),bytes,DXGI_FORMAT_R32_UINT}; meshData.RayLodCounts[lod]=static_cast<UINT>(count);
        }
        XMFLOAT3 lo=vertices.front().Position,hi=lo;
        for(const auto& v:vertices)
        {
            lo.x=(std::min)(lo.x,v.Position.x); lo.y=(std::min)(lo.y,v.Position.y); lo.z=(std::min)(lo.z,v.Position.z);
            hi.x=(std::max)(hi.x,v.Position.x); hi.y=(std::max)(hi.y,v.Position.y); hi.z=(std::max)(hi.z,v.Position.z);
        }
        meshData.RayCenter={(lo.x+hi.x)*.5f,(lo.y+hi.y)*.5f,(lo.z+hi.z)*.5f};
        meshData.RayRadius=.5f*sqrtf((hi.x-lo.x)*(hi.x-lo.x)+(hi.y-lo.y)*(hi.y-lo.y)+(hi.z-lo.z)*(hi.z-lo.z));
        if(rayProxy && meshData.RayRadius>1e-5f)
        {
            vector<StaticModelVertex> proxy(8);
            for(UINT corner=0;corner<8;++corner)
            {
                auto& v=proxy[corner]; v.Position={(corner&1)?hi.x:lo.x,(corner&2)?hi.y:lo.y,(corner&4)?hi.z:lo.z};
                XMStoreFloat3(&v.Normal,XMVector3Normalize(XMLoadFloat3(&v.Position)-XMLoadFloat3(&meshData.RayCenter))); v.Diffuse={1,1,1,1};
            }
            const unsigned int box[]={0,2,3,0,3,1,4,5,7,4,7,6,0,1,5,0,5,4,2,6,7,2,7,3,0,4,6,0,6,2,1,3,7,1,7,5};
            if(CreateDefaultBufferAndUpload(device,UINT(proxy.size()*sizeof(StaticModelVertex)),proxy.data(),D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,"Ray proxy allocation failed\n","ray proxy vertices",meshData.RayProxyVertices) &&
                CreateDefaultBufferAndUpload(device,sizeof(box),box,D3D12_RESOURCE_STATE_INDEX_BUFFER,"Ray proxy allocation failed\n","ray proxy indices",meshData.RayProxyIndices))
            {
                meshData.RayProxyVertexView={meshData.RayProxyVertices->GetGPUVirtualAddress(),UINT(proxy.size()*sizeof(StaticModelVertex)),sizeof(StaticModelVertex)};
                meshData.RayProxyIndexView={meshData.RayProxyIndices->GetGPUVirtualAddress(),sizeof(box),DXGI_FORMAT_R32_UINT};
            }
        }
    }
	return true;
}

bool StaticModelResource::LoadObj(const char* fileName, ID3D12Device* device)
{
    LoadLODMetadata(fileName);
    // The legacy OBJ importer combines every face into one section. Preserve its
    // single-material behavior, but retain authored material sections via Assimp.
    ifstream source(fileName);
    unordered_set<string> sectionMaterials;
    string sourceLine;
    while (getline(source, sourceLine))
    {
        if (sourceLine.rfind("usemtl ", 0) == 0)
        {
            sectionMaterials.insert(sourceLine.substr(7));
        }
    }
    if (sectionMaterials.size() > 1)
    {
        return LoadAssimpModel(fileName, device, false);
    }
	m_MaterialPath.clear();

	ifstream ifs(fileName);
	if (!ifs)
	{
		char buf[512];
		sprintf_s(buf, "ERROR: Failed to open OBJ file: %s\n", fileName);
		Debug::Log("%s", buf);
		return false;
	}

	Reserve();

	unordered_map<string, MtlData> materialMap;
	XMFLOAT4 currentKd = { 1.0f, 1.0f, 1.0f, 1.0f };

	string line;
	vector<string> tokens;
	tokens.reserve(4);
	while (getline(ifs, line))
	{
		if (line.rfind("mtllib ", 0) == 0)
		{
			istringstream iss(line.substr(7));
			string mtlName;
			iss >> mtlName;

			filesystem::path objPath(fileName);
			auto dir = objPath.parent_path();
			string mtlPath;
			if (dir.empty())
			{
				mtlPath = mtlName;
			}
			else
			{
				mtlPath = (dir / mtlName).string();
			}

			materialMap = ParseMtl(mtlPath);

			for (auto const& pair : materialMap)
			{
				const MtlData& data = pair.second;
				if (!data.MapKd.empty() && m_MaterialPath.empty())
				{
					if (dir.empty())
					{
						m_MaterialPath = data.MapKd;
					}
					else
					{
						m_MaterialPath = (dir / data.MapKd).string();
					}
				}
			}
		}
		else if (line.rfind("usemtl ", 0) == 0)
		{
			istringstream iss(line.substr(7));
			string mtlName;
			if (iss >> mtlName)
			{
				auto it = materialMap.find(mtlName);
				if (it != materialMap.end())
				{
					currentKd = it->second.Kd;
				}
			}
		}
		else if (line.rfind("v ", 0) == 0)
		{
			XMFLOAT3 pos{};
			istringstream iss(line.substr(2));
			iss >> pos.x >> pos.y >> pos.z;
			m_Positions.push_back(pos);
		}
		else if (line.rfind("vt ", 0) == 0)
		{
			XMFLOAT2 tex{};
			istringstream iss(line.substr(3));
			iss >> tex.x >> tex.y;
			tex.y = 1.0f - tex.y;
			m_TexCoords.push_back(tex);
		}
		else if (line.rfind("vn ", 0) == 0)
		{
			XMFLOAT3 normal{};
			istringstream iss(line.substr(3));
			iss >> normal.x >> normal.y >> normal.z;
			m_Normals.push_back(normal);
		}
		else if (line.rfind("f ", 0) == 0)
		{
			istringstream iss(line.substr(2));
			string tok;
			tokens.clear();
			while (iss >> tok && tokens.size() < 4) tokens.push_back(tok);

			unsigned int vi[4] = {}, ti[4] = {}, ni[4] {};
			int count = 0;
			for (auto& t : tokens)
			{
				if (sscanf_s(t.c_str(), "%u/%u/%u", &vi[count], &ti[count], &ni[count]) == 3) {}
				else if (sscanf_s(t.c_str(), "%u//%u", &vi[count], &ni[count]) == 2) { ti[count] = 0; }
				else if (sscanf_s(t.c_str(), "%u/%u", &vi[count], &ti[count]) == 2) {}
				else if (sscanf_s(t.c_str(), "%u", &vi[count]) == 1) {}
				count++;
			}

			for (int i = 0; i < count - 2; ++i)
			{
				int faceIndices[3] = { 0, i + 1, i + 2 };
				for (int j = 0; j < 3; ++j)
				{
					int idx = faceIndices[j];
					StaticModelVertex vertex {};

					if (vi[idx] > 0 && vi[idx] <= (unsigned int)m_Positions.size())
						vertex.Position = m_Positions[vi[idx] - 1];
					if (ni[idx] > 0 && ni[idx] <= (unsigned int)m_Normals.size())
						vertex.Normal = m_Normals[ni[idx] - 1];
					if (ti[idx] > 0 && ti[idx] <= (unsigned int)m_TexCoords.size())
						vertex.TexCoord = m_TexCoords[ti[idx] - 1];

					vertex.Diffuse = currentKd;
					m_Indices.push_back((unsigned int)m_Vertices.size());
					m_Vertices.push_back(vertex);
				}
			}
		}
	}

	if (m_Vertices.empty())
	{
		Debug::Log("ERROR: OBJ file has no vertices\n");
		return false;
	}

	XMFLOAT3 minPos = { FLT_MAX, FLT_MAX, FLT_MAX };
	XMFLOAT3 maxPos = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
	for (const auto& v : m_Vertices)
	{
		if (v.Position.x < minPos.x) minPos.x = v.Position.x;
		if (v.Position.y < minPos.y) minPos.y = v.Position.y;
		if (v.Position.z < minPos.z) minPos.z = v.Position.z;

		if (v.Position.x > maxPos.x) maxPos.x = v.Position.x;
		if (v.Position.y > maxPos.y) maxPos.y = v.Position.y;
		if (v.Position.z > maxPos.z) maxPos.z = v.Position.z;
	}

	m_AabbCenter.x = (minPos.x + maxPos.x) * 0.5f;
	m_AabbCenter.y = (minPos.y + maxPos.y) * 0.5f;
	m_AabbCenter.z = (minPos.z + maxPos.z) * 0.5f;

	m_AabbExtents.x = (maxPos.x - minPos.x) * 0.5f;
	m_AabbExtents.y = (maxPos.y - minPos.y) * 0.5f;
	m_AabbExtents.z = (maxPos.z - minPos.z) * 0.5f;

	StaticMeshData meshData {};
	meshData.MeshName = "OBJ Mesh";
	meshData.MaterialName = "";
	meshData.MaterialPartId = 10.0f;
	meshData.AppliedMaterialPartId = meshData.MaterialPartId;
	meshData.DefaultToonOutlineEnabled = true;
	meshData.CpuVertices = m_Vertices;

	const UINT vertexBufferSize = sizeof(StaticModelVertex) * (UINT)m_Vertices.size();
	{
		D3D12_RESOURCE_DESC vbDesc = CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize);
		CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
		HRESULT hr = device->CreateCommittedResource(
			&heapProps,
			D3D12_HEAP_FLAG_NONE,
			&vbDesc,
			D3D12_RESOURCE_STATE_COMMON,
			nullptr,
			IID_PPV_ARGS(&meshData.VertexBuffer));
		if (FAILED(hr) || !meshData.VertexBuffer)
		{
			Debug::Log("ERROR: Failed to create DEFAULT vertex buffer for static model\n");
			return false;
		}

		if (!UploadBufferData(device, meshData.VertexBuffer.Get(), m_Vertices.data(), vertexBufferSize,
			D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, "static vertex"))
		{
			return false;
		}
	}

	meshData.VertexBufferView.BufferLocation = meshData.VertexBuffer->GetGPUVirtualAddress();
	meshData.VertexBufferView.StrideInBytes = sizeof(StaticModelVertex);
	meshData.VertexBufferView.SizeInBytes = vertexBufferSize;
	meshData.VertexCount = (UINT)m_Vertices.size();

	// Perf: OBJパスも Assimp パスと同様にインデックス順を最適化。
	if (!m_Indices.empty() && !m_Vertices.empty())
	{
		meshopt_optimizeVertexCache(
			m_Indices.data(), m_Indices.data(), m_Indices.size(), m_Vertices.size());
		meshopt_optimizeOverdraw(
			m_Indices.data(), m_Indices.data(), m_Indices.size(),
			reinterpret_cast<const float*>(m_Vertices.data()),
			m_Vertices.size(), sizeof(StaticModelVertex), 1.05f);
	}

	const UINT indexBufferSize = sizeof(unsigned int) * (UINT)m_Indices.size();
	{
		D3D12_RESOURCE_DESC ibDesc = CD3DX12_RESOURCE_DESC::Buffer(indexBufferSize);
		CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
		HRESULT hr = device->CreateCommittedResource(
			&heapProps,
			D3D12_HEAP_FLAG_NONE,
			&ibDesc,
			D3D12_RESOURCE_STATE_COMMON,
			nullptr,
			IID_PPV_ARGS(&meshData.IndexBuffer));
		if (FAILED(hr) || !meshData.IndexBuffer)
		{
			Debug::Log("ERROR: Failed to create DEFAULT index buffer for static model\n");
			return false;
		}

		if (!UploadBufferData(device, meshData.IndexBuffer.Get(), m_Indices.data(), indexBufferSize,
			D3D12_RESOURCE_STATE_INDEX_BUFFER, "static index"))
		{
			return false;
		}
	}

	meshData.IndexBufferView.BufferLocation = meshData.IndexBuffer->GetGPUVirtualAddress();
	meshData.IndexBufferView.Format = DXGI_FORMAT_R32_UINT;
	meshData.IndexBufferView.SizeInBytes = indexBufferSize;
	meshData.IndexCount = (UINT)m_Indices.size();
	meshData.CpuIndices.assign(m_Indices.begin(), m_Indices.end());

	{
		for (int mode = 0; mode < kToonOutlineModeCount; ++mode)
		{
			vector<StaticModelVertex> teoVertices;
			vector<unsigned int> teoIndices;
			BuildTeoMesh(
				m_Vertices,
				m_Indices,
				teoVertices,
				teoIndices,
				static_cast<ToonOutlineMeshMode>(mode));
			if (teoVertices.empty() || teoIndices.empty())
			{
				continue;
			}
			meshData.CpuTeoVerticesByMode[mode] = teoVertices;

			const UINT teoVertexBufferSize = sizeof(StaticModelVertex) * (UINT)teoVertices.size();
			if (!CreateDefaultBufferAndUpload(device, teoVertexBufferSize, teoVertices.data(),
				D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
				"ERROR: Failed to create DEFAULT TEO vertex buffer for static model\n",
				"static teo vertex", meshData.TeoVertexBuffers[mode]))
			{
				return false;
			}
			meshData.TeoVertexBufferViews[mode].BufferLocation = meshData.TeoVertexBuffers[mode]->GetGPUVirtualAddress();
			meshData.TeoVertexBufferViews[mode].StrideInBytes = sizeof(StaticModelVertex);
			meshData.TeoVertexBufferViews[mode].SizeInBytes = teoVertexBufferSize;
			meshData.TeoVertexCounts[mode] = (UINT)teoVertices.size();

			const UINT teoIndexBufferSize = sizeof(unsigned int) * (UINT)teoIndices.size();
			if (!CreateDefaultBufferAndUpload(device, teoIndexBufferSize, teoIndices.data(),
				D3D12_RESOURCE_STATE_INDEX_BUFFER,
				"ERROR: Failed to create DEFAULT TEO index buffer for static model\n",
				"static teo index", meshData.TeoIndexBuffers[mode]))
			{
				return false;
			}
			meshData.TeoIndexBufferViews[mode].BufferLocation = meshData.TeoIndexBuffers[mode]->GetGPUVirtualAddress();
			meshData.TeoIndexBufferViews[mode].Format = DXGI_FORMAT_R32_UINT;
			meshData.TeoIndexBufferViews[mode].SizeInBytes = teoIndexBufferSize;
			meshData.TeoIndexCounts[mode] = (UINT)teoIndices.size();

			if (mode == static_cast<int>(ToonOutlineMeshMode::Balanced))
			{
				meshData.TeoVertexBuffer = meshData.TeoVertexBuffers[mode];
				meshData.TeoIndexBuffer = meshData.TeoIndexBuffers[mode];
				meshData.TeoVertexBufferView = meshData.TeoVertexBufferViews[mode];
				meshData.TeoIndexBufferView = meshData.TeoIndexBufferViews[mode];
				meshData.TeoVertexCount = meshData.TeoVertexCounts[mode];
				meshData.TeoIndexCount = meshData.TeoIndexCounts[mode];
			}
		}
	}

	if (!m_MaterialPath.empty())
	{
		meshData.TextureIndex = TextureManager::LoadTexture(m_MaterialPath.c_str());
	}
	if (!BuildLodIndexBuffers(device, m_Vertices, m_Indices, meshData))
	{
		return false;
	}

    meshData.VirtualGeometrySettings = m_VGSettings;
	MeshletManager::BuildImportedMesh(meshData);
	m_Meshes.push_back(move(meshData));

	char buf[512];
	sprintf_s(buf, "Static model loaded: %s (vertices=%zu, indices=%zu)\n",
		fileName, m_Vertices.size(), m_Indices.size());
	Debug::Log("%s", buf);

	return true;
}

bool StaticModelResource::LoadVirtualGeometry(const char* fileName, ID3D12Device* device)
{
    try
    {
        LoadLODMetadata(fileName);
        auto manifest = std::make_shared<VirtualGeometryPageCache::PageManifest>();
        std::string error;
        if (!device || !VirtualGeometryPageCache::Open(ModelPathFromUtf8(fileName), *manifest, error))
        {
            Debug::Log("VG import failed: %s\n", error.c_str());
            return false;
        }
        manifest->StreamingPriority = m_VGSettings.StreamingPriority;
        StaticMeshData mesh;
        mesh.VirtualGeometryRootFallback = true;
        mesh.MeshName = ModelPathToUtf8(ModelPathFromUtf8(fileName).stem());
        mesh.VirtualGeometrySettings = m_VGSettings;
        mesh.AppliedMaterialPartId = float(ShaderClass::PBR);
        mesh.MaterialPartId = mesh.AppliedMaterialPartId;
        // Only pinned roots are expanded for unsupported-device/shadow/picking fallback.
        // Fine geometry remains encoded on disk and is never uploaded as a full vertex buffer.
        std::vector<uint8_t> page(VirtualGeometryPages::PageBytes);
        uint32_t lastPage = UINT32_MAX;
        XMFLOAT3 lower{INFINITY,INFINITY,INFINITY}, upper{-INFINITY,-INFINITY,-INFINITY};
        for (const auto root : manifest->Metadata.Roots)
        {
            const auto& cluster = manifest->Metadata.Clusters.at(root);
            if (cluster.PageIndex != lastPage)
            {
                if (!VirtualGeometryPageCache::ReadPage(*manifest, cluster.PageIndex, page, error))
                {
                    Debug::Log("VG root fallback page failed: %s\n", error.c_str());
                    return false;
                }
                lastPage = cluster.PageIndex;
            }
            if (mesh.CpuVertices.size()+cluster.VertexCount > UINT32_MAX ||
                mesh.CpuIndices.size()+uint64_t(cluster.TriangleCount)*3 > UINT32_MAX)
            {
                return false;
            }
            const uint32_t first = static_cast<uint32_t>(mesh.CpuVertices.size());
            const bool encoded = (cluster.Flags & VirtualGeometryCompression::ClusterFlag) != 0;
            const auto payload = std::span<const uint8_t>(page).subspan(cluster.VertexOffset);
            for (uint32_t index = 0; index < cluster.VertexCount; ++index)
            {
                StaticModelVertex vertex;
                if (encoded)
                {
                    const auto decoded = VirtualGeometryCompression::DecodeVertexCPU(payload, index);
                    vertex.Position = {decoded.Position[0],decoded.Position[1],decoded.Position[2]};
                    vertex.Normal = {decoded.Normal[0],decoded.Normal[1],decoded.Normal[2]};
                    if (!decoded.Texcoords.empty()) vertex.TexCoord = {decoded.Texcoords[0][0],decoded.Texcoords[0][1]};
                    vertex.Diffuse = {decoded.Color[0],decoded.Color[1],decoded.Color[2],decoded.Color[3]};
                }
                else
                {
                    static_assert(sizeof(StaticModelVertex) == sizeof(VirtualGeometryVertex));
                    memcpy(&vertex, payload.data()+size_t(index)*sizeof(StaticModelVertex), sizeof(vertex));
                }
                mesh.CpuVertices.push_back(vertex);
            }
            for (uint32_t triangle = 0; triangle < cluster.TriangleCount; ++triangle)
            {
                std::array<uint32_t,3> indices;
                if (encoded)
                {
                    indices = VirtualGeometryCompression::LoadTriangleCPU(payload, triangle);
                }
                else
                {
                    const size_t offset = size_t(cluster.TriangleOffset)+size_t(triangle)*3;
                    indices = {page.at(offset),page.at(offset+1),page.at(offset+2)};
                }
                for (const auto index : indices)
                {
                    if (index >= cluster.VertexCount) return false;
                    mesh.CpuIndices.push_back(first+index);
                }
            }
            lower.x = std::min(lower.x, cluster.Bounds[0]-cluster.Bounds[3]);
            lower.y = std::min(lower.y, cluster.Bounds[1]-cluster.Bounds[3]);
            lower.z = std::min(lower.z, cluster.Bounds[2]-cluster.Bounds[3]);
            upper.x = std::max(upper.x, cluster.Bounds[0]+cluster.Bounds[3]);
            upper.y = std::max(upper.y, cluster.Bounds[1]+cluster.Bounds[3]);
            upper.z = std::max(upper.z, cluster.Bounds[2]+cluster.Bounds[3]);
        }
        if (mesh.CpuVertices.empty() || mesh.CpuIndices.empty() ||
            mesh.CpuVertices.size() > UINT32_MAX/sizeof(StaticModelVertex) || mesh.CpuIndices.size() > UINT32_MAX/4)
        {
            return false;
        }
        mesh.VertexCount = static_cast<UINT>(mesh.CpuVertices.size());
        mesh.IndexCount = static_cast<UINT>(mesh.CpuIndices.size());
        const UINT vertexBytes = mesh.VertexCount*sizeof(StaticModelVertex), indexBytes = mesh.IndexCount*4;
        if (!CreateDefaultBufferAndUpload(device, vertexBytes, mesh.CpuVertices.data(),
                D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, "VG root vertex allocation failed", "VG root vertex", mesh.VertexBuffer) ||
            !CreateDefaultBufferAndUpload(device, indexBytes, mesh.CpuIndices.data(),
                D3D12_RESOURCE_STATE_INDEX_BUFFER, "VG root index allocation failed", "VG root index", mesh.IndexBuffer))
        {
            return false;
        }
        mesh.VertexBufferView = {mesh.VertexBuffer->GetGPUVirtualAddress(),vertexBytes,sizeof(StaticModelVertex)};
        mesh.IndexBufferView = {mesh.IndexBuffer->GetGPUVirtualAddress(),indexBytes,DXGI_FORMAT_R32_UINT};
        auto runtime = std::make_shared<VirtualGeometryRuntime::RuntimeAsset>();
        runtime->Key = manifest->Identity;
        runtime->CachePath = manifest->Path;
        runtime->ImportSettings = m_VGSettings;
        runtime->Hierarchy.Stats.SourceTriangles = manifest->SourceTriangles;
        runtime->Hierarchy.Stats.SourceVertices = manifest->SourceVertices;
        runtime->Hierarchy.Stats.TotalClusters = static_cast<uint32_t>(manifest->Metadata.Clusters.size());
        runtime->Hierarchy.Stats.TotalGroups = static_cast<uint32_t>(manifest->Metadata.Groups.size());
        for (const auto& cluster : manifest->Metadata.Clusters)
        {
            runtime->Hierarchy.Stats.HierarchyLevels = std::max(runtime->Hierarchy.Stats.HierarchyLevels,cluster.Level+1);
        }
        runtime->Pages = std::move(manifest);
        mesh.VirtualGeometryLODs[0] = std::move(runtime);
        m_AabbCenter = {(lower.x+upper.x)*0.5f,(lower.y+upper.y)*0.5f,(lower.z+upper.z)*0.5f};
        m_AabbExtents = {(upper.x-lower.x)*0.5f,(upper.y-lower.y)*0.5f,(upper.z-lower.z)*0.5f};
        m_Meshes.clear();
        m_Meshes.push_back(std::move(mesh));
        return true;
    }
    catch (const std::exception& error)
    {
        Debug::Log("VG import failed safely: %s\n", error.what());
        return false;
    }
}

namespace
{
uint64_t HashSourceFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return 0;
    uint64_t hash = 14695981039346656037ull;
    std::array<char,65536> bytes;
    while (input)
    {
        input.read(bytes.data(), bytes.size());
        for (std::streamsize index = 0; index < input.gcount(); ++index)
        {
            hash = (hash ^ static_cast<unsigned char>(bytes[index]))*1099511628211ull;
        }
    }
    return input.bad() ? 0 : hash;
}
std::vector<std::filesystem::path> GeometryDependencies(const std::filesystem::path& source)
{
    std::vector<std::filesystem::path> paths{source};
    std::string extension = source.extension().string();
    std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char letter)
    {
        return static_cast<char>(std::tolower(letter));
    });
    if (extension == ".gltf" || extension == ".glb")
    {
        std::ifstream input(source,std::ios::binary);
        Json::Value document;
        Json::CharReaderBuilder reader;
        std::string error;
        if (extension == ".glb")
        {
            std::array<uint32_t,5> header{};
            input.read(reinterpret_cast<char*>(header.data()),sizeof(header));
            if (!input || header[0] != 0x46546c67u || header[1] != 2 || header[4] != 0x4e4f534au ||
                header[3] > 16u*1024*1024 || header[2] < sizeof(header) || header[3] > header[2]-sizeof(header)) return {};
            std::string json(header[3],'\0');
            if (!input.read(json.data(),json.size())) return {};
            std::istringstream jsonStream(json);
            if (!Json::parseFromStream(reader,jsonStream,&document,&error)) return {};
        }
        else if (!Json::parseFromStream(reader, input, &document, &error)) return {};
        for (const auto& buffer : document["buffers"])
        {
            if (extension == ".glb" && !buffer.isMember("uri")) continue;
            const auto uri = buffer["uri"].asString();
            if (uri.rfind("data:",0) != 0)
            {
                // URI escapes require the importer; skip the fast cache rather than miss a dependency.
                if (uri.empty() || uri.find('%') != std::string::npos) return {};
                paths.push_back(source.parent_path()/std::filesystem::u8path(uri));
            }
        }
    }
    if (extension == ".obj")
    {
        std::ifstream input(source);
        std::string line;
        while (std::getline(input,line))
        {
            if (line.rfind("mtllib ",0) == 0)
            {
                paths.push_back(source.parent_path()/line.substr(7));
            }
        }
    }
    return paths;
}
}
bool StaticModelResource::LoadDerivedVirtualGeometry(const char* fileName, ID3D12Device* device, bool convert)
{
    try
    {
        LoadLODMetadata(fileName);
        if (!m_VGSettings.Enabled || GetGeometryRenderingMode() == GeometryRenderingMode::Legacy) return false;
        char compressionOverride[8]{};
        if (GetEnvironmentVariableA("DX12_VG_COMPRESSION_RAW",compressionOverride,sizeof(compressionOverride))) return false;
        auto indexPath = ModelPathFromUtf8(fileName);
        indexPath += ".vgderived";
        std::ifstream input(indexPath);
        Json::Value index;
        Json::CharReaderBuilder reader;
        std::string error;
        if (!Json::parseFromStream(reader,input,&index,&error) || index["builder"].asUInt() != VirtualGeometryConfig::BuilderVersion ||
            index["compression"].asUInt() != VirtualGeometryCompression::CompressionVersion || index["convert"].asBool() != convert ||
            index["settings"].asString() != std::to_string(VirtualGeometryImport::SettingsKey(m_VGSettings))) return false;
        const auto dependencies = GeometryDependencies(ModelPathFromUtf8(fileName));
        if (dependencies.empty() || index["dependencies"].size() != dependencies.size()) return false;
        for (Json::ArrayIndex entry = 0; entry < dependencies.size(); ++entry)
        {
            const auto hash = HashSourceFile(dependencies[entry]);
            if (!hash || index["dependencies"][entry].asString() != std::to_string(hash)) return false;
        }
        if (!index["meshes"].isArray() || index["meshes"].empty() || index["meshes"].size() > 65536) return false;
        const std::string originalSource = fileName;
        const auto settings = m_VGSettings;
        std::vector<StaticMeshData> meshes;
        XMFLOAT3 lower{INFINITY,INFINITY,INFINITY}, upper{-INFINITY,-INFINITY,-INFINITY};
        for (const auto& entry : index["meshes"])
        {
            const std::filesystem::path path = std::filesystem::u8path(entry["path"].asString());
            const auto cache = std::filesystem::weakly_canonical("asset/virtualgeometry-cache");
            const auto relative = std::filesystem::weakly_canonical(path).lexically_relative(cache);
            if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") return false;
            if (!LoadVirtualGeometry(ModelPathToUtf8(path).c_str(),device)) return false;
            auto mesh = std::move(m_Meshes.front());
            mesh.MeshName = entry["name"].asString();
            mesh.MaterialName = entry["material"].asString();
            mesh.MaterialPartId = entry["part"].asFloat();
            mesh.AppliedMaterialPartId = mesh.MaterialPartId;
            mesh.VirtualGeometrySettings = settings;
            auto runtime = std::make_shared<VirtualGeometryRuntime::RuntimeAsset>(*mesh.VirtualGeometryLODs[0]);
            runtime->ImportSettings = settings;
            mesh.VirtualGeometryLODs[0] = std::move(runtime);
            if (!std::isfinite(mesh.MaterialPartId)) return false;
            const auto texture = entry["texture"].asString();
            if (!texture.empty()) mesh.TextureIndex = TextureManager::LoadTexture(std::filesystem::u8path(texture));
            if (m_MaterialPath.empty() && !texture.empty()) m_MaterialPath = texture;
            meshes.push_back(std::move(mesh));
            lower.x = std::min(lower.x,m_AabbCenter.x-m_AabbExtents.x);
            lower.y = std::min(lower.y,m_AabbCenter.y-m_AabbExtents.y);
            lower.z = std::min(lower.z,m_AabbCenter.z-m_AabbExtents.z);
            upper.x = std::max(upper.x,m_AabbCenter.x+m_AabbExtents.x);
            upper.y = std::max(upper.y,m_AabbCenter.y+m_AabbExtents.y);
            upper.z = std::max(upper.z,m_AabbCenter.z+m_AabbExtents.z);
        }
        m_Meshes = std::move(meshes);
        m_AabbCenter = {(lower.x+upper.x)*0.5f,(lower.y+upper.y)*0.5f,(lower.z+upper.z)*0.5f};
        m_AabbExtents = {(upper.x-lower.x)*0.5f,(upper.y-lower.y)*0.5f,(upper.z-lower.z)*0.5f};
        LoadLODMetadata(originalSource.c_str());
        m_UsesDerivedVG = true;
        m_DerivedVGPublished = true;
        CaptureSourceIdentity(ModelPathFromUtf8(originalSource.c_str()));
        return true;
    }
    catch (const std::exception&)
    {
        return false; // Any invalid cache falls through to the source importer.
    }
}
void StaticModelResource::PublishDerivedVirtualGeometry(bool convert)
{
    if (m_DerivedVGPublished || m_Meshes.empty() || m_SourcePath.empty() ||
        std::filesystem::path(m_SourcePath).extension() == ".vgmesh" ||
        std::filesystem::path(m_SourcePath).extension() == ".vgp2") return;
    try
    {
        for (const auto& mesh : m_Meshes)
        {
            const auto& runtime = mesh.VirtualGeometryLODs[0];
            if (mesh.MaterialPartId != mesh.AppliedMaterialPartId) return;
            if (!runtime || (!runtime->Pages && !VirtualGeometryResources::ManifestFor(runtime.get()))) return;
        }
        Json::Value index;
        index["builder"] = VirtualGeometryConfig::BuilderVersion;
        index["compression"] = VirtualGeometryCompression::CompressionVersion;
        index["convert"] = convert;
        index["settings"] = std::to_string(VirtualGeometryImport::SettingsKey(m_VGSettings));
        const auto dependencies = GeometryDependencies(ModelPathFromUtf8(m_SourcePath.c_str()));
        if (dependencies.empty()) return;
        if (dependencies != m_VGDependencies || dependencies.size() != m_VGDependencyHashes.size()) return;
        for (size_t dependency = 0; dependency < dependencies.size(); ++dependency)
        {
            const uint64_t hash = HashSourceFile(dependencies[dependency]);
            if (!hash || hash != m_VGDependencyHashes[dependency]) return;
            index["dependencies"].append(std::to_string(hash));
        }
        const auto textures = TextureManager::GetLoadedTextureInfos();
        for (const auto& mesh : m_Meshes)
        {
            const auto& runtime = mesh.VirtualGeometryLODs[0];
            const auto manifest = runtime ? (runtime->Pages ? runtime->Pages : VirtualGeometryResources::ManifestFor(runtime.get())) : nullptr;
            if (!manifest) return;
            std::string registryError;
            MaterialManager::Registry().Register(manifest->Path,AssetType::Mesh,registryError);
            Json::Value entry;
            entry["path"] = manifest->Path.generic_string();
            entry["name"] = mesh.MeshName;
            entry["material"] = mesh.MaterialName;
            entry["part"] = mesh.MaterialPartId;
            if (mesh.TextureIndex >= 0)
            {
                bool found = false;
                for (const auto& texture : textures)
                {
                    if (texture.SrvIndex == mesh.TextureIndex && std::filesystem::is_regular_file(std::filesystem::u8path(texture.Path)))
                    {
                        entry["texture"] = texture.Path;
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    m_DerivedVGPublished = true; // Embedded textures need the original importer.
                    return;
                }
            }
            index["meshes"].append(entry);
        }
        auto path = ModelPathFromUtf8(m_SourcePath.c_str());
        path += ".vgderived";
        auto temporary = path;
        temporary += ".tmp";
        {
            std::ofstream output(temporary);
            output << index;
            output.flush();
            if (!output) return;
        }
        m_DerivedVGPublished = MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    }
    catch (const std::exception& error)
    {
        Debug::Log("VG source index publication skipped: %s\n",error.what());
    }
}
void StaticModelResource::CaptureSourceIdentity(const std::filesystem::path& path)
{
    try
    {
        m_VGDependencies = GeometryDependencies(path);
    }
    catch (const std::exception&)
    {
        m_VGDependencies.clear(); // Invalid dependency metadata must reach the normal importer as an error.
    }
    m_VGDependencyHashes.clear();
    m_VGDependencyTimes.clear();
    for (const auto& dependency : m_VGDependencies)
    {
        m_VGDependencyHashes.push_back(HashSourceFile(dependency));
        std::error_code ec;
        m_VGDependencyTimes.push_back(std::filesystem::last_write_time(dependency,ec));
    }
}
bool StaticModelResource::SourceDependenciesChanged() const
{
    for (size_t index = 0; index < m_VGDependencies.size(); ++index)
    {
        std::error_code ec;
        if (std::filesystem::last_write_time(m_VGDependencies[index],ec) != m_VGDependencyTimes[index]) return true;
    }
    return false;
}

bool StaticModelResource::LoadAssimpModel(const char* fileName, ID3D12Device* device, bool isConvert)
{
    LoadLODMetadata(fileName);
	if (!device)
	{
		Debug::Log("ERROR: Invalid device for static model: %s\n", fileName);
		return false;
	}

	m_Meshes.clear();
	m_MaterialPath.clear();
	Clear();

	const filesystem::path modelPath = ModelPathFromUtf8(fileName);
	const bool isPmxModel = ModelPathLowerExtension(modelPath) == ".pmx";
	PmxModel pmxModel{};
	vector<vector<uint32_t>> pmxMeshVertexIndices{};
	bool ownsGeneratedScene = false;
	const aiScene* scene = nullptr;
	if (isPmxModel)
	{
		if (!LoadPmxModel(fileName, pmxModel))
		{
			return false;
		}
		scene = CreatePmxGeneratedScene(pmxModel, pmxMeshVertexIndices);
		ownsGeneratedScene = true;
	}
	else
	{
		unsigned int flags =
			aiProcess_Triangulate |
			aiProcess_JoinIdenticalVertices |
			aiProcess_ImproveCacheLocality |
			aiProcess_GenSmoothNormals;
		if (isConvert)
		{
			flags |= aiProcess_ConvertToLeftHanded;
		}
		scene = ImportModelScene(fileName, flags);
	}
	auto releaseScene = [&]()
		{
			if (!scene)
			{
				return;
			}
			if (ownsGeneratedScene)
			{
				DestroyPmxGeneratedScene(const_cast<aiScene*>(scene));
			}
			else
			{
				aiReleaseImport(scene);
			}
			scene = nullptr;
		};
	if (!scene || !scene->HasMeshes())
	{
		Debug::Log("ERROR: Failed to load static model: %s (%s)\n", fileName, aiGetErrorString());
		releaseScene();
		return false;
	}
	if (!isPmxModel)
	{
		LogAssimpModelInfo(scene, fileName, "Static");
	}
	const string dirPath = ModelPathToUtf8(modelPath.parent_path());

	XMFLOAT3 minPos = { FLT_MAX, FLT_MAX, FLT_MAX };
	XMFLOAT3 maxPos = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
	size_t totalVertices = 0;
	size_t totalIndices = 0;

	for (unsigned int m = 0; m < scene->mNumMeshes; ++m)
	{
		aiMesh* mesh = scene->mMeshes[m];
		if (!mesh || mesh->mNumVertices == 0 || mesh->mNumFaces == 0)
		{
			continue;
		}

		aiColor3D diffuse(1.0f, 1.0f, 1.0f);
		float opacity = 1.0f;
		aiString materialName;
		if (mesh->mMaterialIndex < scene->mNumMaterials)
		{
			aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];
			material->Get(AI_MATKEY_NAME, materialName);
			material->Get(AI_MATKEY_COLOR_DIFFUSE, diffuse);
			material->Get(AI_MATKEY_OPACITY, opacity);
		}
		const float materialPartId = ResolveMaterialPartId(scene, mesh);
		const XMFLOAT4 meshDiffuse(diffuse.r, diffuse.g, diffuse.b, materialPartId);

		vector<StaticModelVertex> vertices(mesh->mNumVertices);
		for (unsigned int v = 0; v < mesh->mNumVertices; ++v)
		{
			StaticModelVertex& vertex = vertices[v];
			vertex.Position = XMFLOAT3(mesh->mVertices[v].x, mesh->mVertices[v].y, mesh->mVertices[v].z);
			if (mesh->HasNormals())
			{
				vertex.Normal = XMFLOAT3(mesh->mNormals[v].x, mesh->mNormals[v].y, mesh->mNormals[v].z);
			}
			else
			{
				vertex.Normal = XMFLOAT3(0.0f, 1.0f, 0.0f);
			}

			const auto texCoords = mesh->mTextureCoords[0];
			if (texCoords)
			{
				vertex.TexCoord = XMFLOAT2(texCoords[v].x, texCoords[v].y);
			}
			else
			{
				vertex.TexCoord = XMFLOAT2(0.0f, 0.0f);
			}
			vertex.Diffuse = meshDiffuse;

			const XMFLOAT3& pos = vertex.Position;
			if (pos.x < minPos.x) minPos.x = pos.x;
			if (pos.y < minPos.y) minPos.y = pos.y;
			if (pos.z < minPos.z) minPos.z = pos.z;
			if (pos.x > maxPos.x) maxPos.x = pos.x;
			if (pos.y > maxPos.y) maxPos.y = pos.y;
			if (pos.z > maxPos.z) maxPos.z = pos.z;
		}

		vector<unsigned int> indices;
		indices.reserve(mesh->mNumFaces * 3);
		for (unsigned int f = 0; f < mesh->mNumFaces; ++f)
		{
			const aiFace& face = mesh->mFaces[f];
			if (face.mNumIndices != 3)
			{
				continue;
			}
			indices.push_back(face.mIndices[0]);
			indices.push_back(face.mIndices[1]);
			indices.push_back(face.mIndices[2]);
		}

		if (indices.empty())
		{
			continue;
		}

		// Perf: GPU頂点キャッシュ効率とオーバードロー削減のためインデックス順を最適化。
		// 頂点自体は並べ替えないので描画結果は不変。
		if (!vertices.empty())
		{
			meshopt_optimizeVertexCache(
				indices.data(), indices.data(), indices.size(), vertices.size());
			meshopt_optimizeOverdraw(
				indices.data(), indices.data(), indices.size(),
				reinterpret_cast<const float*>(vertices.data()),
				vertices.size(), sizeof(StaticModelVertex), 1.05f);
		}

		StaticMeshData meshData {};
		meshData.MeshName = mesh->mName.C_Str();
		meshData.MaterialName = materialName.C_Str();
		meshData.MaterialPartId = materialPartId;
		meshData.AppliedMaterialPartId = meshData.MaterialPartId;
		meshData.DefaultToonOutlineEnabled = materialPartId != 3.0f;
		meshData.CpuVertices = vertices;
		meshData.CpuIndices.assign(indices.begin(), indices.end());
		const UINT vertexBufferSize = sizeof(StaticModelVertex) * (UINT)vertices.size();
		auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		auto vbDesc = CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize);
		HRESULT hr = device->CreateCommittedResource(
			&heapProps,
			D3D12_HEAP_FLAG_NONE,
			&vbDesc,
			D3D12_RESOURCE_STATE_COMMON,
			nullptr,
			IID_PPV_ARGS(&meshData.VertexBuffer));
		if (FAILED(hr) || !meshData.VertexBuffer)
		{
			Debug::Log("ERROR: Failed to create DEFAULT vertex buffer for static model\n");
			releaseScene();
			return false;
		}
		if (!UploadBufferData(device, meshData.VertexBuffer.Get(), vertices.data(), vertexBufferSize,
			D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, "static fbx vertex"))
		{
			releaseScene();
			return false;
		}

		meshData.VertexBufferView.BufferLocation = meshData.VertexBuffer->GetGPUVirtualAddress();
		meshData.VertexBufferView.StrideInBytes = sizeof(StaticModelVertex);
		meshData.VertexBufferView.SizeInBytes = vertexBufferSize;
		meshData.VertexCount = (UINT)vertices.size();

		const UINT indexBufferSize = sizeof(unsigned int) * (UINT)indices.size();
		auto ibDesc = CD3DX12_RESOURCE_DESC::Buffer(indexBufferSize);
		hr = device->CreateCommittedResource(
			&heapProps,
			D3D12_HEAP_FLAG_NONE,
			&ibDesc,
			D3D12_RESOURCE_STATE_COMMON,
			nullptr,
			IID_PPV_ARGS(&meshData.IndexBuffer));
		if (FAILED(hr) || !meshData.IndexBuffer)
		{
			Debug::Log("ERROR: Failed to create DEFAULT index buffer for static model\n");
			releaseScene();
			return false;
		}
		if (!UploadBufferData(device, meshData.IndexBuffer.Get(), indices.data(), indexBufferSize,
			D3D12_RESOURCE_STATE_INDEX_BUFFER, "static fbx index"))
		{
			releaseScene();
			return false;
		}

		meshData.IndexBufferView.BufferLocation = meshData.IndexBuffer->GetGPUVirtualAddress();
		meshData.IndexBufferView.Format = DXGI_FORMAT_R32_UINT;
		meshData.IndexBufferView.SizeInBytes = indexBufferSize;
		meshData.IndexCount = (UINT)indices.size();
		meshData.TextureIndex = ResolveStaticMeshTextureIndex(scene, mesh, fileName, dirPath);

	{
			for (int mode = 0; mode < kToonOutlineModeCount; ++mode)
			{
				vector<StaticModelVertex> teoVertices;
				vector<unsigned int> teoIndices;
				BuildTeoMesh(
					vertices,
					indices,
					teoVertices,
					teoIndices,
					static_cast<ToonOutlineMeshMode>(mode));
				if (teoVertices.empty() || teoIndices.empty())
				{
					continue;
				}
				meshData.CpuTeoVerticesByMode[mode] = teoVertices;

				const UINT teoVertexBufferSize = sizeof(StaticModelVertex) * (UINT)teoVertices.size();
				if (!CreateDefaultBufferAndUpload(device, teoVertexBufferSize, teoVertices.data(),
					D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
					"ERROR: Failed to create DEFAULT TEO vertex buffer for static model\n",
					"static fbx teo vertex", meshData.TeoVertexBuffers[mode]))
				{
					releaseScene();
					return false;
				}
				meshData.TeoVertexBufferViews[mode].BufferLocation = meshData.TeoVertexBuffers[mode]->GetGPUVirtualAddress();
				meshData.TeoVertexBufferViews[mode].StrideInBytes = sizeof(StaticModelVertex);
				meshData.TeoVertexBufferViews[mode].SizeInBytes = teoVertexBufferSize;
				meshData.TeoVertexCounts[mode] = (UINT)teoVertices.size();

				const UINT teoIndexBufferSize = sizeof(unsigned int) * (UINT)teoIndices.size();
				if (!CreateDefaultBufferAndUpload(device, teoIndexBufferSize, teoIndices.data(),
					D3D12_RESOURCE_STATE_INDEX_BUFFER,
					"ERROR: Failed to create DEFAULT TEO index buffer for static model\n",
					"static fbx teo index", meshData.TeoIndexBuffers[mode]))
				{
					releaseScene();
					return false;
				}
				meshData.TeoIndexBufferViews[mode].BufferLocation = meshData.TeoIndexBuffers[mode]->GetGPUVirtualAddress();
				meshData.TeoIndexBufferViews[mode].Format = DXGI_FORMAT_R32_UINT;
				meshData.TeoIndexBufferViews[mode].SizeInBytes = teoIndexBufferSize;
				meshData.TeoIndexCounts[mode] = (UINT)teoIndices.size();

				if (mode == static_cast<int>(ToonOutlineMeshMode::Balanced))
				{
					meshData.TeoVertexBuffer = meshData.TeoVertexBuffers[mode];
					meshData.TeoIndexBuffer = meshData.TeoIndexBuffers[mode];
					meshData.TeoVertexBufferView = meshData.TeoVertexBufferViews[mode];
					meshData.TeoIndexBufferView = meshData.TeoIndexBufferViews[mode];
					meshData.TeoVertexCount = meshData.TeoVertexCounts[mode];
					meshData.TeoIndexCount = meshData.TeoIndexCounts[mode];
				}
			}
		}

		totalVertices += vertices.size();
		totalIndices += indices.size();
		if (!BuildLodIndexBuffers(device, vertices, indices, meshData))
		{
			releaseScene();
			return false;
		}
        meshData.VirtualGeometrySettings = m_VGSettings;
		MeshletManager::BuildImportedMesh(meshData);
	m_Meshes.push_back(move(meshData));
	}

	if (m_Meshes.empty())
	{
		Debug::Log("ERROR: Static model has no drawable meshes: %s\n", fileName);
		releaseScene();
		return false;
	}

	m_AabbCenter.x = (minPos.x + maxPos.x) * 0.5f;
	m_AabbCenter.y = (minPos.y + maxPos.y) * 0.5f;
	m_AabbCenter.z = (minPos.z + maxPos.z) * 0.5f;
	m_AabbExtents.x = (maxPos.x - minPos.x) * 0.5f;
	m_AabbExtents.y = (maxPos.y - minPos.y) * 0.5f;
	m_AabbExtents.z = (maxPos.z - minPos.z) * 0.5f;

	Debug::Log("Static model loaded: %s (meshes=%zu, vertices=%zu, indices=%zu)\n",
		fileName, m_Meshes.size(), totalVertices, totalIndices);

	releaseScene();
	return true;
}

bool StaticModelResource::ApplyMeshShadingOverridePartIds(ID3D12Device* device, const vector<int>& overridePartIds)
{
	if (!device)
	{
		return false;
	}

	bool success = true;
	for (UINT meshIndex = 0; meshIndex < (UINT)m_Meshes.size(); ++meshIndex)
	{
		StaticMeshData& meshData = m_Meshes[meshIndex];
		float targetPartId;
		if ((meshIndex < overridePartIds.size() && overridePartIds[meshIndex] >= 0))
		{
			targetPartId = static_cast<float>(overridePartIds[meshIndex]);
		}
		else
		{
			targetPartId = meshData.MaterialPartId;
		}
		if (fabsf(meshData.AppliedMaterialPartId - targetPartId) < 0.001f)
		{
			continue;
		}

		for (StaticModelVertex& vertex : meshData.CpuVertices)
		{
			vertex.Diffuse.w = targetPartId;
		}
		if (meshData.VertexBuffer && !meshData.CpuVertices.empty())
		{
			const UINT vertexBufferSize = sizeof(StaticModelVertex) * (UINT)meshData.CpuVertices.size();
			success &= UploadBufferData(device, meshData.VertexBuffer.Get(), meshData.CpuVertices.data(), vertexBufferSize,
				D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
				D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
				"static mesh shading vertex");
		}

		for (int mode = 0; mode < kToonOutlineModeCount; ++mode)
		{
			auto& teoVertices = meshData.CpuTeoVerticesByMode[mode];
			for (StaticModelVertex& vertex : teoVertices)
			{
				vertex.Diffuse.w = targetPartId;
			}
			if (meshData.TeoVertexBuffers[mode] && !teoVertices.empty())
			{
				const UINT teoVertexBufferSize = sizeof(StaticModelVertex) * (UINT)teoVertices.size();
				success &= UploadBufferData(device, meshData.TeoVertexBuffers[mode].Get(), teoVertices.data(), teoVertexBufferSize,
					D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
					D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
					"static mesh shading teo vertex");
			}
		}

		meshData.AppliedMaterialPartId = targetPartId;
		MeshletManager::BuildImportedMesh(meshData);
		GPUDrivenRenderer::UpdateGeometry(meshData);
	}

	return success;
}

void StaticModelResource::Uninit()
{
	m_Meshes.clear();
	Clear();
}

bool StaticModelResource::PollVirtualGeometry()
{
    bool changed = false;
    for (auto& mesh : m_Meshes)
    {
        changed = MeshletManager::PollImportedMesh(mesh) || changed;
    }
    return changed;
}
void StaticModelResource::RebuildVirtualGeometry(const VGImportSettings& settings)
{
    m_DerivedVGPublished = false;
    m_VGSettings = settings;
    for (auto& mesh : m_Meshes)
    {
        mesh.VirtualGeometrySettings = settings;
        if (mesh.VirtualGeometryRootFallback && mesh.VirtualGeometryLODs[0] && mesh.VirtualGeometryLODs[0]->Pages)
        {
            auto runtime = std::make_shared<VirtualGeometryRuntime::RuntimeAsset>(*mesh.VirtualGeometryLODs[0]);
            runtime->ImportSettings = settings;
            mesh.VirtualGeometryLODs[0] = std::move(runtime);
            continue; // Prebuilt pages retain their native hierarchy when runtime policy changes.
        }
        MeshletManager::BuildImportedMesh(mesh);
        GPUDrivenRenderer::UpdateGeometry(mesh);
    }
}


