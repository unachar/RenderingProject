#include "pch.h"
#include "animationmodel.h"
#include "texturemanager.h"
#include "rendererdraw.h"
#include "renderershader.h"
#include "psomanager.h"
#include "materialpartresolver.h"
#include "material.h"
#include "modelimportutils.h"
#include "toonoutlinebuilder.h"
#include <filesystem>
#include <cctype>

using namespace std;
using namespace DirectX;

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
		Debug::Log("    MaterialPartId=%.0f\n", MaterialPartResolver::ResolveMaterialPartId(scene, mesh));
	}
	Debug::Log("==== end %s model import info ====\n", modelKind);
}

void AnimationModelResource::CreateBone(aiNode* node)
{
	string name = node->mName.C_Str();
	if (m_BoneIndexMap.find(name) == m_BoneIndexMap.end())
	{
		uint32_t idx = (uint32_t)m_BoneNames.size();
		m_BoneNames.push_back(name);
		m_BoneIndexMap[name] = idx;
		Bone bone{};
		bone.BindLocalMatrix = node->mTransformation;
		bone.AnimationMatrix = node->mTransformation;
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

	unsigned int flags =
		aiProcess_Triangulate |
		aiProcess_JoinIdenticalVertices |
		aiProcess_ImproveCacheLocality;
	if (isConvert)
	{
		flags |= aiProcess_ConvertToLeftHanded;
	}
	m_AiScene = ModelImportUtils::ImportScene(fileName, flags);
	if (!m_AiScene)
	{
		Debug::Log("ERROR: Failed to load animation model: %s (%s)\n", fileName, aiGetErrorString());
		return false;
	}
	LogAssimpModelInfo(m_AiScene, fileName, "Animation");

	m_Meshes.resize(m_AiScene->mNumMeshes);
	m_DeformVertex.resize(m_AiScene->mNumMeshes);
	m_GpuSkinVertices.resize(m_AiScene->mNumMeshes);
	m_TeoGpuSkinVertices.resize(m_AiScene->mNumMeshes);
	m_TeoGpuSkinVerticesByMode.resize(m_AiScene->mNumMeshes);

	CreateBone(m_AiScene->mRootNode);

	const filesystem::path modelPath = ModelImportUtils::FromUtf8(fileName);
	const string dirPath = ModelImportUtils::ToUtf8(modelPath.parent_path());

	XMFLOAT3 minPos = { FLT_MAX, FLT_MAX, FLT_MAX };
	XMFLOAT3 maxPos = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
	bool hasVertices = false;

	for (unsigned int m = 0; m < m_AiScene->mNumMeshes; m++)
	{
		aiMesh* mesh = m_AiScene->mMeshes[m];
		m_Meshes[m].TextureIndex = ResolveMeshTextureIndex(mesh, fileName, dirPath);
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
		const float materialPartId = MaterialPartResolver::ResolveMaterialPartId(m_AiScene, mesh);
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

		for (unsigned int b = 0; b < mesh->mNumBones; b++)
		{
			aiBone* bone = mesh->mBones[b];
			m_Bone[bone->mName.C_Str()].OffsetMatrix = bone->mOffsetMatrix;

			auto it = m_BoneIndexMap.find(bone->mName.C_Str());
			uint32_t boneIdx = (it != m_BoneIndexMap.end()) ? it->second : 0;

			for (unsigned int w = 0; w < bone->mNumWeights; w++)
			{
				aiVertexWeight weight = bone->mWeights[w];
				int num = m_DeformVertex[m][weight.mVertexId].BoneNum;
				if (num < m_kMAX_BONE_INFLUENCES)
				{
					m_DeformVertex[m][weight.mVertexId].BoneWeight[num] = weight.mWeight;
					m_DeformVertex[m][weight.mVertexId].BoneName[num] = bone->mName.C_Str();
					m_DeformVertex[m][weight.mVertexId].BoneNum++;

					m_GpuSkinVertices[m][weight.mVertexId].BoneIndices[num] = boneIdx;
					m_GpuSkinVertices[m][weight.mVertexId].BoneWeights[num] = weight.mWeight;
				}
			}
		}

		for (GpuSkinVertex& vertex : m_GpuSkinVertices[m])
		{
			float totalWeight = 0.0f;
			for (int i = 0; i < m_kMAX_BONE_INFLUENCES; ++i)
			{
				totalWeight += vertex.BoneWeights[i];
			}
			if (totalWeight > 0.0f)
			{
				for (int i = 0; i < m_kMAX_BONE_INFLUENCES; ++i)
				{
					vertex.BoneWeights[i] /= totalWeight;
				}
			}
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

			D3D12_SUBRESOURCE_DATA ibData {};
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

			for (int mode = 0; mode < ToonOutlineBuilder::kModeCount; ++mode)
			{
				vector<unsigned int> teoIndices;
				ToonOutlineBuilder::BuildTeoMesh(
					m_GpuSkinVertices[m],
					indices,
					m_TeoGpuSkinVerticesByMode[m][mode],
					teoIndices,
					static_cast<ToonOutlineBuilder::Mode>(mode));
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

				D3D12_SUBRESOURCE_DATA teoIbData {};
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

				if (mode == static_cast<int>(ToonOutlineBuilder::Mode::Balanced))
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
	}

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

	UpdateBindPoseBoneMatrix(m_AiScene->mRootNode, aiMatrix4x4());
	WriteBoneMatricesToBuffer();

	return true;
}

bool AnimationModelResource::LoadAnimation(const char* fileName, const char* name)
{
	const aiScene* anim = ModelImportUtils::ImportScene(fileName, aiProcess_ConvertToLeftHanded);
	if (!anim)
	{
		Debug::Log("ERROR: Failed to load animation: %s (%s)\n", fileName, aiGetErrorString());
		return false;
	}
	m_Animation[name] = anim;
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
		HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
			&resDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_BoneBuffer));
		if (FAILED(hr)) return false;

		CD3DX12_RANGE readRange(0, 0);
		hr = m_BoneBuffer->Map(0, &readRange, &m_pBoneBufferMapped);
		if (FAILED(hr))
		{
			Debug::Log("ERROR: Failed to map bone buffer\n");
			m_pBoneBufferMapped = nullptr;
			return false;
		}

		if (m_pBoneBufferMapped)
		{
			vector<XMFLOAT4X4> identityBones(m_kMAX_BONES);
			XMFLOAT4X4 identity;
			XMStoreFloat4x4(&identity, XMMatrixIdentity());
			for (auto& mtx : identityBones)
			{
				mtx = identity;
			}
			memcpy(m_pBoneBufferMapped, identityBones.data(), sizeof(XMFLOAT4X4) * m_kMAX_BONES);
		}
	}

	{
		D3D12_DESCRIPTOR_HEAP_DESC heapDesc {};
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

			D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc {};
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

		{
			D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc {};
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srvDesc.Buffer.NumElements = m_kMAX_BONES;
			srvDesc.Buffer.StructureByteStride = sizeof(XMFLOAT4X4);
			srvDesc.Format = DXGI_FORMAT_UNKNOWN;
			CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(m_SkinningDescHeap->GetCPUDescriptorHandleForHeapStart(),
				descriptorBase + m_kBONE_SRV_OFFSET, descSize);
			device->CreateShaderResourceView(m_BoneBuffer.Get(), &srvDesc, srvHandle);
		}

		{
			UINT bufSize = sizeof(ModelVertex) * vertexCount;
			auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
			auto resDesc = CD3DX12_RESOURCE_DESC::Buffer(bufSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
			HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
				&resDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_Meshes[m].VertexBuffer));
			if (FAILED(hr)) return false;

			D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc {};
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
		}

		for (int mode = 0; mode < ToonOutlineBuilder::kModeCount; ++mode)
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

				D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc {};
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

				D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc {};
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

				if (mode == static_cast<int>(ToonOutlineBuilder::Mode::Balanced))
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

		for (int mode = 0; mode < ToonOutlineBuilder::kModeCount; ++mode)
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
			if (mode == static_cast<int>(ToonOutlineBuilder::Mode::Balanced))
			{
				m_TeoGpuSkinVertices[meshIndex] = teoVertices;
			}
		}
	}
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
	if (!m_pBoneBufferMapped)
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
		const aiMatrix4x4& src = m_Bone.at(kv.first).Matrix;
		XMFLOAT4X4& dst = m_BoneMatricesScratch[idx];
		dst._11 = src.a1; dst._12 = src.a2; dst._13 = src.a3; dst._14 = src.a4;
		dst._21 = src.b1; dst._22 = src.b2; dst._23 = src.b3; dst._24 = src.b4;
		dst._31 = src.c1; dst._32 = src.c2; dst._33 = src.c3; dst._34 = src.c4;
		dst._41 = src.d1; dst._42 = src.d2; dst._43 = src.d3; dst._44 = src.d4;
	}

	UINT copySize = sizeof(XMFLOAT4X4) * m_kMAX_BONES;
	memcpy(m_pBoneBufferMapped, m_BoneMatricesScratch.data(), copySize);
}

void AnimationModelResource::UpdateBoneMatrices(const char* animName1, float frame1,
	const char* animName2, float frame2, float blendRate)
{
	if (!m_AiScene)
	{
		return;
	}

	aiAnimation* animation1 = GetAnimation(animName1);
	aiAnimation* animation2 = GetAnimation(animName2);

	if (!animation1 && !animation2)
	{
		UpdateBindPoseBoneMatrix(m_AiScene->mRootNode, aiMatrix4x4());
		WriteBoneMatricesToBuffer();
		return;
	}

	for (auto& [boneName, bone] : m_Bone)
	{
		aiNodeAnim* nodeAnim1 = FindNodeAnimChannel(animation1, boneName);
		aiNodeAnim* nodeAnim2 = FindNodeAnimChannel(animation2, boneName);

		aiVector3D bindScale(1.0f, 1.0f, 1.0f);
		aiQuaternion bindRotation(1, 0, 0, 0);
		aiVector3D bindPosition(0, 0, 0);
		bone.BindLocalMatrix.Decompose(bindScale, bindRotation, bindPosition);

		aiQuaternion rotation1 = bindRotation;
		aiVector3D pos1 = bindPosition;
		aiVector3D scale1 = bindScale;
		aiQuaternion rotation2 = bindRotation;
		aiVector3D pos2 = bindPosition;
		aiVector3D scale2 = bindScale;

		SampleNodeAnimation(animation1, nodeAnim1, frame1, rotation1, pos1, scale1);
		SampleNodeAnimation(animation2, nodeAnim2, frame2, rotation2, pos2, scale2);

		const aiVector3D pos = pos1 * (1.f - blendRate) + pos2 * blendRate;
		const aiVector3D scale = scale1 * (1.f - blendRate) + scale2 * blendRate;

		aiQuaternion rotation;
		aiQuaternion::Interpolate(rotation, rotation1, rotation2, blendRate);

		bone.AnimationMatrix = aiMatrix4x4(scale, rotation, pos);
	}

	UpdateBoneMatrix(m_AiScene->mRootNode, aiMatrix4x4());
	WriteBoneMatricesToBuffer();
}

void AnimationModelResource::DispatchGpuSkinning(ID3D12GraphicsCommandList* pCommandList)
{
	if (!m_SkinningDescHeap || !RendererShader::GetSkinningRootSignature() || !PsoManager::GetSkinningPso()) return;

	pCommandList->SetComputeRootSignature(RendererShader::GetSkinningRootSignature());
	pCommandList->SetPipelineState(PsoManager::GetSkinningPso());

	ID3D12DescriptorHeap* heaps[] = { m_SkinningDescHeap.Get() };
	pCommandList->SetDescriptorHeaps(_countof(heaps), heaps);

	UINT descSize = m_pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	for (UINT m = 0; m < m_Meshes.size(); m++)
	{
		if (m_Meshes[m].VertexCount == 0) continue;
		const UINT descriptorBase = m * m_kSKINNING_DESCRIPTORS_PER_MESH;

		CD3DX12_GPU_DESCRIPTOR_HANDLE srvInputHandle(m_SkinningDescHeap->GetGPUDescriptorHandleForHeapStart(), m_Meshes[m].SrvInputVertexIndex, descSize);
		pCommandList->SetComputeRootDescriptorTable(0, srvInputHandle);

		CD3DX12_GPU_DESCRIPTOR_HANDLE srvBoneHandle(m_SkinningDescHeap->GetGPUDescriptorHandleForHeapStart(), descriptorBase + m_kBONE_SRV_OFFSET, descSize);
		pCommandList->SetComputeRootDescriptorTable(1, srvBoneHandle);

		CD3DX12_GPU_DESCRIPTOR_HANDLE uavOutputHandle(m_SkinningDescHeap->GetGPUDescriptorHandleForHeapStart(), m_Meshes[m].UavOutputVertexIndex, descSize);
		pCommandList->SetComputeRootDescriptorTable(2, uavOutputHandle);

		UINT threadGroups = (m_Meshes[m].VertexCount + 63) / 64;
		pCommandList->Dispatch(threadGroups, 1, 1);

		for (int mode = 0; mode < ToonOutlineBuilder::kModeCount; ++mode)
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

			UINT teoThreadGroups = (m_Meshes[m].TeoVertexCounts[mode] + 63) / 64;
			pCommandList->Dispatch(teoThreadGroups, 1, 1);
		}
	}

	if (ID3D12DescriptorHeap* cbvHeap = RendererResource::GetCbvHeap())
	{
		ID3D12DescriptorHeap* rendererHeaps[] = { cbvHeap };
		pCommandList->SetDescriptorHeaps(_countof(rendererHeaps), rendererHeaps);
	}
}

void AnimationModelResource::Uninit()
{
	if (m_BoneBuffer && m_pBoneBufferMapped)
	{
		m_BoneBuffer->Unmap(0, nullptr);
		m_pBoneBufferMapped = nullptr;
	}

	m_Meshes.clear();
	m_DeformVertex.clear();
	m_GpuSkinVertices.clear();
	m_Bone.clear();
	m_BoneNames.clear();
	m_BoneIndexMap.clear();
	m_BoneMatricesScratch.clear();
	m_AabbCenter = {};
	m_AabbExtents = {};

	if (m_AiScene)
	{
		aiReleaseImport(m_AiScene);
		m_AiScene = nullptr;
	}

	for (auto& pair : m_Animation)
	{
		aiReleaseImport(pair.second);
	}
	m_Animation.clear();
}

