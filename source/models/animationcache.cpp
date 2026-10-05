#include "pch.h"
#include "animationmodel.h"
#include "animationmath.h"


using namespace AnimationMath;

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

	const VmdAnimation* secondary;
	if (secondaryAnimation)
	{
		secondary = secondaryAnimation;
	}
	else
	{
		secondary = primaryAnimation;
	}
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
		const VmdAnimation* animation;
		if (animationIt != m_VmdAnimations.end())
		{
			animation = &animationIt->second;
		}
		else
		{
			animation = nullptr;
		}
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
		const VmdAnimation* layerAnimation;
		if (animationIt != m_VmdAnimations.end())
		{
			layerAnimation = &animationIt->second;
		}
		else
		{
			layerAnimation = nullptr;
		}
		m_CachedVmdLayerAnimations.push_back(
			layerAnimation);
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

void AnimationModelResource::InvalidateAnimationPoseCache()
{
	m_HasCachedPose = false;
	m_HasCachedLayeredPose = false;
	m_UseLayeredVmdIk = false;
}
