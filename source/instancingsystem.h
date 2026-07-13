#pragma once

#include "systembase.h"
#include "ecs.h"
#include <d3d12.h>
#include <wrl.h>

class InstancingSystem final : public SystemBase
{
public:
    static bool CanInstance(EntityID entity);
    static bool IsEntityVisible(EntityID entity);
    static bool IsAvailable() { return s_Available; }

    void Init() override;
    void Uninit() override;
    void Draw(RenderPass renderPass, bool receivingPostProcessOnly) override;

private:
    static constexpr UINT kMaxInstancesPerFrame = g_kMAX_ENTITIES * 4;

    struct InstanceTransform
    {
        XMFLOAT4X4 World{};
    };

    Microsoft::WRL::ComPtr<ID3D12Resource> m_InstanceUpload;
    InstanceTransform* m_MappedInstances = nullptr;
    UINT m_FrameIndex = UINT_MAX;
    UINT m_FrameCursor = 0;
    static inline bool s_Available = false;
};
