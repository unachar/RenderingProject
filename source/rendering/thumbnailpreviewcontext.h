#pragma once
#include "staticmodel.h"
#include "componentmanager.h"
#include <memory>

struct ThumbnailPreviewPart
{
    MaterialComponent Material;
    UINT FirstVertex = 0;
    UINT VertexCount = 0;
};

class ThumbnailPreviewContext
{
  public:
    ThumbnailPreviewContext();
    ~ThumbnailPreviewContext();
    bool Initialize(UINT size, UINT descriptorIndex);
    bool PreparePipelines(const MaterialComponent& material, const std::vector<ThumbnailPreviewPart>& parts);
    bool Render(const MaterialComponent& material, const std::vector<StaticModelVertex>& geometry = {},
                const std::vector<ThumbnailPreviewPart>& parts = {});
    ID3D12Resource* GetTarget() const;
    uint64_t MemoryBytes() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};
