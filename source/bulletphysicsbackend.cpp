#include "pch.h"
#include "physicsbackend.h"

#include <btBulletDynamicsCommon.h>
#include <BulletDynamics/ConstraintSolver/btGeneric6DofSpring2Constraint.h>
#include <BulletCollision/CollisionShapes/btConvexHullShape.h>
#include <unordered_map>


	btVector3 ToBt(const XMFLOAT3& value)
	{
		return btVector3(value.x, value.y, value.z);
	}

	btQuaternion ToBt(const XMFLOAT4& value)
	{
		return btQuaternion(value.x, value.y, value.z, value.w);
	}

	btTransform ToBt(const PhysicsTransform& value)
	{
		return btTransform(ToBt(value.Rotation), ToBt(value.Position));
	}

	PhysicsTransform FromBt(const btTransform& value)
	{
		const btVector3& p = value.getOrigin();
		const btQuaternion& q = value.getRotation();
		return {
			{ p.x(), p.y(), p.z() },
			{ q.x(), q.y(), q.z(), q.w() }
		};
	}

	class BulletPhysicsBackend final : public IPhysicsBackend
	{
		struct BodyRecord
		{
			unique_ptr<btCollisionShape> Shape{};
			unique_ptr<btDefaultMotionState> MotionState{};
			unique_ptr<btRigidBody> Body{};
		};

		unique_ptr<btDefaultCollisionConfiguration> m_CollisionConfiguration{};
		unique_ptr<btCollisionDispatcher> m_Dispatcher{};
		unique_ptr<btBroadphaseInterface> m_Broadphase{};
		unique_ptr<btSequentialImpulseConstraintSolver> m_Solver{};
		unique_ptr<btDiscreteDynamicsWorld> m_World{};
		unordered_map<PhysicsBodyHandle, BodyRecord> m_Bodies{};
		unordered_map<PhysicsJointHandle, unique_ptr<btTypedConstraint>> m_Joints{};
		PhysicsBodyHandle m_NextBody = 1;
		PhysicsJointHandle m_NextJoint = 1;

	public:
		const char* GetName() const override { return "Bullet"; }

		bool Init() override
		{
			m_CollisionConfiguration = make_unique<btDefaultCollisionConfiguration>();
			m_Dispatcher = make_unique<btCollisionDispatcher>(m_CollisionConfiguration.get());
			m_Broadphase = make_unique<btDbvtBroadphase>();
			m_Solver = make_unique<btSequentialImpulseConstraintSolver>();
			m_World = make_unique<btDiscreteDynamicsWorld>(
				m_Dispatcher.get(), m_Broadphase.get(), m_Solver.get(), m_CollisionConfiguration.get());
			m_World->setGravity(btVector3(0.0f, -9.81f, 0.0f));
			m_World->getSolverInfo().m_numIterations = 10;
			return true;
		}

		void Shutdown() override
		{
			if (m_World)
			{
				for (auto& [handle, constraint] : m_Joints)
				{
					m_World->removeConstraint(constraint.get());
				}
				for (auto& [handle, record] : m_Bodies)
				{
					m_World->removeRigidBody(record.Body.get());
				}
			}
			m_Joints.clear();
			m_Bodies.clear();
			m_World.reset();
			m_Solver.reset();
			m_Broadphase.reset();
			m_Dispatcher.reset();
			m_CollisionConfiguration.reset();
			m_NextBody = 1;
			m_NextJoint = 1;
		}

		bool IsAvailable() const override { return m_World != nullptr; }

		void SetGravity(const XMFLOAT3& gravity) override
		{
			if (m_World)
			{
				m_World->setGravity(ToBt(gravity));
			}
		}

		void Step(float fixedDeltaTime) override
		{
			if (m_World && fixedDeltaTime > 0.0f)
			{
				m_World->stepSimulation(fixedDeltaTime, 1, fixedDeltaTime);
			}
		}

		PhysicsBodyHandle CreateBody(const PhysicsBodyDesc& desc) override
		{
			if (!m_World)
			{
				return kInvalidPhysicsBody;
			}

			BodyRecord record{};
			switch (desc.Shape)
			{
			case PhysicsShape::Mesh:
				if (!desc.MeshVertices.empty())
				{
					auto hull = make_unique<btConvexHullShape>();
					for (const XMFLOAT3& vertex : desc.MeshVertices)
						hull->addPoint(ToBt(vertex), false);
					hull->recalcLocalAabb();
					record.Shape = move(hull);
				}
				else
				{
					record.Shape = make_unique<btBoxShape>(btVector3(
						max(desc.HalfExtent.x, 0.001f),
						max(desc.HalfExtent.y, 0.001f),
						max(desc.HalfExtent.z, 0.001f)));
				}
				break;
			case PhysicsShape::Sphere:
				record.Shape = make_unique<btSphereShape>(max(desc.Radius, 0.001f));
				break;
			case PhysicsShape::Capsule:
				record.Shape = make_unique<btCapsuleShape>(
					max(desc.Radius, 0.001f), max(desc.Height, 0.001f));
				break;
			case PhysicsShape::Box:
			default:
				record.Shape = make_unique<btBoxShape>(btVector3(
					max(desc.HalfExtent.x, 0.001f),
					max(desc.HalfExtent.y, 0.001f),
					max(desc.HalfExtent.z, 0.001f)));
				break;
			}

			const bool dynamic = desc.BodyType == PhysicsBodyType::Dynamic;
			const btScalar mass = dynamic ? max(desc.Mass, 0.0001f) : 0.0f;
			btVector3 inertia(0.0f, 0.0f, 0.0f);
			if (dynamic)
			{
				record.Shape->calculateLocalInertia(mass, inertia);
			}
			record.MotionState = make_unique<btDefaultMotionState>(ToBt(desc.Transform));
			btRigidBody::btRigidBodyConstructionInfo info(
				mass, record.MotionState.get(), record.Shape.get(), inertia);
			info.m_friction = desc.Friction;
			info.m_restitution = desc.Restitution;
			info.m_linearDamping = desc.LinearDamping;
			info.m_angularDamping = desc.AngularDamping;
			record.Body = make_unique<btRigidBody>(info);
			record.Body->setUserIndex2(static_cast<int>(desc.UserData & 0x7fffffff));
			if (!desc.AllowSleeping)
			{
				record.Body->setActivationState(DISABLE_DEACTIVATION);
			}
			if (desc.ContinuousCollision)
			{
				record.Body->setCcdMotionThreshold(max(desc.Radius * 0.5f, 0.01f));
				record.Body->setCcdSweptSphereRadius(max(desc.Radius * 0.2f, 0.005f));
			}
			if (desc.BodyType == PhysicsBodyType::Kinematic)
			{
				record.Body->setCollisionFlags(
					record.Body->getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);
				record.Body->setActivationState(DISABLE_DEACTIVATION);
			}

			const PhysicsBodyHandle handle = m_NextBody++;
			const short group = static_cast<short>(1u << (desc.CollisionLayer & 15u));
			const short mask = static_cast<short>(desc.CollisionMask);
			m_World->addRigidBody(record.Body.get(), group, mask);
			record.Body->setGravity(m_World->getGravity() * desc.GravityFactor);
			m_Bodies.emplace(handle, std::move(record));
			return handle;
		}

		void DestroyBody(PhysicsBodyHandle body) override
		{
			auto it = m_Bodies.find(body);
			if (it == m_Bodies.end())
			{
				return;
			}
			if (m_World)
			{
				m_World->removeRigidBody(it->second.Body.get());
			}
			m_Bodies.erase(it);
		}

		bool GetBodyTransform(PhysicsBodyHandle body, PhysicsTransform& transform) const override
		{
			auto it = m_Bodies.find(body);
			if (it == m_Bodies.end())
			{
				return false;
			}
			transform = FromBt(it->second.Body->getWorldTransform());
			return true;
		}

		void SetBodyTransform(PhysicsBodyHandle body, const PhysicsTransform& transform, bool activate) override
		{
			auto it = m_Bodies.find(body);
			if (it == m_Bodies.end())
			{
				return;
			}
			const btTransform worldTransform = ToBt(transform);
			it->second.Body->setWorldTransform(worldTransform);
			it->second.Body->setInterpolationWorldTransform(worldTransform);
			it->second.MotionState->setWorldTransform(worldTransform);
			if (activate)
			{
				it->second.Body->activate(true);
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
			it->second.Body->setLinearVelocity(ToBt(linearVelocity));
			it->second.Body->setAngularVelocity(ToBt(angularVelocity));
			it->second.Body->activate(true);
		}

		PhysicsJointHandle CreateJoint(const PhysicsJointDesc& desc) override
		{
			auto bodyA = m_Bodies.find(desc.BodyA);
			auto bodyB = m_Bodies.find(desc.BodyB);
			if (!m_World || bodyA == m_Bodies.end() || bodyB == m_Bodies.end())
			{
				return kInvalidPhysicsJoint;
			}

			const btTransform worldFrame = ToBt(desc.Frame);
			const btTransform frameA = bodyA->second.Body->getWorldTransform().inverse() * worldFrame;
			const btTransform frameB = bodyB->second.Body->getWorldTransform().inverse() * worldFrame;
			auto joint = make_unique<btGeneric6DofSpring2Constraint>(
				*bodyA->second.Body, *bodyB->second.Body, frameA, frameB);
			joint->setLinearLowerLimit(ToBt(desc.LinearLimitMin));
			joint->setLinearUpperLimit(ToBt(desc.LinearLimitMax));
			joint->setAngularLowerLimit(ToBt(desc.AngularLimitMin));
			joint->setAngularUpperLimit(ToBt(desc.AngularLimitMax));
			const float linearSpring[3] = {
				desc.LinearSpring.x, desc.LinearSpring.y, desc.LinearSpring.z
			};
			const float angularSpring[3] = {
				desc.AngularSpring.x, desc.AngularSpring.y, desc.AngularSpring.z
			};
			for (int axis = 0; axis < 3; ++axis)
			{
				if (linearSpring[axis] > 0.0f)
				{
					joint->enableSpring(axis, true);
					joint->setStiffness(axis, linearSpring[axis]);
				}
				if (angularSpring[axis] > 0.0f)
				{
					joint->enableSpring(axis + 3, true);
					joint->setStiffness(axis + 3, angularSpring[axis]);
				}
			}

			const PhysicsJointHandle handle = m_NextJoint++;
			m_World->addConstraint(joint.get(), true);
			m_Joints.emplace(handle, std::move(joint));
			return handle;
		}

		void DestroyJoint(PhysicsJointHandle joint) override
		{
			auto it = m_Joints.find(joint);
			if (it == m_Joints.end())
			{
				return;
			}
			if (m_World)
			{
				m_World->removeConstraint(it->second.get());
			}
			m_Joints.erase(it);
		}
	};


unique_ptr<IPhysicsBackend> CreateBulletPhysicsBackend()
{
	return make_unique<BulletPhysicsBackend>();
}
