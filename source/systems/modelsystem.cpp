#include "pch.h"
#include "modelsystem.h"
#include "gpudrivenindirect.h"
#include "instancingsystem.h"
#include "componentmanager.h"
#include "modelmanager.h"
#include "texturemanager.h"
#include "systemmanager.h"
#include "graphicsdevice.h"
#include "shaderpipelines.h"
#include "psomanager.h"
#include "camera.h"
#include "imguimanager.h"
#include "materialsystem.h"
#include "toonoutlinebuilder.h"
#include "meshshaderpipeline.h"
#include <vector>
#include <algorithm>
#include "world.h"
#include "shaderdescription.h"
#include "scenepasses.h"
#include "materialbindings.h"
#include "frameconstants.h"
#include "lightingresources.h"
#include "renderconfiguration.h"


	static const MaterialComponent& DefaultMaterial()
	{
		static const MaterialComponent material{};
		return material;
	}

	static bool IsSkyEntity(EntityID entity)
	{
		return ComponentManager::HasComponent<NameComponent>(entity) &&
			ComponentManager::GetComponentUnchecked<NameComponent>(entity).Name == "Sky";
	}

	static bool ShouldCastShadow(EntityID entity)
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

	static bool ShouldDrawToonOutline(EntityID entity)
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
			max(static_cast<float>(GraphicsDevice::GetSceneWidth()), 1.0f),
			max(static_cast<float>(GraphicsDevice::GetSceneHeight()), 1.0f)
		};
		cb.ToonOutlineUseScreenSpace =
			static_cast<int>(material.ToonOutlineWidthModeSetting == ToonOutlineWidthMode::ScreenPixels);
	}

	D3D12_GPU_DESCRIPTOR_HANDLE CreateToonOutlineCbv(UINT8* cbvDataBegin, EntityID entity, const MaterialComponent& material, float widthScale = 1.0f)
	{
		if (!cbvDataBegin)
		{
			return {};
		}

		const auto* source = reinterpret_cast<const ConstantBuffer3D*>(cbvDataBegin + (entity * FrameConstants::g_kCB_ALIGNED_SIZE));
		ConstantBuffer3D outlineConstants = *source;
		ApplyToonOutlineConstants(outlineConstants, material, widthScale);
		return FrameConstants::AllocateTransientConstantBuffer(outlineConstants);
	}

	int GetTeoModeIndex(const MaterialComponent& material)
	{
		return clamp(static_cast<int>(material.ToonTeoRenderMode), 0, kToonOutlineModeCount - 1);
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

	static float GetCameraDepth(EntityID entity, const XMMATRIX& view)
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


void ModelDrawBackend::Draw(RenderPass renderPass, bool receivingPostProcessOnly)
{
	if (renderPass == RenderPass::ShadowMap)
	{
		ID3D12GraphicsCommandList* pCommandList = GraphicsDevice::GetCommandList();
		if (!pCommandList)
		{
			return;
		}

		ID3D12PipelineState* shadowPso = PsoManager::GetOrCreateShadowMapPso();
		UINT8* pCbvDataBegin = FrameConstants::GetConstantBufferPtr();
		ID3D12DescriptorHeap* cbvHeap = FrameConstants::GetCbvHeap();
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
				memcpy(pCbvDataBegin + (entity * FrameConstants::g_kCB_ALIGNED_SIZE), &cb, sizeof(cb));
				pCommandList->SetGraphicsRootDescriptorTable(0, FrameConstants::GetConstantBufferHandle(entity));
			};

		for (EntityID i : World::GetView<AnimationModelComponent>())
		{
			auto& animComp = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(i);
			if (animComp.ModelId < 0 || !ShouldCastShadow(i) ||
				!LightingResources::ShouldDrawEntityInCurrentShadowPass(i))
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

			pCommandList->SetGraphicsRootSignature(ShaderPipelines::GetModelRootSignature());
			if (LightingResources::GetShadowCB())
			{
				pCommandList->SetGraphicsRootConstantBufferView(5, LightingResources::GetCurrentShadowConstantBufferAddress());
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
			if (staticComp.ModelId < 0 || !ShouldCastShadow(i) ||
				!LightingResources::ShouldDrawEntityInCurrentShadowPass(i))
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
		ID3D12GraphicsCommandList* commandList = GraphicsDevice::GetCommandList();
		ID3D12PipelineState* pso = PsoManager::GetVelocityGeometryPso();
		if (!commandList || !pso) return;
		XMMATRIX view = XMMatrixIdentity(), projection = XMMatrixIdentity();
		Camera::GetCameraMatrices(Camera::GetCameraEntity(), view, projection);
		XMMATRIX previousViewProjection =
			XMLoadFloat4x4(&GraphicsDevice::GetPreviousViewMatrix()) * XMLoadFloat4x4(&GraphicsDevice::GetPreviousProjectionMatrix());
		commandList->SetGraphicsRootSignature(ShaderPipelines::GetModelRootSignature());
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
				commandList->SetGraphicsRootDescriptorTable(0, FrameConstants::AllocateTransientConstantBuffer(cb));
			};

		for (EntityID entity : World::GetView<AnimationModelComponent>())
		{
			auto& component = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity);
			AnimationModelResource* model = ModelManager::GetAnimModel(component.ModelId);
			if (!model || !ComponentManager::HasComponent<TransformComponent>(entity)) continue;

			model->DispatchGpuSkinning(commandList);
			commandList->SetGraphicsRootSignature(ShaderPipelines::GetModelRootSignature());
			commandList->SetPipelineState(pso);
			commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
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
	const bool occlusionPhaseTwo = renderPass == RenderPass::OcclusionPhase2;
	ID3D12GraphicsCommandList* pCommandList = GraphicsDevice::GetCommandList();
	if (!pCommandList)
	{
		return;
	}

	ScenePasses::BeginModelPass();
	UINT8* pCbvDataBegin = FrameConstants::GetConstantBufferPtr();
	UINT cbvIncrement = FrameConstants::GetCbvIncrementSize();
	auto heapStart = FrameConstants::GetCbvHeap()->GetGPUDescriptorHandleForHeapStart();
	const int defaultTextureIndex = TextureManager::GetDefaultTextureIndex();
	const int defaultNormalIndex = TextureManager::GetDefaultTextureIndex();

	XMMATRIX viewMat;
	XMMATRIX projMat;
	Camera::GetCameraMatrices(Camera::GetCameraEntity(), viewMat, projMat);
	const XMMATRIX transposedView = XMMatrixTranspose(viewMat);
	const XMMATRIX transposedProj = XMMatrixTranspose(projMat);
	const XMMATRIX viewProjection = viewMat * projMat;

	{
		m_AnimDrawCalls.clear();
		m_AnimDrawCalls.reserve(World::GetView<AnimationModelComponent>().size());

		auto animEntities = World::GetView<AnimationModelComponent>();
		for (EntityID i : animEntities)
		{
			auto& animComp = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(i);
			if (animComp.ModelId < 0)
			{
				continue;
			}

			bool isReceiving = MaterialSystem::IsReceivingPostProcess(i);
			const MaterialComponent* material;
			if (Registry::HasComponent(i, ComponentType::MATERIAL))
			{
				material = &ComponentManager::GetComponentUnchecked<MaterialComponent>(i);
			}
			else
			{
				material = nullptr;
			}
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
			if (InstancingSystem::CanInstance(i)) continue;

			ShaderDescription psoResource{};
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
				cb.UseTexture = static_cast<int>(mat.UseTexture);
				cb.MaterialMetallic = mat.Metallic;
				cb.MaterialRoughness = mat.Roughness;
				cb.MaterialFresnel = mat.Fresnel;
				cb.MaterialAlpha = mat.Alpha;
				cb.MaterialIsTransparent = static_cast<int>(mat.IsTransparent);
				cb.MaterialMode = static_cast<int>(mat.ShaderClassMode);
				cb.ShaderClass = static_cast<int>(mat.ShaderClass);
				ApplyToonOutlineConstants(cb, mat);
				cb.MaterialPadding = static_cast<float>(IsSkyEntity(i));
				cb.FlipNormal = static_cast<int>(IsSkyEntity(i));
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

			memcpy(pCbvDataBegin + (i * FrameConstants::g_kCB_ALIGNED_SIZE), &cb, sizeof(cb));
			float cameraDepth;
			if (drawTransparent)
			{
				cameraDepth = GetCameraDepth(i, viewMat);
			}
			else
			{
				cameraDepth = 0.0f;
			}
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
				else
				{
					if (fabsf(a.cameraDepth - b.cameraDepth) > 0.0001f)
					{
						return a.cameraDepth < b.cameraDepth;
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

		vector<D3D12_RESOURCE_BARRIER> barriers;
		barriers.reserve(128);

		for (const auto& dc : m_AnimDrawCalls)
		{
			TextureManager::TouchTexture(dc.srvIndex);
			TextureManager::TouchTexture(dc.normalSrvIndex);
			if (InstancingSystem::CanInstance(dc.EntityID))
			{
				continue;
			}
			dc.model->DispatchGpuSkinning(pCommandList);
			lastPso = nullptr;

			barriers.clear();
			for (UINT m = 0; m < dc.model->GetMeshCount(); m++)
			{
				const MeshData& meshData = dc.model->GetMeshData(m);
				if (!meshData.VertexBuffer)
				{
					continue;
				}

				barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
					meshData.VertexBuffer.Get(),
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
					D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER));

				for (int teoMode = 0; teoMode < kToonOutlineModeCount; ++teoMode)
				{
					if (!meshData.TeoVertexBuffers[teoMode])
					{
						continue;
					}
					barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
						meshData.TeoVertexBuffers[teoMode].Get(),
						D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
						D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER));
				}
			}
			if (!barriers.empty())
			{
				pCommandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
			}

			ScenePasses::BeginModelPass();
			const MaterialComponent* materialSelection = nullptr;
			if (dc.material)
			{
				materialSelection = &(*dc.material);
			}
			else
			{
				materialSelection = &(DefaultMaterial());
			}
			const MaterialComponent& material = *materialSelection;
			MaterialBindings::SetMaterial(dc.EntityID, material);

			if (dc.pso != lastPso)
			{
				pCommandList->SetPipelineState(dc.pso);
				lastPso = dc.pso;
			}

			D3D12_GPU_DESCRIPTOR_HANDLE cbvHandle = FrameConstants::GetConstantBufferHandle(dc.EntityID);
			pCommandList->SetGraphicsRootDescriptorTable(0, cbvHandle);

			CD3DX12_GPU_DESCRIPTOR_HANDLE srvHandle(heapStart, dc.srvIndex, cbvIncrement);
			pCommandList->SetGraphicsRootDescriptorTable(1, srvHandle);
			CD3DX12_GPU_DESCRIPTOR_HANDLE normalSrvHandle(heapStart, dc.normalSrvIndex, cbvIncrement);
			pCommandList->SetGraphicsRootDescriptorTable(6, normalSrvHandle);

			const XMMATRIX world = XMLoadFloat4x4(
				&ComponentManager::GetComponentUnchecked<TransformComponent>(dc.EntityID).WorldMatrix);
			const bool indirectBaseDrawn = m_IndirectDraws && m_IndirectDraws->ExecutePrimary(
				pCommandList,
				dc.model,
				dc.EntityID,
				world,
				viewProjection,
				dc.srvIndex,
				heapStart,
				cbvIncrement,
				dc.pso);

			for (UINT m = 0; m < dc.model->GetMeshCount(); m++)
			{
				const MeshData& meshData = dc.model->GetMeshData(m);

				if (meshData.TextureIndex >= 0)
				{
					TextureManager::TouchTexture(meshData.TextureIndex);
					CD3DX12_GPU_DESCRIPTOR_HANDLE meshSrvHandle(heapStart, meshData.TextureIndex, cbvIncrement);
					pCommandList->SetGraphicsRootDescriptorTable(1, meshSrvHandle);
				}
				else
				{
					CD3DX12_GPU_DESCRIPTOR_HANDLE entitySrvHandle(heapStart, dc.srvIndex, cbvIncrement);
					pCommandList->SetGraphicsRootDescriptorTable(1, entitySrvHandle);
				}
				if (!indirectBaseDrawn && !occlusionPhaseTwo)
				{
					pCommandList->SetPipelineState(dc.pso);
					pCommandList->IASetVertexBuffers(0, 1, &meshData.VertexBufferView);
					pCommandList->IASetIndexBuffer(&meshData.IndexBufferView);
					pCommandList->DrawIndexedInstanced(meshData.IndexCount, 1, 0, 0, 0);
				}

				if (!occlusionPhaseTwo && outlinePso && ShouldDrawToonOutline(dc.EntityID) && ShouldDrawMeshToonOutline(material, m, meshData))
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

			barriers.clear();
			for (UINT m = 0; m < dc.model->GetMeshCount(); m++)
			{
				const MeshData& meshData = dc.model->GetMeshData(m);
				if (!meshData.VertexBuffer)
				{
					continue;
				}

				barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
					meshData.VertexBuffer.Get(),
					D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

				for (int teoMode = 0; teoMode < kToonOutlineModeCount; ++teoMode)
				{
					if (!meshData.TeoVertexBuffers[teoMode])
					{
						continue;
					}
					barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
						meshData.TeoVertexBuffers[teoMode].Get(),
						D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
						D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
				}
			}
			if (!barriers.empty())
			{
				pCommandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
			}
		}
	}

	{
		m_StaticDrawCalls.clear();
		m_StaticDrawCalls.reserve(World::GetView<StaticModelComponent>().size());

		auto staticEntities = World::GetView<StaticModelComponent>();
		for (EntityID i : staticEntities)
		{
			if (InstancingSystem::CanInstance(i)) continue;
			auto& staticComp = ComponentManager::GetComponentUnchecked<StaticModelComponent>(i);
			if (staticComp.ModelId < 0)
			{
				continue;
			}

			bool isReceiving = MaterialSystem::IsReceivingPostProcess(i);
			const MaterialComponent* material;
			if (Registry::HasComponent(i, ComponentType::MATERIAL))
			{
				material = &ComponentManager::GetComponentUnchecked<MaterialComponent>(i);
			}
			else
			{
				material = nullptr;
			}
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

			ShaderDescription psoResource{};
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
				cb.UseTexture = static_cast<int>(mat.UseTexture);
				cb.MaterialMetallic = mat.Metallic;
				cb.MaterialRoughness = mat.Roughness;
				cb.MaterialFresnel = mat.Fresnel;
				cb.MaterialAlpha = mat.Alpha;
				cb.MaterialIsTransparent = static_cast<int>(mat.IsTransparent);
				cb.MaterialMode = static_cast<int>(mat.ShaderClassMode);
				cb.ShaderClass = static_cast<int>(mat.ShaderClass);
				ApplyToonOutlineConstants(cb, mat);
				cb.MaterialPadding = static_cast<float>(IsSkyEntity(i));
				cb.FlipNormal = static_cast<int>(IsSkyEntity(i));
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

			memcpy(pCbvDataBegin + (i * FrameConstants::g_kCB_ALIGNED_SIZE), &cb, sizeof(cb));
			float cameraDepth;
			if (drawTransparent)
			{
				cameraDepth = GetCameraDepth(i, viewMat);
			}
			else
			{
				cameraDepth = 0.0f;
			}
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
				else
				{
					if (fabsf(a.cameraDepth - b.cameraDepth) > 0.0001f)
					{
						return a.cameraDepth < b.cameraDepth;
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
		const bool useMeshShaders = !drawTransparent &&
			RenderConfiguration::GetRenderMode() == RenderMode::DEFERRED &&
			MeshShaderPipeline::IsSupported();

		for (const auto& dc : m_StaticDrawCalls)
		{
			TextureManager::TouchTexture(dc.srvIndex);
			TextureManager::TouchTexture(dc.normalSrvIndex);
			const MaterialComponent* materialSelection = nullptr;
			if (dc.material)
			{
				materialSelection = &(*dc.material);
			}
			else
			{
				materialSelection = &(DefaultMaterial());
			}
			const MaterialComponent& material = *materialSelection;
			MaterialBindings::SetMaterial(dc.EntityID, material);

			if (dc.pso != lastPso)
			{
				pCommandList->SetPipelineState(dc.pso);
				lastPso = dc.pso;
			}

			D3D12_GPU_DESCRIPTOR_HANDLE cbvHandle = FrameConstants::GetConstantBufferHandle(dc.EntityID);
			pCommandList->SetGraphicsRootDescriptorTable(0, cbvHandle);

			CD3DX12_GPU_DESCRIPTOR_HANDLE srvHandle(heapStart, dc.srvIndex, cbvIncrement);
			pCommandList->SetGraphicsRootDescriptorTable(1, srvHandle);
			CD3DX12_GPU_DESCRIPTOR_HANDLE normalSrvHandle(heapStart, dc.normalSrvIndex, cbvIncrement);
			pCommandList->SetGraphicsRootDescriptorTable(6, normalSrvHandle);

			const XMMATRIX world = XMLoadFloat4x4(
				&ComponentManager::GetComponentUnchecked<TransformComponent>(dc.EntityID).WorldMatrix);
			const bool indirectBaseDrawn = !useMeshShaders && m_IndirectDraws && m_IndirectDraws->ExecutePrimary(
				pCommandList,
				dc.model,
				dc.EntityID,
				world,
				viewProjection,
				dc.srvIndex,
				heapStart,
				cbvIncrement,
				dc.pso);

			for (UINT m = 0; m < dc.model->GetMeshCount(); m++)
			{
				const StaticMeshData& meshData = dc.model->GetMeshData(m);

				if (meshData.TextureIndex >= 0)
				{
					TextureManager::TouchTexture(meshData.TextureIndex);
					CD3DX12_GPU_DESCRIPTOR_HANDLE meshSrvHandle(heapStart, meshData.TextureIndex, cbvIncrement);
					pCommandList->SetGraphicsRootDescriptorTable(1, meshSrvHandle);
				}
				else
				{
					CD3DX12_GPU_DESCRIPTOR_HANDLE entitySrvHandle(heapStart, dc.srvIndex, cbvIncrement);
					pCommandList->SetGraphicsRootDescriptorTable(1, entitySrvHandle);
				}
				const bool meshShaderDrawn = useMeshShaders && MeshShaderPipeline::Draw(
					pCommandList,
					meshData,
					dc.model->GetAabbCenter(),
					dc.model->GetAabbExtents());

				if (!meshShaderDrawn && !indirectBaseDrawn && !occlusionPhaseTwo)
				{
					pCommandList->SetPipelineState(dc.pso);
					pCommandList->IASetVertexBuffers(0, 1, &meshData.VertexBufferView);
					pCommandList->IASetIndexBuffer(&meshData.IndexBufferView);
					pCommandList->DrawIndexedInstanced(meshData.IndexCount, 1, 0, 0, 0);
				}

				if (!occlusionPhaseTwo && outlinePso && ShouldDrawToonOutline(dc.EntityID) && ShouldDrawMeshToonOutline(material, m, meshData))
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
