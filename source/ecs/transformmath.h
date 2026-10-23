#pragma once
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>

// ワールド行列の共通実装。
// 従来の Scaling * RotationX * RotationY * RotationZ * Translation（行列生成5回＋4x4乗算4回）と
// 同じ回転順（X→Y→Z、行ベクトル）を、sin/cos 3組からの解析式で直接組み立てる。
// 以前は同じ式が TransformSystem / Light / Sun / エディター / 物理 の5か所に重複していた。
namespace TransformMath
{
    inline DirectX::XMMATRIX RotationXYZ(const DirectX::XMFLOAT3& rotation)
    {
        float sx;
        float cx;
        float sy;
        float cy;
        float sz;
        float cz;
        DirectX::XMScalarSinCos(&sx, &cx, rotation.x);
        DirectX::XMScalarSinCos(&sy, &cy, rotation.y);
        DirectX::XMScalarSinCos(&sz, &cz, rotation.z);
        const float sxsy = sx * sy;
        const float cxsy = cx * sy;
        return DirectX::XMMATRIX(cy * cz, cy * sz, -sy, 0.0f, sxsy * cz - cx * sz, sxsy * sz + cx * cz, sx * cy, 0.0f,
                                 cxsy * cz + sx * sz, cxsy * sz - sx * cz, cx * cy, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    }

    inline DirectX::XMMATRIX World(const DirectX::XMFLOAT3& position, const DirectX::XMFLOAT3& rotation,
                                   const DirectX::XMFLOAT3& scale)
    {
        DirectX::XMMATRIX m = RotationXYZ(rotation);
        m.r[0] = DirectX::XMVectorScale(m.r[0], scale.x);
        m.r[1] = DirectX::XMVectorScale(m.r[1], scale.y);
        m.r[2] = DirectX::XMVectorScale(m.r[2], scale.z);
        m.r[3] = DirectX::XMVectorSet(position.x, position.y, position.z, 1.0f);
        return m;
    }

    inline DirectX::XMMATRIX RotationTranslation(const DirectX::XMFLOAT3& rotation, const DirectX::XMFLOAT3& position)
    {
        DirectX::XMMATRIX m = RotationXYZ(rotation);
        m.r[3] = DirectX::XMVectorSet(position.x, position.y, position.z, 1.0f);
        return m;
    }

    inline bool Same(const DirectX::XMFLOAT3& left, const DirectX::XMFLOAT3& right)
    {
        return left.x == right.x && left.y == right.y && left.z == right.z;
    }

    template <typename TransformT> inline DirectX::XMMATRIX World(const TransformT& transform)
    {
        if constexpr (requires { transform.HasAffineWorld; })
        {
            if (transform.HasAffineWorld && Same(transform.Position, transform.AffinePosition) &&
                Same(transform.Rotation, transform.AffineRotation) && Same(transform.Scale, transform.AffineScale))
            {
                return DirectX::XMLoadFloat4x4(&transform.WorldMatrix);
            }
        }
        return World(transform.Position, transform.Rotation, transform.Scale);
    }

    template <typename TransformT> bool SetWorld(TransformT& transform, DirectX::FXMMATRIX world)
    {
        using namespace DirectX;
        XMVECTOR scale;
        XMVECTOR quaternion;
        XMVECTOR position;
        if (!XMMatrixDecompose(&scale, &quaternion, &position, world))
        {
            // TRS cannot describe shear. Keep the exact affine matrix for rendering,
            // and expose an orthogonal frame to tools that require Euler angles.
            const float scaleX = XMVectorGetX(XMVector3Length(world.r[0]));
            if (scaleX < 0.0000001f)
            {
                return false;
            }
            const XMVECTOR axisX = XMVectorScale(world.r[0], 1.0f / scaleX);
            const XMVECTOR projectedY =
                XMVectorSubtract(world.r[1], XMVectorMultiply(axisX, XMVector3Dot(world.r[1], axisX)));
            const float scaleY = XMVectorGetX(XMVector3Length(projectedY));
            if (scaleY < 0.0000001f)
            {
                return false;
            }
            const XMVECTOR axisY = XMVectorScale(projectedY, 1.0f / scaleY);
            const XMVECTOR axisZ = XMVector3Cross(axisX, axisY);
            const float scaleZ = XMVectorGetX(XMVector3Dot(world.r[2], axisZ));
            if (std::abs(scaleZ) < 0.0000001f)
            {
                return false;
            }
            scale = XMVectorSet(scaleX, scaleY, scaleZ, 0);
            position = world.r[3];
            quaternion = XMQuaternionRotationMatrix(XMMATRIX(axisX, axisY, axisZ, XMVectorSet(0, 0, 0, 1)));
        }
        XMFLOAT4X4 rotation;
        XMStoreFloat4x4(&rotation, XMMatrixRotationQuaternion(quaternion));
        const float horizontal = std::hypot(rotation._11, rotation._12);
        const float pitch = std::atan2(-rotation._13, horizontal);
        if (horizontal > 0.00001f)
        {
            transform.Rotation = {std::atan2(rotation._23, rotation._33), pitch,
                                  std::atan2(rotation._12, rotation._11)};
        }
        else
        {
            transform.Rotation = {std::atan2(-rotation._32, rotation._22), pitch, 0.0f};
        }
        XMStoreFloat3(&transform.Scale, scale);
        XMStoreFloat3(&transform.Position, position);
        XMStoreFloat4x4(&transform.WorldMatrix, world);
        if constexpr (requires { transform.HasAffineWorld; })
        {
            transform.HasAffineWorld = true;
            transform.AffinePosition = transform.Position;
            transform.AffineRotation = transform.Rotation;
            transform.AffineScale = transform.Scale;
        }
        transform.IsDirty = true;
        return true;
    }

    template <typename TransformT>
    bool InheritParent(TransformT& transform, DirectX::FXMMATRIX previousParent, DirectX::CXMMATRIX parent)
    {
        using namespace DirectX;
        if (std::abs(XMVectorGetX(XMMatrixDeterminant(previousParent))) < 0.0000001f)
        {
            return false;
        }
        return SetWorld(transform, World(transform) * XMMatrixInverse(nullptr, previousParent) * parent);
    }
} // 名前空間 TransformMath の終わり。
