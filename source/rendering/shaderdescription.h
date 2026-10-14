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
    D3D12_CULL_MODE cullMode = D3D12_CULL_MODE_BACK;
    bool depthWrite = true;


	HWND hwnd{};
	UINT width{};
	UINT height{};
	RenderMode renderMode{};
};
