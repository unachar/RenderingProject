#include "pch.h"
#include "gridsystem.h"
#include "grid.h"
#include "rendererdraw.h"
#include "renderercore.h"
#include "texturemanager.h"
#include "systemmanager.h"
#include "camera.h"

void GridSystem::Draw(RenderPass renderPass, bool receivingPostProcessOnly)
{
	if (renderPass == RenderPass::OverlayScene || renderPass == RenderPass::ShadowMap)
	{
		return;
	}

	const bool isDeferred = RendererCore::GetRenderMode() == RenderMode::DEFERRED;
	if (!isDeferred && !receivingPostProcessOnly)
	{
		return;
	}

	if (!Grid::IsInitialized())
	{
		return;
	}

	ID3D12GraphicsCommandList* pCommandList = RendererCore::GetCommandList();

	ID3D12PipelineState* pso = isDeferred ? Grid::GetDeferredLinePso() : Grid::GetLinePso();
	if (!pso)
	{
		return;
	}

	pCommandList->SetPipelineState(pso);
	RendererDraw::BeginLinePass();

	XMMATRIX viewMat;
	XMMATRIX projMat;
	Camera::GetCameraMatrices(Camera::GetCameraEntity(), viewMat, projMat);

	ConstantBuffer3D cb{};
	cb.World = XMMatrixTranspose(XMMatrixIdentity());
	cb.View = XMMatrixTranspose(viewMat);
	cb.Projection = XMMatrixTranspose(projMat);
	cb.UseTexture = 0;

	UINT cbvIncrement = RendererCore::GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	pCommandList->SetGraphicsRootDescriptorTable(0, RendererResource::AllocateTransientConstantBuffer(cb));

	int srvIndex = TextureManager::GetDefaultTextureIndex();
	CD3DX12_GPU_DESCRIPTOR_HANDLE srvHandle(
		RendererResource::GetCbvHeap()->GetGPUDescriptorHandleForHeapStart(), srvIndex, cbvIncrement);
	pCommandList->SetGraphicsRootDescriptorTable(1, srvHandle);

	pCommandList->IASetVertexBuffers(0, 1, Grid::GetVertexBufferView());
	pCommandList->DrawInstanced(Grid::GetVertexCount(), 1, 0, 0);
}

