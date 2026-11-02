#pragma once
// ============================================================================
// ISequencerBackend : Sequencer UI（sequencerwindow.cpp）がエンジンへアクセスする唯一の窓口。
//
//  ・UI は TimelineComponent のデータ編集と ImGui 描画だけを行い、ECS / 再生 / Undo / 描画テクスチャは
//    すべてこのインターフェース経由で扱う（UI 表示処理と Timeline 評価処理の分離）。
//  ・エンジン実装は sequencerbridge.cpp（SequencerEditorBridge）。
//  ・tests/sequencer では偽実装を使い、実際の ImGui でヘッドレスに UI 操作を検証する。
// ============================================================================
#include "timelinecomponent.h"
#include <cstdint>
#include <string>
#include <vector>

enum class SequencerEntityKind : uint8_t
{
    Object,
    Character, // AnimationModelComponent
    Camera,    // CameraComponent
    Light,     // LightComponent
};

class ISequencerBackend
{
  public:
    virtual ~ISequencerBackend() = default;

    // ---- シーン参照
    virtual void CollectDirectors(std::vector<EntityID>& out) = 0;             // TimelineComponent を持つ Entity
    virtual void CollectTrackableEntities(std::vector<EntityID>& out) = 0;     // トラック追加の候補
    virtual void CollectCameras(std::vector<EntityID>& out) = 0;               // カメラカットの候補
    virtual TimelineComponent* GetTimeline(EntityID director) = 0;
    virtual bool IsAlive(EntityID entity) = 0;
    virtual const char* GetEntityName(EntityID entity) = 0;
    virtual SequencerEntityKind GetEntityKind(EntityID entity) = 0;
    virtual EntityID GetEditorSelection() = 0;
    virtual void SetEditorSelection(EntityID entity) = 0;
    virtual bool ReadProperty(EntityID target, TimelineProperty property, int fogVolume,
                              DirectX::XMFLOAT4& value) = 0;
    virtual bool IsSpotLight(EntityID entity) = 0;
    virtual bool IsPhysicallyBasedLight(EntityID entity) = 0;

    // ---- 再生・評価（既存の TimeLineSystem をそのまま使う）
    virtual bool IsSimulationRunning() = 0; // TimeLineSystem::Update が毎フレーム評価している
    virtual bool IsProjectPlaying() = 0;    // Play / 一時停止中
    virtual float GetDeltaTime() = 0;
    virtual float GetProjectFrameRate() = 0;
    virtual void Evaluate(EntityID director, TimelineComponent& timeline, bool cameraCut) = 0;
    virtual void Advance(EntityID director, TimelineComponent& timeline, float deltaTime) = 0;

    // ---- Undo / Redo（既存のエディター履歴に積む）
    // ドラッグ等の操作：変更する直前に BeginEdit、操作終了で CommitEdit。
    virtual void BeginEdit(EntityID director) = 0;  // 編集前の状態を記録（既に記録中なら何もしない）
    virtual void CommitEdit(bool changed) = 0;      // changed なら履歴に 1 段積む
    // インスペクターのウィジェット：描画前に CaptureFrameState、値が変わったら BeginEditFromFrameState。
    virtual void CaptureFrameState(EntityID director) = 0;
    virtual void BeginEditFromFrameState() = 0;
    virtual uint64_t GetHistoryRevision() = 0; // Undo / Redo / 他ウィンドウの編集で変わる

    // ---- 既存エディター UI の再利用
    virtual bool DrawAnimationLayers(EntityID director, size_t trackIndex, TimelineTrackData& track) = 0;
    virtual bool AppendAnimation(TimelineTrackData& track, const std::string& path) = 0;
    virtual bool DrawMonitorFrames(EntityID director, size_t monitorIndex, LiveMonitorBinding& monitor) = 0;
    // 既存のシーン描画結果（追加の GPU パスなし）を現在のカーソル位置に描く。
    virtual float GetPreviewAspect() = 0;
    virtual bool DrawPreviewImage(float width, float height) = 0;
    virtual const char* GetPreviewLabel() = 0;
    virtual std::string MakeAssetRelativePath(const std::string& path) = 0;
    virtual void Log(const char* message) = 0;
};
