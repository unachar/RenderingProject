Texture2DArray<float> StationaryDepth : register(t0);
cbuffer ShadowCacheLayer : register(b0) { uint Layer; };

float4 VSMain(uint vertex : SV_VertexID) : SV_Position
{
    float2 uv = float2((vertex << 1) & 2, vertex & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float PSMain(float4 position : SV_Position) : SV_Depth
{
    return StationaryDepth.Load(int4(int2(position.xy), Layer, 0));
}
