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

#if defined(RENDERINGPROJECT_USE_DXC) && RENDERINGPROJECT_USE_DXC
#include "dxccompiler.h"

// Preserve legacy call sites while routing runtime HLSL compilation through
// IDxcCompiler3. Legacy stage_5_x targets are promoted to stage_6_0 by the
// compatibility adapter.
#define D3DCompileFromFile DxcCompileFromFileCompat
#endif

#include "renderprofiler.h"

// Count the engine's actual D3D12 submissions without replacing the command
// list object. These expression-style macros remain a single statement, so they
// are safe under an unbraced if/for statement. D3D12 interfaces have already
// been declared above, preventing the macros from altering SDK declarations.
// Define RENDERINGPROJECT_DISABLE_RENDER_CALL_INSTRUMENTATION before pch.h to
// disable this layer while diagnosing an integration conflict.
#ifndef RENDERINGPROJECT_DISABLE_RENDER_CALL_INSTRUMENTATION
#define DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation) \
	DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation), \
	RenderProfiler::RecordDraw(static_cast<UINT>(InstanceCount), false)

#define DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation) \
	DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation), \
	RenderProfiler::RecordDraw(static_cast<UINT>(InstanceCount), true)

#define Dispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ) \
	Dispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ), \
	RenderProfiler::RecordDispatch( \
		static_cast<UINT>(ThreadGroupCountX), \
		static_cast<UINT>(ThreadGroupCountY), \
		static_cast<UINT>(ThreadGroupCountZ))

#define ExecuteIndirect(pCommandSignature, MaxCommandCount, pArgumentBuffer, ArgumentBufferOffset, pCountBuffer, CountBufferOffset) \
	ExecuteIndirect(pCommandSignature, MaxCommandCount, pArgumentBuffer, ArgumentBufferOffset, pCountBuffer, CountBufferOffset), \
	RenderProfiler::RecordExecuteIndirect(static_cast<UINT>(MaxCommandCount))

#define SetPipelineState(pPipelineState) \
	SetPipelineState(pPipelineState), RenderProfiler::RecordPipelineStateBind()

#define SetGraphicsRootDescriptorTable(RootParameterIndex, BaseDescriptor) \
	SetGraphicsRootDescriptorTable(RootParameterIndex, BaseDescriptor), \
	RenderProfiler::RecordGraphicsDescriptorTableBind()

#define SetComputeRootDescriptorTable(RootParameterIndex, BaseDescriptor) \
	SetComputeRootDescriptorTable(RootParameterIndex, BaseDescriptor), \
	RenderProfiler::RecordComputeDescriptorTableBind()

#define ResourceBarrier(NumBarriers, pBarriers) \
	ResourceBarrier(NumBarriers, pBarriers), \
	RenderProfiler::RecordResourceBarriers(static_cast<UINT>(NumBarriers))
#endif
