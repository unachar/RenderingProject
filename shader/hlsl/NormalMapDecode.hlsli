// =============================================================================================
// NormalMapDecode.hlsli - タンジェント空間法線マップのデコード。
// 法線マップは取り込み時に BC5（RG の 2ch）へ圧縮される。BC5 のサンプル結果は B=0 になるため、
// その場合は Z を sqrt(1 - x^2 - y^2) で再構成する。非圧縮・BC7 の RGB 法線マップ（B は通常 0.5 以上）は
// 従来どおり xyz * 2 - 1 で復元するので、既存アセットの見た目は変わらない。
// =============================================================================================
#ifndef NORMAL_MAP_DECODE_HLSLI
#define NORMAL_MAP_DECODE_HLSLI

float3 DecodeTangentNormalSample(float4 encoded)
{
    float3 normal = encoded.xyz * 2.0f - 1.0f;
    [flatten] if (encoded.z <= 0.0f)
    {
        normal.z = sqrt(saturate(1.0f - dot(normal.xy, normal.xy)));
    }
    return normal;
}

#endif // NORMAL_MAP_DECODE_HLSLI
