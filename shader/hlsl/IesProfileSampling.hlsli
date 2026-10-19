#ifndef IES_PROFILE_SAMPLING_INCLUDED
#define IES_PROFILE_SAMPLING_INCLUDED

// IES_TEXTURE_HEAP はエンジンの線形テクスチャヒープを指す。
float SampleIesProfile(float4 profile, float3 emitterDirection, float3 fromLight)
{
    uint encodedIndex = (uint)round(profile.x);
    if (encodedIndex == 0u || encodedIndex > 512u)
    {
        return 1.0f;
    }
    float3 forward = float3(0, -1, 0);
    if (dot(emitterDirection, emitterDirection) > 1e-8f)
    {
        forward = normalize(emitterDirection);
    }
    float3 direction = forward;
    if (dot(fromLight, fromLight) > 1e-8f)
    {
        direction = normalize(fromLight);
    }
    float3 reference = abs(forward.y) < 0.999f ? float3(0, 1, 0) : float3(0, 0, 1);
    float3 right = normalize(cross(reference, forward));
    float3 up = cross(forward, right);
    const float pi = 3.14159265358979323846f;
    float2 tangentDirection = float2(dot(direction, right), dot(direction, up));
    float azimuth = 0.0f;
    if (dot(tangentDirection, tangentDirection) > 1e-10f)
    {
        azimuth = atan2(tangentDirection.y, tangentDirection.x) / (2.0f * pi);
    }
    float polar = acos(clamp(dot(direction, forward), -1.0f, 1.0f)) / pi;

    uint index = NonUniformResourceIndex(encodedIndex - 1u);
    uint width, height;
    IES_TEXTURE_HEAP[index].GetDimensions(width, height);
    float2 pixel = float2(frac(azimuth - profile.y), polar) * float2(width, height) - 0.5f;
    int2 first = int2(floor(pixel));
    float2 weight = frac(pixel);
    // 方位角の継ぎ目を折り返し、極方向はクランプする。サンプラーの状態に依存しない。
    uint x0 = (uint)(first.x + (int)width) % width;
    uint x1 = (x0 + 1u) % width;
    int y0 = clamp(first.y, 0, (int)height - 1);
    int y1 = clamp(first.y + 1, 0, (int)height - 1);
    float a = IES_TEXTURE_HEAP[index].Load(int3(x0, y0, 0)).r;
    float b = IES_TEXTURE_HEAP[index].Load(int3(x1, y0, 0)).r;
    float c = IES_TEXTURE_HEAP[index].Load(int3(x0, y1, 0)).r;
    float d = IES_TEXTURE_HEAP[index].Load(int3(x1, y1, 0)).r;
    return saturate(lerp(lerp(a, b, weight.x), lerp(c, d, weight.x), weight.y));
}
#endif
