#ifndef TOON_OUTLINE_PROJECTION_HLSLI
#define TOON_OUTLINE_PROJECTION_HLSLI

// normalClip is the extrusion vector transformed with w=0. The quotient-rule
// numerator gives its exact projected direction without another point transform.
float2 ToonOutlineClipOffset(float4 clip, float4 normalClip, float2 viewport, float pixelWidth)
{
    if (clip.w <= 0.00001f || pixelWidth <= 0.0f)
    {
        return float2(0.0f, 0.0f);
    }
    float width = max(viewport.x, 1.0f);
    float height = max(viewport.y, 1.0f);
    float dx = (normalClip.x * clip.w - clip.x * normalClip.w) * width;
    float dy = (normalClip.y * clip.w - clip.y * normalClip.w) * height;
    float lengthSquared = dx * dx + dy * dy;
    if (lengthSquared <= 1.0e-12f)
    {
        // Includes the zero normals on the fixed/base vertices of a TEO strip.
        return float2(0.0f, 0.0f);
    }
    float scale = rsqrt(lengthSquared) * pixelWidth * 2.0f * clip.w;
    return float2(dx * scale / width, dy * scale / height);
}
#endif
