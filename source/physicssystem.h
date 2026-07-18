#pragma once

#include "systembase.h"
#include "physicsbackend.h"
#include "pmxphysicsdata.h"
#include "ecs.h"
#include <array>
#include <memory>
#include <unordered_map>
#include <vector>

struct PhysicsSettings
{
	float FixedTimeStep = 1.0f / 60.0f;
	int MaxSubSteps = 5;
	float MaxAccumulatedTime = 0.1f;
	float TimeScale = 1.0f;
	DirectX::XMFLOAT3 Gravity = { 0.0f, -9.81f, 0.0f };
};

class PhysicsSystem final : public SystemBase
{
	struct EntityBodyRuntime
	{
		PhysicsEngine Engine = PhysicsEngine::Bullet;
		PhysicsBodyHandle Body = kInvalidPhysicsBody;
		uint64_t SettingsHash = 0;
	};

	struct BoneBodyRuntime
	{
		PhysicsBodyHandle Body = kInvalidPhysicsBody;
		std::string BoneName{};
		uint8_t Operation = 0;
		DirectX::XMFLOAT4X4 Offset{};
		DirectX::XMFLOAT4X4 InverseOffset{};
	};

	struct BoneRigRuntime
	{
		PhysicsEngine Engine = PhysicsEngine::Bullet;
		int ModelId = -1;
		uint64_t SettingsHash = 0;
		std::vector<BoneBodyRuntime> Bodies{};
		std::vector<PhysicsJointHandle> Joints{};
	};

	std::array<std::unique_ptr<IPhysicsBackend>, 3> m_Backends{};
	std::unordered_map<EntityID, EntityBodyRuntime> m_EntityBodies{};
	std::unordered_map<EntityID, BoneRigRuntime> m_BoneRigs{};
	PhysicsSettings m_Settings{};
	double m_Accumulator = 0.0;
	uint64_t m_PlaySession = 0;
	int m_LastSubStepCount = 0;

	IPhysicsBackend* GetBackend(PhysicsEngine engine) const;
	void ClearRuntime();
	void DestroyEntityBody(EntityID entity);
	void DestroyBoneRig(EntityID entity);
	void SynchronizeRuntimeObjects();
	void CreateEntityBody(EntityID entity);
	void CreateBoneRig(EntityID entity);
	void SynchronizeKinematicBodies();
	void ApplySimulationResults();
	void StepFixed(float fixedDeltaTime);

public:
	void Init() override;
	void Uninit() override;
	void Update() override;

	PhysicsSettings& GetSettings() { return m_Settings; }
	const PhysicsSettings& GetSettings() const { return m_Settings; }
	int GetLastSubStepCount() const { return m_LastSubStepCount; }
	size_t GetEntityBodyCount() const { return m_EntityBodies.size(); }
	size_t GetBoneRigCount() const { return m_BoneRigs.size(); }
	size_t GetBoneBodyCount() const;
	size_t GetBoneJointCount() const;
	bool IsBackendAvailable(PhysicsEngine engine) const;
	const char* GetBackendName(PhysicsEngine engine) const;
	void ResetSimulation();
};
