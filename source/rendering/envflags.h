#pragma once
#include <windows.h>
#include <cstdlib>

// 環境変数は起動中に変わらない前提で、最初の1回だけ読む（毎フレーム・毎ドローの読み取りを削減）。
inline bool ReadEnvironmentFlagOnce(const char* name)
{
	char value[16]{};
	return GetEnvironmentVariableA(name, value, static_cast<DWORD>(sizeof(value))) > 0 && atoi(value) != 0;
}

inline bool IsShadowReferenceDrawEnabled()
{
	static const bool enabled = ReadEnvironmentFlagOnce("DX12_SHADOW_REFERENCE_DRAW");
	return enabled;
}

inline bool IsFeatureSmokeTestEnabled()
{
	static const bool enabled = ReadEnvironmentFlagOnce("DX12_FEATURE_SMOKE_TEST");
	return enabled;
}
