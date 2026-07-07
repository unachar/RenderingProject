#pragma once

#include "animationmodel.h"

namespace PmxBinary
{
	struct Vertex
	{
		aiVector3D Position{};
		aiVector3D Normal{};
		XMFLOAT2 TexCoord{};
		uint8_t DeformType = 0;
		int32_t BoneIndices[4] = { -1, -1, -1, -1 };
		float BoneWeights[4] = {};
		aiVector3D SdefC{};
		aiVector3D SdefR0{};
		aiVector3D SdefR1{};
		float EdgeScale = 1.0f;
	};

	struct Material
	{
		string Name{};
		string EnglishName{};
		XMFLOAT4 Diffuse{ 1.0f, 1.0f, 1.0f, 1.0f };
		XMFLOAT3 Specular{};
		float SpecularPower = 0.0f;
		XMFLOAT3 Ambient{};
		uint8_t DrawFlags = 0;
		XMFLOAT4 EdgeColor{};
		float EdgeSize = 0.0f;
		int32_t TextureIndex = -1;
		int32_t SphereTextureIndex = -1;
		uint8_t SphereMode = 0;
		uint8_t ToonMode = 0;
		int32_t ToonTextureIndex = -1;
		uint32_t IndexCount = 0;
	};

	struct IkLink
	{
		int32_t BoneIndex = -1;
		bool HasLimit = false;
		aiVector3D LimitMin{};
		aiVector3D LimitMax{};
	};

	struct Bone
	{
		string Name{};
		string EnglishName{};
		aiVector3D Position{};
		int32_t ParentIndex = -1;
		int32_t DeformDepth = 0;
		uint16_t Flags = 0;
		int32_t AppendBoneIndex = -1;
		float AppendWeight = 0.0f;
		int32_t IkTargetIndex = -1;
		uint32_t IkIterationCount = 0;
		float IkLimitAngle = 0.0f;
		vector<IkLink> IkLinks{};
	};

	struct Model
	{
		string ModelName{};
		string EnglishModelName{};
		vector<Vertex> Vertices{};
		vector<uint32_t> Indices{};
		vector<string> Textures{};
		vector<Material> Materials{};
		vector<Bone> Bones{};
		vector<PmxMorph> Morphs{};
	};

	bool LoadModel(const char* fileName, Model& outModel);
	aiScene* CreateGeneratedScene(const Model& model, vector<vector<uint32_t>>& outMeshVertexPmxIndices);
	void DestroyGeneratedScene(aiScene* scene);
	void PopulateAnimationMetadata(
		const Model& model,
		vector<PmxAppendConstraint>& appendConstraints,
		vector<PmxIkConstraint>& ikConstraints,
		vector<aiVector3D>& baseVertices,
		vector<aiVector3D>& baseNormals,
		vector<XMFLOAT2>& baseTexCoords,
		vector<PmxMorph>& morphs,
		unordered_map<string, uint32_t>& morphIndexMap);
	void ApplyVertexDeformData(
		const Model& model,
		const vector<vector<uint32_t>>& meshVertexPmxIndices,
		uint32_t meshIndex,
		uint32_t vertexIndex,
		GpuSkinVertex& gpuVertex);
}
