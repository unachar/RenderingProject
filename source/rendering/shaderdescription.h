#pragma once
#include "rendererstate.h"

struct ShaderDescription
{

	const char* csoPath{};
	const char* vsPath{};
	const char* psPath{};
	const char* psMrtPath{};
	bool isModel{};
	ID3DBlob** ppBlob{};
	ComPtr<ID3DBlob> vsBlob;
	ComPtr<ID3DBlob> psBlob;
	ComPtr<ID3DBlob> csBlob;
	ID3D12RootSignature* rootSignature{};
	D3D_PRIMITIVE_TOPOLOGY topology{};
	bool frontCounterClockwise{};
	bool enableAlphaBlend{};


	HWND hwnd{};
	UINT width{};
	UINT height{};
	RenderMode renderMode{};
};
