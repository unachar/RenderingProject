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
};

// Engine-independent rigid body settings. Runtime-only engine objects are owned
// by PhysicsSystem so the component remains safe to copy for editor undo/redo.
struct PhysicsComponent
{
	bool UsePhysics = false;
	bool UsePhysicsBone = false;
	PhysicsEngine UsePhysicsEngine = PhysicsEngine::Bullet;
	PhysicsBodyType BodyType = PhysicsBodyType::Dynamic;
	PhysicsShape Shape = PhysicsShape::Box;

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

	// Multipliers applied to PMX-authored rigid body / joint parameters.
	float PmxMassScale = 1.0f;
	float PmxDampingScale = 1.0f;
	float PmxStiffnessScale = 1.0f;
	float PmxColliderScale = 1.0f;

	uint16_t CollisionLayer = 0;
	uint16_t CollisionMask = 0xffff;

	// Incremented by the inspector and may also be changed by gameplay code.
	// PhysicsSystem additionally compares all settings, so direct field edits
	// still cause the runtime body to be rebuilt.
	uint64_t SettingsRevision = 1;
};
