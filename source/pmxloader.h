#pragma once

#include "animationmodel.h"
#include "pmxphysicsdata.h"


	struct PmxVertex
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

	struct PmxMaterial
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

	struct PmxBinaryIkLink
	{
		int32_t BoneIndex = -1;
		bool HasLimit = false;
		aiVector3D LimitMin{};
		aiVector3D LimitMax{};
	};

	struct PmxBone
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
		vector<PmxBinaryIkLink> IkLinks{};
	};

	struct PmxModel
	{
		string ModelName{};
		string EnglishModelName{};
		vector<PmxVertex> Vertices{};
		vector<uint32_t> Indices{};
		vector<string> Textures{};
		vector<PmxMaterial> Materials{};
		vector<PmxBone> Bones{};
		vector<PmxMorph> Morphs{};
		vector<PmxRigidBodyData> RigidBodies{};
		vector<PmxJointData> Joints{};
	};

	bool LoadPmxModel(const char* fileName, PmxModel& outModel);
	aiScene* CreatePmxGeneratedScene(const PmxModel& model, vector<vector<uint32_t>>& outMeshVertexPmxIndices);
	void DestroyPmxGeneratedScene(aiScene* scene);
	void PopulatePmxAnimationMetadata(
		const PmxModel& model,
		vector<PmxAppendConstraint>& appendConstraints,
		vector<PmxIkConstraint>& ikConstraints,
		vector<aiVector3D>& baseVertices,
		vector<aiVector3D>& baseNormals,
		vector<XMFLOAT2>& baseTexCoords,
		vector<PmxMorph>& morphs,
		unordered_map<string, uint32_t>& morphIndexMap);
	void ApplyPmxVertexDeformData(
		const PmxModel& model,
		const vector<vector<uint32_t>>& meshVertexPmxIndices,
		uint32_t meshIndex,
		uint32_t vertexIndex,
		GpuSkinVertex& gpuVertex);
