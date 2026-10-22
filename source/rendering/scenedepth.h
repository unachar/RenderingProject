#pragma once
#include <DirectXMath.h>
#include <cassert>
#include <cmath>

namespace SceneDepth
{
    inline constexpr float ClearValue = 0.0f;

    // 左手系・行ベクトル。clip.z = nearClip、clip.w = view.z より depth = nearClip / view.z。
    inline DirectX::XMMATRIX InfiniteReverseZPerspectiveLH(float fovY, float aspect, float nearClip)
    {
        assert(std::isfinite(fovY) && fovY > 0.0f && fovY < DirectX::XM_PI);
        assert(std::isfinite(aspect) && aspect > 0.0f);
        assert(std::isfinite(nearClip) && nearClip > 0.0f);
        const float yScale = 1.0f / std::tan(fovY * 0.5f);
        const float xScale = yScale / aspect;
        return DirectX::XMMatrixSet(xScale, 0, 0, 0,
                                    0, yScale, 0, 0,
                                    0, 0, 0, 1,
                                    0, 0, nearClip, 0);
    }

    inline DirectX::XMVECTOR NormalizeFrustumPlane(DirectX::FXMVECTOR plane)
    {
        // 無限遠の平面は法線がゼロ。常に内側となる平面へ置換して NaN を防ぐ。
        if (DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(plane)) == 0.0f)
        {
            return DirectX::XMVectorSet(0, 0, 0, 1);
        }
        return DirectX::XMPlaneNormalize(plane);
    }
}
