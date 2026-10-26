#pragma once
#include "proceduralstudio.h"
#include <d3d12.h>
#include <memory>

namespace ProcStudio
{
    // Records on the caller's queue. Keep this object alive until its last submission completes.
    // Dimensions may change only after the caller has completed previous submissions.
    class TextureGpu
    {
      public:
        TextureGpu();
        ~TextureGpu();
        TextureGpu(const TextureGpu&) = delete;
        TextureGpu& operator=(const TextureGpu&) = delete;

        // Caller must complete all recorded submissions before releasing.
        void Release();

        static bool Supports(const TextureParams& params);
        bool Initialize(ID3D12Device* device, std::string& error);
        bool Generate(ID3D12GraphicsCommandList* commands, const TextureParams& params, int width, int height,
                      std::string& error);
        // RGBA8 rows, 256-byte aligned; suitable for CopyTextureRegion without CPU access.
        ID3D12Resource* Preview(ID3D12GraphicsCommandList* commands, int size, int tiles, std::string& error);
        bool CopyToReadback(ID3D12GraphicsCommandList* commands, std::string& error);
        // Call only after the fence for CopyToReadback has completed.
        bool Readback(Image& image, std::string& error) const;

      private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };

    // Export worker: owns a separate queue and waits only for its own compute submission.
    bool GenerateTextureGpu(ID3D12Device* device, const TextureParams& params, int width, int height, Image& image,
                            std::string& error);
}
