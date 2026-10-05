#include "pch.h"
#include "rendergraph.h"
#include <iostream>
#include <stdexcept>

void Require(bool condition, const char* message)
{
	if (!condition)
	{
		throw runtime_error(message);
	}
}

int main()
{
	try
	{
		{
			RenderGraph graph;
			vector<int> calls;
			auto resource = graph.CreateLogicalResource("scene");
			graph.AddPass("write", [&](auto& builder) { builder.Write(resource); }, [&](auto*) { calls.push_back(1); });
			graph.AddPass("read", [&](auto& builder) { builder.Read(resource); }, [&](auto*) { calls.push_back(2); });
			graph.AddPass("rewrite", [&](auto& builder) { builder.Write(resource); }, [&](auto*) { calls.push_back(3); });
			Require(graph.Compile() && graph.Execute(nullptr), "logical graph failed");
			Require(calls == vector<int>({ 1, 2, 3 }), "read/write ordering changed");
		}
		{
			RenderGraph graph;
			auto resource = graph.CreateLogicalResource(nullptr);
			graph.AddPass(nullptr, [&](auto& builder) { builder.Read(resource).Write(resource); }, [](auto*) {});
			Require(graph.Compile() && graph.Execute(nullptr), "access merge or unnamed resource failed");
		}
		{
			RenderGraph graph;
			graph.AddPass("invalid", [](auto& builder) { builder.Read({}); }, [](auto*) {});
			Require(!graph.Compile(), "invalid handle was accepted");
			Require(graph.GetLastError().find("invalid resource") != string::npos, "invalid handle diagnostic changed");
		}
		{
			RenderGraph graph;
			graph.AddPass("missing", {}, {});
			Require(!graph.Compile(), "missing callback was accepted");
		}
		{
			RenderGraph graph;
			Require(!graph.Execute(nullptr), "uncompiled graph executed");
			graph.Reset();
			Require(graph.GetLastError().empty() && graph.GetPassCount() == 0, "reset retained old graph");
			graph.AddPass("rebuild", {}, [](auto*) {});
			Require(graph.Compile() && graph.Execute(nullptr), "reset graph could not rebuild");
		}
		{
			RenderGraph graph;
			ID3D12Resource resource;
			ID3D12GraphicsCommandList commands;
			RenderGraph::ImportedResource imported{};
			imported.Resource = &resource;
			imported.HasFinalState = true;
			imported.Subresource = 3;
			auto handle = graph.ImportResource(imported);
			graph.AddPass("depth", [&](auto& builder) { builder.Write(handle, D3D12_RESOURCE_STATE_DEPTH_WRITE); }, [](auto*) {});
			graph.AddPass("sample", [&](auto& builder) { builder.Read(handle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); }, [](auto*) {});
			Require(graph.Compile() && graph.Execute(&commands), "barrier graph failed");
			Require(commands.Barriers.size() == 3, "transition or final barrier missing");
			Require(commands.Barriers[0].Before == D3D12_RESOURCE_STATE_COMMON && commands.Barriers[0].After == D3D12_RESOURCE_STATE_DEPTH_WRITE, "initial transition changed");
			Require(commands.Barriers[1].Before == D3D12_RESOURCE_STATE_DEPTH_WRITE && commands.Barriers[1].After == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, "read transition changed");
			Require(commands.Barriers[2].After == D3D12_RESOURCE_STATE_COMMON, "final state not restored");
			for (const auto& barrier : commands.Barriers)
			{
				Require(barrier.Resource == &resource && barrier.Subresource == 3, "barrier resource/subresource changed");
			}
		}
		{
			RenderGraph graph;
			ID3D12Resource resource;
			ID3D12GraphicsCommandList commands;
			RenderGraph::ImportedResource imported{};
			imported.Resource = &resource;
			auto handle = graph.ImportResource(imported);
			graph.AddPass("write UAV", [&](auto& builder) { builder.Write(handle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); }, [](auto*) {});
			graph.AddPass("read UAV", [&](auto& builder) { builder.Read(handle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); }, [](auto*) {});
			graph.AddPass("read again", [&](auto& builder) { builder.Read(handle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); }, [](auto*) {});
			Require(graph.Compile() && graph.Execute(&commands), "UAV graph failed");
			Require(commands.Barriers.size() == 2 && !commands.Barriers[0].IsUav && commands.Barriers[1].IsUav, "UAV ordering barrier changed");
		}
		{
			RenderGraph graph;
			ID3D12Resource resource;
			RenderGraph::ImportedResource imported{};
			imported.Resource = &resource;
			graph.ImportResource(imported);
			Require(graph.Compile() && !graph.Execute(nullptr), "automatic barriers accepted null command list");
		}
		{
			RenderGraph graph;
			ID3D12Resource resource;
			RenderGraph::ImportedResource imported{};
			imported.Resource = &resource;
			imported.AutomaticBarriers = false;
			auto handle = graph.ImportResource(imported);
			graph.AddPass("manual", [&](auto& builder) { builder.Write(handle, D3D12_RESOURCE_STATE_DEPTH_WRITE); }, [](auto*) {});
			Require(graph.Compile() && graph.Execute(nullptr), "manual barriers unexpectedly require a command list");
		}
		{
			RenderGraph graph;
			auto handle = graph.CreateLogicalResource("conflict");
			graph.AddPass("conflict", [&](auto& builder) { builder.Read(handle, D3D12_RESOURCE_STATE_DEPTH_WRITE).Write(handle, D3D12_RESOURCE_STATE_COMMON); }, [](auto*) {});
			Require(!graph.Compile(), "conflicting states in one pass were accepted");
		}
		cout << "PASS: 10 RenderGraph behavior scenarios (CPU command recorder only)\n";
	}
	catch (const exception& error)
	{
		cerr << error.what() << '\n';
		return 1;
	}
	return 0;
}
