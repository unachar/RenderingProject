#pragma once

#include "physicscomponent.h"
#include <DirectXMath.h>
#include <cstdint>
#include <memory>

using PhysicsBodyHandle = uint64_t;
using PhysicsJointHandle = uint64_t;
static constexpr PhysicsBodyHandle kInvalidPhysicsBody = 0;
static constexpr PhysicsJointHandle kInvalidPhysicsJoint = 0;

struct PhysicsTransform
{
	DirectX::XMFLOAT3 Position = { 0.0f, 0.0f, 0.0f };
	DirectX::XMFLOAT4 Rotation = { 0.0f, 0.0f, 0.0f, 1.0f };
};

struct PhysicsBodyDesc
{
	PhysicsBodyType BodyType = PhysicsBodyType::Dynamic;
	PhysicsShape Shape = PhysicsShape::Box;
	PhysicsTransform Transform{};
	DirectX::XMFLOAT3 HalfExtent = { 0.5f, 0.5f, 0.5f };
	float Radius = 0.5f;
	float Height = 1.0f;
	float Mass = 1.0f;
	float Friction = 0.5f;
	float Restitution = 0.0f;
	float LinearDamping = 0.05f;
	float AngularDamping = 0.05f;
	float GravityFactor = 1.0f;
	bool ContinuousCollision = false;
	bool AllowSleeping = true;
	uint16_t CollisionLayer = 0;
	uint16_t CollisionMask = 0xffff;
	uint64_t UserData = 0;
};

struct PhysicsJointDesc
{
	PhysicsBodyHandle BodyA = kInvalidPhysicsBody;
	PhysicsBodyHandle BodyB = kInvalidPhysicsBody;
	PhysicsTransform Frame{};
	DirectX::XMFLOAT3 LinearLimitMin{};
	DirectX::XMFLOAT3 LinearLimitMax{};
	DirectX::XMFLOAT3 AngularLimitMin{};
	DirectX::XMFLOAT3 AngularLimitMax{};
	DirectX::XMFLOAT3 LinearSpring{};
	DirectX::XMFLOAT3 AngularSpring{};
};

class IPhysicsBackend
{
public:
	virtual ~IPhysicsBackend() = default;
	virtual const char* GetName() const = 0;
	virtual bool Init() = 0;
	virtual void Shutdown() = 0;
	virtual bool IsAvailable() const = 0;
	virtual void SetGravity(const DirectX::XMFLOAT3& gravity) = 0;
	virtual void Step(float fixedDeltaTime) = 0;

	virtual PhysicsBodyHandle CreateBody(const PhysicsBodyDesc& desc) = 0;
	virtual void DestroyBody(PhysicsBodyHandle body) = 0;
	virtual bool GetBodyTransform(PhysicsBodyHandle body, PhysicsTransform& transform) const = 0;
	virtual void SetBodyTransform(PhysicsBodyHandle body, const PhysicsTransform& transform, bool activate) = 0;
	virtual void SetBodyVelocity(
		PhysicsBodyHandle body,
		const DirectX::XMFLOAT3& linearVelocity,
		const DirectX::XMFLOAT3& angularVelocity) = 0;

	virtual PhysicsJointHandle CreateJoint(const PhysicsJointDesc& desc) = 0;
	virtual void DestroyJoint(PhysicsJointHandle joint) = 0;
};

std::unique_ptr<IPhysicsBackend> CreateBulletPhysicsBackend();
std::unique_ptr<IPhysicsBackend> CreateJoltPhysicsBackend();
std::unique_ptr<IPhysicsBackend> CreatePhysXPhysicsBackend();

