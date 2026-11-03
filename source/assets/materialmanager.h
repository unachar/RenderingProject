#pragma once
#include "assetregistry.h"
#include "noiseasset.h"
#include "componentmanager.h"
#include <functional>
#include <array>

class MaterialGraph;
enum class MaterialSamplerType : int;

struct MaterialRuntime
{
    AssetHandle Handle = 0;
    MaterialAssetData Saved;
    MaterialAssetData Editing;
    MaterialAssetData BoundData;
    array<filesystem::path, static_cast<size_t>(MaterialTextureSlot::Count)> TexturePaths;
    array<int, static_cast<size_t>(MaterialTextureSlot::Count)> TextureDescriptors{-1, -1, -1, -1, -1, -1};
    MaterialNoiseSources NoiseSources;
    MaterialComponent Binding;
    uint64_t Revision = 0;
    uint64_t PipelineRevision = 0;
    bool Dirty = false;
    bool Missing = false;
    string Error;
    string SourceError;
    // グラフィックスのフェンス完了後、終了処理まで生存させる。
    ComPtr<ID3D12Resource> Constants;
    uint8_t* MappedConstants = nullptr;
    array<uint64_t, 3> UploadedRevision{};
    // ---- マテリアルグラフの描画時評価（MaterialGraphVM.hlsli）
    int GraphProgramSrv = -1;           // プログラムのデータテクスチャ（テクスチャ表の SRV）
    uint64_t GraphProgramHash = 0;      // 書き込み済みのプログラム
    std::array<float, 4> GraphHeader{}; // [0] テクセル（毎フレーム時刻を書き換える）
    bool GraphUsesTime = false;
    bool GraphUsesView = false;  // カメラ距離など、視点で値が変わる入力を使う
    bool GraphEmissive = false;  // Material Output の Emissive が繋がっている
    std::vector<std::array<float, 4>> GraphTexels; // プログラム（発光の動的ベイク用に CPU 側にも保持）
    bool GraphActive = false;    // プログラムあり（コンパイル成功）
    bool GraphPublished = false; // Binding.GraphProgramID に反映済み
    bool GraphChecked = false;   // .graph.json を読んだ / エディターから受け取った
    int GraphInstructions = 0;
    int GraphRegisters = 0;
    string GraphStatus; // 空 = 正常。描画時評価できない理由
};

class MaterialManager
{
  public:
    static AssetRegistry& Registry();
    static void Init();
    static void PrepareStartupAssets();
    static void Shutdown();
    static void Refresh();
    static MaterialRuntime& GetRuntime(AssetHandle handle);
    static const MaterialRuntime* FindRuntime(AssetHandle handle);
    static MaterialComponent BuildPreviewBinding(
        const MaterialAssetData& material,
        const array<int, static_cast<size_t>(MaterialTextureSlot::Count)>& descriptors);
    static const MaterialComponent& Resolve(const MaterialComponent& legacy, size_t slot = 0);
    static AssetHandle SlotHandle(const MaterialComponent& material, size_t slot);
    static bool SetEditing(AssetHandle handle, const MaterialAssetData& editing);
    static bool Save(AssetHandle handle);
    static bool Revert(AssetHandle handle);
    static void Discard(AssetHandle handle);
    static bool HasSurfaceForPass(EntityID entity, bool transparent);
    static size_t SlotCount(EntityID entity);
    static bool IsRenderable(EntityID entity);
    static bool Assign(EntityID entity, size_t slot, AssetHandle handle);
    static void NotifyAssignmentsChanged();
    static bool SaveSceneReferences(const filesystem::path& path, string& error);
    static bool LoadSceneReferences(const filesystem::path& path, string& error);
    // ---- マテリアルグラフの描画時評価（GPU）
    // グラフをプログラムにしてエンジンのシェーダーへ渡す（nullptr / 設定オフ / 未対応ノードで無効 = ベイクの値）
    static void SetGraphProgram(AssetHandle handle, const MaterialGraph* graph);
    // 毎フレーム（描画の記録前）：保存済みグラフの読み込み、プログラムの反映、時刻の更新
    static void UpdateGraphPrograms();
    static bool IsGraphProgramActive(AssetHandle handle);
    // グラフの時刻（Time ノード。描画時評価と同じ時計）
    static float GraphTime();
    // 発光が視点 / 時刻で変わるグラフ（GI・反射用の発光テクスチャを毎フレーム焼き直す対象）
    struct DynamicEmissionSource
    {
        const std::vector<std::array<float, 4>>* Program = nullptr;
        uint64_t Hash = 0;
        bool UsesTime = false;
        bool UsesView = false;
    };
    static bool GetDynamicEmissionSource(AssetHandle handle, DynamicEmissionSource& source);
    // グラフが参照するテクスチャを読み込み、マテリアルのテクスチャ表の番号（0〜511、失敗は -1）を返す
    static int ResolveGraphTexture(AssetHandle texture, MaterialSamplerType sampler);

  private:
    static void UpdateBinding(MaterialRuntime& runtime, const MaterialAssetData& material);
};

