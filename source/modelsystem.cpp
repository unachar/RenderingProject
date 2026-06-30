#include "pch.h"
#include "modelsystem.h"
#include "componentmanager.h"
#include "modelmanager.h"
#include "texturemanager.h"
#include "systemmanager.h"
#include "rendererdraw.h"
#include "renderercore.h"
#include "renderershader.h"
#include "psomanager.h"
#include "camera.h"
#include "materialsystem.h"
#include "toonoutlinebuilder.h"
#include <vector>
#include <algorithm>
#include "world.h"

using namespace std;

namespace
{
	bool IsSkyEntity(EntityID entity)
	{
		return ComponentManager::HasComponent<NameComponent>(entity) &&
			ComponentManager::GetComponentUnchecked<NameComponent>(entity).Name == "Sky";
	}

	bool ShouldCastShadow(EntityID entity)
	{
		if (IsSkyEntity(entity) || ComponentManager::HasComponent<LightComponent>(entity))
		{
			return false;
		}
		if (!Registry::HasComponent(entity, ComponentType::MATERIAL))
		{
			return true;
		}

		const auto& material = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
		if (MaterialSystem::IsTransparentMaterial(material))
		{
			return false;
		}
		return !(material.ShaderClassMode == MaterialMode::Manual &&
			material.ShaderClass == ShaderClass::Shadow);
	}

	bool ShouldDrawToonOutline(EntityID entity)
	{
		if (!Registry::HasComponent(entity, ComponentType::MATERIAL))
		{
			return false;
		}

		const auto& material = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
		return material.ShaderClassMode == MaterialMode::Auto ||
			(material.ShaderClassMode == MaterialMode::Manual &&
				material.ShaderClass == ShaderClass::Toon);
	}

	void ApplyToonOutlineConstants(ConstantBuffer3D& cb, const MaterialComponent& material, float widthScale = 1.0f)
	{
		cb.ToonOutlineWidth = material.ToonOutlineWidth * widthScale;
		cb.ToonOutlineScreenWidth = material.ToonOutlineScreenWidth * widthScale;
		cb.ViewportSize = {
			max(static_cast<float>(RendererCore::GetSceneWidth()), 1.0f),
			max(static_cast<float>(RendererCore::GetSceneHeight()), 1.0f)
		};
		cb.ToonOutlineUseScreenSpace =
			material.ToonOutlineWidthModeSetting == ToonOutlineWidthMode::ScreenPixels ? 1 : 0;
	}

	void SetMappedToonOutlineParams(UINT8* cbvDataBegin, EntityID entity, const MaterialComponent& material, float widthScale = 1.0f)
	{
		if (!cbvDataBegin)
		{
			return;
		}

		auto* cb = reinterpret_cast<ConstantBuffer3D*>(cbvDataBegin + (entity * RendererResource::g_kCB_ALIGNED_SIZE));
		ApplyToonOutlineConstants(*cb, material, widthScale);
	}

	int GetTeoModeIndex(const MaterialComponent& material)
	{
		return std::clamp(static_cast<int>(material.ToonTeoRenderMode), 0, ToonOutlineBuilder::kModeCount - 1);
	}

	template <class TMeshData>
	bool ShouldDrawMeshToonOutline(const MaterialComponent& material, UINT meshIndex, const TMeshData& meshData)
	{
		if (meshIndex < material.ToonMeshOutlineOverrides.size())
		{
			switch (material.ToonMeshOutlineOverrides[meshIndex])
			{
			case MeshOutlineOverride::ForceOn:
				return true;
			case MeshOutlineOverride::ForceOff:
				return false;
			default:
				break;
			}
		}
		return meshData.DefaultToonOutlineEnabled;
	}

	float GetCameraDistanceSq(EntityID entity, const XMFLOAT3& cameraPos)
	{
		if (!Registry::HasComponent(entity, ComponentType::TRANSFORM))
		{
			return 0.0f;
		}
		const XMFLOAT3& pos = ComponentManager::GetComponentUnchecked<TransformComponent>(entity).Position;
		const float dx = pos.x - cameraPos.x;
		const float dy = pos.y - cameraPos.y;
		const float dz = pos.z - cameraPos.z;
		return dx * dx + dy * dy + dz * dz;
	}

	const char* GetModelVsPath(EntityID entity)
	{
		if (ComponentManager::HasComponent<ShaderComponent>(entity))
		{
			const auto& shader = ComponentManager::GetComponentUnchecked<ShaderComponent>(entity);
			if (!shader.VsPath.empty())
			{
				return shader.VsPath.c_str();
			}
		}
		return "shader/hlsl/build/modelshaderVS.cso";
	}

	const char* GetModelPsPath(EntityID entity)
	{
		if (ComponentManager::HasComponent<ShaderComponent>(entity))
		{
			const auto& shader = ComponentManager::GetComponentUnchecked<ShaderComponent>(entity);
			if (!shader.PsPath.empty())
			{
				return shader.PsPath.c_str();
			}
		}
		return "shader/hlsl/build/modelshaderPS.cso";
	}
}

void ModelSystem::Draw(RenderPass renderPass, bool receivingPostProcessOnly)
{
	if (renderPass == RenderPass::ShadowMap)
	{
		ID3D12GraphicsCommandList* pCommandList = RendererCore::GetCommandList();
		if (!pCommandList)
		{
			return;
		}

		ID3D12PipelineState* shadowPso = PsoManager::GetOrCreateShadowMapPso();
		UINT8* pCbvDataBegin = RendererResource::GetConstantBufferPtr();
		ID3D12DescriptorHeap* cbvHeap = RendererResource::GetCbvHeap();
		if (!shadowPso || !pCbvDataBegin || !cbvHeap)
		{
			return;
		}

		pCommandList->SetPipelineState(shadowPso);
		const UINT cbvIncrement = RendererResource::GetCbvIncrementSize();
		auto heapStart = cbvHeap->GetGPUDescriptorHandleForHeapStart();

		auto writeShadowCb = [&](EntityID entity)
			{
				XMMATRIX world = XMLoadFloat4x4(&ComponentManager::GetComponentUnchecked<TransformComponent>(entity).WorldMatrix);
				ConstantBuffer3D cb{};
				cb.World = XMMatrixTranspose(world);
				cb.UseTexture = 0;
				memcpy(pCbvDataBegin + (entity * RendererResource::g_kCB_ALIGNED_SIZE), &cb, sizeof(cb));
				CD3DX12_GPU_DESCRIPTOR_HANDLE cbvHandle(heapStart, entity, cbvIncrement);
				pCommandList->SetGraphicsRootDescriptorTable(0, cbvHandle);
			};

		for (EntityID i : World::GetView<AnimationModelComponent>())
		{
			auto& animComp = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(i);
			if (animComp.ModelId < 0 || !ShouldCastShadow(i))
			{
				continue;
			}

			AnimationModelResource* model = ModelManager::GetAnimModel(animComp.ModelId);
			if (!model)
			{
				continue;
			}

			model->DispatchGpuSkinning(pCommandList);
			for (UINT m = 0; m < model->GetMeshCount(); ++m)
			{
				const MeshData& meshData = model->GetMeshData(m);
				if (!meshData.VertexBuffer)
				{
					continue;
				}
				D3D12_RESOURCE_BARRIER vbBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
					meshData.VertexBuffer.Get(),
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
					D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
				pCommandList->ResourceBarrier(1, &vbBarrier);
			}

			pCommandList->SetPipelineState(shadowPso);
			writeShadowCb(i);
			for (UINT m = 0; m < model->GetMeshCount(); ++m)
			{
				const MeshData& meshData = model->GetMeshData(m);
				pCommandList->IASetVertexBuffers(0, 1, &meshData.VertexBufferView);
				pCommandList->IASetIndexBuffer(&meshData.IndexBufferView);
				pCommandList->DrawIndexedInstanced(meshData.IndexCount, 1, 0, 0, 0);

				D3D12_RESOURCE_BARRIER backBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
					meshData.VertexBuffer.Get(),
					D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				pCommandList->ResourceBarrier(1, &backBarrier);
			}
		}

		for (EntityID i : World::GetView<StaticModelComponent>())
		{
			auto& staticComp = ComponentManager::GetComponentUnchecked<StaticModelComponent>(i);
			if (staticComp.ModelId < 0 || !ShouldCastShadow(i))
			{
				continue;
			}

			StaticModelResource* model = ModelManager::GetStaticModel(staticComp.ModelId);
			if (!model)
			{
				continue;
			}

			writeShadowCb(i);
			for (UINT m = 0; m < model->GetMeshCount(); ++m)
			{
				const StaticMeshData& meshData = model->GetMeshData(m);
				pCommandList->IASetVertexBuffers(0, 1, &meshData.VertexBufferView);
				pCommandList->IASetIndexBuffer(&meshData.IndexBufferView);
				pCommandList->DrawIndexedInstanced(meshData.IndexCount, 1, 0, 0, 0);
			}
		}
		return;
	}

	const bool drawTransparent = (renderPass == RenderPass::OverlayScene);
	ID3D12GraphicsCommandList* pCommandList = RendererCore::GetCommandList();
	if (!pCommandList)
	{
		return;
	}

	RendererDraw::BeginModelPass();
	UINT8* pCbvDataBegin = RendererResource::GetConstantBufferPtr();
	UINT cbvIncrement = RendererResource::GetCbvIncrementSize();
	auto heapStart = RendererResource::GetCbvHeap()->GetGPUDescriptorHandleForHeapStart();
	const int defaultTextureIndex = TextureManager::GetDefaultTextureIndex();
	const int defaultNormalIndex = TextureManager::GetDefaultTextureIndex();

	XMMATRIX viewMat;
	XMMATRIX projMat;
	Camera::GetCameraMatrices(Camera::GetCameraEntity(), viewMat, projMat);
	const XMMATRIX transposedView = XMMatrixTranspose(viewMat);
	const XMMATRIX transposedProj = XMMatrixTranspose(projMat);
	XMFLOAT3 cameraPos = { 0.0f, 0.0f, 5.0f };
	const EntityID cameraEntity = Camera::GetCameraEntity();
	if (Registry::HasComponent(cameraEntity, ComponentType::TRANSFORM))
	{
		cameraPos = ComponentManager::GetComponentUnchecked<TransformComponent>(cameraEntity).Position;
	}

	{
		m_AnimDrawCalls.clear();

		auto animEntities = World::GetView<AnimationModelComponent>();
		for (EntityID i : animEntities)
		{
			auto& animComp = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(i);
			if (animComp.ModelId < 0)
			{
				continue;
			}

			bool isReceiving = MaterialSystem::IsReceivingPostProcess(i);
			bool hasMaterial = Registry::HasComponent(i, ComponentType::MATERIAL);
			bool isTransparent = hasMaterial && MaterialSystem::IsTransparentMaterial(ComponentManager::GetComponentUnchecked<MaterialComponent>(i));
			if (isTransparent != drawTransparent)
			{
				continue;
			}
			if (receivingPostProcessOnly && !isReceiving)
			{
				continue;
			}
			AnimationModelResource* model = ModelManager::GetAnimModel(animComp.ModelId);
			if (!model)
			{
				continue;
			}

			rendererResource psoResource{};
			psoResource.vsPath = GetModelVsPath(i);
			psoResource.psPath = GetModelPsPath(i);
			psoResource.isModel = true;
			psoResource.enableAlphaBlend = drawTransparent;
			ID3D12PipelineState* pso = PsoManager::GetOrCreateGraphicsPso(psoResource);
			if (!pso)
			{
				continue;
			}

			XMMATRIX world = XMLoadFloat4x4(&ComponentManager::GetComponentUnchecked<TransformComponent>(i).WorldMatrix);

			ConstantBuffer3D cb{};
			cb.World = XMMatrixTranspose(world);
			cb.View = transposedView;
			cb.Projection = transposedProj;
			cb.UseTexture = 0;

			int srvIndex = defaultTextureIndex;
			int normalSrvIndex = defaultNormalIndex;
			if (hasMaterial)
			{
				auto& mat = ComponentManager::GetComponentUnchecked<MaterialComponent>(i);
				cb.UseTexture = mat.UseTexture ? 1 : 0;
				cb.MaterialMetallic = mat.Metallic;
				cb.MaterialRoughness = mat.Roughness;
				cb.MaterialFresnel = mat.Fresnel;
				cb.MaterialAlpha = mat.Alpha;
				cb.MaterialMode = static_cast<int>(mat.ShaderClassMode);
				cb.ShaderClass = static_cast<int>(mat.ShaderClass);
				ApplyToonOutlineConstants(cb, mat);
				cb.MaterialPadding = IsSkyEntity(i) ? 1.0f : 0.0f;
				cb.FlipNormal = IsSkyEntity(i) ? 1 : 0;
				if (cb.UseTexture != 0 && mat.TextureID >= 0)
				{
					srvIndex = mat.TextureID;
				}
				if (mat.NormalMapID >= 0)
				{
					cb.UseNormalMap = 1;
					normalSrvIndex = mat.NormalMapID;
				}
			}

			memcpy(pCbvDataBegin + (i * RendererResource::g_kCB_ALIGNED_SIZE), &cb, sizeof(cb));
			m_AnimDrawCalls.push_back({ i, pso, srvIndex, normalSrvIndex, model });
		}

		sort(m_AnimDrawCalls.begin(), m_AnimDrawCalls.end(), [drawTransparent, cameraPos](const AnimDrawCall& a, const AnimDrawCall& b)
			{
				if (drawTransparent)
				{
					const float da = GetCameraDistanceSq(a.EntityID, cameraPos);
					const float db = GetCameraDistanceSq(b.EntityID, cameraPos);
					if (fabsf(da - db) > 0.0001f)
					{
						return da > db;
					}
				}
				if (a.pso != b.pso)
				{
					return a.pso < b.pso;
				}
				if (a.srvIndex != b.srvIndex)
				{
					return a.srvIndex < b.srvIndex;
				}
				return a.model < b.model;
			});

		ID3D12PipelineState* lastPso = nullptr;
		ID3D12PipelineState* outlinePso = PsoManager::GetOrCreateToonOutlinePso();

		for (const auto& dc : m_AnimDrawCalls)
		{
			dc.model->DispatchGpuSkinning(pCommandList);

			for (UINT m = 0; m < dc.model->GetMeshCount(); m++)
			{
				const MeshData& meshData = dc.model->GetMeshData(m);
				if (!meshData.VertexBuffer)
				{
					continue;
				}

				D3D12_RESOURCE_BARRIER vbBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
					meshData.VertexBuffer.Get(),
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
					D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
				pCommandList->ResourceBarrier(1, &vbBarrier);

				for (int teoMode = 0; teoMode < ToonOutlineBuilder::kModeCount; ++teoMode)
				{
					if (!meshData.TeoVertexBuffers[teoMode])
					{
						continue;
					}
					D3D12_RESOURCE_BARRIER teoVbBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
						meshData.TeoVertexBuffers[teoMode].Get(),
						D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
						D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
					pCommandList->ResourceBarrier(1, &teoVbBarrier);
				}
			}

			RendererDraw::BeginModelPass();
			MaterialComponent material{};
			if (Registry::HasComponent(dc.EntityID, ComponentType::MATERIAL))
			{
				material = ComponentManager::GetComponentUnchecked<MaterialComponent>(dc.EntityID);
			}
			RendererResource::SetMaterial(dc.EntityID, material);

			if (dc.pso != lastPso)
			{
				pCommandList->SetPipelineState(dc.pso);
				lastPso = dc.pso;
			}

			CD3DX12_GPU_DESCRIPTOR_HANDLE cbvHandle(heapStart, dc.EntityID, cbvIncrement);
			pCommandList->SetGraphicsRootDescriptorTable(0, cbvHandle);

			CD3DX12_GPU_DESCRIPTOR_HANDLE srvHandle(heapStart, dc.srvIndex, cbvIncrement);
			pCommandList->SetGraphicsRootDescriptorTable(1, srvHandle);
			CD3DX12_GPU_DESCRIPTOR_HANDLE normalSrvHandle(heapStart, dc.normalSrvIndex, cbvIncrement);
			pCommandList->SetGraphicsRootDescriptorTable(6, normalSrvHandle);

			for (UINT m = 0; m < dc.model->GetMeshCount(); m++)
			{
				const MeshData& meshData = dc.model->GetMeshData(m);

				if (meshData.TextureIndex >= 0)
				{
					CD3DX12_GPU_DESCRIPTOR_HANDLE meshSrvHandle(heapStart, meshData.TextureIndex, cbvIncrement);
					pCommandList->SetGraphicsRootDescriptorTable(1, meshSrvHandle);
				}
				else
				{
					CD3DX12_GPU_DESCRIPTOR_HANDLE entitySrvHandle(heapStart, dc.srvIndex, cbvIncrement);
					pCommandList->SetGraphicsRootDescriptorTable(1, entitySrvHandle);
				}

				pCommandList->IASetVertexBuffers(0, 1, &meshData.VertexBufferView);
				pCommandList->IASetIndexBuffer(&meshData.IndexBufferView);
				pCommandList->DrawIndexedInstanced(meshData.IndexCount, 1, 0, 0, 0);

				if (outlinePso && ShouldDrawToonOutline(dc.EntityID) && ShouldDrawMeshToonOutline(material, m, meshData))
				{
					const int teoMode = GetTeoModeIndex(material);
					const bool hasTeoMesh =
						meshData.TeoVertexBuffers[teoMode] &&
						meshData.TeoIndexBuffers[teoMode] &&
						meshData.TeoIndexCounts[teoMode] > 0;
					const bool drawExtrude =
						material.ToonOutlineRenderMode == ToonOutlineMode::Extrude ||
						material.ToonOutlineRenderMode == ToonOutlineMode::Mix ||
						(material.ToonOutlineRenderMode == ToonOutlineMode::TEO && !hasTeoMesh);
					const bool drawTeo =
						hasTeoMesh &&
						(material.ToonOutlineRenderMode == ToonOutlineMode::TEO ||
							material.ToonOutlineRenderMode == ToonOutlineMode::Mix);

					pCommandList->SetPipelineState(outlinePso);
					if (drawExtrude)
					{
						SetMappedToonOutlineParams(pCbvDataBegin, dc.EntityID, material);
						pCommandList->IASetVertexBuffers(0, 1, &meshData.VertexBufferView);
						pCommandList->IASetIndexBuffer(&meshData.IndexBufferView);
						pCommandList->DrawIndexedInstanced(meshData.IndexCount, 1, 0, 0, 0);
					}
					if (drawTeo)
					{
						SetMappedToonOutlineParams(pCbvDataBegin, dc.EntityID, material, material.ToonOutlineTeoWidthScale);
						pCommandList->IASetVertexBuffers(0, 1, &meshData.TeoVertexBufferViews[teoMode]);
						pCommandList->IASetIndexBuffer(&meshData.TeoIndexBufferViews[teoMode]);
						pCommandList->DrawIndexedInstanced(meshData.TeoIndexCounts[teoMode], 1, 0, 0, 0);
					}
					SetMappedToonOutlineParams(pCbvDataBegin, dc.EntityID, material);
					pCommandList->SetPipelineState(dc.pso);
					lastPso = dc.pso;
				}

				D3D12_RESOURCE_BARRIER backBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
					meshData.VertexBuffer.Get(),
					D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				pCommandList->ResourceBarrier(1, &backBarrier);

				for (int teoMode = 0; teoMode < ToonOutlineBuilder::kModeCount; ++teoMode)
				{
					if (!meshData.TeoVertexBuffers[teoMode])
					{
						continue;
					}
					D3D12_RESOURCE_BARRIER teoBackBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
						meshData.TeoVertexBuffers[teoMode].Get(),
						D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
						D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
					pCommandList->ResourceBarrier(1, &teoBackBarrier);
				}
			}
		}
	}

	{
		m_StaticDrawCalls.clear();

		auto staticEntities = World::GetView<StaticModelComponent>();
		for (EntityID i : staticEntities)
		{
			auto& staticComp = ComponentManager::GetComponentUnchecked<StaticModelComponent>(i);
			if (staticComp.ModelId < 0)
			{
				continue;
			}

			bool isReceiving = MaterialSystem::IsReceivingPostProcess(i);
			bool hasMaterial = Registry::HasComponent(i, ComponentType::MATERIAL);
			bool isTransparent = hasMaterial && MaterialSystem::IsTransparentMaterial(ComponentManager::GetComponentUnchecked<MaterialComponent>(i));
			if (isTransparent != drawTransparent)
			{
				continue;
			}
			if (receivingPostProcessOnly && !isReceiving)
			{
				continue;
			}
			StaticModelResource* model = ModelManager::GetStaticModel(staticComp.ModelId);
			if (!model)
			{
				continue;
			}

			rendererResource psoResource{};
			psoResource.vsPath = GetModelVsPath(i);
			psoResource.psPath = GetModelPsPath(i);
			psoResource.isModel = true;
			psoResource.enableAlphaBlend = drawTransparent;
			ID3D12PipelineState* pso = PsoManager::GetOrCreateGraphicsPso(psoResource);
			if (!pso)
			{
				continue;
			}

			XMMATRIX world = XMLoadFloat4x4(&ComponentManager::GetComponentUnchecked<TransformComponent>(i).WorldMatrix);

			ConstantBuffer3D cb{};
			cb.World = XMMatrixTranspose(world);
			cb.View = transposedView;
			cb.Projection = transposedProj;
			cb.UseTexture = 0;

			int srvIndex = defaultTextureIndex;
			int normalSrvIndex = defaultNormalIndex;
			if (hasMaterial)
			{
				auto& mat = ComponentManager::GetComponentUnchecked<MaterialComponent>(i);
				cb.UseTexture = mat.UseTexture ? 1 : 0;
				cb.MaterialMetallic = mat.Metallic;
				cb.MaterialRoughness = mat.Roughness;
				cb.MaterialFresnel = mat.Fresnel;
				cb.MaterialAlpha = mat.Alpha;
				cb.MaterialMode = static_cast<int>(mat.ShaderClassMode);
				cb.ShaderClass = static_cast<int>(mat.ShaderClass);
				ApplyToonOutlineConstants(cb, mat);
				cb.MaterialPadding = IsSkyEntity(i) ? 1.0f : 0.0f;
				cb.FlipNormal = IsSkyEntity(i) ? 1 : 0;
				if (cb.UseTexture != 0 && mat.TextureID >= 0)
				{
					srvIndex = mat.TextureID;
				}
				if (mat.NormalMapID >= 0)
				{
					cb.UseNormalMap = 1;
					normalSrvIndex = mat.NormalMapID;
				}
			}

			memcpy(pCbvDataBegin + (i * RendererResource::g_kCB_ALIGNED_SIZE), &cb, sizeof(cb));
			m_StaticDrawCalls.push_back({ i, pso, srvIndex, normalSrvIndex, model });
		}

		sort(m_StaticDrawCalls.begin(), m_StaticDrawCalls.end(), [drawTransparent, cameraPos](const StaticDrawCall& a, const StaticDrawCall& b)
			{
				if (drawTransparent)
				{
					const float da = GetCameraDistanceSq(a.EntityID, cameraPos);
					const float db = GetCameraDistanceSq(b.EntityID, cameraPos);
					if (fabsf(da - db) > 0.0001f)
					{
						return da > db;
					}
				}
				if (a.pso != b.pso)
				{
					return a.pso < b.pso;
				}
				if (a.srvIndex != b.srvIndex)
				{
					return a.srvIndex < b.srvIndex;
				}
				return a.model < b.model;
			});

		ID3D12PipelineState* lastPso = nullptr;
		ID3D12PipelineState* outlinePso = PsoManager::GetOrCreateToonOutlinePso();

		for (const auto& dc : m_StaticDrawCalls)
		{
			MaterialComponent material{};
			if (Registry::HasComponent(dc.EntityID, ComponentType::MATERIAL))
			{
				material = ComponentManager::GetComponentUnchecked<MaterialComponent>(dc.EntityID);
			}
			RendererResource::SetMaterial(dc.EntityID, material);

			if (dc.pso != lastPso)
			{
				pCommandList->SetPipelineState(dc.pso);
				lastPso = dc.pso;
			}

			CD3DX12_GPU_DESCRIPTOR_HANDLE cbvHandle(heapStart, dc.EntityID, cbvIncrement);
			pCommandList->SetGraphicsRootDescriptorTable(0, cbvHandle);

			CD3DX12_GPU_DESCRIPTOR_HANDLE srvHandle(heapStart, dc.srvIndex, cbvIncrement);
			pCommandList->SetGraphicsRootDescriptorTable(1, srvHandle);
			CD3DX12_GPU_DESCRIPTOR_HANDLE normalSrvHandle(heapStart, dc.normalSrvIndex, cbvIncrement);
			pCommandList->SetGraphicsRootDescriptorTable(6, normalSrvHandle);

			for (UINT m = 0; m < dc.model->GetMeshCount(); m++)
			{
				const StaticMeshData& meshData = dc.model->GetMeshData(m);

				if (meshData.TextureIndex >= 0)
				{
					CD3DX12_GPU_DESCRIPTOR_HANDLE meshSrvHandle(heapStart, meshData.TextureIndex, cbvIncrement);
					pCommandList->SetGraphicsRootDescriptorTable(1, meshSrvHandle);
				}
				else
				{
					CD3DX12_GPU_DESCRIPTOR_HANDLE entitySrvHandle(heapStart, dc.srvIndex, cbvIncrement);
					pCommandList->SetGraphicsRootDescriptorTable(1, entitySrvHandle);
				}

				pCommandList->IASetVertexBuffers(0, 1, &meshData.VertexBufferView);
				pCommandList->IASetIndexBuffer(&meshData.IndexBufferView);
				pCommandList->DrawIndexedInstanced(meshData.IndexCount, 1, 0, 0, 0);

				if (outlinePso && ShouldDrawToonOutline(dc.EntityID) && ShouldDrawMeshToonOutline(material, m, meshData))
				{
					const int teoMode = GetTeoModeIndex(material);
					const bool hasTeoMesh =
						meshData.TeoVertexBuffers[teoMode] &&
						meshData.TeoIndexBuffers[teoMode] &&
						meshData.TeoIndexCounts[teoMode] > 0;
					const bool drawExtrude =
						material.ToonOutlineRenderMode == ToonOutlineMode::Extrude ||
						material.ToonOutlineRenderMode == ToonOutlineMode::Mix ||
						(material.ToonOutlineRenderMode == ToonOutlineMode::TEO && !hasTeoMesh);
					const bool drawTeo =
						hasTeoMesh &&
						(material.ToonOutlineRenderMode == ToonOutlineMode::TEO ||
							material.ToonOutlineRenderMode == ToonOutlineMode::Mix);

					pCommandList->SetPipelineState(outlinePso);
					if (drawExtrude)
					{
						SetMappedToonOutlineParams(pCbvDataBegin, dc.EntityID, material);
						pCommandList->IASetVertexBuffers(0, 1, &meshData.VertexBufferView);
						pCommandList->IASetIndexBuffer(&meshData.IndexBufferView);
						pCommandList->DrawIndexedInstanced(meshData.IndexCount, 1, 0, 0, 0);
					}
					if (drawTeo)
					{
						SetMappedToonOutlineParams(pCbvDataBegin, dc.EntityID, material, material.ToonOutlineTeoWidthScale);
						pCommandList->IASetVertexBuffers(0, 1, &meshData.TeoVertexBufferViews[teoMode]);
						pCommandList->IASetIndexBuffer(&meshData.TeoIndexBufferViews[teoMode]);
						pCommandList->DrawIndexedInstanced(meshData.TeoIndexCounts[teoMode], 1, 0, 0, 0);
					}
					SetMappedToonOutlineParams(pCbvDataBegin, dc.EntityID, material);
					pCommandList->SetPipelineState(dc.pso);
					lastPso = dc.pso;
				}
			}
		}
	}
}

