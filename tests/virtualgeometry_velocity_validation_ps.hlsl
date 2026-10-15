struct VelocityInput
{
    float4 Position : SV_POSITION;
    float4 CurrentClip : TEXCOORD0;
    float4 PreviousClip : TEXCOORD1;
    nointerpolation uint MaterialFlags : TEXCOORD2;
};
float4 main(VelocityInput input) : SV_TARGET0
{
    float2 current = input.CurrentClip.xy/input.CurrentClip.w;
    float2 previous = input.PreviousClip.xy/input.PreviousClip.w;
    return float4(current-previous,1,1);
}
