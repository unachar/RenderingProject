#include "pch.h"
#include "physicsbackend.h"

#include <PxPhysicsAPI.h>
#include <thread>
#include <unordered_map>

namespace
{
	using namespace physx;

	PxVec3 ToPx(const XMFLOAT3& value)
	{
		return PxVec3(value.x, value.y, value.z);
	}

	PxQuat ToPx(const XMFLOAT4& value)
	{
		PxQuat result(value.x, value.y, value.z, value.w);
		return result.getNormalized();
	}

	PxTransform ToPx(const PhysicsTransform& value)
	{
		return PxTransform(ToPx(value.Position), ToPx(value.Rotation));
	}

	PhysicsTransform FromPx(const PxTransform& value)
	{
		return {
			{ value.p.x, value.p.y, value.p.z },
			{ value.q.x, value.q.y, value.q.z, value.q.w }
		};
	}

	PxFilterFlags PhysicsFilterShader(
		PxFilterObjectAttributes attributes0,
		PxFilterData filterData0,
		PxFilterObjectAttributes attributes1,
		PxFilterData filterData1,
		PxPairFlags& pairFlags,
		const void*,
		PxU32)
	{
		if (PxFilterObjectIsTrigger(attributes0) || PxFilterObjectIsTrigger(attributes1))
		{
			pairFlags = PxPairFlag::eTRIGGER_DEFAULT;
			return PxFilterFlag::eDEFAULT;
		}
		if ((filterData0.word0 & filterData1.word1) == 0 ||
			(filterData1.word0 & filterData0.word1) == 0)
		{
			return PxFilterFlag::eSUPPRESS;
		}
		pairFlags = PxPairFlag::eCONTACT_DEFAULT |
			PxPairFlag::eDETECT_CCD_CONTACT;
		return PxFilterFlag::eDEFAULT;
	}

	class PhysXPhysicsBackend final : public IPhysicsBackend
	{
		struct BodyRecord
		{
			PxRigidActor* Actor = nullptr;
			float GravityFactor = 1.0f;
		};

		PxDefaultAllocator m_Allocator{};
		PxDefaultErrorCallback m_ErrorCallback{};
		PxFoundation* m_Foundation = nullptr;
		PxPhysics* m_Physics = nullptr;
		PxDefaultCpuDispatcher* m_Dispatcher = nullptr;
		PxScene* m_Scene = nullptr;
		PxMaterial* m_DefaultMaterial = nullptr;
		unordered_map<PhysicsBodyHandle, BodyRecord> m_Bodies{};
		unordered_map<PhysicsJointHandle, PxD6Joint*> m_Joints{};
		PhysicsBodyHandle m_NextBody = 1;
		PhysicsJointHandle m_NextJoint = 1;
		XMFLOAT3 m_Gravity = { 0.0f, -9.81f, 0.0f };

	public:
		const char* GetName() const override { return "PhysX"; }

		bool Init() override
		{
			m_Foundation = PxCreateFoundation(PX_PHYSICS_VERSION, m_Allocator, m_ErrorCallback);
			if (!m_Foundation)
			{
				return false;
			}
			PxTolerancesScale scale{};
			m_Physics = PxCreatePhysics(PX_PHYSICS_VERSION, *m_Foundation, scale, true, nullptr);
			if (!m_Physics)
			{
				Shutdown();
				return false;
			}

			PxSceneDesc sceneDesc(scale);
			sceneDesc.gravity = ToPx(m_Gravity);
			m_Dispatcher = PxDefaultCpuDispatcherCreate(
				max(1u, std::thread::hardware_concurrency() / 2u));
			sceneDesc.cpuDispatcher = m_Dispatcher;
			sceneDesc.filterShader = PhysicsFilterShader;
			sceneDesc.flags |= PxSceneFlag::eENABLE_CCD;
			m_Scene = m_Physics->createScene(sceneDesc);
			m_DefaultMaterial = m_Physics->createMaterial(0.5f, 0.5f, 0.0f);
			if (!m_Scene || !m_DefaultMaterial)
			{
				Shutdown();
				return false;
			}
			return true;
		}

		void Shutdown() override
		{
			for (auto& [handle, joint] : m_Joints)
			{
				joint->release();
			}
			m_Joints.clear();
			for (auto& [handle, record] : m_Bodies)
			{
				record.Actor->release();
			}
			m_Bodies.clear();
			if (m_DefaultMaterial) m_DefaultMaterial->release();
			if (m_Scene) m_Scene->release();
			if (m_Dispatcher) m_Dispatcher->release();
			if (m_Physics) m_Physics->release();
			if (m_Foundation) m_Foundation->release();
			m_DefaultMaterial = nullptr;
			m_Scene = nullptr;
			m_Dispatcher = nullptr;
			m_Physics = nullptr;
			m_Foundation = nullptr;
			m_NextBody = 1;
			m_NextJoint = 1;
		}

		bool IsAvailable() const override { return m_Scene != nullptr; }

		void SetGravity(const XMFLOAT3& gravity) override
		{
			m_Gravity = gravity;
			if (m_Scene)
			{
				m_Scene->setGravity(ToPx(gravity));
			}
		}

		void Step(float fixedDeltaTime) override
		{
			if (!m_Scene || fixedDeltaTime <= 0.0f)
			{
				return;
			}
			for (const auto& [handle, record] : m_Bodies)
			{
				if (record.GravityFactor == 1.0f)
				{
					continue;
				}
				PxRigidDynamic* dynamic = record.Actor->is<PxRigidDynamic>();
				if (!dynamic ||
					dynamic->getRigidBodyFlags().isSet(PxRigidBodyFlag::eKINEMATIC) ||
					dynamic->getActorFlags().isSet(PxActorFlag::eDISABLE_GRAVITY))
				{
					continue;
				}
				const float adjustment = record.GravityFactor - 1.0f;
				dynamic->addForce(ToPx(m_Gravity) * adjustment, PxForceMode::eACCELERATION);
			}
			m_Scene->simulate(fixedDeltaTime);
			m_Scene->fetchResults(true);
		}

		PhysicsBodyHandle CreateBody(const PhysicsBodyDesc& desc) override
		{
			if (!m_Physics || !m_Scene || !m_DefaultMaterial)
			{
				return kInvalidPhysicsBody;
			}

			PxMaterial* material = m_Physics->createMaterial(
				max(desc.Friction, 0.0f),
				max(desc.Friction, 0.0f),
				clamp(desc.Restitution, 0.0f, 1.0f));
			if (!material)
			{
				return kInvalidPhysicsBody;
			}
			PxShape* shape = nullptr;
			switch (desc.Shape)
			{
			case PhysicsShape::Sphere:
				shape = m_Physics->createShape(
					PxSphereGeometry(max(desc.Radius, 0.001f)), *material, true);
				break;
			case PhysicsShape::Capsule:
				shape = m_Physics->createShape(
					PxCapsuleGeometry(max(desc.Radius, 0.001f), max(desc.Height * 0.5f, 0.001f)),
					*material, true);
				break;
			case PhysicsShape::Box:
			default:
				shape = m_Physics->createShape(
					PxBoxGeometry(
						max(desc.HalfExtent.x, 0.001f),
						max(desc.HalfExtent.y, 0.001f),
						max(desc.HalfExtent.z, 0.001f)),
					*material, true);
				break;
			}
			if (!shape)
			{
				material->release();
				return kInvalidPhysicsBody;
			}
			if (desc.Shape == PhysicsShape::Capsule)
			{
				// PhysX capsules are X-axis aligned; Bullet and Jolt use Y.
				// Normalize the backend convention so a shared descriptor has
				// identical orientation in all three engines.
				shape->setLocalPose(PxTransform(
					PxQuat(PxHalfPi, PxVec3(0.0f, 0.0f, 1.0f))));
			}
			material->release();
			shape->setContactOffset(max(0.02f, desc.Radius * 0.02f));
			PxFilterData filter{};
			filter.word0 = 1u << (desc.CollisionLayer & 15u);
			filter.word1 = desc.CollisionMask;
			shape->setSimulationFilterData(filter);

			PxRigidActor* actor = nullptr;
			if (desc.BodyType == PhysicsBodyType::Static)
			{
				actor = m_Physics->createRigidStatic(ToPx(desc.Transform));
			}
			else
			{
				PxRigidDynamic* dynamic = m_Physics->createRigidDynamic(ToPx(desc.Transform));
				if (dynamic)
				{
					dynamic->setLinearDamping(max(desc.LinearDamping, 0.0f));
					dynamic->setAngularDamping(max(desc.AngularDamping, 0.0f));
					dynamic->setRigidBodyFlag(PxRigidBodyFlag::eENABLE_CCD, desc.ContinuousCollision);
					dynamic->setRigidBodyFlag(
						PxRigidBodyFlag::eKINEMATIC,
						desc.BodyType == PhysicsBodyType::Kinematic);
					dynamic->setSleepThreshold(desc.AllowSleeping ? 0.005f : 0.0f);
					actor = dynamic;
				}
			}
			if (!actor)
			{
				shape->release();
				return kInvalidPhysicsBody;
			}
			actor->attachShape(*shape);
			shape->release();
			actor->userData = reinterpret_cast<void*>(static_cast<uintptr_t>(desc.UserData));
			if (PxRigidDynamic* dynamic = actor->is<PxRigidDynamic>())
			{
				if (desc.BodyType == PhysicsBodyType::Dynamic)
				{
					PxRigidBodyExt::setMassAndUpdateInertia(*dynamic, max(desc.Mass, 0.0001f));
				}
				if (desc.GravityFactor == 0.0f)
				{
					dynamic->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, true);
				}
			}
			m_Scene->addActor(*actor);
			const PhysicsBodyHandle handle = m_NextBody++;
			m_Bodies.emplace(handle, BodyRecord{ actor, desc.GravityFactor });
			return handle;
		}

		void DestroyBody(PhysicsBodyHandle body) override
		{
			auto it = m_Bodies.find(body);
			if (it == m_Bodies.end())
			{
				return;
			}
			it->second.Actor->release();
			m_Bodies.erase(it);
		}

		bool GetBodyTransform(PhysicsBodyHandle body, PhysicsTransform& transform) const override
		{
			auto it = m_Bodies.find(body);
			if (it == m_Bodies.end())
			{
				return false;
			}
			transform = FromPx(it->second.Actor->getGlobalPose());
			return true;
		}

		void SetBodyTransform(PhysicsBodyHandle body, const PhysicsTransform& transform, bool activate) override
		{
			auto it = m_Bodies.find(body);
			if (it == m_Bodies.end())
			{
				return;
			}
			it->second.Actor->setGlobalPose(ToPx(transform), activate);
			if (activate)
			{
				if (PxRigidDynamic* dynamic = it->second.Actor->is<PxRigidDynamic>())
				{
					dynamic->wakeUp();
				}
			}
		}

		void SetBodyVelocity(
			PhysicsBodyHandle body,
			const XMFLOAT3& linearVelocity,
			const XMFLOAT3& angularVelocity) override
		{
			auto it = m_Bodies.find(body);
			if (it == m_Bodies.end())
			{
				return;
			}
			if (PxRigidDynamic* dynamic = it->second.Actor->is<PxRigidDynamic>())
			{
				if (!dynamic->getRigidBodyFlags().isSet(PxRigidBodyFlag::eKINEMATIC))
				{
					dynamic->setLinearVelocity(ToPx(linearVelocity));
					dynamic->setAngularVelocity(ToPx(angularVelocity));
					dynamic->wakeUp();
				}
			}
		}

		PhysicsJointHandle CreateJoint(const PhysicsJointDesc& desc) override
		{
			auto bodyA = m_Bodies.find(desc.BodyA);
			auto bodyB = m_Bodies.find(desc.BodyB);
			if (!m_Physics || bodyA == m_Bodies.end() || bodyB == m_Bodies.end())
			{
				return kInvalidPhysicsJoint;
			}
			const PxTransform worldFrame = ToPx(desc.Frame);
			const PxTransform localA = bodyA->second.Actor->getGlobalPose().getInverse() * worldFrame;
			const PxTransform localB = bodyB->second.Actor->getGlobalPose().getInverse() * worldFrame;
			PxD6Joint* joint = PxD6JointCreate(
				*m_Physics, bodyA->second.Actor, localA, bodyB->second.Actor, localB);
			if (!joint)
			{
				return kInvalidPhysicsJoint;
			}

			const float linearMin[3] = {
				desc.LinearLimitMin.x, desc.LinearLimitMin.y, desc.LinearLimitMin.z
			};
			const float linearMax[3] = {
				desc.LinearLimitMax.x, desc.LinearLimitMax.y, desc.LinearLimitMax.z
			};
			const float linearSpring[3] = {
				desc.LinearSpring.x, desc.LinearSpring.y, desc.LinearSpring.z
			};
			const PxD6Axis::Enum linearAxes[3] = {
				PxD6Axis::eX, PxD6Axis::eY, PxD6Axis::eZ
			};
			for (int axis = 0; axis < 3; ++axis)
			{
				joint->setMotion(
					linearAxes[axis],
					linearMax[axis] > linearMin[axis] ? PxD6Motion::eLIMITED : PxD6Motion::eLOCKED);
				if (linearMax[axis] > linearMin[axis])
				{
					PxJointLinearLimitPair limit(
						linearMin[axis], linearMax[axis],
						PxSpring(max(linearSpring[axis], 0.0f), 0.0f));
					joint->setLinearLimit(linearAxes[axis], limit);
				}
			}

			joint->setMotion(
				PxD6Axis::eTWIST,
				desc.AngularLimitMax.x > desc.AngularLimitMin.x
					? PxD6Motion::eLIMITED : PxD6Motion::eLOCKED);
			if (desc.AngularLimitMax.x > desc.AngularLimitMin.x)
			{
				joint->setTwistLimit(PxJointAngularLimitPair(
					desc.AngularLimitMin.x, desc.AngularLimitMax.x,
					PxSpring(max(desc.AngularSpring.x, 0.0f), 0.0f)));
			}
			const bool swingY = desc.AngularLimitMax.y > desc.AngularLimitMin.y;
			const bool swingZ = desc.AngularLimitMax.z > desc.AngularLimitMin.z;
			joint->setMotion(PxD6Axis::eSWING1, swingY ? PxD6Motion::eLIMITED : PxD6Motion::eLOCKED);
			joint->setMotion(PxD6Axis::eSWING2, swingZ ? PxD6Motion::eLIMITED : PxD6Motion::eLOCKED);
			if (swingY || swingZ)
			{
				joint->setPyramidSwingLimit(PxJointLimitPyramid(
					swingY ? desc.AngularLimitMin.y : 0.0f,
					swingY ? desc.AngularLimitMax.y : 0.0f,
					swingZ ? desc.AngularLimitMin.z : 0.0f,
					swingZ ? desc.AngularLimitMax.z : 0.0f,
					PxSpring(max(desc.AngularSpring.y, desc.AngularSpring.z), 0.0f)));
			}
			joint->setAngularDriveConfig(PxD6AngularDriveConfig::eSWING_TWIST);
			if (desc.AngularSpring.x > 0.0f)
				joint->setDrive(PxD6Drive::eTWIST, PxD6JointDrive(desc.AngularSpring.x, 0.0f, PX_MAX_F32));
			if (desc.AngularSpring.y > 0.0f)
				joint->setDrive(PxD6Drive::eSWING1, PxD6JointDrive(desc.AngularSpring.y, 0.0f, PX_MAX_F32));
			if (desc.AngularSpring.z > 0.0f)
				joint->setDrive(PxD6Drive::eSWING2, PxD6JointDrive(desc.AngularSpring.z, 0.0f, PX_MAX_F32));

			const PhysicsJointHandle handle = m_NextJoint++;
			m_Joints.emplace(handle, joint);
			return handle;
		}

		void DestroyJoint(PhysicsJointHandle joint) override
		{
			auto it = m_Joints.find(joint);
			if (it == m_Joints.end())
			{
				return;
			}
			it->second->release();
			m_Joints.erase(it);
		}
	};
}

unique_ptr<IPhysicsBackend> CreatePhysXPhysicsBackend()
{
	return make_unique<PhysXPhysicsBackend>();
}
