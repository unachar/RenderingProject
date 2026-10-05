#include "pch.h"
#include "animationmodel.h"
#include "graphicsdevice.h"
#include "shaderpipelines.h"
#include "psomanager.h"
#include "frameconstants.h"


using namespace AnimationMath;

bool AnimationModelResource::CreateGpuSkinningBuffers(ID3D12Device* device)
{
	UINT numMeshes = (UINT)m_Meshes.size();

	{
		UINT boneBufferSize = sizeof(XMFLOAT4X4) * m_kMAX_BONES;
		auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		auto resDesc = CD3DX12_RESOURCE_DESC::Buffer(boneBufferSize);

		vector<XMFLOAT4X4> identityBones(m_kMAX_BONES);
		XMFLOAT4X4 identity;
		XMStoreFloat4x4(&identity, XMMatrixIdentity());
		for (auto& mtx : identityBones)
		{
			mtx = identity;
		}

		for (UINT frame = 0; frame < RendererState::g_kFRAME_COUNT; ++frame)
		{
			HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
				&resDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_BoneBuffers[frame]));
			if (FAILED(hr)) return false;
			CD3DX12_RANGE readRange(0, 0);
			hr = m_BoneBuffers[frame]->Map(0, &readRange, &m_pBoneBufferMapped[frame]);
			if (FAILED(hr))
			{
				Debug::Log("ERROR: Failed to map bone buffer\n");
				m_pBoneBufferMapped[frame] = nullptr;
				return false;
			}

			if (m_pBoneBufferMapped[frame])
			{
				memcpy(m_pBoneBufferMapped[frame], identityBones.data(), sizeof(XMFLOAT4X4) * m_kMAX_BONES);
			}
		}
	}

	{
		D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
		heapDesc.NumDescriptors = numMeshes * m_kSKINNING_DESCRIPTORS_PER_MESH;
		heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		HRESULT hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_SkinningDescHeap));
		if (FAILED(hr)) return false;
	}

	UINT descSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	for (UINT m = 0; m < numMeshes; m++)
	{
		UINT vertexCount = m_Meshes[m].VertexCount;
		const UINT descriptorBase = m * m_kSKINNING_DESCRIPTORS_PER_MESH;

		{
			UINT bufSize = sizeof(GpuSkinVertex) * vertexCount;
			auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
			auto resDesc = CD3DX12_RESOURCE_DESC::Buffer(bufSize);
			HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
				&resDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_Meshes[m].InputVertexBuffer));
			if (FAILED(hr)) return false;

			UINT8* pDest = nullptr;
			CD3DX12_RANGE readRange(0, 0);
			hr = m_Meshes[m].InputVertexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pDest));
			if (FAILED(hr) || !pDest)
			{
				Debug::Log("ERROR: Failed to map input vertex buffer\n");
				return false;
			}
			memcpy(pDest, m_GpuSkinVertices[m].data(), bufSize);
			m_Meshes[m].InputVertexBuffer->Unmap(0, nullptr);

			D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srvDesc.Buffer.NumElements = vertexCount;
			srvDesc.Buffer.StructureByteStride = sizeof(GpuSkinVertex);
			srvDesc.Format = DXGI_FORMAT_UNKNOWN;
			m_Meshes[m].SrvInputVertexIndex = descriptorBase + m_kINPUT_VERTEX_SRV_OFFSET;
			CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(m_SkinningDescHeap->GetCPUDescriptorHandleForHeapStart(),
				m_Meshes[m].SrvInputVertexIndex, descSize);
			device->CreateShaderResourceView(m_Meshes[m].InputVertexBuffer.Get(), &srvDesc, srvHandle);
		}

		for (UINT frame = 0; frame < RendererState::g_kFRAME_COUNT; ++frame)
		{
			D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srvDesc.Buffer.NumElements = m_kMAX_BONES;
			srvDesc.Buffer.StructureByteStride = sizeof(XMFLOAT4X4);
			srvDesc.Format = DXGI_FORMAT_UNKNOWN;
			CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(m_SkinningDescHeap->GetCPUDescriptorHandleForHeapStart(),
				descriptorBase + m_kBONE_SRV_OFFSET + frame, descSize);
			device->CreateShaderResourceView(m_BoneBuffers[frame].Get(), &srvDesc, srvHandle);
		}

		{
			UINT bufSize = sizeof(ModelVertex) * vertexCount;
			auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
			auto resDesc = CD3DX12_RESOURCE_DESC::Buffer(bufSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
			HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
				&resDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_Meshes[m].VertexBuffer));
			if (FAILED(hr)) return false;
			hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
				&resDesc, D3D12_RESOURCE_STATE_COMMON, nullptr,
				IID_PPV_ARGS(&m_Meshes[m].PreviousVertexBuffer));
			if (FAILED(hr)) return false;

			D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
			uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
			uavDesc.Buffer.NumElements = vertexCount;
			uavDesc.Buffer.StructureByteStride = sizeof(ModelVertex);
			uavDesc.Format = DXGI_FORMAT_UNKNOWN;
			m_Meshes[m].UavOutputVertexIndex = descriptorBase + m_kOUTPUT_VERTEX_UAV_OFFSET;
			CD3DX12_CPU_DESCRIPTOR_HANDLE uavHandle(m_SkinningDescHeap->GetCPUDescriptorHandleForHeapStart(),
				m_Meshes[m].UavOutputVertexIndex, descSize);
			device->CreateUnorderedAccessView(m_Meshes[m].VertexBuffer.Get(), nullptr, &uavDesc, uavHandle);

			m_Meshes[m].VertexBufferView.BufferLocation = m_Meshes[m].VertexBuffer->GetGPUVirtualAddress();
			m_Meshes[m].VertexBufferView.SizeInBytes = bufSize;
			m_Meshes[m].VertexBufferView.StrideInBytes = sizeof(ModelVertex);
			m_Meshes[m].PreviousVertexBufferView.BufferLocation = m_Meshes[m].PreviousVertexBuffer->GetGPUVirtualAddress();
			m_Meshes[m].PreviousVertexBufferView.SizeInBytes = bufSize;
			m_Meshes[m].PreviousVertexBufferView.StrideInBytes = sizeof(ModelVertex);
		}

		for (int mode = 0; mode < kToonOutlineModeCount; ++mode)
		{
			const UINT teoVertexCount = m_Meshes[m].TeoVertexCounts[mode];
			if (teoVertexCount == 0 || m_TeoGpuSkinVerticesByMode[m][mode].empty())
			{
				continue;
			}

			{
				UINT bufSize = sizeof(GpuSkinVertex) * teoVertexCount;
				auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
				auto resDesc = CD3DX12_RESOURCE_DESC::Buffer(bufSize);
				HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
					&resDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_Meshes[m].TeoInputVertexBuffers[mode]));
				if (FAILED(hr)) return false;

				UINT8* pDest = nullptr;
				CD3DX12_RANGE readRange(0, 0);
				hr = m_Meshes[m].TeoInputVertexBuffers[mode]->Map(0, &readRange, reinterpret_cast<void**>(&pDest));
				if (FAILED(hr) || !pDest)
				{
					Debug::Log("ERROR: Failed to map TEO input vertex buffer\n");
					return false;
				}
				memcpy(pDest, m_TeoGpuSkinVerticesByMode[m][mode].data(), bufSize);
				m_Meshes[m].TeoInputVertexBuffers[mode]->Unmap(0, nullptr);

				D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
				srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
				srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				srvDesc.Buffer.NumElements = teoVertexCount;
				srvDesc.Buffer.StructureByteStride = sizeof(GpuSkinVertex);
				srvDesc.Format = DXGI_FORMAT_UNKNOWN;
				m_Meshes[m].SrvTeoInputVertexIndices[mode] = descriptorBase + m_kTEO_DESCRIPTOR_OFFSET + (mode * 2);
				CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(m_SkinningDescHeap->GetCPUDescriptorHandleForHeapStart(),
					m_Meshes[m].SrvTeoInputVertexIndices[mode], descSize);
				device->CreateShaderResourceView(m_Meshes[m].TeoInputVertexBuffers[mode].Get(), &srvDesc, srvHandle);
			}

			{
				UINT bufSize = sizeof(ModelVertex) * teoVertexCount;
				auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
				auto resDesc = CD3DX12_RESOURCE_DESC::Buffer(bufSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
				HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
					&resDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_Meshes[m].TeoVertexBuffers[mode]));
				if (FAILED(hr)) return false;

				D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
				uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
				uavDesc.Buffer.NumElements = teoVertexCount;
				uavDesc.Buffer.StructureByteStride = sizeof(ModelVertex);
				uavDesc.Format = DXGI_FORMAT_UNKNOWN;
				m_Meshes[m].UavTeoOutputVertexIndices[mode] = descriptorBase + m_kTEO_DESCRIPTOR_OFFSET + (mode * 2) + 1;
				CD3DX12_CPU_DESCRIPTOR_HANDLE uavHandle(m_SkinningDescHeap->GetCPUDescriptorHandleForHeapStart(),
					m_Meshes[m].UavTeoOutputVertexIndices[mode], descSize);
				device->CreateUnorderedAccessView(m_Meshes[m].TeoVertexBuffers[mode].Get(), nullptr, &uavDesc, uavHandle);

				m_Meshes[m].TeoVertexBufferViews[mode].BufferLocation = m_Meshes[m].TeoVertexBuffers[mode]->GetGPUVirtualAddress();
				m_Meshes[m].TeoVertexBufferViews[mode].SizeInBytes = bufSize;
				m_Meshes[m].TeoVertexBufferViews[mode].StrideInBytes = sizeof(ModelVertex);

				if (mode == static_cast<int>(ToonOutlineMeshMode::Balanced))
				{
					m_Meshes[m].TeoInputVertexBuffer = m_Meshes[m].TeoInputVertexBuffers[mode];
					m_Meshes[m].TeoVertexBuffer = m_Meshes[m].TeoVertexBuffers[mode];
					m_Meshes[m].TeoVertexBufferView = m_Meshes[m].TeoVertexBufferViews[mode];
					m_Meshes[m].SrvTeoInputVertexIndex = m_Meshes[m].SrvTeoInputVertexIndices[mode];
					m_Meshes[m].UavTeoOutputVertexIndex = m_Meshes[m].UavTeoOutputVertexIndices[mode];
				}
			}
		}
	}
	return true;
}

void AnimationModelResource::DispatchGpuSkinning(ID3D12GraphicsCommandList* pCommandList)
{
	if (!pCommandList ||
		!m_SkinningDescHeap ||
		!ShaderPipelines::GetSkinningRootSignature() ||
		!PsoManager::GetSkinningPso() ||
		m_DispatchedSkinningVersion == m_SkinningVersion)
	{
		return;
	}

	pCommandList->SetComputeRootSignature(ShaderPipelines::GetSkinningRootSignature());
	pCommandList->SetPipelineState(PsoManager::GetSkinningPso());

	ID3D12DescriptorHeap* heaps[] = { m_SkinningDescHeap.Get() };
	pCommandList->SetDescriptorHeaps(_countof(heaps), heaps);

	UINT descSize = m_pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	constexpr UINT kSkinningThreadGroupSize = 128;

	for (UINT m = 0; m < m_Meshes.size(); m++)
	{
		if (m_Meshes[m].VertexCount == 0) continue;

		if (m_Meshes[m].PreviousVertexValid)
		{
			D3D12_RESOURCE_BARRIER copyBarriers[2] =
			{
				CD3DX12_RESOURCE_BARRIER::Transition(m_Meshes[m].VertexBuffer.Get(),
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(m_Meshes[m].PreviousVertexBuffer.Get(),
					D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_COPY_DEST)
			};
			pCommandList->ResourceBarrier(_countof(copyBarriers), copyBarriers);
			pCommandList->CopyResource(m_Meshes[m].PreviousVertexBuffer.Get(), m_Meshes[m].VertexBuffer.Get());
			copyBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(m_Meshes[m].VertexBuffer.Get(),
				D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			copyBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(m_Meshes[m].PreviousVertexBuffer.Get(),
				D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
			pCommandList->ResourceBarrier(_countof(copyBarriers), copyBarriers);
		}
		const UINT descriptorBase = m * m_kSKINNING_DESCRIPTORS_PER_MESH;

		CD3DX12_GPU_DESCRIPTOR_HANDLE srvInputHandle(m_SkinningDescHeap->GetGPUDescriptorHandleForHeapStart(), m_Meshes[m].SrvInputVertexIndex, descSize);
		pCommandList->SetComputeRootDescriptorTable(0, srvInputHandle);

		const UINT boneFrameIndex = GraphicsDevice::GetFrameIndex() % RendererState::g_kFRAME_COUNT;
		CD3DX12_GPU_DESCRIPTOR_HANDLE srvBoneHandle(m_SkinningDescHeap->GetGPUDescriptorHandleForHeapStart(), descriptorBase + m_kBONE_SRV_OFFSET + boneFrameIndex, descSize);
		pCommandList->SetComputeRootDescriptorTable(1, srvBoneHandle);

		CD3DX12_GPU_DESCRIPTOR_HANDLE uavOutputHandle(m_SkinningDescHeap->GetGPUDescriptorHandleForHeapStart(), m_Meshes[m].UavOutputVertexIndex, descSize);
		pCommandList->SetComputeRootDescriptorTable(2, uavOutputHandle);

		UINT threadGroups = (m_Meshes[m].VertexCount + kSkinningThreadGroupSize - 1) / kSkinningThreadGroupSize;
		pCommandList->Dispatch(threadGroups, 1, 1);
		D3D12_RESOURCE_BARRIER skinningUavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(m_Meshes[m].VertexBuffer.Get());
		pCommandList->ResourceBarrier(1, &skinningUavBarrier);

		if (!m_Meshes[m].PreviousVertexValid)
		{
			D3D12_RESOURCE_BARRIER previousToCopy = CD3DX12_RESOURCE_BARRIER::Transition(
				m_Meshes[m].PreviousVertexBuffer.Get(),
				D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
			pCommandList->ResourceBarrier(1, &previousToCopy);
			D3D12_RESOURCE_BARRIER toCopy = CD3DX12_RESOURCE_BARRIER::Transition(m_Meshes[m].VertexBuffer.Get(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
			pCommandList->ResourceBarrier(1, &toCopy);
			pCommandList->CopyResource(m_Meshes[m].PreviousVertexBuffer.Get(), m_Meshes[m].VertexBuffer.Get());
			D3D12_RESOURCE_BARRIER ready[2] =
			{
				CD3DX12_RESOURCE_BARRIER::Transition(m_Meshes[m].VertexBuffer.Get(),
					D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(m_Meshes[m].PreviousVertexBuffer.Get(),
					D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER)
			};
			pCommandList->ResourceBarrier(_countof(ready), ready);
			m_Meshes[m].PreviousVertexValid = true;
		}

		for (int mode = 0; mode < kToonOutlineModeCount; ++mode)
		{
			if (m_Meshes[m].TeoVertexCounts[mode] == 0 ||
				!m_Meshes[m].TeoInputVertexBuffers[mode] ||
				!m_Meshes[m].TeoVertexBuffers[mode])
			{
				continue;
			}

			CD3DX12_GPU_DESCRIPTOR_HANDLE teoSrvInputHandle(
				m_SkinningDescHeap->GetGPUDescriptorHandleForHeapStart(),
				m_Meshes[m].SrvTeoInputVertexIndices[mode],
				descSize);
			pCommandList->SetComputeRootDescriptorTable(0, teoSrvInputHandle);

			CD3DX12_GPU_DESCRIPTOR_HANDLE teoUavOutputHandle(
				m_SkinningDescHeap->GetGPUDescriptorHandleForHeapStart(),
				m_Meshes[m].UavTeoOutputVertexIndices[mode],
				descSize);
			pCommandList->SetComputeRootDescriptorTable(2, teoUavOutputHandle);

			UINT teoThreadGroups = (m_Meshes[m].TeoVertexCounts[mode] + kSkinningThreadGroupSize - 1) / kSkinningThreadGroupSize;
			pCommandList->Dispatch(teoThreadGroups, 1, 1);
			D3D12_RESOURCE_BARRIER teoUavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(m_Meshes[m].TeoVertexBuffers[mode].Get());
			pCommandList->ResourceBarrier(1, &teoUavBarrier);
		}
	}

	if (ID3D12DescriptorHeap* cbvHeap = FrameConstants::GetCbvHeap())
	{
		ID3D12DescriptorHeap* rendererHeaps[] = { cbvHeap };
		pCommandList->SetDescriptorHeaps(_countof(rendererHeaps), rendererHeaps);
	}

	m_DispatchedSkinningVersion = m_SkinningVersion;
}
