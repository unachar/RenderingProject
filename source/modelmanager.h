#pragma once

#include "animationmodel.h"
#include "staticmodel.h"
#include <vector>
#include <memory>
#include <string>
#include <unordered_map>

class ModelManager
{
private:
	static vector<unique_ptr<AnimationModelResource>> m_AnimModels;
	static vector<unique_ptr<StaticModelResource>> m_StaticModels;
	static unordered_map<string, int> m_AnimModelCache;
	static unordered_map<string, int> m_StaticModelCache;

	struct LoadModelResult
	{
		int ModelId = -1;
		bool Succeeded = false;
		double ElapsedMs = 0.0;
		string Error;
	};
	static LoadModelResult LoadAnimModelDetailed(const char* fileName, bool isConvert = true);
	static LoadModelResult LoadStaticModelDetailed(const char* fileName, bool isConvert = true);
public:
	static int LoadAnimModel(const char* fileName, bool isConvert = true);
	static int LoadStaticModel(const char* fileName, bool isConvert = true);

	static void LoadAnimation(int modelId, const char* fileName, const char* name);

	static AnimationModelResource* GetAnimModel(int modelId);
	static StaticModelResource* GetStaticModel(int modelId);

	static void Uninit();
};

