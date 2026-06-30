// ==========================================================
// test.slang
// コンピュートシェーダによるGPUスキニング
// ==========================================================

//入力頂点構造体（ボーンインデックス・ウェイト付き）
struct GpuSkinVertex
{
    float3 Position;
    float3 Normal;
    float2 TexCoord;
    float4 Diffuse;
    int4   BoneIndices; //ボーンインデックス（最大4本）
    float4 BoneWeights; //ボーンウェイト（最大4本）
};

//スキニング後の出力頂点
struct SkinnedVertex
{
    float3 Position;
    float3 Normal;
    float2 TexCoord;
    float4 Diffuse;
};

//入力リソース
StructuredBuffer<GpuSkinVertex> g_InputVertices : register(t0);
StructuredBuffer<float4x4>      g_BoneMatrices  : register(t1);

//出力リソース
RWStructuredBuffer<SkinnedVertex> g_OutputVertices : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    uint vertexIndex = (uint)dispatchId.x;

    //バッファ範囲外アクセスを防止
    uint vertexCount;
    uint stride;
    g_InputVertices.GetDimensions(vertexCount, stride);
    if (vertexIndex >= vertexCount)
    {
        return;
    }

    GpuSkinVertex input = g_InputVertices[vertexIndex];

    //スキニング計算（最大4ボーンのブレンド）
    float4 pos = float4(input.Position, 1.0f);
    float4 nor = float4(input.Normal, 0.0f);

    float4 skinnedPos = float4(0, 0, 0, 0);
    float4 skinnedNor = float4(0, 0, 0, 0);

    float totalWeight = input.BoneWeights.x + input.BoneWeights.y
                      + input.BoneWeights.z + input.BoneWeights.w;

    //ウェイトが0の場合はバインドポーズ（変換なし）で出力
    if (totalWeight < 0.0001f)
    {
        skinnedPos = pos;
        skinnedNor = nor;
    }
    else
    {
        skinnedPos += input.BoneWeights.x * mul(pos, g_BoneMatrices[input.BoneIndices.x]);
        skinnedPos += input.BoneWeights.y * mul(pos, g_BoneMatrices[input.BoneIndices.y]);
        skinnedPos += input.BoneWeights.z * mul(pos, g_BoneMatrices[input.BoneIndices.z]);
        skinnedPos += input.BoneWeights.w * mul(pos, g_BoneMatrices[input.BoneIndices.w]);

        //ノーマルには移動成分を含めない（w=0）
        skinnedNor += input.BoneWeights.x * mul(nor, g_BoneMatrices[input.BoneIndices.x]);
        skinnedNor += input.BoneWeights.y * mul(nor, g_BoneMatrices[input.BoneIndices.y]);
        skinnedNor += input.BoneWeights.z * mul(nor, g_BoneMatrices[input.BoneIndices.z]);
        skinnedNor += input.BoneWeights.w * mul(nor, g_BoneMatrices[input.BoneIndices.w]);
    }

    SkinnedVertex output;
    output.Position = skinnedPos.xyz;
    output.Normal   = normalize(skinnedNor.xyz);
    output.TexCoord = input.TexCoord;
    output.Diffuse  = input.Diffuse;

    g_OutputVertices[vertexIndex] = output;
}
