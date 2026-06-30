#include "pch.h"
#include "rendersystem.h"
#include "rendererdraw.h"
#include "renderercore.h"
#include "renderershader.h"
#include "psomanager.h"
#include "componentmanager.h"
#include "texturemanager.h"
#include "systemmanager.h"
#include "materialsystem.h"
#include "camera.h"
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
		if (Registry::HasComponent(entity, ComponentType::MESH) &&
			!Registry::HasComponent(entity, ComponentType::STATIC_MODEL) &&
			!Registry::HasComponent(entity, ComponentType::ANIMATION_MODEL))
		{
			return material.ShaderClassMode == MaterialMode::Manual &&
				material.ShaderClass == ShaderClass::Toon;
		}
		if (Registry::HasComponent(entity, ComponentType::SPRITE) &&
			ComponentManager::GetComponentUnchecked<SpriteComponent>(entity).Is3D)
		{
			return material.ShaderClassMode == MaterialMode::Manual &&
				material.ShaderClass == ShaderClass::Toon;
		}

		return material.ShaderClassMode == MaterialMode::Auto ||
			(material.ShaderClassMode == MaterialMode::Manual &&
				material.ShaderClass == ShaderClass::Toon);
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

	const char* GetSpriteVsPath(EntityID entity, bool is3D)
	{
		if (ComponentManager::HasComponent<ShaderComponent>(entity))
		{
			const auto& shader = ComponentManager::GetComponentUnchecked<ShaderComponent>(entity);
			if (!shader.VsPath.empty())
			{
				return shader.VsPath.c_str();
			}
		}
		return is3D ? "shader/hlsl/build/colorshader3dVS.cso" : "shader/hlsl/build/colorshaderVS.cso";
	}

	const char* GetSpritePsPath(EntityID entity, bool is3D)
	{
		if (ComponentManager::HasComponent<ShaderComponent>(entity))
		{
			const auto& shader = ComponentManager::GetComponentUnchecked<ShaderComponent>(entity);
			if (!shader.PsPath.empty())
			{
				return shader.PsPath.c_str();
			}
		}
		return is3D ? "shader/hlsl/build/colorshader3dPS.cso" : "shader/hlsl/build/colorshaderPS.cso";
	}

	const char* GetMeshVsPath(EntityID entity)
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

	const char* GetMeshPsPath(EntityID entity)
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

void RenderSystem::Draw(RenderPass renderPass, bool receivingPostProcessOnly)
{
	if (renderPass == RenderPass::ShadowMap)
	{
		ID3D12GraphicsCommandList* pCommandList = RendererCore::GetCommandList();
		ID3D12PipelineState* shadowPso = PsoManager::GetOrCreateShadowMapPso();
		UINT8* pCbvDataBegin = RendererResource::GetConstantBufferPtr();
		ID3D12DescriptorHeap* cbvHeap = RendererResource::GetCbvHeap();
		if (!pCommandList || !shadowPso || !pCbvDataBegin || !cbvHeap)
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

		for (EntityID i : World::GetView<SpriteComponent>())
		{
			const auto& sprite = ComponentManager::GetComponentUnchecked<SpriteComponent>(i);
			if (!sprite.Is3D ||
				sprite.VertexBufferView.BufferLocation == 0 ||
				!Registry::HasComponent(i, ComponentType::TRANSFORM) ||
				!ShouldCastShadow(i))
			{
				continue;
			}

			writeShadowCb(i);
			pCommandList->IASetVertexBuffers(0, 1, &sprite.VertexBufferView);
			pCommandList->DrawInstanced(sprite.VertexCount, 1, 0, 0);
		}

		for (EntityID i : World::GetView<MeshComponent>())
		{
			if (Registry::HasComponent(i, ComponentType::STATIC_MODEL) ||
				Registry::HasComponent(i, ComponentType::ANIMATION_MODEL))
			{
				continue;
			}

			const auto& mesh = ComponentManager::GetComponentUnchecked<MeshComponent>(i);
			if (mesh.VertexBufferView.BufferLocation == 0 ||
				!Registry::HasComponent(i, ComponentType::TRANSFORM) ||
				!ShouldCastShadow(i))
			{
				continue;
			}

			writeShadowCb(i);
			pCommandList->IASetVertexBuffers(0, 1, &mesh.VertexBufferView);
			pCommandList->DrawInstanced(mesh.VertexCount, 1, 0, 0);
		}
		return;
	}

	const bool drawTransparent = (renderPass == RenderPass::OverlayScene);
	const bool drawDeferredOpaque = RendererCore::GetRenderMode() == RenderMode::DEFERRED && renderPass == RenderPass::PrimaryScene;
	ID3D12GraphicsCommandList* pCommandList = RendererCore::GetCommandList();
	if (!pCommandList)
	{
		return;
	}

	ID3D12DescriptorHeap* pCbvHeap = RendererResource::GetCbvHeap();
	if (!pCbvHeap)
	{
		return;
	}

	RendererDraw::BeginSpritePass();

	UINT8* pCbvDataBegin = RendererResource::GetConstantBufferPtr();
	if (!pCbvDataBegin)
	{
		return;
	}

	UINT descriptorIncrement = RendererResource::GetCbvIncrementSize();

	auto spriteEntities = World::GetView<SpriteComponent>();
	const size_t spriteCapacity = Registry::GetActiveEntities(ComponentType::SPRITE).size();
	m_SpriteDrawCalls.clear();
	m_ModelDrawCalls.clear();
	m_SpriteDrawCalls.reserve(spriteCapacity);
	m_ModelDrawCalls.reserve(spriteCapacity);

	XMMATRIX viewMat = XMMatrixIdentity();
	XMMATRIX projMat = XMMatrixIdentity();
	Camera::GetCameraMatrices(Camera::GetCameraEntity(), viewMat, projMat);
	const XMMATRIX transposedView = XMMatrixTranspose(viewMat);
	const XMMATRIX transposedProj = XMMatrixTranspose(projMat);
	XMFLOAT3 cameraPos = { 0.0f, 0.0f, 5.0f };
	const EntityID activeCameraEntity = Camera::GetCameraEntity();
	if (Registry::HasComponent(activeCameraEntity, ComponentType::TRANSFORM))
	{
		cameraPos = ComponentManager::GetComponentUnchecked<TransformComponent>(activeCameraEntity).Position;
	}
	const float sceneAspectRatio = RendererCore::GetSceneAspectRatio();
	const int defaultTextureIndex = TextureManager::GetDefaultTextureIndex();
	const int defaultNormalIndex = TextureManager::GetDefaultTextureIndex();

	for (EntityID i : spriteEntities)
	{
		auto& sprite = ComponentManager::GetComponentUnchecked<SpriteComponent>(i);
		bool isReceiving = MaterialSystem::IsReceivingPostProcess(i);
		bool hasMaterial = Registry::HasComponent(i, ComponentType::MATERIAL);
		bool isTransparent = hasMaterial && MaterialSystem::IsTransparentMaterial(ComponentManager::GetComponentUnchecked<MaterialComponent>(i));
		if (isTransparent != drawTransparent)
		{
			continue;
		}
		if (!drawTransparent && !drawDeferredOpaque && isReceiving != receivingPostProcessOnly)
		{
			continue;
		}

		if (sprite.VertexBufferView.BufferLocation != 0)
		{
			if (sprite.Is3D)
			{
				if (!Registry::HasComponent(i, ComponentType::TRANSFORM))
				{
					continue;
				}

				RendererDraw::BeginModelPass();
				rendererResource psoResource{};
				psoResource.vsPath = GetSpriteVsPath(i, true);
				psoResource.psPath = GetSpritePsPath(i, true);
				psoResource.isModel = true;
				psoResource.enableAlphaBlend = drawTransparent;
				ID3D12PipelineState* pso = PsoManager::GetOrCreateGraphicsPso(psoResource);
				if (!pso)
				{
					continue;
				}

				XMMATRIX world = XMLoadFloat4x4(&ComponentManager::GetComponentUnchecked<TransformComponent>(i).WorldMatrix);

				ConstantBuffer3D cb3D{};
				cb3D.World = XMMatrixTranspose(world);
				cb3D.View = transposedView;
				cb3D.Projection = transposedProj;
				cb3D.UseTexture = 0;

				// Get camera position from camera entity
				EntityID cameraEntity = Camera::GetCameraEntity();
				if (Registry::HasComponent(cameraEntity, ComponentType::TRANSFORM))
				{
					cb3D.CameraPos = ComponentManager::GetComponentUnchecked<TransformComponent>(cameraEntity).Position;
				}
				else
				{
					cb3D.CameraPos = { 0.0f, 0.0f, 5.0f };
				}

				int srvIndex = defaultTextureIndex;
				int normalSrvIndex = defaultNormalIndex;
				if (hasMaterial)
				{
					auto& mat = ComponentManager::GetComponentUnchecked<MaterialComponent>(i);
					cb3D.UseTexture = mat.UseTexture ? 1 : 0;
					cb3D.MaterialMetallic = mat.Metallic;
					cb3D.MaterialRoughness = mat.Roughness;
					cb3D.MaterialFresnel = mat.Fresnel;
					cb3D.MaterialAlpha = mat.Alpha;
					cb3D.MaterialMode = static_cast<int>(mat.ShaderClassMode);
					cb3D.ShaderClass = static_cast<int>(mat.ShaderClass);
					cb3D.ToonOutlineWidth = mat.ToonOutlineWidth;
					if (cb3D.UseTexture != 0 && mat.TextureID >= 0)
					{
						srvIndex = mat.TextureID;
					}
					if (mat.NormalMapID >= 0)
					{
						cb3D.UseNormalMap = 1;
						normalSrvIndex = mat.NormalMapID;
					}
				}
				if (srvIndex >= g_kMAX_ENTITIES + RendererResource::g_kMAX_SRVS)
				{
					continue;
				}

				memcpy(pCbvDataBegin + (i * RendererResource::g_kCB_ALIGNED_SIZE), &cb3D, sizeof(cb3D));
				m_ModelDrawCalls.push_back(DrawCall{ i, pso, srvIndex, normalSrvIndex, sprite.VertexBufferView, sprite.VertexCount, true });
				continue;
			}

			RendererDraw::BeginSpritePass();
			rendererResource psoResource{};
			psoResource.vsPath = GetSpriteVsPath(i, false);
			psoResource.psPath = GetSpritePsPath(i, false);
			psoResource.isModel = false;
			psoResource.enableAlphaBlend = drawTransparent;
			ID3D12PipelineState* pso = PsoManager::GetOrCreateGraphicsPso(psoResource);
			if (!pso)
			{
				continue;
			}

			struct CbData
			{
				float x, y;
				int UseTexture;
				float AspectRatio;
				float ScaleX, ScaleY;
				int UseNormalMap;
				int MaterialMode;
				int ShaderClass;
				float MaterialAlpha;
				float Pad0, Pad1;
			} cbData = { 0, 0, 0, 1.0f, 1.0f, 1.0f, 0, 0, 10, 1.0f, 0, 0 };
			if (Registry::HasComponent(i, ComponentType::TRANSFORM))
			{
				auto& t = ComponentManager::GetComponentUnchecked<TransformComponent>(i);
				cbData.x = t.Position.x;
				cbData.y = t.Position.y;
				cbData.ScaleX = t.Scale.x;
				cbData.ScaleY = t.Scale.y;
			}
			cbData.AspectRatio = sceneAspectRatio;

			int srvIndex = defaultTextureIndex;
			int normalSrvIndex = defaultNormalIndex;
			if (hasMaterial)
			{
				auto& mat = ComponentManager::GetComponentUnchecked<MaterialComponent>(i);
				cbData.UseTexture = mat.UseTexture ? 1 : 0;
				cbData.MaterialMode = static_cast<int>(mat.ShaderClassMode);
				cbData.ShaderClass = static_cast<int>(mat.ShaderClass);
				cbData.MaterialAlpha = mat.Alpha;
				if (cbData.UseTexture != 0 && mat.TextureID >= 0)
				{
					srvIndex = mat.TextureID;
				}
				if (mat.NormalMapID >= 0)
				{
					cbData.UseNormalMap = 1;
					normalSrvIndex = mat.NormalMapID;
				}
			}
			if (srvIndex >= g_kMAX_ENTITIES + RendererResource::g_kMAX_SRVS)
			{
				continue;
			}

			auto* pCb = reinterpret_cast<CbData*>(pCbvDataBegin + (i * RendererResource::g_kCB_ALIGNED_SIZE));
			*pCb = cbData;

			m_SpriteDrawCalls.push_back(DrawCall{ i, pso, srvIndex, normalSrvIndex, sprite.VertexBufferView, sprite.VertexCount, false });
		}
	}

	for (EntityID i : World::GetView<MeshComponent>())
	{
		if (Registry::HasComponent(i, ComponentType::STATIC_MODEL) ||
			Registry::HasComponent(i, ComponentType::ANIMATION_MODEL))
		{
			continue;
		}

		auto& mesh = ComponentManager::GetComponentUnchecked<MeshComponent>(i);
		if (mesh.VertexBufferView.BufferLocation == 0)
		{
			continue;
		}
		if (!Registry::HasComponent(i, ComponentType::TRANSFORM))
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
		if (!drawTransparent && !drawDeferredOpaque && isReceiving != receivingPostProcessOnly)
		{
			continue;
		}

		RendererDraw::BeginModelPass();
		rendererResource psoResource{};
		psoResource.vsPath = GetMeshVsPath(i);
		psoResource.psPath = GetMeshPsPath(i);
		psoResource.isModel = true;
		psoResource.enableAlphaBlend = drawTransparent;
		ID3D12PipelineState* pso = PsoManager::GetOrCreateGraphicsPso(psoResource);
		if (!pso)
		{
			continue;
		}

		XMMATRIX world = XMLoadFloat4x4(&ComponentManager::GetComponentUnchecked<TransformComponent>(i).WorldMatrix);

		ConstantBuffer3D cb3D{};
		cb3D.World = XMMatrixTranspose(world);
		cb3D.View = transposedView;
		cb3D.Projection = transposedProj;
		cb3D.UseTexture = 0;

		EntityID cameraEntity = Camera::GetCameraEntity();
		if (Registry::HasComponent(cameraEntity, ComponentType::TRANSFORM))
		{
			cb3D.CameraPos = ComponentManager::GetComponentUnchecked<TransformComponent>(cameraEntity).Position;
		}
		else
		{
			cb3D.CameraPos = { 0.0f, 0.0f, 5.0f };
		}

		int srvIndex = defaultTextureIndex;
		int normalSrvIndex = defaultNormalIndex;
		if (hasMaterial)
		{
			auto& mat = ComponentManager::GetComponentUnchecked<MaterialComponent>(i);
			cb3D.UseTexture = mat.UseTexture ? 1 : 0;
			cb3D.MaterialMetallic = mat.Metallic;
			cb3D.MaterialRoughness = mat.Roughness;
			cb3D.MaterialFresnel = mat.Fresnel;
			cb3D.MaterialAlpha = mat.Alpha;
			cb3D.MaterialMode = static_cast<int>(mat.ShaderClassMode);
			cb3D.ShaderClass = static_cast<int>(mat.ShaderClass);
			cb3D.ToonOutlineWidth = mat.ToonOutlineWidth;
			if (cb3D.UseTexture != 0 && mat.TextureID >= 0)
			{
				srvIndex = mat.TextureID;
			}
			if (mat.NormalMapID >= 0)
			{
				cb3D.UseNormalMap = 1;
				normalSrvIndex = mat.NormalMapID;
			}
		}
		if (srvIndex >= g_kMAX_ENTITIES + RendererResource::g_kMAX_SRVS)
		{
			continue;
		}

		memcpy(pCbvDataBegin + (i * RendererResource::g_kCB_ALIGNED_SIZE), &cb3D, sizeof(cb3D));
		m_ModelDrawCalls.push_back(DrawCall{ i, pso, srvIndex, normalSrvIndex, mesh.VertexBufferView, mesh.VertexCount, true });
	}

	auto sortDrawCalls = [drawTransparent, cameraPos](vector<DrawCall>& drawCalls)
		{
			sort(drawCalls.begin(), drawCalls.end(), [drawTransparent, cameraPos](const DrawCall& a, const DrawCall& b)
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
				return a.normalSrvIndex < b.normalSrvIndex;
			});
		};

	sortDrawCalls(m_SpriteDrawCalls);
	sortDrawCalls(m_ModelDrawCalls);

	auto heapStart = pCbvHeap->GetGPUDescriptorHandleForHeapStart();

	auto submitDrawCalls = [&](const vector<DrawCall>& drawCalls)
		{
			ID3D12PipelineState* lastPso = nullptr;
			ID3D12PipelineState* outlinePso = PsoManager::GetOrCreateToonOutlinePso();
			int lastSrvIndex = -1;
			int lastNormalSrvIndex = -1;

			for (const auto& dc : drawCalls)
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

				CD3DX12_GPU_DESCRIPTOR_HANDLE cbvHandle(heapStart, dc.EntityID, descriptorIncrement);
				pCommandList->SetGraphicsRootDescriptorTable(0, cbvHandle);

				if (dc.srvIndex != lastSrvIndex)
				{
					CD3DX12_GPU_DESCRIPTOR_HANDLE srvHandle(heapStart, dc.srvIndex, descriptorIncrement);
					pCommandList->SetGraphicsRootDescriptorTable(1, srvHandle);
					lastSrvIndex = dc.srvIndex;
				}
				if (dc.normalSrvIndex != lastNormalSrvIndex)
				{
					CD3DX12_GPU_DESCRIPTOR_HANDLE normalSrvHandle(heapStart, dc.normalSrvIndex, descriptorIncrement);
					pCommandList->SetGraphicsRootDescriptorTable(6, normalSrvHandle);
					lastNormalSrvIndex = dc.normalSrvIndex;
				}

				pCommandList->IASetVertexBuffers(0, 1, &dc.vbv);
				pCommandList->DrawInstanced(dc.vertexCount, 1, 0, 0);

				if (dc.is3D && outlinePso && ShouldDrawToonOutline(dc.EntityID))
				{
					pCommandList->SetPipelineState(outlinePso);
					pCommandList->DrawInstanced(dc.vertexCount, 1, 0, 0);
					lastPso = outlinePso;
				}
			}
		};

	if (!m_SpriteDrawCalls.empty())
	{
		RendererDraw::BeginSpritePass();
		submitDrawCalls(m_SpriteDrawCalls);
	}

	if (!m_ModelDrawCalls.empty())
	{
		RendererDraw::BeginModelPass();
		submitDrawCalls(m_ModelDrawCalls);
	}
}

