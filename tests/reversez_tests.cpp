#include <DirectXMath.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include "rendering/scenedepth.h"

using namespace DirectX;

int main()
{
    const float nearClip = 0.1f;
    const XMMATRIX projection = SceneDepth::InfiniteReverseZPerspectiveLH(XM_PIDIV4, 16.0f / 9.0f, nearClip);
    const auto projectDepth = [&](float z) {
        const XMVECTOR clip = XMVector4Transform(XMVectorSet(0, 0, z, 1), projection);
        return XMVectorGetZ(clip) / XMVectorGetW(clip);
    };
    assert(std::abs(projectDepth(nearClip) - 1.0f) < 1e-6f);
    float previous = 1.0f;
    const XMMATRIX inverse = XMMatrixInverse(nullptr, projection);
    for (float z : {0.2f, 1.0f, 1000.0f, 100000.0f, 100000000.0f})
    {
        const float depth = projectDepth(z);
        assert(depth > 0.0f && depth < previous);
        assert(std::abs(depth - nearClip / z) < depth * 1e-5f);
        const XMVECTOR world = XMVector4Transform(XMVectorSet(0, 0, depth, 1), inverse);
        const float recovered = XMVectorGetZ(world) / XMVectorGetW(world);
        assert(std::abs(recovered - z) < z * 1e-5f);
        previous = depth;
    }
    assert(projectDepth(nearClip * 0.5f) > 1.0f);
    const XMVECTOR infinitePlane = SceneDepth::NormalizeFrustumPlane(XMVectorSet(0, 0, 0, nearClip));
    assert(!XMVector4IsNaN(infinitePlane));
    assert(XMVectorGetW(infinitePlane) > 0.0f);
    const XMVECTOR finitePlane = SceneDepth::NormalizeFrustumPlane(XMVectorSet(0, 0, 2, -4));
    assert(XMVectorGetZ(finitePlane) == 1.0f && XMVectorGetW(finitePlane) == -2.0f);
    std::puts("Infinite reverse-Z projection tests: PASS");
}
