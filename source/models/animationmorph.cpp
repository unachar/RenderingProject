#include "pch.h"
#include "animationmodel.h"
#include "animationmath.h"
#include "graphicsdevice.h"
#include <charconv>
#include <map>

using namespace AnimationMath;

void AnimationModelResource::BuildPmxVertexMeshMap()
{
    m_PmxVertexToMeshVertices.clear();
    if (m_PmxBaseVertices.empty() || m_GpuSkinVertices.empty())
    {
        return;
    }

    auto quantize = [](float value) {
        return llround(static_cast<double>(value) * 10000.0);
    };
    auto makePositionKey = [&](float x, float y, float z) {
        return to_string(quantize(x)) + "," + to_string(quantize(y)) + "," + to_string(quantize(z));
    };
    auto makeVertexKey = [&](const aiVector3D& position, const aiVector3D& normal, const XMFLOAT2& uv) {
        return makePositionKey(position.x, position.y, position.z) + "|" +
               makePositionKey(normal.x, normal.y, normal.z) + "|" + to_string(quantize(uv.x)) + "," +
               to_string(quantize(uv.y));
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
                m_PmxVertexToMeshVertices[pmxVertexIndex].push_back({meshIndex, vertexIndex});
            }
        }
    }

    Debug::Log("PMX morph vertex map built: pmxVertices=%zu, mappedMeshVertices=%zu, preciseMappedMeshVertices=%zu\n",
               m_PmxBaseVertices.size(), mappedMeshVertices, preciseMappedMeshVertices);
}

void AnimationModelResource::ApplyVmdMorphs(const VmdAnimation* animation, float currentFrame)
{
    if (!m_IsPmxModel)
    {
        ApplyImportedVmdMorphs({{animation, currentFrame}});
        return;
    }
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
            m_VmdActiveMorphsScratch.push_back({binding.MorphIndex, weight});
        }
    }
    ApplyActiveVmdMorphs();
}

void AnimationModelResource::ApplyVmdMorphLayers()
{
    if (!m_IsPmxModel)
    {
        vector<pair<const VmdAnimation*, float>> layers;
        for (size_t index = 0; index < m_CachedVmdLayerAnimations.size(); ++index)
        {
            layers.emplace_back(m_CachedVmdLayerAnimations[index], m_VmdLayerFramesScratch[index]);
        }
        ApplyImportedVmdMorphs(layers);
        return;
    }
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
        const float weight =
            SampleVmdMorphTrackCached(binding.Track, m_VmdLayerFramesScratch[binding.LayerIndex], binding.Cursor);
        if (fabsf(weight) > 0.00001f)
        {
            m_VmdActiveMorphsScratch.push_back({binding.MorphIndex, weight});
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

    const bool canApplyVertexMorphs = !m_PmxVertexToMeshVertices.empty() && !m_BaseGpuSkinVertices.empty() &&
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
    fill(m_VmdMorphPositionOffsetsScratch.begin(), m_VmdMorphPositionOffsetsScratch.end(),
         aiVector3D(0.0f, 0.0f, 0.0f));
    fill(m_VmdMorphUvOffsetsScratch.begin(), m_VmdMorphUvOffsetsScratch.end(), XMFLOAT2(0.0f, 0.0f));

    vector<aiVector3D>& accumulatedOffsets = m_VmdMorphPositionOffsetsScratch;
    vector<XMFLOAT2>& accumulatedUvOffsets = m_VmdMorphUvOffsetsScratch;
    bool vertexBufferChanged = false;
    auto accumulateMorph = [&](auto&& self, uint32_t morphIndex, float weight, int depth) -> void {
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
                m_ResourceMap.ForEachMaterialMesh(offset.MaterialIndex, m_Meshes.size(), [&](uint32_t meshIndex) {
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
                });
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
            aiQuaternion::Interpolate(weightedRotation, aiQuaternion(1.0f, 0.0f, 0.0f, 0.0f), offset.Rotation,
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
        for (uint32_t pmxVertexIndex = 0; pmxVertexIndex < static_cast<uint32_t>(accumulatedOffsets.size());
             ++pmxVertexIndex)
        {
            const aiVector3D& offset = accumulatedOffsets[pmxVertexIndex];
            const XMFLOAT2& uvOffset = accumulatedUvOffsets[pmxVertexIndex];
            const bool hasPositionOffset =
                fabsf(offset.x) > 0.000001f || fabsf(offset.y) > 0.000001f || fabsf(offset.z) > 0.000001f;
            const bool hasUvOffset = fabsf(uvOffset.x) > 0.000001f || fabsf(uvOffset.y) > 0.000001f;
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

    auto aiToXm = [](const aiMatrix4x4& src) {
        return XMMatrixSet(src.a1, src.b1, src.c1, src.d1, src.a2, src.b2, src.c2, src.d2, src.a3, src.b3, src.c3,
                           src.d3, src.a4, src.b4, src.c4, src.d4);
    };

    auto rebuildGlobals = [&]() {
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
                const XMMATRIX parent =
                    XMLoadFloat4x4(&m_PmxGlobalMatricesScratch[static_cast<size_t>(fn.ParentIndex)]);
                world = XMMatrixMultiply(local, parent);
            }
            XMStoreFloat4x4(&m_PmxGlobalMatricesScratch[i], world);
        }
    };

    auto loadGlobal = [&](const string& boneName) {
        auto it = m_PmxRuntimeNodeIndexMap.find(boneName);
        if (it != m_PmxRuntimeNodeIndexMap.end())
        {
            return XMLoadFloat4x4(&m_PmxGlobalMatricesScratch[it->second]);
        }
        return XMMatrixIdentity();
    };

    auto getTranslation = [&](const string& boneName) {
        auto it = m_PmxRuntimeNodeIndexMap.find(boneName);
        if (it == m_PmxRuntimeNodeIndexMap.end())
        {
            return XMVectorZero();
        }

        const XMFLOAT4X4& matrix = m_PmxGlobalMatricesScratch[it->second];
        return XMVectorSet(matrix._41, matrix._42, matrix._43, 1.0f);
    };

    auto getParentGlobal = [&](const string& boneName) {
        auto parentIt = m_BoneParentMap.find(boneName);
        if (parentIt == m_BoneParentMap.end())
        {
            return XMMatrixIdentity();
        }
        return loadGlobal(parentIt->second);
    };
    auto applyIkLinkLimit = [](const PmxIkLink& link, XMVECTOR& localAxis, float& angle) -> bool {
        if (!link.HasLimit)
        {
            return true;
        }

        const float component[3] = {
            XMVectorGetX(localAxis),
            XMVectorGetY(localAxis),
            XMVectorGetZ(localAxis),
        };
        const float minLimit[3] = {link.LimitMin.x, link.LimitMin.y, link.LimitMin.z};
        const float maxLimit[3] = {link.LimitMax.x, link.LimitMax.y, link.LimitMax.z};

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

        XMVECTOR limitedAxis = XMVectorSet(limitedComponent[0], limitedComponent[1], limitedComponent[2], 0.0f);
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
            limitedAxis = XMVectorSet(fallbackComponent[0], fallbackComponent[1], fallbackComponent[2], 0.0f);
        }

        localAxis = XMVector3Normalize(limitedAxis);
        angle = min(angle, maxAllowedAngle);
        return true;
    };

    m_PmxAppendResultsScratch.clear();

    auto getLocalAnimationDelta = [](const Bone& bone, aiQuaternion& outRotation, aiVector3D& outTranslation) {
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

    auto applyAppend = [&](const PmxAppendConstraint& constraint) {
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
            aiQuaternion::Interpolate(weightedRotation, aiQuaternion(1.0f, 0.0f, 0.0f, 0.0f), appendRotation,
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

    auto isToeIkConstraint = [](const string& boneName) {
        return boneName.find("つま先") != string::npos;
    };

    auto isKneeBone = [](const string& boneName) {
        return boneName.find("ひざ") != string::npos || boneName.find("膝") != string::npos;
    };

    auto axisVectorFromIndex = [](int axisIndex) {
        switch (axisIndex)
        {
            case 1:
                return XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
            case 2:
                return XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
            default:
                return XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
        }
    };

    auto pickIkLimitAxis = [](const PmxIkLink& link, float& outMinLimit, float& outMaxLimit) {
        const float minLimit[3] = {link.LimitMin.x, link.LimitMin.y, link.LimitMin.z};
        const float maxLimit[3] = {link.LimitMax.x, link.LimitMax.y, link.LimitMax.z};
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

    auto solveIk = [&](const PmxIkConstraint& constraint) {
        if (isToeIkConstraint(constraint.BoneName))
        {
            return false;
        }

        if (!IsVmdIkEnabled(animation, constraint.BoneName, currentFrame) ||
            m_Bone.find(constraint.BoneName) == m_Bone.end() || m_Bone.find(constraint.TargetBoneName) == m_Bone.end())
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
            const float currentDistanceSq =
                XMVectorGetX(XMVector3LengthSq(XMVectorSubtract(goalPosition, effectorPosition)));
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

                    XMVECTOR projectedEffector = XMVectorSubtract(
                        toEffector, XMVectorScale(worldAxis, XMVectorGetX(XMVector3Dot(toEffector, worldAxis))));
                    XMVECTOR projectedGoal = XMVectorSubtract(
                        toGoal, XMVectorScale(worldAxis, XMVectorGetX(XMVector3Dot(toGoal, worldAxis))));
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

vector<string> AnimationModelResource::GetPreviewExpressionNames() const
{
    vector<string> names;
    if (!m_AiScene || !m_PmxMorphs.empty())
    {
        return names;
    }
    for (unsigned meshIndex = 0; meshIndex < m_AiScene->mNumMeshes; ++meshIndex)
    {
        const auto& mesh = *m_AiScene->mMeshes[meshIndex];
        for (unsigned target = 0; target < mesh.mNumAnimMeshes; ++target)
        {
            const auto& morph = *mesh.mAnimMeshes[target];
            names.push_back("Mesh " + to_string(meshIndex + 1) + " / " +
                            (morph.mName.length ? string(morph.mName.C_Str()) : to_string(target + 1)));
        }
    }
    return names;
}

bool AnimationModelResource::SetPreviewExpression(size_t expression, float weight)
{
    if (!m_AiScene || !m_PmxMorphs.empty() || !std::isfinite(weight))
    {
        return false;
    }
    for (size_t meshIndex = 0; meshIndex < m_Meshes.size(); ++meshIndex)
    {
        const auto& mesh = *m_AiScene->mMeshes[meshIndex];
        if (expression >= mesh.mNumAnimMeshes)
        {
            expression -= mesh.mNumAnimMeshes;
            continue;
        }

        auto& meshData = m_Meshes[meshIndex];
        const auto& morph = *mesh.mAnimMeshes[expression];
        if (!meshData.InputVertexBuffer || morph.mNumVertices != mesh.mNumVertices)
        {
            return false;
        }

        UINT8* destination = nullptr;
        const CD3DX12_RANGE readRange(0, 0);
        if (FAILED(meshData.InputVertexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&destination))))
        {
            return false;
        }
        auto& vertices = m_GpuSkinVertices[meshIndex];
        vertices = m_BaseGpuSkinVertices[meshIndex];
        for (size_t index = 0; index < vertices.size(); ++index)
        {
            auto& vertex = vertices[index];
            if (morph.mVertices)
            {
                const auto offset = (morph.mVertices[index] - mesh.mVertices[index]) * weight;
                vertex.Position.x += offset.x;
                vertex.Position.y += offset.y;
                vertex.Position.z += offset.z;
            }
            if (morph.mNormals && mesh.mNormals)
            {
                const auto offset = (morph.mNormals[index] - mesh.mNormals[index]) * weight;
                vertex.Normal.x += offset.x;
                vertex.Normal.y += offset.y;
                vertex.Normal.z += offset.z;
            }
        }
        memcpy(destination, vertices.data(), vertices.size() * sizeof(GpuSkinVertex));
        meshData.InputVertexBuffer->Unmap(0, nullptr);
        ++m_SkinningVersion;
        return true;
    }
    return false;
}

namespace
{
    string MorphUtf8(string_view text)
    {
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0))
        {
            return string(text);
        }
        const int length = MultiByteToWideChar(932, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (length <= 0) return {};
        wstring wide(length, L'\0');
        MultiByteToWideChar(932, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
        string utf8(WideCharToMultiByte(CP_UTF8, 0, wide.data(), length, nullptr, 0, nullptr, nullptr), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), length, utf8.data(), static_cast<int>(utf8.size()), nullptr, nullptr);
        return utf8;
    }

    template<class T>
    vector<T> ReadFbxShapeArray(string_view block, string_view field)
    {
        const auto property = block.find(field);
        if (property == string_view::npos) return {};
        const auto start = block.find("a:", property + field.size());
        if (start == string_view::npos) return {};
        const auto end = block.find('}', start + 2);
        if (end == string_view::npos) return {};
        vector<T> values;
        const char* cursor = block.data() + start + 2;
        const char* limit = block.data() + end;
        while (cursor < limit)
        {
            if (*cursor == ',' || isspace(static_cast<unsigned char>(*cursor)))
            {
                ++cursor;
                continue;
            }
            T value{};
            const auto parsed = from_chars(cursor, limit, value);
            if (parsed.ec != errc{}) return {};
            values.push_back(value);
            cursor = parsed.ptr;
        }
        return values;
    }

    string_view MorphTrackAlias(string_view target)
    {
        static constexpr pair<string_view, string_view> aliases[] = {
            {"aa", "あ"}, {"ih", "い"}, {"ou", "う"}, {"ee", "え"}, {"oh", "お"},
            {"mouth_a", "あ"}, {"mouth_i", "い"}, {"mouth_u", "う"}, {"mouth_e", "え"}, {"mouth_o", "お"},
            {"mouth_a2", "あ２"}, {"mouth_wa", "ワ"}, {"mouth_wa2", "ワ２"},
            {"blink", "まばたき"}, {"eye_blink", "まばたき"}, {"eye_smile", "笑い"}, {"eyes_smile", "笑い"},
            {"blinkLeft", "ウィンク"}, {"eye_winkL", "ウィンク"},
            {"blinkRight", "ウィンク右"}, {"eye_winkR", "ウィンク右"},
            {"eye_winkL2", "ウィンク２"}, {"eye_winkR2", "ウィンク２右"},
            {"eyebrow_trouble", "困る"}, {"eyeblow_trouble", "困る"}, {"eyeblow_smile", "にこり"},
            {"eyeblow_angry", "怒り"}, {"eyeblow_up", "上"}, {"eyeblow_down", "下"},
            {"eye_surprised", "びっくり"}, {"face_cheek", "照れ"}, {"sother_shy", "照れ"},
        };
        for (const auto& [name, track] : aliases)
        {
            if (target == name) return track;
        }
        return target;
    }
}

void AnimationModelResource::LoadImportedMorphs(const filesystem::path& path)
{
    if (m_IsPmxModel) return;
    m_ImportedMorphs.resize(m_MorphSourceMeshes.size());
    auto extension = path.extension().string();
    for (char& ch : extension) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    Json::Value glb;
    if (extension == ".vrm" || extension == ".glb") VmdRetarget::ReadGlbJson(path, glb);

    for (size_t slot = 0; slot < m_MorphSourceMeshes.size(); ++slot)
    {
        const auto& mesh = *m_AiScene->mMeshes[m_MorphSourceMeshes[slot]];
        const Json::Value* names = nullptr;
        for (const auto& source : std::as_const(glb)["meshes"])
        {
            if (source["name"].isString() && source["name"].asString() == mesh.mName.C_Str())
            {
                names = &source["extras"]["targetNames"];
                break;
            }
        }
        for (unsigned index = 0; index < mesh.mNumAnimMeshes; ++index)
        {
            const auto& shape = *mesh.mAnimMeshes[index];
            if (!shape.mVertices || shape.mNumVertices != mesh.mNumVertices) continue;
            ImportedMorph morph;
            morph.Name = names && names->isArray() && index < names->size() && (*names)[index].isString()
                             ? (*names)[index].asString() : MorphUtf8(shape.mName.C_Str());
            if (morph.Name.empty()) continue;
            for (unsigned vertex = 0; vertex < mesh.mNumVertices; ++vertex)
            {
                const auto delta = shape.mVertices[vertex] - mesh.mVertices[vertex];
                if (delta.SquareLength() > 1e-14f) morph.PositionOffsets.emplace_back(vertex, delta);
            }
            if (!morph.PositionOffsets.empty()) m_ImportedMorphs[slot].push_back(move(morph));
        }
    }

    // Alicia FBX 7.1 stores shapes inline in the face geometry. Assimp only imports
    // connected BlendShape deformers, so recover these deltas from its source positions.
    if (extension != ".fbx" || path.parent_path().filename() != L"Alicia") return;
    ifstream input(path, ios::binary | ios::ate);
    if (!input || input.tellg() <= 0 || input.tellg() > 32 * 1024 * 1024) return;
    string bytes(static_cast<size_t>(input.tellg()), '\0');
    input.seekg(0);
    if (!input.read(bytes.data(), bytes.size()) || !bytes.starts_with("; FBX")) return;
    const string_view file(bytes);
    for (size_t start = file.find("    Geometry:"); start != string_view::npos;)
    {
        const size_t next = file.find("    Geometry:", start + 13);
        const auto geometry = file.substr(start, next == string_view::npos ? next : next - start);
        start = next;
        size_t shapeStart = geometry.find("        Shape: \"");
        if (shapeStart == string_view::npos || geometry.find("        Shape: \"", shapeStart + 16) == string_view::npos)
            continue;
        const auto base = ReadFbxShapeArray<float>(geometry.substr(0, shapeStart), "Vertices:");
        if (base.empty() || base.size() % 3 != 0) continue;
        const auto key = [](float x, float y, float z) {
            return array<int64_t, 3>{llround(x * 10000.0), llround(y * 10000.0), llround(z * 10000.0)};
        };
        map<array<int64_t, 3>, vector<unsigned>> controlPoints;
        for (unsigned vertex = 0; vertex < base.size() / 3; ++vertex)
        {
            const auto offset = vertex * 3;
            controlPoints[key(base[offset], base[offset + 1], base[offset + 2])].push_back(vertex);
        }
        while (shapeStart != string_view::npos)
        {
            const size_t nextShape = geometry.find("        Shape: \"", shapeStart + 16);
            const auto shape = geometry.substr(shapeStart, nextShape == string_view::npos ? nextShape : nextShape - shapeStart);
            shapeStart = nextShape;
            const auto quote = shape.find('"');
            const auto close = shape.find('"', quote + 1);
            if (close == string_view::npos) continue;
            const string name = MorphUtf8(shape.substr(quote + 1, close - quote - 1));
            const auto indices = ReadFbxShapeArray<unsigned>(shape, "Indexes:");
            const auto deltas = ReadFbxShapeArray<float>(shape, "Vertices:");
            if (name.empty() || indices.empty() || deltas.size() != indices.size() * 3) continue;
            unordered_map<unsigned, aiVector3D> offsets;
            for (size_t index = 0; index < indices.size(); ++index)
            {
                offsets.emplace(indices[index], aiVector3D(deltas[index * 3], deltas[index * 3 + 1],
                                                            deltas[index * 3 + 2] * (m_SourceConvert ? -1 : 1)));
            }
            for (size_t slot = 0; slot < m_MorphSourceMeshes.size(); ++slot)
            {
                const auto& mesh = *m_AiScene->mMeshes[m_MorphSourceMeshes[slot]];
                if (string_view(mesh.mName.C_Str()) != "face" || mesh.mNumAnimMeshes != 0) continue;
                ImportedMorph morph;
                morph.Name = name;
                for (unsigned vertex = 0; vertex < mesh.mNumVertices; ++vertex)
                {
                    const auto& position = mesh.mVertices[vertex];
                    const auto match = controlPoints.find(key(position.x, position.y,
                                                             m_SourceConvert ? -position.z : position.z));
                    if (match == controlPoints.end()) continue;
                    for (unsigned point : match->second)
                    {
                        if (const auto delta = offsets.find(point); delta != offsets.end())
                        {
                            morph.PositionOffsets.emplace_back(vertex, delta->second);
                            break;
                        }
                    }
                }
                if (!morph.PositionOffsets.empty()) m_ImportedMorphs[slot].push_back(move(morph));
            }
        }
    }
}

void AnimationModelResource::ApplyImportedVmdMorphs(const vector<pair<const VmdAnimation*, float>>& layers)
{
    bool anyActive = false;
    bool synchronized = false;
    for (size_t slot = 0; slot < m_ImportedMorphs.size(); ++slot)
    {
        const auto& morphs = m_ImportedMorphs[slot];
        if (morphs.empty()) continue;
        vector<pair<const ImportedMorph*, float>> active;
        for (const auto& morph : morphs)
        {
            const string alias(MorphTrackAlias(morph.Name));
            for (auto layer = layers.rbegin(); layer != layers.rend(); ++layer)
            {
                if (!layer->first) continue;
                const auto& tracks = layer->first->MorphTracks;
                auto track = tracks.find(morph.Name);
                if (track == tracks.end()) track = tracks.find(alias);
                if (track == tracks.end()) continue;
                const float weight = SampleVmdMorphTrack(&track->second, layer->second);
                if (std::isfinite(weight) && fabsf(weight) > 1e-5f) active.emplace_back(&morph, weight);
                break;
            }
        }
        if (active.empty() && !m_HasImportedVmdMorphs) continue;
        auto& buffer = m_Meshes[slot].InputVertexBuffer;
        if (!buffer) continue;
        if (!synchronized)
        {
            if (!GraphicsDevice::WaitForGpuIdle()) return;
            synchronized = true;
        }
        auto& vertices = m_GpuSkinVertices[slot];
        vertices = m_BaseGpuSkinVertices[slot];
        for (const auto& [morph, weight] : active)
        {
            for (const auto& [index, offset] : morph->PositionOffsets)
            {
                auto& position = vertices[index].Position;
                position.x += offset.x * weight;
                position.y += offset.y * weight;
                position.z += offset.z * weight;
            }
        }
        void* destination = nullptr;
        const CD3DX12_RANGE readRange(0, 0);
        if (SUCCEEDED(buffer->Map(0, &readRange, &destination)))
        {
            memcpy(destination, vertices.data(), vertices.size() * sizeof(GpuSkinVertex));
            buffer->Unmap(0, nullptr);
            ++m_SkinningVersion;
        }
        anyActive |= !active.empty();
    }
    m_HasImportedVmdMorphs = anyActive;
}
