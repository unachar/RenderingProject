#include "pch.h"
#include "timelinesystem.h"
#include "world.h"

vector<unique_ptr<TimeLineDirector>> TimeLineSystem::m_Directors;

void TimeLineSystem::Update()
{
	const float deltaTime = World::GetDeltaTime();
	for (auto& director : m_Directors)
	{
		if (director)
		{
			director->Update(deltaTime);
		}
	}
}

void TimeLineSystem::Uninit()
{
	Clear();
}

TimeLineDirector& TimeLineSystem::CreateDirector()
{
	auto director = make_unique<TimeLineDirector>();
	TimeLineDirector& ref = *director;
	m_Directors.push_back(move(director));
	return ref;
}

void TimeLineSystem::AddDirector(unique_ptr<TimeLineDirector> director)
{
	if (director)
	{
		m_Directors.push_back(move(director));
	}
}

void TimeLineSystem::Clear()
{
	m_Directors.clear();
}

size_t TimeLineSystem::GetDirectorCount()
{
	return m_Directors.size();
}
