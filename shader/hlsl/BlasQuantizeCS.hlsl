// =============================================================================================
// BlasQuantizeCS.hlsl - DXR BLAS 入力頂点を R16G16B16A16_SNORM へ量子化する。
// 1 グループ（256 スレッド）が 1 頂点ソースを担当し、
//   1) 位置の AABB をグループ共有メモリでリダクション
//   2) AABB 中心・半径で正規化して 16bit SNORM に書き出し
//   3) 復元用の 3x4 行列（行優先）を先頭 48 バイトに書き出す
// BLAS 構築時は Triangles.Transform3x4 にこの行列を指定するため、BLAS 空間は元のオブジェクト空間の
// まま（TLAS のインスタンス行列・ヒット位置・法線計算は変更不要）。
// 出力レイアウト: [0..47] float3x4 / [48..63] float4(最小値, 最大値の x) 予約 / [64..] 頂点 8 バイト
// =============================================================================================
cbuffer QuantizeConstants : register(b0)
{
    uint VertexCount;
    uint Stride;
    uint Reserved0;
    uint Reserved1;
};

ByteAddressBuffer SourceVertices : register(t0);
RWByteAddressBuffer PackedVertices : register(u0);

static const uint kThreads = 256;
static const uint kPositionOffset = 64;

groupshared float3 s_Min[kThreads];
groupshared float3 s_Max[kThreads];

bool IsFinitePosition(float3 p)
{
    return all(abs(p) < 3.0e38f); // NaN は比較が偽になるため除外される
}

int Snorm16(float value)
{
    return (int)round(clamp(value, -1.0f, 1.0f) * 32767.0f);
}

[numthreads(kThreads, 1, 1)] void main(uint groupIndex : SV_GroupIndex)
{
    float3 lo = 3.0e38f;
    float3 hi = -3.0e38f;
    for (uint v = groupIndex; v < VertexCount; v += kThreads)
    {
        float3 p = asfloat(SourceVertices.Load3(v * Stride));
        if (IsFinitePosition(p))
        {
            lo = min(lo, p);
            hi = max(hi, p);
        }
    }
    s_Min[groupIndex] = lo;
    s_Max[groupIndex] = hi;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint step = kThreads / 2; step > 0; step >>= 1)
    {
        if (groupIndex < step)
        {
            s_Min[groupIndex] = min(s_Min[groupIndex], s_Min[groupIndex + step]);
            s_Max[groupIndex] = max(s_Max[groupIndex], s_Max[groupIndex + step]);
        }
        GroupMemoryBarrierWithGroupSync();
    }
    lo = s_Min[0];
    hi = s_Max[0];
    if (any(lo > hi))
    {
        lo = 0.0f;
        hi = 0.0f;
    }
    const float3 center = (lo + hi) * 0.5f;
    // ゼロ幅の軸でも 0 除算しない。
    const float3 halfExtent = max((hi - lo) * 0.5f, 1.0e-6f);
    const float3 inverse = 1.0f / halfExtent;
    if (groupIndex == 0)
    {
        // 行優先 3x4: x' = halfExtent.x * q.x + center.x（DXR の Transform3x4 と同じ並び）
        PackedVertices.Store4(0, asuint(float4(halfExtent.x, 0.0f, 0.0f, center.x)));
        PackedVertices.Store4(16, asuint(float4(0.0f, halfExtent.y, 0.0f, center.y)));
        PackedVertices.Store4(32, asuint(float4(0.0f, 0.0f, halfExtent.z, center.z)));
        PackedVertices.Store4(48, asuint(float4(lo.x, lo.y, lo.z, hi.x)));
    }
    for (uint v = groupIndex; v < VertexCount; v += kThreads)
    {
        float3 p = asfloat(SourceVertices.Load3(v * Stride));
        // 非有限値は中心へ寄せる（元の BLAS では非アクティブ三角形になる頂点）。
        p = IsFinitePosition(p) ? p : center;
        const int3 q = int3(Snorm16((p.x - center.x) * inverse.x), Snorm16((p.y - center.y) * inverse.y),
                            Snorm16((p.z - center.z) * inverse.z));
        PackedVertices.Store2(kPositionOffset + v * 8,
                              uint2((uint(q.x) & 0xffffu) | (uint(q.y) << 16), uint(q.z) & 0xffffu));
    }
}
