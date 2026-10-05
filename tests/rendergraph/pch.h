#pragma once
#include "d3d12.h"
#include <algorithm>
#include <functional>
#include <queue>
#include <string>
#include <vector>

using namespace std;

struct Debug
{
	static void Log(const char*, ...) {}
};
