#include "pch.h"
#include "modelmanager.h"
#include "rendererdraw.h"
#include "renderercore.h"
#include "texturemanager.h"
#include <chrono>
#include <algorithm>
#include <cctype>
#include <filesystem>

using namespace std;

vector<unique_ptr<AnimationModelResource>> ModelManager::m_AnimModels;
vector<unique_ptr<StaticModelResource>> ModelManager::m_StaticModels;
unordered_map<string, int> ModelManager::m_AnimModelCache;
unordered_map<string, int> ModelManager::m_StaticModelCache;

namespace
{
	string GetLowerExtension(const char* fileName)
	{
		string ext = filesystem::path(fileName ? fileName : "").extension().string();
		transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c)
			{
				return static_cast<char>(tolower(c));
			});
		return ext;
	}

	string MakeModelCacheKey(const char* fileName, bool isConvert)
	{
		return string(fileName ? fileName : "") + (isConvert ? "|convert" : "|raw");
	}
}

int ModelManager::LoadAnimModel(const char* fileName, bool isConvert)
{
	string key = MakeModelCacheKey(fileName, isConvert);
	auto it = m_AnimModelCache.find(key);
	if (it != m_AnimModelCache.end())
	{
		return it->second;
	}
	int id = LoadAnimModelDetailed(fileName, isConvert).ModelId;

	if (id >= 0)
	{
		m_AnimModelCache[key] = id;
	}
	return id;
}

int ModelManager::LoadStaticModel(const char* fileName, bool isConvert)
{
	string key = MakeModelCacheKey(fileName, isConvert);
	auto it = m_StaticModelCache.find(key);
	if (it != m_StaticModelCache.end())
	{
		return it->second;
	}
	int id = LoadStaticModelDetailed(fileName, isConvert).ModelId;
	if (id >= 0)
	{
		m_StaticModelCache[key] = id;
	}
	return id;
}

ModelManager::LoadModelResult ModelManager::LoadAnimModelDetailed(const char* fileName, bool isConvert)
{
	LoadModelResult result{};
	auto start = chrono::high_resolution_clock::now();

	TextureManager::BeginTextureLoading();
	auto model = make_unique<AnimationModelResource>();
	if (!model->Load(fileName, RendererCore::GetDevice(), isConvert))
	{
		TextureManager::EndTextureLoading();
		auto end = chrono::high_resolution_clock::now();
		result.ElapsedMs = chrono::duration<double, milli>(end - start).count();
		result.Error = "Animation model load failed";
		Debug::Log("Anim model load failed: %s (%.2f ms)\n", fileName, result.ElapsedMs);
		return result;
	}

	result.ModelId = (int)m_AnimModels.size();
	m_AnimModels.push_back(move(model));
	TextureManager::EndTextureLoading();

	auto end = chrono::high_resolution_clock::now();
	result.ElapsedMs = chrono::duration<double, milli>(end - start).count();
	result.Succeeded = true;
	Debug::Log("Anim model loaded: %s (id=%d, %.2f ms)\n", fileName, result.ModelId, result.ElapsedMs);
	return result;
}

ModelManager::LoadModelResult ModelManager::LoadStaticModelDetailed(const char* fileName, bool isConvert)
{
	LoadModelResult result{};
	auto start = chrono::high_resolution_clock::now();

	TextureManager::BeginTextureLoading();
	auto model = make_unique<StaticModelResource>();
	const string ext = GetLowerExtension(fileName);
	bool loaded = false;
	if (ext == ".obj")
	{
		loaded = model->LoadObj(fileName, RendererCore::GetDevice());
	}
	else if (ext == ".fbx" || ext == ".vrm" || ext == ".pmx")
	{
		loaded = model->LoadAssimpModel(fileName, RendererCore::GetDevice(), isConvert);
	}
	else
	{
		Debug::Log("ERROR: Unsupported static model extension: %s\n", fileName);
	}

	if (!loaded)
	{
		TextureManager::EndTextureLoading();
		auto end = chrono::high_resolution_clock::now();
		result.ElapsedMs = chrono::duration<double, milli>(end - start).count();
		result.Error = "Static model load failed";
		Debug::Log("Static model load failed: %s (%.2f ms)\n", fileName, result.ElapsedMs);
		return result;
	}

	result.ModelId = (int)m_StaticModels.size();
	m_StaticModels.push_back(move(model));
	TextureManager::EndTextureLoading();

	auto end = chrono::high_resolution_clock::now();
	result.ElapsedMs = chrono::duration<double, milli>(end - start).count();
	result.Succeeded = true;
	Debug::Log("Static model loaded: %s (id=%d, %.2f ms)\n", fileName, result.ModelId, result.ElapsedMs);
	return result;
}

void ModelManager::LoadAnimation(int modelId, const char* fileName, const char* name)
{
	if (modelId >= 0 && modelId < (int)m_AnimModels.size())
	{
		m_AnimModels[modelId]->LoadAnimation(fileName, name);
	}
}

AnimationModelResource* ModelManager::GetAnimModel(int modelId)
{
	if (modelId >= 0 && modelId < (int)m_AnimModels.size())
	{
		return m_AnimModels[modelId].get();
	}
	return nullptr;
}

StaticModelResource* ModelManager::GetStaticModel(int modelId)
{
	if (modelId >= 0 && modelId < (int)m_StaticModels.size())
	{
		return m_StaticModels[modelId].get();
	}
	return nullptr;
}

void ModelManager::Uninit()
{
	for (auto& model : m_AnimModels)
	{
		model->Uninit();
	}
	m_AnimModels.clear();
	m_AnimModelCache.clear();

	for (auto& model : m_StaticModels)
	{
		model->Uninit();
	}
	m_StaticModels.clear();
	m_StaticModelCache.clear();
}

