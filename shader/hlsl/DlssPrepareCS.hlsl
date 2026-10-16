Texture2D<float4> Input : register(t0);
#ifndef DLSS_COPY
Texture2D<float4> InputColor : register(t1);
RWTexture2D<float2> Motion : register(u0);
RWTexture2D<float4> Color : register(u1);

[numthreads(8, 8, 1)]
void Prepare(uint3 pixel : SV_DispatchThreadID)
{
    uint width;
    uint height;
    Input.GetDimensions(width, height);
    if (pixel.x >= width || pixel.y >= height)
    {
        return;
    }
    // The engine stores current-minus-previous UV in ZW; NGX expects the reverse.
    float2 motion = -Input[pixel.xy].zw;
    Motion[pixel.xy] = all(isfinite(motion)) ? motion : float2(0, 0);
    Color[pixel.xy] = InputColor[pixel.xy];
}

#else
RWTexture2D<float4> DisplayOutput : register(u0);

[numthreads(8, 8, 1)]
void Copy(uint3 pixel : SV_DispatchThreadID)
{
    uint width;
    uint height;
    Input.GetDimensions(width, height);
    if (pixel.x < width && pixel.y < height)
    {
        DisplayOutput[pixel.xy] = Input[pixel.xy];
    }
}
#endif

