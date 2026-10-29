#include "pch.h"
#include "proceduralstudiowindow.h"
#include "proceduralstudio.h"
#include "proceduraltexturegpu.h"
#include "proceduralio.h"
#include "imagemaps.h"
#include "assets/noiseasset.h"
#include "assets/materialmanager.h"
#include "proceduralmodelexport.h"
#include "editordescriptors.h"
#include "graphicsdevice.h"
#include "rendererstate.h"
#include "cloud/cloudrenderer.h"
#include <imgui.h>
#include <objbase.h>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <cmath>
#include <future>
#include <memory>

// 名前付き名前空間と using 宣言を使う。エンジンのプリコンパイル済みヘッダーは
// DirectX:: と std:: をグローバルへ導入するため、Image / Mesh などの短い名前を漏らさない。
namespace ProcStudioEditorDetail
{
    using ProcStudio::Basis;
    using ProcStudio::Float2;
    using ProcStudio::Float3;
    using ProcStudio::Float4;
    using ProcStudio::FractalType;
    using ProcStudio::GenerateModel;
    using ProcStudio::GenerateSolidVolume;
    using ProcStudio::GenerateTexture;
    using ProcStudio::GenerateVolume;
    using ProcStudio::Image;
    using ProcStudio::IsSolidCapable;
    using ProcStudio::Mesh;
    using ProcStudio::ModelKind;
    using ProcStudio::ModelKindName;
    using ProcStudio::ModelParams;
    using ProcStudio::NoiseParams;
    using ProcStudio::TextureColor;
    using ProcStudio::TextureKind;
    using ProcStudio::TextureKindName;
    using ProcStudio::TextureOutput;
    using ProcStudio::TextureParams;
    using ProcStudio::Volume;

    constexpr int kPreviewSize = 512;
    constexpr int kTexturePreview = 256;
    constexpr int kSolidPreview = 48;
    constexpr UINT kUploads = RendererState::g_kFRAME_COUNT;

    // ------------------------------------------------------------------------------------------------
    // GPU 側はエディターのディスクリプターで表示する RGBA8 テクスチャ 1 枚。アップロードリングで更新する。
    struct GpuPreview
    {
        ComPtr<ID3D12Resource> Texture;
        array<ComPtr<ID3D12Resource>, kUploads> Uploads;
        array<UINT64, kUploads> Fences{};
        UINT Next = 0;
        shared_ptr<EditorDescriptorLease> Lease;
        string Error;

        bool Ensure()
        {
            if (Texture && Lease)
            {
                return true;
            }
            auto* device = GraphicsDevice::GetDevice();
            if (!device)
            {
                return false;
            }
            Lease = EditorDescriptors::Allocate();
            if (!Lease)
            {
                Error = "プレビュー用のディスクリプタが不足しています";
                return false;
            }
            const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            const auto desc =
                CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, kPreviewSize, kPreviewSize, 1, 1);
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                       IID_PPV_ARGS(&Texture))))
            {
                Error = "プレビューテクスチャを作成できません";
                Lease.reset();
                return false;
            }
            Texture->SetName(L"Procedural Studio Preview");
            const UINT64 bytes = GetRequiredIntermediateSize(Texture.Get(), 0, 1);
            const auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
            const auto uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
            for (auto& upload : Uploads)
            {
                if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
                                                           D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                           IID_PPV_ARGS(&upload))))
                {
                    Error = "プレビューのアップロードバッファを作成できません";
                    Release();
                    return false;
                }
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(Texture.Get(), &srv, Lease->Cpu);
            return true;
        }

        // 全アップロードバッファが使用中なら false を返し、呼び出し元が次フレームで再試行する。
        bool Upload(const vector<uint32_t>& pixels)
        {
            if (!Ensure())
            {
                return false;
            }
            const UINT slot = Next % kUploads;
            if (GraphicsDevice::GetCompletedFence() < Fences[slot])
            {
                return false;
            }
            auto* commands = GraphicsDevice::GetCommandList();
            D3D12_SUBRESOURCE_DATA data{};
            data.pData = pixels.data();
            data.RowPitch = kPreviewSize * 4;
            data.SlicePitch = data.RowPitch * kPreviewSize;
            auto before = CD3DX12_RESOURCE_BARRIER::Transition(
                Texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            commands->ResourceBarrier(1, &before);
            UpdateSubresources(commands, Texture.Get(), Uploads[slot].Get(), 0, 0, 1, &data);
            auto after = CD3DX12_RESOURCE_BARRIER::Transition(Texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            commands->ResourceBarrier(1, &after);
            Fences[slot] = GraphicsDevice::GetPendingSubmissionFence();
            ++Next;
            return true;
        }

        bool CopyGpuPixels(ID3D12Resource* packed)
        {
            if (!packed || !Ensure())
            {
                return false;
            }
            auto* commands = GraphicsDevice::GetCommandList();
            const auto before = CD3DX12_RESOURCE_BARRIER::Transition(
                Texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            commands->ResourceBarrier(1, &before);
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = packed;
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, kPreviewSize, kPreviewSize, 1,
                                                kPreviewSize * 4};
            const CD3DX12_TEXTURE_COPY_LOCATION target(Texture.Get(), 0);
            commands->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
            const auto after = CD3DX12_RESOURCE_BARRIER::Transition(Texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            commands->ResourceBarrier(1, &after);
            return true;
        }

        void Release()
        {
            // ディスクリプタープールは未完了フェンスまで再利用を遅延し、リソースはアイドルまで保持する。
            if (Texture || Lease)
            {
                GraphicsDevice::WaitForGpuIdle();
            }
            Texture.Reset();
            for (auto& upload : Uploads)
            {
                upload.Reset();
            }
            Fences = {};
            Lease.reset();
        }
    };

    // ------------------------------------------------------------------------------------------------
    // シーンレンダラーとその状態に依存しない、プレビュー用ソフトウェアラスタライザー。
    enum class Shading
    {
        VertexColor,
        Texture,
        Solid,
        Water
    };

    struct OrbitCamera
    {
        float Yaw = 0.7f;
        float Pitch = 0.35f;
        float Distance = 3.2f;
    };

    struct RenderInput
    {
        const Mesh* Geometry = nullptr;
        Shading Mode = Shading::VertexColor;
        const Image* Texture = nullptr;
        // 接空間法線マップ（0〜1 エンコード）。UV から三角形ごとに接線を求めて適用する。
        const Image* NormalMap = nullptr;
        bool NormalFlipY = false; // true: DirectX 規約 (-Y)。
        float UvScale = 1.0f;
        const vector<Float4>* Solid = nullptr;
        int SolidExtent = 0;
        float SolidScale = 1.0f;
        float TerrainGridSize = 0.0f;
        OrbitCamera View;
    };

    float Clamp01(float v)
    {
        return std::clamp(v, 0.0f, 1.0f);
    }

    Float3 operator+(Float3 a, Float3 b)
    {
        return {a.X + b.X, a.Y + b.Y, a.Z + b.Z};
    }

    Float3 operator-(Float3 a, Float3 b)
    {
        return {a.X - b.X, a.Y - b.Y, a.Z - b.Z};
    }

    Float3 operator*(Float3 a, float s)
    {
        return {a.X * s, a.Y * s, a.Z * s};
    }

    Float3 operator*(Float3 a, Float3 b)
    {
        return {a.X * b.X, a.Y * b.Y, a.Z * b.Z};
    }

    float Dot(Float3 a, Float3 b)
    {
        return a.X * b.X + a.Y * b.Y + a.Z * b.Z;
    }

    Float3 Cross(Float3 a, Float3 b)
    {
        return {a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X};
    }

    Float3 Normalize(Float3 a)
    {
        const float l = sqrtf(Dot(a, a));
        return l > 1e-12f ? a * (1 / l) : Float3{0, 1, 0};
    }

    Float3 Mix(Float3 a, Float3 b, float t)
    {
        return a + (b - a) * t;
    }

    float ToSrgb(float v)
    {
        v = Clamp01(v);
        return v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1 / 2.4f) - 0.055f;
    }

    uint32_t Pack(Float3 c, bool gamma)
    {
        const auto byte = [&](float v) {
            return static_cast<uint32_t>(Clamp01(gamma ? ToSrgb(v) : v) * 255.0f + 0.5f);
        };
        return byte(c.X) | (byte(c.Y) << 8) | (byte(c.Z) << 16) | 0xff000000u;
    }

    Float3 Tonemap(Float3 c)
    {
        // ACES の近似式（Narkowicz）。
        const auto f = [](float x) {
            return Clamp01((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f));
        };
        return {f(c.X), f(c.Y), f(c.Z)};
    }

    Float3 Sky(Float3 direction)
    {
        const float t = Clamp01(direction.Y * 0.5f + 0.5f);
        return Mix(Float3{0.32f, 0.3f, 0.28f},
                   Mix(Float3{0.75f, 0.82f, 0.92f}, Float3{0.3f, 0.5f, 0.85f}, Clamp01(direction.Y)),
                   t > 0.5f ? 1.0f : t * 2);
    }

    Float3 SampleSolid(const RenderInput& input, Float3 p)
    {
        const int n = input.SolidExtent;
        if (!input.Solid || n <= 0)
        {
            return {1, 1, 1};
        }
        const auto wrap = [&](float v) {
            const float scaled = v * input.SolidScale * n - 0.5f;
            return scaled;
        };
        const float fx = wrap(p.X);
        const float fy = wrap(p.Y);
        const float fz = wrap(p.Z);
        const int x0 = static_cast<int>(floorf(fx));
        const int y0 = static_cast<int>(floorf(fy));
        const int z0 = static_cast<int>(floorf(fz));
        const float tx = fx - x0;
        const float ty = fy - y0;
        const float tz = fz - z0;
        const auto at = [&](int x, int y, int z) {
            x = ((x % n) + n) % n;
            y = ((y % n) + n) % n;
            z = ((z % n) + n) % n;
            const auto& v = (*input.Solid)[(static_cast<size_t>(z) * n + y) * n + x];
            return Float3{v.X, v.Y, v.Z};
        };
        const Float3 c00 = Mix(at(x0, y0, z0), at(x0 + 1, y0, z0), tx);
        const Float3 c10 = Mix(at(x0, y0 + 1, z0), at(x0 + 1, y0 + 1, z0), tx);
        const Float3 c01 = Mix(at(x0, y0, z0 + 1), at(x0 + 1, y0, z0 + 1), tx);
        const Float3 c11 = Mix(at(x0, y0 + 1, z0 + 1), at(x0 + 1, y0 + 1, z0 + 1), tx);
        return Mix(Mix(c00, c10, ty), Mix(c01, c11, ty), tz);
    }

    void Rasterize(const RenderInput& input, vector<uint32_t>& out)
    {
        const int size = kPreviewSize;
        out.assign(static_cast<size_t>(size) * size, 0);
        vector<float> depth(out.size(), 0.0f);
        for (int y = 0; y < size; ++y)
        {
            const float t = static_cast<float>(y) / size;
            const Float3 background = Mix(Float3{0.16f, 0.17f, 0.2f}, Float3{0.06f, 0.06f, 0.07f}, t);
            for (int x = 0; x < size; ++x)
            {
                out[static_cast<size_t>(y) * size + x] = Pack(background, false);
            }
        }
        if (!input.Geometry || input.Geometry->Indices.empty())
        {
            return;
        }
        const Mesh& mesh = *input.Geometry;
        Float3 minimum;
        Float3 maximum;
        mesh.Bounds(minimum, maximum);
        const Float3 center = (minimum + maximum) * 0.5f;
        const float radius = (std::max)(sqrtf(Dot(maximum - minimum, maximum - minimum)) * 0.5f, 1e-3f);
        const float extent = (std::max)({maximum.X - minimum.X, maximum.Y - minimum.Y, maximum.Z - minimum.Z, 1e-4f});
        const Float3 offset{cosf(input.View.Pitch) * sinf(input.View.Yaw), sinf(input.View.Pitch),
                            cosf(input.View.Pitch) * cosf(input.View.Yaw)};
        const Float3 eye = center + offset * (radius * input.View.Distance);
        const Float3 forward = Normalize(center - eye);
        const Float3 right = Normalize(Cross(forward, Float3{0, 1, 0}));
        const Float3 up = Cross(right, forward);
        const float focal = 1.0f / tanf(20.0f * 3.14159265f / 180.0f);
        const float nearPlane = radius * 0.01f;

        struct Projected
        {
            float X;
            float Y;
            float InvZ;
            bool Valid;
        };

        vector<Projected> projected(mesh.Positions.size());
        for (size_t i = 0; i < mesh.Positions.size(); ++i)
        {
            const Float3 v = mesh.Positions[i] - eye;
            const float z = Dot(v, forward);
            if (z < nearPlane)
            {
                projected[i] = {0, 0, 0, false};
                continue;
            }
            const float invZ = 1 / z;
            projected[i] = {size * 0.5f + Dot(v, right) * invZ * focal * size * 0.5f,
                            size * 0.5f - Dot(v, up) * invZ * focal * size * 0.5f, invZ, true};
        }
        const Float3 keyLight = Normalize({-0.45f, 0.8f, 0.4f});
        const Float3 fillLight = Normalize({0.6f, 0.25f, -0.6f});
        const bool hasUv = mesh.UVs.size() == mesh.Positions.size();
        const bool hasColor = mesh.Colors.size() == mesh.Positions.size();
        const bool hasNormal = mesh.Normals.size() == mesh.Positions.size();
        for (size_t t = 0; t + 2 < mesh.Indices.size(); t += 3)
        {
            const uint32_t i0 = mesh.Indices[t];
            const uint32_t i1 = mesh.Indices[t + 1];
            const uint32_t i2 = mesh.Indices[t + 2];
            const Projected &a = projected[i0], &b = projected[i1], &c = projected[i2];
            if (!a.Valid || !b.Valid || !c.Valid)
            {
                continue;
            }
            const float area = (b.X - a.X) * (c.Y - a.Y) - (b.Y - a.Y) * (c.X - a.X);
            if (fabsf(area) < 1e-6f)
            {
                continue;
            }
            const int minX = (std::max)(0, static_cast<int>(floorf((std::min)({a.X, b.X, c.X}))));
            const int maxX = (std::min)(size - 1, static_cast<int>(ceilf((std::max)({a.X, b.X, c.X}))));
            const int minY = (std::max)(0, static_cast<int>(floorf((std::min)({a.Y, b.Y, c.Y}))));
            const int maxY = (std::min)(size - 1, static_cast<int>(ceilf((std::max)({a.Y, b.Y, c.Y}))));
            if (minX > maxX || minY > maxY)
            {
                continue;
            }
            const float inverseArea = 1 / area;
            // 法線マップ用の接線・従接線（UV の +u / +v 方向）。三角形ごとに一定。
            Float3 triangleTangent{};
            Float3 triangleBitangent{};
            bool hasTangent = false;
            if (input.NormalMap && hasUv)
            {
                const Float3 e1 = mesh.Positions[i1] - mesh.Positions[i0];
                const Float3 e2 = mesh.Positions[i2] - mesh.Positions[i0];
                const float du1 = mesh.UVs[i1].X - mesh.UVs[i0].X;
                const float dv1 = mesh.UVs[i1].Y - mesh.UVs[i0].Y;
                const float du2 = mesh.UVs[i2].X - mesh.UVs[i0].X;
                const float dv2 = mesh.UVs[i2].Y - mesh.UVs[i0].Y;
                const float det = du1 * dv2 - du2 * dv1;
                if (fabsf(det) > 1e-12f)
                {
                    const float r = 1 / det;
                    triangleTangent = (e1 * dv2 - e2 * dv1) * r;
                    triangleBitangent = (e2 * du1 - e1 * du2) * r;
                    hasTangent = true;
                }
            }
            for (int y = minY; y <= maxY; ++y)
            {
                const float py = y + 0.5f;
                for (int x = minX; x <= maxX; ++x)
                {
                    const float px = x + 0.5f;
                    const float w0 = ((b.X - px) * (c.Y - py) - (b.Y - py) * (c.X - px)) * inverseArea;
                    const float w1 = ((c.X - px) * (a.Y - py) - (c.Y - py) * (a.X - px)) * inverseArea;
                    const float w2 = 1 - w0 - w1;
                    if (w0 < 0 || w1 < 0 || w2 < 0)
                    {
                        continue;
                    }
                    const float invZ = w0 * a.InvZ + w1 * b.InvZ + w2 * c.InvZ;
                    const size_t pixel = static_cast<size_t>(y) * size + x;
                    if (invZ <= depth[pixel])
                    {
                        continue;
                    }
                    depth[pixel] = invZ;
                    // 透視補正済みの重み。
                    const float p0 = w0 * a.InvZ / invZ;
                    const float p1 = w1 * b.InvZ / invZ;
                    const float p2 = w2 * c.InvZ / invZ;
                    const auto interpolate3 = [&](const vector<Float3>& values) {
                        return values[i0] * p0 + values[i1] * p1 + values[i2] * p2;
                    };
                    const Float3 position = interpolate3(mesh.Positions);
                    Float3 normal = hasNormal ? Normalize(interpolate3(mesh.Normals))
                                              : Normalize(Cross(mesh.Positions[i1] - mesh.Positions[i0],
                                                                mesh.Positions[i2] - mesh.Positions[i0]));
                    const Float3 view = Normalize(eye - position);
                    const bool backFacing = Dot(normal, view) < 0;
                    const float u = hasUv ? mesh.UVs[i0].X * p0 + mesh.UVs[i1].X * p1 + mesh.UVs[i2].X * p2 : 0.0f;
                    const float v = hasUv ? mesh.UVs[i0].Y * p0 + mesh.UVs[i1].Y * p1 + mesh.UVs[i2].Y * p2 : 0.0f;
                    if (hasTangent)
                    {
                        // 接空間法線は OpenGL 規約（+Y = 画像の上 = -v）。FlipY なら DirectX 規約から変換する。
                        const Float4 encoded = input.NormalMap->Sample(u * input.UvScale, v * input.UvScale);
                        const float tx = encoded.X * 2 - 1;
                        const float ty = (encoded.Y * 2 - 1) * (input.NormalFlipY ? -1.0f : 1.0f);
                        const float tz = encoded.Z * 2 - 1;
                        const Float3 tangent = Normalize(triangleTangent - normal * Dot(normal, triangleTangent));
                        Float3 bitangent = triangleBitangent - normal * Dot(normal, triangleBitangent);
                        bitangent = Normalize(bitangent - tangent * Dot(tangent, bitangent));
                        normal = Normalize(tangent * tx + bitangent * -ty + normal * tz);
                    }
                    if (backFacing)
                    {
                        normal = normal * -1.0f;
                    }
                    Float3 albedo = hasColor ? interpolate3(mesh.Colors) : Float3{0.8f, 0.8f, 0.8f};
                    if (input.Mode == Shading::Texture && input.Texture && hasUv)
                    {
                        const Float4 texel = input.Texture->Sample(u * input.UvScale, v * input.UvScale);
                        albedo = albedo * Float3{texel.X, texel.Y, texel.Z};
                    }
                    else if (input.Mode == Shading::Solid)
                    {
                        albedo = SampleSolid(input, (position - minimum) * (1 / extent));
                    }
                    const float key = (std::max)(Dot(normal, keyLight), 0.0f);
                    const float fill = (std::max)(Dot(normal, fillLight), 0.0f);
                    const Float3 ambient =
                        Mix(Float3{0.22f, 0.2f, 0.18f}, Float3{0.45f, 0.52f, 0.62f}, normal.Y * 0.5f + 0.5f);
                    const Float3 halfVector = Normalize(keyLight + view);
                    const float nDotH = (std::max)(Dot(normal, halfVector), 0.0f);
                    Float3 color;
                    if (input.Mode == Shading::Water)
                    {
                        const float nDotV = Clamp01(Dot(normal, view));
                        const float fresnel = 0.02f + 0.98f * powf(1 - nDotV, 5);
                        const Float3 reflected = normal * (2 * Dot(normal, view)) - view;
                        const Float3 body = albedo * (ambient * 0.6f + Float3{1, 0.95f, 0.85f} * (key * 0.5f));
                        color = Mix(body, Sky(reflected) * 1.2f, fresnel) +
                                Float3{1, 0.95f, 0.85f} * (powf(nDotH, 180) * 4);
                    }
                    else
                    {
                        color = albedo * (ambient * 0.55f + Float3{1.0f, 0.95f, 0.88f} * (key * 1.15f) +
                                          Float3{0.55f, 0.65f, 0.8f} * (fill * 0.35f)) +
                                Float3{1, 1, 1} * (powf(nDotH, 48) * 0.12f * key);
                    }
                    Float3 displayColor = Tonemap(color * 0.95f);
                    if (input.TerrainGridSize > 0)
                    {
                        const float gridX = (position.X - minimum.X) / input.TerrainGridSize;
                        const float gridZ = (position.Z - minimum.Z) / input.TerrainGridSize;
                        const float distance = (std::min)(fabsf(gridX - roundf(gridX)), fabsf(gridZ - roundf(gridZ))) *
                                               input.TerrainGridSize;
                        const float pixelWidth = 2.0f / (invZ * focal * size);
                        const float coverage = Clamp01(1.0f - distance / pixelWidth);
                        displayColor = Mix(displayColor, Float3{1.0f, 0.7f, 0.15f}, coverage * 0.85f);
                    }
                    out[pixel] = Pack(displayColor, true);
                }
            }
        }
    }

    void BlitImage(const Image& image, bool gamma, int tiles, vector<uint32_t>& out)
    {
        out.resize(static_cast<size_t>(kPreviewSize) * kPreviewSize);
        for (int y = 0; y < kPreviewSize; ++y)
        {
            for (int x = 0; x < kPreviewSize; ++x)
            {
                const float u = (x + 0.5f) / kPreviewSize * tiles;
                const float v = (y + 0.5f) / kPreviewSize * tiles;
                const Float4 c = image.Sample(u, v);
                out[static_cast<size_t>(y) * kPreviewSize + x] = Pack({c.X, c.Y, c.Z}, gamma);
            }
        }
    }

    void BlitVolumeSlice(const vector<Float4>* colors, const Volume* density, int extent, float slice,
                         vector<uint32_t>& out)
    {
        out.resize(static_cast<size_t>(kPreviewSize) * kPreviewSize);
        if (extent <= 0)
        {
            return;
        }
        const int z = std::clamp(static_cast<int>(slice * extent), 0, extent - 1);
        for (int y = 0; y < kPreviewSize; ++y)
        {
            for (int x = 0; x < kPreviewSize; ++x)
            {
                const int vx = x * extent / kPreviewSize;
                const int vy = y * extent / kPreviewSize;
                const size_t index = (static_cast<size_t>(z) * extent + vy) * extent + vx;
                Float3 c{0, 0, 0};
                if (colors)
                {
                    c = {(*colors)[index].X, (*colors)[index].Y, (*colors)[index].Z};
                }
                else if (density)
                {
                    const float d = density->Density[index];
                    c = {d, d, d};
                }
                out[static_cast<size_t>(y) * kPreviewSize + x] = Pack(c, colors != nullptr);
            }
        }
    }

    // ------------------------------------------------------------------------------------------------
    // 画像から生成する高さ・法線マップのプレビュー。
    enum class ImageView
    {
        Source, // 元画像
        Height,
        Normal,
        Lit // 法線マップで陰影を付けた平面
    };

    struct ImagePreviewInput
    {
        shared_ptr<const Image> Source;
        shared_ptr<const ProcStudio::ImageMaps> Maps;
        ImageView View = ImageView::Normal;
        bool FlipY = false;
        bool SourceColor = true; // 陰影表示で元画像の色を使う。false ならグレー。
        float LightAzimuth = 0.785f;
        float LightElevation = 0.6f;
        int Tiles = 1;
    };

    // 縦横比を保って正方形のプレビューへ収める。余白は背景色。
    void BlitImageMaps(const ImagePreviewInput& input, vector<uint32_t>& out)
    {
        out.assign(static_cast<size_t>(kPreviewSize) * kPreviewSize, Pack(Float3{0.08f, 0.08f, 0.09f}, false));
        const Image* image = input.View == ImageView::Source   ? input.Source.get()
                             : input.View == ImageView::Height ? (input.Maps ? &input.Maps->Height : nullptr)
                                                               : (input.Maps ? &input.Maps->Normal : nullptr);
        if (!image || image->Width <= 0 || image->Height <= 0)
        {
            return;
        }
        const float aspect = static_cast<float>(image->Width) / image->Height;
        const float drawW = aspect >= 1 ? kPreviewSize : kPreviewSize * aspect;
        const float drawH = aspect >= 1 ? kPreviewSize / aspect : kPreviewSize;
        const float originX = (kPreviewSize - drawW) * 0.5f;
        const float originY = (kPreviewSize - drawH) * 0.5f;
        const float cosE = cosf(input.LightElevation);
        // 接空間の光の向き（+X = 右、+Y = 画像の上、+Z = 手前）。
        const Float3 light = Normalize({cosE * cosf(input.LightAzimuth), cosE * sinf(input.LightAzimuth),
                                        sinf(input.LightElevation)});
        for (int y = 0; y < kPreviewSize; ++y)
        {
            const float fy = (y + 0.5f - originY) / drawH;
            if (fy < 0 || fy >= 1)
            {
                continue;
            }
            for (int x = 0; x < kPreviewSize; ++x)
            {
                const float fx = (x + 0.5f - originX) / drawW;
                if (fx < 0 || fx >= 1)
                {
                    continue;
                }
                const float u = fx * input.Tiles;
                const float v = fy * input.Tiles;
                const Float4 c = image->Sample(u, v);
                uint32_t& pixel = out[static_cast<size_t>(y) * kPreviewSize + x];
                if (input.View == ImageView::Lit)
                {
                    const float ny = (c.Y * 2 - 1) * (input.FlipY ? -1.0f : 1.0f);
                    const Float3 n = Normalize({c.X * 2 - 1, ny, c.Z * 2 - 1});
                    Float3 albedo{0.72f, 0.72f, 0.72f};
                    if (input.SourceColor && input.Source)
                    {
                        const Float4 a = input.Source->Sample(u, v);
                        albedo = {a.X, a.Y, a.Z};
                    }
                    const float diffuse = (std::max)(Dot(n, light), 0.0f);
                    const Float3 halfVector = Normalize(light + Float3{0, 0, 1});
                    const float specular = powf((std::max)(n.Z > 0 ? Dot(n, halfVector) : 0.0f, 0.0f), 40) * 0.15f;
                    pixel = Pack(albedo * (0.18f + diffuse * 1.0f) + Float3{1, 1, 1} * specular, true);
                }
                else
                {
                    // 高さ・法線は保存されるファイルと同じ値をそのまま表示する。
                    pixel = Pack({c.X, c.Y, c.Z}, input.View == ImageView::Source);
                }
            }
        }
    }

    // ------------------------------------------------------------------------------------------------
    // ウィンドウの状態。
    enum class Category
    {
        Texture,
        Solid,
        Model
    };
    enum class Surface
    {
        VertexColor,
        Texture,
        Solid
    };
    enum class ViewMode
    {
        Model3D,
        Flat2D,
        VolumeSlice
    };

    template <typename T> struct Job
    {
        future<T> Result;
        bool Running = false;
        bool Dirty = true;

        template <typename F> void Start(F work)
        {
            Result = async(launch::async, std::move(work));
            Running = true;
            Dirty = false;
        }

        bool Poll(T& value)
        {
            if (!Running || Result.wait_for(chrono::seconds(0)) != future_status::ready)
            {
                return false;
            }
            Running = false;
            value = Result.get();
            return true;
        }
    };

    enum class TextureSource
    {
        Procedural,
        Image
    };

    // 画像から生成する高さ・法線マップの元。プレビューは長辺 kImageProcessMax に縮小して処理し、
    // 保存時は元ファイルを読み直して元の解像度で処理する（巨大画像を常駐させないため）。
    constexpr int kImageProcessMax = 1024;

    struct LoadedImage
    {
        filesystem::path Path;
        shared_ptr<const Image> Preview;
        int Width = 0;
        int Height = 0;
        bool Srgb = true;
        string Error;
    };

    struct ImageSourceState
    {
        TextureSource Source = TextureSource::Procedural;
        ProcStudio::ImageMapParams Params;
        ImageView View = ImageView::Normal;
        bool SourceColor = true;
        float LightAzimuth = 0.785f;   // ラジアン
        float LightElevation = 0.6f;   // ラジアン
        LoadedImage Loaded;            // 現在の画像（Preview が null なら未読み込み）
        filesystem::path PendingPath;  // 読み込み中に次のドロップがあった場合の予約
        shared_ptr<const ProcStudio::ImageMaps> Maps;
        // エクスプローラーからのドロップを受け付ける矩形（クライアント座標）と、記録したフレーム。
        vector<ImVec4> DropRects;
        int DropFrame = -100;
    };

    struct State
    {
        Category Mode = Category::Texture;
        ViewMode View = ViewMode::Flat2D;
        Surface ModelSurface = Surface::VertexColor;
        TextureParams Texture;
        TextureParams Solid;
        ModelParams Model;
        ModelKind TexturePreviewMesh = ModelKind::Sphere;
        float UvScale = 1.0f;
        float SolidScale = 1.0f;
        int Tiles = 1;
        float Slice = 0.5f;
        bool Animate = false;
        float AnimationSpeed = 1.0f;
        int SaveResolution = 2;    // 256, 512, 1024, 2048, 4096
        int ModelExportFormat = 0; // OBJ, FBX (ASCII)
        int VolumeExtent = 1;      // 64, 128
        OrbitCamera Orbit;

        Image TextureImage;
        shared_ptr<const Mesh> ModelMesh = make_shared<Mesh>();
        ModelParams GeneratedModel;
        ModelParams PendingModel;
        bool ShowTerrainGrid = true;
        Mesh PrimitiveMesh;
        ModelKind PrimitiveKind = ModelKind::Count;
        vector<Float4> SolidVoxels;
        int SolidExtent = 0;
        Volume CloudPreview;

        Job<Image> TextureJob;
        ImageSourceState Img;
        Job<LoadedImage> ImageLoadJob;
        Job<ProcStudio::ImageMaps> ImageMapJob;
        bool PreviewFromImage = false;
        Job<Mesh> MeshJob;
        Job<vector<uint32_t>> PreviewJob;
        Category PreviewCategory = Category::Texture;
        ViewMode PreviewView = ViewMode::Model3D;
        Job<vector<Float4>> SolidJob;
        Job<Volume> CloudSliceJob;
        future<string> SaveTask;
        bool Saving = false;
        future<Volume> CloudTask;
        bool CloudRunning = false;
        string Status;

        vector<uint32_t> Pixels;
        bool PreviewDirty = true;
        bool UploadPending = false;
        GpuPreview Gpu;
        ProcStudio::TextureGpu TextureCompute;
        bool GpuAttempted = false;
        bool GpuAvailable = false;
        bool GpuTextureValid = false;
        bool CpuTextureValid = false;
        bool CpuTextureCurrent = false;
        bool ReadbackPending = false;
        UINT64 ReadbackFence = 0;
        string ComputeError;
        bool Initialized = false;
    };

    State& S()
    {
        static State state;
        return state;
    }

    void InitializeDefaults(State& s)
    {
        s.Texture.Kind = TextureKind::FractalNoise;
        s.Solid.Kind = TextureKind::Marble;
        s.Solid.Noise.Frequency = 3;
        s.Solid.ColorA = {0.12f, 0.12f, 0.14f};
        s.Solid.ColorB = {0.92f, 0.91f, 0.88f};
        s.Solid.ColorC = {0.6f, 0.62f, 0.66f};
        s.Model.Kind = ModelKind::FractalTree;
        s.Model.Noise.Frequency = 3;
        s.Initialized = true;
    }

    // ------------------------------------------------------------------------------------------------
    // パラメーター編集用ウィジェット。
    bool NoiseWidgets(NoiseParams& n)
    {
        bool changed = false;
        int basis = static_cast<int>(n.BasisType);
        if (ImGui::Combo("基底ノイズ", &basis, "Perlin\0Value\0Worley (F1)\0Cell Edge (F2-F1)\0"
                         "White\0Simplex\0Voronoi (セル ID)\0Cell (格子 ID)\0Gabor\0"
                         "Wavelet (Compact)\0Curl (Z 成分)\0Value-Gradient\0Sparse Convolution\0"
                         "Spot\0Local Random Phase\0Chevron\0Lattice-Free\0Phasor\0"
                         "Anisotropic Gabor\0Harmonic\0Flow\0Ridged (単一)\0"))
        {
            n.BasisType = static_cast<Basis>(basis);
            changed = true;
        }
        int fractal = static_cast<int>(n.Fractal);
        if (ImGui::Combo("フラクタル", &fractal, "fBm\0Ridged Multifractal\0Billow\0Turbulence\0Hybrid Multifractal\0Domain Warp\0なし\0"
                         "Multifractal\0Heterogeneous Multifractal\0"))
        {
            n.Fractal = static_cast<FractalType>(fractal);
            changed = true;
        }
        changed |= ImGui::InputScalar("Seed", ImGuiDataType_U32, &n.Seed);
        changed |= ImGui::SliderFloat("周波数", &n.Frequency, 0.5f, 32.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
        ImGui::BeginDisabled(n.Fractal == FractalType::None);
        changed |= ImGui::SliderInt("オクターブ", &n.Octaves, 1, 10);
        changed |= ImGui::SliderFloat("Lacunarity", &n.Lacunarity, 1.0f, 4.0f);
        changed |= ImGui::SliderFloat("Gain", &n.Gain, 0.1f, 0.9f);
        ImGui::EndDisabled();
        if (n.Fractal == FractalType::DomainWarp)
        {
            changed |= ImGui::SliderFloat("Warp 強度", &n.WarpStrength, 0.0f, 4.0f);
        }
        if (n.Fractal == FractalType::Ridged || n.Fractal == FractalType::HybridMultifractal ||
            n.Fractal == FractalType::HeterogeneousMultifractal)
        {
            changed |= ImGui::SliderFloat("Ridge オフセット", &n.RidgeOffset, 0.5f, 1.5f);
        }
        if (n.BasisType == Basis::Gabor || n.BasisType == Basis::AnisotropicGabor ||
            n.BasisType == Basis::Phasor || n.BasisType == Basis::LocalRandom)
        {
            changed |= ImGui::SliderFloat("カーネル周波数", &n.KernelFrequency, 0.25f, 8.0f);
        }
        if (n.BasisType == Basis::AnisotropicGabor || n.BasisType == Basis::Phasor)
        {
            changed |= ImGui::SliderAngle("方向", &n.Direction);
            changed |= ImGui::SliderFloat("異方性", &n.Anisotropy, 1.0f, 8.0f);
        }
        if (n.BasisType == Basis::Gabor || n.BasisType == Basis::AnisotropicGabor ||
            n.BasisType == Basis::Phasor || n.BasisType == Basis::LocalRandom ||
            n.BasisType == Basis::Flow || n.BasisType == Basis::Chevron)
        {
            changed |= ImGui::SliderAngle("位相", &n.Phase, -360.0f, 360.0f);
        }
        return changed;
    }

    bool TextureWidgets(TextureParams& t, bool solid)
    {
        bool changed = false;
        if (ImGui::BeginCombo("種類", TextureKindName(t.Kind)))
        {
            for (int i = 0; i < static_cast<int>(TextureKind::Count); ++i)
            {
                const auto kind = static_cast<TextureKind>(i);
                if (solid && !IsSolidCapable(kind))
                {
                    continue;
                }
                if (ImGui::Selectable(TextureKindName(kind), t.Kind == kind))
                {
                    t.Kind = kind;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        if (!solid)
        {
            int output = static_cast<int>(t.Output);
            if (ImGui::Combo("出力", &output, "カラー\0高さ (Height)\0法線 (Normal)\0"))
            {
                t.Output = static_cast<TextureOutput>(output);
                changed = true;
            }
            if (t.Output == TextureOutput::Normal)
            {
                changed |= ImGui::SliderFloat("法線強度", &t.NormalStrength, 0.5f, 32.0f);
            }
            const bool escape = t.Kind == TextureKind::Mandelbrot || t.Kind == TextureKind::Julia;
            ImGui::BeginDisabled(escape);
            changed |= ImGui::Checkbox("タイリング (シームレス)", &t.Tileable);
            ImGui::EndDisabled();
        }
        const bool escape = t.Kind == TextureKind::Mandelbrot || t.Kind == TextureKind::Julia;
        const bool geometric = t.Kind == TextureKind::Geometric;
        if (!escape && !geometric && t.Kind != TextureKind::Checker &&
            ImGui::TreeNodeEx("ノイズ", ImGuiTreeNodeFlags_DefaultOpen))
        {
            changed |= NoiseWidgets(t.Noise);
            ImGui::TreePop();
        }
        switch (t.Kind)
        {
            case TextureKind::Geometric: {
                int pattern = static_cast<int>(t.Pattern);
                if (ImGui::Combo("模様", &pattern, "縞\0格子\0水玉\0ひし形\0六角形\0三角形\0"))
                {
                    t.Pattern = static_cast<ProcStudio::GeometricPattern>(pattern);
                    changed = true;
                }
                changed |= ImGui::SliderInt("横の繰り返し数", &t.RepeatX, 1, 64, "%d", ImGuiSliderFlags_AlwaysClamp);
                if (t.Pattern != ProcStudio::GeometricPattern::Stripes)
                {
                    changed |=
                        ImGui::SliderInt("縦の繰り返し数", &t.RepeatY, 1, 64, "%d", ImGuiSliderFlags_AlwaysClamp);
                }
                changed |= ImGui::SliderFloat("太さ / 大きさ", &t.PatternSize, 0.05f, 0.95f, "%.2f",
                                              ImGuiSliderFlags_AlwaysClamp);
                changed |= ImGui::SliderFloat("境界の柔らかさ", &t.PatternSoftness, 0.001f, 0.15f, "%.3f",
                                              ImGuiSliderFlags_AlwaysClamp);
                ImGui::TextWrapped("縦横の繰り返し数を揃えると、正方形の画像では模様の縦横比が揃います。");
                break;
            }
            case TextureKind::Marble:
            case TextureKind::Wood:
                changed |= ImGui::SliderFloat("乱流", &t.Turbulence, 0.0f, 12.0f);
                changed |= ImGui::SliderFloat(t.Kind == TextureKind::Wood ? "年輪数" : "縞の数", &t.RingCount, 1.0f,
                                              24.0f, "%.0f");
                break;
            case TextureKind::Brick:
                changed |= ImGui::SliderInt("横の数", &t.BricksX, 1, 16);
                changed |= ImGui::SliderInt("縦の数", &t.BricksY, 1, 32);
                changed |= ImGui::SliderFloat("目地の幅", &t.Mortar, 0.0f, 0.2f);
                break;
            case TextureKind::Checker:
                changed |= ImGui::SliderInt("分割数", &t.BricksX, 1, 32);
                break;
            case TextureKind::FractalWater:
                changed |= ImGui::SliderFloat("時間", &t.Time, 0.0f, 100.0f);
                changed |= ImGui::SliderFloat("波の鋭さ", &t.Choppiness, 0.0f, 1.0f);
                break;
            case TextureKind::FractalCloud:
                changed |= ImGui::SliderFloat("被覆率", &t.Coverage, 0.0f, 1.0f);
                changed |= ImGui::SliderFloat("柔らかさ", &t.Softness, 0.02f, 1.0f);
                break;
            case TextureKind::Mandelbrot:
            case TextureKind::Julia:
                changed |=
                    ImGui::DragFloat("中心 X", &t.CenterX, 0.01f / (std::max)(t.Zoom, 1.0f), -2.5f, 2.5f, "%.6f");
                changed |=
                    ImGui::DragFloat("中心 Y", &t.CenterY, 0.01f / (std::max)(t.Zoom, 1.0f), -2.0f, 2.0f, "%.6f");
                changed |= ImGui::SliderFloat("ズーム", &t.Zoom, 0.25f, 10000.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
                changed |= ImGui::SliderInt("反復回数", &t.Iterations, 16, 2000);
                if (t.Kind == TextureKind::Julia)
                {
                    changed |= ImGui::DragFloat("c (実部)", &t.JuliaX, 0.002f, -2.0f, 2.0f, "%.4f");
                    changed |= ImGui::DragFloat("c (虚部)", &t.JuliaY, 0.002f, -2.0f, 2.0f, "%.4f");
                }
                break;
            default:
                break;
        }
        if (!escape && t.Kind != TextureKind::FractalCloud)
        {
            changed |= ImGui::SliderFloat("コントラスト", &t.Contrast, 0.2f, 4.0f);
            changed |= ImGui::SliderFloat("バイアス", &t.Bias, -0.5f, 0.5f);
            if (ImGui::TreeNode("カラー"))
            {
                const char* colorALabel = t.Kind == TextureKind::Brick ? "未使用" : "カラー A (低)";
                const char* colorBLabel = t.Kind == TextureKind::Brick ? "目地" : "カラー B (中)";
                if (geometric)
                {
                    colorALabel = "背景色";
                    colorBLabel = "模様の色";
                }
                changed |= ImGui::ColorEdit3(colorALabel, t.ColorA.data(), ImGuiColorEditFlags_Float);
                changed |= ImGui::ColorEdit3(colorBLabel, t.ColorB.data(), ImGuiColorEditFlags_Float);
                if (!geometric)
                {
                    changed |= ImGui::ColorEdit3(t.Kind == TextureKind::Brick ? "レンガ" : "カラー C (高)",
                                                 t.ColorC.data(), ImGuiColorEditFlags_Float);
                }
                ImGui::TreePop();
            }
        }
        return changed;
    }

    bool LeafWidgets(ProcStudio::LeafParams& leaf)
    {
        bool changed = false;
        if (ImGui::BeginCombo("葉の形", ProcStudio::LeafShapeName(leaf.Shape)))
        {
            for (int i = 0; i < static_cast<int>(ProcStudio::LeafShape::Count); ++i)
            {
                const auto shape = static_cast<ProcStudio::LeafShape>(i);
                if (ImGui::Selectable(ProcStudio::LeafShapeName(shape), leaf.Shape == shape))
                {
                    leaf.Shape = shape;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        const bool fractal = leaf.Shape == ProcStudio::LeafShape::Pinnate ||
                             leaf.Shape == ProcStudio::LeafShape::Fern || leaf.Shape == ProcStudio::LeafShape::Palmate;
        if (fractal)
        {
            changed |= ImGui::SliderInt("フラクタルの深さ", &leaf.Depth, 0, 4);
            changed |= ImGui::SliderInt(leaf.Shape == ProcStudio::LeafShape::Palmate ? "裂片の数" : "小葉の対数",
                                        &leaf.Leaflets, 1, 10);
            changed |= ImGui::SliderFloat("小葉の角度", &leaf.LeafletAngle, 5.0f, 85.0f);
            changed |= ImGui::SliderFloat("小葉の縮小率", &leaf.LeafletScale, 0.15f, 0.8f);
            changed |= ImGui::SliderFloat("先端への縮小", &leaf.Taper, 0.0f, 1.0f);
        }
        if (leaf.Shape != ProcStudio::LeafShape::Card)
        {
            changed |= ImGui::SliderFloat("葉身の幅", &leaf.Width, 0.05f, 1.0f);
            changed |= ImGui::SliderInt("葉身の分割", &leaf.Segments, 2, 12);
        }
        changed |= ImGui::SliderFloat("垂れ (反り)", &leaf.Droop, -0.5f, 1.0f);
        changed |= ImGui::SliderFloat("中央の折れ", &leaf.Fold, -0.5f, 0.8f);
        changed |= ImGui::ColorEdit3("葉の色", leaf.Color.data(), ImGuiColorEditFlags_Float);
        changed |= ImGui::ColorEdit3("先端の色", leaf.TipColor.data(), ImGuiColorEditFlags_Float);
        changed |= ImGui::SliderFloat("色のばらつき", &leaf.ColorVariation, 0.0f, 0.5f);
        return changed;
    }

    bool ModelWidgets(ModelParams& m)
    {
        bool changed = false;
        if (ImGui::BeginCombo("モデル", ModelKindName(m.Kind)))
        {
            for (int i = static_cast<int>(ModelKind::Terrain); i < static_cast<int>(ModelKind::Count); ++i)
            {
                const auto kind = static_cast<ModelKind>(i);
                if (ImGui::Selectable(ModelKindName(kind), m.Kind == kind))
                {
                    m.Kind = kind;
                    changed = true;
                }
            }
            for (int i = 0; i < static_cast<int>(ModelKind::Terrain); ++i)
            {
                const auto kind = static_cast<ModelKind>(i);
                if (ImGui::Selectable(ModelKindName(kind), m.Kind == kind))
                {
                    m.Kind = kind;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        switch (m.Kind)
        {
            case ModelKind::Terrain:
                changed |= ImGui::InputScalar("Seed##model", ImGuiDataType_U32, &m.Seed);
                ImGui::SeparatorText("地形マップ");
                changed |= ImGui::SliderInt("区画数 X", &m.TerrainMapsX, 1, ProcStudio::kMaxTerrainMaps, "%d",
                                            ImGuiSliderFlags_AlwaysClamp);
                changed |= ImGui::SliderInt("区画数 Z", &m.TerrainMapsZ, 1, ProcStudio::kMaxTerrainMaps, "%d",
                                            ImGuiSliderFlags_AlwaysClamp);
                changed |= ImGui::SliderInt("1区画の分割数", &m.Resolution, 8, 256, "%d", ImGuiSliderFlags_AlwaysClamp);
                changed |=
                    ImGui::SliderFloat("1区画のサイズ", &m.Size, 1.0f, 100.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
                {
                    const size_t stepsX = static_cast<size_t>(m.Resolution) * m.TerrainMapsX;
                    const size_t stepsZ = static_cast<size_t>(m.Resolution) * m.TerrainMapsZ;
                    ImGui::Text("全体 %.1f × %.1f m / %d 区画", m.Size * m.TerrainMapsX, m.Size * m.TerrainMapsZ,
                                m.TerrainMapsX * m.TerrainMapsZ);
                    ImGui::TextDisabled("生成予定: 頂点 %zu / 三角形 %zu", (stepsX + 1) * (stepsZ + 1),
                                        stepsX * stepsZ * 2);
                    ImGui::TextWrapped("同じノイズの続きを生成し、全体を1つのメッシュとして保存します。");
                }
                changed |= ImGui::SliderFloat("高さ", &m.Height, 0.0f, 30.0f);
                changed |= ImGui::Checkbox("高度・傾斜で色分け", &m.TerrainColors);
                if (ImGui::TreeNodeEx("地形ノイズ", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    changed |= NoiseWidgets(m.Noise);
                    ImGui::TreePop();
                }
                break;
            case ModelKind::FractalTree:
                changed |= ImGui::InputScalar("Seed##model", ImGuiDataType_U32, &m.Seed);
                changed |= ImGui::SliderInt("再帰の深さ", &m.Depth, 1, 8);
                changed |= ImGui::SliderInt("分岐数", &m.Branches, 1, 5);
                changed |= ImGui::SliderFloat("幹の長さ", &m.TrunkLength, 0.2f, 6.0f);
                changed |= ImGui::SliderFloat("幹の太さ", &m.TrunkRadius, 0.01f, 0.6f);
                changed |= ImGui::SliderFloat("長さの減衰", &m.LengthRatio, 0.4f, 0.95f);
                changed |= ImGui::SliderFloat("太さの減衰", &m.RadiusRatio, 0.3f, 0.95f);
                changed |= ImGui::SliderFloat("分岐角度", &m.SpreadAngle, 5.0f, 85.0f);
                changed |= ImGui::SliderFloat("屈光性 (上向き)", &m.Tropism, -0.5f, 1.0f);
                changed |= ImGui::SliderFloat("ばらつき", &m.Jitter, 0.0f, 1.0f);
                changed |= ImGui::SliderInt("断面の分割数", &m.RadialSegments, 3, 16);
                changed |= ImGui::Checkbox("葉", &m.Leaves);
                if (m.Leaves)
                {
                    changed |= ImGui::SliderFloat("葉の大きさ", &m.LeafSize, 0.02f, 1.5f);
                    changed |= ImGui::SliderInt("枝先あたりの葉", &m.LeavesPerTip, 1, 8);
                    changed |= ImGui::SliderInt("葉の三角形上限", &m.LeafTriangleBudget, 20000, 2000000, "%d",
                                                ImGuiSliderFlags_Logarithmic);
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip("超えるとフラクタルの深さを自動で下げます。");
                    }
                    if (ImGui::TreeNodeEx("葉 (フラクタル)", ImGuiTreeNodeFlags_DefaultOpen))
                    {
                        changed |= LeafWidgets(m.Leaf);
                        ImGui::TreePop();
                    }
                }
                break;
            case ModelKind::FractalLeaf:
                changed |= ImGui::InputScalar("Seed##model", ImGuiDataType_U32, &m.Seed);
                changed |= LeafWidgets(m.Leaf);
                break;
            case ModelKind::Rock:
                changed |= ImGui::InputScalar("Seed##model", ImGuiDataType_U32, &m.Seed);
                changed |= ImGui::SliderInt("細分化", &m.Subdivisions, 1, 6);
                changed |= ImGui::SliderFloat("変位量", &m.Displacement, 0.0f, 1.0f);
                if (ImGui::TreeNodeEx("変位ノイズ", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    changed |= NoiseWidgets(m.Noise);
                    ImGui::TreePop();
                }
                break;
            case ModelKind::WaterSurface:
                changed |= ImGui::InputScalar("Seed##model", ImGuiDataType_U32, &m.Seed);
                changed |= ImGui::SliderInt("解像度", &m.Resolution, 8, 192);
                changed |= ImGui::SliderFloat("サイズ", &m.Size, 1.0f, 50.0f);
                changed |= ImGui::SliderFloat("波の高さ", &m.WaveAmplitude, 0.0f, 3.0f);
                changed |= ImGui::SliderFloat("時間", &m.Time, 0.0f, 100.0f);
                if (ImGui::TreeNodeEx("波ノイズ", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    changed |= NoiseWidgets(m.Noise);
                    ImGui::TreePop();
                }
                break;
            case ModelKind::MengerSponge:
                changed |= ImGui::SliderInt("レベル", &m.Level, 0, 3);
                break;
            case ModelKind::SierpinskiPyramid:
                changed |= ImGui::SliderInt("レベル", &m.Level, 0, 7);
                break;
            default:
                break;
        }
        return changed;
    }

    // ------------------------------------------------------------------------------------------------
    // バックグラウンド処理。
    const Mesh& PreviewMesh(State& s)
    {
        if (s.Mode == Category::Texture)
        {
            if (s.PrimitiveKind != s.TexturePreviewMesh)
            {
                ModelParams primitive;
                primitive.Kind = s.TexturePreviewMesh;
                s.PrimitiveMesh = GenerateModel(primitive);
                s.PrimitiveKind = s.TexturePreviewMesh;
            }
            return s.PrimitiveMesh;
        }
        return *s.ModelMesh;
    }

    bool NeedsTexture(const State& s)
    {
        return s.Mode == Category::Texture || (s.Mode == Category::Model && s.ModelSurface == Surface::Texture);
    }

    bool NeedsSolid(const State& s)
    {
        return s.Mode == Category::Solid || (s.Mode == Category::Model && s.ModelSurface == Surface::Solid);
    }

    bool NeedsMesh(const State& s)
    {
        return s.Mode != Category::Texture;
    }

    bool ImageActive(const State& s)
    {
        return s.Mode == Category::Texture && s.Img.Source == TextureSource::Image;
    }

    // std::async のワーカーで WIC を使うため、スレッドごとに COM を初期化する。
    struct ComScope
    {
        HRESULT Result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~ComScope()
        {
            if (SUCCEEDED(Result))
            {
                CoUninitialize();
            }
        }
    };

    bool IsImagePath(const filesystem::path& path)
    {
        return AssetRegistry::Classify(path) == AssetType::Texture;
    }

    void StartImageLoad(State& s, const filesystem::path& path)
    {
        s.ImageLoadJob.Start([path] {
            ComScope com;
            LoadedImage result;
            result.Path = path;
            Image full;
            bool srgb = true;
            if (!ProceduralIO::LoadImageFile(path, full, result.Error, &srgb))
            {
                if (result.Error.empty())
                {
                    result.Error = "画像を読み込めません";
                }
                return result;
            }
            result.Width = full.Width;
            result.Height = full.Height;
            result.Srgb = srgb;
            result.Preview = make_shared<const Image>(ProcStudio::DownsampleImage(full, kImageProcessMax));
            return result;
        });
        s.Status = "画像を読み込み中: " + ProceduralIO::ToUtf8(path.filename());
    }

    // ドロップ・参照から呼ぶ。読み込み中なら完了後に最新の 1 件だけ読み込む。
    void RequestImageLoad(State& s, const filesystem::path& path)
    {
        if (!IsImagePath(path))
        {
            s.Status = "画像ファイルではありません: " + ProceduralIO::ToUtf8(path.filename());
            return;
        }
        s.Mode = Category::Texture;
        s.Img.Source = TextureSource::Image;
        if (s.View == ViewMode::VolumeSlice)
        {
            s.View = ViewMode::Flat2D;
        }
        s.PreviewDirty = true;
        if (s.ImageLoadJob.Running)
        {
            s.Img.PendingPath = path;
            return;
        }
        StartImageLoad(s, path);
    }

    void PumpImageJobs(State& s)
    {
        LoadedImage loaded;
        if (s.ImageLoadJob.Poll(loaded))
        {
            if (loaded.Preview)
            {
                s.Status = "画像を読み込みました: " + ProceduralIO::ToUtf8(loaded.Path.filename()) + " (" +
                           to_string(loaded.Width) + " x " + to_string(loaded.Height) + ")";
                s.Img.Loaded = std::move(loaded);
                s.ImageMapJob.Dirty = true;
                s.PreviewDirty = true;
            }
            else
            {
                s.Status = "画像の読み込みに失敗: " + loaded.Error;
            }
            if (!s.Img.PendingPath.empty())
            {
                const auto next = s.Img.PendingPath;
                s.Img.PendingPath.clear();
                StartImageLoad(s, next);
            }
        }
        ProcStudio::ImageMaps maps;
        if (s.ImageMapJob.Poll(maps))
        {
            s.Img.Maps = make_shared<const ProcStudio::ImageMaps>(std::move(maps));
            s.PreviewDirty = true;
        }
        if (s.Img.Loaded.Preview && s.ImageMapJob.Dirty && !s.ImageMapJob.Running)
        {
            const auto source = s.Img.Loaded.Preview;
            const auto params = s.Img.Params;
            const bool srgb = s.Img.Loaded.Srgb;
            const float scale = static_cast<float>(source->Width) / (std::max)(s.Img.Loaded.Width, 1);
            s.ImageMapJob.Start([source, params, srgb, scale] {
                return ProcStudio::BuildImageMaps(*source, params, srgb, scale);
            });
        }
    }

    void PumpJobs(State& s)
    {
        if (!s.GpuAttempted && GraphicsDevice::GetDevice())
        {
            s.GpuAttempted = true;
            s.GpuAvailable = s.TextureCompute.Initialize(GraphicsDevice::GetDevice(), s.ComputeError);
        }
        if (s.ReadbackPending && GraphicsDevice::GetCompletedFence() >= s.ReadbackFence)
        {
            s.ReadbackPending = false;
            s.CpuTextureValid = s.TextureCompute.Readback(s.TextureImage, s.ComputeError);
            s.CpuTextureCurrent = s.CpuTextureValid;
            if (!s.CpuTextureValid)
            {
                s.GpuAvailable = false;
                s.GpuTextureValid = false;
                s.TextureJob.Dirty = true;
            }
            s.PreviewDirty = true;
        }
        Image image;
        if (s.TextureJob.Poll(image))
        {
            s.TextureImage = std::move(image);
            s.CpuTextureValid = true;
            s.CpuTextureCurrent = true;
            s.PreviewDirty = true;
        }
        Mesh mesh;
        if (s.MeshJob.Poll(mesh))
        {
            s.ModelMesh = make_shared<Mesh>(std::move(mesh));
            s.GeneratedModel = s.PendingModel;
            s.PreviewDirty = true;
        }
        vector<Float4> voxels;
        if (s.SolidJob.Poll(voxels))
        {
            s.SolidVoxels = std::move(voxels);
            s.SolidExtent = kSolidPreview;
            s.PreviewDirty = true;
        }
        Volume cloud;
        if (s.CloudSliceJob.Poll(cloud))
        {
            s.CloudPreview = std::move(cloud);
            s.PreviewDirty = true;
        }
        PumpImageJobs(s);
        // 種類ごとに実行中ジョブは最大 1 件。実行中の変更は、完了後に再実行して反映する。
        // 画像ソース中はプロシージャルの生成を止める（戻したときに Dirty なら再生成される）。
        if (NeedsTexture(s) && !ImageActive(s) && s.TextureJob.Dirty && !s.TextureJob.Running &&
            !s.ReadbackPending)
        {
            const auto params = s.Texture;
            s.GpuTextureValid = false;
            if (s.GpuAvailable && ProcStudio::TextureGpu::Supports(params))
            {
                s.GpuTextureValid = s.TextureCompute.Generate(GraphicsDevice::GetCommandList(), params, kTexturePreview,
                                                              kTexturePreview, s.ComputeError);
                if (!s.GpuTextureValid)
                {
                    s.GpuAvailable = false;
                }
            }
            // Keep the last completed snapshot visible while the next one is generated.
            s.CpuTextureCurrent = false;
            if (s.GpuTextureValid)
            {
                s.TextureJob.Dirty = false;
                s.PreviewDirty = true;
            }
            else
            {
                s.TextureJob.Start([params] {
                    return GenerateTexture(params, kTexturePreview, kTexturePreview);
                });
            }
        }
        // Software mesh previews need CPU pixels; flat previews stay on the GPU.
        const bool cpuTextureNeeded = NeedsTexture(s) && !ImageActive(s) && s.View == ViewMode::Model3D;
        if (s.GpuTextureValid && cpuTextureNeeded && !s.CpuTextureCurrent && !s.ReadbackPending)
        {
            if (s.TextureCompute.CopyToReadback(GraphicsDevice::GetCommandList(), s.ComputeError))
            {
                s.ReadbackPending = true;
                s.ReadbackFence = GraphicsDevice::GetPendingSubmissionFence();
            }
            else
            {
                s.GpuAvailable = false;
                s.GpuTextureValid = false;
                s.TextureJob.Dirty = true;
            }
        }
        if (NeedsMesh(s) && s.MeshJob.Dirty && !s.MeshJob.Running)
        {
            const auto params = s.Model;
            s.PendingModel = params;
            s.MeshJob.Start([params] {
                return GenerateModel(params);
            });
        }
        if (NeedsSolid(s) && s.SolidJob.Dirty && !s.SolidJob.Running)
        {
            auto params = s.Solid;
            params.Tileable = true;
            s.SolidJob.Start([params] {
                return GenerateSolidVolume(params, kSolidPreview);
            });
        }
        if (s.Mode == Category::Texture && s.View == ViewMode::VolumeSlice && s.CloudSliceJob.Dirty &&
            !s.CloudSliceJob.Running)
        {
            const auto params = s.Texture;
            s.CloudSliceJob.Start([params] {
                return GenerateVolume(params, 64);
            });
        }
    }

    void RenderImagePreview(State& s)
    {
        if (s.PreviewDirty && !s.PreviewJob.Running)
        {
            const auto& img = s.Img;
            if (s.View == ViewMode::Model3D && img.Maps)
            {
                RenderInput input;
                const auto geometry = make_shared<const Mesh>(PreviewMesh(s));
                input.View = s.Orbit;
                input.UvScale = s.UvScale;
                input.Mode = Shading::Texture;
                input.NormalFlipY = img.Params.FlipY;
                shared_ptr<const Image> albedo = img.Loaded.Preview;
                if (!img.SourceColor || !albedo)
                {
                    auto gray = make_shared<Image>();
                    gray->Width = 1;
                    gray->Height = 1;
                    gray->Pixels = {Float4{0.6f, 0.6f, 0.6f, 1.0f}};
                    albedo = gray;
                }
                const auto maps = img.Maps;
                s.PreviewCategory = s.Mode;
                s.PreviewView = s.View;
                s.PreviewFromImage = true;
                s.PreviewJob.Start([input, geometry, albedo, maps]() mutable {
                    input.Geometry = geometry.get();
                    input.Texture = albedo.get();
                    input.NormalMap = &maps->Normal;
                    vector<uint32_t> rendered;
                    Rasterize(input, rendered);
                    return rendered;
                });
            }
            else
            {
                ImagePreviewInput input;
                input.Source = img.Loaded.Preview;
                input.Maps = img.Maps;
                input.View = img.View;
                input.FlipY = img.Params.FlipY;
                input.SourceColor = img.SourceColor;
                input.LightAzimuth = img.LightAzimuth;
                input.LightElevation = img.LightElevation;
                input.Tiles = s.Tiles;
                BlitImageMaps(input, s.Pixels);
                s.UploadPending = true;
            }
            s.PreviewDirty = false;
        }
        if (s.UploadPending && s.Pixels.size() == size_t(kPreviewSize) * kPreviewSize && s.Gpu.Upload(s.Pixels))
        {
            s.UploadPending = false;
        }
    }

    void RenderPreview(State& s)
    {
        // Drain finished software jobs even while the GPU flat view is active.
        vector<uint32_t> pixels;
        if (s.PreviewJob.Poll(pixels) && s.PreviewCategory == s.Mode && s.PreviewView == s.View &&
            s.PreviewFromImage == ImageActive(s))
        {
            s.Pixels = std::move(pixels);
            s.UploadPending = true;
        }
        if (ImageActive(s))
        {
            RenderImagePreview(s);
            return;
        }
        if (s.Mode == Category::Texture && s.View == ViewMode::Flat2D && s.GpuTextureValid)
        {
            if (s.PreviewDirty)
            {
                auto* packed =
                    s.TextureCompute.Preview(GraphicsDevice::GetCommandList(), kPreviewSize, s.Tiles, s.ComputeError);
                if (packed && s.Gpu.CopyGpuPixels(packed))
                {
                    s.PreviewDirty = false;
                    s.UploadPending = false;
                }
                else
                {
                    s.GpuAvailable = false;
                    s.GpuTextureValid = false;
                    s.TextureJob.Dirty = true;
                }
            }
            return;
        }
        if (NeedsTexture(s) && !s.CpuTextureValid && s.View != ViewMode::VolumeSlice)
        {
            return;
        }
        if (s.PreviewDirty && !s.PreviewJob.Running)
        {
            if (s.View == ViewMode::Flat2D && s.Mode == Category::Texture)
            {
                BlitImage(s.TextureImage, s.Texture.Output == TextureOutput::Color, s.Tiles, s.Pixels);
                s.UploadPending = true;
            }
            else if (s.View == ViewMode::VolumeSlice)
            {
                if (s.Mode == Category::Solid)
                {
                    BlitVolumeSlice(&s.SolidVoxels, nullptr, s.SolidVoxels.empty() ? 0 : s.SolidExtent, s.Slice,
                                    s.Pixels);
                }
                else
                {
                    BlitVolumeSlice(nullptr, &s.CloudPreview, s.CloudPreview.Extent, s.Slice, s.Pixels);
                }
                s.UploadPending = true;
            }
            else
            {
                RenderInput input;
                // 不変の形状を保持し、描画中に次の生成結果へ切り替わっても参照を有効に保つ。
                const auto geometry =
                    s.Mode == Category::Texture ? make_shared<const Mesh>(PreviewMesh(s)) : s.ModelMesh;
                input.View = s.Orbit;
                input.UvScale = s.UvScale;
                input.SolidExtent = s.SolidVoxels.empty() ? 0 : s.SolidExtent;
                input.SolidScale = s.SolidScale;
                if (s.Mode == Category::Texture)
                {
                    input.Mode = Shading::Texture;
                }
                else if (s.Mode == Category::Solid)
                {
                    input.Mode = Shading::Solid;
                }
                else
                {
                    input.Mode = s.ModelSurface == Surface::Texture        ? Shading::Texture
                                 : s.ModelSurface == Surface::Solid        ? Shading::Solid
                                 : s.Model.Kind == ModelKind::WaterSurface ? Shading::Water
                                                                           : Shading::VertexColor;
                }
                if (s.Mode != Category::Texture && s.GeneratedModel.Kind == ModelKind::Terrain && s.ShowTerrainGrid)
                {
                    input.TerrainGridSize = s.GeneratedModel.Size;
                }
                Image texture = input.Mode == Shading::Texture ? s.TextureImage : Image{};
                vector<Float4> solid = input.Mode == Shading::Solid ? s.SolidVoxels : vector<Float4>{};
                s.PreviewCategory = s.Mode;
                s.PreviewView = s.View;
                s.PreviewFromImage = false;
                s.PreviewJob.Start([input, geometry, texture = std::move(texture), solid = std::move(solid)]() mutable {
                    input.Geometry = geometry.get();
                    input.Texture = &texture;
                    input.Solid = &solid;
                    vector<uint32_t> rendered;
                    Rasterize(input, rendered);
                    return rendered;
                });
            }
            s.PreviewDirty = false;
        }
        if (s.UploadPending && s.Pixels.size() == size_t(kPreviewSize) * kPreviewSize && s.Gpu.Upload(s.Pixels))
        {
            s.UploadPending = false;
        }
    }

    // ------------------------------------------------------------------------------------------------
    // 書き出し。
    int SaveSize(int index)
    {
        return 256 << std::clamp(index, 0, 4);
    }

    wstring Widen(const string& text)
    {
        const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
        wstring result(length > 0 ? length - 1 : 0, L'\0');
        if (length > 1)
        {
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), length);
        }
        return result;
    }

    void StartSave(State& s, function<string()> work, const string& label)
    {
        s.SaveTask = async(launch::async, std::move(work));
        s.Saving = true;
        s.Status = "保存中: " + label;
    }

    constexpr const wchar_t* kImageFilter =
        L"PNG (*.png)\0*.png\0DDS (*.dds)\0*.dds\0TGA (*.tga)\0*.tga\0JPEG (*.jpg)\0*.jpg\0BMP (*.bmp)\0*.bmp\0";

    void SaveNoise(State& s)
    {
        string error;
        if (!NoiseSerializer::Validate(s.Texture, error))
        {
            s.Status = error;
            return;
        }
        filesystem::path path;
        const auto defaultPath = MaterialManager::Registry().Root() / "procedural.noise";
        if (!ProceduralIO::SaveFileDialog(L"Noise definition (*.noise)\0*.noise\0", L"noise", defaultPath.wstring(),
                                          path))
        {
            return;
        }
        if (NoiseSerializer::Save(path, s.Texture, error))
        {
            MaterialManager::Refresh();
            s.Status = ".noise を保存しました。アセットから Base Color / Normal / Height (POM) にドロップできます。";
        }
        else
        {
            s.Status = "保存に失敗: " + error;
        }
    }

    void LoadNoise(State& s)
    {
        filesystem::path path;
        if (!ProceduralIO::OpenFileDialog(L"Noise definition (*.noise)\0*.noise\0", path))
        {
            return;
        }
        TextureParams params;
        string error;
        if (!NoiseSerializer::Load(path, params, error))
        {
            s.Status = "読み込みに失敗: " + error;
            return;
        }
        s.Texture = params;
        s.TextureJob.Dirty = true;
        s.CloudSliceJob.Dirty = true;
        s.PreviewDirty = true;
        s.Status = ".noise の設定を読み込みました";
    }

    void SaveTexture(State& s)
    {
        filesystem::path path;
        if (!ProceduralIO::SaveFileDialog(kImageFilter, L"png", L"procedural_texture.png", path))
        {
            return;
        }
        const auto params = s.Texture;
        const int size = SaveSize(s.SaveResolution);
        const auto encoding = params.Output == TextureOutput::Color    ? ProceduralIO::Encoding::Color
                              : params.Output == TextureOutput::Height ? ProceduralIO::Encoding::Height
                                                                       : ProceduralIO::Encoding::Linear;
        const ComPtr<ID3D12Device> device = GraphicsDevice::GetDevice();
        const bool useGpu = s.GpuAvailable && ProcStudio::TextureGpu::Supports(params);
        StartSave(
            s,
            [params, size, encoding, path, device, useGpu] {
                string error;
                Image image;
                if (!useGpu || !ProcStudio::GenerateTextureGpu(device.Get(), params, size, size, image, error))
                {
                    image = GenerateTexture(params, size, size);
                }
                error.clear();
                ProceduralIO::SaveImageFile(path, image, encoding, error);
                return error;
            },
            ProceduralIO::ToUtf8(path.filename()));
    }

    enum ImageSaveTarget
    {
        SaveHeightMap = 1,
        SaveNormalMap = 2,
        SaveBothMaps = 3
    };

    // 元ファイルを読み直し、元の解像度で高さ・法線を作って保存する。
    // メモリのピークを抑えるため、高さを求めたら元画像を解放し、出力は 1 枚ずつ作る。
    void SaveImageMaps(State& s, int targets)
    {
        if (!s.Img.Loaded.Preview)
        {
            s.Status = "先に画像を読み込んでください";
            return;
        }
        const filesystem::path sourcePath = s.Img.Loaded.Path;
        const wstring suffix = targets == SaveHeightMap ? L"_height" : targets == SaveNormalMap ? L"_normal" : L"";
        const auto defaultPath = MaterialManager::Registry().Root() / (sourcePath.stem().wstring() + suffix + L".png");
        filesystem::path path;
        if (!ProceduralIO::SaveFileDialog(kImageFilter, L"png", defaultPath.wstring(), path))
        {
            return;
        }
        filesystem::path heightPath;
        filesystem::path normalPath;
        if (targets == SaveBothMaps)
        {
            heightPath = path;
            heightPath.replace_filename(path.stem().wstring() + L"_height" + path.extension().wstring());
            normalPath = path;
            normalPath.replace_filename(path.stem().wstring() + L"_normal" + path.extension().wstring());
        }
        else if (targets == SaveHeightMap)
        {
            heightPath = path;
        }
        else
        {
            normalPath = path;
        }
        const auto params = s.Img.Params;
        StartSave(
            s,
            [sourcePath, params, heightPath, normalPath]() -> string {
                ComScope com;
                string error;
                Image full;
                bool srgb = true;
                if (!ProceduralIO::LoadImageFile(sourcePath, full, error, &srgb))
                {
                    return "元画像を読み込めません: " + error;
                }
                const int width = full.Width;
                const int height = full.Height;
                const auto heights = ProcStudio::ComputeHeight(full, params, srgb, 1.0f);
                full = Image{};
                if (!heightPath.empty())
                {
                    Image image;
                    image.Width = width;
                    image.Height = height;
                    image.Pixels.resize(heights.size());
                    for (size_t i = 0; i < heights.size(); ++i)
                    {
                        image.Pixels[i] = {heights[i], heights[i], heights[i], 1.0f};
                    }
                    if (!ProceduralIO::SaveImageFile(heightPath, image, ProceduralIO::Encoding::Height, error))
                    {
                        return error;
                    }
                }
                if (!normalPath.empty())
                {
                    const Image normal = ProcStudio::HeightToNormal(heights, width, height, params, 1.0f);
                    if (!ProceduralIO::SaveImageFile(normalPath, normal, ProceduralIO::Encoding::Linear, error))
                    {
                        return error;
                    }
                }
                return {};
            },
            ProceduralIO::ToUtf8(path.filename()));
    }

    void SaveCloudVolume(State& s)
    {
        filesystem::path path;
        if (!ProceduralIO::SaveFileDialog(L"3D DDS (*.dds)\0*.dds\0", L"dds", L"fractal_cloud_volume.dds", path))
        {
            return;
        }
        const auto params = s.Texture;
        const int extent = s.VolumeExtent == 0 ? 64 : 128;
        StartSave(
            s,
            [params, extent, path] {
                string error;
                ProceduralIO::SaveDensityVolume(path, GenerateVolume(params, extent), error);
                return error;
            },
            ProceduralIO::ToUtf8(path.filename()));
    }

    void SaveSolidVolume(State& s)
    {
        filesystem::path path;
        if (!ProceduralIO::SaveFileDialog(L"3D DDS (*.dds)\0*.dds\0", L"dds", L"solid_texture.dds", path))
        {
            return;
        }
        auto params = s.Solid;
        params.Tileable = true;
        const int extent = s.VolumeExtent == 0 ? 64 : 128;
        StartSave(
            s,
            [params, extent, path] {
                string error;
                ProceduralIO::SaveColorVolume(path, GenerateSolidVolume(params, extent), extent, error);
                return error;
            },
            ProceduralIO::ToUtf8(path.filename()));
    }

    void SaveModel(State& s)
    {
        filesystem::path path;
        const bool fbx = s.ModelExportFormat == 1;
        const wchar_t* filter = fbx ? L"Autodesk FBX (*.fbx)\0*.fbx\0" : L"Wavefront OBJ (*.obj)\0*.obj\0";
        if (!ProceduralIO::SaveFileDialog(filter, fbx ? L"fbx" : L"obj",
                                          fbx ? L"procedural_model.fbx" : L"procedural_model.obj", path))
        {
            return;
        }
        const auto modelParams = s.Model;
        const bool solidMode = s.Mode == Category::Solid || s.ModelSurface == Surface::Solid;
        const bool textureMode = s.Mode == Category::Model && s.ModelSurface == Surface::Texture;
        auto solidParams = s.Solid;
        solidParams.Tileable = true;
        const auto textureParams = s.Texture;
        const float solidScale = s.SolidScale;
        const float uvScale = s.UvScale;
        const int textureSize = SaveSize(s.SaveResolution);
        StartSave(
            s,
            [=] {
                Mesh mesh = GenerateModel(modelParams);
                string error, textureName;
                if (solidMode)
                {
                    // プレビューと同じオブジェクト空間の対応付けで、立体テクスチャを頂点色へ焼き込む。
                    Float3 minimum;
                    Float3 maximum;
                    mesh.Bounds(minimum, maximum);
                    const float extent =
                        (std::max)({maximum.X - minimum.X, maximum.Y - minimum.Y, maximum.Z - minimum.Z, 1e-4f});
                    for (size_t i = 0; i < mesh.Positions.size(); ++i)
                    {
                        const Float3 p = (mesh.Positions[i] - minimum) * (solidScale / extent);
                        mesh.Colors[i] =
                            TextureColor(solidParams, p.X - floorf(p.X), p.Y - floorf(p.Y), p.Z - floorf(p.Z), true);
                    }
                }
                if (textureMode)
                {
                    for (auto& uv : mesh.UVs)
                    {
                        uv = {uv.X * uvScale, uv.Y * uvScale};
                    }
                    auto texturePath = path;
                    texturePath.replace_filename(path.stem().wstring() + L"_albedo.png");
                    const auto image = GenerateTexture(textureParams, textureSize, textureSize);
                    const auto encoding = textureParams.Output == TextureOutput::Color ? ProceduralIO::Encoding::Color
                                                                                       : ProceduralIO::Encoding::Linear;
                    if (!ProceduralIO::SaveImageFile(texturePath, image, encoding, error))
                    {
                        return error;
                    }
                    textureName = ProceduralIO::ToUtf8(texturePath.filename());
                }
                ProceduralIO::SaveModelFile(path, mesh, !textureMode, textureName, error);
                return error;
            },
            ProceduralIO::ToUtf8(path.filename()));
    }

    void ApplyCloud(State& s)
    {
        const auto params = s.Texture;
        const int extent = s.VolumeExtent == 0 ? 64 : 128;
        s.CloudTask = async(launch::async, [params, extent] {
            return GenerateVolume(params, extent);
        });
        s.CloudRunning = true;
        s.Status = "雲ノイズを生成中...";
    }

    void PollTasks(State& s)
    {
        if (s.Saving && s.SaveTask.wait_for(chrono::seconds(0)) == future_status::ready)
        {
            s.Saving = false;
            const string error = s.SaveTask.get();
            s.Status = error.empty() ? "保存しました" : "保存に失敗: " + error;
        }
        if (s.CloudRunning && s.CloudTask.wait_for(chrono::seconds(0)) == future_status::ready)
        {
            s.CloudRunning = false;
            const auto volume = s.CloudTask.get();
            CloudRenderer::SetCustomBaseNoise(volume.Density, volume.Extent, "プロシージャルスタジオ: フラクタル雲");
            if (!CloudRenderer::GetSettings().Enabled)
            {
                s.Status = "雲ノイズを適用しました (プロシージャルクラウドで Enable clouds を有効にしてください)";
            }
            else
            {
                s.Status = "雲ノイズをプロシージャルクラウドに適用しました";
            }
        }
    }

    // ------------------------------------------------------------------------------------------------
    // 画像ドロップ。直前の項目をアセットブラウザからのドロップ先にし、エクスプローラーからの
    // ドロップ（WM_DROPFILES）用に矩形を記録する。
    void ImageDropTarget(State& s)
    {
        const ImVec2 offset = ImGui::GetMainViewport()->Pos;
        const ImVec2 minimum = ImGui::GetItemRectMin();
        const ImVec2 maximum = ImGui::GetItemRectMax();
        s.Img.DropRects.push_back(
            ImVec4(minimum.x - offset.x, minimum.y - offset.y, maximum.x - offset.x, maximum.y - offset.y));
        s.Img.DropFrame = ImGui::GetFrameCount();
        if (!ImGui::BeginDragDropTarget())
        {
            return;
        }
        filesystem::path path;
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(AssetPayloadType))
        {
            if (payload->DataSize == sizeof(AssetDragPayload))
            {
                AssetDragPayload asset{};
                memcpy(&asset, payload->Data, sizeof(asset));
                if (asset.Type == AssetType::Texture)
                {
                    path = MaterialManager::Registry().Resolve(asset.Handle);
                }
                else
                {
                    s.Status = "テクスチャ画像をドロップしてください";
                }
            }
        }
        else if (const ImGuiPayload* pathPayload = ImGui::AcceptDragDropPayload("ASSET_PATH"))
        {
            const char* text = static_cast<const char*>(pathPayload->Data);
            if (text && text[0] != '\0')
            {
                path = filesystem::u8path(text);
            }
        }
        ImGui::EndDragDropTarget();
        if (!path.empty())
        {
            RequestImageLoad(s, path);
        }
    }

    constexpr const wchar_t* kImageOpenFilter =
        L"画像 (*.png;*.jpg;*.dds;*.tga;*.bmp;*.tif;*.hdr)\0*.png;*.jpg;*.jpeg;*.dds;*.tga;*.bmp;*.tif;*.tiff;*.hdr\0"
        L"すべてのファイル\0*.*\0";

    void DrawImageSourcePanel(State& s, bool busySaving)
    {
        auto& img = s.Img;
        bool changed = false;

        ImGui::SeparatorText("元画像");
        {
            string label;
            if (img.Loaded.Preview)
            {
                label = ProceduralIO::ToUtf8(img.Loaded.Path.filename()) + "\n" + to_string(img.Loaded.Width) +
                        " x " + to_string(img.Loaded.Height) + "   (クリック / ドロップで差し替え)";
            }
            else
            {
                label = "ここに画像をドロップ\n(エクスプローラー / アセットブラウザ、クリックで参照)";
            }
            label += "###ImageDropZone";
            if (ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 56)))
            {
                filesystem::path path;
                if (ProceduralIO::OpenFileDialog(kImageOpenFilter, path))
                {
                    RequestImageLoad(s, path);
                }
            }
            ImageDropTarget(s);
        }
        if (s.ImageLoadJob.Running)
        {
            ImGui::TextDisabled("読み込み中...");
        }

        ImGui::SeparatorText("高さマップ");
        auto& p = img.Params;
        if (ImGui::BeginCombo("高さの元", ProcStudio::HeightChannelName(p.Channel)))
        {
            for (int i = 0; i < static_cast<int>(ProcStudio::HeightChannel::Count); ++i)
            {
                const auto channel = static_cast<ProcStudio::HeightChannel>(i);
                if (ImGui::Selectable(ProcStudio::HeightChannelName(channel), p.Channel == channel))
                {
                    p.Channel = channel;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        changed |= ImGui::Checkbox("反転 (明るい所を低く)", &p.Invert);
        changed |= ImGui::SliderFloat("ぼかし", &p.BlurRadius, 0.0f, 32.0f, "%.1f px", ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("細かいノイズを抑えます (元画像のピクセル単位)。");
        }
        changed |= ImGui::SliderFloat("大きな明暗の除去", &p.HighPassRadius, 0.0f, 512.0f, p.HighPassRadius > 0 ? "%.0f px" : "なし",
                                      ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("写真の照明ムラなど、指定半径より大きな明暗を高さから取り除きます。0 で無効。");
        }
        changed |= ImGui::Checkbox("自動レベル補正 (0〜1 に正規化)", &p.AutoLevels);
        changed |= ImGui::SliderFloat("コントラスト##image", &p.Contrast, 0.0f, 8.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        changed |= ImGui::SliderFloat("バイアス##image", &p.Bias, -1.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        changed |= ImGui::SliderFloat("ガンマ##image", &p.Gamma, 0.1f, 5.0f, "%.2f",
                                      ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);

        ImGui::SeparatorText("法線マップ");
        changed |= ImGui::SliderFloat("法線強度##image", &p.NormalStrength, 0.1f, 128.0f, "%.2f",
                                      ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
        int filter = static_cast<int>(p.Filter);
        if (ImGui::Combo("フィルター", &filter, "中央差分 (鋭い)\0Sobel\0Scharr\0"))
        {
            p.Filter = static_cast<ProcStudio::NormalFilter>(filter);
            changed = true;
        }
        int convention = p.FlipY ? 1 : 0;
        if (ImGui::Combo("Y の向き", &convention, "OpenGL (+Y)\0DirectX (-Y)\0"))
        {
            p.FlipY = convention == 1;
            changed = true;
        }
        changed |= ImGui::Checkbox("端を折り返す (タイリング)", &p.Tileable);
        if (ImGui::Button("設定を初期値に戻す"))
        {
            p = ProcStudio::ImageMapParams{};
            changed = true;
        }
        if (changed)
        {
            s.ImageMapJob.Dirty = true;
        }

        ImGui::SeparatorText("プレビュー");
        bool previewChanged = false;
        int view = static_cast<int>(img.View);
        previewChanged |= ImGui::Combo("2D 表示", &view, "元画像\0高さ\0法線\0陰影 (ライティング)\0");
        img.View = static_cast<ImageView>(view);
        previewChanged |= ImGui::Checkbox("陰影・3D で元画像の色を使う", &img.SourceColor);
        previewChanged |= ImGui::SliderAngle("光の方位", &img.LightAzimuth, -180.0f, 180.0f);
        previewChanged |= ImGui::SliderAngle("光の高さ", &img.LightElevation, 5.0f, 90.0f);
        if (previewChanged)
        {
            s.PreviewDirty = true;
        }

        ImGui::SeparatorText("保存");
        ImGui::BeginDisabled(busySaving || !img.Loaded.Preview);
        if (ImGui::Button("高さマップを保存..."))
        {
            SaveImageMaps(s, SaveHeightMap);
        }
        ImGui::SameLine();
        if (ImGui::Button("法線マップを保存..."))
        {
            SaveImageMaps(s, SaveNormalMap);
        }
        if (ImGui::Button("両方を保存..."))
        {
            SaveImageMaps(s, SaveBothMaps);
        }
        ImGui::EndDisabled();
        if (img.Loaded.Preview)
        {
            ImGui::TextWrapped("元の解像度 (%d x %d) で保存します。高さは PNG / DDS / TIF なら 16 bit。"
                               "「両方を保存」は <名前>_height / <名前>_normal で保存します。",
                               img.Loaded.Width, img.Loaded.Height);
        }
    }

    // ------------------------------------------------------------------------------------------------
    void DrawPreviewPanel(State& s)
    {
        if (!s.Gpu.Ensure())
        {
            ImGui::TextWrapped("%s", s.Gpu.Error.c_str());
            return;
        }
        int view = static_cast<int>(s.View);
        ImGui::RadioButton("3D", &view, static_cast<int>(ViewMode::Model3D));
        if (s.Mode == Category::Texture)
        {
            ImGui::SameLine();
            ImGui::RadioButton("2D", &view, static_cast<int>(ViewMode::Flat2D));
            if (s.Texture.Kind == TextureKind::FractalCloud && !ImageActive(s))
            {
                ImGui::SameLine();
                ImGui::RadioButton("3D ノイズ断面", &view, static_cast<int>(ViewMode::VolumeSlice));
            }
        }
        else if (s.Mode == Category::Solid)
        {
            ImGui::SameLine();
            ImGui::RadioButton("ボリューム断面", &view, static_cast<int>(ViewMode::VolumeSlice));
        }
        if (s.Mode != Category::Texture && s.Model.Kind == ModelKind::Terrain)
        {
            ImGui::SameLine();
            if (ImGui::Checkbox("区画線", &s.ShowTerrainGrid))
            {
                s.PreviewDirty = true;
            }
        }
        if (view != static_cast<int>(s.View))
        {
            s.View = static_cast<ViewMode>(view);
            s.PreviewDirty = true;
        }
        if (s.View == ViewMode::Flat2D)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90);
            if (ImGui::SliderInt("タイル", &s.Tiles, 1, 4))
            {
                s.PreviewDirty = true;
            }
        }
        else if (s.View == ViewMode::VolumeSlice)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(140);
            if (ImGui::SliderFloat("断面", &s.Slice, 0, 1))
            {
                s.PreviewDirty = true;
            }
        }
        else if (s.Mode == Category::Texture)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120);
            int mesh = static_cast<int>(s.TexturePreviewMesh);
            if (ImGui::Combo("##mesh", &mesh, "球\0立方体\0平面\0"))
            {
                s.TexturePreviewMesh = static_cast<ModelKind>(mesh);
                s.PreviewDirty = true;
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100);
            if (ImGui::SliderFloat("UV", &s.UvScale, 0.25f, 8.0f, "%.2f"))
            {
                s.PreviewDirty = true;
            }
        }
        const auto available = ImGui::GetContentRegionAvail();
        const float side = (std::max)(64.0f, (std::min)(available.x, available.y - 24));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
        ImGui::ImageButton("##ProceduralPreview", (ImTextureID)s.Gpu.Lease->Gpu.ptr, ImVec2(side, side));
        ImGui::PopStyleVar();
        const bool previewActive = ImGui::IsItemActive();
        const bool previewHovered = ImGui::IsItemHovered();
        if (s.Mode == Category::Texture)
        {
            // プレビューへ画像をドロップすると、画像から生成するモードへ切り替える。
            ImageDropTarget(s);
        }
        if (s.View == ViewMode::Model3D)
        {
            if (previewActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            {
                const auto delta = ImGui::GetIO().MouseDelta;
                s.Orbit.Yaw -= delta.x * 0.01f;
                s.Orbit.Pitch = std::clamp(s.Orbit.Pitch + delta.y * 0.01f, -1.5f, 1.5f);
                s.PreviewDirty = true;
            }
            if (previewHovered && ImGui::GetIO().MouseWheel != 0)
            {
                s.Orbit.Distance = std::clamp(s.Orbit.Distance * powf(0.9f, ImGui::GetIO().MouseWheel), 1.05f, 10.0f);
                s.PreviewDirty = true;
            }
            const Mesh& mesh = PreviewMesh(s);
            ImGui::TextDisabled("ドラッグで回転 / ホイールでズーム   頂点 %zu  三角形 %zu", mesh.Positions.size(),
                                mesh.Indices.size() / 3);
        }
        if (ImageActive(s))
        {
            if (s.Img.Loaded.Preview)
            {
                ImGui::TextDisabled("画像: %s (%d x %d) / プレビュー処理 %d x %d",
                                    ProceduralIO::ToUtf8(s.Img.Loaded.Path.filename()).c_str(), s.Img.Loaded.Width,
                                    s.Img.Loaded.Height, s.Img.Loaded.Preview->Width, s.Img.Loaded.Preview->Height);
            }
            else
            {
                ImGui::TextDisabled("画像をプレビューか左のドロップ領域へドロップしてください");
            }
        }
        else if (NeedsTexture(s))
        {
            ImGui::TextDisabled(s.GpuTextureValid ? "テクスチャ生成: GPU" : "テクスチャ生成: CPU");
            if (!s.GpuAvailable && !s.ComputeError.empty() && ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("GPU を利用できないため CPU で生成します\n%s", s.ComputeError.c_str());
            }
        }
        const bool busy = s.ReadbackPending || s.TextureJob.Running || s.MeshJob.Running || s.SolidJob.Running ||
                          s.CloudSliceJob.Running || s.PreviewJob.Running || s.ImageLoadJob.Running ||
                          s.ImageMapJob.Running;
        if (busy)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(生成中...)");
        }
    }

    void DrawSaveRow(State& s, bool texture)
    {
        if (texture)
        {
            ImGui::SetNextItemWidth(140);
            ImGui::Combo("保存解像度", &s.SaveResolution, "256\000512\0001024\0002048\0004096\0");
        }
    }

    void DrawWindow(bool* open)
    {
        State& s = S();
        if (!s.Initialized)
        {
            InitializeDefaults(s);
        }
        try
        {
            PollTasks(s);
        }
        catch (const std::exception& error)
        {
            s.Status = "生成に失敗: " + string(error.what());
            s.Animate = false;
        }
        s.Img.DropRects.clear();
        ImGui::SetNextWindowSize(ImVec2(1040, 680), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("プロシージャルスタジオ", open))
        {
            ImGui::End();
            return;
        }
        // カテゴリータブ。
        const Category previous = s.Mode;
        if (ImGui::BeginTabBar("##ProceduralCategory"))
        {
            if (ImGui::BeginTabItem("テクスチャ"))
            {
                s.Mode = Category::Texture;
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("ソリッドテクスチャ"))
            {
                s.Mode = Category::Solid;
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("モデル"))
            {
                s.Mode = Category::Model;
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        if (s.Mode != previous)
        {
            if (s.Mode != Category::Texture && s.View == ViewMode::Flat2D)
            {
                s.View = ViewMode::Model3D;
            }
            if (s.Mode == Category::Model && s.View == ViewMode::VolumeSlice)
            {
                s.View = ViewMode::Model3D;
            }
            s.PreviewDirty = true;
        }

        if (ImGui::BeginTable("##ProceduralLayout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV))
        {
            ImGui::TableSetupColumn("設定", ImGuiTableColumnFlags_WidthFixed, 380.0f);
            ImGui::TableSetupColumn("プレビュー", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::BeginChild("##ProceduralSettings", ImVec2(0, 0), ImGuiChildFlags_None);
            const bool busySaving = s.Saving || s.CloudRunning;
            switch (s.Mode)
            {
                case Category::Texture: {
                    int source = static_cast<int>(s.Img.Source);
                    ImGui::RadioButton("プロシージャル", &source, static_cast<int>(TextureSource::Procedural));
                    ImGui::SameLine();
                    ImGui::RadioButton("画像から生成 (高さ / 法線)", &source, static_cast<int>(TextureSource::Image));
                    if (source != static_cast<int>(s.Img.Source))
                    {
                        s.Img.Source = static_cast<TextureSource>(source);
                        if (s.View == ViewMode::VolumeSlice)
                        {
                            s.View = ViewMode::Flat2D;
                        }
                        s.PreviewDirty = true;
                    }
                    if (s.Img.Source == TextureSource::Image)
                    {
                        DrawImageSourcePanel(s, busySaving);
                        break;
                    }
                    ImGui::Separator();
                    if (TextureWidgets(s.Texture, false))
                    {
                        s.TextureJob.Dirty = true;
                        s.CloudSliceJob.Dirty = true;
                    }
                    if (s.Texture.Kind == TextureKind::FractalWater)
                    {
                        ImGui::Checkbox("アニメーション", &s.Animate);
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(120);
                        ImGui::SliderFloat("速度", &s.AnimationSpeed, 0.1f, 5.0f);
                    }
                    if (s.Texture.Kind != TextureKind::FractalCloud && s.View == ViewMode::VolumeSlice)
                    {
                        s.View = ViewMode::Flat2D;
                        s.PreviewDirty = true;
                    }
                    ImGui::SeparatorText("ノイズ定義 (.noise)");
                    const bool supportsNoise = ProcStudio::TextureGpu::Supports(s.Texture);
                    ImGui::BeginDisabled(busySaving || !supportsNoise);
                    if (ImGui::Button(".noise を保存..."))
                    {
                        SaveNoise(s);
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(busySaving);
                    if (ImGui::Button(".noise を読み込む..."))
                    {
                        LoadNoise(s);
                    }
                    ImGui::EndDisabled();
                    ImGui::TextWrapped(
                        "asset フォルダに保存し、マテリアルへドロップ。画像解像度に依存せず描画時に計算します。");
                    if (!supportsNoise)
                    {
                        ImGui::TextWrapped("Mandelbrot / Julia は CPU 計算のため、画像として保存してください。");
                    }
                    ImGui::SeparatorText("画像を保存");
                    DrawSaveRow(s, true);
                    ImGui::BeginDisabled(busySaving);
                    if (ImGui::Button("テクスチャを保存..."))
                    {
                        SaveTexture(s);
                    }
                    ImGui::EndDisabled();
                    if (s.Texture.Kind == TextureKind::FractalCloud)
                    {
                        ImGui::SeparatorText("体積雲");
                        ImGui::SetNextItemWidth(140);
                        ImGui::Combo("ボリューム解像度", &s.VolumeExtent, "64^3\000128^3\0");
                        ImGui::BeginDisabled(busySaving);
                        if (ImGui::Button("プロシージャルクラウドに適用"))
                        {
                            ApplyCloud(s);
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("3D ノイズを保存 (.dds)..."))
                        {
                            SaveCloudVolume(s);
                        }
                        ImGui::EndDisabled();
                        ImGui::TextWrapped(
                            "同じ設定で周期的な 3D ノイズを生成し、雲の形状ノイズとして使用します。保存した .dds "
                            "はプロシージャルクラウドの「テクスチャを指定して生成」から読み込めます。");
                    }
                    break;
                }
                case Category::Solid: {
                    ImGui::TextWrapped(
                        "3D 空間で定義されたテクスチャです。UV を使わずにモデル内部まで連続した模様になります。");
                    if (TextureWidgets(s.Solid, true))
                    {
                        s.SolidJob.Dirty = true;
                    }
                    if (ImGui::SliderFloat("繰り返し", &s.SolidScale, 0.25f, 8.0f))
                    {
                        s.PreviewDirty = true;
                    }
                    if (ImGui::CollapsingHeader("プレビューモデル", ImGuiTreeNodeFlags_DefaultOpen))
                    {
                        if (ModelWidgets(s.Model))
                        {
                            s.MeshJob.Dirty = true;
                        }
                    }
                    ImGui::SeparatorText("保存");
                    ImGui::SetNextItemWidth(140);
                    ImGui::Combo("ボリューム解像度", &s.VolumeExtent, "64^3\000128^3\0");
                    ImGui::BeginDisabled(busySaving);
                    if (ImGui::Button("3D テクスチャを保存 (.dds)..."))
                    {
                        SaveSolidVolume(s);
                    }
                    ImGui::SetNextItemWidth(140);
                    ImGui::Combo("モデル形式", &s.ModelExportFormat, "OBJ\0FBX (ASCII)\0");
                    if (ImGui::Button("焼き込んだモデルを保存..."))
                    {
                        SaveModel(s);
                    }
                    ImGui::EndDisabled();
                    break;
                }
                case Category::Model: {
                    if (ModelWidgets(s.Model))
                    {
                        s.MeshJob.Dirty = true;
                    }
                    if (s.Model.Kind == ModelKind::WaterSurface)
                    {
                        ImGui::Checkbox("アニメーション", &s.Animate);
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(120);
                        ImGui::SliderFloat("速度", &s.AnimationSpeed, 0.1f, 5.0f);
                    }
                    ImGui::SeparatorText("表面");
                    int surface = static_cast<int>(s.ModelSurface);
                    ImGui::RadioButton("頂点カラー", &surface, 0);
                    ImGui::SameLine();
                    ImGui::RadioButton("テクスチャ", &surface, 1);
                    ImGui::SameLine();
                    ImGui::RadioButton("ソリッド", &surface, 2);
                    if (surface != static_cast<int>(s.ModelSurface))
                    {
                        s.ModelSurface = static_cast<Surface>(surface);
                        s.PreviewDirty = true;
                    }
                    if (s.ModelSurface == Surface::Texture)
                    {
                        ImGui::TextDisabled("「テクスチャ」タブの設定を使用します。");
                        if (ImGui::SliderFloat("UV の繰り返し", &s.UvScale, 0.25f, 16.0f))
                        {
                            s.PreviewDirty = true;
                        }
                    }
                    else if (s.ModelSurface == Surface::Solid)
                    {
                        ImGui::TextDisabled("「ソリッドテクスチャ」タブの設定を使用します。");
                        if (ImGui::SliderFloat("繰り返し", &s.SolidScale, 0.25f, 8.0f))
                        {
                            s.PreviewDirty = true;
                        }
                    }
                    ImGui::SeparatorText("保存");
                    if (s.ModelSurface == Surface::Texture)
                    {
                        DrawSaveRow(s, true);
                    }
                    ImGui::BeginDisabled(busySaving);
                    ImGui::SetNextItemWidth(140);
                    ImGui::Combo("モデル形式", &s.ModelExportFormat, "OBJ\0FBX (ASCII)\0");
                    if (ImGui::Button("モデルを保存..."))
                    {
                        SaveModel(s);
                    }
                    ImGui::EndDisabled();
                    ImGui::TextWrapped(
                        s.ModelSurface == Surface::Texture
                            ? "モデルと <名前>_albedo.png を同じフォルダに保存します。OBJ には MTL も保存します。"
                            : "頂点カラー付きのモデルを保存します。OBJ には MTL も保存します。");
                    break;
                }
            }
            if (!s.Status.empty())
            {
                ImGui::Separator();
                ImGui::TextWrapped("%s", s.Status.c_str());
            }
            ImGui::EndChild();
            ImGui::TableSetColumnIndex(1);
            DrawPreviewPanel(s);
            ImGui::EndTable();
        }

        // 水のアニメーションでは時刻を進め、頻度制限付きのジョブで再生成する。
        if (s.Animate)
        {
            const float delta = ImGui::GetIO().DeltaTime * s.AnimationSpeed;
            if (s.Mode == Category::Texture && s.Texture.Kind == TextureKind::FractalWater)
            {
                s.Texture.Time = fmodf(s.Texture.Time + delta, 100.0f);
                s.TextureJob.Dirty = true;
            }
            else if (s.Mode == Category::Model && s.Model.Kind == ModelKind::WaterSurface)
            {
                s.Model.Time = fmodf(s.Model.Time + delta, 100.0f);
                s.MeshJob.Dirty = true;
            }
        }
        try
        {
            PumpJobs(s);
            RenderPreview(s);
        }
        catch (const std::exception& error)
        {
            s.Status = "生成に失敗: " + string(error.what());
            s.PreviewDirty = false;
            s.Animate = false;
        }
        ImGui::End();
    }

    bool AcceptDroppedFileImpl(const char* path, float x, float y)
    {
        State& s = S();
        // 直前（またはその前）のフレームで描画したドロップ領域だけを有効にする。
        if (!path || s.Img.DropRects.empty() || ImGui::GetFrameCount() - s.Img.DropFrame > 2)
        {
            return false;
        }
        bool inside = false;
        for (const auto& rect : s.Img.DropRects)
        {
            inside |= x >= rect.x && y >= rect.y && x < rect.z && y < rect.w;
        }
        if (!inside)
        {
            return false;
        }
        // DragQueryFileA の文字列は ANSI コードページ。filesystem::path(string) も同じ変換を使う。
        RequestImageLoad(s, filesystem::path(string(path)));
        return true;
    }

    void ShutdownWindow()
    {
        State& s = S();
        // ワーカーがエディターより長く生存しないよう、完了を待つ。
        if (s.TextureJob.Running)
        {
            s.TextureJob.Result.wait();
        }
        if (s.ImageLoadJob.Running)
        {
            s.ImageLoadJob.Result.wait();
        }
        if (s.ImageMapJob.Running)
        {
            s.ImageMapJob.Result.wait();
        }
        if (s.MeshJob.Running)
        {
            s.MeshJob.Result.wait();
        }
        if (s.PreviewJob.Running)
        {
            s.PreviewJob.Result.wait();
        }
        if (s.SolidJob.Running)
        {
            s.SolidJob.Result.wait();
        }
        if (s.CloudSliceJob.Running)
        {
            s.CloudSliceJob.Result.wait();
        }
        if (s.Saving)
        {
            s.SaveTask.wait();
        }
        if (s.CloudRunning)
        {
            s.CloudTask.wait();
        }
        GraphicsDevice::WaitForGpuIdle();
        s.TextureCompute.Release();
        s.GpuAttempted = false;
        s.GpuAvailable = false;
        s.GpuTextureValid = false;
        s.ReadbackPending = false;
        s.TextureJob.Dirty = true;
        s.Gpu.Release();
    }
} // 名前空間 ProcStudioEditorDetail の終わり。

void ProceduralStudioWindow::Draw(bool* open)
{
    ProcStudioEditorDetail::DrawWindow(open);
}

bool ProceduralStudioWindow::AcceptDroppedFile(const char* path, float x, float y)
{
    return ProcStudioEditorDetail::AcceptDroppedFileImpl(path, x, y);
}

void ProceduralStudioWindow::Shutdown()
{
    ProcStudioEditorDetail::ShutdownWindow();
}

