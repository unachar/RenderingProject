#pragma once

#include <DirectXMath.h>
#include <cstdint>

enum class PhysicsEngine : int
{
	Bullet = 0,
	Jolt = 1,
	PhysX = 2,
};

enum class PhysicsBodyType : int
{
	Static = 0,
	Dynamic = 1,
	Kinematic = 2,
};

enum class PhysicsShape : int
{
	Box = 0,
	Sphere = 1,
	Capsule = 2,
	Mesh = 3,
};

enum class PhysicsColliderRole : int
{
	Default = 0,
	Floor = 1,
	Wall = 2,
	Obstacle = 3,
};



struct PhysicsComponent
{
	bool UsePhysics = false;
	bool UsePhysicsBone = false;
	PhysicsEngine UsePhysicsEngine = PhysicsEngine::Bullet;
	PhysicsBodyType BodyType = PhysicsBodyType::Dynamic;
	PhysicsShape Shape = PhysicsShape::Box;
	PhysicsColliderRole ColliderRole = PhysicsColliderRole::Default;

	DirectX::XMFLOAT3 ColliderCenter = { 0.0f, 0.0f, 0.0f };
	DirectX::XMFLOAT3 ColliderSize = { 1.0f, 1.0f, 1.0f };
	float SphereRadius = 0.5f;
	float CapsuleRadius = 0.5f;
	float CapsuleHeight = 1.0f;

	DirectX::XMFLOAT3 Velocity = { 0.0f, 0.0f, 0.0f };
	DirectX::XMFLOAT3 AngularVelocity = { 0.0f, 0.0f, 0.0f };
	float Mass = 1.0f;
	float Friction = 0.5f;
	float Restitution = 0.0f;
	float LinearDamping = 0.05f;
	float AngularDamping = 0.05f;
	float GravityFactor = 1.0f;
	bool UseGravity = true;
	bool EnableContinuousCollision = false;
	bool AllowSleeping = true;


	float PmxMassScale = 1.0f;
	float PmxDampingScale = 1.0f;
	float PmxStiffnessScale = 1.0f;
	float PmxColliderScale = 1.0f;

	uint16_t CollisionLayer = 0;
	uint16_t CollisionMask = 0xffff;




	uint64_t SettingsRevision = 1;
};
