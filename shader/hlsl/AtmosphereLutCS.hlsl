// =============================================================================================
// AtmosphereLutCS.hlsl  -  physical sky LUTs (runs on the async compute queue when available)
//   TransmittanceLut : optical depth (Rayleigh, Mie) from (r, mu) to the top of the atmosphere.
//                      Depends only on the planet geometry -> built once.
//   MultiScatterLut  : Hillaire 2020 isotropic multiple scattering transfer Psi_ms(h, mu_s).
//                      Rebuilt when the atmosphere coefficients change.
//   SkyViewLut       : in-scattered radiance (rgb) and view transmittance (rgb) for the camera
//                      altitude and sun direction of this frame. Built every frame.
//   Density model and coefficients are identical to EvaluatePhysicalSky (SkyAtmosphere.hlsli).
// =============================================================================================
#include "AtmosphereLutCommon.hlsli"

cbuffer AtmosphereLutConstants : register(b0)
{
    float4 SunDirection;      // xyz normalised
    float4 Coefficients;      // x rayleigh strength, y mie strength, z density, w mie g
    float4 Parameters;        // x camera altitude (m), y multiple scattering strength, z ground albedo
};

Texture2D<float2> TransmittanceLut : register(t0);
Texture2D<float4> MultiScatterLut : register(t1);
RWTexture2D<float4> Output0 : register(u0);
RWTexture2D<float4> Output1 : register(u1);
RWTexture2D<float2> OutputDepth : register(u2);
SamplerState LinearClamp : register(s0);

float2 RaySphere(float3 origin, float3 direction, float radius)
{
    float projected = dot(origin, direction);
    float discriminant = projected * projected - dot(origin, origin) + radius * radius;
    if (discriminant < 0.0f) return float2(-1, -1);
    float root = sqrt(discriminant);
    return float2(-projected - root, -projected + root);
}

float2 Density(float3 position)
{
    float altitude = max(length(position) - kLutGroundRadius, 0.0f);
    return exp(-altitude / float2(8000.0f, 1200.0f));
}

void Betas(out float3 betaRayleigh, out float3 betaMie)
{
    float densityScale = max(Coefficients.z, 0.0f) / 0.36f;
    betaRayleigh = float3(5.8e-6f, 13.5e-6f, 33.1e-6f) * max(Coefficients.x, 0.0f) / 0.42f * densityScale;
    betaMie = 21e-6f * max(Coefficients.y, 0.0f) / 0.075f * densityScale;
}

float2 SunOpticalDepth(float3 position, float3 sun)
{
    float radius = length(position);
    float mu = dot(position / radius, sun);
    return TransmittanceLut.SampleLevel(LinearClamp, TransmittanceLutUv(radius, mu), 0);
}

// ---------------------------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void BuildTransmittance(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= kTransmittanceLutSize)) return;
    float radius, mu;
    TransmittanceLutParameters((id.xy + 0.5f) / float2(kTransmittanceLutSize), radius, mu);
    float3 origin = float3(0, radius, 0);
    float3 direction = float3(sqrt(saturate(1 - mu * mu)), mu, 0);
    float distance = max(RaySphere(origin, direction, kLutAtmosphereRadius).y, 0.0f);
    const int steps = 64;
    float stepLength = distance / steps;
    float2 depth = 0;
    [loop] for (int index = 0; index < steps; ++index)
    {
        depth += Density(origin + direction * ((index + 0.5f) * stepLength)) * stepLength;
    }
    OutputDepth[id.xy] = depth;
}

// ---------------------------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void BuildMultiScatter(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= kMultiScatterLutSize)) return;
    float2 uv = id.xy / (float2(kMultiScatterLutSize) - 1.0f);
    float sunCos = uv.x * 2 - 1;
    float radius = kLutGroundRadius + max(uv.y * (kLutAtmosphereRadius - kLutGroundRadius), 10.0f);
    float3 origin = float3(0, radius, 0);
    float3 sun = float3(sqrt(saturate(1 - sunCos * sunCos)), sunCos, 0);
    float3 betaRayleigh, betaMie;
    Betas(betaRayleigh, betaMie);
    const float isotropicPhase = 1.0f / (4.0f * kLutPi);
    const uint directionsPerAxis = 8;
    const int steps = 20;
    float3 secondOrder = 0;
    float3 transfer = 0;
    [loop] for (uint sample = 0; sample < directionsPerAxis * directionsPerAxis; ++sample)
    {
        // Stratified uniform sphere directions.
        float2 cell = (float2(sample % directionsPerAxis, sample / directionsPerAxis) + 0.5f) / directionsPerAxis;
        float cosTheta = 1 - 2 * cell.y;
        float sinTheta = sqrt(saturate(1 - cosTheta * cosTheta));
        float phi = 2 * kLutPi * cell.x;
        float3 direction = float3(sinTheta * cos(phi), cosTheta, sinTheta * sin(phi));
        float groundDistance = RaySphere(origin, direction, kLutGroundRadius).x;
        bool hitsGround = groundDistance > 0;
        float distance = hitsGround ? groundDistance : max(RaySphere(origin, direction, kLutAtmosphereRadius).y, 0.0f);
        float stepLength = distance / steps;
        float3 throughput = 1;
        float3 luminance = 0;
        float3 multiScatterAsOne = 0;
        [loop] for (int index = 0; index < steps; ++index)
        {
            float3 position = origin + direction * ((index + 0.5f) * stepLength);
            float2 density = Density(position);
            float3 scattering = betaRayleigh * density.x + betaMie * density.y;
            float3 extinction = max(scattering, 1e-9f);
            float3 stepTransmittance = exp(-extinction * stepLength);
            float3 sunTransmittance = 0;
            if (RaySphere(position, sun, kLutGroundRadius).x <= 0)
            {
                float2 sunDepth = SunOpticalDepth(position, sun);
                sunTransmittance = exp(-betaRayleigh * sunDepth.x - betaMie * sunDepth.y);
            }
            float3 source = sunTransmittance * scattering * isotropicPhase;
            float3 integrated = (source - source * stepTransmittance) / extinction;
            luminance += throughput * integrated;
            multiScatterAsOne += throughput * (scattering - scattering * stepTransmittance) / extinction;
            throughput *= stepTransmittance;
        }
        if (hitsGround)
        {
            float3 groundPoint = origin + direction * groundDistance;
            float3 groundNormal = normalize(groundPoint);
            float2 sunDepth = SunOpticalDepth(groundPoint, sun);
            float3 sunTransmittance = exp(-betaRayleigh * sunDepth.x - betaMie * sunDepth.y);
            luminance += throughput * sunTransmittance * saturate(dot(groundNormal, sun)) * Parameters.z / kLutPi;
        }
        secondOrder += luminance;
        transfer += multiScatterAsOne;
    }
    const float sampleWeight = 4.0f * kLutPi / (directionsPerAxis * directionsPerAxis);
    float3 inScattered = secondOrder * sampleWeight * isotropicPhase;
    float3 multiScatterAs1 = transfer * sampleWeight * isotropicPhase;
    float3 psi = inScattered / max(1.0f - multiScatterAs1, 0.001f);
    Output0[id.xy] = float4(psi, 1);
}

// ---------------------------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void BuildSkyView(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= kSkyViewLutSize)) return;
    float altitude = max(Parameters.x, 2.0f);
    float3 origin = float3(0, kLutGroundRadius + altitude, 0);
    float3 sun = normalize(SunDirection.xyz);
    float3 direction = SkyViewLutDirection(float2(id.xy), sun, origin.y);
    float endDistance = max(RaySphere(origin, direction, kLutAtmosphereRadius).y, 0.0f);
    float groundDistance = RaySphere(origin, direction, kLutGroundRadius).x;
    if (groundDistance > 0) endDistance = min(endDistance, groundDistance);
    float3 betaRayleigh, betaMie;
    Betas(betaRayleigh, betaMie);

    const int steps = 32;
    float stepDistance = endDistance / steps;
    float2 viewDepth = 0;
    float3 rayleigh = 0, mie = 0, multiple = 0;
    [loop] for (int index = 0; index < steps; ++index)
    {
        float3 position = origin + direction * ((index + 0.5f) * stepDistance);
        float2 localDepth = Density(position) * stepDistance;
        viewDepth += localDepth * 0.5f;
        float3 viewTransmission = exp(-betaRayleigh * viewDepth.x - betaMie * viewDepth.y);
        float radius = length(position);
        if (RaySphere(position, sun, kLutGroundRadius).x <= 0)
        {
            float2 sunDepth = SunOpticalDepth(position, sun);
            float2 totalDepth = viewDepth + sunDepth;
            float3 transmission = exp(-betaRayleigh * totalDepth.x - betaMie * totalDepth.y);
            rayleigh += localDepth.x * transmission;
            mie += localDepth.y * transmission;
        }
        // Multiple scattering is not shadowed by the planet (it arrives from all directions).
        float3 psi = MultiScatterLut.SampleLevel(LinearClamp,
            MultiScatterLutUv(radius, dot(position / radius, sun)), 0).rgb;
        multiple += (betaRayleigh * localDepth.x + betaMie * localDepth.y) * psi * viewTransmission;
        viewDepth += localDepth * 0.5f;
    }
    float cosine = dot(direction, sun);
    float rayleighPhase = 3.0f / (16.0f * kLutPi) * (1.0f + cosine * cosine);
    float g = clamp(Coefficients.w, -0.95f, 0.95f);
    float g2 = g * g;
    float denominator = max(1.0f + g2 - 2.0f * g * cosine, 0.0001f);
    float miePhase = (1.0f - g2) / (4.0f * kLutPi * denominator * sqrt(denominator));
    float3 scatter = rayleigh * betaRayleigh * rayleighPhase + mie * betaMie * miePhase +
        multiple * max(Parameters.y, 0.0f);
    Output0[id.xy] = float4(scatter, 1);
    Output1[id.xy] = float4(exp(-betaRayleigh * viewDepth.x - betaMie * viewDepth.y), 1);
}
