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

	float ticksPerSecond;
	if ((animation->mTicksPerSecond != 0))
	{
		ticksPerSecond = static_cast<float>(animation->mTicksPerSecond);
	}
	else
	{
		ticksPerSecond = 25.0f;
	}
	const float duration = static_cast<float>(animation->mDuration);
	float t;
	if (duration > 0.0f)
	{
		t = fmod(max(0.0f, timeSeconds) * ticksPerSecond, duration);
	}
	else
	{
		t = 0.0f;
	}

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
					float factor;
					if (end > start)
					{
						factor = clamp((t - start) / (end - start), 0.0f, 1.0f);
					}
					else
					{
						factor = 0.0f;
					}
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
					float factor;
					if (end > start)
					{
						factor = clamp((t - start) / (end - start), 0.0f, 1.0f);
					}
					else
					{
						factor = 0.0f;
					}
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
				XMMATRIX local;
				if (fn.BonePtr)
				{
					local = aiToXm(fn.BonePtr->AnimationMatrix);
				}
				else
				{
					local = aiToXm(fn.Node->mTransformation);
				}
				XMMATRIX world;
				if (fn.ParentIndex >= 0)
				{
					world = XMMatrixMultiply(local, fastGlobals[fn.ParentIndex]);
				}
				else
				{
					world = local;
				}
				fastGlobals[i] = world;
				XMFLOAT4X4 stored{};
				XMStoreFloat4x4(&stored, world);
				globalMatrices[fn.Name] = stored;
			}
		};

	auto loadGlobal = [&](const string& boneName)
		{
			auto it = globalMatrices.find(boneName);
			if (it != globalMatrices.end())
			{
				return XMLoadFloat4x4(&it->second);
			}
			return XMMatrixIdentity();
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
		float maxAngle;
		if (constraint.LimitAngle > 0.0f)
		{
			maxAngle = constraint.LimitAngle;
		}
		else
		{
			maxAngle = XM_PIDIV4;
		}
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
