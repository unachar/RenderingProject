#include "pch.h"
#include "animationmath.h"

using namespace AnimationMath;
#include "animationmodel.h"
#include "pmxloader.h"
#include "texturemanager.h"
#include "graphicsdevice.h"
#include "shaderpipelines.h"
#include "psomanager.h"
#include "materialpartresolver.h"
#include "material.h"
#include "modelimportutils.h"
#include "toonoutlinebuilder.h"
#include "../../External/meshoptimizer/src/meshoptimizer.h"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <cctype>
#include <cmath>
#include <limits>
#include "frameconstants.h"

static bool CreateLodIndexBuffer(
	ID3D12Device* device,
	const vector<unsigned int>& indices,
	ComPtr<ID3D12Resource>& resource,
	D3D12_INDEX_BUFFER_VIEW& view)
{
	if (!device || indices.empty())
	{
		return false;
	}

	const UINT byteSize = static_cast<UINT>(indices.size() * sizeof(unsigned int));
	const D3D12_RESOURCE_DESC description = CD3DX12_RESOURCE_DESC::Buffer(byteSize);
	const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
	if (FAILED(device->CreateCommittedResource(
		&heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COMMON,
		nullptr, IID_PPV_ARGS(&resource))))
	{
		return false;
	}

	UINT64 uploadSize = 0;
	device->GetCopyableFootprints(&description, 0, 1, 0, nullptr, nullptr, nullptr, &uploadSize);
	ComPtr<ID3D12Resource> upload = TextureManager::AcquireUploadBuffer(device, uploadSize);
	if (!upload)
	{
		resource.Reset();
		return false;
	}

	const bool batchMode = TextureManager::IsBatchLoading();
	ComPtr<ID3D12CommandAllocator> allocator;
	ComPtr<ID3D12GraphicsCommandList> commandList;
	if (batchMode)
	{
		commandList = TextureManager::GetBatchCommandList();
	}
	else
	{
		if (FAILED(device->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
			FAILED(device->CreateCommandList(
				0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
				IID_PPV_ARGS(&commandList))))
		{
			TextureManager::ReleaseUploadBuffer(upload, uploadSize);
			resource.Reset();
			return false;
		}
	}
	if (!commandList)
	{
		TextureManager::ReleaseUploadBuffer(upload, uploadSize);
		resource.Reset();
		return false;
	}

	const auto toCopy = CD3DX12_RESOURCE_BARRIER::Transition(
		resource.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
	commandList->ResourceBarrier(1, &toCopy);
	D3D12_SUBRESOURCE_DATA data{};
	data.pData = indices.data();
	data.RowPitch = byteSize;
	data.SlicePitch = byteSize;
	UpdateSubresources(commandList.Get(), resource.Get(), upload.Get(), 0, 0, 1, &data);
	const auto ready = CD3DX12_RESOURCE_BARRIER::Transition(
		resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER);
	commandList->ResourceBarrier(1, &ready);

	if (!batchMode && !TextureManager::ExecuteCommandListAndSync(commandList.Get()))
	{
		TextureManager::ReleaseUploadBuffer(upload, uploadSize);
		resource.Reset();
		return false;
	}
	TextureManager::ReleaseUploadBuffer(upload, uploadSize, batchMode);

	view.BufferLocation = resource->GetGPUVirtualAddress();
	view.Format = DXGI_FORMAT_R32_UINT;
	view.SizeInBytes = byteSize;
	return true;
}

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
			Debug::Log("Texture[%s][%u]: %s\n", GetAssimpTextureTypeName(type), i, path.C_Str());
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


static string DecodeAsciiFixedString(const char* data, size_t length)
{
	size_t byteCount = 0;
	while (byteCount < length && data[byteCount] != '\0')
	{
		++byteCount;
	}
	return string(data, byteCount);
}

static string DecodeShiftJisFixedString(const char* data, size_t length)
{
	size_t byteCount = 0;
	while (byteCount < length && data[byteCount] != '\0')
	{
		++byteCount;
	}
	if (byteCount == 0)
	{
		return {};
	}

	int wideLength = MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, data, static_cast<int>(byteCount), nullptr, 0);
	if (wideLength <= 0)
	{
		wideLength = MultiByteToWideChar(932, 0, data, static_cast<int>(byteCount), nullptr, 0);
	}
	if (wideLength <= 0)
	{
		return string(data, byteCount);
	}

	wstring wideText(static_cast<size_t>(wideLength), L'\0');
	MultiByteToWideChar(932, 0, data, static_cast<int>(byteCount), wideText.data(), wideLength);

	const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, wideText.data(), wideLength, nullptr, 0, nullptr, nullptr);
	if (utf8Length <= 0)
	{
		return string(data, byteCount);
	}

	string utf8Text(static_cast<size_t>(utf8Length), '\0');
	WideCharToMultiByte(CP_UTF8, 0, wideText.data(), wideLength, utf8Text.data(), utf8Length, nullptr, nullptr);
	return utf8Text;
}

template <typename T>
static bool ReadBinary(ifstream& stream, T& value)
{
	return static_cast<bool>(stream.read(reinterpret_cast<char*>(&value), sizeof(T)));
}

template <size_t N>
static bool ReadBinaryArray(ifstream& stream, array<char, N>& value)
{
	return static_cast<bool>(stream.read(value.data(), static_cast<streamsize>(value.size())));
}

static uint64_t GetRemainingBytes(ifstream& stream)
{
	const streampos current = stream.tellg();
	if (current < 0)
	{
		return 0;
	}

	stream.seekg(0, ios::end);
	const streampos end = stream.tellg();
	stream.seekg(current, ios::beg);
	if (end < current)
	{
		return 0;
	}
	return static_cast<uint64_t>(end - current);
}

static bool ReadOptionalSectionCount(ifstream& stream, uint32_t& count, const char* sectionName, const char* fileName)
{
	const uint64_t remainingBytes = GetRemainingBytes(stream);
	if (remainingBytes == 0)
	{
		count = 0;
		return true;
	}
	if (remainingBytes < sizeof(uint32_t))
	{
		Debug::Log("ERROR: VMD %s count is truncated: %s\n", sectionName, fileName);
		return false;
	}
	return ReadBinary(stream, count);
}

static bool SkipBinaryBytes(ifstream& stream, uint64_t byteCount, const char* sectionName, const char* fileName)
{
	if (GetRemainingBytes(stream) < byteCount)
	{
		Debug::Log("ERROR: VMD %s data is truncated: %s\n", sectionName, fileName);
		return false;
	}

	stream.seekg(static_cast<streamoff>(byteCount), ios::cur);
	return static_cast<bool>(stream);
}

static float NormalizeVmdInterpolationByte(unsigned char value)
{
	return clamp(static_cast<float>(value) / 127.0f, 0.0f, 1.0f);
}

static float GetYFromXOnBezier(float x, const XMFLOAT2& p1, const XMFLOAT2& p2, uint8_t iterationCount)
{
	x = clamp(x, 0.0f, 1.0f);
	if (fabsf(p1.x - p1.y) < 0.0001f && fabsf(p2.x - p2.y) < 0.0001f)
	{
		return x;
	}

	float t = x;
	const float k0 = 1.0f + 3.0f * p1.x - 3.0f * p2.x;
	const float k1 = 3.0f * p2.x - 6.0f * p1.x;
	const float k2 = 3.0f * p1.x;

	for (uint8_t i = 0; i < iterationCount; ++i)
	{
		const float ft = k0 * t * t * t + k1 * t * t + k2 * t - x;
		if (fabsf(ft) <= 0.00001f)
		{
			break;
		}
		t = clamp(t - ft / 2.0f, 0.0f, 1.0f);
	}

	const float r = 1.0f - t;
	return t * t * t + 3.0f * t * t * r * p2.y + 3.0f * t * r * r * p1.y;
}

static aiVector3D ConvertVmdPositionToAssimpLeftHanded(const aiVector3D& position)
{
	return aiVector3D(position.x, position.y, position.z);
}

static aiQuaternion ConvertVmdRotationToAssimpLeftHanded(aiQuaternion rotation)
{


	rotation.Normalize();
	return rotation;
}

static void NormalizeBoneInfluences(GpuSkinVertex& gpuVertex, DeformVertex& deformVertex)
{
	float totalWeight = 0.0f;
	for (int i = 0; i < 4; ++i)
	{
		if (gpuVertex.BoneWeights[i] < 0.0f)
		{
			gpuVertex.BoneWeights[i] = 0.0f;
		}
		totalWeight += gpuVertex.BoneWeights[i];
	}


	if (totalWeight <= 0.000001f)
	{
		deformVertex.BoneNum = 0;
		for (int i = 0; i < 4; ++i)
		{
			gpuVertex.BoneIndices[i] = 0;
			gpuVertex.BoneWeights[i] = 0.0f;
			deformVertex.BoneName[i].clear();
			deformVertex.BoneWeight[i] = 0.0f;
		}
		return;
	}

	const float invTotalWeight = 1.0f / totalWeight;
	for (int i = 0; i < 4; ++i)
	{
		gpuVertex.BoneWeights[i] *= invTotalWeight;
		deformVertex.BoneWeight[i] *= invTotalWeight;
	}
}


static aiMatrix4x4 GetBoneOffsetMatrixOrFallback(const aiScene* scene, const aiBone* bone)
{
	const aiMatrix4x4 identity = MakeAiIdentityMatrix();
	if (!bone)
	{
		return identity;
	}

	if (IsUsableSkinningMatrix(bone->mOffsetMatrix))
	{
		return bone->mOffsetMatrix;
	}

	aiMatrix4x4 bindGlobalMatrix = identity;
	if (scene && FindNodeGlobalMatrix(scene->mRootNode, bone->mName.C_Str(), identity, bindGlobalMatrix))
	{
		aiMatrix4x4 fallbackOffset = bindGlobalMatrix;
		fallbackOffset.Inverse();
		if (IsUsableSkinningMatrix(fallbackOffset))
		{
			return fallbackOffset;
		}
	}

	return identity;
}

void AnimationModelResource::CreateBone(aiNode* node)
{
	string name = node->mName.C_Str();
	if (node->mParent)
	{
		m_BoneParentMap[name] = node->mParent->mName.C_Str();
	}
	if (m_BoneIndexMap.find(name) == m_BoneIndexMap.end())
	{
		uint32_t idx = (uint32_t)m_BoneNames.size();
		m_BoneNames.push_back(name);
		m_BoneIndexMap[name] = idx;
		const aiMatrix4x4 identity = MakeAiIdentityMatrix();
		Bone bone{};
		bone.Matrix = identity;
		bone.BindLocalMatrix = node->mTransformation;
		bone.AnimationMatrix = node->mTransformation;
		bone.OffsetMatrix = identity;
		m_Bone.emplace(name, bone);
	}

	for (unsigned int n = 0; n < node->mNumChildren; n++)
	{
		CreateBone(node->mChildren[n]);
	}
}

bool AnimationModelResource::Load(const char* fileName, ID3D12Device* device, bool isConvert)
{
	m_pDevice = device;

	const filesystem::path modelPath = ModelPathFromUtf8(fileName);
	const string extension = ModelPathLowerExtension(modelPath);
	const bool isPmxModel = extension == ".pmx";
	PmxModel pmxModel{};
	vector<vector<uint32_t>> pmxMeshVertexIndices{};

	if (isPmxModel)
	{
		if (!LoadPmxModel(fileName, pmxModel))
		{
			return false;
		}

		m_AiScene = CreatePmxGeneratedScene(pmxModel, pmxMeshVertexIndices);
		m_OwnsGeneratedAiScene = true;
		if (!m_AiScene)
		{
			Debug::Log("ERROR: Failed to generate PMX animation model scene: %s\n", fileName);
			return false;
		}
	}
	else
	{
		unsigned int flags = aiProcessPreset_TargetRealtime_MaxQuality &
			~(aiProcess_CalcTangentSpace | aiProcess_ValidateDataStructure);
		if (isConvert)
		{
			flags |= aiProcess_ConvertToLeftHanded;
		}
		m_AiScene = ImportModelScene(fileName, flags);
		m_OwnsGeneratedAiScene = false;
		if (!m_AiScene)
		{
			Debug::Log("ERROR: Failed to load animation model: %s (%s)\n", fileName, aiGetErrorString());
			return false;
		}
		LogAssimpModelInfo(m_AiScene, fileName, "Animation");
	}
	m_Meshes.resize(m_AiScene->mNumMeshes);
	m_DeformVertex.resize(m_AiScene->mNumMeshes);
	m_GpuSkinVertices.resize(m_AiScene->mNumMeshes);
	m_BaseGpuSkinVertices.resize(m_AiScene->mNumMeshes);
	m_TeoGpuSkinVertices.resize(m_AiScene->mNumMeshes);
	m_TeoGpuSkinVerticesByMode.resize(m_AiScene->mNumMeshes);

	CreateBone(m_AiScene->mRootNode);
	m_PmxAppendConstraints.clear();
	m_PmxIkConstraints.clear();
	m_PmxBaseVertices.clear();
	m_PmxBaseNormals.clear();
	m_PmxBaseTexCoords.clear();
	m_PmxMorphs.clear();
	m_PmxMorphIndexMap.clear();
	m_PmxRigidBodies.clear();
	m_PmxJoints.clear();
	if (isPmxModel)
	{
		PopulatePmxAnimationMetadata(
			pmxModel,
			m_PmxAppendConstraints,
			m_PmxIkConstraints,
			m_PmxBaseVertices,
			m_PmxBaseNormals,
			m_PmxBaseTexCoords,
			m_PmxMorphs,
			m_PmxMorphIndexMap);
		m_PmxRigidBodies = pmxModel.RigidBodies;
		m_PmxJoints = pmxModel.Joints;
	}

	const string dirPath = ModelPathToUtf8(modelPath.parent_path());
	XMFLOAT3 minPos = { FLT_MAX, FLT_MAX, FLT_MAX };
	XMFLOAT3 maxPos = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
	bool hasVertices = false;

	for (unsigned int m = 0; m < m_AiScene->mNumMeshes; m++)
	{
		aiMesh* mesh = m_AiScene->mMeshes[m];
		m_Meshes[m].TextureIndex = ResolveMeshTextureIndex(mesh, fileName, dirPath);
		m_Meshes[m].MaterialIndex = static_cast<int>(mesh->mMaterialIndex);
		m_Meshes[m].MeshName = mesh->mName.C_Str();

		aiColor3D diffuse(1.0f, 1.0f, 1.0f);
		float opacity = 1.0f;
		aiString materialName;
		if (mesh->mMaterialIndex < m_AiScene->mNumMaterials)
		{
			auto* material = m_AiScene->mMaterials[mesh->mMaterialIndex];
			material->Get(AI_MATKEY_NAME, materialName);
			material->Get(AI_MATKEY_COLOR_DIFFUSE, diffuse);
			material->Get(AI_MATKEY_OPACITY, opacity);
		}
		const float materialPartId = ResolveMaterialPartId(m_AiScene, mesh);
		m_Meshes[m].MaterialName = materialName.C_Str();
		m_Meshes[m].MaterialPartId = materialPartId;
		m_Meshes[m].DefaultToonOutlineEnabled = materialPartId != 3.0f;
		const XMFLOAT4 meshDiffuse(diffuse.r, diffuse.g, diffuse.b, materialPartId);

		m_GpuSkinVertices[m].resize(mesh->mNumVertices);
		for (unsigned int v = 0; v < mesh->mNumVertices; v++)
		{
			GpuSkinVertex& gv = m_GpuSkinVertices[m][v];
			gv.Position = XMFLOAT3(mesh->mVertices[v].x, mesh->mVertices[v].y, mesh->mVertices[v].z);
			if (mesh->HasNormals())
			{
				gv.Normal = XMFLOAT3(mesh->mNormals[v].x, mesh->mNormals[v].y, mesh->mNormals[v].z);
			}
			else
			{
				gv.Normal = XMFLOAT3(0.0f, 1.0f, 0.0f);
			}

			minPos.x = min(minPos.x, gv.Position.x);
			minPos.y = min(minPos.y, gv.Position.y);
			minPos.z = min(minPos.z, gv.Position.z);
			maxPos.x = max(maxPos.x, gv.Position.x);
			maxPos.y = max(maxPos.y, gv.Position.y);
			maxPos.z = max(maxPos.z, gv.Position.z);
			hasVertices = true;

			const auto texCoords = mesh->mTextureCoords[0];
			if (texCoords)
			{
				gv.TexCoord = XMFLOAT2(texCoords[v].x, texCoords[v].y);
			}
			else
			{
				gv.TexCoord = XMFLOAT2(0.0f, 0.0f);
			}
			gv.Diffuse = meshDiffuse;
			for (int i = 0; i < m_kMAX_BONE_INFLUENCES; ++i)
			{
				gv.BoneIndices[i] = 0;
				gv.BoneWeights[i] = 0.0f;
			}
			if (isPmxModel)
			{
				ApplyPmxVertexDeformData(pmxModel, pmxMeshVertexIndices, m, v, gv);
			}
		}

		m_DeformVertex[m].resize(mesh->mNumVertices);
		for (unsigned int v = 0; v < mesh->mNumVertices; v++)
		{
			auto& deform = m_DeformVertex[m][v];
			deform.Position = mesh->mVertices[v];
			if (mesh->HasNormals())
			{
				deform.Normal = mesh->mNormals[v];
			}
			else
			{
				deform.Normal = aiVector3D(0.0f, 1.0f, 0.0f);
			}
			deform.BoneNum = 0;
			for (int i = 0; i < m_kMAX_BONE_INFLUENCES; ++i)
			{
				deform.BoneName[i].clear();
				deform.BoneWeight[i] = 0.0f;
			}
		}

		struct PendingBoneWeight
		{
			uint32_t BoneIndex = 0;
			float Weight = 0.0f;
			string BoneName{};
		};
		vector<vector<PendingBoneWeight>> pendingBoneWeights(mesh->mNumVertices);

		for (unsigned int b = 0; b < mesh->mNumBones; b++)
		{
			aiBone* bone = mesh->mBones[b];
			const string boneName = bone->mName.C_Str();

			auto boneIt = m_Bone.find(boneName);
			if (boneIt == m_Bone.end())
			{
				uint32_t idx = (uint32_t)m_BoneNames.size();
				m_BoneNames.push_back(boneName);
				m_BoneIndexMap[boneName] = idx;

				const aiMatrix4x4 identity = MakeAiIdentityMatrix();
				Bone fallbackBone{};
				fallbackBone.Matrix = identity;
				fallbackBone.BindLocalMatrix = identity;
				fallbackBone.AnimationMatrix = fallbackBone.BindLocalMatrix;
				fallbackBone.OffsetMatrix = identity;
				boneIt = m_Bone.emplace(boneName, fallbackBone).first;
			}

			boneIt->second.OffsetMatrix = GetBoneOffsetMatrixOrFallback(m_AiScene, bone);

			auto it = m_BoneIndexMap.find(boneName);
			uint32_t boneIdx;
			if ((it != m_BoneIndexMap.end()))
			{
				boneIdx = it->second;
			}
			else
			{
				boneIdx = 0;
			}

			for (unsigned int w = 0; w < bone->mNumWeights; w++)
			{
				const aiVertexWeight weight = bone->mWeights[w];
				if (weight.mVertexId >= mesh->mNumVertices || weight.mWeight <= 0.0f)
				{
					continue;
				}
				pendingBoneWeights[weight.mVertexId].push_back({ boneIdx, weight.mWeight, boneName });
			}
		}

		auto hasInfluence = [&](unsigned int vertexIndex) -> bool
			{
				return vertexIndex < pendingBoneWeights.size() && !pendingBoneWeights[vertexIndex].empty();
			};


		for (int pass = 0; pass < 8; ++pass)
		{
			bool changed = false;
			for (unsigned int f = 0; f < mesh->mNumFaces; ++f)
			{
				const aiFace& face = mesh->mFaces[f];
				if (face.mNumIndices < 3)
				{
					continue;
				}

				for (unsigned int i = 0; i < face.mNumIndices; ++i)
				{
					const unsigned int vertexIndex = face.mIndices[i];
					if (vertexIndex >= mesh->mNumVertices || hasInfluence(vertexIndex))
					{
						continue;
					}

					for (unsigned int j = 0; j < face.mNumIndices; ++j)
					{
						const unsigned int neighborIndex = face.mIndices[j];
						if (neighborIndex >= mesh->mNumVertices || neighborIndex == vertexIndex)
						{
							continue;
						}

						if (hasInfluence(neighborIndex))
						{
							pendingBoneWeights[vertexIndex] = pendingBoneWeights[neighborIndex];
							changed = true;
							break;
						}
					}
				}
			}

			if (!changed)
			{
				break;
			}
		}


		size_t zeroWeightVerticesFixedByNearest = 0;
		size_t zeroWeightVerticesFallbackToRoot = 0;
		uint32_t wholeMeshFallbackBoneIndex = 0;
		if (m_BoneIndexMap.count("センター")) wholeMeshFallbackBoneIndex = m_BoneIndexMap["センター"];
		else if (m_BoneIndexMap.count("全ての親")) wholeMeshFallbackBoneIndex = m_BoneIndexMap["全ての親"];

		for (unsigned int v = 0; v < mesh->mNumVertices; ++v)
		{
			if (hasInfluence(v))
			{
				continue;
			}

			const aiVector3D& position = mesh->mVertices[v];
			float bestDistanceSq = numeric_limits<float>::max();
			int bestVertex = -1;

			for (unsigned int n = 0; n < mesh->mNumVertices; ++n)
			{
				if (!hasInfluence(n))
				{
					continue;
				}

				const aiVector3D diff = mesh->mVertices[n] - position;
				const float distanceSq = diff.x * diff.x + diff.y * diff.y + diff.z * diff.z;
				if (distanceSq < bestDistanceSq)
				{
					bestDistanceSq = distanceSq;
					bestVertex = static_cast<int>(n);
				}
			}

			if (bestVertex >= 0)
			{
				pendingBoneWeights[v] = pendingBoneWeights[static_cast<unsigned int>(bestVertex)];
				++zeroWeightVerticesFixedByNearest;
			}
			else
			{


				pendingBoneWeights[v].push_back({ wholeMeshFallbackBoneIndex, 1.0f, "<whole-mesh-fallback>" });
				++zeroWeightVerticesFallbackToRoot;
			}
		}

		if (zeroWeightVerticesFixedByNearest > 0 || zeroWeightVerticesFallbackToRoot > 0)
		{
			Debug::Log("Zero-weight vertices repaired: mesh=%u name='%s' nearest=%zu wholeMeshFallback=%zu\n",
				m, mesh->mName.C_Str(), zeroWeightVerticesFixedByNearest, zeroWeightVerticesFallbackToRoot);
		}

		for (unsigned int v = 0; v < mesh->mNumVertices; ++v)
		{
			auto& influences = pendingBoneWeights[v];
			sort(influences.begin(), influences.end(), [](const PendingBoneWeight& lhs, const PendingBoneWeight& rhs)
				{
					return lhs.Weight > rhs.Weight;
				});

			const int influenceCount = min<int>(m_kMAX_BONE_INFLUENCES, static_cast<int>(influences.size()));
			m_DeformVertex[m][v].BoneNum = influenceCount;
			for (int i = 0; i < influenceCount; ++i)
			{
				m_DeformVertex[m][v].BoneWeight[i] = influences[i].Weight;
				m_DeformVertex[m][v].BoneName[i] = influences[i].BoneName;
				m_GpuSkinVertices[m][v].BoneIndices[i] = influences[i].BoneIndex;
				m_GpuSkinVertices[m][v].BoneWeights[i] = influences[i].Weight;
			}

			NormalizeBoneInfluences(m_GpuSkinVertices[m][v], m_DeformVertex[m][v]);
		}

		{
		vector<unsigned int> indices(mesh->mNumFaces * 3);
		for (unsigned int f = 0; f < mesh->mNumFaces; f++)
		{
			const aiFace* face = &mesh->mFaces[f];
			indices[f * 3 + 0] = face->mIndices[0];
			indices[f * 3 + 1] = face->mIndices[1];
			indices[f * 3 + 2] = face->mIndices[2];
		}
		if (!indices.empty() && !m_GpuSkinVertices[m].empty())
		{
			meshopt_optimizeVertexCache(
				indices.data(), indices.data(), indices.size(), m_GpuSkinVertices[m].size());
			meshopt_optimizeOverdraw(
				indices.data(), indices.data(), indices.size(),
				reinterpret_cast<const float*>(m_GpuSkinVertices[m].data()),
				m_GpuSkinVertices[m].size(), sizeof(GpuSkinVertex), 1.05f);
		}
			const UINT indexBufferSize = sizeof(unsigned int) * (UINT)indices.size();

			D3D12_RESOURCE_DESC ibDesc = CD3DX12_RESOURCE_DESC::Buffer(indexBufferSize);
			CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
			HRESULT hr = m_pDevice->CreateCommittedResource(&heapProps,
				D3D12_HEAP_FLAG_NONE, &ibDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_Meshes[m].IndexBuffer));
			if (FAILED(hr))
			{
				Debug::Log("ERROR: Failed to create DEFAULT index buffer for animated mesh\n");
				return false;
			}

			UINT64 uploadBufferSize = 0;
			m_pDevice->GetCopyableFootprints(&ibDesc, 0, 1, 0, nullptr, nullptr, nullptr, &uploadBufferSize);
			ComPtr<ID3D12Resource> uploadBuffer = TextureManager::AcquireUploadBuffer(m_pDevice, uploadBufferSize);
			if (!uploadBuffer)
			{
				Debug::Log("ERROR: Failed to acquire upload buffer for animated mesh indices\n");
				return false;
			}

			ComPtr<ID3D12CommandAllocator> cmdAlloc;
			ComPtr<ID3D12GraphicsCommandList> cmdList;
			const bool batchMode = TextureManager::IsBatchLoading();
			if (batchMode)
			{
				cmdList = TextureManager::GetBatchCommandList();
				if (!cmdList)
				{
					TextureManager::ReleaseUploadBuffer(uploadBuffer, uploadBufferSize);
					Debug::Log("ERROR: Batch command list is not initialized (anim index)\n");
					return false;
				}
			}
			else
			{
				hr = m_pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc));
				if (FAILED(hr))
				{
					TextureManager::ReleaseUploadBuffer(uploadBuffer, uploadBufferSize);
					Debug::Log("ERROR: CreateCommandAllocator failed (anim index)\n");
					return false;
				}
				hr = m_pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc.Get(), nullptr, IID_PPV_ARGS(&cmdList));
				if (FAILED(hr))
				{
					TextureManager::ReleaseUploadBuffer(uploadBuffer, uploadBufferSize);
					Debug::Log("ERROR: CreateCommandList failed (anim index)\n");
					return false;
				}
			}

			auto toCopyDest = CD3DX12_RESOURCE_BARRIER::Transition(m_Meshes[m].IndexBuffer.Get(),
				D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
			cmdList->ResourceBarrier(1, &toCopyDest);

			D3D12_SUBRESOURCE_DATA ibData{};
			ibData.pData = indices.data();
			ibData.RowPitch = indexBufferSize;
			ibData.SlicePitch = ibData.RowPitch;

			UpdateSubresources(cmdList.Get(), m_Meshes[m].IndexBuffer.Get(), uploadBuffer.Get(), 0, 0, 1, &ibData);

			auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_Meshes[m].IndexBuffer.Get(),
				D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER);
			cmdList->ResourceBarrier(1, &barrier);

			if (!batchMode)
			{
				if (!TextureManager::ExecuteCommandListAndSync(cmdList.Get()))
				{
					TextureManager::ReleaseUploadBuffer(uploadBuffer, uploadBufferSize);
					Debug::Log("ERROR: Failed to execute and sync command list (anim index)\n");
					return false;
				}
			}

			TextureManager::ReleaseUploadBuffer(uploadBuffer, uploadBufferSize, batchMode);

			m_Meshes[m].IndexBufferView.BufferLocation = m_Meshes[m].IndexBuffer->GetGPUVirtualAddress();
			m_Meshes[m].IndexBufferView.Format = DXGI_FORMAT_R32_UINT;
			m_Meshes[m].IndexBufferView.SizeInBytes = indexBufferSize;
			m_Meshes[m].IndexCount = (UINT)indices.size();


			constexpr float lodRatios[MeshData::LodCount - 1] = { 0.50f, 0.20f };
			constexpr float lodErrors[MeshData::LodCount - 1] = { 0.01f, 0.035f };
			for (UINT lodIndex = 0; lodIndex < MeshData::LodCount - 1; ++lodIndex)
			{
				const size_t targetCount = max<size_t>(
					3,
					(static_cast<size_t>(indices.size() * lodRatios[lodIndex]) / 3) * 3);
				vector<unsigned int> lodIndices(indices.size());
				const size_t resultCount = meshopt_simplify(
					lodIndices.data(),
					indices.data(),
					indices.size(),
					reinterpret_cast<const float*>(m_GpuSkinVertices[m].data()),
					m_GpuSkinVertices[m].size(),
					sizeof(GpuSkinVertex),
					targetCount,
					lodErrors[lodIndex],
					meshopt_SimplifyRegularize);
				if (resultCount >= indices.size() || resultCount < 3)
				{
					continue;
				}
			lodIndices.resize(resultCount);
			meshopt_optimizeVertexCache(
				lodIndices.data(), lodIndices.data(), lodIndices.size(), m_GpuSkinVertices[m].size());
			if (CreateLodIndexBuffer(
					m_pDevice,
					lodIndices,
					m_Meshes[m].LodIndexBuffers[lodIndex],
					m_Meshes[m].LodIndexBufferViews[lodIndex]))
				{
					m_Meshes[m].LodIndexCounts[lodIndex] = static_cast<UINT>(resultCount);
				}
			}

			for (int mode = 0; mode < kToonOutlineModeCount; ++mode)
			{
				vector<unsigned int> teoIndices;
				BuildTeoMesh(
					m_GpuSkinVertices[m],
					indices,
					m_TeoGpuSkinVerticesByMode[m][mode],
					teoIndices,
					static_cast<ToonOutlineMeshMode>(mode));
				if (m_TeoGpuSkinVerticesByMode[m][mode].empty() || teoIndices.empty())
				{
					continue;
				}

				const UINT teoIndexBufferSize = sizeof(unsigned int) * (UINT)teoIndices.size();
				D3D12_RESOURCE_DESC teoIbDesc = CD3DX12_RESOURCE_DESC::Buffer(teoIndexBufferSize);
				HRESULT teoHr = m_pDevice->CreateCommittedResource(&heapProps,
					D3D12_HEAP_FLAG_NONE, &teoIbDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_Meshes[m].TeoIndexBuffers[mode]));
				if (FAILED(teoHr))
				{
					Debug::Log("ERROR: Failed to create DEFAULT TEO index buffer for animated mesh\n");
					return false;
				}

				UINT64 teoUploadBufferSize = 0;
				m_pDevice->GetCopyableFootprints(&teoIbDesc, 0, 1, 0, nullptr, nullptr, nullptr, &teoUploadBufferSize);
				ComPtr<ID3D12Resource> teoUploadBuffer = TextureManager::AcquireUploadBuffer(m_pDevice, teoUploadBufferSize);
				if (!teoUploadBuffer)
				{
					Debug::Log("ERROR: Failed to acquire upload buffer for animated TEO mesh indices\n");
					return false;
				}

				ComPtr<ID3D12CommandAllocator> teoCmdAlloc;
				ComPtr<ID3D12GraphicsCommandList> teoCmdList;
				const bool teoBatchMode = TextureManager::IsBatchLoading();
				if (teoBatchMode)
				{
					teoCmdList = TextureManager::GetBatchCommandList();
					if (!teoCmdList)
					{
						TextureManager::ReleaseUploadBuffer(teoUploadBuffer, teoUploadBufferSize);
						Debug::Log("ERROR: Batch command list is not initialized (anim TEO index)\n");
						return false;
					}
				}
				else
				{
					teoHr = m_pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&teoCmdAlloc));
					if (FAILED(teoHr))
					{
						TextureManager::ReleaseUploadBuffer(teoUploadBuffer, teoUploadBufferSize);
						Debug::Log("ERROR: CreateCommandAllocator failed (anim TEO index)\n");
						return false;
					}
					teoHr = m_pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, teoCmdAlloc.Get(), nullptr, IID_PPV_ARGS(&teoCmdList));
					if (FAILED(teoHr))
					{
						TextureManager::ReleaseUploadBuffer(teoUploadBuffer, teoUploadBufferSize);
						Debug::Log("ERROR: CreateCommandList failed (anim TEO index)\n");
						return false;
					}
				}

				auto teoToCopyDest = CD3DX12_RESOURCE_BARRIER::Transition(m_Meshes[m].TeoIndexBuffers[mode].Get(),
					D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
				teoCmdList->ResourceBarrier(1, &teoToCopyDest);

				D3D12_SUBRESOURCE_DATA teoIbData{};
				teoIbData.pData = teoIndices.data();
				teoIbData.RowPitch = teoIndexBufferSize;
				teoIbData.SlicePitch = teoIbData.RowPitch;
				UpdateSubresources(teoCmdList.Get(), m_Meshes[m].TeoIndexBuffers[mode].Get(), teoUploadBuffer.Get(), 0, 0, 1, &teoIbData);

				auto teoBarrier = CD3DX12_RESOURCE_BARRIER::Transition(m_Meshes[m].TeoIndexBuffers[mode].Get(),
					D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER);
				teoCmdList->ResourceBarrier(1, &teoBarrier);

				if (!teoBatchMode)
				{
					if (!TextureManager::ExecuteCommandListAndSync(teoCmdList.Get()))
					{
						TextureManager::ReleaseUploadBuffer(teoUploadBuffer, teoUploadBufferSize);
						Debug::Log("ERROR: Failed to execute and sync command list (anim TEO index)\n");
						return false;
					}
				}

				TextureManager::ReleaseUploadBuffer(teoUploadBuffer, teoUploadBufferSize, teoBatchMode);
				m_Meshes[m].TeoIndexBufferViews[mode].BufferLocation = m_Meshes[m].TeoIndexBuffers[mode]->GetGPUVirtualAddress();
				m_Meshes[m].TeoIndexBufferViews[mode].Format = DXGI_FORMAT_R32_UINT;
				m_Meshes[m].TeoIndexBufferViews[mode].SizeInBytes = teoIndexBufferSize;
				m_Meshes[m].TeoIndexCounts[mode] = (UINT)teoIndices.size();
				m_Meshes[m].TeoVertexCounts[mode] = (UINT)m_TeoGpuSkinVerticesByMode[m][mode].size();

				if (mode == static_cast<int>(ToonOutlineMeshMode::Balanced))
				{
					m_TeoGpuSkinVertices[m] = m_TeoGpuSkinVerticesByMode[m][mode];
					m_Meshes[m].TeoIndexBuffer = m_Meshes[m].TeoIndexBuffers[mode];
					m_Meshes[m].TeoIndexBufferView = m_Meshes[m].TeoIndexBufferViews[mode];
					m_Meshes[m].TeoIndexCount = m_Meshes[m].TeoIndexCounts[mode];
					m_Meshes[m].TeoVertexCount = m_Meshes[m].TeoVertexCounts[mode];
				}
			}
		}

		m_Meshes[m].VertexCount = mesh->mNumVertices;
		m_BaseGpuSkinVertices[m] = m_GpuSkinVertices[m];
	}

	BuildPmxVertexMeshMap();
	InvalidateVmdRuntimeCache();
	InvalidatePmxRuntimeCache();

	if (hasVertices)
	{
		m_AabbCenter.x = (minPos.x + maxPos.x) * 0.5f;
		m_AabbCenter.y = (minPos.y + maxPos.y) * 0.5f;
		m_AabbCenter.z = (minPos.z + maxPos.z) * 0.5f;
		m_AabbExtents.x = (maxPos.x - minPos.x) * 0.5f;
		m_AabbExtents.y = (maxPos.y - minPos.y) * 0.5f;
		m_AabbExtents.z = (maxPos.z - minPos.z) * 0.5f;
	}

	if (!CreateGpuSkinningBuffers(device))
	{
		Debug::Log("ERROR: Failed to create GPU skinning buffers for model: %s\n", fileName);
		return false;
	}

	UpdateBindPoseBoneMatrix(m_AiScene->mRootNode, MakeAiIdentityMatrix());
	WriteBoneMatricesToBuffer();

	return true;
}

bool AnimationModelResource::LoadAnimation(const char* fileName, const char* name)
{
	const filesystem::path animPath = ModelPathFromUtf8(fileName);
	if (!filesystem::exists(animPath))
	{
		Debug::Log("ERROR: Animation file not found: %s\n", fileName);
		return false;
	}

	if (ModelPathLowerExtension(animPath) == ".vmd")
	{
		return LoadVmdAnimation(fileName, name);
	}

	const aiScene* anim = ImportModelScene(fileName, aiProcess_ConvertToLeftHanded);
	if (!anim)
	{
		Debug::Log("ERROR: Failed to load animation: %s (%s)\n", fileName, aiGetErrorString());
		return false;
	}

	if (!anim->HasAnimations())
	{
		Debug::Log("ERROR: Animation file has no animation tracks: %s (meshes=%u, materials=%u)\n",
			fileName, anim->mNumMeshes, anim->mNumMaterials);
		aiReleaseImport(anim);
		return false;
	}

	string animationName;
	if (name)
	{
		animationName = name;
	}
	else
	{
		animationName = "";
	}
	m_Animation[animationName] = anim;
	m_VmdAnimations.erase(animationName);
	InvalidateVmdRuntimeCache();
	return true;
}

bool AnimationModelResource::LoadVmdAnimation(const char* fileName, const char* name)
{
	VmdAnimation animation{};
	if (!LoadVmdAnimationFile(fileName, animation))
	{
		return false;
	}


	if (animation.BoneTracks.empty() && animation.MorphTracks.empty() && animation.IkTracks.empty())
	{
		Debug::Log("ERROR: VMD has no bone, morph, or IK tracks: %s (motions=%u, morphs=%u, ikFrames=%u)\n",
			fileName, animation.MotionCount, animation.MorphCount, animation.IkCount);
		return false;
	}

	size_t matchedTrackCount = 0;
	for (const auto& pair : animation.BoneTracks)
	{
		if (m_Bone.find(pair.first) != m_Bone.end())
		{
			++matchedTrackCount;
		}
	}
	size_t matchedMorphTrackCount = 0;
	for (const auto& pair : animation.MorphTracks)
	{
		if (m_PmxMorphIndexMap.find(pair.first) != m_PmxMorphIndexMap.end())
		{
			++matchedMorphTrackCount;
		}
	}

	string animationName;
	if (name)
	{
		animationName = name;
	}
	else
	{
		animationName = "";
	}
	m_VmdAnimations[animationName] = move(animation);
	m_Animation.erase(animationName);
	InvalidateVmdRuntimeCache();

	const VmdAnimation& loaded = m_VmdAnimations[animationName];
	if (!loaded.BoneTracks.empty() && matchedTrackCount == 0)
	{
		Debug::Log("WARNING: VMD loaded but no bone names matched this model: %s\n", fileName);
	}
	if (!loaded.MorphTracks.empty() && matchedMorphTrackCount == 0)
	{
		Debug::Log("WARNING: VMD loaded but no morph names matched this model: %s\n", fileName);
	}
	return true;
}


bool AnimationModelResource::TryLoadEmbeddedTextureByIndex(const aiString& texPath, const char* modelName, int& outTexIndex) const
{
	if (texPath.length == 0 || texPath.data[0] != '*')
	{
		return false;
	}

	const int texIdx = atoi(&texPath.data[1]);
	if (texIdx < 0 || texIdx >= static_cast<int>(m_AiScene->mNumTextures))
	{
		return false;
	}

	aiTexture* aiTex = m_AiScene->mTextures[texIdx];
	string uniqueName = string(modelName) + "_" + texPath.C_Str();
	outTexIndex = TextureManager::LoadTextureFromMemory(
		uniqueName.c_str(),
		reinterpret_cast<const uint8_t*>(aiTex->pcData),
		aiTex->mWidth);
	return true;
}

bool AnimationModelResource::TryLoadEmbeddedTextureByName(const aiString& texPath, const char* modelName, int& outTexIndex) const
{
	for (unsigned int i = 0; i < m_AiScene->mNumTextures; ++i)
	{
		if (strcmp(m_AiScene->mTextures[i]->mFilename.C_Str(), texPath.C_Str()) != 0)
		{
			continue;
		}

		aiTexture* aiTex = m_AiScene->mTextures[i];
		string uniqueName = string(modelName) + "_" + texPath.C_Str();
		outTexIndex = TextureManager::LoadTextureFromMemory(
			uniqueName.c_str(),
			reinterpret_cast<const uint8_t*>(aiTex->pcData),
			aiTex->mWidth);
		return true;
	}
	return false;
}

int AnimationModelResource::ResolveMeshTextureIndex(const aiMesh* mesh, const char* fileName, const string& dirPath) const
{
	int textureIndex = TextureManager::GetDefaultTextureIndex();
	if (mesh->mMaterialIndex >= m_AiScene->mNumMaterials)
	{
		return textureIndex;
	}

	aiMaterial* material = m_AiScene->mMaterials[mesh->mMaterialIndex];
	aiString texPath;
	const bool hasTexture =
		(material->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) == AI_SUCCESS) ||
		(material->GetTexture(aiTextureType_BASE_COLOR, 0, &texPath) == AI_SUCCESS);
	if (!hasTexture)
	{
		return textureIndex;
	}

	if (TryLoadEmbeddedTextureByIndex(texPath, fileName, textureIndex) ||
		TryLoadEmbeddedTextureByName(texPath, fileName, textureIndex))
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

	Debug::Log("WARNING: Animation model texture not found: %s\n", texPath.C_Str());
	return textureIndex;
}


bool AnimationModelResource::ApplyMeshShadingOverridePartIds(const vector<int>& overridePartIds)
{
	bool success = true;
	for (UINT meshIndex = 0; meshIndex < (UINT)m_Meshes.size(); ++meshIndex)
	{
		MeshData& meshData = m_Meshes[meshIndex];
		float targetPartId;
		if ((meshIndex < overridePartIds.size() && overridePartIds[meshIndex] >= 0))
		{
			targetPartId = static_cast<float>(overridePartIds[meshIndex]);
		}
		else
		{
			targetPartId = meshData.MaterialPartId;
		}

		for (GpuSkinVertex& vertex : m_GpuSkinVertices[meshIndex])
		{
			vertex.Diffuse.w = targetPartId;
		}
		if (meshIndex < m_BaseGpuSkinVertices.size())
		{
			for (GpuSkinVertex& vertex : m_BaseGpuSkinVertices[meshIndex])
			{
				vertex.Diffuse.w = targetPartId;
			}
		}
		if (meshData.InputVertexBuffer && !m_GpuSkinVertices[meshIndex].empty())
		{
			UINT8* pDest = nullptr;
			CD3DX12_RANGE readRange(0, 0);
			HRESULT hr = meshData.InputVertexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pDest));
			if (SUCCEEDED(hr) && pDest)
			{
				memcpy(pDest, m_GpuSkinVertices[meshIndex].data(),
					sizeof(GpuSkinVertex) * m_GpuSkinVertices[meshIndex].size());
				meshData.InputVertexBuffer->Unmap(0, nullptr);
			}
			else
			{
				Debug::Log("ERROR: Failed to update animated mesh shading input buffer\n");
				success = false;
			}
		}

		for (int mode = 0; mode < kToonOutlineModeCount; ++mode)
		{
			auto& teoVertices = m_TeoGpuSkinVerticesByMode[meshIndex][mode];
			for (GpuSkinVertex& vertex : teoVertices)
			{
				vertex.Diffuse.w = targetPartId;
			}
			if (meshData.TeoInputVertexBuffers[mode] && !teoVertices.empty())
			{
				UINT8* pDest = nullptr;
				CD3DX12_RANGE readRange(0, 0);
				HRESULT hr = meshData.TeoInputVertexBuffers[mode]->Map(0, &readRange, reinterpret_cast<void**>(&pDest));
				if (SUCCEEDED(hr) && pDest)
				{
					memcpy(pDest, teoVertices.data(), sizeof(GpuSkinVertex) * teoVertices.size());
					meshData.TeoInputVertexBuffers[mode]->Unmap(0, nullptr);
				}
				else
				{
					Debug::Log("ERROR: Failed to update animated mesh shading TEO input buffer\n");
					success = false;
				}
			}
			if (mode == static_cast<int>(ToonOutlineMeshMode::Balanced))
			{
				m_TeoGpuSkinVertices[meshIndex] = teoVertices;
			}
		}
	}


	++m_SkinningVersion;
	return success;
}


void AnimationModelResource::Uninit()
{
	for (UINT frame = 0; frame < RendererState::g_kFRAME_COUNT; ++frame)
	{
		if (m_BoneBuffers[frame] && m_pBoneBufferMapped[frame])
		{
			m_BoneBuffers[frame]->Unmap(0, nullptr);
			m_pBoneBufferMapped[frame] = nullptr;
		}
	}

	m_Meshes.clear();
	m_DeformVertex.clear();
	m_GpuSkinVertices.clear();
	m_BaseGpuSkinVertices.clear();
	m_Bone.clear();
	m_BoneNames.clear();
	m_BoneIndexMap.clear();
	m_NodeAnimChannelCache.clear();
	m_SkinningMatrixCount = 0;
	m_BoneParentMap.clear();
	m_PmxAppendConstraints.clear();
	m_PmxIkConstraints.clear();
	m_PmxBaseVertices.clear();
	m_PmxBaseNormals.clear();
	m_PmxBaseTexCoords.clear();
	m_PmxMorphs.clear();
	m_PmxMorphIndexMap.clear();
	m_PmxRigidBodies.clear();
	m_PmxJoints.clear();
	m_PmxVertexToMeshVertices.clear();
	m_BoneMatricesScratch.clear();
	m_SkinningVersion = 0;
	m_DispatchedSkinningVersion = UINT64_MAX;
    m_DispatchedOutlineSkinningVersion = UINT64_MAX;
	m_LastPoseAnimation1.clear();
	m_LastPoseAnimation2.clear();
	m_LastPoseFrame1 = 0.0f;
	m_LastPoseFrame2 = 0.0f;
	m_LastPoseBlendRate = 0.0f;
	m_HasCachedPose = false;
	InvalidateVmdRuntimeCache();
	InvalidatePmxRuntimeCache();
	m_HasAppliedVmdMorphs = false;
	m_AabbCenter = {};
	m_AabbExtents = {};

	if (m_AiScene)
	{
		if (m_OwnsGeneratedAiScene)
		{
			DestroyPmxGeneratedScene(const_cast<aiScene*>(m_AiScene));
		}
		else
		{
			aiReleaseImport(m_AiScene);
		}
		m_AiScene = nullptr;
		m_OwnsGeneratedAiScene = false;
	}
	for (auto& pair : m_Animation)
	{
		aiReleaseImport(pair.second);
	}
	m_Animation.clear();
	m_VmdAnimations.clear();
}

