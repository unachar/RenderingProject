#ifndef GEOMETRIC_SPECULAR_AA_INCLUDED
#define GEOMETRIC_SPECULAR_AA_INCLUDED

// Evaluation path: enable with -D GEOMETRIC_SPECULAR_AA=1. Keep existing rendering by default.
#ifndef GEOMETRIC_SPECULAR_AA
#define GEOMETRIC_SPECULAR_AA 0
#endif

#ifdef __cplusplus
#include <algorithm>
#include <cmath>
#define SPECULAR_AA_INLINE inline
#else
#define SPECULAR_AA_INLINE
#endif

namespace GeometricSpecularAA
{
    SPECULAR_AA_INLINE bool Finite(float value)
    {
#ifdef __cplusplus
        return std::isfinite(value);
#else
        return isfinite(value);
#endif
    }

    SPECULAR_AA_INLINE float ClampUnit(float value)
    {
#ifdef __cplusplus
        return std::clamp(value, 0.0f, 1.0f);
#else
        return saturate(value);
#endif
    }

    SPECULAR_AA_INLINE bool EligibleMaterial(float materialClass)
    {
        // Only the existing PBR class. Toon, eye, unlit and debug classes retain authored roughness.
        return materialClass > 10.5f && materialClass < 11.5f;
    }

    SPECULAR_AA_INLINE float FiniteGradientSquared(float dxSquared, float dySquared)
    {
        float gradient = 0.0f;
        if (Finite(dxSquared) && dxSquared > 0.0f)
        {
            gradient += dxSquared;
        }
        if (Finite(dySquared) && dySquared > 0.0f)
        {
            gradient += dySquared;
        }
        return gradient;
    }

    // Geometric specular antialiasing, as evaluated in Filament's surface_shading_lit.fs.
    // Tokuyoshi / Kaplanyan, "Improved Geometric Specular Antialiasing" (2019).
    // r is perceptual roughness, GGX alpha is r^2: filter alpha^2, then convert back.
    SPECULAR_AA_INLINE float FilterRoughness(float roughness, float normalGradientSquared,
                                           float varianceScale, float threshold)
    {
        if (!Finite(roughness))
        {
            return 0.0f;
        }
        roughness = ClampUnit(roughness);
        if (!Finite(normalGradientSquared) || !Finite(varianceScale) || !Finite(threshold) ||
            normalGradientSquared <= 0.0f || varianceScale <= 0.0f || threshold <= 0.0f)
        {
            return roughness;
        }
        float kernel = 2.0f * varianceScale * normalGradientSquared;
        if (kernel > threshold)
        {
            kernel = threshold;
        }
        const float alpha = roughness * roughness;
        const float filteredAlphaSquared = ClampUnit(alpha * alpha + kernel);
#ifdef __cplusplus
        return std::sqrt(std::sqrt(filteredAlphaSquared));
#else
        return sqrt(sqrt(filteredAlphaSquared));
#endif
    }

#ifndef __cplusplus
    bool ValidNormal(float3 normal)
    {
        const float lengthSquared = dot(normal, normal);
        return all(isfinite(normal)) && isfinite(lengthSquared) && lengthSquared > 1.0e-12f;
    }

    float3 GeometryNormal(float3 normal)
    {
        if (!ValidNormal(normal))
        {
            return float3(0, 1, 0);
        }
        return normal * rsqrt(dot(normal, normal));
    }

    float3 NormalDifference(float3 center, float3 neighbor, bool reconstructed)
    {
        if (!reconstructed || !ValidNormal(center) || !ValidNormal(neighbor))
        {
            return 0;
        }
        return GeometryNormal(neighbor) - GeometryNormal(center);
    }

    float FilterMaterialRoughness(float roughness, float gradientSquared, float materialClass)
    {
#if GEOMETRIC_SPECULAR_AA
        if (EligibleMaterial(materialClass))
        {
            return FilterRoughness(roughness, gradientSquared, 0.15f, 0.2f);
        }
#endif
        return roughness;
    }
#endif
}

#undef SPECULAR_AA_INLINE
#endif
