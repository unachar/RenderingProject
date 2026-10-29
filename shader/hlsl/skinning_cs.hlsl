#include "SkinningNormal.hlsli"


struct GpuSkinVertex
{
    float3 Position;
    float3 Normal;
    float2 TexCoord;
    float4 Diffuse;
    int4 BoneIndices;
    float4 BoneWeights;
    int DeformType;
    int3 DeformPadding;
    float4 SdefC;
    float4 SdefR0;
    float4 SdefR1;
};

struct SkinnedVertex
{
    float3 Position;
    float3 Normal;
    float2 TexCoord;
    float4 Diffuse;
};

StructuredBuffer<GpuSkinVertex> g_InputVertices : register(t0);
StructuredBuffer<float4x4> g_BoneMatrices : register(t1);
RWStructuredBuffer<SkinnedVertex> g_OutputVertices : register(u0);

#include "DualQuaternionSkinning.hlsli"

[numthreads(128, 1, 1)] void main(uint3 dispatchId : SV_DispatchThreadID) {
    const uint vertexIndex = dispatchId.x;

    uint vertexCount;
    uint vertexStride;
    g_InputVertices.GetDimensions(vertexCount, vertexStride);
    if (vertexIndex >= vertexCount)
    {
        return;
    }

    uint boneCount;
    uint boneStride;
    g_BoneMatrices.GetDimensions(boneCount, boneStride);

    GpuSkinVertex input = g_InputVertices[vertexIndex];

    bool validX = isfinite(input.BoneWeights.x) && input.BoneWeights.x > 0.0f &&
                  input.BoneIndices.x >= 0 && (uint)input.BoneIndices.x < boneCount;
    bool validY = isfinite(input.BoneWeights.y) && input.BoneWeights.y > 0.0f &&
                  input.BoneIndices.y >= 0 && (uint)input.BoneIndices.y < boneCount;
    bool validZ = isfinite(input.BoneWeights.z) && input.BoneWeights.z > 0.0f &&
                  input.BoneIndices.z >= 0 && (uint)input.BoneIndices.z < boneCount;
    bool validW = isfinite(input.BoneWeights.w) && input.BoneWeights.w > 0.0f &&
                  input.BoneIndices.w >= 0 && (uint)input.BoneIndices.w < boneCount;

    float4 weights = float4(validX ? input.BoneWeights.x : 0.0f, validY ? input.BoneWeights.y : 0.0f,
                            validZ ? input.BoneWeights.z : 0.0f, validW ? input.BoneWeights.w : 0.0f);

    const float totalWeight = weights.x + weights.y + weights.z + weights.w;

    if (!isfinite(totalWeight) || totalWeight <= 0.0001f)
    {
        SkinnedVertex output;
        output.Position = input.Position;
        output.Normal = NormalizeSkinnedNormal(input.Normal, input.Normal);
        output.TexCoord = input.TexCoord;
        output.Diffuse = input.Diffuse;
        g_OutputVertices[vertexIndex] = output;
        return;
    }

    weights /= totalWeight;

    if (input.DeformType == 3 && validX && validY)
    {
        const float sdefTotal = max(weights.x + weights.y, 0.00001f);
        const float w0 = weights.x / sdefTotal;
        const float w1 = weights.y / sdefTotal;
        const float3 sdefC = input.SdefC.xyz;
        const float3 sdefR0 = input.SdefR0.xyz;
        const float3 sdefR1 = input.SdefR1.xyz;
        const float3 rw = sdefR0 * w0 + sdefR1 * w1;
        const float3 r0 = sdefC + sdefR0 - rw;
        const float3 r1 = sdefC + sdefR1 - rw;
        const float3 cr0 = (sdefC + r0) * 0.5f;
        const float3 cr1 = (sdefC + r1) * 0.5f;

        const float4x4 m0 = g_BoneMatrices[(uint)input.BoneIndices.x];
        const float4x4 m1 = g_BoneMatrices[(uint)input.BoneIndices.y];
        const float4 q0 = QuaternionFromMatrix((float3x3)m0);
        const float4 q1 = QuaternionFromMatrix((float3x3)m1);
        const float3x3 rotation = MatrixFromQuaternion(QuaternionSlerp(q0, q1, w1));

        const float3 sdefPos = mul(input.Position - sdefC, rotation) + mul(float4(cr0, 1.0f), m0).xyz * w0 +
                               mul(float4(cr1, 1.0f), m1).xyz * w1;
        const float3 sdefNor = mul(input.Normal, rotation);

        const bool invalidSdefPosition = any(sdefPos != sdefPos);
        SkinnedVertex output;
        output.Position = invalidSdefPosition ? input.Position : sdefPos;
        output.Normal = NormalizeSkinnedNormal(input.Normal, sdefNor);
        output.TexCoord = input.TexCoord;
        output.Diffuse = input.Diffuse;
        g_OutputVertices[vertexIndex] = output;
        return;
    }

    // All model importers share this path. Preserve authored SDEF above; use DQS
    // for BDEF/QDEF and ordinary FBX/VRM vertices when every influence is rigid.
    SkinningDualQuaternion blended = ZeroDualQuaternion();
    float4 reference = float4(0.0f, 0.0f, 0.0f, 1.0f);
    bool hasReference = false;
    bool rigid = true;
    [unroll]
    for (int influence = 0; influence < 4; ++influence)
    {
        if (weights[influence] <= 0.0f) continue;
        const float4x4 boneMatrix = g_BoneMatrices[(uint)input.BoneIndices[influence]];
        if (!IsRigidSkinningMatrix(boneMatrix))
        {
            rigid = false;
            break;
        }
        const SkinningDualQuaternion value = DualQuaternionFromMatrix(boneMatrix);
        if (!hasReference)
        {
            reference = value.Real;
            hasReference = true;
        }
        blended = AccumulateDualQuaternion(blended, value, weights[influence], reference);
    }
    if (rigid && hasReference && dot(blended.Real, blended.Real) > 0.00000001f)
    {
        blended = NormalizeDualQuaternion(blended);
        const float3 position = TransformDualQuaternionPosition(blended, input.Position);
        const float3 normal = RotateDualQuaternionVector(blended, input.Normal);
        if (all(isfinite(position)) && all(isfinite(normal)))
        {
            SkinnedVertex output;
            output.Position = position;
            output.Normal = NormalizeSkinnedNormal(input.Normal, normal);
            output.TexCoord = input.TexCoord;
            output.Diffuse = input.Diffuse;
            g_OutputVertices[vertexIndex] = output;
            return;
        }
    }

    // DQS cannot encode scale, shear or reflection. Keep the complete LBS result
    // for this vertex instead of mixing incompatible transformations.
    const float4 pos = float4(input.Position, 1.0f);
    const float4 nor = float4(input.Normal, 0.0f);

    float4 skinnedPos = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float4 skinnedNor = float4(0.0f, 0.0f, 0.0f, 0.0f);

    if (validX)
    {
        const float4x4 m = g_BoneMatrices[(uint)input.BoneIndices.x];
        skinnedPos += weights.x * mul(pos, m);
        skinnedNor += weights.x * mul(nor, m);
    }
    if (validY)
    {
        const float4x4 m = g_BoneMatrices[(uint)input.BoneIndices.y];
        skinnedPos += weights.y * mul(pos, m);
        skinnedNor += weights.y * mul(nor, m);
    }
    if (validZ)
    {
        const float4x4 m = g_BoneMatrices[(uint)input.BoneIndices.z];
        skinnedPos += weights.z * mul(pos, m);
        skinnedNor += weights.z * mul(nor, m);
    }
    if (validW)
    {
        const float4x4 m = g_BoneMatrices[(uint)input.BoneIndices.w];
        skinnedPos += weights.w * mul(pos, m);
        skinnedNor += weights.w * mul(nor, m);
    }

    const bool invalidSkinnedPosition =
        (skinnedPos.w != skinnedPos.w) || any(skinnedPos.xyz != skinnedPos.xyz) || abs(skinnedPos.w) <= 0.0001f;
    if (invalidSkinnedPosition)
    {
        SkinnedVertex output;
        output.Position = input.Position;
        output.Normal = NormalizeSkinnedNormal(input.Normal, input.Normal);
        output.TexCoord = input.TexCoord;
        output.Diffuse = input.Diffuse;
        g_OutputVertices[vertexIndex] = output;
        return;
    }

    skinnedPos.xyz /= skinnedPos.w;

    SkinnedVertex output;
    output.Position = skinnedPos.xyz;
    output.Normal = NormalizeSkinnedNormal(input.Normal, skinnedNor.xyz);
    output.TexCoord = input.TexCoord;
    output.Diffuse = input.Diffuse;
    g_OutputVertices[vertexIndex] = output;
}
