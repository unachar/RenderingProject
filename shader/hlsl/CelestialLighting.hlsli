#ifndef CELESTIAL_LIGHTING
#define CELESTIAL_LIGHTING

// 月はワールドの +Z 方向、仰角 35 度に固定する。全パスで共有する。
float3 VirtualMoonDirection()
{
    return float3(0.0f, 0.5735764f, 0.8191520f);
}

float3 VirtualMoonIrradiance()
{
    const float3 sun = SkySunDirection.xyz;
    const float lengthSquared = dot(sun, sun);
    if (lengthSquared < 0.00000001f || SkyParameters.x <= 0.0f)
    {
        return 0.0f;
    }
    const float sunElevationSine = sun.y * rsqrt(lengthSquared);
    // 薄明で徐々に見え始め、太陽高度が -12 度で夜の明るさに達する。
    const float nightVisibility = 1.0f - smoothstep(-0.2079117f, -0.0174524f, sunElevationSine);
    return float3(0.55f, 0.65f, 0.80f) * nightVisibility;
}

// 球状の大気。浅い角度のレイでは受光点付近を密にサンプリングする。
float3 CelestialAtmosphereTransmission(float altitude, float3 direction)
{
    const float radius = 6360000.0;
    float3 origin = float3(0, radius + max(altitude, 2.0), 0);
    float projection = dot(origin, direction);
    float groundDiscriminant = projection * projection - dot(origin, origin) + radius * radius;
    if (groundDiscriminant >= 0 && -projection - sqrt(groundDiscriminant) > 0)
    {
        return 0;
    }
    float distance = -projection + sqrt(max(projection * projection - dot(origin, origin) + 6460000.0 * 6460000.0, 0));
    float2 depth = 0;
    [unroll] for (uint i = 0; i < 8; ++i)
    {
        float a = float(i) / 8.0;
        float b = float(i + 1) / 8.0;
        float height = max(length(origin + direction * (0.5 * (a * a + b * b) * distance)) - radius, 0);
        depth += exp(-height / float2(8000, 1200)) * ((b * b - a * a) * distance);
    }
    float scale = max(AtmosphereParams0.w, 0) / 0.36;
    float3 rayleigh = float3(5.8e-6, 13.5e-6, 33.1e-6) * max(AtmosphereParams0.y, 0) / 0.42 * scale;
    float mie = 21e-6 * max(AtmosphereParams0.z, 0) / 0.075 * scale;
    return exp(-rayleigh * depth.x - mie * depth.y);
}
#endif
