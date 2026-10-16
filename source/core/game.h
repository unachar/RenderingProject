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
	// Register a Game-owned instance after its Create has completed.
	static bool RegisterEntity(EntityBase* entity);

	template<typename T>
	static void AddEntity()
	{
		GetPendingEntities().emplace_back(make_unique<T>());
	}


};

// An inline static member declares automatic creation once per entity class.
template<typename T>
class EntityRegistration
{
public:
	EntityRegistration()
	{
		Game::AddEntity<T>();
	}
};



