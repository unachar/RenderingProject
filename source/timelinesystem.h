#pragma once

#include "systembase.h"
#include "timeline.h"

class TimeLineSystem : public SystemBase
{
private:
	static vector<unique_ptr<TimeLineDirector>> m_Directors;

public:
	void Update() override;
	void Uninit() override;

	static TimeLineDirector& CreateDirector();
	static void AddDirector(unique_ptr<TimeLineDirector> director);
	static void Clear();
	static size_t GetDirectorCount();
};
