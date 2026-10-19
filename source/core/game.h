#pragma once
#include "ecs.h"
#include "entitybase.h"

#include <memory>
#include <vector>

class Game
{
  private:
    static vector<unique_ptr<EntityBase>>& GetEntities();
    static vector<unique_ptr<EntityBase>>& GetPendingEntities();

  public:
    static void Init();
    static void Create();
    static void Uninit();
    static void Update();
    static void Draw();
    static void Run();
    // Create 完了後に Game 所有のインスタンスを登録する。
    static bool RegisterEntity(EntityBase* entity);

    template <typename T> static void AddEntity()
    {
        GetPendingEntities().emplace_back(make_unique<T>());
    }
};

// インライン静的メンバーにより、エンティティクラスごとに一度だけ自動生成を宣言する。
template <typename T> class EntityRegistration
{
  public:
    EntityRegistration()
    {
        Game::AddEntity<T>();
    }
};
