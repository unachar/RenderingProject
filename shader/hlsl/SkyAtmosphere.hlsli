#include "CelestialLighting.hlsli"
// Single scattering in a spherical Earth atmosphere. Distances are in metres.
float2 SkySphereIntersection(float3 origin, float3 direction, float radius)
{
    float projected = dot(origin, direction);
    float discriminant = projected * projected - dot(origin, origin) + radius * radius;
    if (discriminant < 0.0f)
    {
        return float2(-1, -1);
    }
    float root = sqrt(discriminant);
    return float2(-projected - root, -projected + root);
}

float2 SkyDensity(float3 position)
{
    float altitude = max(length(position) - 6360000.0f, 0.0f);
    return exp(-altitude / float2(8000.0f, 1200.0f));
}

uint CelestialHash(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

float CelestialRandom(uint value)
{
    return (CelestialHash(value) >> 8) * (1.0f / 16777216.0f);
}

float3 SampleStarLayer(float3 direction, float angularFootprint, int longitudeCells, uint seed)
{
    const float pi = 3.14159265f;
    float2 skyUv = float2(atan2(direction.z, direction.x) / (2.0f * pi) + 0.5f,
        asin(clamp(direction.y, -1.0f, 1.0f)) / pi + 0.5f);
    int latitudeCells = longitudeCells / 2;
    float2 gridSize = float2(longitudeCells, latitudeCells);
    int2 cell = int2(floor(skyUv * gridSize));
    float3 radiance = 0;
    [unroll]
    for (int row = -1; row <= 1; ++row)
    {
        [unroll]
        for (int column = -1; column <= 1; ++column)
        {
            int2 neighbour = cell + int2(column, row);
            if (neighbour.y < 0 || neighbour.y >= latitudeCells) continue;
            int wrappedX = (neighbour.x + longitudeCells) % longitudeCells;
            uint hash = CelestialHash(uint(wrappedX) * 1597334677u ^ uint(neighbour.y) * 3812015801u ^ seed);
            float latitude = ((neighbour.y + 0.5f) / latitudeCells - 0.5f) * pi;
            // Compensate for the smaller solid angle of cells near the poles.
            if (CelestialRandom(hash) > 0.16f * cos(latitude)) continue;
            float2 starUv = (float2(neighbour) + float2(CelestialRandom(hash + 17u), CelestialRandom(hash + 31u))) / gridSize;
            float2 separation = (skyUv - starUv) * float2(2.0f * pi * max(cos(latitude), 0.01f), pi);
            float angularRadius = lerp(0.0004f, 0.0015f, pow(CelestialRandom(hash + 71u), 4.0f));
            float footprint = max(angularFootprint * 0.45f, 0.00004f);
            float variance = angularRadius * angularRadius + footprint * footprint;
            // Convolution with the pixel footprint preserves flux as resolution and jitter change.
            float profile = exp(-dot(separation, separation) / (2.0f * variance)) *
                angularRadius * angularRadius / variance;
            float temperature = CelestialRandom(hash + 113u);
            float3 spectrum = temperature < 0.35f ?
                lerp(float3(1.0f, 0.56f, 0.28f), float3(1.0f, 0.93f, 0.78f), temperature / 0.35f) :
                lerp(float3(1.0f, 0.93f, 0.78f), float3(0.50f, 0.70f, 1.0f), (temperature - 0.35f) / 0.65f);
            radiance += spectrum * profile * lerp(2.0f, 18.0f, pow(CelestialRandom(hash + 157u), 5.0f));
        }
    }
    return radiance;
}

float3 EvaluateCelestialSky(float3 direction, float angularFootprint)
{
    float3 stars = SampleStarLayer(direction, angularFootprint, 144, 42u) +
        SampleStarLayer(direction, angularFootprint, 288, 9631u) * 0.35f;
    float galacticLatitude = dot(direction, normalize(float3(0.26f, 0.64f, -0.72f)));
    float band = exp(-galacticLatitude * galacticLatitude * 95.0f);
    float cloud = 0.6f + 0.4f * sin(dot(direction, float3(37.0f, 59.0f, 23.0f))) *
        sin(dot(direction, float3(71.0f, -43.0f, 53.0f)));
    float3 distantSky = float3(0.002f, 0.004f, 0.009f) +
        float3(0.025f, 0.018f, 0.040f) * band * cloud;
    return (stars + distantSky) * max(SkyParameters.w, 0.0f);
}

// Analytic single scattering ray march (reference / fallback when the LUT is unavailable).
void MarchPhysicalSky(float3 direction, float3 origin, float3 sun, float endDistance,
    int viewSteps, int sunSteps, out float3 sky, out float3 viewTransmission)
{
    const float groundRadius = 6360000.0f;
    const float atmosphereRadius = 6460000.0f;
    float densityScale = max(AtmosphereParams0.w, 0.0f) / 0.36f;
    float3 betaRayleigh = float3(5.8e-6f, 13.5e-6f, 33.1e-6f) * max(AtmosphereParams0.y, 0.0f) / 0.42f * densityScale;
    float3 betaMie = 21e-6f * max(AtmosphereParams0.z, 0.0f) / 0.075f * densityScale;
    float stepDistance = endDistance / (float)viewSteps;
    float2 viewOpticalDepth = 0;
    float3 rayleigh = 0;
    float3 mie = 0;
    [loop]
    for (int stepIndex = 0; stepIndex < viewSteps; ++stepIndex)
    {
        float3 position = origin + direction * ((stepIndex + 0.5f) * stepDistance);
        float2 localDepth = SkyDensity(position) * stepDistance;
        viewOpticalDepth += localDepth * 0.5f;
        if (SkySphereIntersection(position, sun, groundRadius).x <= 0.0f)
        {
            float sunStep = max(SkySphereIntersection(position, sun, atmosphereRadius).y, 0.0f) / (float)sunSteps;
            float2 sunOpticalDepth = 0;
            for (int sample = 0; sample < sunSteps; ++sample)
            {
                sunOpticalDepth += SkyDensity(position + sun * ((sample + 0.5f) * sunStep)) * sunStep;
            }
            float2 totalDepth = viewOpticalDepth + sunOpticalDepth;
            float3 transmission = exp(-betaRayleigh * totalDepth.x - betaMie * totalDepth.y);
            rayleigh += localDepth.x * transmission;
            mie += localDepth.y * transmission;
        }
        viewOpticalDepth += localDepth * 0.5f;
    }
    float cosine = dot(direction, sun);
    float rayleighPhase = 3.0f / (16.0f * 3.14159265f) * (1.0f + cosine * cosine);
    sky = rayleigh * betaRayleigh * rayleighPhase + mie * betaMie * HenyeyGreensteinCommon(cosine, AtmosphereParams1.z);
    viewTransmission = exp(-betaRayleigh * viewOpticalDepth.x - betaMie * viewOpticalDepth.y);
}

#ifdef SKY_LUT
// Sky view LUT (AtmosphereLutCS.hlsl). The consumer defines the registers:
//   SKY_LUT_SCATTER_REGISTER, SKY_LUT_TRANSMISSION_REGISTER, SKY_LUT_SAMPLER_REGISTER.
// A null descriptor (LUT disabled / not built yet) reads alpha 0 -> analytic fallback.
#include "AtmosphereLutCommon.hlsli"
Texture2D<float4> SkyViewScatterLut : register(SKY_LUT_SCATTER_REGISTER);
Texture2D<float4> SkyViewTransmissionLut : register(SKY_LUT_TRANSMISSION_REGISTER);
SamplerState SkyLutSampler : register(SKY_LUT_SAMPLER_REGISTER);

bool SampleSkyViewLut(float3 direction, float3 sun, float viewHeight, out float3 sky, out float3 viewTransmission)
{
    float2 uv = SkyViewLutUv(direction, sun, viewHeight);
    float4 scatter = SkyViewScatterLut.SampleLevel(SkyLutSampler, uv, 0);
    sky = scatter.rgb;
    viewTransmission = SkyViewTransmissionLut.SampleLevel(SkyLutSampler, uv, 0).rgb;
    return scatter.a > 0.5f;
}
#endif

// includeDiscs = false removes the sun and moon discs. Diffuse GI rays use it: both discs are
// already lit directly, and their tiny solid angle turns into rare, very bright samples that
// no temporal denoiser can hide (sparkling / flicker). Their irradiance share is negligible.
// viewSteps / sunSteps select the analytic fallback quality. With SKY_LUT the scattering and
// view transmission come from the sky view LUT (one bilinear fetch instead of ~80 density
// evaluations); discs, stars, moon, ground and airglow are evaluated per pixel as before.
float3 EvaluatePhysicalSkySteps(float3 direction, float angularFootprint, bool includeDiscs,
    int viewSteps, int sunSteps)
{
    direction = normalize(direction);
    const float groundRadius = 6360000.0f;
    const float atmosphereRadius = 6460000.0f;
    float3 origin = float3(0, groundRadius + max(AtmosphereCamera.y, 2.0f), 0);
    float3 sun = normalize(SkySunDirection.xyz);
    float groundDistance = SkySphereIntersection(origin, direction, groundRadius).x;
    bool hitsGround = groundDistance > 0.0f;
    float3 sky = 0;
    float3 viewTransmission = 1;
    bool fromLut = false;
#ifdef SKY_LUT
    fromLut = SampleSkyViewLut(direction, sun, origin.y, sky, viewTransmission);
#endif
    [branch]
    if (!fromLut)
    {
        float endDistance = max(SkySphereIntersection(origin, direction, atmosphereRadius).y, 0.0f);
        if (hitsGround)
        {
            endDistance = min(endDistance, groundDistance);
        }
        MarchPhysicalSky(direction, origin, sun, endDistance, viewSteps, sunSteps, sky, viewTransmission);
    }
    sky *= max(SkyParameters.z, 0.0f);
    float cosine = dot(direction, sun);
    if (includeDiscs && !hitsGround && sun.y > -0.01f)
    {
        sky += smoothstep(cos(0.0052f), cos(0.0042f), cosine) * viewTransmission * max(SkyParameters.z, 0.0f) * 4.0f;
    }
    if (hitsGround)
    {
        sky += float3(0.035f, 0.028f, 0.018f) * SkyParameters.y * viewTransmission;
    }
    else
    {
        // Stars remain fixed throughout the day. Scattered solar light reduces their perceived contrast.
        float solarGlare = dot(sky, float3(0.2126f, 0.7152f, 0.0722f));
        float stellarContrast = rcp(1 + max(solarGlare, 0) * 10000000);
        sky += EvaluateCelestialSky(direction, angularFootprint) * viewTransmission * stellarContrast;
        float3 moon = VirtualMoonDirection();
        float moonCosine = dot(direction, moon);
        float discRadius = 0.00465;
        float discEdge = max(angularFootprint, 0.00005);
        float disc = smoothstep(cos(discRadius + discEdge), cos(max(discRadius - discEdge, 0.00001)), moonCosine);
        float3 moonLight = VirtualMoonIrradiance();
        float halo = 0.008 / max(1.0 - moonCosine + 0.008, 0.008);
        sky += moonLight * ((includeDiscs ? disc * 120 : 0) + halo * halo * 0.8) * viewTransmission;
        // Airglow is emitted inside the atmosphere rather than extinguished as distant starlight.
        sky += float3(0.006f, 0.010f, 0.024f) * lerp(0.2f, 1.0f, smoothstep(0.0f, 0.12f, direction.y));
    }
    return max(sky, 0.0f);
}

// Reference quality (unchanged output): visible sky, mirror reflections.
float3 EvaluatePhysicalSky(float3 direction, float angularFootprint, bool includeDiscs = true)
{
    return EvaluatePhysicalSkySteps(direction, angularFootprint, includeDiscs, 16, 4);
}

// Reduced quality for low frequency terms (diffuse sky lighting of secondary hits, GI sky
// rays). Single scattering is smooth along the ray; 8 / 2 steps differ by a few percent at
// the horizon only, and those terms are further filtered by the denoiser. ~4x cheaper.
float3 EvaluatePhysicalSkyFast(float3 direction, float angularFootprint, bool includeDiscs = true)
{
    return EvaluatePhysicalSkySteps(direction, angularFootprint, includeDiscs, 8, 2);
}

#ifndef SKY_COMPUTE
float3 EvaluateSkyAtmosphere(float3 direction)
{
    float angularFootprint = max(length(ddx(direction)), length(ddy(direction)));
    return EvaluatePhysicalSky(direction, angularFootprint);
}
#endif

