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
        // 再利用時の幾何補正を行いながら、既存の提案分布を維持する。
        return .2126f * red + .7152f * green + .0722f * blue;
    }

    inline float GiGuideAcceptance(float cosine, float inversePdf)
    {
        float probability = cosine * inversePdf / 3.14159265f;
        if (probability > 0)
        {
            if (probability < 1)
            {
                return probability;
            }
            return 1;
        }
        return 0;
    }

    // 選択したターゲット値は立体角変換と分けて保持する。
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
