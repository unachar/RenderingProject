#ifndef STAGE_LIGHTING_HLSLI
#define STAGE_LIGHTING_HLSLI
// Extra.z >= 0: legacy density; < 0: stage mode, -(density + 1).
float StageVolumeDensity(float encodedDensity)
{
    return encodedDensity < 0.0f ? max(-encodedDensity - 1.0f, 0.0f) : max(encodedDensity, 0.0f);
}
float StageRangeAttenuation(float distanceToSource, float range, float encodedDensity)
{
    range = max(range, 0.01f);
    if (encodedDensity < 0.0f)
        return 1.0f - smoothstep(max(range - 1.0f, 0.0f), range, distanceToSource);
    float fade = saturate(1.0f - distanceToSource / range);
    return fade * fade;
}
float StageCapsuleAxial(float projection, float segmentLength)
{
    float halfLength = max(segmentLength, 0.0f) * 0.5f;
    return clamp(projection, -halfLength, halfLength);
}
#endif
