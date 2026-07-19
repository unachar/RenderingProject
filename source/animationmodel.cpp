#include "pch.h"
#include "animationmodel.h"
#include "pmxloader.h"
#include "texturemanager.h"
#include "renderercore.h"
#include "rendererdraw.h"
#include "renderershader.h"
#include "psomanager.h"
#include "materialpartresolver.h"
#include "material.h"
#include "modelimportutils.h"
#include "toonoutlinebuilder.h"
#include "../External/meshoptimizer/src/meshoptimizer.h"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <cctype>
#include <cmath>
#include <limits>
using namespace std;
using namespace DirectX;

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
			mesh->mNumBones, mesh->HasNormals() ? 1 : 0, mesh->HasTextureCoords(0) ? 1 : 0);
		Debug::Log("    MaterialPartId=%.0f\n", ResolveMaterialPartId(scene, mesh));
	}
	Debug::Log("==== end %s model import info ====\n", modelKind);
}

static aiMatrix4x4 MakeAiIdentityMatrix()
{
	return aiMatrix4x4(
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f);
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

static bool IsFiniteAiMatrix(const aiMatrix4x4& matrix)
{
	const float values[] =
	{
		matrix.a1, matrix.a2, matrix.a3, matrix.a4,
		matrix.b1, matrix.b2, matrix.b3, matrix.b4,
		matrix.c1, matrix.c2, matrix.c3, matrix.c4,
		matrix.d1, matrix.d2, matrix.d3, matrix.d4,
	};
	for (float value : values)
	{
		if (!isfinite(value))
		{
			return false;
		}
	}
	return true;
}

static bool IsUsableSkinningMatrix(const aiMatrix4x4& matrix)
{
	if (!IsFiniteAiMatrix(matrix) || fabsf(matrix.d4) <= 0.000001f)
	{
		return false;
	}

	const float linearLengthSq =
		matrix.a1 * matrix.a1 + matrix.a2 * matrix.a2 + matrix.a3 * matrix.a3 +
		matrix.b1 * matrix.b1 + matrix.b2 * matrix.b2 + matrix.b3 * matrix.b3 +
		matrix.c1 * matrix.c1 + matrix.c2 * matrix.c2 + matrix.c3 * matrix.c3;
	return linearLengthSq > 0.000001f;
}

static bool FindNodeGlobalMatrix(aiNode* node, const string& boneName, const aiMatrix4x4& parentMatrix, aiMatrix4x4& outMatrix)
{
	if (!node)
	{
		return false;
	}

	const aiMatrix4x4 worldMatrix = parentMatrix * node->mTransformation;
	if (boneName == node->mName.C_Str())
	{
		outMatrix = worldMatrix;
		return true;
	}

	for (unsigned int childIndex = 0; childIndex < node->mNumChildren; ++childIndex)
	{
		if (FindNodeGlobalMatrix(node->mChildren[childIndex], boneName, worldMatrix, outMatrix))
		{
			return true;
		}
	}
	return false;
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
		unsigned int flags = aiProcessPreset_TargetRealtime_MaxQuality;
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
			gv.Normal = mesh->HasNormals()
				? XMFLOAT3(mesh->mNormals[v].x, mesh->mNormals[v].y, mesh->mNormals[v].z)
				: XMFLOAT3(0.0f, 1.0f, 0.0f);

			minPos.x = min(minPos.x, gv.Position.x);
			minPos.y = min(minPos.y, gv.Position.y);
			minPos.z = min(minPos.z, gv.Position.z);
			maxPos.x = max(maxPos.x, gv.Position.x);
			maxPos.y = max(maxPos.y, gv.Position.y);
			maxPos.z = max(maxPos.z, gv.Position.z);
			hasVertices = true;

			const auto texCoords = mesh->mTextureCoords[0];
			gv.TexCoord = texCoords ? XMFLOAT2(texCoords[v].x, texCoords[v].y) : XMFLOAT2(0.0f, 0.0f);
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
			deform.Normal = mesh->HasNormals() ? mesh->mNormals[v] : aiVector3D(0.0f, 1.0f, 0.0f);
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
			uint32_t boneIdx = (it != m_BoneIndexMap.end()) ? it->second : 0;

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

	const string animationName = name ? name : "";
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

	const string animationName = name ? name : "";
	m_VmdAnimations[animationName] = std::move(animation);
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

void AnimationModelResource::InvalidateVmdRuntimeCache()
{


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

void AnimationModelResource::RebuildVmdRuntimeCache(const VmdAnimation* primaryAnimation, const VmdAnimation* secondaryAnimation)
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

	const VmdAnimation* secondary = secondaryAnimation ? secondaryAnimation : primaryAnimation;
	m_VmdBoneBindings.reserve(m_Bone.size());
	for (auto& pair : m_Bone)
	{
		VmdBoneBinding binding{};
		binding.BonePtr = &pair.second;
		binding.BonePtr->BindLocalMatrix.Decompose(binding.BaseScale, binding.BaseRotation, binding.BasePosition);

		auto primaryTrackIt = primaryAnimation->BoneTracks.find(pair.first);
		if (primaryTrackIt != primaryAnimation->BoneTracks.end())
		{
			binding.PrimaryTrack = &primaryTrackIt->second;
		}

		auto secondaryTrackIt = secondary->BoneTracks.find(pair.first);
		if (secondaryTrackIt != secondary->BoneTracks.end())
		{
			binding.SecondaryTrack = &secondaryTrackIt->second;
		}

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
		m_VmdMorphBindings.push_back({ morphIt->second, &track.second });
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

bool AnimationModelResource::IsVmdLayeredRuntimeCacheValid(
	const vector<AnimationPlaybackLayer>& animationLayers) const
{
	if (m_CachedVmdLayerAnimations.size() != animationLayers.size())
	{
		return false;
	}

	for (size_t layerIndex = 0; layerIndex < animationLayers.size(); ++layerIndex)
	{
		const auto animationIt = m_VmdAnimations.find(animationLayers[layerIndex].AnimationName);
		const VmdAnimation* animation = animationIt != m_VmdAnimations.end() ? &animationIt->second : nullptr;
		if (m_CachedVmdLayerAnimations[layerIndex] != animation)
		{
			return false;
		}
	}
	return true;
}

void AnimationModelResource::RebuildVmdLayeredRuntimeCache(
	const vector<AnimationPlaybackLayer>& animationLayers)
{
	m_HasCachedLayeredPose = false;
	m_UseLayeredVmdIk = true;
	m_CachedVmdLayerAnimations.clear();
	m_CachedVmdLayerAnimations.reserve(animationLayers.size());
	for (const AnimationPlaybackLayer& layer : animationLayers)
	{
		const auto animationIt = m_VmdAnimations.find(layer.AnimationName);
		m_CachedVmdLayerAnimations.push_back(
			animationIt != m_VmdAnimations.end() ? &animationIt->second : nullptr);
	}

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
		binding.BonePtr->BindLocalMatrix.Decompose(
			binding.BaseScale, binding.BaseRotation, binding.BasePosition);

		for (size_t layerIndex = 0; layerIndex < m_CachedVmdLayerAnimations.size(); ++layerIndex)
		{
			const VmdAnimation* animation = m_CachedVmdLayerAnimations[layerIndex];
			if (!animation)
			{
				continue;
			}
			const auto trackIt = animation->BoneTracks.find(bonePair.first);
			if (trackIt != animation->BoneTracks.end())
			{
				binding.Track = &trackIt->second;
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
				m_VmdLayeredMorphBindings.push_back(
					{ morphIndex, &trackPair.second, layerIndex, {} });
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

	auto buildFast = [&](auto&& self, aiNode* node, int parent) -> void
		{
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
			m_PmxRuntimeNodes.push_back({ node, nodeName, parent, bonePtr });

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
		m_PmxOrderedTransformSteps.push_back({ false, i, c.DeformDepth, c.BoneOrder });
	}
	for (size_t i = 0; i < m_PmxIkConstraints.size(); ++i)
	{
		const auto& c = m_PmxIkConstraints[i];
		m_PmxOrderedTransformSteps.push_back({ true, i, c.DeformDepth, c.BoneOrder });
	}
	sort(m_PmxOrderedTransformSteps.begin(), m_PmxOrderedTransformSteps.end(),
		[](const PmxOrderedTransformStep& lhs, const PmxOrderedTransformStep& rhs)
		{
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
bool AnimationModelResource::LoadPmxIkData(const char* fileName)
{
	const filesystem::path pmxPath = ModelPathFromUtf8(fileName);
	ifstream stream(pmxPath, ios::binary | ios::ate);
	if (!stream)
	{
		Debug::Log("WARNING: Failed to open PMX for IK metadata: %s\n", fileName);
		return false;
	}

	const streamsize fileSize = stream.tellg();
	if (fileSize <= 0)
	{
		Debug::Log("WARNING: PMX file is empty: %s\n", fileName);
		return false;
	}

	vector<uint8_t> bytes(static_cast<size_t>(fileSize));
	stream.seekg(0, ios::beg);
	if (!stream.read(reinterpret_cast<char*>(bytes.data()), fileSize))
	{
		Debug::Log("WARNING: Failed to read PMX for IK metadata: %s\n", fileName);
		return false;
	}

	struct PmxReader
	{
		const vector<uint8_t>& Bytes;
		size_t Pos = 0;
		uint8_t Encoding = 1;
		uint8_t AdditionalUvCount = 0;
		uint8_t VertexIndexSize = 4;
		uint8_t TextureIndexSize = 4;
		uint8_t MaterialIndexSize = 4;
		uint8_t BoneIndexSize = 4;
		uint8_t MorphIndexSize = 4;
		uint8_t RigidBodyIndexSize = 4;

		bool CanRead(size_t byteCount) const
		{
			return Pos <= Bytes.size() && byteCount <= Bytes.size() - Pos;
		}

		bool ReadBytes(void* outValue, size_t byteCount)
		{
			if (!CanRead(byteCount))
			{
				return false;
			}
			memcpy(outValue, Bytes.data() + Pos, byteCount);
			Pos += byteCount;
			return true;
		}

		bool Read(uint8_t& value) { return ReadBytes(&value, sizeof(value)); }
		bool Read(int8_t& value) { return ReadBytes(&value, sizeof(value)); }
		bool Read(uint16_t& value) { return ReadBytes(&value, sizeof(value)); }
		bool Read(int16_t& value) { return ReadBytes(&value, sizeof(value)); }
		bool Read(uint32_t& value) { return ReadBytes(&value, sizeof(value)); }
		bool Read(int32_t& value) { return ReadBytes(&value, sizeof(value)); }
		bool Read(float& value) { return ReadBytes(&value, sizeof(value)); }

		bool Skip(size_t byteCount)
		{
			if (!CanRead(byteCount))
			{
				return false;
			}
			Pos += byteCount;
			return true;
		}

		bool ReadIndex(uint8_t indexSize, int32_t& value)
		{
			if (indexSize == 1)
			{
				uint8_t raw = 0;
				if (!Read(raw)) return false;
				value = (raw == 0xff) ? -1 : static_cast<int32_t>(raw);
				return true;
			}
			if (indexSize == 2)
			{
				uint16_t raw = 0;
				if (!Read(raw)) return false;
				value = (raw == 0xffff) ? -1 : static_cast<int32_t>(raw);
				return true;
			}
			return Read(value);
		}

		bool ReadUnsignedIndex(uint8_t indexSize, uint32_t& value)
		{
			if (indexSize == 1)
			{
				uint8_t v = 0;
				if (!Read(v)) return false;
				value = v;
				return true;
			}
			if (indexSize == 2)
			{
				uint16_t v = 0;
				if (!Read(v)) return false;
				value = static_cast<uint32_t>(v);
				return true;
			}
			int32_t signedValue = 0;
			if (!Read(signedValue) || signedValue < 0)
			{
				return false;
			}
			value = static_cast<uint32_t>(signedValue);
			return true;
		}

		bool SkipIndex(uint8_t indexSize)
		{
			return Skip(indexSize);
		}

		bool ReadText(string& text)
		{
			int32_t byteLength = 0;
			if (!Read(byteLength) || byteLength < 0 || !CanRead(static_cast<size_t>(byteLength)))
			{
				return false;
			}
			if (byteLength == 0)
			{
				text.clear();
				return true;
			}

			if (Encoding == 0)
			{
				const int wideLength = byteLength / 2;
				wstring wideText(static_cast<size_t>(wideLength), L'\0');
				memcpy(wideText.data(), Bytes.data() + Pos, static_cast<size_t>(wideLength) * sizeof(wchar_t));
				Pos += static_cast<size_t>(byteLength);

				const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, wideText.data(), wideLength, nullptr, 0, nullptr, nullptr);
				if (utf8Length <= 0)
				{
					text.clear();
					return true;
				}
				text.assign(static_cast<size_t>(utf8Length), '\0');
				WideCharToMultiByte(CP_UTF8, 0, wideText.data(), wideLength, text.data(), utf8Length, nullptr, nullptr);
				return true;
			}

			text.assign(reinterpret_cast<const char*>(Bytes.data() + Pos), static_cast<size_t>(byteLength));
			Pos += static_cast<size_t>(byteLength);
			return true;
		}
	};

	struct RawIkLink
	{
		int32_t BoneIndex = -1;
		bool HasLimit = false;
		aiVector3D LimitMin{};
		aiVector3D LimitMax{};
	};

	struct RawBone
	{
		string Name{};
		uint16_t Flags = 0;
		int32_t DeformDepth = 0;
		int32_t AppendBoneIndex = -1;
		float AppendWeight = 0.0f;
		int32_t IkTargetIndex = -1;
		uint32_t IkIterationCount = 0;
		float IkLimitAngle = 0.0f;
		vector<RawIkLink> IkLinks{};
	};

	auto readFloat3 = [](PmxReader& reader, aiVector3D& outValue)
		{
			return reader.Read(outValue.x) && reader.Read(outValue.y) && reader.Read(outValue.z);
		};
	auto readFloat4 = [](PmxReader& reader, XMFLOAT4& outValue)
		{
			return reader.Read(outValue.x) && reader.Read(outValue.y) &&
				reader.Read(outValue.z) && reader.Read(outValue.w);
		};

	PmxReader reader{ bytes };
	array<char, 4> magic{};
	if (!reader.CanRead(4))
	{
		return false;
	}
	memcpy(magic.data(), bytes.data(), 4);
	reader.Pos = 4;
	if (string(magic.data(), magic.size()) != "PMX ")
	{
		Debug::Log("WARNING: PMX IK metadata skipped because header is invalid: %s\n", fileName);
		return false;
	}

	float version = 0.0f;
	uint8_t configSize = 0;
	if (!reader.Read(version) || !reader.Read(configSize) || !reader.CanRead(configSize))
	{
		Debug::Log("WARNING: PMX IK metadata header is truncated: %s\n", fileName);
		return false;
	}
	if (configSize < 8)
	{
		Debug::Log("WARNING: PMX IK metadata config is too short: %s\n", fileName);
		return false;
	}

	reader.Read(reader.Encoding);
	reader.Read(reader.AdditionalUvCount);
	reader.Read(reader.VertexIndexSize);
	reader.Read(reader.TextureIndexSize);
	reader.Read(reader.MaterialIndexSize);
	reader.Read(reader.BoneIndexSize);
	reader.Read(reader.MorphIndexSize);
	reader.Read(reader.RigidBodyIndexSize);
	if (configSize > 8)
	{
		reader.Skip(configSize - 8);
	}

	string ignoredText;
	if (!reader.ReadText(ignoredText) || !reader.ReadText(ignoredText) ||
		!reader.ReadText(ignoredText) || !reader.ReadText(ignoredText))
	{
		Debug::Log("WARNING: PMX IK metadata model text is truncated: %s\n", fileName);
		return false;
	}

	int32_t vertexCount = 0;
	if (!reader.Read(vertexCount) || vertexCount < 0)
	{
		Debug::Log("WARNING: PMX IK metadata vertex count is invalid: %s\n", fileName);
		return false;
	}
	m_PmxBaseVertices.clear();
	m_PmxBaseNormals.clear();
	m_PmxBaseTexCoords.clear();
	m_PmxBaseVertices.resize(static_cast<size_t>(vertexCount));
	m_PmxBaseNormals.resize(static_cast<size_t>(vertexCount));
	m_PmxBaseTexCoords.resize(static_cast<size_t>(vertexCount));
	for (int32_t i = 0; i < vertexCount; ++i)
	{
		aiVector3D position{};
		aiVector3D normal{};
		XMFLOAT2 uv{};
		if (!readFloat3(reader, position) ||
			!readFloat3(reader, normal) ||
			!reader.Read(uv.x) ||
			!reader.Read(uv.y) ||
			!reader.Skip(static_cast<size_t>(reader.AdditionalUvCount) * 16))
		{
			Debug::Log("WARNING: PMX IK metadata vertex data is truncated: %s\n", fileName);
			return false;
		}
		m_PmxBaseVertices[static_cast<size_t>(i)] = ConvertVmdPositionToAssimpLeftHanded(position);
		m_PmxBaseNormals[static_cast<size_t>(i)] = ConvertVmdPositionToAssimpLeftHanded(normal);
		m_PmxBaseTexCoords[static_cast<size_t>(i)] = uv;

		uint8_t deformType = 0;
		if (!reader.Read(deformType))
		{
			return false;
		}
		switch (deformType)
		{
		case 0:
			if (!reader.SkipIndex(reader.BoneIndexSize)) return false;
			break;
		case 1:
			if (!reader.Skip(static_cast<size_t>(reader.BoneIndexSize) * 2 + 4)) return false;
			break;
		case 2:
			if (!reader.Skip(static_cast<size_t>(reader.BoneIndexSize) * 4 + 16)) return false;
			break;
		case 3:
			if (!reader.Skip(static_cast<size_t>(reader.BoneIndexSize) * 2 + 40)) return false;
			break;
		case 4:
			if (!reader.Skip(static_cast<size_t>(reader.BoneIndexSize) * 4 + 16)) return false;
			break;
		default:
			Debug::Log("WARNING: PMX IK metadata unknown deform type %u: %s\n", deformType, fileName);
			return false;
		}
		if (!reader.Skip(4))
		{
			return false;
		}
	}

	int32_t indexCount = 0;
	if (!reader.Read(indexCount) || indexCount < 0 ||
		!reader.Skip(static_cast<size_t>(indexCount) * reader.VertexIndexSize))
	{
		Debug::Log("WARNING: PMX IK metadata index data is truncated: %s\n", fileName);
		return false;
	}

	int32_t textureCount = 0;
	if (!reader.Read(textureCount) || textureCount < 0)
	{
		return false;
	}
	for (int32_t i = 0; i < textureCount; ++i)
	{
		if (!reader.ReadText(ignoredText)) return false;
	}

	int32_t materialCount = 0;
	if (!reader.Read(materialCount) || materialCount < 0)
	{
		return false;
	}
	for (int32_t i = 0; i < materialCount; ++i)
	{
		if (!reader.ReadText(ignoredText) || !reader.ReadText(ignoredText) ||
			!reader.Skip(65) ||
			!reader.SkipIndex(reader.TextureIndexSize) ||
			!reader.SkipIndex(reader.TextureIndexSize))
		{
			return false;
		}

		uint8_t sphereMode = 0;
		uint8_t toonFlag = 0;
		if (!reader.Read(sphereMode) || !reader.Read(toonFlag))
		{
			return false;
		}
		if (toonFlag == 0)
		{
			if (!reader.SkipIndex(reader.TextureIndexSize)) return false;
		}
		else
		{
			if (!reader.Skip(1)) return false;
		}
		if (!reader.ReadText(ignoredText) || !reader.Skip(4))
		{
			return false;
		}
	}

	int32_t boneCount = 0;
	if (!reader.Read(boneCount) || boneCount < 0)
	{
		Debug::Log("WARNING: PMX IK metadata bone count is invalid: %s\n", fileName);
		return false;
	}

	vector<RawBone> rawBones(static_cast<size_t>(boneCount));
	for (int32_t i = 0; i < boneCount; ++i)
	{
		RawBone& rawBone = rawBones[static_cast<size_t>(i)];
		string englishName;
		aiVector3D ignoredVec{};
		int32_t ignoredIndex = -1;
		if (!reader.ReadText(rawBone.Name) ||
			!reader.ReadText(englishName) ||
			!readFloat3(reader, ignoredVec) ||
			!reader.ReadIndex(reader.BoneIndexSize, ignoredIndex) ||
			!reader.Read(rawBone.DeformDepth) ||
			!reader.Read(rawBone.Flags))
		{
			Debug::Log("WARNING: PMX IK metadata bone data is truncated: %s\n", fileName);
			return false;
		}

		if ((rawBone.Flags & 0x0001) != 0)
		{
			if (!reader.ReadIndex(reader.BoneIndexSize, ignoredIndex)) return false;
		}
		else if (!reader.Skip(12))
		{
			return false;
		}

		if ((rawBone.Flags & 0x0100) != 0 || (rawBone.Flags & 0x0200) != 0)
		{
			if (!reader.ReadIndex(reader.BoneIndexSize, rawBone.AppendBoneIndex) ||
				!reader.Read(rawBone.AppendWeight))
			{
				return false;
			}
		}
		if ((rawBone.Flags & 0x0400) != 0 && !reader.Skip(12)) return false;
		if ((rawBone.Flags & 0x0800) != 0 && !reader.Skip(24)) return false;
		if ((rawBone.Flags & 0x2000) != 0 && !reader.Skip(4)) return false;

		if ((rawBone.Flags & 0x0020) != 0)
		{
			int32_t linkCount = 0;
			if (!reader.ReadIndex(reader.BoneIndexSize, rawBone.IkTargetIndex) ||
				!reader.Read(rawBone.IkIterationCount) ||
				!reader.Read(rawBone.IkLimitAngle) ||
				!reader.Read(linkCount) ||
				linkCount < 0)
			{
				return false;
			}

			rawBone.IkLinks.resize(static_cast<size_t>(linkCount));
			for (int32_t link = 0; link < linkCount; ++link)
			{
				RawIkLink& rawLink = rawBone.IkLinks[static_cast<size_t>(link)];
				uint8_t hasLimit = 0;
				if (!reader.ReadIndex(reader.BoneIndexSize, rawLink.BoneIndex) || !reader.Read(hasLimit))
				{
					return false;
				}
				rawLink.HasLimit = hasLimit != 0;
				if (rawLink.HasLimit)
				{
					if (!readFloat3(reader, rawLink.LimitMin) || !readFloat3(reader, rawLink.LimitMax))
					{
						return false;
					}
				}
			}
		}
	}

	int32_t morphCount = 0;
	if (!reader.Read(morphCount) || morphCount < 0)
	{
		Debug::Log("WARNING: PMX morph metadata count is invalid: %s\n", fileName);
		return false;
	}

	m_PmxMorphs.clear();
	m_PmxMorphIndexMap.clear();
	m_PmxMorphs.resize(static_cast<size_t>(morphCount));
	for (int32_t i = 0; i < morphCount; ++i)
	{
		PmxMorph& morph = m_PmxMorphs[static_cast<size_t>(i)];
		string englishName;
		uint8_t panel = 0;
		int32_t offsetCount = 0;
		if (!reader.ReadText(morph.Name) ||
			!reader.ReadText(englishName) ||
			!reader.Read(panel) ||
			!reader.Read(morph.Type) ||
			!reader.Read(offsetCount) ||
			offsetCount < 0)
		{
			Debug::Log("WARNING: PMX morph metadata is truncated: %s\n", fileName);
			return false;
		}
		if (!morph.Name.empty())
		{
			m_PmxMorphIndexMap[morph.Name] = static_cast<uint32_t>(i);
		}

		for (int32_t offset = 0; offset < offsetCount; ++offset)
		{
			switch (morph.Type)
			{
			case 0:
			{
				int32_t morphIndex = -1;
				float weight = 0.0f;
				if (!reader.ReadIndex(reader.MorphIndexSize, morphIndex) || !reader.Read(weight))
				{
					return false;
				}
				if (morphIndex >= 0)
				{
					morph.GroupOffsets.push_back({ static_cast<uint32_t>(morphIndex), weight });
				}
				break;
			}
			case 1:
			{
				uint32_t vertexIndex = 0;
				aiVector3D position{};
				if (!reader.ReadUnsignedIndex(reader.VertexIndexSize, vertexIndex) ||
					!readFloat3(reader, position))
				{
					return false;
				}
				morph.PositionOffsets.push_back({ vertexIndex, ConvertVmdPositionToAssimpLeftHanded(position) });
				break;
			}
			case 2:
			{
				int32_t boneIndex = -1;
				aiVector3D position{};
				float qx = 0.0f;
				float qy = 0.0f;
				float qz = 0.0f;
				float qw = 1.0f;
				if (!reader.ReadIndex(reader.BoneIndexSize, boneIndex) ||
					!readFloat3(reader, position) ||
					!reader.Read(qx) ||
					!reader.Read(qy) ||
					!reader.Read(qz) ||
					!reader.Read(qw))
				{
					return false;
				}
				if (boneIndex >= 0 && boneIndex < boneCount)
				{
					PmxBoneMorphOffset boneOffset{};
					boneOffset.BoneName = rawBones[static_cast<size_t>(boneIndex)].Name;
					boneOffset.Position = ConvertVmdPositionToAssimpLeftHanded(position);
					boneOffset.Rotation = ConvertVmdRotationToAssimpLeftHanded(aiQuaternion(qw, qx, qy, qz));
					morph.BoneOffsets.push_back(boneOffset);
				}
				break;
			}
			case 3:
			{
				uint32_t vertexIndex = 0;
				XMFLOAT4 uv{};
				if (!reader.ReadUnsignedIndex(reader.VertexIndexSize, vertexIndex) ||
					!readFloat4(reader, uv))
				{
					return false;
				}
				morph.UvOffsets.push_back({ vertexIndex, uv });
				break;
			}
			case 4:
			case 5:
			case 6:
			case 7:
				if (!reader.SkipIndex(reader.VertexIndexSize) || !reader.Skip(16)) return false;
				break;
			case 8:
			{
				PmxMaterialMorphOffset materialOffset{};
				XMFLOAT4 ignored{};
				float ignoredFloat = 0.0f;
				if (!reader.ReadIndex(reader.MaterialIndexSize, materialOffset.MaterialIndex) ||
					!reader.Read(materialOffset.Operation) ||
					!readFloat4(reader, materialOffset.Diffuse) ||
					!reader.Skip(12) ||
					!reader.Read(ignoredFloat) ||
					!reader.Skip(12) ||
					!readFloat4(reader, ignored) ||
					!reader.Read(ignoredFloat) ||
					!readFloat4(reader, ignored) ||
					!readFloat4(reader, ignored) ||
					!readFloat4(reader, ignored))
				{
					return false;
				}
				morph.MaterialOffsets.push_back(materialOffset);
				break;
			}
			break;
			case 9:
				if (!reader.SkipIndex(reader.MorphIndexSize) || !reader.Skip(4)) return false;
				break;
			case 10:
				if (!reader.SkipIndex(reader.RigidBodyIndexSize) || !reader.Skip(25)) return false;
				break;
			default:
				Debug::Log("WARNING: PMX morph metadata unknown morph type %u: %s\n", morph.Type, fileName);
				return false;
			}
		}
	}

	m_PmxAppendConstraints.clear();
	for (int32_t i = 0; i < boneCount; ++i)
	{
		const RawBone& rawBone = rawBones[static_cast<size_t>(i)];
		if (((rawBone.Flags & 0x0100) == 0 && (rawBone.Flags & 0x0200) == 0) ||
			rawBone.AppendBoneIndex < 0 ||
			rawBone.AppendBoneIndex >= boneCount)
		{
			continue;
		}

		PmxAppendConstraint constraint{};
		constraint.BoneName = rawBone.Name;
		constraint.AppendBoneName = rawBones[static_cast<size_t>(rawBone.AppendBoneIndex)].Name;
		constraint.Weight = rawBone.AppendWeight;
		constraint.InheritRotation = (rawBone.Flags & 0x0100) != 0;
		constraint.InheritTranslation = (rawBone.Flags & 0x0200) != 0;
		constraint.Local = (rawBone.Flags & 0x0080) != 0;
		constraint.DeformDepth = rawBone.DeformDepth;
		constraint.BoneOrder = static_cast<uint32_t>(i);
		if (!constraint.BoneName.empty() && !constraint.AppendBoneName.empty())
		{
			m_PmxAppendConstraints.push_back(std::move(constraint));
		}
	}

	m_PmxIkConstraints.clear();
	for (int32_t i = 0; i < boneCount; ++i)
	{
		const RawBone& rawBone = rawBones[static_cast<size_t>(i)];
		if ((rawBone.Flags & 0x0020) == 0 ||
			rawBone.IkTargetIndex < 0 ||
			rawBone.IkTargetIndex >= boneCount)
		{
			continue;
		}

		PmxIkConstraint constraint{};
		constraint.BoneName = rawBone.Name;
		constraint.TargetBoneName = rawBones[static_cast<size_t>(rawBone.IkTargetIndex)].Name;
		constraint.IterationCount = rawBone.IkIterationCount;
		constraint.LimitAngle = rawBone.IkLimitAngle;
		constraint.DeformDepth = rawBone.DeformDepth;
		constraint.BoneOrder = static_cast<uint32_t>(i);
		for (const RawIkLink& rawLink : rawBone.IkLinks)
		{
			if (rawLink.BoneIndex < 0 || rawLink.BoneIndex >= boneCount)
			{
				continue;
			}

			PmxIkLink link{};
			link.BoneName = rawBones[static_cast<size_t>(rawLink.BoneIndex)].Name;
			link.HasLimit = rawLink.HasLimit;
			link.LimitMin = rawLink.LimitMin;
			link.LimitMax = rawLink.LimitMax;
			constraint.Links.push_back(link);
		}

		if (!constraint.BoneName.empty() && !constraint.TargetBoneName.empty() && !constraint.Links.empty())
		{
			m_PmxIkConstraints.push_back(std::move(constraint));
		}
	}

	auto comparePmxTransformOrder = [](const auto& lhs, const auto& rhs)
		{
			if (lhs.DeformDepth != rhs.DeformDepth)
			{
				return lhs.DeformDepth < rhs.DeformDepth;
			}
			return lhs.BoneOrder < rhs.BoneOrder;
		};
	sort(m_PmxAppendConstraints.begin(), m_PmxAppendConstraints.end(), comparePmxTransformOrder);
	sort(m_PmxIkConstraints.begin(), m_PmxIkConstraints.end(), comparePmxTransformOrder);

	size_t positionMorphCount = 0;
	size_t uvMorphCount = 0;
	size_t boneMorphCount = 0;
	size_t materialMorphCount = 0;
	size_t groupMorphCount = 0;
	for (const PmxMorph& morph : m_PmxMorphs)
	{
		if (!morph.PositionOffsets.empty()) ++positionMorphCount;
		if (!morph.UvOffsets.empty()) ++uvMorphCount;
		if (!morph.BoneOffsets.empty()) ++boneMorphCount;
		if (!morph.MaterialOffsets.empty()) ++materialMorphCount;
		if (!morph.GroupOffsets.empty()) ++groupMorphCount;
	}

	Debug::Log("PMX animation metadata loaded: %s (vertices=%zu, bones=%d, appendConstraints=%zu, ikConstraints=%zu, morphs=%zu, positionMorphs=%zu, uvMorphs=%zu, boneMorphs=%zu, materialMorphs=%zu, groupMorphs=%zu)\n",
		fileName, m_PmxBaseVertices.size(), boneCount, m_PmxAppendConstraints.size(), m_PmxIkConstraints.size(),
		m_PmxMorphs.size(), positionMorphCount, uvMorphCount, boneMorphCount, materialMorphCount, groupMorphCount);
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

aiNodeAnim* AnimationModelResource::FindNodeAnimChannel(aiAnimation* animation, const string& boneName) const
{
	if (!animation)
	{
		return nullptr;
	}

	for (unsigned int c = 0; c < animation->mNumChannels; ++c)
	{
		if (animation->mChannels[c]->mNodeName == aiString(boneName))
		{
			return animation->mChannels[c];
		}
	}
	return nullptr;
}

void AnimationModelResource::SampleNodeAnimation(aiAnimation* animation, aiNodeAnim* channel, float timeSeconds,
	aiQuaternion& outRotation, aiVector3D& outPosition, aiVector3D& outScale) const
{
	if (!animation || !channel)
	{
		return;
	}

	const float ticksPerSecond = (animation->mTicksPerSecond != 0) ? static_cast<float>(animation->mTicksPerSecond) : 25.0f;
	const float duration = static_cast<float>(animation->mDuration);
	const float t = duration > 0.0f ? fmod(max(0.0f, timeSeconds) * ticksPerSecond, duration) : 0.0f;

	auto sampleVector = [t](const aiVectorKey* keys, unsigned int count, aiVector3D& output)
		{
			if (!keys || count == 0) return;
			if (count == 1 || t <= static_cast<float>(keys[0].mTime))
			{
				output = keys[0].mValue;
				return;
			}
			for (unsigned int i = 0; i + 1 < count; ++i)
			{
				const float start = static_cast<float>(keys[i].mTime);
				const float end = static_cast<float>(keys[i + 1].mTime);
				if (t <= end)
				{
					const float factor = end > start ? clamp((t - start) / (end - start), 0.0f, 1.0f) : 0.0f;
					output = keys[i].mValue * (1.0f - factor) + keys[i + 1].mValue * factor;
					return;
				}
			}
			output = keys[count - 1].mValue;
		};

	auto sampleRotation = [t](const aiQuatKey* keys, unsigned int count, aiQuaternion& output)
		{
			if (!keys || count == 0) return;
			if (count == 1 || t <= static_cast<float>(keys[0].mTime))
			{
				output = keys[0].mValue;
				return;
			}
			for (unsigned int i = 0; i + 1 < count; ++i)
			{
				const float start = static_cast<float>(keys[i].mTime);
				const float end = static_cast<float>(keys[i + 1].mTime);
				if (t <= end)
				{
					const float factor = end > start ? clamp((t - start) / (end - start), 0.0f, 1.0f) : 0.0f;
					aiQuaternion::Interpolate(output, keys[i].mValue, keys[i + 1].mValue, factor);
					output.Normalize();
					return;
				}
			}
			output = keys[count - 1].mValue;
		};

	sampleRotation(channel->mRotationKeys, channel->mNumRotationKeys, outRotation);
	sampleVector(channel->mPositionKeys, channel->mNumPositionKeys, outPosition);
	sampleVector(channel->mScalingKeys, channel->mNumScalingKeys, outScale);
}

bool AnimationModelResource::CreateGpuSkinningBuffers(ID3D12Device* device)
{
	UINT numMeshes = (UINT)m_Meshes.size();

	{
		UINT boneBufferSize = sizeof(XMFLOAT4X4) * m_kMAX_BONES;
		auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		auto resDesc = CD3DX12_RESOURCE_DESC::Buffer(boneBufferSize);

		vector<XMFLOAT4X4> identityBones(m_kMAX_BONES);
		XMFLOAT4X4 identity;
		XMStoreFloat4x4(&identity, XMMatrixIdentity());
		for (auto& mtx : identityBones)
		{
			mtx = identity;
		}

		for (UINT frame = 0; frame < RendererState::g_kFRAME_COUNT; ++frame)
		{
			HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
				&resDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_BoneBuffers[frame]));
			if (FAILED(hr)) return false;
			CD3DX12_RANGE readRange(0, 0);
			hr = m_BoneBuffers[frame]->Map(0, &readRange, &m_pBoneBufferMapped[frame]);
			if (FAILED(hr))
			{
				Debug::Log("ERROR: Failed to map bone buffer\n");
				m_pBoneBufferMapped[frame] = nullptr;
				return false;
			}

			if (m_pBoneBufferMapped[frame])
			{
				memcpy(m_pBoneBufferMapped[frame], identityBones.data(), sizeof(XMFLOAT4X4) * m_kMAX_BONES);
			}
		}
	}

	{
		D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
		heapDesc.NumDescriptors = numMeshes * m_kSKINNING_DESCRIPTORS_PER_MESH;
		heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		HRESULT hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_SkinningDescHeap));
		if (FAILED(hr)) return false;
	}

	UINT descSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	for (UINT m = 0; m < numMeshes; m++)
	{
		UINT vertexCount = m_Meshes[m].VertexCount;
		const UINT descriptorBase = m * m_kSKINNING_DESCRIPTORS_PER_MESH;

		{
			UINT bufSize = sizeof(GpuSkinVertex) * vertexCount;
			auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
			auto resDesc = CD3DX12_RESOURCE_DESC::Buffer(bufSize);
			HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
				&resDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_Meshes[m].InputVertexBuffer));
			if (FAILED(hr)) return false;

			UINT8* pDest = nullptr;
			CD3DX12_RANGE readRange(0, 0);
			hr = m_Meshes[m].InputVertexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pDest));
			if (FAILED(hr) || !pDest)
			{
				Debug::Log("ERROR: Failed to map input vertex buffer\n");
				return false;
			}
			memcpy(pDest, m_GpuSkinVertices[m].data(), bufSize);
			m_Meshes[m].InputVertexBuffer->Unmap(0, nullptr);

			D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srvDesc.Buffer.NumElements = vertexCount;
			srvDesc.Buffer.StructureByteStride = sizeof(GpuSkinVertex);
			srvDesc.Format = DXGI_FORMAT_UNKNOWN;
			m_Meshes[m].SrvInputVertexIndex = descriptorBase + m_kINPUT_VERTEX_SRV_OFFSET;
			CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(m_SkinningDescHeap->GetCPUDescriptorHandleForHeapStart(),
				m_Meshes[m].SrvInputVertexIndex, descSize);
			device->CreateShaderResourceView(m_Meshes[m].InputVertexBuffer.Get(), &srvDesc, srvHandle);
		}

		for (UINT frame = 0; frame < RendererState::g_kFRAME_COUNT; ++frame)
		{
			D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srvDesc.Buffer.NumElements = m_kMAX_BONES;
			srvDesc.Buffer.StructureByteStride = sizeof(XMFLOAT4X4);
			srvDesc.Format = DXGI_FORMAT_UNKNOWN;
			CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(m_SkinningDescHeap->GetCPUDescriptorHandleForHeapStart(),
				descriptorBase + m_kBONE_SRV_OFFSET + frame, descSize);
			device->CreateShaderResourceView(m_BoneBuffers[frame].Get(), &srvDesc, srvHandle);
		}

		{
			UINT bufSize = sizeof(ModelVertex) * vertexCount;
			auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
			auto resDesc = CD3DX12_RESOURCE_DESC::Buffer(bufSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
			HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
				&resDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_Meshes[m].VertexBuffer));
			if (FAILED(hr)) return false;
			hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
				&resDesc, D3D12_RESOURCE_STATE_COMMON, nullptr,
				IID_PPV_ARGS(&m_Meshes[m].PreviousVertexBuffer));
			if (FAILED(hr)) return false;

			D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
			uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
			uavDesc.Buffer.NumElements = vertexCount;
			uavDesc.Buffer.StructureByteStride = sizeof(ModelVertex);
			uavDesc.Format = DXGI_FORMAT_UNKNOWN;
			m_Meshes[m].UavOutputVertexIndex = descriptorBase + m_kOUTPUT_VERTEX_UAV_OFFSET;
			CD3DX12_CPU_DESCRIPTOR_HANDLE uavHandle(m_SkinningDescHeap->GetCPUDescriptorHandleForHeapStart(),
				m_Meshes[m].UavOutputVertexIndex, descSize);
			device->CreateUnorderedAccessView(m_Meshes[m].VertexBuffer.Get(), nullptr, &uavDesc, uavHandle);

			m_Meshes[m].VertexBufferView.BufferLocation = m_Meshes[m].VertexBuffer->GetGPUVirtualAddress();
			m_Meshes[m].VertexBufferView.SizeInBytes = bufSize;
			m_Meshes[m].VertexBufferView.StrideInBytes = sizeof(ModelVertex);
			m_Meshes[m].PreviousVertexBufferView.BufferLocation = m_Meshes[m].PreviousVertexBuffer->GetGPUVirtualAddress();
			m_Meshes[m].PreviousVertexBufferView.SizeInBytes = bufSize;
			m_Meshes[m].PreviousVertexBufferView.StrideInBytes = sizeof(ModelVertex);
		}

		for (int mode = 0; mode < kToonOutlineModeCount; ++mode)
		{
			const UINT teoVertexCount = m_Meshes[m].TeoVertexCounts[mode];
			if (teoVertexCount == 0 || m_TeoGpuSkinVerticesByMode[m][mode].empty())
			{
				continue;
			}

			{
				UINT bufSize = sizeof(GpuSkinVertex) * teoVertexCount;
				auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
				auto resDesc = CD3DX12_RESOURCE_DESC::Buffer(bufSize);
				HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
					&resDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_Meshes[m].TeoInputVertexBuffers[mode]));
				if (FAILED(hr)) return false;

				UINT8* pDest = nullptr;
				CD3DX12_RANGE readRange(0, 0);
				hr = m_Meshes[m].TeoInputVertexBuffers[mode]->Map(0, &readRange, reinterpret_cast<void**>(&pDest));
				if (FAILED(hr) || !pDest)
				{
					Debug::Log("ERROR: Failed to map TEO input vertex buffer\n");
					return false;
				}
				memcpy(pDest, m_TeoGpuSkinVerticesByMode[m][mode].data(), bufSize);
				m_Meshes[m].TeoInputVertexBuffers[mode]->Unmap(0, nullptr);

				D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
				srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
				srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				srvDesc.Buffer.NumElements = teoVertexCount;
				srvDesc.Buffer.StructureByteStride = sizeof(GpuSkinVertex);
				srvDesc.Format = DXGI_FORMAT_UNKNOWN;
				m_Meshes[m].SrvTeoInputVertexIndices[mode] = descriptorBase + m_kTEO_DESCRIPTOR_OFFSET + (mode * 2);
				CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(m_SkinningDescHeap->GetCPUDescriptorHandleForHeapStart(),
					m_Meshes[m].SrvTeoInputVertexIndices[mode], descSize);
				device->CreateShaderResourceView(m_Meshes[m].TeoInputVertexBuffers[mode].Get(), &srvDesc, srvHandle);
			}

			{
				UINT bufSize = sizeof(ModelVertex) * teoVertexCount;
				auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
				auto resDesc = CD3DX12_RESOURCE_DESC::Buffer(bufSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
				HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
					&resDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_Meshes[m].TeoVertexBuffers[mode]));
				if (FAILED(hr)) return false;

				D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
				uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
				uavDesc.Buffer.NumElements = teoVertexCount;
				uavDesc.Buffer.StructureByteStride = sizeof(ModelVertex);
				uavDesc.Format = DXGI_FORMAT_UNKNOWN;
				m_Meshes[m].UavTeoOutputVertexIndices[mode] = descriptorBase + m_kTEO_DESCRIPTOR_OFFSET + (mode * 2) + 1;
				CD3DX12_CPU_DESCRIPTOR_HANDLE uavHandle(m_SkinningDescHeap->GetCPUDescriptorHandleForHeapStart(),
					m_Meshes[m].UavTeoOutputVertexIndices[mode], descSize);
				device->CreateUnorderedAccessView(m_Meshes[m].TeoVertexBuffers[mode].Get(), nullptr, &uavDesc, uavHandle);

				m_Meshes[m].TeoVertexBufferViews[mode].BufferLocation = m_Meshes[m].TeoVertexBuffers[mode]->GetGPUVirtualAddress();
				m_Meshes[m].TeoVertexBufferViews[mode].SizeInBytes = bufSize;
				m_Meshes[m].TeoVertexBufferViews[mode].StrideInBytes = sizeof(ModelVertex);

				if (mode == static_cast<int>(ToonOutlineMeshMode::Balanced))
				{
					m_Meshes[m].TeoInputVertexBuffer = m_Meshes[m].TeoInputVertexBuffers[mode];
					m_Meshes[m].TeoVertexBuffer = m_Meshes[m].TeoVertexBuffers[mode];
					m_Meshes[m].TeoVertexBufferView = m_Meshes[m].TeoVertexBufferViews[mode];
					m_Meshes[m].SrvTeoInputVertexIndex = m_Meshes[m].SrvTeoInputVertexIndices[mode];
					m_Meshes[m].UavTeoOutputVertexIndex = m_Meshes[m].UavTeoOutputVertexIndices[mode];
				}
			}
		}
	}
	return true;
}

aiAnimation* AnimationModelResource::GetAnimation(const string& name)
{
	auto it = m_Animation.find(name);
	if (it != m_Animation.end() && it->second->HasAnimations())
	{
		return it->second->mAnimations[0];
	}
	if (m_AiScene && m_AiScene->HasAnimations())
	{
		if (name == "default" || name.empty())
		{
			return m_AiScene->mAnimations[0];
		}
		for (unsigned int i = 0; i < m_AiScene->mNumAnimations; ++i)
		{
			if (name == m_AiScene->mAnimations[i]->mName.C_Str())
			{
				return m_AiScene->mAnimations[i];
			}
		}
		if (m_AiScene->mNumAnimations == 1)
		{
			return m_AiScene->mAnimations[0];
		}
	}
	return nullptr;
}

bool AnimationModelResource::ApplyMeshShadingOverridePartIds(const vector<int>& overridePartIds)
{
	bool success = true;
	for (UINT meshIndex = 0; meshIndex < (UINT)m_Meshes.size(); ++meshIndex)
	{
		MeshData& meshData = m_Meshes[meshIndex];
		const float targetPartId =
			(meshIndex < overridePartIds.size() && overridePartIds[meshIndex] >= 0)
			? static_cast<float>(overridePartIds[meshIndex])
			: meshData.MaterialPartId;

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

void AnimationModelResource::UpdateBoneMatrix(aiNode* node, aiMatrix4x4 matrix)
{
	auto it = m_Bone.find(node->mName.C_Str());
	if (it == m_Bone.end())
	{
		for (unsigned int n = 0; n < node->mNumChildren; n++)
		{
			UpdateBoneMatrix(node->mChildren[n], matrix);
		}
		return;
	}

	Bone* bonePtr = &it->second;
	aiMatrix4x4 worldMatrix = matrix * bonePtr->AnimationMatrix;
	bonePtr->Matrix = worldMatrix * bonePtr->OffsetMatrix;

	for (unsigned int n = 0; n < node->mNumChildren; n++)
	{
		UpdateBoneMatrix(node->mChildren[n], worldMatrix);
	}
}

void AnimationModelResource::UpdateBindPoseBoneMatrix(aiNode* node, aiMatrix4x4 matrix)
{
	if (!node)
	{
		return;
	}

	aiMatrix4x4 worldMatrix = matrix * node->mTransformation;

	auto it = m_Bone.find(node->mName.C_Str());
	if (it != m_Bone.end())
	{
		Bone* bonePtr = &it->second;
		bonePtr->BindLocalMatrix = node->mTransformation;
		bonePtr->AnimationMatrix = node->mTransformation;
		bonePtr->Matrix = worldMatrix * bonePtr->OffsetMatrix;
	}

	for (unsigned int n = 0; n < node->mNumChildren; n++)
	{
		UpdateBindPoseBoneMatrix(node->mChildren[n], worldMatrix);
	}
}

void AnimationModelResource::WriteBoneMatricesToBuffer()
{
	bool hasMappedBoneBuffer = false;
	for (void* mapped : m_pBoneBufferMapped)
	{
		hasMappedBoneBuffer |= (mapped != nullptr);
	}
	if (!hasMappedBoneBuffer)
	{
		return;
	}

	if (m_BoneMatricesScratch.size() != m_kMAX_BONES)
	{
		m_BoneMatricesScratch.resize(m_kMAX_BONES);
	}

	XMFLOAT4X4 identity;
	XMStoreFloat4x4(&identity, XMMatrixIdentity());
	for (auto& mtx : m_BoneMatricesScratch)
	{
		mtx = identity;
	}

	for (const auto& kv : m_BoneIndexMap)
	{
		uint32_t idx = kv.second;
		if (idx >= m_kMAX_BONES) continue;
		XMFLOAT4X4& dst = m_BoneMatricesScratch[idx];
		const aiMatrix4x4& src = m_Bone.at(kv.first).Matrix;
		if (!IsUsableSkinningMatrix(src))
		{
			dst = identity;
			continue;
		}

		dst._11 = src.a1; dst._12 = src.a2; dst._13 = src.a3; dst._14 = src.a4;
		dst._21 = src.b1; dst._22 = src.b2; dst._23 = src.b3; dst._24 = src.b4;
		dst._31 = src.c1; dst._32 = src.c2; dst._33 = src.c3; dst._34 = src.c4;
		dst._41 = src.d1; dst._42 = src.d2; dst._43 = src.d3; dst._44 = src.d4;
	}

	const UINT frameIndex =
		RendererCore::GetFrameIndex() % RendererState::g_kFRAME_COUNT;
	void* mapped = m_pBoneBufferMapped[frameIndex];
	if (mapped)
	{
		const UINT copySize = sizeof(XMFLOAT4X4) * m_kMAX_BONES;
		memcpy(mapped, m_BoneMatricesScratch.data(), copySize);
	}








	++m_SkinningVersion;
}

bool AnimationModelResource::GetBoneGlobalTransform(const string& boneName, XMFLOAT4X4& transform)
{
	if (m_PmxRuntimeNodes.empty())
	{
		RebuildPmxRuntimeCache();
	}
	auto nodeIt = m_PmxRuntimeNodeIndexMap.find(boneName);
	if (nodeIt == m_PmxRuntimeNodeIndexMap.end())
	{
		return false;
	}
	if (m_PmxGlobalMatricesScratch.size() != m_PmxRuntimeNodes.size())
	{
		m_PmxGlobalMatricesScratch.resize(m_PmxRuntimeNodes.size());
	}

	auto aiToXm = [](const aiMatrix4x4& src)
		{
			return XMMatrixSet(
				src.a1, src.b1, src.c1, src.d1,
				src.a2, src.b2, src.c2, src.d2,
				src.a3, src.b3, src.c3, src.d3,
				src.a4, src.b4, src.c4, src.d4);
		};
	for (size_t i = 0; i < m_PmxRuntimeNodes.size(); ++i)
	{
		const PmxRuntimeNode& node = m_PmxRuntimeNodes[i];
		XMMATRIX local = node.BonePtr
			? aiToXm(node.BonePtr->AnimationMatrix)
			: aiToXm(node.Node->mTransformation);
		XMMATRIX global = local;
		if (node.ParentIndex >= 0)
		{
			global = XMMatrixMultiply(
				local,
				XMLoadFloat4x4(&m_PmxGlobalMatricesScratch[static_cast<size_t>(node.ParentIndex)]));
		}
		XMStoreFloat4x4(&m_PmxGlobalMatricesScratch[i], global);
	}
	transform = m_PmxGlobalMatricesScratch[nodeIt->second];
	return true;
}

bool AnimationModelResource::GetBoneBindGlobalTransform(
	const string& boneName,
	XMFLOAT4X4& transform)
{
	if (m_PmxRuntimeNodes.empty())
	{
		RebuildPmxRuntimeCache();
	}
	auto nodeIt = m_PmxRuntimeNodeIndexMap.find(boneName);
	if (nodeIt == m_PmxRuntimeNodeIndexMap.end())
	{
		return false;
	}
	if (m_PmxGlobalMatricesScratch.size() != m_PmxRuntimeNodes.size())
	{
		m_PmxGlobalMatricesScratch.resize(m_PmxRuntimeNodes.size());
	}

	auto aiToXm = [](const aiMatrix4x4& src)
		{
			return XMMatrixSet(
				src.a1, src.b1, src.c1, src.d1,
				src.a2, src.b2, src.c2, src.d2,
				src.a3, src.b3, src.c3, src.d3,
				src.a4, src.b4, src.c4, src.d4);
		};
	for (size_t i = 0; i < m_PmxRuntimeNodes.size(); ++i)
	{
		const PmxRuntimeNode& node = m_PmxRuntimeNodes[i];
		const XMMATRIX local = node.BonePtr
			? aiToXm(node.BonePtr->BindLocalMatrix)
			: aiToXm(node.Node->mTransformation);
		XMMATRIX global = local;
		if (node.ParentIndex >= 0)
		{
			global = XMMatrixMultiply(
				local,
				XMLoadFloat4x4(
					&m_PmxGlobalMatricesScratch[static_cast<size_t>(node.ParentIndex)]));
		}
		XMStoreFloat4x4(&m_PmxGlobalMatricesScratch[i], global);
	}
	transform = m_PmxGlobalMatricesScratch[nodeIt->second];
	return true;
}

bool AnimationModelResource::SetBoneGlobalTransform(
	const string& boneName,
	const XMFLOAT4X4& transform,
	bool preserveTranslation)
{
	XMFLOAT4X4 currentGlobal{};
	if (!GetBoneGlobalTransform(boneName, currentGlobal))
	{
		return false;
	}
	auto nodeIt = m_PmxRuntimeNodeIndexMap.find(boneName);
	if (nodeIt == m_PmxRuntimeNodeIndexMap.end())
	{
		return false;
	}
	PmxRuntimeNode& node = m_PmxRuntimeNodes[nodeIt->second];
	if (!node.BonePtr)
	{
		return false;
	}

	XMFLOAT4X4 target = transform;
	if (preserveTranslation)
	{
		target._41 = currentGlobal._41;
		target._42 = currentGlobal._42;
		target._43 = currentGlobal._43;
	}
	XMMATRIX parentGlobal = XMMatrixIdentity();
	if (node.ParentIndex >= 0)
	{
		parentGlobal = XMLoadFloat4x4(
			&m_PmxGlobalMatricesScratch[static_cast<size_t>(node.ParentIndex)]);
	}
	XMVECTOR determinant{};
	const XMMATRIX parentInverse = XMMatrixInverse(&determinant, parentGlobal);
	const XMMATRIX local = XMMatrixMultiply(XMLoadFloat4x4(&target), parentInverse);

	XMVECTOR scale{};
	XMVECTOR rotation{};
	XMVECTOR translation{};
	if (!XMMatrixDecompose(&scale, &rotation, &translation, local))
	{
		return false;
	}
	XMFLOAT3 localScale{};
	XMFLOAT3 localPosition{};
	XMFLOAT4 localRotation{};
	XMStoreFloat3(&localScale, scale);
	XMStoreFloat3(&localPosition, translation);
	XMStoreFloat4(&localRotation, XMQuaternionNormalize(rotation));
	node.BonePtr->AnimationMatrix = aiMatrix4x4(
		aiVector3D(localScale.x, localScale.y, localScale.z),
		aiQuaternion(localRotation.w, localRotation.x, localRotation.y, localRotation.z),
		aiVector3D(localPosition.x, localPosition.y, localPosition.z));
	return true;
}

void AnimationModelResource::CommitPhysicsPose()
{
	if (!m_AiScene || !m_AiScene->mRootNode)
	{
		return;
	}
	UpdateBoneMatrix(m_AiScene->mRootNode, MakeAiIdentityMatrix());
	WriteBoneMatricesToBuffer();
}

void AnimationModelResource::InvalidateAnimationPoseCache()
{
	m_HasCachedPose = false;
	m_HasCachedLayeredPose = false;
	m_UseLayeredVmdIk = false;
}

void AnimationModelResource::ResetBoneMatricesToBindPose()
{
	if (!m_AiScene || !m_AiScene->mRootNode)
	{
		return;
	}
	InvalidateAnimationPoseCache();
	UpdateBindPoseBoneMatrix(m_AiScene->mRootNode, MakeAiIdentityMatrix());
	WriteBoneMatricesToBuffer();
}

void AnimationModelResource::BuildPmxVertexMeshMap()
{
	m_PmxVertexToMeshVertices.clear();
	if (m_PmxBaseVertices.empty() || m_GpuSkinVertices.empty())
	{
		return;
	}

	auto quantize = [](float value)
		{
			return llround(static_cast<double>(value) * 10000.0);
		};
	auto makePositionKey = [&](float x, float y, float z)
		{
			return to_string(quantize(x)) + "," + to_string(quantize(y)) + "," + to_string(quantize(z));
		};
	auto makeVertexKey = [&](const aiVector3D& position, const aiVector3D& normal, const XMFLOAT2& uv)
		{
			return makePositionKey(position.x, position.y, position.z) + "|" +
				makePositionKey(normal.x, normal.y, normal.z) + "|" +
				to_string(quantize(uv.x)) + "," + to_string(quantize(uv.y));
		};

	unordered_map<string, vector<uint32_t>> pmxVerticesByPosition;
	unordered_map<string, vector<uint32_t>> pmxVerticesByVertex;
	pmxVerticesByPosition.reserve(m_PmxBaseVertices.size());
	pmxVerticesByVertex.reserve(m_PmxBaseVertices.size());
	for (uint32_t i = 0; i < static_cast<uint32_t>(m_PmxBaseVertices.size()); ++i)
	{
		const aiVector3D& position = m_PmxBaseVertices[i];
		pmxVerticesByPosition[makePositionKey(position.x, position.y, position.z)].push_back(i);
		if (i < m_PmxBaseNormals.size() && i < m_PmxBaseTexCoords.size())
		{
			pmxVerticesByVertex[makeVertexKey(position, m_PmxBaseNormals[i], m_PmxBaseTexCoords[i])].push_back(i);
		}
	}

	m_PmxVertexToMeshVertices.resize(m_PmxBaseVertices.size());
	size_t mappedMeshVertices = 0;
	size_t preciseMappedMeshVertices = 0;
	for (uint32_t meshIndex = 0; meshIndex < static_cast<uint32_t>(m_GpuSkinVertices.size()); ++meshIndex)
	{
		const vector<GpuSkinVertex>& vertices = m_GpuSkinVertices[meshIndex];
		for (uint32_t vertexIndex = 0; vertexIndex < static_cast<uint32_t>(vertices.size()); ++vertexIndex)
		{
			const XMFLOAT3& position = vertices[vertexIndex].Position;
			const XMFLOAT3& normal = vertices[vertexIndex].Normal;
			const XMFLOAT2& uv = vertices[vertexIndex].TexCoord;

			aiVector3D aiPosition(position.x, position.y, position.z);
			aiVector3D aiNormal(normal.x, normal.y, normal.z);
			const vector<uint32_t>* matchedPmxVertices = nullptr;
			auto preciseIt = pmxVerticesByVertex.find(makeVertexKey(aiPosition, aiNormal, uv));
			if (preciseIt != pmxVerticesByVertex.end())
			{
				matchedPmxVertices = &preciseIt->second;
				++preciseMappedMeshVertices;
			}
			else
			{
				auto positionIt = pmxVerticesByPosition.find(makePositionKey(position.x, position.y, position.z));
				if (positionIt != pmxVerticesByPosition.end())
				{
					matchedPmxVertices = &positionIt->second;
				}
			}
			if (!matchedPmxVertices)
			{
				continue;
			}

			++mappedMeshVertices;
			for (uint32_t pmxVertexIndex : *matchedPmxVertices)
			{
				m_PmxVertexToMeshVertices[pmxVertexIndex].push_back({ meshIndex, vertexIndex });
			}
		}
	}

	Debug::Log("PMX morph vertex map built: pmxVertices=%zu, mappedMeshVertices=%zu, preciseMappedMeshVertices=%zu\n",
		m_PmxBaseVertices.size(), mappedMeshVertices, preciseMappedMeshVertices);
}

float AnimationModelResource::SampleVmdMorph(const VmdAnimation* animation, const string& morphName, float timeSeconds) const
{
	if (!animation)
	{
		return 0.0f;
	}

	auto trackIt = animation->MorphTracks.find(morphName);
	if (trackIt == animation->MorphTracks.end())
	{
		return 0.0f;
	}

	const float currentFrame = VmdToFrameTime(animation, timeSeconds);
	return SampleVmdMorphTrack(&trackIt->second, currentFrame);
}

void AnimationModelResource::ApplyVmdMorphs(const VmdAnimation* animation, float currentFrame)
{
	if (!animation || m_PmxMorphs.empty())
	{
		return;
	}

	if (animation != m_CachedVmdPrimaryAnimation)
	{
		RebuildVmdRuntimeCache(animation, m_CachedVmdSecondaryAnimation);
	}

	m_VmdActiveMorphsScratch.clear();
	m_VmdActiveMorphsScratch.reserve(m_VmdMorphBindings.size());
	for (VmdMorphBinding& binding : m_VmdMorphBindings)
	{
		const float weight = SampleVmdMorphTrackCached(binding.Track, currentFrame, binding.Cursor);
		if (fabsf(weight) > 0.00001f)
		{
			m_VmdActiveMorphsScratch.push_back({ binding.MorphIndex, weight });
		}
	}
	ApplyActiveVmdMorphs();
}

void AnimationModelResource::ApplyVmdMorphLayers()
{
	if (m_PmxMorphs.empty())
	{
		return;
	}

	m_VmdActiveMorphsScratch.clear();
	for (VmdLayeredMorphBinding& binding : m_VmdLayeredMorphBindings)
	{
		if (binding.LayerIndex >= m_VmdLayerFramesScratch.size())
		{
			continue;
		}
		const float weight = SampleVmdMorphTrackCached(
			binding.Track, m_VmdLayerFramesScratch[binding.LayerIndex], binding.Cursor);
		if (fabsf(weight) > 0.00001f)
		{
			m_VmdActiveMorphsScratch.push_back({ binding.MorphIndex, weight });
		}
	}
	ApplyActiveVmdMorphs();
}

void AnimationModelResource::ApplyActiveVmdMorphs()
{

	if (m_VmdActiveMorphsScratch.empty() && !m_HasAppliedVmdMorphs)
	{
		return;
	}

	const bool canApplyVertexMorphs =
		!m_PmxVertexToMeshVertices.empty() &&
		!m_BaseGpuSkinVertices.empty() &&
		m_BaseGpuSkinVertices.size() == m_GpuSkinVertices.size();

	if (canApplyVertexMorphs)
	{
		for (size_t meshIndex = 0; meshIndex < m_GpuSkinVertices.size(); ++meshIndex)
		{
			m_GpuSkinVertices[meshIndex] = m_BaseGpuSkinVertices[meshIndex];
		}
	}

	if (m_VmdMorphPositionOffsetsScratch.size() != m_PmxBaseVertices.size())
	{
		m_VmdMorphPositionOffsetsScratch.resize(m_PmxBaseVertices.size());
		m_VmdMorphUvOffsetsScratch.resize(m_PmxBaseVertices.size());
	}
	fill(m_VmdMorphPositionOffsetsScratch.begin(), m_VmdMorphPositionOffsetsScratch.end(), aiVector3D(0.0f, 0.0f, 0.0f));
	fill(m_VmdMorphUvOffsetsScratch.begin(), m_VmdMorphUvOffsetsScratch.end(), XMFLOAT2(0.0f, 0.0f));

	vector<aiVector3D>& accumulatedOffsets = m_VmdMorphPositionOffsetsScratch;
	vector<XMFLOAT2>& accumulatedUvOffsets = m_VmdMorphUvOffsetsScratch;
	bool vertexBufferChanged = false;
	auto accumulateMorph = [&](auto&& self, uint32_t morphIndex, float weight, int depth) -> void
		{
			if (weight == 0.0f || morphIndex >= m_PmxMorphs.size() || depth > 8)
			{
				return;
			}

			const PmxMorph& morph = m_PmxMorphs[morphIndex];
			if (canApplyVertexMorphs)
			{
				for (const PmxPositionMorphOffset& offset : morph.PositionOffsets)
				{
					if (offset.VertexIndex >= accumulatedOffsets.size())
					{
						continue;
					}
					accumulatedOffsets[offset.VertexIndex] += offset.Position * weight;
				}

				for (const PmxUvMorphOffset& offset : morph.UvOffsets)
				{
					if (offset.VertexIndex >= accumulatedUvOffsets.size())
					{
						continue;
					}
					accumulatedUvOffsets[offset.VertexIndex].x += offset.Uv.x * weight;
					accumulatedUvOffsets[offset.VertexIndex].y += offset.Uv.y * weight;
				}

				for (const PmxMaterialMorphOffset& offset : morph.MaterialOffsets)
				{
					for (size_t meshIndex = 0; meshIndex < m_Meshes.size(); ++meshIndex)
					{
						const MeshData& meshData = m_Meshes[meshIndex];
						if (offset.MaterialIndex >= 0 && meshData.MaterialIndex != offset.MaterialIndex)
						{
							continue;
						}

						for (GpuSkinVertex& vertex : m_GpuSkinVertices[meshIndex])
						{
							if (offset.Operation == 0)
							{
								vertex.Diffuse.x *= 1.0f + (offset.Diffuse.x - 1.0f) * weight;
								vertex.Diffuse.y *= 1.0f + (offset.Diffuse.y - 1.0f) * weight;
								vertex.Diffuse.z *= 1.0f + (offset.Diffuse.z - 1.0f) * weight;
							}
							else
							{
								vertex.Diffuse.x += offset.Diffuse.x * weight;
								vertex.Diffuse.y += offset.Diffuse.y * weight;
								vertex.Diffuse.z += offset.Diffuse.z * weight;
							}
						}
						vertexBufferChanged = true;
					}
				}
			}

			for (const PmxBoneMorphOffset& offset : morph.BoneOffsets)
			{
				auto boneIt = m_Bone.find(offset.BoneName);
				if (boneIt == m_Bone.end())
				{
					continue;
				}

				aiVector3D scale(1.0f, 1.0f, 1.0f);
				aiQuaternion rotation(1.0f, 0.0f, 0.0f, 0.0f);
				aiVector3D position(0.0f, 0.0f, 0.0f);
				boneIt->second.AnimationMatrix.Decompose(scale, rotation, position);

				position += offset.Position * weight;

				aiQuaternion weightedRotation;
				aiQuaternion::Interpolate(weightedRotation,
					aiQuaternion(1.0f, 0.0f, 0.0f, 0.0f),
					offset.Rotation,
					clamp(fabsf(weight), 0.0f, 1.0f));
				weightedRotation.Normalize();
				if (weight < 0.0f)
				{
					weightedRotation.Conjugate();
				}
				rotation = rotation * weightedRotation;
				rotation.Normalize();

				boneIt->second.AnimationMatrix = aiMatrix4x4(scale, rotation, position);
			}

			for (const auto& groupOffset : morph.GroupOffsets)
			{
				self(self, groupOffset.first, weight * groupOffset.second, depth + 1);
			}
		};

	for (const auto& activeMorph : m_VmdActiveMorphsScratch)
	{
		accumulateMorph(accumulateMorph, activeMorph.first, activeMorph.second, 0);
	}

	bool anyMorphApplied = false;
	if (canApplyVertexMorphs)
	{
		anyMorphApplied = vertexBufferChanged;
		for (uint32_t pmxVertexIndex = 0; pmxVertexIndex < static_cast<uint32_t>(accumulatedOffsets.size()); ++pmxVertexIndex)
		{
			const aiVector3D& offset = accumulatedOffsets[pmxVertexIndex];
			const XMFLOAT2& uvOffset = accumulatedUvOffsets[pmxVertexIndex];
			const bool hasPositionOffset =
				fabsf(offset.x) > 0.000001f || fabsf(offset.y) > 0.000001f || fabsf(offset.z) > 0.000001f;
			const bool hasUvOffset =
				fabsf(uvOffset.x) > 0.000001f || fabsf(uvOffset.y) > 0.000001f;
			if (!hasPositionOffset && !hasUvOffset)
			{
				continue;
			}

			if (pmxVertexIndex >= m_PmxVertexToMeshVertices.size())
			{
				continue;
			}

			for (const auto& target : m_PmxVertexToMeshVertices[pmxVertexIndex])
			{
				const uint32_t meshIndex = target.first;
				const uint32_t vertexIndex = target.second;
				if (meshIndex >= m_GpuSkinVertices.size() || vertexIndex >= m_GpuSkinVertices[meshIndex].size())
				{
					continue;
				}

				GpuSkinVertex& vertex = m_GpuSkinVertices[meshIndex][vertexIndex];
				if (hasPositionOffset)
				{
					vertex.Position.x += offset.x;
					vertex.Position.y += offset.y;
					vertex.Position.z += offset.z;
				}
				if (hasUvOffset)
				{
					vertex.TexCoord.x += uvOffset.x;
					vertex.TexCoord.y += uvOffset.y;
				}
				anyMorphApplied = true;
			}
		}
	}

	if ((!anyMorphApplied && !m_HasAppliedVmdMorphs) || !canApplyVertexMorphs)
	{
		return;
	}

	for (size_t meshIndex = 0; meshIndex < m_Meshes.size(); ++meshIndex)
	{
		MeshData& meshData = m_Meshes[meshIndex];
		if (!meshData.InputVertexBuffer || m_GpuSkinVertices[meshIndex].empty())
		{
			continue;
		}

		UINT8* pDest = nullptr;
		CD3DX12_RANGE readRange(0, 0);
		HRESULT hr = meshData.InputVertexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pDest));
		if (SUCCEEDED(hr) && pDest)
		{
			memcpy(pDest, m_GpuSkinVertices[meshIndex].data(),
				sizeof(GpuSkinVertex) * m_GpuSkinVertices[meshIndex].size());
			meshData.InputVertexBuffer->Unmap(0, nullptr);
		}
	}
	m_HasAppliedVmdMorphs = anyMorphApplied;
}

void AnimationModelResource::ApplyPmxOrderedTransforms(const VmdAnimation* animation, float currentFrame)
{
	if (!m_AiScene || (m_PmxAppendConstraints.empty() && m_PmxIkConstraints.empty()))
	{
		return;
	}

	if (m_PmxRuntimeNodes.empty() || m_PmxOrderedTransformSteps.empty())
	{
		RebuildPmxRuntimeCache();
	}
	if (m_PmxRuntimeNodes.empty())
	{
		return;
	}

	auto aiToXm = [](const aiMatrix4x4& src)
		{
			return XMMatrixSet(
				src.a1, src.b1, src.c1, src.d1,
				src.a2, src.b2, src.c2, src.d2,
				src.a3, src.b3, src.c3, src.d3,
				src.a4, src.b4, src.c4, src.d4);
		};

	auto rebuildGlobals = [&]()
		{
			if (m_PmxGlobalMatricesScratch.size() != m_PmxRuntimeNodes.size())
			{
				m_PmxGlobalMatricesScratch.resize(m_PmxRuntimeNodes.size());
			}

			for (size_t i = 0; i < m_PmxRuntimeNodes.size(); ++i)
			{
				const PmxRuntimeNode& fn = m_PmxRuntimeNodes[i];
				XMMATRIX local = fn.BonePtr ? aiToXm(fn.BonePtr->AnimationMatrix) : aiToXm(fn.Node->mTransformation);
				XMMATRIX world = local;
				if (fn.ParentIndex >= 0)
				{
					const XMMATRIX parent = XMLoadFloat4x4(&m_PmxGlobalMatricesScratch[static_cast<size_t>(fn.ParentIndex)]);
					world = XMMatrixMultiply(local, parent);
				}
				XMStoreFloat4x4(&m_PmxGlobalMatricesScratch[i], world);
			}
		};

	auto loadGlobal = [&](const string& boneName)
		{
			auto it = m_PmxRuntimeNodeIndexMap.find(boneName);
			return it != m_PmxRuntimeNodeIndexMap.end()
				? XMLoadFloat4x4(&m_PmxGlobalMatricesScratch[it->second])
				: XMMatrixIdentity();
		};

	auto getTranslation = [&](const string& boneName)
		{
			auto it = m_PmxRuntimeNodeIndexMap.find(boneName);
			if (it == m_PmxRuntimeNodeIndexMap.end())
			{
				return XMVectorZero();
			}

			const XMFLOAT4X4& matrix = m_PmxGlobalMatricesScratch[it->second];
			return XMVectorSet(matrix._41, matrix._42, matrix._43, 1.0f);
		};

	auto getParentGlobal = [&](const string& boneName)
		{
			auto parentIt = m_BoneParentMap.find(boneName);
			if (parentIt == m_BoneParentMap.end())
			{
				return XMMatrixIdentity();
			}
			return loadGlobal(parentIt->second);
		};
	auto applyIkLinkLimit = [](const PmxIkLink& link, XMVECTOR& localAxis, float& angle) -> bool
		{
			if (!link.HasLimit)
			{
				return true;
			}

			const float component[3] =
			{
				XMVectorGetX(localAxis),
				XMVectorGetY(localAxis),
				XMVectorGetZ(localAxis),
			};
			const float minLimit[3] = { link.LimitMin.x, link.LimitMin.y, link.LimitMin.z };
			const float maxLimit[3] = { link.LimitMax.x, link.LimitMax.y, link.LimitMax.z };

			float limitedComponent[3] = {};
			float maxAllowedAngle = 0.0f;
			int fallbackAxis = -1;
			float fallbackAxisLimit = 0.0f;
			for (int axis = 0; axis < 3; ++axis)
			{
				float low = minLimit[axis];
				float high = maxLimit[axis];
				if (low > high)
				{
					swap(low, high);
				}

				const float axisLimit = max(fabsf(low), fabsf(high));
				const bool allowsAxis = fabsf(high - low) > 0.00001f && axisLimit > 0.00001f;
				if (!allowsAxis)
				{
					continue;
				}

				float signedComponent = component[axis];
				const bool allowsPositive = high > 0.00001f;
				const bool allowsNegative = low < -0.00001f;
				if (allowsPositive && !allowsNegative)
				{
					signedComponent = fabsf(signedComponent);
				}
				else if (!allowsPositive && allowsNegative)
				{
					signedComponent = -fabsf(signedComponent);
				}

				limitedComponent[axis] = signedComponent;
				maxAllowedAngle = max(maxAllowedAngle, axisLimit);
				if (axisLimit > fallbackAxisLimit)
				{
					fallbackAxis = axis;
					fallbackAxisLimit = axisLimit;
				}
			}

			if (maxAllowedAngle <= 0.00001f)
			{
				return false;
			}

			XMVECTOR limitedAxis = XMVectorSet(
				limitedComponent[0],
				limitedComponent[1],
				limitedComponent[2],
				0.0f);
			if (XMVectorGetX(XMVector3LengthSq(limitedAxis)) < 0.000001f)
			{
				float fallbackComponent[3] = {};
				float low = minLimit[fallbackAxis];
				float high = maxLimit[fallbackAxis];
				if (low > high)
				{
					swap(low, high);
				}

				const bool allowsPositive = high > 0.00001f;
				const bool allowsNegative = low < -0.00001f;
				float sign = component[fallbackAxis] < 0.0f ? -1.0f : 1.0f;
				if (allowsPositive && !allowsNegative)
				{
					sign = 1.0f;
				}
				else if (!allowsPositive && allowsNegative)
				{
					sign = -1.0f;
				}
				fallbackComponent[fallbackAxis] = sign;
				limitedAxis = XMVectorSet(
					fallbackComponent[0],
					fallbackComponent[1],
					fallbackComponent[2],
					0.0f);
			}

			localAxis = XMVector3Normalize(limitedAxis);
			angle = min(angle, maxAllowedAngle);
			return true;
		};

	m_PmxAppendResultsScratch.clear();

	auto getLocalAnimationDelta = [](const Bone& bone, aiQuaternion& outRotation, aiVector3D& outTranslation)
		{
			aiVector3D baseScale(1.0f, 1.0f, 1.0f);
			aiQuaternion baseRotation(1.0f, 0.0f, 0.0f, 0.0f);
			aiVector3D basePosition(0.0f, 0.0f, 0.0f);
			bone.BindLocalMatrix.Decompose(baseScale, baseRotation, basePosition);

			aiVector3D currentScale(1.0f, 1.0f, 1.0f);
			aiQuaternion currentRotation(1.0f, 0.0f, 0.0f, 0.0f);
			aiVector3D currentPosition(0.0f, 0.0f, 0.0f);
			bone.AnimationMatrix.Decompose(currentScale, currentRotation, currentPosition);

			aiQuaternion inverseBase = baseRotation;
			inverseBase.Conjugate();
			outRotation = inverseBase * currentRotation;
			outRotation.Normalize();
			outTranslation = currentPosition - basePosition;
		};

	auto applyAppend = [&](const PmxAppendConstraint& constraint)
		{
			auto boneIt = m_Bone.find(constraint.BoneName);
			auto appendIt = m_Bone.find(constraint.AppendBoneName);
			if (boneIt == m_Bone.end() || appendIt == m_Bone.end())
			{
				return false;
			}

			aiVector3D scale(1.0f, 1.0f, 1.0f);
			aiQuaternion rotation(1.0f, 0.0f, 0.0f, 0.0f);
			aiVector3D position(0.0f, 0.0f, 0.0f);
			boneIt->second.AnimationMatrix.Decompose(scale, rotation, position);

			aiQuaternion appendRotation(1.0f, 0.0f, 0.0f, 0.0f);
			aiVector3D appendPosition(0.0f, 0.0f, 0.0f);
			auto appendResultIt = m_PmxAppendResultsScratch.find(constraint.AppendBoneName);
			if (!constraint.Local && appendResultIt != m_PmxAppendResultsScratch.end())
			{
				appendRotation = appendResultIt->second.Rotation;
				appendPosition = appendResultIt->second.Translation;
			}
			else
			{
				getLocalAnimationDelta(appendIt->second, appendRotation, appendPosition);
			}

			PmxAppendResult appendResult{};
			if (constraint.InheritRotation)
			{
				aiQuaternion weightedRotation;
				aiQuaternion::Interpolate(weightedRotation,
					aiQuaternion(1.0f, 0.0f, 0.0f, 0.0f),
					appendRotation,
					clamp(fabsf(constraint.Weight), 0.0f, 1.0f));
				weightedRotation.Normalize();
				if (constraint.Weight < 0.0f)
				{
					weightedRotation.Conjugate();
				}
				rotation = rotation * weightedRotation;
				rotation.Normalize();
				appendResult.Rotation = weightedRotation;
			}

			if (constraint.InheritTranslation)
			{
				appendResult.Translation = appendPosition * constraint.Weight;
				position += appendResult.Translation;
			}

			boneIt->second.AnimationMatrix = aiMatrix4x4(scale, rotation, position);
			m_PmxAppendResultsScratch[constraint.BoneName] = appendResult;
			return true;
		};

	auto isToeIkConstraint = [](const string& boneName)
		{
			return boneName.find("つま先") != string::npos;
		};

	auto isKneeBone = [](const string& boneName)
		{
			return boneName.find("ひざ") != string::npos || boneName.find("膝") != string::npos;
		};

	auto axisVectorFromIndex = [](int axisIndex)
		{
			switch (axisIndex)
			{
			case 1: return XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
			case 2: return XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
			default: return XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
			}
		};

	auto pickIkLimitAxis = [](const PmxIkLink& link, float& outMinLimit, float& outMaxLimit)
		{
			const float minLimit[3] = { link.LimitMin.x, link.LimitMin.y, link.LimitMin.z };
			const float maxLimit[3] = { link.LimitMax.x, link.LimitMax.y, link.LimitMax.z };
			int bestAxis = 0;
			float bestRange = 0.0f;
			for (int axis = 0; axis < 3; ++axis)
			{
				const float range = max(fabsf(minLimit[axis]), fabsf(maxLimit[axis]));
				if (range > bestRange)
				{
					bestRange = range;
					bestAxis = axis;
				}
			}
			outMinLimit = minLimit[bestAxis];
			outMaxLimit = maxLimit[bestAxis];
			return bestAxis;
		};

	auto solveIk = [&](const PmxIkConstraint& constraint)
		{


			if (isToeIkConstraint(constraint.BoneName))
			{
				return false;
			}

			if (!IsVmdIkEnabled(animation, constraint.BoneName, currentFrame) ||
				m_Bone.find(constraint.BoneName) == m_Bone.end() ||
				m_Bone.find(constraint.TargetBoneName) == m_Bone.end())
			{
				return false;
			}

			bool changed = false;
			const uint32_t iterationCount = min<uint32_t>(constraint.IterationCount, 12);
			const float normalMaxAngle = 0.12f;
			const float kneeMaxAngle = 0.08f;

			for (uint32_t iteration = 0; iteration < iterationCount; ++iteration)
			{
				const XMVECTOR goalPosition = getTranslation(constraint.BoneName);
				const XMVECTOR effectorPosition = getTranslation(constraint.TargetBoneName);
				const float currentDistanceSq = XMVectorGetX(XMVector3LengthSq(XMVectorSubtract(goalPosition, effectorPosition)));
				if (currentDistanceSq < 0.000001f)
				{
					break;
				}

				for (const PmxIkLink& link : constraint.Links)
				{
					auto linkBoneIt = m_Bone.find(link.BoneName);
					if (linkBoneIt == m_Bone.end())
					{
						continue;
					}

					const XMVECTOR linkPosition = getTranslation(link.BoneName);
					const XMVECTOR toEffector = XMVectorSubtract(getTranslation(constraint.TargetBoneName), linkPosition);
					const XMVECTOR toGoal = XMVectorSubtract(getTranslation(constraint.BoneName), linkPosition);
					if (XMVectorGetX(XMVector3LengthSq(toEffector)) < 0.000001f ||
						XMVectorGetX(XMVector3LengthSq(toGoal)) < 0.000001f)
					{
						continue;
					}

					const XMMATRIX parentGlobal = getParentGlobal(link.BoneName);
					XMVECTOR localAxis = XMVectorZero();
					float angle = 0.0f;

					if (isKneeBone(link.BoneName))
					{
						float minLimit = 0.0f;
						float maxLimit = 0.0f;
						const int hingeAxisIndex = link.HasLimit ? pickIkLimitAxis(link, minLimit, maxLimit) : 0;
						localAxis = axisVectorFromIndex(hingeAxisIndex);

						XMVECTOR worldAxis = XMVector3TransformNormal(localAxis, parentGlobal);
						if (XMVectorGetX(XMVector3LengthSq(worldAxis)) < 0.000001f)
						{
							continue;
						}
						worldAxis = XMVector3Normalize(worldAxis);

						XMVECTOR projectedEffector = XMVectorSubtract(toEffector, XMVectorScale(worldAxis, XMVectorGetX(XMVector3Dot(toEffector, worldAxis))));
						XMVECTOR projectedGoal = XMVectorSubtract(toGoal, XMVectorScale(worldAxis, XMVectorGetX(XMVector3Dot(toGoal, worldAxis))));
						if (XMVectorGetX(XMVector3LengthSq(projectedEffector)) < 0.000001f ||
							XMVectorGetX(XMVector3LengthSq(projectedGoal)) < 0.000001f)
						{
							continue;
						}

						projectedEffector = XMVector3Normalize(projectedEffector);
						projectedGoal = XMVector3Normalize(projectedGoal);

						float dot = XMVectorGetX(XMVector3Dot(projectedEffector, projectedGoal));
						dot = clamp(dot, -1.0f, 1.0f);
						angle = acosf(dot);
						if (angle < 0.00001f)
						{
							continue;
						}

						const float sign = XMVectorGetX(XMVector3Dot(XMVector3Cross(projectedEffector, projectedGoal), worldAxis)) < 0.0f ? -1.0f : 1.0f;
						angle *= sign;

						float stepLimit = kneeMaxAngle;
						if (link.HasLimit)
						{
							const float axisLimit = max(fabsf(minLimit), fabsf(maxLimit));
							if (axisLimit <= 0.00001f)
							{
								continue;
							}
							stepLimit = min(stepLimit, axisLimit);

							const bool allowsPositive = maxLimit > 0.00001f;
							const bool allowsNegative = minLimit < -0.00001f;
							if (allowsPositive && !allowsNegative)
							{
								angle = fabsf(angle);
							}
							else if (!allowsPositive && allowsNegative)
							{
								angle = -fabsf(angle);
							}
						}
						angle = clamp(angle, -stepLimit, stepLimit);
					}
					else
					{
						const XMVECTOR effectorDir = XMVector3Normalize(toEffector);
						const XMVECTOR goalDir = XMVector3Normalize(toGoal);
						float dot = XMVectorGetX(XMVector3Dot(effectorDir, goalDir));
						dot = clamp(dot, -1.0f, 1.0f);

						angle = acosf(dot);
						if (angle < 0.00001f)
						{
							continue;
						}
						angle = min(angle, normalMaxAngle);

						XMVECTOR worldAxis = XMVector3Cross(effectorDir, goalDir);
						if (XMVectorGetX(XMVector3LengthSq(worldAxis)) < 0.000001f)
						{
							continue;
						}
						worldAxis = XMVector3Normalize(worldAxis);

						XMVECTOR determinant = XMMatrixDeterminant(parentGlobal);
						const XMMATRIX inverseParent = XMMatrixInverse(&determinant, parentGlobal);
						localAxis = XMVector3TransformNormal(worldAxis, inverseParent);
						if (XMVectorGetX(XMVector3LengthSq(localAxis)) < 0.000001f)
						{
							continue;
						}
						localAxis = XMVector3Normalize(localAxis);
						if (!applyIkLinkLimit(link, localAxis, angle))
						{
							continue;
						}
					}

					if (fabsf(angle) < 0.00001f)
					{
						continue;
					}

					const aiMatrix4x4 previousMatrix = linkBoneIt->second.AnimationMatrix;
					const XMVECTOR deltaRotation = XMQuaternionRotationAxis(localAxis, angle);

					aiVector3D scale(1.0f, 1.0f, 1.0f);
					aiQuaternion rotation(1.0f, 0.0f, 0.0f, 0.0f);
					aiVector3D position(0.0f, 0.0f, 0.0f);
					linkBoneIt->second.AnimationMatrix.Decompose(scale, rotation, position);

					const XMVECTOR currentRotation = XMVectorSet(rotation.x, rotation.y, rotation.z, rotation.w);
					XMVECTOR updatedRotation = XMQuaternionMultiply(currentRotation, deltaRotation);
					updatedRotation = XMQuaternionNormalize(updatedRotation);

					XMFLOAT4 storedRotation{};
					XMStoreFloat4(&storedRotation, updatedRotation);
					aiQuaternion aiUpdatedRotation(storedRotation.w, storedRotation.x, storedRotation.y, storedRotation.z);
					aiUpdatedRotation.Normalize();
					linkBoneIt->second.AnimationMatrix = aiMatrix4x4(scale, aiUpdatedRotation, position);

					rebuildGlobals();
					const float newDistanceSq = XMVectorGetX(XMVector3LengthSq(
						XMVectorSubtract(getTranslation(constraint.BoneName), getTranslation(constraint.TargetBoneName))));
					if (newDistanceSq > currentDistanceSq + 0.0001f)
					{
						linkBoneIt->second.AnimationMatrix = previousMatrix;
						rebuildGlobals();
						continue;
					}

					changed = true;
				}
			}
			return changed;
		};

	rebuildGlobals();
	for (const PmxOrderedTransformStep& step : m_PmxOrderedTransformSteps)
	{
		const bool changed = step.IsIk
			? solveIk(m_PmxIkConstraints[step.Index])
			: applyAppend(m_PmxAppendConstraints[step.Index]);
		if (changed)
		{
			rebuildGlobals();
		}
	}
}

void AnimationModelResource::SampleVmdBone(const VmdAnimation* animation, const string& boneName, float timeSeconds,
	aiQuaternion& outRotation, aiVector3D& outPosition) const
{
	outRotation = aiQuaternion(1.0f, 0.0f, 0.0f, 0.0f);
	outPosition = aiVector3D(0.0f, 0.0f, 0.0f);

	if (!animation)
	{
		return;
	}

	auto trackIt = animation->BoneTracks.find(boneName);
	if (trackIt == animation->BoneTracks.end())
	{
		return;
	}

	const float currentFrame = VmdToFrameTime(animation, timeSeconds);
	SampleVmdBoneTrack(&trackIt->second, currentFrame, outRotation, outPosition);
}

void AnimationModelResource::UpdateVmdBoneMatrices(const VmdAnimation* animation1, float frame1,
	const VmdAnimation* animation2, float frame2, float blendRate)
{
	m_UseLayeredVmdIk = false;
	if (!m_AiScene)
	{
		return;
	}

	const VmdAnimation* primaryAnimation = animation1 ? animation1 : animation2;
	const VmdAnimation* secondaryAnimation = animation2 ? animation2 : primaryAnimation;
	if (!primaryAnimation)
	{
		return;
	}

	if (m_CachedVmdPrimaryAnimation != primaryAnimation || m_CachedVmdSecondaryAnimation != secondaryAnimation)
	{
		RebuildVmdRuntimeCache(primaryAnimation, secondaryAnimation);
	}

	const float safeBlendRate = clamp(blendRate, 0.0f, 1.0f);
	const float primaryTime = animation1 ? frame1 : frame2;
	const float secondaryTime = animation2 ? frame2 : primaryTime;
	const float currentFrame1 = VmdToFrameTime(primaryAnimation, primaryTime);
	const bool useSecondarySample = secondaryAnimation && safeBlendRate > 0.0001f &&
		(secondaryAnimation != primaryAnimation || fabsf(secondaryTime - primaryTime) > 0.0001f);
	const float currentFrame2 = useSecondarySample ? VmdToFrameTime(secondaryAnimation, secondaryTime) : currentFrame1;
	for (VmdBoneBinding& binding : m_VmdBoneBindings)
	{
		Bone* bonePtr = binding.BonePtr;
		if (!bonePtr)
		{
			continue;
		}

		aiQuaternion rotation1(1.0f, 0.0f, 0.0f, 0.0f);
		aiVector3D pos1(0.0f, 0.0f, 0.0f);
		SampleVmdBoneTrackCached(binding.PrimaryTrack, currentFrame1, binding.PrimaryCursor, rotation1, pos1);

		aiQuaternion rotation = rotation1;
		aiVector3D pos = pos1;
		if (useSecondarySample)
		{
			aiQuaternion rotation2(1.0f, 0.0f, 0.0f, 0.0f);
			aiVector3D pos2(0.0f, 0.0f, 0.0f);
			SampleVmdBoneTrackCached(binding.SecondaryTrack, currentFrame2, binding.SecondaryCursor, rotation2, pos2);
			pos = pos1 * (1.0f - safeBlendRate) + pos2 * safeBlendRate;
			aiQuaternion::Interpolate(rotation, rotation1, rotation2, safeBlendRate);
			rotation.Normalize();
		}

		aiQuaternion localRotation = binding.BaseRotation * rotation;
		localRotation.Normalize();
		bonePtr->AnimationMatrix = aiMatrix4x4(binding.BaseScale, localRotation, binding.BasePosition + pos);
	}

	ApplyVmdMorphs(primaryAnimation, currentFrame1);
	ApplyPmxOrderedTransforms(primaryAnimation, currentFrame1);
	UpdateBoneMatrix(m_AiScene->mRootNode, MakeAiIdentityMatrix());
	WriteBoneMatricesToBuffer();
}

void AnimationModelResource::UpdateVmdBoneMatrices(
	const vector<AnimationPlaybackLayer>& animationLayers)
{
	if (!m_AiScene || animationLayers.empty())
	{
		return;
	}

	if (!IsVmdLayeredRuntimeCacheValid(animationLayers))
	{
		RebuildVmdLayeredRuntimeCache(animationLayers);
	}
	m_UseLayeredVmdIk = true;

	bool hasVmdAnimation = false;
	for (size_t layerIndex = 0; layerIndex < animationLayers.size(); ++layerIndex)
	{
		const VmdAnimation* animation = m_CachedVmdLayerAnimations[layerIndex];
		hasVmdAnimation = hasVmdAnimation || animation != nullptr;
		m_VmdLayerFramesScratch[layerIndex] = VmdToFrameTime(
			animation, animationLayers[layerIndex].CurrentTime);
	}
	if (!hasVmdAnimation)
	{
		return;
	}

	bool poseUnchanged = m_HasCachedLayeredPose &&
		m_LastLayerPoseTimes.size() == animationLayers.size();
	if (poseUnchanged)
	{
		for (size_t layerIndex = 0; layerIndex < animationLayers.size(); ++layerIndex)
		{
			if (fabsf(m_LastLayerPoseTimes[layerIndex] - animationLayers[layerIndex].CurrentTime) > 0.000001f)
			{
				poseUnchanged = false;
				break;
			}
		}
	}
	if (poseUnchanged)
	{
		return;
	}

	for (VmdLayeredBoneBinding& binding : m_VmdLayeredBoneBindings)
	{
		if (!binding.BonePtr)
		{
			continue;
		}

		aiQuaternion rotation(1.0f, 0.0f, 0.0f, 0.0f);
		aiVector3D position(0.0f, 0.0f, 0.0f);
		if (binding.Track && binding.LayerIndex < m_VmdLayerFramesScratch.size())
		{
			SampleVmdBoneTrackCached(
				binding.Track,
				m_VmdLayerFramesScratch[binding.LayerIndex],
				binding.Cursor,
				rotation,
				position);
		}

		aiQuaternion localRotation = binding.BaseRotation * rotation;
		localRotation.Normalize();
		binding.BonePtr->AnimationMatrix = aiMatrix4x4(
			binding.BaseScale, localRotation, binding.BasePosition + position);
	}

	ApplyVmdMorphLayers();
	ApplyPmxOrderedTransforms(nullptr, 0.0f);
	UpdateBoneMatrix(m_AiScene->mRootNode, MakeAiIdentityMatrix());
	WriteBoneMatricesToBuffer();

	for (size_t layerIndex = 0; layerIndex < animationLayers.size(); ++layerIndex)
	{
		m_LastLayerPoseTimes[layerIndex] = animationLayers[layerIndex].CurrentTime;
	}
	m_HasCachedLayeredPose = true;
}

bool AnimationModelResource::IsVmdIkEnabled(const VmdAnimation* animation, const string& ikBoneName, float currentFrame)
{
	if (m_UseLayeredVmdIk)
	{
		auto bindingIt = m_VmdLayeredIkBindings.find(ikBoneName);
		if (bindingIt == m_VmdLayeredIkBindings.end())
		{
			return true;
		}

		VmdLayeredIkBinding& binding = bindingIt->second;
		if (!binding.Track || binding.LayerIndex >= m_VmdLayerFramesScratch.size())
		{
			return true;
		}
		return SampleVmdIkTrackCached(
			binding.Track, m_VmdLayerFramesScratch[binding.LayerIndex], binding.Cursor);
	}

	if (!animation)
	{
		return true;
	}

	const vector<VmdIkKeyframe>* keys = nullptr;
	VmdTrackSampleCursor* cursor = nullptr;
	if (animation == m_CachedVmdPrimaryAnimation)
	{
		auto cachedIt = m_VmdIkTrackCache.find(ikBoneName);
		if (cachedIt != m_VmdIkTrackCache.end())
		{
			keys = cachedIt->second;
		}
		auto cursorIt = m_VmdIkTrackCursors.find(ikBoneName);
		if (cursorIt != m_VmdIkTrackCursors.end())
		{
			cursor = &cursorIt->second;
		}
	}
	else
	{
		auto trackIt = animation->IkTracks.find(ikBoneName);
		if (trackIt != animation->IkTracks.end())
		{
			keys = &trackIt->second;
		}
	}

	if (!keys)
	{
		return true;
	}

	if (cursor)
	{
		return SampleVmdIkTrackCached(keys, currentFrame, *cursor);
	}
	return SampleVmdIkTrack(keys, currentFrame);
}

void AnimationModelResource::ApplyPmxIk(const VmdAnimation* animation, float timeSeconds)
{
	if (!m_AiScene || m_PmxIkConstraints.empty())
	{
		return;
	}

	const float currentFrame = VmdToFrameTime(animation, timeSeconds);

	auto aiToXm = [](const aiMatrix4x4& src)
		{
			return XMMatrixSet(
				src.a1, src.b1, src.c1, src.d1,
				src.a2, src.b2, src.c2, src.d2,
				src.a3, src.b3, src.c3, src.d3,
				src.a4, src.b4, src.c4, src.d4);
		};

	struct FastNode {
		aiNode* Node;
		string Name;
		int ParentIndex;
		Bone* BonePtr;
	};
	vector<FastNode> fastNodes;
	auto buildFast = [&](auto&& self, aiNode* node, int parent) -> void {
		if (!node) return;
		int myIdx = (int)fastNodes.size();
		Bone* bPtr = nullptr;
		auto it = m_Bone.find(node->mName.C_Str());
		if (it != m_Bone.end()) bPtr = &it->second;
		fastNodes.push_back({ node, node->mName.C_Str(), parent, bPtr });
		for (unsigned int i = 0; i < node->mNumChildren; ++i) self(self, node->mChildren[i], myIdx);
		};
	buildFast(buildFast, m_AiScene->mRootNode, -1);

	unordered_map<string, XMFLOAT4X4> globalMatrices;
	vector<XMMATRIX> fastGlobals;
	auto rebuildGlobals = [&]()
		{
			fastGlobals.resize(fastNodes.size());
			for (size_t i = 0; i < fastNodes.size(); ++i)
			{
				const auto& fn = fastNodes[i];
				XMMATRIX local = fn.BonePtr ? aiToXm(fn.BonePtr->AnimationMatrix) : aiToXm(fn.Node->mTransformation);
				XMMATRIX world = fn.ParentIndex >= 0 ? XMMatrixMultiply(local, fastGlobals[fn.ParentIndex]) : local;
				fastGlobals[i] = world;
				XMFLOAT4X4 stored{};
				XMStoreFloat4x4(&stored, world);
				globalMatrices[fn.Name] = stored;
			}
		};

	auto loadGlobal = [&](const string& boneName)
		{
			auto it = globalMatrices.find(boneName);
			return it != globalMatrices.end() ? XMLoadFloat4x4(&it->second) : XMMatrixIdentity();
		};

	auto getTranslation = [&](const string& boneName)
		{
			auto it = globalMatrices.find(boneName);
			if (it == globalMatrices.end())
			{
				return XMVectorZero();
			}
			const XMFLOAT4X4& matrix = it->second;
			return XMVectorSet(matrix._41, matrix._42, matrix._43, 1.0f);
		};

	auto getParentGlobal = [&](const string& boneName)
		{
			auto parentIt = m_BoneParentMap.find(boneName);
			if (parentIt == m_BoneParentMap.end())
			{
				return XMMatrixIdentity();
			}
			return loadGlobal(parentIt->second);
		};

	auto applyIkLinkLimit = [](const PmxIkLink& link, XMVECTOR& localAxis, float& angle) -> bool
		{
			if (!link.HasLimit)
			{
				return true;
			}

			const float component[3] =
			{
				XMVectorGetX(localAxis),
				XMVectorGetY(localAxis),
				XMVectorGetZ(localAxis),
			};
			const float minLimit[3] = { link.LimitMin.x, link.LimitMin.y, link.LimitMin.z };
			const float maxLimit[3] = { link.LimitMax.x, link.LimitMax.y, link.LimitMax.z };

			float limitedComponent[3] = {};
			float maxAllowedAngle = 0.0f;
			int fallbackAxis = -1;
			float fallbackAxisLimit = 0.0f;
			for (int axis = 0; axis < 3; ++axis)
			{
				float low = minLimit[axis];
				float high = maxLimit[axis];
				if (low > high)
				{
					swap(low, high);
				}

				const float axisLimit = max(fabsf(low), fabsf(high));
				const bool allowsAxis = fabsf(high - low) > 0.00001f && axisLimit > 0.00001f;
				if (!allowsAxis)
				{
					continue;
				}

				float signedComponent = component[axis];
				const bool allowsPositive = high > 0.00001f;
				const bool allowsNegative = low < -0.00001f;
				if (allowsPositive && !allowsNegative)
				{
					signedComponent = fabsf(signedComponent);
				}
				else if (!allowsPositive && allowsNegative)
				{
					signedComponent = -fabsf(signedComponent);
				}

				limitedComponent[axis] = signedComponent;
				maxAllowedAngle = max(maxAllowedAngle, axisLimit);
				if (axisLimit > fallbackAxisLimit)
				{
					fallbackAxis = axis;
					fallbackAxisLimit = axisLimit;
				}
			}

			if (maxAllowedAngle <= 0.00001f)
			{
				return false;
			}

			XMVECTOR limitedAxis = XMVectorSet(
				limitedComponent[0],
				limitedComponent[1],
				limitedComponent[2],
				0.0f);

			if (XMVectorGetX(XMVector3LengthSq(limitedAxis)) < 0.000001f)
			{
				float fallbackComponent[3] = {};
				float low = minLimit[fallbackAxis];
				float high = maxLimit[fallbackAxis];
				if (low > high)
				{
					swap(low, high);
				}

				const bool allowsPositive = high > 0.00001f;
				const bool allowsNegative = low < -0.00001f;
				float sign = component[fallbackAxis] < 0.0f ? -1.0f : 1.0f;
				if (allowsPositive && !allowsNegative)
				{
					sign = 1.0f;
				}
				else if (!allowsPositive && allowsNegative)
				{
					sign = -1.0f;
				}
				fallbackComponent[fallbackAxis] = sign;
				limitedAxis = XMVectorSet(
					fallbackComponent[0],
					fallbackComponent[1],
					fallbackComponent[2],
					0.0f);
			}

			localAxis = XMVector3Normalize(limitedAxis);
			angle = min(angle, maxAllowedAngle);
			return true;
		};

	rebuildGlobals();
	for (const PmxIkConstraint& constraint : m_PmxIkConstraints)
	{
		if (!IsVmdIkEnabled(animation, constraint.BoneName, currentFrame) ||
			m_Bone.find(constraint.BoneName) == m_Bone.end() ||
			m_Bone.find(constraint.TargetBoneName) == m_Bone.end())
		{
			continue;
		}

		const uint32_t iterationCount = min<uint32_t>(constraint.IterationCount, 64);
		const float maxAngle = constraint.LimitAngle > 0.0f ? constraint.LimitAngle : XM_PIDIV4;
		for (uint32_t iteration = 0; iteration < iterationCount; ++iteration)
		{
			const XMVECTOR goalPosition = getTranslation(constraint.BoneName);
			const XMVECTOR effectorPosition = getTranslation(constraint.TargetBoneName);
			const float currentDistanceSq = XMVectorGetX(XMVector3LengthSq(XMVectorSubtract(goalPosition, effectorPosition)));
			if (currentDistanceSq < 0.000001f)
			{
				break;
			}

			for (const PmxIkLink& link : constraint.Links)
			{
				auto linkBoneIt = m_Bone.find(link.BoneName);
				if (linkBoneIt == m_Bone.end())
				{
					continue;
				}

				const XMVECTOR linkPosition = getTranslation(link.BoneName);
				const XMVECTOR toEffector = XMVectorSubtract(getTranslation(constraint.TargetBoneName), linkPosition);
				const XMVECTOR toGoal = XMVectorSubtract(getTranslation(constraint.BoneName), linkPosition);
				if (XMVectorGetX(XMVector3LengthSq(toEffector)) < 0.000001f ||
					XMVectorGetX(XMVector3LengthSq(toGoal)) < 0.000001f)
				{
					continue;
				}

				const XMVECTOR effectorDir = XMVector3Normalize(toEffector);
				const XMVECTOR goalDir = XMVector3Normalize(toGoal);
				float dot = XMVectorGetX(XMVector3Dot(effectorDir, goalDir));
				dot = clamp(dot, -1.0f, 1.0f);

				float angle = acosf(dot);
				if (angle < 0.00001f)
				{
					continue;
				}
				angle = min(angle, maxAngle);

				XMVECTOR axis = XMVector3Cross(effectorDir, goalDir);
				if (XMVectorGetX(XMVector3LengthSq(axis)) < 0.000001f)
				{
					continue;
				}
				axis = XMVector3Normalize(axis);

				const XMMATRIX parentGlobal = getParentGlobal(link.BoneName);
				XMVECTOR determinant = XMMatrixDeterminant(parentGlobal);
				const XMMATRIX inverseParent = XMMatrixInverse(&determinant, parentGlobal);
				XMVECTOR localAxis = XMVector3TransformNormal(axis, inverseParent);
				if (XMVectorGetX(XMVector3LengthSq(localAxis)) < 0.000001f)
				{
					continue;
				}
				localAxis = XMVector3Normalize(localAxis);
				if (!applyIkLinkLimit(link, localAxis, angle))
				{
					continue;
				}

				const XMVECTOR deltaRotation = XMQuaternionRotationAxis(localAxis, angle);

				aiVector3D scale(1.0f, 1.0f, 1.0f);
				aiQuaternion rotation(1.0f, 0.0f, 0.0f, 0.0f);
				aiVector3D position(0.0f, 0.0f, 0.0f);
				linkBoneIt->second.AnimationMatrix.Decompose(scale, rotation, position);

				const XMVECTOR currentRotation = XMVectorSet(rotation.x, rotation.y, rotation.z, rotation.w);
				XMVECTOR updatedRotation = XMQuaternionMultiply(currentRotation, deltaRotation);
				updatedRotation = XMQuaternionNormalize(updatedRotation);

				XMFLOAT4 storedRotation{};
				XMStoreFloat4(&storedRotation, updatedRotation);
				aiQuaternion aiUpdatedRotation(storedRotation.w, storedRotation.x, storedRotation.y, storedRotation.z);
				aiUpdatedRotation.Normalize();
				linkBoneIt->second.AnimationMatrix = aiMatrix4x4(scale, aiUpdatedRotation, position);

				rebuildGlobals();
			}
		}
	}
}

void AnimationModelResource::UpdateBoneMatrices(const char* animName1, float frame1,
	const char* animName2, float frame2, float blendRate)
{
	if (!m_AiScene)
	{
		return;
	}
	m_HasCachedLayeredPose = false;
	m_UseLayeredVmdIk = false;

	const string animationName1 = animName1 ? animName1 : "";
	const string animationName2 = animName2 ? animName2 : "";
	const float safeBlendRate = clamp(blendRate, 0.0f, 1.0f);
	if (m_HasCachedPose &&
		m_LastPoseAnimation1 == animationName1 &&
		m_LastPoseAnimation2 == animationName2 &&
		fabsf(m_LastPoseFrame1 - frame1) <= 0.000001f &&
		fabsf(m_LastPoseFrame2 - frame2) <= 0.000001f &&
		fabsf(m_LastPoseBlendRate - safeBlendRate) <= 0.000001f)
	{
		return;
	}

	auto cachePose = [&]()
		{
			m_LastPoseAnimation1 = animationName1;
			m_LastPoseAnimation2 = animationName2;
			m_LastPoseFrame1 = frame1;
			m_LastPoseFrame2 = frame2;
			m_LastPoseBlendRate = safeBlendRate;
			m_HasCachedPose = true;
		};

	const VmdAnimation* vmdAnimation1 = nullptr;
	const VmdAnimation* vmdAnimation2 = nullptr;
	auto vmdIt1 = m_VmdAnimations.find(animationName1);
	if (vmdIt1 != m_VmdAnimations.end())
	{
		vmdAnimation1 = &vmdIt1->second;
	}
	auto vmdIt2 = m_VmdAnimations.find(animationName2);
	if (vmdIt2 != m_VmdAnimations.end())
	{
		vmdAnimation2 = &vmdIt2->second;
	}
	if (vmdAnimation1 || vmdAnimation2)
	{
		UpdateVmdBoneMatrices(vmdAnimation1, frame1, vmdAnimation2, frame2, safeBlendRate);
		cachePose();
		return;
	}

	aiAnimation* animation1 = GetAnimation(animationName1);
	aiAnimation* animation2 = GetAnimation(animationName2);

	if (!animation1 && !animation2)
	{
		return;
	}

	auto sampleRotationKey = [](aiNodeAnim* nodeAnim, float timeInTicks, double duration)
		{
			if (!nodeAnim || nodeAnim->mNumRotationKeys == 0)
			{
				return aiQuaternion(1.0f, 0.0f, 0.0f, 0.0f);
			}

			const float t = duration > 0.0 ? fmod(timeInTicks, static_cast<float>(duration)) : timeInTicks;
			const unsigned int keyIndex = static_cast<unsigned int>(max(0.0f, t)) % nodeAnim->mNumRotationKeys;
			return nodeAnim->mRotationKeys[keyIndex].mValue;
		};

	auto samplePositionKey = [](aiNodeAnim* nodeAnim, float timeInTicks, double duration)
		{
			if (!nodeAnim || nodeAnim->mNumPositionKeys == 0)
			{
				return aiVector3D(0.0f, 0.0f, 0.0f);
			}

			const float t = duration > 0.0 ? fmod(timeInTicks, static_cast<float>(duration)) : timeInTicks;
			const unsigned int keyIndex = static_cast<unsigned int>(max(0.0f, t)) % nodeAnim->mNumPositionKeys;
			return nodeAnim->mPositionKeys[keyIndex].mValue;
		};

	for (auto& pair : m_Bone)
	{
		Bone* bonePtr = &pair.second;
		aiNodeAnim* nodeAnim1 = FindNodeAnimChannel(animation1, pair.first);
		aiNodeAnim* nodeAnim2 = FindNodeAnimChannel(animation2, pair.first);

		const float ticksPerSecond1 = (animation1 && animation1->mTicksPerSecond != 0)
			? static_cast<float>(animation1->mTicksPerSecond)
			: 25.0f;
		const float ticksPerSecond2 = (animation2 && animation2->mTicksPerSecond != 0)
			? static_cast<float>(animation2->mTicksPerSecond)
			: 25.0f;

		const float timeInTicks1 = frame1 * ticksPerSecond1;
		const float timeInTicks2 = frame2 * ticksPerSecond2;

		aiQuaternion rotation1 = sampleRotationKey(nodeAnim1, timeInTicks1, animation1 ? animation1->mDuration : 0.0);
		aiVector3D pos1 = samplePositionKey(nodeAnim1, timeInTicks1, animation1 ? animation1->mDuration : 0.0);
		aiQuaternion rotation2 = sampleRotationKey(nodeAnim2, timeInTicks2, animation2 ? animation2->mDuration : 0.0);
		aiVector3D pos2 = samplePositionKey(nodeAnim2, timeInTicks2, animation2 ? animation2->mDuration : 0.0);

		const aiVector3D pos = pos1 * (1.f - blendRate) + pos2 * blendRate;

		aiQuaternion rotation;
		aiQuaternion::Interpolate(rotation, rotation1, rotation2, blendRate);

		bonePtr->AnimationMatrix = aiMatrix4x4(aiVector3D(1.0f, 1.0f, 1.0f), rotation, pos);
	}

	UpdateBoneMatrix(m_AiScene->mRootNode, MakeAiIdentityMatrix());
	WriteBoneMatricesToBuffer();
	cachePose();
}

void AnimationModelResource::UpdateBoneMatrices(
	const vector<AnimationPlaybackLayer>& animationLayers)
{
	if (!m_AiScene || animationLayers.empty())
	{
		return;
	}

	if (animationLayers.size() == 1)
	{
		const AnimationPlaybackLayer& layer = animationLayers.front();
		UpdateBoneMatrices(
			layer.AnimationName.c_str(), layer.CurrentTime,
			layer.AnimationName.c_str(), layer.CurrentTime, 0.0f);
		return;
	}

	bool hasVmdAnimation = false;
	for (const AnimationPlaybackLayer& layer : animationLayers)
	{
		if (m_VmdAnimations.find(layer.AnimationName) != m_VmdAnimations.end())
		{
			hasVmdAnimation = true;
			break;
		}
	}

	if (hasVmdAnimation)
	{


		m_HasCachedPose = false;
		UpdateVmdBoneMatrices(animationLayers);
		return;
	}




	const AnimationPlaybackLayer& highestPriorityLayer = animationLayers.back();
	UpdateBoneMatrices(
		highestPriorityLayer.AnimationName.c_str(), highestPriorityLayer.CurrentTime,
		highestPriorityLayer.AnimationName.c_str(), highestPriorityLayer.CurrentTime, 0.0f);
}

void AnimationModelResource::DispatchGpuSkinning(ID3D12GraphicsCommandList* pCommandList)
{
	if (!pCommandList ||
		!m_SkinningDescHeap ||
		!RendererShader::GetSkinningRootSignature() ||
		!PsoManager::GetSkinningPso() ||
		m_DispatchedSkinningVersion == m_SkinningVersion)
	{
		return;
	}

	pCommandList->SetComputeRootSignature(RendererShader::GetSkinningRootSignature());
	pCommandList->SetPipelineState(PsoManager::GetSkinningPso());

	ID3D12DescriptorHeap* heaps[] = { m_SkinningDescHeap.Get() };
	pCommandList->SetDescriptorHeaps(_countof(heaps), heaps);

	UINT descSize = m_pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	constexpr UINT kSkinningThreadGroupSize = 128;

	for (UINT m = 0; m < m_Meshes.size(); m++)
	{
		if (m_Meshes[m].VertexCount == 0) continue;

		if (m_Meshes[m].PreviousVertexValid)
		{
			D3D12_RESOURCE_BARRIER copyBarriers[2] =
			{
				CD3DX12_RESOURCE_BARRIER::Transition(m_Meshes[m].VertexBuffer.Get(),
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(m_Meshes[m].PreviousVertexBuffer.Get(),
					D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_COPY_DEST)
			};
			pCommandList->ResourceBarrier(_countof(copyBarriers), copyBarriers);
			pCommandList->CopyResource(m_Meshes[m].PreviousVertexBuffer.Get(), m_Meshes[m].VertexBuffer.Get());
			copyBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(m_Meshes[m].VertexBuffer.Get(),
				D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			copyBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(m_Meshes[m].PreviousVertexBuffer.Get(),
				D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
			pCommandList->ResourceBarrier(_countof(copyBarriers), copyBarriers);
		}
		const UINT descriptorBase = m * m_kSKINNING_DESCRIPTORS_PER_MESH;

		CD3DX12_GPU_DESCRIPTOR_HANDLE srvInputHandle(m_SkinningDescHeap->GetGPUDescriptorHandleForHeapStart(), m_Meshes[m].SrvInputVertexIndex, descSize);
		pCommandList->SetComputeRootDescriptorTable(0, srvInputHandle);

		const UINT boneFrameIndex = RendererCore::GetFrameIndex() % RendererState::g_kFRAME_COUNT;
		CD3DX12_GPU_DESCRIPTOR_HANDLE srvBoneHandle(m_SkinningDescHeap->GetGPUDescriptorHandleForHeapStart(), descriptorBase + m_kBONE_SRV_OFFSET + boneFrameIndex, descSize);
		pCommandList->SetComputeRootDescriptorTable(1, srvBoneHandle);

		CD3DX12_GPU_DESCRIPTOR_HANDLE uavOutputHandle(m_SkinningDescHeap->GetGPUDescriptorHandleForHeapStart(), m_Meshes[m].UavOutputVertexIndex, descSize);
		pCommandList->SetComputeRootDescriptorTable(2, uavOutputHandle);

		UINT threadGroups = (m_Meshes[m].VertexCount + kSkinningThreadGroupSize - 1) / kSkinningThreadGroupSize;
		pCommandList->Dispatch(threadGroups, 1, 1);
		D3D12_RESOURCE_BARRIER skinningUavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(m_Meshes[m].VertexBuffer.Get());
		pCommandList->ResourceBarrier(1, &skinningUavBarrier);

		if (!m_Meshes[m].PreviousVertexValid)
		{
			D3D12_RESOURCE_BARRIER previousToCopy = CD3DX12_RESOURCE_BARRIER::Transition(
				m_Meshes[m].PreviousVertexBuffer.Get(),
				D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
			pCommandList->ResourceBarrier(1, &previousToCopy);
			D3D12_RESOURCE_BARRIER toCopy = CD3DX12_RESOURCE_BARRIER::Transition(m_Meshes[m].VertexBuffer.Get(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
			pCommandList->ResourceBarrier(1, &toCopy);
			pCommandList->CopyResource(m_Meshes[m].PreviousVertexBuffer.Get(), m_Meshes[m].VertexBuffer.Get());
			D3D12_RESOURCE_BARRIER ready[2] =
			{
				CD3DX12_RESOURCE_BARRIER::Transition(m_Meshes[m].VertexBuffer.Get(),
					D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(m_Meshes[m].PreviousVertexBuffer.Get(),
					D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER)
			};
			pCommandList->ResourceBarrier(_countof(ready), ready);
			m_Meshes[m].PreviousVertexValid = true;
		}

		for (int mode = 0; mode < kToonOutlineModeCount; ++mode)
		{
			if (m_Meshes[m].TeoVertexCounts[mode] == 0 ||
				!m_Meshes[m].TeoInputVertexBuffers[mode] ||
				!m_Meshes[m].TeoVertexBuffers[mode])
			{
				continue;
			}

			CD3DX12_GPU_DESCRIPTOR_HANDLE teoSrvInputHandle(
				m_SkinningDescHeap->GetGPUDescriptorHandleForHeapStart(),
				m_Meshes[m].SrvTeoInputVertexIndices[mode],
				descSize);
			pCommandList->SetComputeRootDescriptorTable(0, teoSrvInputHandle);

			CD3DX12_GPU_DESCRIPTOR_HANDLE teoUavOutputHandle(
				m_SkinningDescHeap->GetGPUDescriptorHandleForHeapStart(),
				m_Meshes[m].UavTeoOutputVertexIndices[mode],
				descSize);
			pCommandList->SetComputeRootDescriptorTable(2, teoUavOutputHandle);

			UINT teoThreadGroups = (m_Meshes[m].TeoVertexCounts[mode] + kSkinningThreadGroupSize - 1) / kSkinningThreadGroupSize;
			pCommandList->Dispatch(teoThreadGroups, 1, 1);
			D3D12_RESOURCE_BARRIER teoUavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(m_Meshes[m].TeoVertexBuffers[mode].Get());
			pCommandList->ResourceBarrier(1, &teoUavBarrier);
		}
	}

	if (ID3D12DescriptorHeap* cbvHeap = RendererResource::GetCbvHeap())
	{
		ID3D12DescriptorHeap* rendererHeaps[] = { cbvHeap };
		pCommandList->SetDescriptorHeaps(_countof(rendererHeaps), rendererHeaps);
	}

	m_DispatchedSkinningVersion = m_SkinningVersion;
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
