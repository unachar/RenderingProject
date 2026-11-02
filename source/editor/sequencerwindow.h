#pragma once
// ============================================================================
// SequencerWindow : ライブ演出用タイムライン（Dear ImGui + ImDrawList）。
//
//  データ  : TimelineComponent（既存形式）をそのまま編集する。再生・補間は TimeLineSystem / TimelineCurve。
//  UI 状態 : 行ツリー・選択・ズーム等は本クラスだけが持ち、シーンには保存しない
//            （表示設定は ImGui の .cfg に "[RPSequencer][Main]" として保存）。
//  エンジン: ISequencerBackend 経由（sequencerbridge.cpp）。
// ============================================================================
#include "sequencerbackend.h"
#include "timelinecurve.h"
#include <imgui.h>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class SequencerWindow
{
  public:
    enum class Mode : int
    {
        DopeSheet = 0,
        Curve,
        Clip,
    };

    enum class TimeFormat : int
    {
        Seconds = 0,
        Frames,
        Timecode, // 分:秒:フレーム
        Beats,    // 小節.拍.(1/100 拍)
    };

    enum class SnapMode : int
    {
        Off = 0,
        Frame,
        Beat,
        Bar,
        Tenth, // 0.1 秒
    };

    enum class RowKind : uint8_t
    {
        Object,
        Group,     // Transform / Animation
        Property,  // TimelineTrackData 1 本
        Channel,   // X / Y / Z（Property と同じキーを表示）
        AnimLayer, // VMD レイヤー（AnimationTime トラックの時計を共有）
        CameraCut, // CameraCut トラック
        Video,     // LiveMonitorBinding
        VideoClip,
        Audio, // TimelineComponent::MusicPath
        AudioClip,
    };

    enum class Category : uint8_t
    {
        Generic,
        Position,
        Rotation,
        Scale,
        Light,
        CameraFov,
        Camera,
        Animation,
        Video,
        Audio,
        CameraCut,
        Character,
        Transform,
    };

    struct Row
    {
        RowKind Kind = RowKind::Object;
        Category Cat = Category::Generic;
        uint8_t Depth = 0;
        bool HasChildren = false;
        bool Open = true;
        bool Visible = true;
        bool Match = true;
        int Parent = -1;
        int Track = -1;
        int Channel = -1;
        int Monitor = -1;
        int Layer = -1;
        EntityID Entity = g_kINVALID_ENTITY;
        ImGuiID Id = 0;
        float Height = 22.0f;
        float Top = 0.0f;     // 行領域内のローカル Y（スクロール前）
        float ScreenY = 0.0f; // 直近の描画位置（テスト・ヒットテスト用）
        char Label[96] = {};
    };

    struct ClipRef
    {
        enum class Type : uint8_t
        {
            Track,
            Video,
            Audio,
        };
        Type Kind = Type::Track;
        int Track = -1; // Video のときは Monitor index
        int Clip = -1;

        bool operator==(const ClipRef& other) const
        {
            return Kind == other.Kind && Track == other.Track && Clip == other.Clip;
        }
    };

    struct ViewSettings
    {
        float PixelsPerSecond = 80.0f;
        float ViewStart = 0.0f;
        float TreeWidth = 280.0f;
        float InspectorWidth = 310.0f;
        float RowHeight = 22.0f;
        float PreviewHeight = 150.0f;
        int ModeValue = 0;
        int TimeFormatValue = 0;
        int SnapValue = 1;
        bool ShowPreview = true;
        bool ShowInspector = true;
        bool Magnet = true;
        bool InlineCurves = true;
        bool RangeEnabled = false;
        float RangeIn = 0.0f;
        float RangeOut = 5.0f;
        bool CurveChannels[4] = {true, true, true, true};
    };

    using KeyRef = TimelineCurve::KeyRef;

    explicit SequencerWindow(ISequencerBackend& backend);
    ~SequencerWindow();

    void Draw(bool* open);
    static void RegisterSettingsHandler();

    // ---- 状態の参照（テスト・他ウィンドウ用）
    EntityID GetDirector() const
    {
        return m_Director;
    }

    void SetDirector(EntityID director);

    ViewSettings& Settings()
    {
        return m_Settings;
    }

    const std::vector<Row>& GetRows() const
    {
        return m_Rows;
    }

    const std::vector<KeyRef>& GetSelectedKeys() const
    {
        return m_SelectedKeys;
    }

    const std::vector<ClipRef>& GetSelectedClips() const
    {
        return m_SelectedClips;
    }

    float TimeToX(float time) const;
    float XToTime(float x) const;

    ImVec2 GetTimelineMin() const
    {
        return m_TimelineMin;
    }

    ImVec2 GetTimelineMax() const
    {
        return m_TimelineMax;
    }

    ImVec2 GetRulerMin() const
    {
        return m_RulerMin;
    }

    ImVec2 GetRulerMax() const
    {
        return m_RulerMax;
    }

    float GetTreeLeft() const
    {
        return m_TreeMin.x;
    }

    void SetRowOpen(ImGuiID id, bool open);
    int FindRow(RowKind kind, int track, int channel = -1) const;
    void FormatTime(float time, char* buffer, size_t size) const;
    float SnapTime(float time, bool allowMagnet) const;
    float GetFrameRate() const;

  private:
    enum class Hit : uint8_t
    {
        None,
        Key,
        SummaryKey,
        Clip,
        ClipLeft,
        ClipRight,
        GroupClip,
        RowEmpty,
        RowResize,
        TreeRow,
        TreeArrow,
        TreeKeyButton,
        Playhead,
        CurveKey,
        CurveHandle,
        CurveEmpty,
    };

    struct HitInfo
    {
        Hit Type = Hit::None;
        int Row = -1;
        KeyRef Key;
        ClipRef Clip;
        float Time = 0.0f;
        int Channel = -1;
        int Handle = -1; // Bezier: 0 = 出、1 = 入
        float Distance = 1.0e9f;
    };

    enum class Drag : uint8_t
    {
        None,
        Pending, // 左クリック後、閾値を超えるまで
        Scrub,
        Keys,
        Marquee,
        Clips,
        ClipLeft,
        ClipRight,
        RangeIn,
        RangeOut,
        DurationEnd,
        Pan,
        TreeSplit,
        InspectorSplit,
        PreviewSplit,
        RowResize,
        CurveKeys,
        CurveHandle,
        CurvePan,
        Overview,
        HScroll,
    };

    struct ClipOrigin
    {
        ClipRef Ref;
        float Start = 0.0f;
        float Duration = 0.0f;
        float End = 0.0f;    // Video の EndTime
        float Offset = 0.0f; // Video の Offset / Audio の MusicOffset
    };

    struct ClipboardKey
    {
        int Track = -1;
        TimelineProperty Property = TimelineProperty::TransformPosition;
        float RelativeTime = 0.0f;
        TimelineKeyframe Key;
    };

    struct CurveSeries
    {
        int Track = -1;
        int Channel = 0;
        float Scale = 1.0f; // 表示単位（ラジアン → 度）
        ImU32 Color = 0;
        size_t SampleOffset = 0;
    };

    // ---- フレーム処理
    TimelineComponent* ResolveTimeline();
    void SyncExternalChanges(TimelineComponent& timeline);
    void TickPlayback(TimelineComponent& timeline);
    void HandleShortcuts(TimelineComponent& timeline);
    void BuildRows(TimelineComponent& timeline);
    void LayoutRows();
    void ValidateSelection(const TimelineComponent& timeline);

    // ---- 描画
    void DrawToolbar(TimelineComponent& timeline);
    void DrawPreview(TimelineComponent& timeline, ImVec2 min, ImVec2 max);
    void DrawOverview(TimelineComponent& timeline, ImVec2 min, ImVec2 max);
    void DrawRuler(TimelineComponent& timeline, ImDrawList* drawList, ImVec2 min, ImVec2 max);
    void DrawRows(TimelineComponent& timeline, ImVec2 size);
    void DrawTreeRow(TimelineComponent& timeline, ImDrawList* drawList, int index, float y);
    void DrawTimelineRow(TimelineComponent& timeline, ImDrawList* drawList, int index, float y);
    void DrawKeysForTrack(TimelineComponent& timeline, ImDrawList* drawList, const Row& row, int rowIndex, float y0,
                          float y1, bool compact);
    void DrawSummaryKeys(TimelineComponent& timeline, ImDrawList* drawList, int rowIndex, float y0, float y1);
    void DrawInlineCurve(TimelineComponent& timeline, ImDrawList* drawList, const Row& row, float y0, float y1);
    void DrawTrackClips(TimelineComponent& timeline, ImDrawList* drawList, const Row& row, int rowIndex, float y0,
                        float y1, bool interactive);
    void DrawGroupClip(TimelineComponent& timeline, ImDrawList* drawList, int rowIndex, float y0, float y1);
    void DrawVideoClip(TimelineComponent& timeline, ImDrawList* drawList, const Row& row, int rowIndex, float y0,
                       float y1);
    void DrawAudioClip(TimelineComponent& timeline, ImDrawList* drawList, const Row& row, int rowIndex, float y0,
                       float y1);
    void DrawClipBar(ImDrawList* drawList, ImVec2 min, ImVec2 max, ImU32 color, const char* label, float start,
                     float end, bool selected, bool hovered, int hoveredEdge, bool enabled, bool resizable);
    void DrawCurveEditor(TimelineComponent& timeline, ImDrawList* drawList, ImVec2 min, ImVec2 max);
    void DrawOverlays(TimelineComponent& timeline, ImDrawList* drawList, ImVec2 min, ImVec2 max);
    void DrawHorizontalScrollbar(TimelineComponent& timeline, ImVec2 min, ImVec2 max);
    void DrawInspector(TimelineComponent& timeline);
    void DrawInspectorKeys(TimelineComponent& timeline);
    void DrawInspectorClip(TimelineComponent& timeline);
    void DrawInspectorTrack(TimelineComponent& timeline, int track);
    void DrawInspectorTimeline(TimelineComponent& timeline);
    void DrawBottomBar(TimelineComponent& timeline);
    void DrawContextMenus(TimelineComponent& timeline);
    void DrawAddTrackMenu(TimelineComponent& timeline, EntityID entity);

    // ---- 入力
    void UpdateHover(const HitInfo& candidate);
    void HandleRowsInput(TimelineComponent& timeline);
    void BeginPendingDrag(TimelineComponent& timeline);
    void StartDrag(TimelineComponent& timeline);
    void UpdateDrag(TimelineComponent& timeline);
    void EndDrag(TimelineComponent& timeline);
    void HandleClick(TimelineComponent& timeline);
    void HandleDoubleClick(TimelineComponent& timeline);
    void HandleWheel(TimelineComponent& timeline, bool overTimeline, bool overTree);
    void SelectKeysInRect(TimelineComponent& timeline, float t0, float t1, int row0, int row1, bool additive);

    // ---- 編集操作
    void BeginEdit();
    void CommitEdit();
    void MarkChanged(TimelineComponent& timeline);
    void WidgetEdited(TimelineComponent& timeline, bool changed);
    void SetTime(TimelineComponent& timeline, float time, bool cut);
    void SetPlaying(TimelineComponent& timeline, bool playing);
    void StepToKey(TimelineComponent& timeline, int direction);
    void AddKeyAtRow(TimelineComponent& timeline, int rowIndex, float time, bool useSceneValue);
    void DeleteSelection(TimelineComponent& timeline);
    void CopySelection(TimelineComponent& timeline);
    void Paste(TimelineComponent& timeline);
    void SetInterpolation(TimelineComponent& timeline, TimelineInterpolation interpolation);
    void SplitClipAtPlayhead(TimelineComponent& timeline, const ClipRef& ref);
    int AddTrack(TimelineComponent& timeline, EntityID entity, TimelineProperty property);
    void AddClipAtPlayhead(TimelineComponent& timeline, int track);
    void FrameAll(TimelineComponent& timeline);
    void SnapshotTracks(const TimelineComponent& timeline);
    void RestoreTracks(TimelineComponent& timeline);
    void BuildSnapTargets(const TimelineComponent& timeline, bool excludeSelection);
    void CollectRowTracks(int rowIndex, std::vector<int>& tracks) const;
    void CollectKeysAt(const TimelineComponent& timeline, int rowIndex, float time, std::vector<KeyRef>& out) const;
    bool IsKeySelected(const KeyRef& ref) const;
    void SelectKey(const KeyRef& ref, bool toggle);
    bool IsClipSelected(const ClipRef& ref) const;
    bool RowShowsClips(const TimelineComponent& timeline, const Row& row) const;
    void BuildCurveSeries(TimelineComponent& timeline);

    // ---- 表示
    void ComputeTickSteps(float& major, float& minor, float& origin) const;
    ImU32 CategoryColor(Category category, float alpha = 1.0f) const;
    float ValueToY(float value) const;
    float YToValue(float y) const;
    bool IconButton(const char* id, int icon, const char* tooltip, bool active = false);

    ISequencerBackend& m_Backend;
    EntityID m_Director = g_kINVALID_ENTITY;
    bool m_DirectorPinned = false;
    ViewSettings m_Settings;
    ViewSettings m_SavedSettings;

    std::vector<Row> m_Rows;
    std::vector<EntityID> m_Objects;
    std::vector<int> m_VisibleRows;
    std::unordered_set<ImGuiID> m_Collapsed;
    std::unordered_map<ImGuiID, float> m_RowHeights;
    std::unordered_set<ImGuiID> m_SelectedRows;
    ImGuiID m_ActiveRow = 0;
    float m_ContentHeight = 0.0f;
    ImGuiTextFilter m_Filter;

    std::vector<KeyRef> m_SelectedKeys; // ソート済み
    std::vector<ClipRef> m_SelectedClips;
    KeyRef m_AnchorKey;
    int m_AnchorRow = -1;
    float m_AnchorTime = 0.0f;
    std::vector<ClipboardKey> m_Clipboard;

    HitInfo m_Hover;
    HitInfo m_Press;
    HitInfo m_ContextHit;
    Drag m_Drag = Drag::None;
    ImGuiMouseButton m_DragButton = 0;
    ImVec2 m_PressMouse{};
    float m_PressTime = 0.0f;
    float m_PressValue = 0.0f;
    float m_DragDelta = 0.0f;
    float m_DragValueDelta = 0.0f;
    float m_DragStartScalar = 0.0f;
    float m_DragStartView = 0.0f;
    std::vector<TimelineCurve::KeyOrigin> m_KeyOrigins;
    std::vector<ClipOrigin> m_ClipOrigins;
    std::vector<std::pair<int, std::vector<TimelineClipData>>> m_TrackSnapshot;
    std::vector<float> m_SnapTargets;
    mutable float m_SnapLine = -1.0f;
    ImVec2 m_MarqueeMin{}, m_MarqueeMax{};
    ImGuiID m_ResizeRow = 0;
    bool m_PlayheadGrab = false;

    bool m_EditOpen = false;
    bool m_EditDirty = false;
    bool m_WidgetEdit = false;
    uint64_t m_LastChecksum = 0;
    uint64_t m_LastHistoryRevision = 0;
    bool m_WasPlaying = false;
    float m_LastEvaluatedTime = -1.0f;

    // 曲線エディター
    std::vector<CurveSeries> m_CurveSeries;
    std::vector<float> m_CurveSamples;
    uint64_t m_CurveCacheKey = 0;
    int m_CurveSampleCount = 0;
    float m_CurveMin = -1.0f;
    float m_CurveMax = 1.0f;
    bool m_CurveAutoFit = true;
    ImVec2 m_CurveMin2{}, m_CurveMax2{};

    // スクラッチ（毎フレームの確保を避ける）
    std::vector<float> m_ScratchTimes;
    std::vector<int> m_ScratchTracks;
    std::vector<ImVec2> m_ScratchPoints;
    std::vector<KeyRef> m_ScratchRefs;
    std::vector<uint8_t> m_SummaryBins;
    std::vector<float> m_SummaryTimes;

    // レイアウト
    ImVec2 m_TreeMin{}, m_TreeMax{};
    ImVec2 m_TimelineMin{}, m_TimelineMax{};
    ImVec2 m_RulerMin{}, m_RulerMax{};
    ImVec2 m_RowsClipMin{}, m_RowsClipMax{};
    float m_ScrollY = 0.0f;
    float m_PendingScrollY = -1.0f;
    bool m_WindowFocused = false;
    bool m_RowsHovered = false;
    bool m_OpenTreeMenu = false;
    bool m_OpenTimelineMenu = false;
    bool m_OpenAddTrackPopup = false;
    char m_StatusText[160] = {};
};
