#include "pch.h"
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
#include "rendererdraw.h"
#include "staticmodel.h"
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
			mesh->mNumBones, mesh->HasNormals() ? 1 : 0, mesh->HasTextureCoords(0) ? 1 : 0);
		Debug::Log("    MaterialPartId=%.0f\n", MaterialPartResolver::ResolveMaterialPartId(scene, mesh));
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
			return MaterialPartResolver::ToLowerString(value);
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

	filesystem::path texFilePath = ModelImportUtils::FromUtf8(texPath.C_Str());
	vector<filesystem::path> candidates;
	if (texFilePath.extension() != ".tga" && texFilePath.extension() != ".TGA")
	{
		filesystem::path tgaPath = texFilePath;
		tgaPath.replace_extension(".tga");
		pushCandidate(candidates, ModelImportUtils::FromUtf8(dirPath.c_str()) / tgaPath);
		pushCandidate(candidates, ModelImportUtils::FromUtf8(dirPath.c_str()) / tgaPath.filename());
	}
	if (isSupportedTextureExtension(texFilePath))
	{
		if (texFilePath.is_absolute())
		{
			pushCandidate(candidates, texFilePath);
		}
		pushCandidate(candidates, ModelImportUtils::FromUtf8(dirPath.c_str()) / texFilePath);
		pushCandidate(candidates, ModelImportUtils::FromUtf8(dirPath.c_str()) / texFilePath.filename());
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

bool StaticModelResource::LoadObj(const char* fileName, ID3D12Device* device)
{
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
			string mtlPath = dir.empty() ? mtlName : (dir / mtlName).string();

			materialMap = ParseMtl(mtlPath);

			for (auto const& pair : materialMap)
			{
				const MtlData& data = pair.second;
				if (!data.MapKd.empty() && m_MaterialPath.empty())
				{
					m_MaterialPath = dir.empty() ? data.MapKd : (dir / data.MapKd).string();
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

	{
		for (int mode = 0; mode < ToonOutlineBuilder::kModeCount; ++mode)
		{
			vector<StaticModelVertex> teoVertices;
			vector<unsigned int> teoIndices;
			ToonOutlineBuilder::BuildTeoMesh(
				m_Vertices,
				m_Indices,
				teoVertices,
				teoIndices,
				static_cast<ToonOutlineBuilder::Mode>(mode));
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

			if (mode == static_cast<int>(ToonOutlineBuilder::Mode::Balanced))
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

	m_Meshes.push_back(move(meshData));

	char buf[512];
	sprintf_s(buf, "Static model loaded: %s (vertices=%zu, indices=%zu)\n",
		fileName, m_Vertices.size(), m_Indices.size());
	Debug::Log("%s", buf);

	return true;
}

bool StaticModelResource::LoadFBX(const char* fileName, ID3D12Device* device, bool isConvert)

{
	return LoadAssimpModel(fileName, device, isConvert);
}

bool StaticModelResource::LoadAssimpModel(const char* fileName, ID3D12Device* device, bool isConvert)
{
	if (!device)
	{
		Debug::Log("ERROR: Invalid device for static model: %s\n", fileName);
		return false;
	}

	m_Meshes.clear();
	m_MaterialPath.clear();
	Clear();

	const filesystem::path modelPath = ModelImportUtils::FromUtf8(fileName);
	const bool isPmxModel = ModelImportUtils::LowerExtension(modelPath) == ".pmx";
	PmxBinary::Model pmxModel{};
	vector<vector<uint32_t>> pmxMeshVertexIndices{};
	bool ownsGeneratedScene = false;
	const aiScene* scene = nullptr;
	if (isPmxModel)
	{
		if (!PmxBinary::LoadModel(fileName, pmxModel))
		{
			return false;
		}
		scene = PmxBinary::CreateGeneratedScene(pmxModel, pmxMeshVertexIndices);
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
		scene = ModelImportUtils::ImportScene(fileName, flags);
	}
	auto releaseScene = [&]()
		{
			if (!scene)
			{
				return;
			}
			if (ownsGeneratedScene)
			{
				PmxBinary::DestroyGeneratedScene(const_cast<aiScene*>(scene));
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
	const string dirPath = ModelImportUtils::ToUtf8(modelPath.parent_path());

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
		const float materialPartId = MaterialPartResolver::ResolveMaterialPartId(scene, mesh);
		const XMFLOAT4 meshDiffuse(diffuse.r, diffuse.g, diffuse.b, materialPartId);

		vector<StaticModelVertex> vertices(mesh->mNumVertices);
		for (unsigned int v = 0; v < mesh->mNumVertices; ++v)
		{
			StaticModelVertex& vertex = vertices[v];
			vertex.Position = XMFLOAT3(mesh->mVertices[v].x, mesh->mVertices[v].y, mesh->mVertices[v].z);
			vertex.Normal = mesh->HasNormals()
				? XMFLOAT3(mesh->mNormals[v].x, mesh->mNormals[v].y, mesh->mNormals[v].z)
				: XMFLOAT3(0.0f, 1.0f, 0.0f);

			const auto texCoords = mesh->mTextureCoords[0];
			vertex.TexCoord = texCoords ? XMFLOAT2(texCoords[v].x, texCoords[v].y) : XMFLOAT2(0.0f, 0.0f);
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

		StaticMeshData meshData {};
		meshData.MeshName = mesh->mName.C_Str();
		meshData.MaterialName = materialName.C_Str();
		meshData.MaterialPartId = materialPartId;
		meshData.AppliedMaterialPartId = meshData.MaterialPartId;
		meshData.DefaultToonOutlineEnabled = materialPartId != 3.0f;
		meshData.CpuVertices = vertices;
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
			for (int mode = 0; mode < ToonOutlineBuilder::kModeCount; ++mode)
			{
				vector<StaticModelVertex> teoVertices;
				vector<unsigned int> teoIndices;
				ToonOutlineBuilder::BuildTeoMesh(
					vertices,
					indices,
					teoVertices,
					teoIndices,
					static_cast<ToonOutlineBuilder::Mode>(mode));
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

				if (mode == static_cast<int>(ToonOutlineBuilder::Mode::Balanced))
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
		const float targetPartId =
			(meshIndex < overridePartIds.size() && overridePartIds[meshIndex] >= 0)
			? static_cast<float>(overridePartIds[meshIndex])
			: meshData.MaterialPartId;
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

		for (int mode = 0; mode < ToonOutlineBuilder::kModeCount; ++mode)
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
	}

	return success;
}

void StaticModelResource::Uninit()
{
	m_Meshes.clear();
	Clear();
}

