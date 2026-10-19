#pragma once

#include "componentmanager.h"

class Sun
{
  public:
    static EntityID CreateDefault();
    static EntityID Create(const XMFLOAT3& position, const XMFLOAT3& target);
    // フレームの記録前に呼ぶ。Inspector や描画コールバックからは呼ばない。
    static bool MigrateLegacyGeometryBeforeFrame();
    static void Sync(EntityID entity);
};
