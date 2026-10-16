#include "pch.h"
#include "animationmodel.h"
#include "animationmath.h"
#include "graphicsdevice.h"


using namespace AnimationMath;

aiNodeAnim* AnimationModelResource::FindNodeAnimChannelCached(aiAnimation* animation, const string& boneName)
{
	if (!animation)
	{
		return nullptr;
	}

	auto& perAnim = m_NodeAnimChannelCache[animation];
	auto cached = perAnim.find(boneName);
	if (cached != perAnim.end())
	{
		return cached->second;
	}

	aiNodeAnim* found = nullptr;
	for (unsigned int c = 0; c < animation->mNumChannels; ++c)
	{
		aiNodeAnim* channel = animation->mChannels[c];
		if (channel && strcmp(channel->mNodeName.C_Str(), boneName.c_str()) == 0)
		{
			found = channel;
			break;
		}
	}
	perAnim.emplace(boneName, found);
	return found;
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

	// Perf: 実使用ボーン数のみ転送する (バッファは生成時に identity 初期化済み)。
	if (m_SkinningMatrixCount == 0)
	{
		uint32_t maxIndex = 0;
		for (const auto& kv : m_BoneIndexMap)
		{
			maxIndex = max(maxIndex, kv.second);
		}
		m_SkinningMatrixCount = min<uint32_t>(m_kMAX_BONES, maxIndex + 1);
	}
	uint32_t matrixCount;
	if ((m_SkinningMatrixCount != 0))
	{
		matrixCount = m_SkinningMatrixCount;
	}
	else
	{
		matrixCount = m_kMAX_BONES;
	}

	if (m_BoneMatricesScratch.size() != matrixCount)
	{
		m_BoneMatricesScratch.resize(matrixCount);
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
		if (idx >= matrixCount) continue;
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
		GraphicsDevice::GetFrameIndex() % RendererState::g_kFRAME_COUNT;
	void* mapped = m_pBoneBufferMapped[frameIndex];
	if (mapped)
	{
		const UINT copySize = sizeof(XMFLOAT4X4) * matrixCount;
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
		XMMATRIX local;
		if (node.BonePtr)
		{
			local = aiToXm(node.BonePtr->AnimationMatrix);
		}
		else
		{
			local = aiToXm(node.Node->mTransformation);
		}
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
		XMMATRIX local;
		if (node.BonePtr)
		{
			local = aiToXm(node.BonePtr->BindLocalMatrix);
		}
		else
		{
			local = aiToXm(node.Node->mTransformation);
		}
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

void AnimationModelResource::UpdateVmdBoneMatrices(const VmdAnimation* animation1, float frame1,
	const VmdAnimation* animation2, float frame2, float blendRate)
{
	m_UseLayeredVmdIk = false;
	if (!m_AiScene)
	{
		return;
	}

	const VmdAnimation* primaryAnimation;
	if (animation1)
	{
		primaryAnimation = animation1;
	}
	else
	{
		primaryAnimation = animation2;
	}
	const VmdAnimation* secondaryAnimation;
	if (animation2)
	{
		secondaryAnimation = animation2;
	}
	else
	{
		secondaryAnimation = primaryAnimation;
	}
	if (!primaryAnimation)
	{
		return;
	}

	if (m_CachedVmdPrimaryAnimation != primaryAnimation || m_CachedVmdSecondaryAnimation != secondaryAnimation)
	{
		RebuildVmdRuntimeCache(primaryAnimation, secondaryAnimation);
	}

	const float safeBlendRate = clamp(blendRate, 0.0f, 1.0f);
	float primaryTime;
	if (animation1)
	{
		primaryTime = frame1;
	}
	else
	{
		primaryTime = frame2;
	}
	float secondaryTime;
	if (animation2)
	{
		secondaryTime = frame2;
	}
	else
	{
		secondaryTime = primaryTime;
	}
	const float currentFrame1 = VmdToFrameTime(primaryAnimation, primaryTime);
	const bool useSecondarySample = secondaryAnimation && safeBlendRate > 0.0001f &&
		(secondaryAnimation != primaryAnimation || fabsf(secondaryTime - primaryTime) > 0.0001f);
	float currentFrame2;
	if (useSecondarySample)
	{
		currentFrame2 = VmdToFrameTime(secondaryAnimation, secondaryTime);
	}
	else
	{
		currentFrame2 = currentFrame1;
	}
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

void AnimationModelResource::UpdateBoneMatrices(const char* animName1, float frame1,
	const char* animName2, float frame2, float blendRate)
{
	if (!m_AiScene)
	{
		return;
	}
	m_HasCachedLayeredPose = false;
	m_UseLayeredVmdIk = false;

	string animationName1;
	if (animName1)
	{
		animationName1 = animName1;
	}
	else
	{
		animationName1 = "";
	}
	string animationName2;
	if (animName2)
	{
		animationName2 = animName2;
	}
	else
	{
		animationName2 = "";
	}
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

			float t;
			if (duration > 0.0)
			{
				t = fmod(timeInTicks, static_cast<float>(duration));
			}
			else
			{
				t = timeInTicks;
			}
			const unsigned int keyIndex = static_cast<unsigned int>(max(0.0f, t)) % nodeAnim->mNumRotationKeys;
			return nodeAnim->mRotationKeys[keyIndex].mValue;
		};

	auto samplePositionKey = [](aiNodeAnim* nodeAnim, float timeInTicks, double duration)
		{
			if (!nodeAnim || nodeAnim->mNumPositionKeys == 0)
			{
				return aiVector3D(0.0f, 0.0f, 0.0f);
			}

			float t;
			if (duration > 0.0)
			{
				t = fmod(timeInTicks, static_cast<float>(duration));
			}
			else
			{
				t = timeInTicks;
			}
			const unsigned int keyIndex = static_cast<unsigned int>(max(0.0f, t)) % nodeAnim->mNumPositionKeys;
			return nodeAnim->mPositionKeys[keyIndex].mValue;
		};

	for (auto& pair : m_Bone)
	{
		Bone* bonePtr = &pair.second;
		aiNodeAnim* nodeAnim1 = FindNodeAnimChannelCached(animation1, pair.first);
		aiNodeAnim* nodeAnim2 = FindNodeAnimChannelCached(animation2, pair.first);

		float ticksPerSecond1;
		if ((animation1 && animation1->mTicksPerSecond != 0))
		{
			ticksPerSecond1 = static_cast<float>(animation1->mTicksPerSecond);
		}
		else
		{
			ticksPerSecond1 = 25.0f;
		}
		float ticksPerSecond2;
		if ((animation2 && animation2->mTicksPerSecond != 0))
		{
			ticksPerSecond2 = static_cast<float>(animation2->mTicksPerSecond);
		}
		else
		{
			ticksPerSecond2 = 25.0f;
		}

		const float timeInTicks1 = frame1 * ticksPerSecond1;
		const float timeInTicks2 = frame2 * ticksPerSecond2;

		double animationDuration;
		if (animation1)
		{
			animationDuration = animation1->mDuration;
		}
		else
		{
			animationDuration = 0.0;
		}
		aiQuaternion rotation1 = sampleRotationKey(nodeAnim1, timeInTicks1, animationDuration);
		double animationDurationValue;
		if (animation1)
		{
			animationDurationValue = animation1->mDuration;
		}
		else
		{
			animationDurationValue = 0.0;
		}
		aiVector3D pos1 = samplePositionKey(nodeAnim1, timeInTicks1, animationDurationValue);
		double animationDuration3;
		if (animation2)
		{
			animationDuration3 = animation2->mDuration;
		}
		else
		{
			animationDuration3 = 0.0;
		}
		aiQuaternion rotation2 = sampleRotationKey(nodeAnim2, timeInTicks2, animationDuration3);
		double animationDuration4;
		if (animation2)
		{
			animationDuration4 = animation2->mDuration;
		}
		else
		{
			animationDuration4 = 0.0;
		}
		aiVector3D pos2 = samplePositionKey(nodeAnim2, timeInTicks2, animationDuration4);

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

