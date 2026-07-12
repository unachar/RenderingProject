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
#include "imguimanager.h"
#include "materialsystem.h"
#include "toonoutlinebuilder.h"
#include <vector>
#include <algorithm>
#include "world.h"

using namespace std;

namespace
{
	const MaterialComponent& DefaultMaterial()
	{
		static const MaterialComponent material{};
		return material;
	}

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

	D3D12_GPU_DESCRIPTOR_HANDLE CreateToonOutlineCbv(UINT8* cbvDataBegin, EntityID entity, const MaterialComponent& material, float widthScale = 1.0f)
	{
		if (!cbvDataBegin)
		{
			return {};
		}

		const auto* source = reinterpret_cast<const ConstantBuffer3D*>(cbvDataBegin + (entity * RendererResource::g_kCB_ALIGNED_SIZE));
		ConstantBuffer3D outlineConstants = *source;
		ApplyToonOutlineConstants(outlineConstants, material, widthScale);
		return RendererResource::AllocateTransientConstantBuffer(outlineConstants);
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

	float GetMeshToonOutlineWidthScale(const MaterialComponent& material, UINT meshIndex)
	{
		if (meshIndex < material.ToonMeshOutlineWidthScales.size())
		{
			return max(material.ToonMeshOutlineWidthScales[meshIndex], 0.0f);
		}
		return 1.0f;
	}

	float GetCameraDepth(EntityID entity, const XMMATRIX& view)
	{
		if (!Registry::HasComponent(entity, ComponentType::TRANSFORM))
		{
			return 0.0f;
		}
		const XMFLOAT3& pos = ComponentManager::GetComponentUnchecked<TransformComponent>(entity).Position;
		const XMVECTOR viewPos = XMVector3TransformCoord(XMLoadFloat3(&pos), view);
		return XMVectorGetZ(viewPos);
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
		auto writeShadowCb = [&](EntityID entity)
			{
				XMMATRIX world = XMLoadFloat4x4(&ComponentManager::GetComponentUnchecked<TransformComponent>(entity).WorldMatrix);
				ConstantBuffer3D cb{};
				cb.World = XMMatrixTranspose(world);
				cb.UseTexture = 0;
				memcpy(pCbvDataBegin + (entity * RendererResource::g_kCB_ALIGNED_SIZE), &cb, sizeof(cb));
				pCommandList->SetGraphicsRootDescriptorTable(0, RendererResource::GetConstantBufferHandle(entity));
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

			pCommandList->SetGraphicsRootSignature(RendererShader::GetModelRootSignature());
			if (RendererResource::GetShadowCB())
			{
				pCommandList->SetGraphicsRootConstantBufferView(5, RendererResource::GetCurrentShadowConstantBufferAddress());
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

	if (renderPass == RenderPass::Velocity)
	{
		ID3D12GraphicsCommandList* commandList = RendererCore::GetCommandList();
		ID3D12PipelineState* pso = PsoManager::GetVelocityGeometryPso();
		if (!commandList || !pso) return;
		XMMATRIX view = XMMatrixIdentity(), projection = XMMatrixIdentity();
		Camera::GetCameraMatrices(Camera::GetCameraEntity(), view, projection);
		XMMATRIX previousViewProjection =
			XMLoadFloat4x4(&RendererCore::GetPreviousViewMatrix()) * XMLoadFloat4x4(&RendererCore::GetPreviousProjectionMatrix());
		commandList->SetGraphicsRootSignature(RendererShader::GetModelRootSignature());
		commandList->SetPipelineState(pso);
		commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		auto setConstants = [&](EntityID entity)
			{
				const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
				ConstantBuffer3D cb{};
				cb.World = XMMatrixTranspose(XMLoadFloat4x4(&transform.WorldMatrix));
				cb.View = XMMatrixTranspose(view);
				cb.Projection = XMMatrixTranspose(projection);
				cb.PreviousWorld = XMMatrixTranspose(XMLoadFloat4x4(&transform.PreviousWorldMatrix));
				cb.PreviousViewProjection = XMMatrixTranspose(previousViewProjection);
				commandList->SetGraphicsRootDescriptorTable(0, RendererResource::AllocateTransientConstantBuffer(cb));
			};

		for (EntityID entity : World::GetView<AnimationModelComponent>())
		{
			auto& component = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity);
			AnimationModelResource* model = ModelManager::GetAnimModel(component.ModelId);
			if (!model || !ComponentManager::HasComponent<TransformComponent>(entity)) continue;
			setConstants(entity);
			for (UINT mesh = 0; mesh < model->GetMeshCount(); ++mesh)
			{
				const MeshData& data = model->GetMeshData(mesh);
				if (!data.VertexBuffer || !data.PreviousVertexValid) continue;
				D3D12_RESOURCE_BARRIER currentReady = CD3DX12_RESOURCE_BARRIER::Transition(data.VertexBuffer.Get(),
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
				commandList->ResourceBarrier(1, &currentReady);
				D3D12_VERTEX_BUFFER_VIEW views[2] = { data.VertexBufferView, data.PreviousVertexBufferView };
				commandList->IASetVertexBuffers(0, 2, views);
				commandList->IASetIndexBuffer(&data.IndexBufferView);
				commandList->DrawIndexedInstanced(data.IndexCount, 1, 0, 0, 0);
				D3D12_RESOURCE_BARRIER currentBack = CD3DX12_RESOURCE_BARRIER::Transition(data.VertexBuffer.Get(),
					D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				commandList->ResourceBarrier(1, &currentBack);
			}
		}

		for (EntityID entity : World::GetView<StaticModelComponent>())
		{
			auto& component = ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity);
			StaticModelResource* model = ModelManager::GetStaticModel(component.ModelId);
			if (!model || !ComponentManager::HasComponent<TransformComponent>(entity)) continue;
			setConstants(entity);
			for (UINT mesh = 0; mesh < model->GetMeshCount(); ++mesh)
			{
				const StaticMeshData& data = model->GetMeshData(mesh);
				D3D12_VERTEX_BUFFER_VIEW views[2] = { data.VertexBufferView, data.VertexBufferView };
				commandList->IASetVertexBuffers(0, 2, views);
				commandList->IASetIndexBuffer(&data.IndexBufferView);
				commandList->DrawIndexedInstanced(data.IndexCount, 1, 0, 0, 0);
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
			const MaterialComponent* material = Registry::HasComponent(i, ComponentType::MATERIAL)
				? &ComponentManager::GetComponentUnchecked<MaterialComponent>(i)
				: nullptr;
			bool isTransparent = material && MaterialSystem::IsTransparentMaterial(*material);
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
			if (material)
			{
				const auto& mat = *material;
				cb.UseTexture = mat.UseTexture ? 1 : 0;
				cb.MaterialMetallic = mat.Metallic;
				cb.MaterialRoughness = mat.Roughness;
				cb.MaterialFresnel = mat.Fresnel;
				cb.MaterialAlpha = mat.Alpha;
				cb.MaterialIsTransparent = mat.IsTransparent ? 1 : 0;
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
			const float cameraDepth = drawTransparent ? GetCameraDepth(i, viewMat) : 0.0f;
			m_AnimDrawCalls.push_back({ i, pso, srvIndex, normalSrvIndex, model, material, cameraDepth });
		}

		sort(m_AnimDrawCalls.begin(), m_AnimDrawCalls.end(), [drawTransparent](const AnimDrawCall& a, const AnimDrawCall& b)
			{
				if (drawTransparent)
				{
					if (fabsf(a.cameraDepth - b.cameraDepth) > 0.0001f)
					{
						return a.cameraDepth > b.cameraDepth;
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
		ID3D12PipelineState* outlinePso = PsoManager::GetOrCreateToonOutlinePso(drawTransparent);

		for (const auto& dc : m_AnimDrawCalls)
		{
			dc.model->DispatchGpuSkinning(pCommandList);
			lastPso = nullptr;

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
			const MaterialComponent& material = dc.material ? *dc.material : DefaultMaterial();
			RendererResource::SetMaterial(dc.EntityID, material);

			if (dc.pso != lastPso)
			{
				pCommandList->SetPipelineState(dc.pso);
				lastPso = dc.pso;
			}

			D3D12_GPU_DESCRIPTOR_HANDLE cbvHandle = RendererResource::GetConstantBufferHandle(dc.EntityID);
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
					const float meshWidthScale = GetMeshToonOutlineWidthScale(material, m);
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
						const D3D12_GPU_DESCRIPTOR_HANDLE outlineCbvHandle = CreateToonOutlineCbv(pCbvDataBegin, dc.EntityID, material, meshWidthScale);
						if (outlineCbvHandle.ptr != 0)
						{
							pCommandList->SetGraphicsRootDescriptorTable(0, outlineCbvHandle);
						}
						else
						{
							pCommandList->SetGraphicsRootDescriptorTable(0, cbvHandle);
						}
						pCommandList->IASetVertexBuffers(0, 1, &meshData.VertexBufferView);
						pCommandList->IASetIndexBuffer(&meshData.IndexBufferView);
						pCommandList->DrawIndexedInstanced(meshData.IndexCount, 1, 0, 0, 0);
					}
					if (drawTeo)
					{
						const D3D12_GPU_DESCRIPTOR_HANDLE outlineCbvHandle = CreateToonOutlineCbv(pCbvDataBegin, dc.EntityID, material, meshWidthScale * material.ToonOutlineTeoWidthScale);
						if (outlineCbvHandle.ptr != 0)
						{
							pCommandList->SetGraphicsRootDescriptorTable(0, outlineCbvHandle);
						}
						else
						{
							pCommandList->SetGraphicsRootDescriptorTable(0, cbvHandle);
						}
						pCommandList->IASetVertexBuffers(0, 1, &meshData.TeoVertexBufferViews[teoMode]);
						pCommandList->IASetIndexBuffer(&meshData.TeoIndexBufferViews[teoMode]);
						pCommandList->DrawIndexedInstanced(meshData.TeoIndexCounts[teoMode], 1, 0, 0, 0);
					}
					pCommandList->SetGraphicsRootDescriptorTable(0, cbvHandle);
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
			const MaterialComponent* material = Registry::HasComponent(i, ComponentType::MATERIAL)
				? &ComponentManager::GetComponentUnchecked<MaterialComponent>(i)
				: nullptr;
			bool isTransparent = material && MaterialSystem::IsTransparentMaterial(*material);
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
			if (material)
			{
				const auto& mat = *material;
				cb.UseTexture = mat.UseTexture ? 1 : 0;
				cb.MaterialMetallic = mat.Metallic;
				cb.MaterialRoughness = mat.Roughness;
				cb.MaterialFresnel = mat.Fresnel;
				cb.MaterialAlpha = mat.Alpha;
				cb.MaterialIsTransparent = mat.IsTransparent ? 1 : 0;
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
			const float cameraDepth = drawTransparent ? GetCameraDepth(i, viewMat) : 0.0f;
			m_StaticDrawCalls.push_back({ i, pso, srvIndex, normalSrvIndex, model, material, cameraDepth });
		}

		sort(m_StaticDrawCalls.begin(), m_StaticDrawCalls.end(), [drawTransparent](const StaticDrawCall& a, const StaticDrawCall& b)
			{
				if (drawTransparent)
				{
					if (fabsf(a.cameraDepth - b.cameraDepth) > 0.0001f)
					{
						return a.cameraDepth > b.cameraDepth;
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
		ID3D12PipelineState* outlinePso = PsoManager::GetOrCreateToonOutlinePso(drawTransparent);

		for (const auto& dc : m_StaticDrawCalls)
		{
			const MaterialComponent& material = dc.material ? *dc.material : DefaultMaterial();
			RendererResource::SetMaterial(dc.EntityID, material);

			if (dc.pso != lastPso)
			{
				pCommandList->SetPipelineState(dc.pso);
				lastPso = dc.pso;
			}

			D3D12_GPU_DESCRIPTOR_HANDLE cbvHandle = RendererResource::GetConstantBufferHandle(dc.EntityID);
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
					const float meshWidthScale = GetMeshToonOutlineWidthScale(material, m);
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
						const D3D12_GPU_DESCRIPTOR_HANDLE outlineCbvHandle = CreateToonOutlineCbv(pCbvDataBegin, dc.EntityID, material, meshWidthScale);
						if (outlineCbvHandle.ptr != 0)
						{
							pCommandList->SetGraphicsRootDescriptorTable(0, outlineCbvHandle);
						}
						else
						{
							pCommandList->SetGraphicsRootDescriptorTable(0, cbvHandle);
						}
						pCommandList->IASetVertexBuffers(0, 1, &meshData.VertexBufferView);
						pCommandList->IASetIndexBuffer(&meshData.IndexBufferView);
						pCommandList->DrawIndexedInstanced(meshData.IndexCount, 1, 0, 0, 0);
					}
					if (drawTeo)
					{
						const D3D12_GPU_DESCRIPTOR_HANDLE outlineCbvHandle = CreateToonOutlineCbv(pCbvDataBegin, dc.EntityID, material, meshWidthScale * material.ToonOutlineTeoWidthScale);
						if (outlineCbvHandle.ptr != 0)
						{
							pCommandList->SetGraphicsRootDescriptorTable(0, outlineCbvHandle);
						}
						else
						{
							pCommandList->SetGraphicsRootDescriptorTable(0, cbvHandle);
						}
						pCommandList->IASetVertexBuffers(0, 1, &meshData.TeoVertexBufferViews[teoMode]);
						pCommandList->IASetIndexBuffer(&meshData.TeoIndexBufferViews[teoMode]);
						pCommandList->DrawIndexedInstanced(meshData.TeoIndexCounts[teoMode], 1, 0, 0, 0);
					}
					pCommandList->SetGraphicsRootDescriptorTable(0, cbvHandle);
					pCommandList->SetPipelineState(dc.pso);
					lastPso = dc.pso;
				}

			}
		}
	}
}

