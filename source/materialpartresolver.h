#pragma once

#include "material.h"
#include "scene.h"
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace MaterialPartResolver
{
	inline const vector<string_view> kWearMaterialTokens =
	{
		"wear", "other"
	};

	inline const vector<string_view> kWearMeshTokens =
	{
		"other", "other02"
	};

	inline const vector<string_view> kAccessoryTokens =
	{
		"ribbon", "bow", "choker", "collar", "necklace", "lace",
		"frill", "accessory", "ornament", "deco"
	};

	inline const vector<string_view> kHairTokens =
	{
		"hair", "kami", "bang", "brow", "eyelash"
	};

	inline const vector<string_view> kSkinTokens =
	{
		"skin", "body", "face", "head", "hand", "arm", "leg",
		"neck", "ear", "eye", "iris", "pupil", "hada"
	};

	inline const vector<string_view> kClothTokens =
	{
		"cloth", "dress", "shoe", "socks", "skirt"
	};

	inline string ToLowerString(string value)
	{
		transform(value.begin(), value.end(), value.begin(), [](unsigned char c)
			{
				return static_cast<char>(tolower(c));
			});
		return value;
	}

	inline bool ContainsAnyToken(const string& value, const vector<string_view>& tokens)
	{
		for (string_view token : tokens)
		{
			if (value.find(token) != string::npos)
			{
				return true;
			}
		}
		return false;
	}

	inline float ResolveMaterialPartId(const aiScene* scene, const aiMesh* mesh)
	{
		if (!scene || !mesh || mesh->mMaterialIndex >= scene->mNumMaterials)
		{
			return 10.0f;
		}

		const aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];
		aiString name;
		material->Get(AI_MATKEY_NAME, name);
		const string materialName = ToLowerString(name.C_Str());
		const string meshName = ToLowerString(mesh->mName.C_Str());
		const string lowerName = materialName + " " + meshName;

		if (ContainsAnyToken(materialName, kWearMaterialTokens) ||
			ContainsAnyToken(meshName, kWearMeshTokens) ||
			(materialName.find("face_alpha") != string::npos && meshName.find("other") != string::npos) ||
			ContainsAnyToken(lowerName, kAccessoryTokens))
		{
			return 2.0f;
		}

		if (ContainsAnyToken(lowerName, kHairTokens))
		{
			return 1.0f;
		}

		if (ContainsAnyToken(lowerName, kSkinTokens))
		{
			return 3.0f;
		}

		if (ContainsAnyToken(lowerName, kClothTokens))
		{
			return 2.0f;
		}

		if (mesh->mMaterialIndex == 0)
		{
			return 1.0f;
		}
		if (mesh->mMaterialIndex == 2)
		{
			return 3.0f;
		}
		return 2.0f;
	}
}
