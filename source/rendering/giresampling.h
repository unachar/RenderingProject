#pragma once
#ifdef __cplusplus
namespace GiResampling
{
#endif

struct GiReuseWeights
{
    float Selection;
    float Target;
};

inline float GiRadianceImportance(float red, float green, float blue)
{
    // Keep the established proposal distribution while correcting reuse geometry.
    return .2126f * red + .7152f * green + .0722f * blue;
}

inline float GiGuideAcceptance(float cosine, float inversePdf)
{
    float probability = cosine * inversePdf / 3.14159265f;
    return probability > 0 ? (probability < 1 ? probability : 1) : 0;
}

// The selected target is kept separately from the solid-angle conversion.
inline GiReuseWeights GiSpatialReuseWeights(float target, float inversePdf, float count, float jacobian)
{
    GiReuseWeights weights;
    weights.Target = target;
    weights.Selection = target * inversePdf * count * jacobian;
    return weights;
}

#ifdef __cplusplus
}
#endif
