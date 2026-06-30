#pragma once
#include <windows.h>
#include <wrl.h>


#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include "d3dx12.h"

#pragma comment(lib, "d3dcompiler.lib")

#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include <queue>
#include <bitset>
#include <algorithm>
#include <functional>
#include <cstdarg>
#include <cstdio>
#include <array>

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace std;



class Debug
{
public:
	static void Log(const char* logtext, ...)
	{
		char text[1024];
		va_list textList;
		va_start(textList, logtext);
		vsnprintf(text, sizeof(text), logtext, textList);
		va_end(textList);

		wchar_t wideText[1024];
		const int wideLength = MultiByteToWideChar(CP_UTF8, 0, text, -1, wideText, static_cast<int>(size(wideText)));
		if (wideLength > 0)
		{
			OutputDebugStringW(wideText);
			return;
		}

		OutputDebugStringA(text);
	}
};

