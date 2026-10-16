#ifndef ATMOSPHERE_LUT_COMMON_HLSLI
#define ATMOSPHERE_LUT_COMMON_HLSLI
// Shared parameterisation of the atmosphere LUTs (Hillaire 2020, "A Scalable and Production
// Ready Sky and Atmosphere Rendering Technique"). Used by AtmosphereLutCS.hlsl (builder) and
// SkyAtmosphere.hlsli (consumers). Must stay identical on both sides.

static const float kLutGroundRadius = 6360000.0f;
static const float kLutAtmosphereRadius = 6460000.0f;
static const float kLutPi = 3.14159265f;
static const uint2 kTransmittanceLutSize = uint2(256, 64);
static const uint2 kMultiScatterLutSize = uint2(32, 32);
static const uint2 kSkyViewLutSize = uint2(192, 108);

// --- Transmittance (optical depth) LUT: Bruneton's (r, mu) mapping --------------------------
float2 TransmittanceLutUv(float radius, float mu)
{
    float H = sqrt(kLutAtmosphereRadius * kLutAtmosphereRadius - kLutGroundRadius * kLutGroundRadius);
    float rho = sqrt(max(radius * radius - kLutGroundRadius * kLutGroundRadius, 0.0f));
    float discriminant = radius * radius * (mu * mu - 1.0f) + kLutAtmosphereRadius * kLutAtmosphereRadius;
    float d = max(-radius * mu + sqrt(max(discriminant, 0.0f)), 0.0f);
    float dMin = kLutAtmosphereRadius - radius;
    float dMax = rho + H;
    float xMu = (d - dMin) / max(dMax - dMin, 1.0f);
    float xR = rho / H;
    return float2(xMu, xR);
}

void TransmittanceLutParameters(float2 uv, out float radius, out float mu)
{
    float H = sqrt(kLutAtmosphereRadius * kLutAtmosphereRadius - kLutGroundRadius * kLutGroundRadius);
    float rho = H * uv.y;
    radius = sqrt(rho * rho + kLutGroundRadius * kLutGroundRadius);
    float dMin = kLutAtmosphereRadius - radius;
    float dMax = rho + H;
    float d = dMin + uv.x * (dMax - dMin);
    mu = d == 0.0f ? 1.0f : (H * H - rho * rho - d * d) / (2.0f * radius * d);
    mu = clamp(mu, -1.0f, 1.0f);
}

// --- Sky view LUT: (sqrt relative sun azimuth, non-linear zenith around the horizon) ----------
void SkyViewHorizon(float viewHeight, out float beta, out float zenithHorizonAngle)
{
    float vHorizon = sqrt(max(viewHeight * viewHeight - kLutGroundRadius * kLutGroundRadius, 0.0f));
    beta = acos(clamp(vHorizon / viewHeight, -1.0f, 1.0f));
    zenithHorizonAngle = kLutPi - beta;
}

float2 SkyViewLutUv(float3 direction, float3 sun, float viewHeight)
{
    float beta, zenithHorizonAngle;
    SkyViewHorizon(viewHeight, beta, zenithHorizonAngle);
    float viewZenith = acos(clamp(direction.y, -1.0f, 1.0f));
    float v;
    if (viewZenith < zenithHorizonAngle)
    {
        float coord = 1.0f - sqrt(max(1.0f - viewZenith / zenithHorizonAngle, 0.0f));
        v = coord * 0.5f;
    }
    else
    {
        v = sqrt(saturate((viewZenith - zenithHorizonAngle) / max(beta, 0.00001f))) * 0.5f + 0.5f;
    }
    float2 view = direction.xz;
    float2 light = sun.xz;
    float viewLength = length(view);
    float lightLength = length(light);
    float cosAzimuth = viewLength > 0.00001f && lightLength > 0.00001f ?
        dot(view / viewLength, light / lightLength) : 1.0f;
    float u = sqrt(acos(clamp(cosAzimuth, -1.0f, 1.0f)) / kLutPi);
    // Texel centre remap so that [0,1] covers first..last texel centre.
    float2 size = float2(kSkyViewLutSize);
    return (float2(u, v) * (size - 1.0f) + 0.5f) / size;
}

float3 SkyViewLutDirection(float2 texel, float3 sun, float viewHeight)
{
    float2 size = float2(kSkyViewLutSize);
    float2 uv = texel / (size - 1.0f); // texel index -> [0,1] at texel centres
    float beta, zenithHorizonAngle;
    SkyViewHorizon(viewHeight, beta, zenithHorizonAngle);
    float viewZenith;
    if (uv.y < 0.5f)
    {
        float coord = 1.0f - 2.0f * uv.y;
        viewZenith = zenithHorizonAngle * (1.0f - coord * coord);
    }
    else
    {
        float coord = uv.y * 2.0f - 1.0f;
        viewZenith = zenithHorizonAngle + beta * coord * coord;
    }
    float relativeAzimuth = uv.x * uv.x * kLutPi;
    float sunAzimuth = length(sun.xz) > 0.00001f ? atan2(sun.z, sun.x) : 0.0f;
    float azimuth = sunAzimuth + relativeAzimuth;
    float sinZenith = sin(viewZenith);
    return float3(sinZenith * cos(azimuth), cos(viewZenith), sinZenith * sin(azimuth));
}

// --- Multiple scattering LUT: (sun cos zenith, altitude) --------------------------------------
float2 MultiScatterLutUv(float radius, float sunCosZenith)
{
    float2 uv = float2(sunCosZenith * 0.5f + 0.5f,
        saturate((radius - kLutGroundRadius) / (kLutAtmosphereRadius - kLutGroundRadius)));
    float2 size = float2(kMultiScatterLutSize);
    return (uv * (size - 1.0f) + 0.5f) / size;
}

#endif
