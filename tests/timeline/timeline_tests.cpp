// Dependency doubles isolate real property-selection code from Windows/DirectX/ECS.
#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>

using namespace std;
using EntityID = unsigned;
struct XMFLOAT3 { float x = 0, y = 0, z = 0; };
struct XMFLOAT4 { float x = 0, y = 0, z = 0, w = 0; };
struct TransformComponent
{
	XMFLOAT3 Position{}, Rotation{}, Scale{};
	bool IsDirty = false;
};
struct CameraComponent
{
	XMFLOAT3 Target{}, LockOnOffset{};
	float Fov = 1.0f, NearClip = 0.1f, FarClip = 100.0f;
};
struct PostProcessComponent { float Intensity = 0.0f; };
float XMConvertToRadians(float degrees) { return degrees * 3.14159265358979323846f / 180.0f; }
struct Registry
{
	inline static bool Alive = true;
	static bool IsAlive(EntityID) { return Alive; }
};
struct ComponentManager
{
	template<typename T> inline static T Value{};
	template<typename T> inline static bool Present = true;
	template<typename T> static bool HasComponent(EntityID) { return Present<T>; }
	template<typename T> static T& GetComponentUnchecked(EntityID) { return Value<T>; }
};
#include "production_enum.h"
struct TimeLineSystem
{
	static XMFLOAT4 ReadProperty(EntityID target, TimelineProperty property, bool* valid);
};
#include "production_bodies.h"

void CheckValue(const XMFLOAT4& value, float x, float y, float z)
{
	assert(value.x == x && value.y == y && value.z == z && value.w == 0);
}

int main()
{
	auto& transform = ComponentManager::Value<TransformComponent>;
	transform.Position = { 1, 2, 3 };
	transform.Rotation = { 4, 5, 6 };
	transform.Scale = { 7, 8, 9 };
	bool valid = false;
	CheckValue(TimeLineSystem::ReadProperty(0, TimelineProperty::TransformPosition, &valid), 1, 2, 3);
	assert(valid);
	CheckValue(TimeLineSystem::ReadProperty(0, TimelineProperty::TransformRotation, &valid), 4, 5, 6);
	CheckValue(TimeLineSystem::ReadProperty(0, TimelineProperty::TransformScale, nullptr), 7, 8, 9);
	ApplyProperty(0, TimelineProperty::TransformPosition, { 10, 11, 12, 0 });
	ApplyProperty(0, TimelineProperty::TransformRotation, { 13, 14, 15, 0 });
	ApplyProperty(0, TimelineProperty::TransformScale, { 16, 17, 18, 0 });
	assert(transform.IsDirty);
	CheckValue(TimeLineSystem::ReadProperty(0, TimelineProperty::TransformPosition, nullptr), 10, 11, 12);
	CheckValue(TimeLineSystem::ReadProperty(0, TimelineProperty::TransformRotation, nullptr), 13, 14, 15);
	CheckValue(TimeLineSystem::ReadProperty(0, TimelineProperty::TransformScale, nullptr), 16, 17, 18);
	Registry::Alive = false;
	valid = true;
	ApplyProperty(0, TimelineProperty::TransformPosition, { 99, 99, 99, 0 });
	CheckValue(TimeLineSystem::ReadProperty(0, TimelineProperty::TransformPosition, &valid), 0, 0, 0);
	assert(!valid && transform.Position.x == 10);
	Registry::Alive = true;
	ComponentManager::Present<TransformComponent> = false;
	valid = true;
	CheckValue(TimeLineSystem::ReadProperty(0, TimelineProperty::TransformScale, &valid), 0, 0, 0);
	assert(!valid);
	ComponentManager::Present<TransformComponent> = true;
	ApplyProperty(0, TimelineProperty::CameraFov, { 0, 0, 0, 0 });
	auto& camera = ComponentManager::Value<CameraComponent>;
	assert(camera.Fov == XMConvertToRadians(1.0f));
	ApplyProperty(0, TimelineProperty::CameraFov, { 100, 0, 0, 0 });
	assert(camera.Fov == XMConvertToRadians(179.0f));
	ApplyProperty(0, TimelineProperty::CameraNearClip, { -1, 0, 0, 0 });
	ApplyProperty(0, TimelineProperty::CameraFarClip, { -1, 0, 0, 0 });
	assert(camera.NearClip == 0.001f && camera.FarClip == 0.002f);
	ApplyProperty(0, TimelineProperty::CameraTarget, { 1, 2, 3, 0 });
	ApplyProperty(0, TimelineProperty::CameraLockOnOffset, { 4, 5, 6, 0 });
	CheckValue(TimeLineSystem::ReadProperty(0, TimelineProperty::CameraTarget, nullptr), 1, 2, 3);
	CheckValue(TimeLineSystem::ReadProperty(0, TimelineProperty::CameraLockOnOffset, nullptr), 4, 5, 6);
	ApplyProperty(0, TimelineProperty::PostProcessIntensity, { 2, 0, 0, 0 });
	CheckValue(TimeLineSystem::ReadProperty(0, TimelineProperty::PostProcessIntensity, nullptr), 1, 0, 0);
	cout << "PASS: timeline property selection, round trips and boundaries (CPU dependency doubles)\n";
}
