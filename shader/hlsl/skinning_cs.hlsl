// ==========================================================
// skinning_cs.hlsl
// コンピュートシェーダによるGPUスキニング
// 2026-07-05: zero-weight / invalid-index safe version
// ==========================================================

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

float4 NormalizeQuaternion(float4 q)
{
    return q * rsqrt(max(dot(q, q), 0.00000001f));
}

float4 QuaternionFromMatrix(float3x3 m)
{
    float4 q;
    const float trace = m[0][0] + m[1][1] + m[2][2];
    if (trace > 0.0f)
    {
        const float s = sqrt(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m[1][2] - m[2][1]) / s;
        q.y = (m[2][0] - m[0][2]) / s;
        q.z = (m[0][1] - m[1][0]) / s;
    }
    else if (m[0][0] > m[1][1] && m[0][0] > m[2][2])
    {
        const float s = sqrt(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
        q.w = (m[1][2] - m[2][1]) / s;
        q.x = 0.25f * s;
        q.y = (m[0][1] + m[1][0]) / s;
        q.z = (m[2][0] + m[0][2]) / s;
    }
    else if (m[1][1] > m[2][2])
    {
        const float s = sqrt(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
        q.w = (m[2][0] - m[0][2]) / s;
        q.x = (m[0][1] + m[1][0]) / s;
        q.y = 0.25f * s;
        q.z = (m[1][2] + m[2][1]) / s;
    }
    else
    {
        const float s = sqrt(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
        q.w = (m[0][1] - m[1][0]) / s;
        q.x = (m[2][0] + m[0][2]) / s;
        q.y = (m[1][2] + m[2][1]) / s;
        q.z = 0.25f * s;
    }
    return NormalizeQuaternion(q);
}

float4 QuaternionSlerp(float4 a, float4 b, float t)
{
    float cosTheta = dot(a, b);
    if (cosTheta < 0.0f)
    {
        b = -b;
        cosTheta = -cosTheta;
    }

    float4 result = NormalizeQuaternion(lerp(a, b, t));
    if (cosTheta <= 0.9995f)
    {
        const float theta = acos(clamp(cosTheta, -1.0f, 1.0f));
        const float sinTheta = max(sin(theta), 0.00001f);
        result = (sin((1.0f - t) * theta) * a + sin(t * theta) * b) / sinTheta;
    }
    return result;
}

float3x3 MatrixFromQuaternion(float4 q)
{
    q = NormalizeQuaternion(q);
    const float x2 = q.x + q.x;
    const float y2 = q.y + q.y;
    const float z2 = q.z + q.z;
    const float xx = q.x * x2;
    const float yy = q.y * y2;
    const float zz = q.z * z2;
    const float xy = q.x * y2;
    const float xz = q.x * z2;
    const float yz = q.y * z2;
    const float wx = q.w * x2;
    const float wy = q.w * y2;
    const float wz = q.w * z2;

    return float3x3(
        1.0f - yy - zz, xy + wz, xz - wy,
        xy - wz, 1.0f - xx - zz, yz + wx,
        xz + wy, yz - wx, 1.0f - xx - yy);
}

[numthreads(128, 1, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
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

    // 範囲外ボーンや負のボーン番号は読まない。
    // C++側でほぼ起きない前提だが、ここで守っておくとトゲ状破綻の最後の保険になる。
    bool validX = input.BoneWeights.x > 0.0f && input.BoneIndices.x >= 0 && (uint) input.BoneIndices.x < boneCount;
    bool validY = input.BoneWeights.y > 0.0f && input.BoneIndices.y >= 0 && (uint) input.BoneIndices.y < boneCount;
    bool validZ = input.BoneWeights.z > 0.0f && input.BoneIndices.z >= 0 && (uint) input.BoneIndices.z < boneCount;
    bool validW = input.BoneWeights.w > 0.0f && input.BoneIndices.w >= 0 && (uint) input.BoneIndices.w < boneCount;

    float4 weights = float4(
        validX ? input.BoneWeights.x : 0.0f,
        validY ? input.BoneWeights.y : 0.0f,
        validZ ? input.BoneWeights.z : 0.0f,
        validW ? input.BoneWeights.w : 0.0f);

    const float totalWeight = weights.x + weights.y + weights.z + weights.w;

    // C++側で未ウェイト頂点は近傍頂点のウェイトで補修する。
    // それでも残った場合は、変換せず元頂点を出す。
    if (totalWeight <= 0.0001f)
    {
        SkinnedVertex output;
        output.Position = input.Position;
        const float normalLenSq = dot(input.Normal, input.Normal);
        output.Normal = (normalLenSq > 0.000001f) ? normalize(input.Normal) : float3(0.0f, 1.0f, 0.0f);
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

        const float4x4 m0 = g_BoneMatrices[(uint) input.BoneIndices.x];
        const float4x4 m1 = g_BoneMatrices[(uint) input.BoneIndices.y];
        const float4 q0 = QuaternionFromMatrix((float3x3) m0);
        const float4 q1 = QuaternionFromMatrix((float3x3) m1);
        const float3x3 rotation = MatrixFromQuaternion(QuaternionSlerp(q0, q1, w1));

        const float3 sdefPos =
            mul(input.Position - sdefC, rotation) +
            mul(float4(cr0, 1.0f), m0).xyz * w0 +
            mul(float4(cr1, 1.0f), m1).xyz * w1;
        const float3 sdefNor = mul(input.Normal, rotation);

        const bool invalidSdefPosition = any(sdefPos != sdefPos);
        SkinnedVertex output;
        output.Position = invalidSdefPosition ? input.Position : sdefPos;
        const float normalLenSq = dot(sdefNor, sdefNor);
        output.Normal = (normalLenSq > 0.000001f) ? normalize(sdefNor) : float3(0.0f, 1.0f, 0.0f);
        output.TexCoord = input.TexCoord;
        output.Diffuse = input.Diffuse;
        g_OutputVertices[vertexIndex] = output;
        return;
    }

    const float4 pos = float4(input.Position, 1.0f);
    const float4 nor = float4(input.Normal, 0.0f);

    float4 skinnedPos = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float4 skinnedNor = float4(0.0f, 0.0f, 0.0f, 0.0f);

    if (validX)
    {
        const float4x4 m = g_BoneMatrices[(uint) input.BoneIndices.x];
        skinnedPos += weights.x * mul(pos, m);
        skinnedNor += weights.x * mul(nor, m);
    }
    if (validY)
    {
        const float4x4 m = g_BoneMatrices[(uint) input.BoneIndices.y];
        skinnedPos += weights.y * mul(pos, m);
        skinnedNor += weights.y * mul(nor, m);
    }
    if (validZ)
    {
        const float4x4 m = g_BoneMatrices[(uint) input.BoneIndices.z];
        skinnedPos += weights.z * mul(pos, m);
        skinnedNor += weights.z * mul(nor, m);
    }
    if (validW)
    {
        const float4x4 m = g_BoneMatrices[(uint) input.BoneIndices.w];
        skinnedPos += weights.w * mul(pos, m);
        skinnedNor += weights.w * mul(nor, m);
    }

    const bool invalidSkinnedPosition =
        (skinnedPos.w != skinnedPos.w) ||
        any(skinnedPos.xyz != skinnedPos.xyz) ||
        abs(skinnedPos.w) <= 0.0001f;
    if (invalidSkinnedPosition)
    {
        SkinnedVertex output;
        output.Position = input.Position;
        const float normalLenSq = dot(input.Normal, input.Normal);
        output.Normal = (normalLenSq > 0.000001f) ? normalize(input.Normal) : float3(0.0f, 1.0f, 0.0f);
        output.TexCoord = input.TexCoord;
        output.Diffuse = input.Diffuse;
        g_OutputVertices[vertexIndex] = output;
        return;
    }

    skinnedPos.xyz /= skinnedPos.w;

    SkinnedVertex output;
    output.Position = skinnedPos.xyz;
    const float normalLenSq = dot(skinnedNor.xyz, skinnedNor.xyz);
    output.Normal = (normalLenSq > 0.000001f) ? normalize(skinnedNor.xyz) : float3(0.0f, 1.0f, 0.0f);
    output.TexCoord = input.TexCoord;
    output.Diffuse = input.Diffuse;
    g_OutputVertices[vertexIndex] = output;
}
