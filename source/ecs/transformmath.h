#pragma once
#include <DirectXMath.h>

// ワールド行列の共通実装。
// 従来の Scaling * RotationX * RotationY * RotationZ * Translation（行列生成5回＋4x4乗算4回）と
// 同じ回転順（X→Y→Z、行ベクトル）を、sin/cos 3組からの解析式で直接組み立てる。
// 以前は同じ式が TransformSystem / Light / Sun / エディター / 物理 の5か所に重複していた。
namespace TransformMath
{
	inline DirectX::XMMATRIX RotationXYZ(const DirectX::XMFLOAT3& rotation)
	{
		float sx, cx, sy, cy, sz, cz;
		DirectX::XMScalarSinCos(&sx, &cx, rotation.x);
		DirectX::XMScalarSinCos(&sy, &cy, rotation.y);
		DirectX::XMScalarSinCos(&sz, &cz, rotation.z);
		const float sxsy = sx * sy;
		const float cxsy = cx * sy;
		return DirectX::XMMATRIX(
			cy * cz,               cy * sz,               -sy,     0.0f,
			sxsy * cz - cx * sz,   sxsy * sz + cx * cz,   sx * cy, 0.0f,
			cxsy * cz + sx * sz,   cxsy * sz - sx * cz,   cx * cy, 0.0f,
			0.0f,                  0.0f,                  0.0f,    1.0f);
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

	template<typename TransformT>
	inline DirectX::XMMATRIX World(const TransformT& transform)
	{
		return World(transform.Position, transform.Rotation, transform.Scale);
	}
}

