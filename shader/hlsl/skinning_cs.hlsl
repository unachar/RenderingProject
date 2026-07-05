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

    SkinnedVertex output;
    output.Position = skinnedPos.xyz;
    const float normalLenSq = dot(skinnedNor.xyz, skinnedNor.xyz);
    output.Normal = (normalLenSq > 0.000001f) ? normalize(skinnedNor.xyz) : float3(0.0f, 1.0f, 0.0f);
    output.TexCoord = input.TexCoord;
    output.Diffuse = input.Diffuse;
    g_OutputVertices[vertexIndex] = output;
}
