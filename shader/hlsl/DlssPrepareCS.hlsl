Texture2D<float4> Input : register(t0);
#ifndef DLSS_COPY
RWTexture2D<float2> Motion : register(u0);

// 最適化：色のコピーは廃止（NGX がポストプロセス出力を直接読む）。ここでは Motion だけを整形する。
[numthreads(8, 8, 1)] void Prepare(uint3 pixel : SV_DispatchThreadID) {
    uint width;
    uint height;
    Motion.GetDimensions(width, height);
    if (pixel.x >= width || pixel.y >= height)
    {
        return;
    }
    // エンジンは現在 UV - 前 UV を ZW に格納するが、NGX は逆方向を要求する。
    float2 motion = -Input[pixel.xy].zw;
    Motion[pixel.xy] = all(isfinite(motion)) ? motion : float2(0, 0);
}

#else
RWTexture2D<float4> DisplayOutput : register(u0);

[numthreads(8, 8, 1)] void Copy(uint3 pixel : SV_DispatchThreadID) {
    uint width;
    uint height;
    Input.GetDimensions(width, height);
    if (pixel.x < width && pixel.y < height)
    {
        DisplayOutput[pixel.xy] = Input[pixel.xy];
    }
}
#endif

