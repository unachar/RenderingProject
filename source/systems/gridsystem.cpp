#include "pch.h"
#include "gridsystem.h"
#include "grid.h"
#include "graphicsdevice.h"
#include "texturemanager.h"
#include "systemmanager.h"
#include "camera.h"
#include "scenepasses.h"
#include "frameconstants.h"
#include "renderconfiguration.h"

void GridSystem::Draw(RenderPass renderPass, bool receivingPostProcessOnly)
{
	if (renderPass == RenderPass::OverlayScene || renderPass == RenderPass::ShadowMap ||
		renderPass == RenderPass::Velocity || renderPass == RenderPass::OcclusionPhase2)
	{
		return;
	}

	const bool isDeferred = RenderConfiguration::GetRenderMode() == RenderMode::DEFERRED;
	if (!isDeferred && !receivingPostProcessOnly)
	{
		return;
	}

	if (!Grid::IsInitialized())
	{
		return;
	}

	ID3D12GraphicsCommandList* pCommandList = GraphicsDevice::GetCommandList();

	ID3D12PipelineState* pso;
	if (isDeferred)
	{
		pso = Grid::GetDeferredLinePso();
	}
	else
	{
		pso = Grid::GetLinePso();
	}
	if (!pso)
	{
		return;
	}

	pCommandList->SetPipelineState(pso);
	ScenePasses::BeginLinePass();

	XMMATRIX viewMat;
	XMMATRIX projMat;
	Camera::GetCameraMatrices(Camera::GetCameraEntity(), viewMat, projMat);

	ConstantBuffer3D cb{};
	cb.World = XMMatrixTranspose(XMMatrixIdentity());
	cb.View = XMMatrixTranspose(viewMat);
	cb.Projection = XMMatrixTranspose(projMat);
	cb.UseTexture = 0;

	UINT cbvIncrement = GraphicsDevice::GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	pCommandList->SetGraphicsRootDescriptorTable(0, FrameConstants::AllocateTransientConstantBuffer(cb));

	int srvIndex = TextureManager::GetDefaultTextureIndex();
	CD3DX12_GPU_DESCRIPTOR_HANDLE srvHandle(
		FrameConstants::GetCbvHeap()->GetGPUDescriptorHandleForHeapStart(), srvIndex, cbvIncrement);
	pCommandList->SetGraphicsRootDescriptorTable(1, srvHandle);

	pCommandList->IASetVertexBuffers(0, 1, Grid::GetVertexBufferView());
	pCommandList->DrawInstanced(Grid::GetVertexCount(), 1, 0, 0);
}

