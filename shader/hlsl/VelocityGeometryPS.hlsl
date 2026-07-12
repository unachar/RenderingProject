struct VelocityInput
{
    float4 Position : SV_POSITION;
    float4 CurrentClip : TEXCOORD0;
    float4 PreviousClip : TEXCOORD1;
};

float2 main(VelocityInput input) : SV_Target
{
    if (input.CurrentClip.w <= 0.00001f || input.PreviousClip.w <= 0.00001f)
    {
        return 0.0f;
    }
    float2 currentNdc = input.CurrentClip.xy / input.CurrentClip.w;
    float2 previousNdc = input.PreviousClip.xy / input.PreviousClip.w;
    float2 currentUv = currentNdc * float2(0.5f, -0.5f) + 0.5f;
    float2 previousUv = previousNdc * float2(0.5f, -0.5f) + 0.5f;
    return currentUv - previousUv;
}
