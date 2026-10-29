#ifndef DUAL_QUATERNION_SKINNING_HLSLI
#define DUAL_QUATERNION_SKINNING_HLSLI

float4 NormalizeQuaternion(float4 q)
{
    return q * rsqrt(max(dot(q, q), 0.00000001f));
}

float4 QuaternionFromMatrix(float3x3 m)
{
    float4 q;
    const float trace = m[0][0] + m[1][1] + m[2][2];
    if (trace > 0.0f)
    {
        const float s = sqrt(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m[1][2] - m[2][1]) / s;
        q.y = (m[2][0] - m[0][2]) / s;
        q.z = (m[0][1] - m[1][0]) / s;
    }
    else if (m[0][0] > m[1][1] && m[0][0] > m[2][2])
    {
        const float s = sqrt(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
        q.w = (m[1][2] - m[2][1]) / s;
        q.x = 0.25f * s;
        q.y = (m[0][1] + m[1][0]) / s;
        q.z = (m[2][0] + m[0][2]) / s;
    }
    else if (m[1][1] > m[2][2])
    {
        const float s = sqrt(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
        q.w = (m[2][0] - m[0][2]) / s;
        q.x = (m[0][1] + m[1][0]) / s;
        q.y = 0.25f * s;
        q.z = (m[1][2] + m[2][1]) / s;
    }
    else
    {
        const float s = sqrt(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
        q.w = (m[0][1] - m[1][0]) / s;
        q.x = (m[2][0] + m[0][2]) / s;
        q.y = (m[1][2] + m[2][1]) / s;
        q.z = 0.25f * s;
    }
    return NormalizeQuaternion(q);
}

float4 QuaternionSlerp(float4 a, float4 b, float t)
{
    float cosTheta = dot(a, b);
    if (cosTheta < 0.0f)
    {
        b = -b;
        cosTheta = -cosTheta;
    }

    float4 result = NormalizeQuaternion(lerp(a, b, t));
    if (cosTheta <= 0.9995f)
    {
        const float theta = acos(clamp(cosTheta, -1.0f, 1.0f));
        const float sinTheta = max(sin(theta), 0.00001f);
        result = (sin((1.0f - t) * theta) * a + sin(t * theta) * b) / sinTheta;
    }
    return result;
}

float3x3 MatrixFromQuaternion(float4 q)
{
    q = NormalizeQuaternion(q);
    const float x2 = q.x + q.x;
    const float y2 = q.y + q.y;
    const float z2 = q.z + q.z;
    const float xx = q.x * x2;
    const float yy = q.y * y2;
    const float zz = q.z * z2;
    const float xy = q.x * y2;
    const float xz = q.x * z2;
    const float yz = q.y * z2;
    const float wx = q.w * x2;
    const float wy = q.w * y2;
    const float wz = q.w * z2;

    return float3x3(1.0f - yy - zz, xy + wz, xz - wy, xy - wz, 1.0f - xx - zz, yz + wx, xz + wy, yz - wx,
                    1.0f - xx - yy);
}

// Only proper rigid affine transforms can be represented without losing scale.
// Matrices here are logical HLSL row-vector matrices, after buffer interpretation.
bool IsRigidSkinningMatrix(float4x4 m)
{
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            if (!isfinite(m[row][column])) return false;
        }
    }
    const float tolerance = 0.0001f;
    const float3 x = float3(m[0][0], m[0][1], m[0][2]);
    const float3 y = float3(m[1][0], m[1][1], m[1][2]);
    const float3 z = float3(m[2][0], m[2][1], m[2][2]);
    return abs(dot(x, x) - 1.0f) <= tolerance && abs(dot(y, y) - 1.0f) <= tolerance &&
           abs(dot(z, z) - 1.0f) <= tolerance && abs(dot(x, y)) <= tolerance &&
           abs(dot(x, z)) <= tolerance && abs(dot(y, z)) <= tolerance &&
           abs(dot(cross(x, y), z) - 1.0f) <= tolerance &&
           abs(m[0][3]) <= tolerance && abs(m[1][3]) <= tolerance &&
           abs(m[2][3]) <= tolerance && abs(m[3][3] - 1.0f) <= tolerance;
}

struct SkinningDualQuaternion
{
    float4 Real;
    float4 Dual;
};

float4 MultiplySkinningQuaternion(float4 a, float4 b)
{
    const float3 av = float3(a.x, a.y, a.z);
    const float3 bv = float3(b.x, b.y, b.z);
    return float4(a.w * bv + b.w * av + cross(av, bv), a.w * b.w - dot(av, bv));
}

SkinningDualQuaternion ZeroDualQuaternion()
{
    SkinningDualQuaternion result;
    result.Real = float4(0.0f, 0.0f, 0.0f, 0.0f);
    result.Dual = result.Real;
    return result;
}

SkinningDualQuaternion DualQuaternionFromMatrix(float4x4 m)
{
    SkinningDualQuaternion result;
    result.Real = QuaternionFromMatrix((float3x3)m);
    const float4 translation = float4(m[3][0], m[3][1], m[3][2], 0.0f);
    result.Dual = 0.5f * MultiplySkinningQuaternion(translation, result.Real);
    return result;
}

SkinningDualQuaternion AccumulateDualQuaternion(SkinningDualQuaternion sum,
                                                SkinningDualQuaternion value, float weight, float4 reference)
{
    // q and -q encode the same rotation: flip BOTH halves to the reference hemisphere.
    const float signedWeight = dot(value.Real, reference) < 0.0f ? -weight : weight;
    sum.Real = sum.Real + signedWeight * value.Real;
    sum.Dual = sum.Dual + signedWeight * value.Dual;
    return sum;
}

SkinningDualQuaternion NormalizeDualQuaternion(SkinningDualQuaternion value)
{
    const float lengthSquared = dot(value.Real, value.Real);
    if (!isfinite(lengthSquared) || lengthSquared <= 0.00000001f)
    {
        value = ZeroDualQuaternion();
        value.Real = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return value;
    }
    const float inverseLength = rsqrt(lengthSquared);
    value.Real = value.Real * inverseLength;
    value.Dual = value.Dual * inverseLength;
    value.Dual = value.Dual - value.Real * dot(value.Real, value.Dual);
    return value;
}

float3 RotateDualQuaternionVector(SkinningDualQuaternion value, float3 direction)
{
    const float3 q = float3(value.Real.x, value.Real.y, value.Real.z);
    return direction + 2.0f * cross(q, cross(q, direction) + value.Real.w * direction);
}

float3 TransformDualQuaternionPosition(SkinningDualQuaternion value, float3 position)
{
    const float4 conjugate = float4(-value.Real.x, -value.Real.y, -value.Real.z, value.Real.w);
    const float4 translation = 2.0f * MultiplySkinningQuaternion(value.Dual, conjugate);
    return RotateDualQuaternionVector(value, position) + float3(translation.x, translation.y, translation.z);
}

#endif
