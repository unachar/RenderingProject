#pragma once
#include "../effects/niagara.h"
#include <imgui.h>
#include <memory>

struct ThumbnailGpuImage;
struct EditorDescriptorLease;

struct NiagaraThumbnailFrame
{
    Niagara::System System;
    std::vector<Niagara::Particle> Particles;
};

// DrawScene is called while the editor-scene overlay render target and depth are bound.
class NiagaraRenderer
{
  public:
    static void BeginFrame();
    static void SetPreview(const Niagara::System& system, const std::vector<Niagara::Particle>& particles, float yaw,
                           float pitch, float distance, const DirectX::XMFLOAT3& target = {0, 1, 0}, bool grid = true,
                           const DirectX::XMFLOAT4& background = {0.025f, 0.028f, 0.035f, 1});
    static ImTextureID PreviewTexture();
    static void RenderPreview();
    static std::shared_ptr<NiagaraThumbnailFrame> PrepareThumbnail(const std::filesystem::path& path,
                                                                  std::string& error);
    static std::shared_ptr<ThumbnailGpuImage> RenderThumbnail(const NiagaraThumbnailFrame& frame, UINT size,
                                                             std::shared_ptr<EditorDescriptorLease> descriptor);
    static void DrawScene(const Niagara::System& system, const std::vector<Niagara::Particle>& particles,
                          const DirectX::XMMATRIX& world);
    static void Shutdown();
    static const std::string& LastError();
};
