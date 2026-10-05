#pragma once
#include <cstdint>
#include <vector>

// Test-only command recorder. This does not validate the D3D12 runtime.
using UINT = unsigned int;
using D3D12_RESOURCE_STATES = uint32_t;
constexpr UINT D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES = UINT32_MAX;
constexpr D3D12_RESOURCE_STATES D3D12_RESOURCE_STATE_COMMON = 0;
constexpr D3D12_RESOURCE_STATES D3D12_RESOURCE_STATE_DEPTH_WRITE = 1;
constexpr D3D12_RESOURCE_STATES D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE = 2;
constexpr D3D12_RESOURCE_STATES D3D12_RESOURCE_STATE_UNORDERED_ACCESS = 4;

struct ID3D12Resource {};
struct D3D12_RESOURCE_BARRIER
{
	bool IsUav = false;
	ID3D12Resource* Resource = nullptr;
	D3D12_RESOURCE_STATES Before = 0;
	D3D12_RESOURCE_STATES After = 0;
	UINT Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
};

struct CD3DX12_RESOURCE_BARRIER
{
	static D3D12_RESOURCE_BARRIER Transition(
		ID3D12Resource* resource,
		D3D12_RESOURCE_STATES before,
		D3D12_RESOURCE_STATES after,
		UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
	{
		return { false, resource, before, after, subresource };
	}
	static D3D12_RESOURCE_BARRIER UAV(ID3D12Resource* resource)
	{
		return { true, resource, 0, 0, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES };
	}
};

struct ID3D12GraphicsCommandList
{
	std::vector<D3D12_RESOURCE_BARRIER> Barriers;
	void ResourceBarrier(UINT count, const D3D12_RESOURCE_BARRIER* barriers)
	{
		Barriers.insert(Barriers.end(), barriers, barriers + count);
	}
};
