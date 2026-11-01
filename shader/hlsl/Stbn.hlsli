#ifndef STBN_INCLUDED
#define STBN_INCLUDED
ByteAddressBuffer StbnSamples : register(t0, space16);

// The first sample uses consecutive temporal slices even with multiple rays.
// Domain offsets translate the whole blue-noise tile, never hash each pixel.
SamplePair StbnSequence(uint2 pixel, uint frame, uint domain, SamplePair fallback, uint sampleIndex = 0,
                       bool preserveR2Temporal = false)
{
    if (StbnSamples.Load(0) == 0)
    {
        return fallback;
    }
    uint offsetSeed = SampleHash(domain * 0x9e3779b9u + sampleIndex * 0x85ebca6bu);
    uint2 offset = uint2(offsetSeed, offsetSeed >> 16);
    uint2 texel = (pixel + offset) & 127u;
    uint slice = (frame + sampleIndex * 17u) & 63u;
    if (preserveR2Temporal)
    {
        slice = 0;
    }
    uint index = (slice * 128u + texel.y) * 128u + texel.x;
    uint packed = StbnSamples.Load(4u + (index >> 1u) * 4u);
    packed >>= (index & 1u) * 16u;
    SamplePair result;
    // Decode quantization bins at their centers; avoid singular warp endpoints.
    result.x = (float(packed & 255u) + 0.5f) / 256.0f;
    result.y = (float((packed >> 8u) & 255u) + 0.5f) / 256.0f;
    if (preserveR2Temporal)
    {
        // Keep the denoiser's low-discrepancy temporal coverage, using STBN
        // instead of a per-pixel hash for its spatial Cranley-Patterson shift.
        float phase = float(((frame + sampleIndex * 8u) & 4095u) + 1u);
        result.x = frac(result.x + phase * .754877666f);
        result.y = frac(result.y + phase * .569840291f);
    }
    return result;
}
#endif
