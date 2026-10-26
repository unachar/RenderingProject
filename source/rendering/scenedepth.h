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

    inline DirectX::XMMATRIX ReverseZOrthographicLH(float height, float aspect, float nearClip, float farClip)
    {
        assert(std::isfinite(height) && height > 0.0f);
        assert(std::isfinite(aspect) && aspect > 0.0f);
        assert(std::isfinite(nearClip) && nearClip > 0.0f);
        assert(std::isfinite(farClip) && farClip > nearClip);
        return DirectX::XMMatrixOrthographicLH(height * aspect, height, farClip, nearClip);
    }

    inline bool IsOrthographic(const DirectX::XMFLOAT4X4& projection)
    {
        return std::abs(projection._44) > 0.5f;
    }

    inline DirectX::XMFLOAT2 ProjectionJitter(const DirectX::XMFLOAT4X4& projection)
    {
        return IsOrthographic(projection) ? DirectX::XMFLOAT2{projection._41, projection._42}
                                          : DirectX::XMFLOAT2{projection._31, projection._32};
    }

    inline void SetProjectionJitter(DirectX::XMFLOAT4X4& projection, DirectX::XMFLOAT2 jitter)
    {
        if (IsOrthographic(projection))
        {
            projection._41 = jitter.x;
            projection._42 = jitter.y;
        }
        else
        {
            projection._31 = jitter.x;
            projection._32 = jitter.y;
        }
    }
}
