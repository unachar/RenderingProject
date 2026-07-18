#include "pch.h"
#include "physicssystem.h"
#include "componentmanager.h"
#include "projectmanager.h"
#include "world.h"
#include "modelmanager.h"
#include "animationmodel.h"
#include <bit>
#include <unordered_set>

namespace
{
	size_t EngineIndex(PhysicsEngine engine)
	{
		const int value = static_cast<int>(engine);
		return value >= 0 && value < 3 ? static_cast<size_t>(value) : 0;
	}

	void HashCombine(uint64_t& hash, uint64_t value)
	{
		hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
	}

	void HashFloat(uint64_t& hash, float value)
	{
		HashCombine(hash, std::bit_cast<uint32_t>(value));
	}

	void HashFloat3(uint64_t& hash, const XMFLOAT3& value)
	{
		HashFloat(hash, value.x);
		HashFloat(hash, value.y);
		HashFloat(hash, value.z);
	}

	uint64_t GetSettingsHash(const PhysicsComponent& physics, const TransformComponent& transform)
	{
		uint64_t hash = 1469598103934665603ull;
		HashCombine(hash, physics.SettingsRevision);
		HashCombine(hash, static_cast<uint64_t>(physics.UsePhysics));
		HashCombine(hash, static_cast<uint64_t>(physics.UsePhysicsBone));
		HashCombine(hash, static_cast<uint64_t>(physics.UsePhysicsEngine));
		HashCombine(hash, static_cast<uint64_t>(physics.BodyType));
		HashCombine(hash, static_cast<uint64_t>(physics.Shape));
		HashFloat3(hash, physics.ColliderCenter);
		HashFloat3(hash, physics.ColliderSize);
		HashFloat(hash, physics.SphereRadius);
		HashFloat(hash, physics.CapsuleRadius);
		HashFloat(hash, physics.CapsuleHeight);
		HashFloat3(hash, physics.Velocity);
		HashFloat3(hash, physics.AngularVelocity);
		HashFloat(hash, physics.Mass);
		HashFloat(hash, physics.Friction);
		HashFloat(hash, physics.Restitution);
		HashFloat(hash, physics.LinearDamping);
		HashFloat(hash, physics.AngularDamping);
		HashFloat(hash, physics.GravityFactor);
		HashCombine(hash, physics.UseGravity);
		HashCombine(hash, physics.EnableContinuousCollision);
		HashCombine(hash, physics.AllowSleeping);
		HashFloat(hash, physics.PmxMassScale);
		HashFloat(hash, physics.PmxDampingScale);
		HashFloat(hash, physics.PmxStiffnessScale);
		HashFloat(hash, physics.PmxColliderScale);
		HashCombine(hash, physics.CollisionLayer);
		HashCombine(hash, physics.CollisionMask);
		HashFloat3(hash, transform.Scale);
		return hash;
	}

	XMMATRIX BuildEntityWorld(const TransformComponent& transform)
	{
		return XMMatrixScaling(transform.Scale.x, transform.Scale.y, transform.Scale.z) *
			XMMatrixRotationX(transform.Rotation.x) *
			XMMatrixRotationY(transform.Rotation.y) *
			XMMatrixRotationZ(transform.Rotation.z) *
			XMMatrixTranslation(transform.Position.x, transform.Position.y, transform.Position.z);
	}

	XMMATRIX BuildEntityRotation(const TransformComponent& transform)
	{
		return XMMatrixRotationX(transform.Rotation.x) *
			XMMatrixRotationY(transform.Rotation.y) *
			XMMatrixRotationZ(transform.Rotation.z);
	}

	PhysicsTransform DecomposePhysicsTransform(FXMMATRIX matrix)
	{
		XMVECTOR scale{};
		XMVECTOR rotation{};
		XMVECTOR translation{};
		PhysicsTransform result{};
		if (XMMatrixDecompose(&scale, &rotation, &translation, matrix))
		{
			XMStoreFloat3(&result.Position, translation);
			XMStoreFloat4(&result.Rotation, XMQuaternionNormalize(rotation));
		}
		return result;
	}

	XMMATRIX ConvertPhysicsWorldToModel(
		const PhysicsTransform& bodyTransform,
		const TransformComponent& entityTransform)
	{
		XMVECTOR determinant{};
		const XMMATRIX entityWorldInverse =
			XMMatrixInverse(&determinant, BuildEntityWorld(entityTransform));
		const XMVECTOR modelPosition = XMVector3TransformCoord(
			XMLoadFloat3(&bodyTransform.Position),
			entityWorldInverse);

		// Entity scale affects a rigid body's position and collider dimensions,
		// but it must never be written into the skeleton. Multiplying the whole
		// body matrix by entityWorldInverse also applies inverse scale to the
		// rotation basis, which makes physics-driven bones grow at small entity
		// scales and shrink at large scales.
		const XMMATRIX entityRotationInverse =
			XMMatrixInverse(&determinant, BuildEntityRotation(entityTransform));
		const XMMATRIX modelRotation =
			XMMatrixRotationQuaternion(XMLoadFloat4(&bodyTransform.Rotation)) *
			entityRotationInverse;

		XMFLOAT3 position{};
		XMStoreFloat3(&position, modelPosition);
		return modelRotation *
			XMMatrixTranslation(position.x, position.y, position.z);
	}

	XMMATRIX BuildPmxTransform(const XMFLOAT3& rotation, const XMFLOAT3& position)
	{
		return XMMatrixRotationX(rotation.x) *
			XMMatrixRotationY(rotation.y) *
			XMMatrixRotationZ(rotation.z) *
			XMMatrixTranslation(position.x, position.y, position.z);
	}

	XMFLOAT3 AbsoluteScale(const TransformComponent& transform)
	{
		return {
			max(fabsf(transform.Scale.x), 0.0001f),
			max(fabsf(transform.Scale.y), 0.0001f),
			max(fabsf(transform.Scale.z), 0.0001f)
		};
	}

	void SetEntityFromBodyTransform(
		TransformComponent& transform,
		const PhysicsComponent& physics,
		const PhysicsTransform& bodyTransform)
	{
		XMMATRIX rotationMatrix = XMMatrixRotationQuaternion(XMLoadFloat4(&bodyTransform.Rotation));
		const XMFLOAT3 scale = AbsoluteScale(transform);
		XMVECTOR localCenter = XMVectorSet(
			physics.ColliderCenter.x * scale.x,
			physics.ColliderCenter.y * scale.y,
			physics.ColliderCenter.z * scale.z,
			0.0f);
		XMFLOAT3 rotatedCenter{};
		XMStoreFloat3(&rotatedCenter, XMVector3TransformNormal(localCenter, rotationMatrix));
		transform.Position = {
			bodyTransform.Position.x - rotatedCenter.x,
			bodyTransform.Position.y - rotatedCenter.y,
			bodyTransform.Position.z - rotatedCenter.z
		};

		XMFLOAT4X4 r{};
		XMStoreFloat4x4(&r, rotationMatrix);
		const float sy = clamp(-r._13, -1.0f, 1.0f);
		transform.Rotation.y = asinf(sy);
		const float cy = cosf(transform.Rotation.y);
		if (fabsf(cy) > 0.00001f)
		{
			transform.Rotation.x = atan2f(r._23, r._33);
			transform.Rotation.z = atan2f(r._12, r._11);
		}
		else
		{
			transform.Rotation.x = atan2f(-r._32, r._22);
			transform.Rotation.z = 0.0f;
		}
		transform.IsDirty = true;
	}
}

IPhysicsBackend* PhysicsSystem::GetBackend(PhysicsEngine engine) const
{
	return m_Backends[EngineIndex(engine)].get();
}

void PhysicsSystem::Init()
{
	m_Backends[0] = CreateBulletPhysicsBackend();
	m_Backends[1] = CreateJoltPhysicsBackend();
	m_Backends[2] = CreatePhysXPhysicsBackend();
	for (auto& backend : m_Backends)
	{
		if (!backend || !backend->Init())
		{
			if (backend)
			{
				Debug::Log("ERROR: %s physics backend failed to initialize\n", backend->GetName());
			}
			continue;
		}
		backend->SetGravity(m_Settings.Gravity);
		Debug::Log("%s physics backend initialized\n", backend->GetName());
	}
}

void PhysicsSystem::Uninit()
{
	ClearRuntime();
	for (auto it = m_Backends.rbegin(); it != m_Backends.rend(); ++it)
	{
		if (*it)
		{
			(*it)->Shutdown();
			it->reset();
		}
	}
}

void PhysicsSystem::ClearRuntime()
{
	vector<EntityID> rigs;
	rigs.reserve(m_BoneRigs.size());
	for (const auto& pair : m_BoneRigs) rigs.push_back(pair.first);
	for (EntityID entity : rigs) DestroyBoneRig(entity);
	vector<EntityID> entities;
	entities.reserve(m_EntityBodies.size());
	for (const auto& pair : m_EntityBodies) entities.push_back(pair.first);
	for (EntityID entity : entities) DestroyEntityBody(entity);
	m_Accumulator = 0.0;
	m_LastSubStepCount = 0;
}

void PhysicsSystem::DestroyEntityBody(EntityID entity)
{
	auto it = m_EntityBodies.find(entity);
	if (it == m_EntityBodies.end())
	{
		return;
	}
	if (IPhysicsBackend* backend = GetBackend(it->second.Engine))
	{
		backend->DestroyBody(it->second.Body);
	}
	m_EntityBodies.erase(it);
}

void PhysicsSystem::DestroyBoneRig(EntityID entity)
{
	auto it = m_BoneRigs.find(entity);
	if (it == m_BoneRigs.end())
	{
		return;
	}
	if (IPhysicsBackend* backend = GetBackend(it->second.Engine))
	{
		for (PhysicsJointHandle joint : it->second.Joints)
		{
			backend->DestroyJoint(joint);
		}
		for (const BoneBodyRuntime& body : it->second.Bodies)
		{
			backend->DestroyBody(body.Body);
		}
	}
	m_BoneRigs.erase(it);
}

void PhysicsSystem::CreateEntityBody(EntityID entity)
{
	if (!ComponentManager::HasComponent<PhysicsComponent>(entity) ||
		!ComponentManager::HasComponent<TransformComponent>(entity))
	{
		return;
	}
	const auto& physics = ComponentManager::GetComponentUnchecked<PhysicsComponent>(entity);
	const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
	IPhysicsBackend* backend = GetBackend(physics.UsePhysicsEngine);
	if (!backend || !backend->IsAvailable())
	{
		return;
	}

	const XMFLOAT3 scale = AbsoluteScale(transform);
	PhysicsBodyDesc desc{};
	desc.BodyType = physics.BodyType;
	desc.Shape = physics.Shape;
	const XMMATRIX world = BuildEntityWorld(transform);
	XMVECTOR center = XMVector3Transform(
		XMLoadFloat3(&physics.ColliderCenter), world);
	XMStoreFloat3(&desc.Transform.Position, center);
	XMVECTOR rotation = XMQuaternionRotationRollPitchYaw(
		transform.Rotation.x, transform.Rotation.y, transform.Rotation.z);
	XMStoreFloat4(&desc.Transform.Rotation, rotation);
	desc.HalfExtent = {
		max(fabsf(physics.ColliderSize.x) * scale.x * 0.5f, 0.001f),
		max(fabsf(physics.ColliderSize.y) * scale.y * 0.5f, 0.001f),
		max(fabsf(physics.ColliderSize.z) * scale.z * 0.5f, 0.001f)
	};
	const float radius = physics.Shape == PhysicsShape::Sphere
		? physics.SphereRadius : physics.CapsuleRadius;
	desc.Radius = max(radius * max(scale.x, scale.z), 0.001f);
	desc.Height = max(physics.CapsuleHeight * scale.y, 0.001f);
	desc.Mass = physics.Mass;
	desc.Friction = physics.Friction;
	desc.Restitution = physics.Restitution;
	desc.LinearDamping = physics.LinearDamping;
	desc.AngularDamping = physics.AngularDamping;
	desc.GravityFactor = physics.UseGravity ? physics.GravityFactor : 0.0f;
	desc.ContinuousCollision = physics.EnableContinuousCollision;
	desc.AllowSleeping = physics.AllowSleeping;
	desc.CollisionLayer = physics.CollisionLayer;
	desc.CollisionMask = physics.CollisionMask;
	desc.UserData = entity;
	const PhysicsBodyHandle body = backend->CreateBody(desc);
	if (body == kInvalidPhysicsBody)
	{
		return;
	}
	backend->SetBodyVelocity(body, physics.Velocity, physics.AngularVelocity);
	m_EntityBodies[entity] = {
		physics.UsePhysicsEngine,
		body,
		GetSettingsHash(physics, transform)
	};
}

void PhysicsSystem::CreateBoneRig(EntityID entity)
{
	if (!ComponentManager::HasComponent<PhysicsComponent>(entity) ||
		!ComponentManager::HasComponent<TransformComponent>(entity) ||
		!ComponentManager::HasComponent<AnimationModelComponent>(entity))
	{
		return;
	}
	const auto& physics = ComponentManager::GetComponentUnchecked<PhysicsComponent>(entity);
	const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
	const auto& animation = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity);
	AnimationModelResource* model = ModelManager::GetAnimModel(animation.ModelId);
	IPhysicsBackend* backend = GetBackend(physics.UsePhysicsEngine);
	if (!model || !model->HasPmxPhysics() || !backend || !backend->IsAvailable())
	{
		return;
	}

	BoneRigRuntime rig{};
	rig.Engine = physics.UsePhysicsEngine;
	rig.ModelId = animation.ModelId;
	rig.SettingsHash = GetSettingsHash(physics, transform);
	const XMMATRIX entityWorld = BuildEntityWorld(transform);
	const XMFLOAT3 scale = AbsoluteScale(transform);
	const float uniformScale = max(scale.x, max(scale.y, scale.z)) *
		max(physics.PmxColliderScale, 0.001f);

	const auto& rigidBodies = model->GetPmxRigidBodies();
	rig.Bodies.reserve(rigidBodies.size());
	for (size_t bodyIndex = 0; bodyIndex < rigidBodies.size(); ++bodyIndex)
	{
		const PmxRigidBodyData& pmx = rigidBodies[bodyIndex];
		const XMMATRIX rigidModel = BuildPmxTransform(pmx.Rotation, pmx.Position);
		XMMATRIX boneGlobal = XMMatrixIdentity();
		XMFLOAT4X4 storedBone{};
		const bool hasBone = !pmx.BoneName.empty() &&
			model->GetBoneBindGlobalTransform(pmx.BoneName, storedBone);
		if (hasBone)
		{
			boneGlobal = XMLoadFloat4x4(&storedBone);
		}
		XMVECTOR determinant{};
		const XMMATRIX offset = hasBone
			? XMMatrixMultiply(rigidModel, XMMatrixInverse(&determinant, boneGlobal))
			: rigidModel;
		const XMMATRIX inverseOffset = XMMatrixInverse(&determinant, offset);

		PhysicsBodyDesc desc{};
		desc.BodyType = pmx.Operation == 0
			? PhysicsBodyType::Kinematic : PhysicsBodyType::Dynamic;
		desc.Shape = pmx.Shape == 0
			? PhysicsShape::Sphere
			: (pmx.Shape == 2 ? PhysicsShape::Capsule : PhysicsShape::Box);
		desc.Transform = DecomposePhysicsTransform(XMMatrixMultiply(rigidModel, entityWorld));
		desc.HalfExtent = {
			max(fabsf(pmx.Size.x) * scale.x * physics.PmxColliderScale, 0.001f),
			max(fabsf(pmx.Size.y) * scale.y * physics.PmxColliderScale, 0.001f),
			max(fabsf(pmx.Size.z) * scale.z * physics.PmxColliderScale, 0.001f)
		};
		desc.Radius = max(fabsf(pmx.Size.x) * uniformScale, 0.001f);
		desc.Height = max(fabsf(pmx.Size.y) * uniformScale, 0.001f);
		desc.Mass = max(pmx.Mass * physics.PmxMassScale, 0.0001f);
		desc.Friction = pmx.Friction;
		desc.Restitution = pmx.Restitution;
		desc.LinearDamping = pmx.LinearDamping * physics.PmxDampingScale;
		desc.AngularDamping = pmx.AngularDamping * physics.PmxDampingScale;
		desc.GravityFactor = physics.UseGravity ? physics.GravityFactor : 0.0f;
		desc.ContinuousCollision = physics.EnableContinuousCollision;
		desc.AllowSleeping = physics.AllowSleeping;
		desc.CollisionLayer = pmx.CollisionGroup;
		desc.CollisionMask = static_cast<uint16_t>(~pmx.CollisionMask);
		desc.UserData = (static_cast<uint64_t>(entity) << 32) | bodyIndex;

		BoneBodyRuntime runtime{};
		runtime.Body = backend->CreateBody(desc);
		runtime.BoneName = pmx.BoneName;
		runtime.Operation = pmx.Operation;
		XMStoreFloat4x4(&runtime.Offset, offset);
		XMStoreFloat4x4(&runtime.InverseOffset, inverseOffset);
		rig.Bodies.push_back(std::move(runtime));
	}

	const auto& joints = model->GetPmxJoints();
	rig.Joints.reserve(joints.size());
	for (const PmxJointData& pmx : joints)
	{
		if (pmx.RigidBodyA < 0 || pmx.RigidBodyB < 0 ||
			pmx.RigidBodyA >= static_cast<int32_t>(rig.Bodies.size()) ||
			pmx.RigidBodyB >= static_cast<int32_t>(rig.Bodies.size()))
		{
			continue;
		}
		const PhysicsBodyHandle bodyA = rig.Bodies[static_cast<size_t>(pmx.RigidBodyA)].Body;
		const PhysicsBodyHandle bodyB = rig.Bodies[static_cast<size_t>(pmx.RigidBodyB)].Body;
		if (bodyA == kInvalidPhysicsBody || bodyB == kInvalidPhysicsBody)
		{
			continue;
		}
		PhysicsJointDesc desc{};
		desc.BodyA = bodyA;
		desc.BodyB = bodyB;
		desc.Frame = DecomposePhysicsTransform(
			XMMatrixMultiply(BuildPmxTransform(pmx.Rotation, pmx.Position), entityWorld));
		desc.LinearLimitMin = {
			pmx.LinearLimitMin.x * scale.x,
			pmx.LinearLimitMin.y * scale.y,
			pmx.LinearLimitMin.z * scale.z
		};
		desc.LinearLimitMax = {
			pmx.LinearLimitMax.x * scale.x,
			pmx.LinearLimitMax.y * scale.y,
			pmx.LinearLimitMax.z * scale.z
		};
		desc.AngularLimitMin = pmx.AngularLimitMin;
		desc.AngularLimitMax = pmx.AngularLimitMax;
		desc.LinearSpring = {
			pmx.LinearSpring.x * physics.PmxStiffnessScale,
			pmx.LinearSpring.y * physics.PmxStiffnessScale,
			pmx.LinearSpring.z * physics.PmxStiffnessScale
		};
		desc.AngularSpring = {
			pmx.AngularSpring.x * physics.PmxStiffnessScale,
			pmx.AngularSpring.y * physics.PmxStiffnessScale,
			pmx.AngularSpring.z * physics.PmxStiffnessScale
		};
		const PhysicsJointHandle joint = backend->CreateJoint(desc);
		if (joint != kInvalidPhysicsJoint)
		{
			rig.Joints.push_back(joint);
		}
	}
	m_BoneRigs[entity] = std::move(rig);
}

void PhysicsSystem::SynchronizeRuntimeObjects()
{
	vector<EntityID> staleBodies;
	for (const auto& [entity, runtime] : m_EntityBodies)
	{
		if (!Registry::IsAlive(entity) ||
			!ComponentManager::HasComponent<PhysicsComponent>(entity) ||
			!ComponentManager::HasComponent<TransformComponent>(entity))
		{
			staleBodies.push_back(entity);
			continue;
		}
		const auto& physics = ComponentManager::GetComponentUnchecked<PhysicsComponent>(entity);
		const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
		if (!physics.UsePhysics || physics.UsePhysicsBone ||
			runtime.Engine != physics.UsePhysicsEngine ||
			runtime.SettingsHash != GetSettingsHash(physics, transform))
		{
			staleBodies.push_back(entity);
		}
	}
	for (EntityID entity : staleBodies) DestroyEntityBody(entity);

	vector<EntityID> staleRigs;
	for (const auto& [entity, runtime] : m_BoneRigs)
	{
		if (!Registry::IsAlive(entity) ||
			!ComponentManager::HasComponent<PhysicsComponent>(entity) ||
			!ComponentManager::HasComponent<TransformComponent>(entity) ||
			!ComponentManager::HasComponent<AnimationModelComponent>(entity))
		{
			staleRigs.push_back(entity);
			continue;
		}
		const auto& physics = ComponentManager::GetComponentUnchecked<PhysicsComponent>(entity);
		const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
		const auto& animation = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity);
		if (!physics.UsePhysics || !physics.UsePhysicsBone ||
			runtime.Engine != physics.UsePhysicsEngine ||
			runtime.ModelId != animation.ModelId ||
			runtime.SettingsHash != GetSettingsHash(physics, transform))
		{
			staleRigs.push_back(entity);
		}
	}
	for (EntityID entity : staleRigs) DestroyBoneRig(entity);

	ComponentManager::ForEach<PhysicsComponent, TransformComponent>(
		[&](EntityID entity, PhysicsComponent& physics, TransformComponent&)
		{
			if (!physics.UsePhysics)
			{
				return;
			}
			if (physics.UsePhysicsBone &&
				ComponentManager::HasComponent<AnimationModelComponent>(entity))
			{
				if (!m_BoneRigs.contains(entity))
				{
					CreateBoneRig(entity);
				}
			}
			else if (!m_EntityBodies.contains(entity))
			{
				CreateEntityBody(entity);
			}
		});
}

void PhysicsSystem::SynchronizeKinematicBodies()
{
	for (const auto& [entity, runtime] : m_EntityBodies)
	{
		if (!Registry::IsAlive(entity))
		{
			continue;
		}
		const auto& physics = ComponentManager::GetComponentUnchecked<PhysicsComponent>(entity);
		if (physics.BodyType != PhysicsBodyType::Kinematic)
		{
			continue;
		}
		const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
		const XMMATRIX world = BuildEntityWorld(transform);
		PhysicsTransform pose{};
		XMStoreFloat3(
			&pose.Position,
			XMVector3Transform(XMLoadFloat3(&physics.ColliderCenter), world));
		XMStoreFloat4(
			&pose.Rotation,
			XMQuaternionRotationRollPitchYaw(
				transform.Rotation.x, transform.Rotation.y, transform.Rotation.z));
		if (IPhysicsBackend* backend = GetBackend(runtime.Engine))
		{
			backend->SetBodyTransform(runtime.Body, pose, false);
		}
	}

	for (const auto& [entity, rig] : m_BoneRigs)
	{
		if (!Registry::IsAlive(entity))
		{
			continue;
		}
		const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
		AnimationModelResource* model = ModelManager::GetAnimModel(rig.ModelId);
		IPhysicsBackend* backend = GetBackend(rig.Engine);
		if (!model || !backend)
		{
			continue;
		}
		const XMMATRIX entityWorld = BuildEntityWorld(transform);
		for (const BoneBodyRuntime& body : rig.Bodies)
		{
			if (body.Operation != 0 || body.Body == kInvalidPhysicsBody)
			{
				continue;
			}
			XMFLOAT4X4 boneStored{};
			if (body.BoneName.empty() ||
				!model->GetBoneGlobalTransform(body.BoneName, boneStored))
			{
				continue;
			}
			const XMMATRIX bodyWorld = XMLoadFloat4x4(&body.Offset) *
				XMLoadFloat4x4(&boneStored) *
				entityWorld;
			backend->SetBodyTransform(body.Body, DecomposePhysicsTransform(bodyWorld), false);
		}
	}
}

void PhysicsSystem::ApplySimulationResults()
{
	++m_PoseApplySerial;
	for (const auto& [entity, runtime] : m_EntityBodies)
	{
		if (!Registry::IsAlive(entity))
		{
			continue;
		}
		const auto& physics = ComponentManager::GetComponentUnchecked<PhysicsComponent>(entity);
		if (physics.BodyType != PhysicsBodyType::Dynamic)
		{
			continue;
		}
		PhysicsTransform pose{};
		IPhysicsBackend* backend = GetBackend(runtime.Engine);
		if (backend && backend->GetBodyTransform(runtime.Body, pose))
		{
			auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
			SetEntityFromBodyTransform(transform, physics, pose);
		}
	}

	for (const auto& [entity, rig] : m_BoneRigs)
	{
		if (!Registry::IsAlive(entity))
		{
			continue;
		}
		const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
		AnimationModelResource* model = ModelManager::GetAnimModel(rig.ModelId);
		IPhysicsBackend* backend = GetBackend(rig.Engine);
		if (!model || !backend)
		{
			continue;
		}
		bool poseChanged = false;
		for (const BoneBodyRuntime& body : rig.Bodies)
		{
			if (body.Operation == 0 || body.Body == kInvalidPhysicsBody || body.BoneName.empty())
			{
				continue;
			}
			PhysicsTransform bodyPose{};
			if (!backend->GetBodyTransform(body.Body, bodyPose))
			{
				continue;
			}
			const XMMATRIX modelRigid =
				ConvertPhysicsWorldToModel(bodyPose, transform);
			const XMMATRIX boneGlobal = XMLoadFloat4x4(&body.InverseOffset) * modelRigid;
			XMFLOAT4X4 stored{};
			XMStoreFloat4x4(&stored, boneGlobal);
			poseChanged |= model->SetBoneGlobalTransform(
				body.BoneName, stored, body.Operation == 2);
		}
		if (poseChanged)
		{
			model->CommitPhysicsPose();
		}
	}
}

void PhysicsSystem::StepFixed(float fixedDeltaTime)
{
	SynchronizeKinematicBodies();
	for (auto& backend : m_Backends)
	{
		if (backend && backend->IsAvailable())
		{
			backend->Step(fixedDeltaTime);
		}
	}
}

void PhysicsSystem::Update()
{
	if (!ProjectManager::IsPlaying())
	{
		if (!m_EntityBodies.empty() || !m_BoneRigs.empty())
		{
			ClearRuntime();
		}
		m_PlaySession = ProjectManager::GetPlaySession();
		return;
	}
	if (m_PlaySession != ProjectManager::GetPlaySession())
	{
		ClearRuntime();
		m_PlaySession = ProjectManager::GetPlaySession();
		for (auto& backend : m_Backends)
		{
			if (backend && backend->IsAvailable())
			{
				backend->SetGravity(m_Settings.Gravity);
			}
		}
	}
	SynchronizeRuntimeObjects();
	if (ProjectManager::IsPaused())
	{
		m_LastSubStepCount = 0;
		return;
	}

	const float fixedDelta = clamp(m_Settings.FixedTimeStep, 1.0f / 1000.0f, 1.0f);
	const int maxSubSteps = clamp(m_Settings.MaxSubSteps, 1, 32);
	const double frameDelta = min(
		static_cast<double>(World::GetRawDeltaTime() * max(m_Settings.TimeScale, 0.0f)),
		static_cast<double>(max(m_Settings.MaxAccumulatedTime, fixedDelta)));
	m_Accumulator = min(
		m_Accumulator + frameDelta,
		static_cast<double>(max(m_Settings.MaxAccumulatedTime, fixedDelta)));
	m_LastSubStepCount = 0;
	while (m_Accumulator + 1.0e-9 >= fixedDelta && m_LastSubStepCount < maxSubSteps)
	{
		StepFixed(fixedDelta);
		m_Accumulator -= fixedDelta;
		++m_LastSubStepCount;
	}
	if (m_LastSubStepCount == maxSubSteps && m_Accumulator >= fixedDelta)
	{
		m_Accumulator = fmod(m_Accumulator, static_cast<double>(fixedDelta));
	}

	// AnimationSystem evaluates the authored pose every render frame, including
	// frames where the fixed-step accumulator does not advance. Reapply the
	// latest simulated body transforms unconditionally so authored and physics
	// poses cannot alternate at render rates above the physics frequency.
	ApplySimulationResults();
}

bool PhysicsSystem::IsBackendAvailable(PhysicsEngine engine) const
{
	const IPhysicsBackend* backend = GetBackend(engine);
	return backend && backend->IsAvailable();
}

const char* PhysicsSystem::GetBackendName(PhysicsEngine engine) const
{
	const IPhysicsBackend* backend = GetBackend(engine);
	return backend ? backend->GetName() : "Unavailable";
}

size_t PhysicsSystem::GetBoneBodyCount() const
{
	size_t count = 0;
	for (const auto& [entity, rig] : m_BoneRigs)
	{
		count += static_cast<size_t>(count_if(
			rig.Bodies.begin(), rig.Bodies.end(),
			[](const BoneBodyRuntime& body)
			{
				return body.Body != kInvalidPhysicsBody;
			}));
	}
	return count;
}

size_t PhysicsSystem::GetBoneJointCount() const
{
	size_t count = 0;
	for (const auto& [entity, rig] : m_BoneRigs)
	{
		count += rig.Joints.size();
	}
	return count;
}

void PhysicsSystem::ResetSimulation()
{
	ClearRuntime();
	m_PlaySession = 0;
}
