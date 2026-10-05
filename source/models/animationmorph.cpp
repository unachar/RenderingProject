#include "pch.h"
#include "animationmodel.h"
#include "animationmath.h"


using namespace AnimationMath;

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
				XMMATRIX local;
				if (fn.BonePtr)
				{
					local = aiToXm(fn.BonePtr->AnimationMatrix);
				}
				else
				{
					local = aiToXm(fn.Node->mTransformation);
				}
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
			if (it != m_PmxRuntimeNodeIndexMap.end())
			{
				return XMLoadFloat4x4(&m_PmxGlobalMatricesScratch[it->second]);
			}
			return XMMatrixIdentity();
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
				float sign;
				if (component[fallbackAxis] < 0.0f)
				{
					sign = -1.0f;
				}
				else
				{
					sign = 1.0f;
				}
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
						int hingeAxisIndex;
						if (link.HasLimit)
						{
							hingeAxisIndex = pickIkLimitAxis(link, minLimit, maxLimit);
						}
						else
						{
							hingeAxisIndex = 0;
						}
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

						float sign;
						if (XMVectorGetX(XMVector3Dot(XMVector3Cross(projectedEffector, projectedGoal), worldAxis)) < 0.0f)
						{
							sign = -1.0f;
						}
						else
						{
							sign = 1.0f;
						}
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
		bool changed;
		if (step.IsIk)
		{
			changed = solveIk(m_PmxIkConstraints[step.Index]);
		}
		else
		{
			changed = applyAppend(m_PmxAppendConstraints[step.Index]);
		}
		if (changed)
		{
			rebuildGlobals();
		}
	}
}
