#pragma once

#include <DirectXMath.h>
#include <cstdint>
#include <string>

struct PmxRigidBodyData
{
	string Name{};
	string EnglishName{};
	int32_t BoneIndex = -1;
	string BoneName{};
	uint8_t CollisionGroup = 0;
	uint16_t CollisionMask = 0xffff;
	uint8_t Shape = 0;
	DirectX::XMFLOAT3 Size{};
	DirectX::XMFLOAT3 Position{};
	DirectX::XMFLOAT3 Rotation{};
	float Mass = 0.0f;
	float LinearDamping = 0.0f;
	float AngularDamping = 0.0f;
	float Restitution = 0.0f;
	float Friction = 0.5f;
	uint8_t Operation = 0;
};

struct PmxJointData
{
	string Name{};
	string EnglishName{};
	uint8_t Type = 0;
	int32_t RigidBodyA = -1;
	int32_t RigidBodyB = -1;
	DirectX::XMFLOAT3 Position{};
	DirectX::XMFLOAT3 Rotation{};
	DirectX::XMFLOAT3 LinearLimitMin{};
	DirectX::XMFLOAT3 LinearLimitMax{};
	DirectX::XMFLOAT3 AngularLimitMin{};
	DirectX::XMFLOAT3 AngularLimitMax{};
	DirectX::XMFLOAT3 LinearSpring{};
	DirectX::XMFLOAT3 AngularSpring{};
};
