#pragma once

#include <d3d12.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>


class RenderGraph
{
public:
	struct ResourceHandle
	{
		static constexpr uint32_t InvalidIndex = UINT32_MAX;
		uint32_t Index = InvalidIndex;

		bool IsValid() const { return Index != InvalidIndex; }
	};

	enum class Access : uint8_t
	{
		Read,
		Write,
		ReadWrite
	};

	struct ImportedResource
	{
		const char* Name = nullptr;
		ID3D12Resource* Resource = nullptr;
		D3D12_RESOURCE_STATES InitialState = D3D12_RESOURCE_STATE_COMMON;
		D3D12_RESOURCE_STATES FinalState = D3D12_RESOURCE_STATE_COMMON;
		bool HasFinalState = false;
		bool AutomaticBarriers = true;
		UINT Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	};

	class PassBuilder
	{
	public:
		PassBuilder& Read(
			ResourceHandle resource,
			D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON);
		PassBuilder& Write(
			ResourceHandle resource,
			D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON);
		PassBuilder& ReadWrite(
			ResourceHandle resource,
			D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON);

	private:
		friend class RenderGraph;
		PassBuilder(RenderGraph& graph, uint32_t passIndex)
			: m_Graph(graph), m_PassIndex(passIndex)
		{
		}

		PassBuilder& Use(
			ResourceHandle resource,
			D3D12_RESOURCE_STATES state,
			Access access);

		RenderGraph& m_Graph;
		uint32_t m_PassIndex;
	};

	using SetupCallback = std::function<void(PassBuilder&)>;
	using ExecuteCallback = std::function<void(ID3D12GraphicsCommandList*)>;


	ResourceHandle CreateLogicalResource(const char* name);



	ResourceHandle ImportResource(const ImportedResource& resource);

	void AddPass(
		const char* name,
		const SetupCallback& setup,
		const ExecuteCallback& execute);

	bool Compile();
	bool Execute(ID3D12GraphicsCommandList* commandList);
	void Reset();

	const std::string& GetLastError() const { return m_LastError; }
	size_t GetPassCount() const { return m_Passes.size(); }
	const std::vector<uint32_t>& GetExecutionOrder() const { return m_ExecutionOrder; }

private:
	struct Usage
	{
		ResourceHandle Resource;
		D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATE_COMMON;
		Access AccessMode = Access::Read;
	};

	struct ResourceRecord
	{
		std::string Name;
		ID3D12Resource* Resource = nullptr;
		D3D12_RESOURCE_STATES InitialState = D3D12_RESOURCE_STATE_COMMON;
		D3D12_RESOURCE_STATES FinalState = D3D12_RESOURCE_STATE_COMMON;
		bool HasFinalState = false;
		bool AutomaticBarriers = true;
		UINT Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	};

	struct PassRecord
	{
		std::string Name;
		std::vector<Usage> Usages;
		std::vector<uint32_t> Dependencies;
		ExecuteCallback Execute;
	};

	void AddUsage(
		uint32_t passIndex,
		ResourceHandle resource,
		D3D12_RESOURCE_STATES state,
		Access access);
	bool Fail(const std::string& message);

	std::vector<ResourceRecord> m_Resources;
	std::vector<PassRecord> m_Passes;
	std::vector<uint32_t> m_ExecutionOrder;
	std::string m_LastError;
	bool m_IsCompiled = false;
};
