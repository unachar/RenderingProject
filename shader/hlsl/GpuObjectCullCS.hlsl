// エンティティ単位の GPU カリング（GpuObjectCulling が実行時にコンパイル）。
// CPU の個別描画経路（スプライト・メッシュ・影・Velocity）は、この結果を SetPredication で参照し、
// 見えない物の描画を GPU 側で破棄する。CPU は可視判定を行わない。
struct ObjectRecord
{
    row_major float4x4 World;
    float4 LocalCenter;  // w: asfloat(フラグ) bit0 = カメラ用境界あり, bit1 = 影を落とす
    float4 LocalExtents;
    float4 ShadowSphere; // ワールド空間の境界球（影パス用）
};

StructuredBuffer<ObjectRecord> Objects : register(t0);
RWStructuredBuffer<uint2> Predicates : register(u0); // x != 0 で描画

cbuffer ObjectCullConstants : register(b0)
{
    float4 Planes[6];
    float4 CameraPositionPixelScale; // xyz: カメラ位置, w: 画面サイズ係数
    uint ObjectCount;
    uint OutputOffset; // uint2 単位
    uint Mode;         // 0 = カメラ, 1 = 影
    uint Flags;        // bit0 = 視錐台, bit1 = 画面サイズ
    float MinPixels;
    float MaxDistance;
    float2 Padding;
};

uint TestCamera(ObjectRecord record)
{
    uint flags = asuint(record.LocalCenter.w);
    if ((flags & 1u) == 0u)
    {
        return 1u; // 境界なし：常に描く（以前の CPU 判定と同じ）
    }
    float3 center = mul(float4(record.LocalCenter.xyz, 1.0f), record.World).xyz;
    float3 e = record.LocalExtents.xyz;
    float3 extents = abs(record.World[0].xyz) * e.x + abs(record.World[1].xyz) * e.y + abs(record.World[2].xyz) * e.z;
    if ((Flags & 1u) != 0u)
    {
        [unroll] for (uint i = 0; i < 6; ++i)
        {
            float distance = dot(Planes[i].xyz, center) + Planes[i].w;
            float projected = dot(abs(Planes[i].xyz), extents);
            if (distance < -projected)
            {
                return 0u;
            }
        }
    }
    if ((Flags & 2u) != 0u || MaxDistance > 0.0f)
    {
        float radius = length(extents);
        float distance = length(center - CameraPositionPixelScale.xyz);
        if (distance > radius)
        {
            if (MaxDistance > 0.0f && distance - radius > MaxDistance)
            {
                return 0u;
            }
            if ((Flags & 2u) != 0u && radius * CameraPositionPixelScale.w / distance < MinPixels)
            {
                return 0u;
            }
        }
    }
    return 1u;
}

uint TestShadow(ObjectRecord record)
{
    uint flags = asuint(record.LocalCenter.w);
    if ((flags & 2u) == 0u)
    {
        return 0u; // 影を落とさない物（以前の IsShadowCasterVisible と同じ）
    }
    float4 center = float4(record.ShadowSphere.xyz, 1.0f);
    [unroll] for (uint i = 0; i < 6; ++i)
    {
        if (dot(Planes[i], center) < -record.ShadowSphere.w)
        {
            return 0u;
        }
    }
    return 1u;
}

[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= ObjectCount)
    {
        return;
    }
    ObjectRecord record = Objects[id.x];
    uint visible = Mode == 0u ? TestCamera(record) : TestShadow(record);
    Predicates[OutputOffset + id.x] = uint2(visible, 0u);
}
