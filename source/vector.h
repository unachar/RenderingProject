#pragma once
#include <cmath>

class Vector3
{
public:
	float x, y, z;

	Vector3() = default;
	Vector3(const Vector3& a) :x(a.x), y(a.y), z(a.z) {};

	Vector3(float nx, float ny, float nz) :x(nx), y(ny), z(nz) {};

	Vector3& operator = (const Vector3& a) {
		x = a.x;
		y = a.y;
		z = a.z;
		return *this;
	}

	Vector3& operator = (float a) 
	{
		x = a;
		y = a;
		z = a;
		return *this;
	}

	Vector3& operator = (XMFLOAT3 a) 
	{
		x = a.x;
		y = a.y;
		z = a.z;
		return *this;
	}

	operator DirectX::XMFLOAT3() const
	{
		return DirectX::XMFLOAT3(x, y, z);
	}

	bool operator ==(const Vector3& a)const {
		return x == a.x && y == a.y && z == a.z;
	}

	bool operator != (const Vector3& a)const {
		return x != a.x || y != a.y || z != a.z;
	}

	Vector3 operator - () const { return Vector3(-x, -y, -z); }

	Vector3 operator +(const Vector3& a) const {
		return Vector3(x + a.x, y + a.y, z + a.z);
	}

	Vector3 operator +(const float& a)const {
		return Vector3(x + a, y + a, z + a);
	}

	Vector3 operator -(const Vector3& a)const {
		return Vector3(x - a.x, y - a.y, z - a.z);
	}

	Vector3 operator *(float a)const {
		return Vector3(x * a, y * a, z * a);
	}

	Vector3 operator /(float a)const {
		float OneOverA = 1.f / a;
		return Vector3(x * OneOverA, y * OneOverA, z * OneOverA);
	}

	Vector3 operator /(Vector3& a)const {
		return Vector3(x / a.x, y / a.y, z / a.z);
	}

	Vector3 operator +=(const Vector3& a) {
		x += a.x; y += a.y; z += a.z;
		return *this;
	}

	Vector3 operator +=(float a) {
		x += a; y += a; z += a;
		return *this;
	}

	Vector3 operator -=(const Vector3& a) {
		x -= a.x; y -= a.y; z -= a.z;
		return *this;
	}

	Vector3 operator *=(const Vector3& a) {
		x *= a.x; y *= a.y; z *= a.z;
		return *this;
	}

	Vector3 operator *= (const float& a) {
		return Vector3(x *= a, y *= a, z *= a);
	}

	Vector3 operator /=(const Vector3& a) {
		x /= a.x; y /= a.y; z /= a.z;
		return *this;
	}

	void Normalize() {
		float magsq = x * x + y * y + z * z;
		if (magsq > 0.f)
		{
			float oneOverMag = 1.f / sqrtf(magsq);
			x *= oneOverMag;
			y *= oneOverMag;
			z *= oneOverMag;
		}
	}

	float Length()const
	{
		return sqrtf(x * x + y * y + z * z); 
	}

	float operator *(const Vector3& a) {
		return x * a.x + y * a.y + z * a.z;
	}

	Vector3 Lerp(const Vector3& a, const Vector3& b, float t)
	{
		return a * (1.0f - t) + b * t;
	}

	static float dot(const Vector3& a, const Vector3& b)
	{
		return a.x * b.x + a.y * b.y + a.z * b.z;
	}

	static Vector3 cross(const Vector3& a, const Vector3& b)
	{
		Vector3 ret{};
		ret.x = a.y * b.z - a.z * b.y;
		ret.y = a.z * b.x - a.x * b.z;
		ret.z = a.x * b.y - a.y * b.x;
		return ret;
	}

	static float Distance(const Vector3& a, const Vector3& b)
	{
		float dx = a.x - b.x;
		float dy = a.y - b.y;
		float dz = a.z - b.z;
		return sqrtf(dx * dx + dy * dy + dz * dz);
	}

	static float DistanceSquared(const Vector3& a, const Vector3& b)
	{
		float dx = a.x - b.x;
		float dy = a.y - b.y;
		float dz = a.z - b.z;
		return dx * dx + dy * dy + dz * dz;
	}

	static const Vector3 Up() { return Vector3(0.f, 1.f, 0.f); }
	static const Vector3 Down() { return Vector3(0.f, -1.f, 0.f); }
	static const Vector3 Left() { return Vector3(-1.f, 0.f, 0.f); }
	static const Vector3 Right() { return Vector3(1.f, 0.f, 0.f); }
	static const Vector3 Forward() { return Vector3(0.f, 0.f, 1.f); }
	static const Vector3 Backward() { return Vector3(0.f, 0.f, -1.f); }
	static const Vector3 One() { return Vector3(1.f, 1.f, 1.f); }
	static const Vector3 Zero() { return Vector3(0.f, 0.f, 0.f); }
};

