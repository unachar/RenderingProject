Texture2D<float4> NoisyRadiance : register(t0);
Texture2D<float> SceneDepth : register(t1);
Texture2D<float4> SceneNormal : register(t2);
StructuredBuffer<float4> SharedInput : register(t3);
RWStructuredBuffer<float4> SharedOutput : register(u0);
RWTexture2D<float4> Result : register(u1);

cbuffer OptixConstants : register(b0)
{
    float4x4 WorldToView;
    uint4 ExtentFlags; // width, height, NRD YCoCg input, preserve reflection hit distance
    float4 Parameters; // output intensity, reserved
};

[numthreads(8, 8, 1)]
void Pack(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= ExtentFlags.xy))
    {
        return;
    }
    uint index = id.x + id.y * ExtentFlags.x;
    uint plane = ExtentFlags.x * ExtentFlags.y;
    bool surface = SceneDepth.Load(int3(id.xy, 0)) < 0.99999f;
    float3 radiance = NoisyRadiance.Load(int3(id.xy, 0)).rgb;
    if (ExtentFlags.z != 0)
    {
        float y = radiance.x;
        float co = radiance.y;
        float cg = radiance.z;
        radiance = float3(y + co - cg, y + cg, y - co - cg);
    }
    if (!all(isfinite(radiance)) || !surface)
    {
        radiance = 0;
    }
    float3 normal = SceneNormal.Load(int3(id.xy, 0)).xyz * 2 - 1;
    if (!surface || dot(normal, normal) < 0.000001f)
    {
        normal = float3(0, 0, -1);
    }
    normal = mul(float4(normalize(normal), 0), WorldToView).xyz;
    normal.z = -normal.z; // D3D left-handed view to the OptiX camera-space guide.
    SharedOutput[index] = float4(max(radiance, 0), 1);
    SharedOutput[plane + index] = float4(normal, 0);
    // These signals are incident radiance/irradiance, before receiver material
    // modulation. A neutral albedo avoids imprinting receiver textures into GI.
    float albedo = surface ? 1.0f : 0.0f;
    SharedOutput[2 * plane + index] = float4(albedo, albedo, albedo, 1);
}

[numthreads(8, 8, 1)]
void Unpack(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= ExtentFlags.xy))
    {
        return;
    }
    uint plane = ExtentFlags.x * ExtentFlags.y;
    uint index = id.x + id.y * ExtentFlags.x;
    float3 radiance = SharedInput[3 * plane + index].rgb;
    if (SceneDepth.Load(int3(id.xy, 0)) >= 0.99999f || !all(isfinite(radiance)))
    {
        radiance = 0;
    }
    float alpha = 1;
    if (ExtentFlags.w != 0)
    {
        alpha = Result[id.xy].a;
    }
    Result[id.xy] = float4(min(max(radiance * Parameters.x, 0), 65504), alpha);
}
