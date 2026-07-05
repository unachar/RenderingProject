#pragma once
#include "ecs.h"
#include "entitybase.h"

#include <xmemory>
class Game
{
private:
	inline static std::vector<std::unique_ptr<EntityBase>> entityBase;
public:
	static void Init();
	static void Create();
	static void Uninit();
	static void Update();
	static void Draw();
	static void Run();

	template<typename T>
	static void AddEntity()
	{
		entityBase.emplace_back(std::make_unique<T>());
	}


};


