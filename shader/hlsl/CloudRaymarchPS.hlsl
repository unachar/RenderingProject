#include "CloudCommon.hlsli"
CloudOutput main(PSInputPostProcess input)
{
    return MarchCloud(input.TexCoord, input.Position.xy);
}

