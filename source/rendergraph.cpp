#include "pch.h"
#include "rendergraph.h"

#include <limits>
#include <queue>


	bool IsWriteAccess(RenderGraph::Access access)
	{
		return access == RenderGraph::Access::Write ||
			access == RenderGraph::Access::ReadWrite;
	}

	RenderGraph::Access MergeAccess(RenderGraph::Access left, RenderGraph::Access right)
	{
		return left == right ? left : RenderGraph::Access::ReadWrite;
	}

	void AddUniqueDependency(std::vector<uint32_t>& dependencies, uint32_t dependency)
	{
		if (std::find(dependencies.begin(), dependencies.end(), dependency) ==
			dependencies.end())
		{
			dependencies.push_back(dependency);
		}
	}


RenderGraph::PassBuilder& RenderGraph::PassBuilder::Read(
	ResourceHandle resource,
	D3D12_RESOURCE_STATES state)
{
	return Use(resource, state, Access::Read);
}

RenderGraph::PassBuilder& RenderGraph::PassBuilder::Write(
	ResourceHandle resource,
	D3D12_RESOURCE_STATES state)
{
	return Use(resource, state, Access::Write);
}

RenderGraph::PassBuilder& RenderGraph::PassBuilder::ReadWrite(
	ResourceHandle resource,
	D3D12_RESOURCE_STATES state)
{
	return Use(resource, state, Access::ReadWrite);
}

RenderGraph::PassBuilder& RenderGraph::PassBuilder::Use(
	ResourceHandle resource,
	D3D12_RESOURCE_STATES state,
	Access access)
{
	m_Graph.AddUsage(m_PassIndex, resource, state, access);
	return *this;
}

RenderGraph::ResourceHandle RenderGraph::CreateLogicalResource(const char* name)
{
	ImportedResource resource{};
	resource.Name = name;
	resource.AutomaticBarriers = false;
	return ImportResource(resource);
}

RenderGraph::ResourceHandle RenderGraph::ImportResource(const ImportedResource& resource)
{
	ResourceRecord record{};
	record.Name = resource.Name ? resource.Name : "Unnamed Resource";
	record.Resource = resource.Resource;
	record.InitialState = resource.InitialState;
	record.FinalState = resource.FinalState;
	record.HasFinalState = resource.HasFinalState;
	record.AutomaticBarriers = resource.AutomaticBarriers && resource.Resource != nullptr;
	record.Subresource = resource.Subresource;

	const ResourceHandle handle{ static_cast<uint32_t>(m_Resources.size()) };
	m_Resources.push_back(std::move(record));
	m_IsCompiled = false;
	return handle;
}

void RenderGraph::AddPass(
	const char* name,
	const SetupCallback& setup,
	const ExecuteCallback& execute)
{
	PassRecord pass{};
	pass.Name = name ? name : "Unnamed Pass";
	pass.Execute = execute;

	const uint32_t passIndex = static_cast<uint32_t>(m_Passes.size());
	m_Passes.push_back(std::move(pass));
	m_IsCompiled = false;

	if (setup)
	{
		PassBuilder builder(*this, passIndex);
		setup(builder);
	}
}

void RenderGraph::AddUsage(
	uint32_t passIndex,
	ResourceHandle resource,
	D3D12_RESOURCE_STATES state,
	Access access)
{
	if (passIndex >= m_Passes.size())
	{
		Fail("RenderGraph pass builder referenced an invalid pass.");
		return;
	}
	if (!resource.IsValid() || resource.Index >= m_Resources.size())
	{
		Fail("RenderGraph pass '" + m_Passes[passIndex].Name +
			"' referenced an invalid resource.");
		return;
	}

	auto& usages = m_Passes[passIndex].Usages;
	auto existing = std::find_if(
		usages.begin(),
		usages.end(),
		[resource](const Usage& usage)
		{
			return usage.Resource.Index == resource.Index;
		});
	if (existing != usages.end())
	{
		if (existing->State != state)
		{
			Fail(
				"RenderGraph pass '" + m_Passes[passIndex].Name +
				"' uses resource '" + m_Resources[resource.Index].Name +
				"' in multiple states.");
			return;
		}
		existing->AccessMode = MergeAccess(existing->AccessMode, access);
		return;
	}

	usages.push_back({ resource, state, access });
}

bool RenderGraph::Compile()
{
	if (!m_LastError.empty())
	{
		return false;
	}

	m_ExecutionOrder.clear();
	for (auto& pass : m_Passes)
	{
		pass.Dependencies.clear();
		if (!pass.Execute)
		{
			return Fail("RenderGraph pass '" + pass.Name + "' has no execute callback.");
		}
	}

	const uint32_t invalidPass = std::numeric_limits<uint32_t>::max();
	std::vector<uint32_t> lastWriter(m_Resources.size(), invalidPass);
	std::vector<std::vector<uint32_t>> readers(m_Resources.size());

	for (uint32_t passIndex = 0; passIndex < m_Passes.size(); ++passIndex)
	{
		auto& pass = m_Passes[passIndex];
		for (const Usage& usage : pass.Usages)
		{
			const uint32_t resourceIndex = usage.Resource.Index;
			if (resourceIndex >= m_Resources.size())
			{
				return Fail("RenderGraph contains an invalid resource handle.");
			}

			if (lastWriter[resourceIndex] != invalidPass)
			{
				AddUniqueDependency(pass.Dependencies, lastWriter[resourceIndex]);
			}

			if (IsWriteAccess(usage.AccessMode))
			{
				for (const uint32_t reader : readers[resourceIndex])
				{
					AddUniqueDependency(pass.Dependencies, reader);
				}
				readers[resourceIndex].clear();
				lastWriter[resourceIndex] = passIndex;
			}
			else
			{
				readers[resourceIndex].push_back(passIndex);
			}
		}
	}

	std::vector<uint32_t> indegree(m_Passes.size(), 0);
	std::vector<std::vector<uint32_t>> outgoing(m_Passes.size());
	for (uint32_t passIndex = 0; passIndex < m_Passes.size(); ++passIndex)
	{
		indegree[passIndex] = static_cast<uint32_t>(
			m_Passes[passIndex].Dependencies.size());
		for (const uint32_t dependency : m_Passes[passIndex].Dependencies)
		{
			if (dependency >= m_Passes.size() || dependency == passIndex)
			{
				return Fail("RenderGraph generated an invalid pass dependency.");
			}
			outgoing[dependency].push_back(passIndex);
		}
	}

	std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<uint32_t>> ready;
	for (uint32_t passIndex = 0; passIndex < m_Passes.size(); ++passIndex)
	{
		if (indegree[passIndex] == 0)
		{
			ready.push(passIndex);
		}
	}

	while (!ready.empty())
	{
		const uint32_t passIndex = ready.top();
		ready.pop();
		m_ExecutionOrder.push_back(passIndex);
		for (const uint32_t dependent : outgoing[passIndex])
		{
			if (--indegree[dependent] == 0)
			{
				ready.push(dependent);
			}
		}
	}

	if (m_ExecutionOrder.size() != m_Passes.size())
	{
		return Fail("RenderGraph contains a dependency cycle.");
	}

	m_IsCompiled = true;
	return true;
}

bool RenderGraph::Execute(ID3D12GraphicsCommandList* commandList)
{
	if (!m_IsCompiled)
	{
		return Fail("RenderGraph::Execute called before a successful Compile.");
	}

	bool needsCommandList = false;
	for (const ResourceRecord& resource : m_Resources)
	{
		needsCommandList = needsCommandList ||
			(resource.Resource != nullptr && resource.AutomaticBarriers);
	}
	if (needsCommandList && !commandList)
	{
		return Fail("RenderGraph automatic barriers require a command list.");
	}

	std::vector<D3D12_RESOURCE_STATES> states;
	states.reserve(m_Resources.size());
	for (const ResourceRecord& resource : m_Resources)
	{
		states.push_back(resource.InitialState);
	}
	std::vector<bool> previousUavAccess(m_Resources.size(), false);
	std::vector<bool> previousUavWrite(m_Resources.size(), false);

	for (const uint32_t passIndex : m_ExecutionOrder)
	{
		PassRecord& pass = m_Passes[passIndex];
		std::vector<D3D12_RESOURCE_BARRIER> barriers;
		barriers.reserve(pass.Usages.size());

		for (const Usage& usage : pass.Usages)
		{
			const uint32_t resourceIndex = usage.Resource.Index;
			const ResourceRecord& resource = m_Resources[resourceIndex];
			if (!resource.Resource || !resource.AutomaticBarriers)
			{
				continue;
			}

			if (states[resourceIndex] != usage.State)
			{
				barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
					resource.Resource,
					states[resourceIndex],
					usage.State,
					resource.Subresource));
				states[resourceIndex] = usage.State;
				previousUavAccess[resourceIndex] = false;
				previousUavWrite[resourceIndex] = false;
			}
			else if (
				previousUavAccess[resourceIndex] &&
				(previousUavWrite[resourceIndex] ||
					IsWriteAccess(usage.AccessMode)) &&
				(usage.State & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) != 0)
			{
				barriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(resource.Resource));
				previousUavAccess[resourceIndex] = false;
				previousUavWrite[resourceIndex] = false;
			}
		}

		if (!barriers.empty())
		{
			commandList->ResourceBarrier(
				static_cast<UINT>(barriers.size()),
				barriers.data());
		}

		pass.Execute(commandList);

		for (const Usage& usage : pass.Usages)
		{
			const uint32_t resourceIndex = usage.Resource.Index;
			const ResourceRecord& resource = m_Resources[resourceIndex];
			if (!resource.Resource || !resource.AutomaticBarriers)
			{
				continue;
			}
			previousUavAccess[resourceIndex] =
				(usage.State & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) != 0;
			previousUavWrite[resourceIndex] =
				previousUavAccess[resourceIndex] &&
				IsWriteAccess(usage.AccessMode);
		}
	}

	std::vector<D3D12_RESOURCE_BARRIER> finalBarriers;
	finalBarriers.reserve(m_Resources.size());
	for (uint32_t resourceIndex = 0; resourceIndex < m_Resources.size(); ++resourceIndex)
	{
		const ResourceRecord& resource = m_Resources[resourceIndex];
		if (!resource.Resource ||
			!resource.AutomaticBarriers ||
			!resource.HasFinalState ||
			states[resourceIndex] == resource.FinalState)
		{
			continue;
		}
		finalBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
			resource.Resource,
			states[resourceIndex],
			resource.FinalState,
			resource.Subresource));
	}
	if (!finalBarriers.empty())
	{
		commandList->ResourceBarrier(
			static_cast<UINT>(finalBarriers.size()),
			finalBarriers.data());
	}

	return true;
}

void RenderGraph::Reset()
{
	m_Resources.clear();
	m_Passes.clear();
	m_ExecutionOrder.clear();
	m_LastError.clear();
	m_IsCompiled = false;
}

bool RenderGraph::Fail(const std::string& message)
{
	m_LastError = message;
	m_IsCompiled = false;
	Debug::Log("ERROR: %s\n", message.c_str());
	return false;
}
