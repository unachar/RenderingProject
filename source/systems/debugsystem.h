#pragma once
#include "systembase.h"
#include "ecs.h"
#include <wrl/client.h>
#include <d3d12.h>

class DebugSystem : public SystemBase
{
private:
	static Microsoft::WRL::ComPtr<ID3D12PipelineState> m_DebugLinePso;
	static Microsoft::WRL::ComPtr<ID3D12PipelineState> m_DebugSolidPso;
	static Microsoft::WRL::ComPtr<ID3D12PipelineState> m_DebugLineDeferredPso;
	static Microsoft::WRL::ComPtr<ID3D12PipelineState> m_DebugSolidDeferredPso;
	static bool m_ShowLightDebug;

	static void InitDebugLinePso();

public:
	void Init() override;
	void Uninit() override;
	void Draw(RenderPass renderPass, bool receivingPostProcessOnly) override;

	static bool GetShowLightDebug() { return m_ShowLightDebug; }
	static void SetShowLightDebug(bool show) { m_ShowLightDebug = show; }
};

