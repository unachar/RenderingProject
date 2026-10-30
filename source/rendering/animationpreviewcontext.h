#pragma once

#include "componentmanager.h"
#include <memory>
#include <vector>

class AnimationModelResource;

// Independent render target and camera; the caller owns the model and descriptor lifetime.
class AnimationPreviewContext
{
  public:
    AnimationPreviewContext();
    ~AnimationPreviewContext();
    bool Initialize(UINT size, UINT descriptorIndex, UINT materialCount);
    bool Render(AnimationModelResource& model, const std::vector<MaterialComponent>& materials);
    void FrameModel(const XMFLOAT3& center, const XMFLOAT3& extents);
    void Orbit(float yaw, float pitch, float zoom);
    const std::string& Error() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};
