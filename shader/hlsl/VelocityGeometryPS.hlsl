struct VelocityInput
{
    float4 Position : SV_POSITION;
    float4 CurrentClip : TEXCOORD0;
    float4 PreviousClip : TEXCOORD1;
#ifdef MESHLET_VELOCITY
    nointerpolation uint MaterialFlags : TEXCOORD2;
#endif
};

float4 EncodeVelocity(float2 velocity)
{
    float2 displayVelocity = sign(velocity) * saturate(sqrt(abs(velocity) * 100.0f));
    return float4(displayVelocity * 0.5f + 0.5f, velocity);
}

float4 main(VelocityInput input, bool frontFace : SV_IsFrontFace) : SV_Target
{
#ifdef MESHLET_VELOCITY
    if ((input.MaterialFlags & 4u) == 0 && !frontFace) discard;
#endif
    if (input.CurrentClip.w <= 0.00001f || input.PreviousClip.w <= 0.00001f)
    {
        return EncodeVelocity(0.0f);
    }
    float2 currentNdc = input.CurrentClip.xy / input.CurrentClip.w;
    float2 previousNdc = input.PreviousClip.xy / input.PreviousClip.w;
    float2 currentUv = currentNdc * float2(0.5f, -0.5f) + 0.5f;
    float2 previousUv = previousNdc * float2(0.5f, -0.5f) + 0.5f;
    return EncodeVelocity(currentUv - previousUv);
}

