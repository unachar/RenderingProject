struct VelocityInput
{
    float4 Position : SV_POSITION;
    float4 CurrentClip : TEXCOORD0;
    float4 PreviousClip : TEXCOORD1;
};

float4 EncodeVelocity(float2 velocity)
{
    float2 displayVelocity = sign(velocity) * saturate(sqrt(abs(velocity) * 100.0f));
    return float4(displayVelocity * 0.5f + 0.5f, velocity);
}

float4 main(VelocityInput input) : SV_Target
{
    float2 currentNdc = input.CurrentClip.xy / input.CurrentClip.w;
    float2 previousNdc = input.PreviousClip.xy / input.PreviousClip.w;
    float2 currentUv = currentNdc * float2(0.5f, -0.5f) + 0.5f;
    float2 previousUv = previousNdc * float2(0.5f, -0.5f) + 0.5f;
    return EncodeVelocity(currentUv - previousUv);
}
