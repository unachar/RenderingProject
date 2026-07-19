#include "pch.h"
#include "physicsbackend.h"

#define JPH_DEBUG_RENDERER
#define JPH_FLOATING_POINT_EXCEPTIONS_ENABLED
#define JPH_OBJECT_STREAM
#define JPH_PROFILE_ENABLED
#define JPH_USE_CPU_COMPUTE
#define JPH_USE_DX12
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/IssueReporting.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyLockMulti.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/CollisionGroup.h>
#include <Jolt/Physics/Collision/GroupFilter.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <unordered_map>

using namespace JPH;

	class PhysicsCollisionGroupFilter final : public GroupFilter
	{
		JPH_DECLARE_SERIALIZABLE_VIRTUAL(JPH_NO_EXPORT, PhysicsCollisionGroupFilter)

	public:
		bool CanCollide(
			const CollisionGroup& group1,
			const CollisionGroup& group2) const override
		{
			const uint32 bit1 = 1u << (group1.GetSubGroupID() & 15u);
			const uint32 bit2 = 1u << (group2.GetSubGroupID() & 15u);
			return (group1.GetGroupID() & bit2) != 0 &&
				(group2.GetGroupID() & bit1) != 0;
		}
	};

	JPH_IMPLEMENT_SERIALIZABLE_VIRTUAL(PhysicsCollisionGroupFilter)
	{
	}

	void JoltTrace(const char* format, ...)
	{
		char buffer[2048]{};
		va_list args;
		va_start(args, format);
		vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args);
		va_end(args);
		Debug::Log("Jolt: %s", buffer);
	}

#ifdef JPH_ENABLE_ASSERTS
	bool JoltAssertFailed(
		const char* expression,
		const char* message,
		const char* file,
		uint line)
	{
		Debug::Log(
			"ERROR: Jolt assertion failed: %s (%s:%u) %s\n",
			expression ? expression : "",
			file ? file : "",
			line,
			message ? message : "");
		FILE* output = nullptr;
		if (fopen_s(&output, "Save/jolt_assert.txt", "a") == 0 && output)
		{
			fprintf(
				output,
				"%s (%s:%u) %s\n",
				expression ? expression : "",
				file ? file : "",
				line,
				message ? message : "");
			fclose(output);
		}
		return false;
	}
#endif

	static constexpr ObjectLayer kJoltStaticObjectLayer = 0;
	static constexpr ObjectLayer kJoltMovingObjectLayer = 1;
	static constexpr ObjectLayer kJoltObjectLayerCount = 2;
	static constexpr BroadPhaseLayer kJoltStaticBroadPhaseLayer(0);
	static constexpr BroadPhaseLayer kJoltMovingBroadPhaseLayer(1);
	static constexpr uint kJoltBroadPhaseLayerCount = 2;

	class JoltObjectLayerPairFilter final : public ObjectLayerPairFilter
	{
	public:
		bool ShouldCollide(ObjectLayer object1, ObjectLayer object2) const override
		{
			return object1 != kJoltStaticObjectLayer || object2 != kJoltStaticObjectLayer;
		}
	};

	class JoltBroadPhaseLayerInterface final : public BroadPhaseLayerInterface
	{
	public:
		uint GetNumBroadPhaseLayers() const override { return kJoltBroadPhaseLayerCount; }
		BroadPhaseLayer GetBroadPhaseLayer(ObjectLayer layer) const override
		{
			return layer == kJoltStaticObjectLayer ? kJoltStaticBroadPhaseLayer : kJoltMovingBroadPhaseLayer;
		}
		const char* GetBroadPhaseLayerName(BroadPhaseLayer layer) const override
		{
			return layer == kJoltStaticBroadPhaseLayer ? "Static" : "Moving";
		}
	};

	class JoltObjectVsBroadPhaseFilter final : public ObjectVsBroadPhaseLayerFilter
	{
	public:
		bool ShouldCollide(ObjectLayer layer, BroadPhaseLayer broadPhaseLayer) const override
		{
			return layer != kJoltStaticObjectLayer || broadPhaseLayer == kJoltMovingBroadPhaseLayer;
		}
	};

	RVec3 ToJoltPosition(const XMFLOAT3& value)
	{
		return RVec3(value.x, value.y, value.z);
	}

	Vec3 ToJoltVector(const XMFLOAT3& value)
	{
		return Vec3(value.x, value.y, value.z);
	}

	Quat ToJolt(const XMFLOAT4& value)
	{
		return Quat(value.x, value.y, value.z, value.w).Normalized();
	}

	BodyID ToBodyId(PhysicsBodyHandle handle)
	{
		return handle == kInvalidPhysicsBody
			? BodyID()
			: BodyID(static_cast<uint32>(handle - 1));
	}

	PhysicsBodyHandle ToHandle(const BodyID& id)
	{
		return id.IsInvalid()
			? kInvalidPhysicsBody
			: static_cast<PhysicsBodyHandle>(id.GetIndexAndSequenceNumber()) + 1;
	}

	class JoltPhysicsBackend final : public IPhysicsBackend
	{
		JoltBroadPhaseLayerInterface m_BroadPhaseLayerInterface{};
		JoltObjectVsBroadPhaseFilter m_ObjectVsBroadPhaseFilter{};
		JoltObjectLayerPairFilter m_ObjectLayerPairFilter{};
		unique_ptr<JPH::PhysicsSystem> m_Physics{};
		unique_ptr<TempAllocatorImpl> m_TempAllocator{};
		unique_ptr<JobSystemThreadPool> m_JobSystem{};
		Ref<PhysicsCollisionGroupFilter> m_CollisionGroupFilter{};
		unordered_map<PhysicsJointHandle, Ref<Constraint>> m_Joints{};
		PhysicsJointHandle m_NextJoint = 1;
		bool m_TypesRegistered = false;

	public:
		const char* GetName() const override { return "Jolt"; }

		bool Init() override
		{
			RegisterDefaultAllocator();
			Trace = JoltTrace;
#ifdef JPH_ENABLE_ASSERTS
			AssertFailed = JoltAssertFailed;
#endif
			if (Factory::sInstance == nullptr)
			{
				Factory::sInstance = new Factory();
				RegisterTypes();
				m_TypesRegistered = true;
			}

			m_TempAllocator = make_unique<TempAllocatorImpl>(32 * 1024 * 1024);
			const uint threadCount = max(1u, thread::hardware_concurrency() > 1
				? thread::hardware_concurrency() - 1 : 1u);
			m_JobSystem = make_unique<JobSystemThreadPool>(
				cMaxPhysicsJobs, cMaxPhysicsBarriers, static_cast<int>(threadCount));
			m_Physics = make_unique<JPH::PhysicsSystem>();
			m_CollisionGroupFilter = new PhysicsCollisionGroupFilter();
			m_Physics->Init(
				65536, 0, 65536, 16384,
				m_BroadPhaseLayerInterface,
				m_ObjectVsBroadPhaseFilter,
				m_ObjectLayerPairFilter);
			m_Physics->SetGravity(Vec3(0.0f, -9.81f, 0.0f));
			return true;
		}

		void Shutdown() override
		{
			if (m_Physics)
			{
				for (auto& [handle, constraint] : m_Joints)
				{
					m_Physics->RemoveConstraint(constraint);
				}
			}
			m_Joints.clear();
			m_Physics.reset();
			m_CollisionGroupFilter = nullptr;
			m_JobSystem.reset();
			m_TempAllocator.reset();
			m_NextJoint = 1;
			if (m_TypesRegistered)
			{
				UnregisterTypes();
				delete Factory::sInstance;
				Factory::sInstance = nullptr;
				m_TypesRegistered = false;
			}
		}

		bool IsAvailable() const override { return m_Physics != nullptr; }

		void SetGravity(const XMFLOAT3& gravity) override
		{
			if (m_Physics)
			{
				m_Physics->SetGravity(ToJoltVector(gravity));
			}
		}

		void Step(float fixedDeltaTime) override
		{
			if (m_Physics && m_TempAllocator && m_JobSystem && fixedDeltaTime > 0.0f)
			{
				m_Physics->Update(fixedDeltaTime, 1, m_TempAllocator.get(), m_JobSystem.get());
			}
		}

		PhysicsBodyHandle CreateBody(const PhysicsBodyDesc& desc) override
		{
			if (!m_Physics)
			{
				return kInvalidPhysicsBody;
			}

			ShapeRefC shape;
			switch (desc.Shape)
			{
			case PhysicsShape::Mesh:
				if (desc.MeshVertices.size() >= 4)
				{
					Array<Vec3> points;
					points.reserve(desc.MeshVertices.size());
					for (const XMFLOAT3& vertex : desc.MeshVertices)
						points.push_back(ToJoltVector(vertex));
					ConvexHullShapeSettings hullSettings(points);
					ShapeSettings::ShapeResult hullResult = hullSettings.Create();
					if (!hullResult.HasError())
						shape = hullResult.Get();
				}
				if (shape == nullptr)
					shape = new BoxShape(Vec3(
						max(desc.HalfExtent.x, 0.001f),
						max(desc.HalfExtent.y, 0.001f),
						max(desc.HalfExtent.z, 0.001f)));
				break;
			case PhysicsShape::Sphere:
				shape = new SphereShape(max(desc.Radius, 0.001f));
				break;
			case PhysicsShape::Capsule:
				shape = new CapsuleShape(max(desc.Height * 0.5f, 0.001f), max(desc.Radius, 0.001f));
				break;
			case PhysicsShape::Box:
			default:
				shape = new BoxShape(Vec3(
					max(desc.HalfExtent.x, 0.001f),
					max(desc.HalfExtent.y, 0.001f),
					max(desc.HalfExtent.z, 0.001f)));
				break;
			}

			EMotionType motionType = EMotionType::Dynamic;
			if (desc.BodyType == PhysicsBodyType::Static) motionType = EMotionType::Static;
			else if (desc.BodyType == PhysicsBodyType::Kinematic) motionType = EMotionType::Kinematic;
			const ObjectLayer layer = motionType == EMotionType::Static
				? kJoltStaticObjectLayer : kJoltMovingObjectLayer;
			BodyCreationSettings settings(
				shape,
				ToJoltPosition(desc.Transform.Position),
				ToJolt(desc.Transform.Rotation),
				motionType,
				layer);
			settings.mUserData = desc.UserData;
			settings.mFriction = desc.Friction;
			settings.mRestitution = desc.Restitution;
			settings.mLinearDamping = max(desc.LinearDamping, 0.0f);
			settings.mAngularDamping = max(desc.AngularDamping, 0.0f);
			settings.mGravityFactor = desc.GravityFactor;
			settings.mAllowSleeping = desc.AllowSleeping;
			settings.mMotionQuality = desc.ContinuousCollision
				? EMotionQuality::LinearCast : EMotionQuality::Discrete;
			settings.mCollisionGroup = CollisionGroup(
				m_CollisionGroupFilter,
				static_cast<CollisionGroup::GroupID>(desc.CollisionMask),
				static_cast<CollisionGroup::SubGroupID>(desc.CollisionLayer & 15u));
			if (motionType == EMotionType::Dynamic)
			{
				settings.mOverrideMassProperties = EOverrideMassProperties::CalculateInertia;
				settings.mMassPropertiesOverride.mMass = max(desc.Mass, 0.0001f);
			}

			BodyID id = m_Physics->GetBodyInterface().CreateAndAddBody(
				settings,
				motionType == EMotionType::Static ? EActivation::DontActivate : EActivation::Activate);
			return ToHandle(id);
		}

		void DestroyBody(PhysicsBodyHandle body) override
		{
			if (!m_Physics || body == kInvalidPhysicsBody)
			{
				return;
			}
			BodyInterface& bodyInterface = m_Physics->GetBodyInterface();
			const BodyID id = ToBodyId(body);
			if (bodyInterface.IsAdded(id))
			{
				bodyInterface.RemoveBody(id);
			}
			bodyInterface.DestroyBody(id);
		}

		bool GetBodyTransform(PhysicsBodyHandle body, PhysicsTransform& transform) const override
		{
			if (!m_Physics || body == kInvalidPhysicsBody)
			{
				return false;
			}
			RVec3 position;
			Quat rotation;
			m_Physics->GetBodyInterface().GetPositionAndRotation(ToBodyId(body), position, rotation);
			transform.Position = {
				static_cast<float>(position.GetX()),
				static_cast<float>(position.GetY()),
				static_cast<float>(position.GetZ())
			};
			transform.Rotation = { rotation.GetX(), rotation.GetY(), rotation.GetZ(), rotation.GetW() };
			return true;
		}

		void SetBodyTransform(PhysicsBodyHandle body, const PhysicsTransform& transform, bool activate) override
		{
			if (!m_Physics || body == kInvalidPhysicsBody)
			{
				return;
			}
			m_Physics->GetBodyInterface().SetPositionAndRotation(
				ToBodyId(body),
				ToJoltPosition(transform.Position),
				ToJolt(transform.Rotation),
				activate ? EActivation::Activate : EActivation::DontActivate);
		}

		void SetBodyVelocity(
			PhysicsBodyHandle body,
			const XMFLOAT3& linearVelocity,
			const XMFLOAT3& angularVelocity) override
		{
			if (!m_Physics || body == kInvalidPhysicsBody)
			{
				return;
			}
			m_Physics->GetBodyInterface().SetLinearAndAngularVelocity(
				ToBodyId(body), ToJoltVector(linearVelocity), ToJoltVector(angularVelocity));
		}

		PhysicsJointHandle CreateJoint(const PhysicsJointDesc& desc) override
		{
			if (!m_Physics)
			{
				return kInvalidPhysicsJoint;
			}
			const BodyID bodyIds[] = {
				ToBodyId(desc.BodyA),
				ToBodyId(desc.BodyB)
			};
			BodyLockMultiWrite lock(
				m_Physics->GetBodyLockInterface(),
				bodyIds,
				static_cast<int>(size(bodyIds)));
			Body* bodyA = lock.GetBody(0);
			Body* bodyB = lock.GetBody(1);
			if (!bodyA || !bodyB)
			{
				return kInvalidPhysicsJoint;
			}

			const Quat frameRotation = ToJolt(desc.Frame.Rotation);
			SixDOFConstraintSettings settings{};
			settings.mSpace = EConstraintSpace::WorldSpace;
			settings.mPosition1 = ToJoltPosition(desc.Frame.Position);
			settings.mPosition2 = ToJoltPosition(desc.Frame.Position);
			settings.mAxisX1 = settings.mAxisX2 = frameRotation * Vec3::sAxisX();
			settings.mAxisY1 = settings.mAxisY2 = frameRotation * Vec3::sAxisY();
			const float limitMin[6] = {
				desc.LinearLimitMin.x, desc.LinearLimitMin.y, desc.LinearLimitMin.z,
				desc.AngularLimitMin.x, desc.AngularLimitMin.y, desc.AngularLimitMin.z
			};
			const float limitMax[6] = {
				desc.LinearLimitMax.x, desc.LinearLimitMax.y, desc.LinearLimitMax.z,
				desc.AngularLimitMax.x, desc.AngularLimitMax.y, desc.AngularLimitMax.z
			};
			for (int axis = 0; axis < 6; ++axis)
			{
				settings.SetLimitedAxis(
					static_cast<SixDOFConstraintSettings::EAxis>(axis),
					limitMin[axis], limitMax[axis]);
			}
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
					settings.mLimitsSpringSettings[axis] = SpringSettings(
						ESpringMode::StiffnessAndDamping, linearSpring[axis], 0.0f);
				}
				if (angularSpring[axis] > 0.0f)
				{
					settings.mMotorSettings[axis + 3].mSpringSettings = SpringSettings(
						ESpringMode::StiffnessAndDamping, angularSpring[axis], 0.0f);
				}
			}

			Ref<SixDOFConstraint> joint = static_cast<SixDOFConstraint*>(
				settings.Create(*bodyA, *bodyB));
			for (int axis = 0; axis < 3; ++axis)
			{
				if (angularSpring[axis] > 0.0f)
				{
					joint->SetMotorState(
						static_cast<SixDOFConstraint::EAxis>(axis + 3),
						EMotorState::Position);
				}
			}
			m_Physics->AddConstraint(joint);
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
			if (m_Physics)
			{
				m_Physics->RemoveConstraint(it->second);
			}
			m_Joints.erase(it);
		}
	};


unique_ptr<IPhysicsBackend> CreateJoltPhysicsBackend()
{
	return make_unique<JoltPhysicsBackend>();
}
