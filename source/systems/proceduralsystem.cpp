#include "pch.h"
#include "proceduralsystem.h"
#include "proceduralscene.h"
#include "world.h"

void ProceduralSystem::Update()
{
    ProceduralScene::UpdateMotion(World::GetDeltaTime());
}
