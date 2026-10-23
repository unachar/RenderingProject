#include "ecs/entityhierarchy.h"
#include "ecs/hierarchytransforms.h"
#include <cassert>
#include <iostream>
#include <unordered_map>

struct Transform
{
    DirectX::XMFLOAT3 Position{0, 0, 0};
    DirectX::XMFLOAT3 Rotation{0, 0, 0};
    DirectX::XMFLOAT3 Scale{1, 1, 1};
    DirectX::XMFLOAT4X4 WorldMatrix{};
    EntityHierarchy::Entity Parent = EntityHierarchy::None;
    DirectX::XMFLOAT4X4 ParentWorldMatrix{};
    bool HasParentWorld = false;
    bool HasAffineWorld = false;
    DirectX::XMFLOAT3 AffinePosition{};
    DirectX::XMFLOAT3 AffineRotation{};
    DirectX::XMFLOAT3 AffineScale{};
    bool IsDirty = true;
};

void CheckNear(float actual, float expected)
{
    if (std::abs(actual - expected) >= 0.0001f)
    {
        std::cerr << actual << " != " << expected << std::endl;
    }
    assert(std::abs(actual - expected) < 0.0001f);
}

void CheckMatrices(DirectX::FXMMATRIX actual, DirectX::CXMMATRIX expected)
{
    DirectX::XMFLOAT4X4 a;
    DirectX::XMFLOAT4X4 b;
    DirectX::XMStoreFloat4x4(&a, actual);
    DirectX::XMStoreFloat4x4(&b, expected);
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            CheckNear(a.m[row][column], b.m[row][column]);
        }
    }
}

int main()
{
    using namespace EntityHierarchy;
    std::unordered_map<Entity, Entity> parents{{1, None}, {2, 1}, {3, 2}, {4, None}};
    auto parent = [&](Entity entity) {
        auto found = parents.find(entity);
        return found == parents.end() ? None : found->second;
    };
    assert(IsDescendant(3, 1, parent));
    assert(!CanReparent(1, 3, parent));
    assert(!CanReparent(1, 1, parent));
    assert(CanReparent(2, 4, parent));
    assert(CanReparent(2, None, parent));
    assert((SelectionRoots({1, 2, 3, 4}, parent) == std::vector<Entity>{1, 4}));
    assert((SelectionRoots({2, 3}, parent) == std::vector<Entity>{2}));
    assert((WithDescendants({1}, {1, 2, 3, 4}, parent) == std::vector<Entity>{1, 2, 3}));
    parents[1] = 3;
    assert(!CanReparent(4, 2, parent)); // Corrupt imported cycles must terminate and be rejected.
    Transform original;
    original.Position = {3, 4, 5};
    original.Rotation = {0.3f, -0.7f, 1.2f};
    original.Scale = {2, 3, 4};
    Transform decoded;
    assert(TransformMath::SetWorld(decoded, TransformMath::World(original)));
    CheckMatrices(TransformMath::World(decoded.Position, decoded.Rotation, decoded.Scale),
                  TransformMath::World(original));
    original.Rotation = {0.2f, DirectX::XM_PIDIV2, 0.5f};
    assert(TransformMath::SetWorld(decoded, TransformMath::World(original)));
    CheckMatrices(TransformMath::World(decoded.Position, decoded.Rotation, decoded.Scale),
                  TransformMath::World(original));

    Transform child;
    child.Position = {2, 0, 0};
    assert(TransformMath::InheritParent(child, DirectX::XMMatrixIdentity(), DirectX::XMMatrixTranslation(5, 0, 0)));
    CheckNear(child.Position.x, 7);
    child = {};
    child.Position = {2, 0, 0};
    assert(TransformMath::InheritParent(child, DirectX::XMMatrixIdentity(),
                                        DirectX::XMMatrixRotationZ(DirectX::XM_PIDIV2)));
    CheckNear(child.Position.x, 0);
    CheckNear(child.Position.y, 2);
    child = {};
    child.Position = {2, 0, 0};
    assert(TransformMath::InheritParent(child, DirectX::XMMatrixIdentity(), DirectX::XMMatrixScaling(2, 2, 2)));
    CheckNear(child.Position.x, 4);
    CheckNear(child.Scale.x, 2);
    assert(!TransformMath::InheritParent(child, DirectX::XMMatrixScaling(0, 1, 1), DirectX::XMMatrixIdentity()));
    child = {};
    child.Position = {2, 1, 0};
    child.Rotation = {0, 0, 0.5f};
    const auto sheared = TransformMath::World(child) * DirectX::XMMatrixScaling(2, 1, 1);
    assert(TransformMath::InheritParent(child, DirectX::XMMatrixIdentity(), DirectX::XMMatrixScaling(2, 1, 1)));
    CheckMatrices(TransformMath::World(child), sheared);

    std::unordered_map<Entity, Transform> transforms;
    transforms[1] = {};
    transforms[2].Parent = 1;
    transforms[3].Parent = 2;
    transforms[2].Position.x = 2;
    transforms[3].Position.x = 3;
    auto lookup = [&](Entity entity) -> Transform& {
        return transforms.at(entity);
    };
    auto lookupParent = [&](Entity entity) {
        return transforms.at(entity).Parent;
    };
    int notified = 0;
    auto notify = [&](Entity) {
        ++notified;
    };
    // Reverse storage order, then initialize parent frames without moving anything.
    UpdateTransforms({3, 2, 1}, lookup, lookupParent, notify);
    CheckNear(transforms[3].Position.x, 3);
    assert(notified == 3);
    transforms[1].Position.x = 5;
    transforms[1].IsDirty = true;
    UpdateTransforms({3, 2, 1}, lookup, lookupParent, notify);
    CheckNear(transforms[2].Position.x, 7);
    CheckNear(transforms[3].Position.x, 8);
    assert(notified == 6);
    UpdateTransforms({3, 2, 1}, lookup, lookupParent, notify);
    CheckNear(transforms[3].Position.x, 8);
    assert(notified == 6); // No second application or redundant notifications.
    transforms[2].Parent = None;
    transforms[1].Position.x = 9;
    transforms[1].IsDirty = true;
    UpdateTransforms({3, 2, 1}, lookup, lookupParent, notify);
    CheckNear(transforms[3].Position.x, 8);
    // Malformed scenes cannot hang traversal.
    transforms[1].Parent = 3;
    transforms[2].Parent = 1;
    UpdateTransforms({3, 2, 1}, lookup, lookupParent, notify);
    assert(transforms[3].Parent == None);
    transforms[4] = child;
    const float originalX = child.Position.x;
    transforms[4].Position.x += 7;
    transforms[4].IsDirty = true;
    UpdateTransforms({4}, lookup, lookupParent, notify);
    transforms[4].Position.x = originalX;
    transforms[4].IsDirty = true;
    UpdateTransforms({4}, lookup, lookupParent, notify);
    CheckNear(transforms[4].WorldMatrix._41, originalX);
    std::cout << "Entity hierarchy and transform tests: PASS\n";
}
