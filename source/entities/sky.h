#pragma once
#include "entitybase.h"
#include "game.h"

// 起動時の空は物理ベースの大気が描画する。旧環境メッシュも明示的には読み込める。
class Sky : public EntityBase
{
  public:
    void Create() override;
};
