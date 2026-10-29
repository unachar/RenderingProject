#ifndef SKINNING_NORMAL_HLSLI
#define SKINNING_NORMAL_HLSLI
float3 NormalizeSkinnedNormal(float3 sourceNormal, float3 transformedNormal)
{
    // TEO base vertices deliberately encode no extrusion as a zero normal.
    // Preserve that sentinel through every deformation path, including DQS/SDEF.
    if (dot(sourceNormal, sourceNormal) <= 0.000001f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }
    float lengthSquared = dot(transformedNormal, transformedNormal);
    return lengthSquared > 0.000001f ? transformedNormal * rsqrt(lengthSquared) : float3(0.0f, 1.0f, 0.0f);
}
#endif
