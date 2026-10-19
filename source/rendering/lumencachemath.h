#ifndef LUMEN_CACHE_MATH_H
#define LUMEN_CACHE_MATH_H
// HLSL と CPU 回帰テストで共有し、平行な層と裏面を除外する。
#ifdef __cplusplus
namespace Lumen
{
#endif
    bool AcceptSurfaceSample(float planeDistance, float tangentDistance, float normalCosine, float texelSize)
    {
        float planeTolerance = texelSize * 0.04f + 0.001f;
        return normalCosine > 0.95f && planeDistance < planeTolerance && tangentDistance < texelSize * 1.5f;
    }
#ifdef __cplusplus
}
#endif
#endif
