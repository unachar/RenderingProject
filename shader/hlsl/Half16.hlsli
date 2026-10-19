// =============================================================================================
// Half16.hlsli - ネイティブ 16bit 演算（DXC -enable-16bit-types）用の型エイリアス。
// Shader16Bit::CompileFromFile が対応 GPU で RP_NATIVE_16BIT=1 と -enable-16bit-types を付けてコンパイルする。
// 非対応時（または FXC / 16bit 無効時）は float に戻るため、同じソースで従来と同じ結果になる。
// =============================================================================================
#ifndef HALF16_HLSLI
#define HALF16_HLSLI

#if defined(RP_NATIVE_16BIT) && RP_NATIVE_16BIT && defined(__HLSL_ENABLE_16_BIT)
#define RP_HALF_ENABLED 1
typedef float16_t hfloat;
typedef float16_t2 hfloat2;
typedef float16_t3 hfloat3;
typedef float16_t4 hfloat4;
// FP16 の最大有限値。HDR 値の加算前にこの値で飽和させ、INF を防ぐ。
static const float kHalfMax = 65504.0f;
#else
#define RP_HALF_ENABLED 0
typedef float hfloat;
typedef float2 hfloat2;
typedef float3 hfloat3;
typedef float4 hfloat4;
static const float kHalfMax = 3.0e38f;
#endif

// float → hfloat 変換（飽和付き）。16bit 時は FP16 の範囲へ収める。
hfloat3 ToHalf3(float3 value)
{
    return (hfloat3)min(value, kHalfMax);
}

hfloat ToHalf(float value)
{
    return (hfloat)min(value, kHalfMax);
}

#endif // HALF16_HLSLI
