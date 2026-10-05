#pragma once
#include "animationmodel.h"
#include <cmath>

namespace AnimationMath
{
inline aiMatrix4x4 MakeAiIdentityMatrix()
{
	return aiMatrix4x4(
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f);
}

inline bool IsFiniteAiMatrix(const aiMatrix4x4& matrix)
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

inline bool IsUsableSkinningMatrix(const aiMatrix4x4& matrix)
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

inline bool FindNodeGlobalMatrix(aiNode* node, const string& boneName, const aiMatrix4x4& parentMatrix, aiMatrix4x4& outMatrix)
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
}
