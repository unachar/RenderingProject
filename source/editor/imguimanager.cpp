#include "pch.h"
#include "envflags.h"
#include "thumbnailvalidation.h"
#include "materialmanager.h"
#include "materialpreview.h"
#include "materialgrapheditor.h"
#include "materialvalidation.h"
#include "cloud/cloudrenderer.h"
#include "renderingvalidation.h"
#include "imguimanager.h"
#include "proceduralstudiowindow.h"
#include "animationpreviewwindow.h"
#include "sequencerbridge.h"
#include "modelmanager.h"
#include "world.h"
#include "camera.h"
#include "graphicsdevice.h"
#include "texturemanager.h"
#include "materialsystem.h"
#include "light.h"
#include "sun.h"
#include "atmosphere.h"
#include "renderersettings.h"
#include "localheightfog.h"
#include "debugsystem.h"
#include "meshshaderpipeline.h"
#include "animator.h"
#include "projectmanager.h"
#include "physicssystem.h"
#include "systemmanager.h"
#include "timelinesystem.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <DirectXCollision.h>
#include <ImGuizmo.h>
#include "rendertargets.h"
#include "frameconstants.h"
#include "lightingresources.h"
#include "renderconfiguration.h"
#include "graphicslog.h"
#include "gbufferpreview.h"

#include "editorwidgets.h"
#include "editorworkspace.h"
#include "editorshell.h"
#include "viewportinputgate.h"
#include "editorpreferences.h"
#include <imgui_internal.h> // DockBuilder は同梱の docking ブランチで提供される。

using namespace EditorWidgets;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
    // ImGui 1.92 はフォントアトラスの拡張中に複数のアトラステクスチャを保持する場合がある。
    struct ImGuiSrvPool
    {
        D3D12_CPU_DESCRIPTOR_HANDLE CpuStart{};
        D3D12_GPU_DESCRIPTOR_HANDLE GpuStart{};
        UINT Increment = 0;
        array<bool, ImGuiManager::kSrvDescriptorCount> Used{};
    };

    ImGuiSrvPool g_ImGuiSrvPool;
}

bool ImGuiManager::Init(HWND hwnd, ID3D12Device* device, ID3D12CommandQueue* commandQueue, int numFrames,
                        DXGI_FORMAT rtvFormat, ID3D12DescriptorHeap* cbvHeap, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
                        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle)
{
    WriteGraphicsLog("ImGui: check version\n");
    IMGUI_CHECKVERSION();
    WriteGraphicsLog("ImGui: create context\n");
    ImGui::CreateContext();
    // シーケンサーの表示設定（[RPSequencer]）を editor_layout.cfg から読めるよう、読み込み前に登録する
    SequencerEditorBridge::RegisterSettings();
    ImGuizmo::SetImGuiContext(ImGui::GetCurrentContext());
    ImGuiIO& io = ImGui::GetIO();
    char validationMode[8]{};
    if (GetEnvironmentVariableA("DX12_VALIDATION_MODE", validationMode, sizeof(validationMode)) > 0 &&
        atoi(validationMode) != 0)
    {
        io.IniFilename = nullptr;
    }
    else
    {
        error_code error;
        filesystem::create_directories("Save", error);
        io.IniFilename = "Save/editor_layout.cfg";
        if (!filesystem::exists(io.IniFilename, error) && filesystem::exists("imgui.ini", error))
        {
            ImGui::LoadIniSettingsFromDisk("imgui.ini");
        }
    }
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigDockingWithShift = false;

    WriteGraphicsLog("ImGui: style\n");
    EditorPreferences::Load(); // テーマ / UI スケール / フォント / スナップ設定
    StyleModernSlim();

    WriteGraphicsLog("ImGui: load font\n");
    // 游ゴシック → メイリオ → MS ゴシックの順に探し、アイコン用に Segoe UI Symbol を合成する
    EditorPreferences::LoadFonts();

    WriteGraphicsLog("ImGui: Win32 backend\n");
    if (!ImGui_ImplWin32_Init(hwnd))
    {
        return false;
    }

    ImGui_ImplDX12_InitInfo initInfo{};
    initInfo.Device = device;
    initInfo.CommandQueue = commandQueue;
    initInfo.NumFramesInFlight = numFrames;
    initInfo.RTVFormat = rtvFormat;
    initInfo.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    initInfo.SrvDescriptorHeap = cbvHeap;
    g_ImGuiSrvPool = {};
    g_ImGuiSrvPool.CpuStart = cpuHandle;
    g_ImGuiSrvPool.GpuStart = gpuHandle;
    g_ImGuiSrvPool.Increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    initInfo.UserData = &g_ImGuiSrvPool;
    initInfo.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu,
                                       D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu) {
        auto& pool = *static_cast<ImGuiSrvPool*>(info->UserData);
        for (UINT i = 0; i < pool.Used.size(); ++i)
        {
            if (pool.Used[i])
            {
                continue;
            }
            pool.Used[i] = true;
            *out_cpu = CD3DX12_CPU_DESCRIPTOR_HANDLE(pool.CpuStart, i, pool.Increment);
            *out_gpu = CD3DX12_GPU_DESCRIPTOR_HANDLE(pool.GpuStart, i, pool.Increment);
            return;
        }
        WriteGraphicsLog("ERROR: ImGui SRV descriptor pool exhausted\n");
        // バックエンドには確保失敗を返す経路がないため、使用中の SRV を上書きしない。
        abort();
    };
    initInfo.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                                      D3D12_GPU_DESCRIPTOR_HANDLE gpu) {
        auto& pool = *static_cast<ImGuiSrvPool*>(info->UserData);
        const SIZE_T offset = cpu.ptr - pool.CpuStart.ptr;
        const SIZE_T index = offset / pool.Increment;
        const bool valid = offset % pool.Increment == 0 && index < pool.Used.size() &&
                           gpu.ptr == pool.GpuStart.ptr + index * pool.Increment;
        IM_ASSERT(valid && "Invalid ImGui SRV descriptor");
        if (valid)
        {
            IM_ASSERT(pool.Used[index] && "ImGui SRV descriptor already freed");
            pool.Used[index] = false;
        }
    };

    WriteGraphicsLog("ImGui: DX12 backend\n");
    if (!ImGui_ImplDX12_Init(&initInfo))
    {
        return false;
    }
    if (!GBufferPreview::Initialize(device, rtvFormat, initInfo.DSVFormat))
    {
        WriteGraphicsLog("ERROR: GBuffer preview initialization failed\n");
    }
    else
    {
        initInfo.SrvDescriptorAllocFn(&initInfo, &GBufferPreview::BloomCpu, &GBufferPreview::BloomGpu);
    }
    WriteGraphicsLog("ImGui: initialization complete\n");

    return true;
}

void ImGuiManager::Uninit()
{
    MaterialGraphEditor::Shutdown();
    EditorWorkspaceManager::Get().Shutdown();
    SequencerEditorBridge::Shutdown();
    GBufferPreview::Shutdown();
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
}

void ImGuiManager::Update()
{
    if (IsFeatureSmokeTestEnabled())
    {
        m_ShowGBufferWindow = true;
        m_ShowProceduralWindow = true;
        m_ShowRaytracingWindow = true;
        m_ShowAtmosphereWindow = true;
        m_ShowRenderDebugger = true;
        m_ShowGpuSceneWindow = true;
        m_ShowGpuDrivenWindow = true;
        m_ShowVisibilityWindow = true;
        m_ShowMeshShaderWindow = true;
        m_ShowVirtualGeometryWindow = true;
        m_ShowCloudWindow = true;
        m_ShowTextureListWindow = true;
    }
    if (!m_ProjectSettingsLoaded)
    {
        if (!RenderingValidation::IsEnabled())
        {
            LoadProjectSettings(); // cfg に scene= があれば、ここでそのシーンを開く
        }
        m_ProjectSettingsLoaded = true;
        if (!m_SceneTracking)
        {
            // 起動時の既定シーン（Untitled）。モデルの非同期読み込みが落ち着くまで基準を更新する
            BeginSceneBaseline(5.0);
        }
    }
    // シーンの切り替え・新規作成・終了は、描画前のこの位置でまとめて実行する
    ProcessSceneAction();

    InitializeWorkspaces();
    EditorWorkspaceManager::Get().BeginFrame();
    // テーマ / UI スケールの変更は NewFrame の前にまとめて反映する（フレーム途中でサイズが変わらないように）
    EditorPreferences::ApplyPending();
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    MaterialValidation::BeforeEditorFrame();
    ThumbnailValidation::BeforeEditorFrame();
    ImGui::NewFrame();
    ValidateEditorObjects();
    MaterialValidation::InjectDragPayload();
    ImGuizmo::BeginFrame();
    DebugSystem::SetShowLightDebug(m_ShowLightDebug);

    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false))
    {
        if (io.KeyShift && MaterialEditorHasFocus())
        {
            MaterialEditorUndo(true);
        }
        else if (io.KeyShift)
        {
            Redo();
        }
        else if (MaterialEditorHasFocus())
        {
            MaterialEditorUndo(false);
        }
        else
        {
            Undo();
        }
    }
    if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false))
    {
        if (MaterialEditorHasFocus())
        {
            MaterialEditorUndo(true);
        }
        else
        {
            Redo();
        }
    }

    MaterialPreview::SetVisible(false);
    // Ctrl+S：シーンを保存（未保存の新規シーンなら名前を付けて保存）、Ctrl+Shift+S：名前を付けて保存、Ctrl+N：新規シーン
    if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false) && !MaterialEditorHasFocus())
    {
        SaveCurrentScene(io.KeyShift);
        string error;
        filesystem::create_directories("Save");
        if (!MaterialManager::SaveSceneReferences("Save/scene_materials.json", error))
        {
            AddLog("Scene save failed: %s", error.c_str());
        }
    }
    if (!io.WantTextInput && io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_N, false))
    {
        RequestNewScene();
    }
    ProcessDroppedFiles();
    EditorShell::HandleGlobalShortcuts(); // F1 ショートカット一覧 / F3 コマンド検索 / Ctrl+, 設定
    // マテリアルグラフ操作中の Q / W / E / R（整列・配置キー）でギズモが切り替わらないようにする
    const bool canSwitchGizmo = !io.WantTextInput && !ImGui::IsAnyItemActive() &&
                                !ImGui::IsMouseDown(ImGuiMouseButton_Right) && !ImGuizmo::IsUsingAny() &&
                                !MaterialGraphEditor::HasFocus();
    if (canSwitchGizmo)
    {
        // Unityと同じ割り当て
        if (!io.KeyCtrl && !io.KeyAlt)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Q, false))
            {
                m_GizmoOperation = 3;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_W, false))
            {
                m_GizmoOperation = 0;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_E, false))
            {
                m_GizmoOperation = 1;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_R, false))
            {
                m_GizmoOperation = 2;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_X, false))
            {
                m_GizmoLocalSpace = !m_GizmoLocalSpace;
            }
        }
    }
    // Ctrl+D：複製（Unityと同じ）。入力中・ギズモ操作中・カメラ飛行中・マテリアルエディター操作中は無効
    if (!io.WantTextInput && io.KeyCtrl && !io.KeyShift && !io.KeyAlt && !ImGui::IsAnyItemActive() &&
        !ImGuizmo::IsUsingAny() && !ImGui::IsMouseDown(ImGuiMouseButton_Right) && !MaterialEditorHasFocus() &&
        !m_AssetBrowserFocused && // アセットブラウザでの Ctrl+D はアセットの複製
        ImGui::IsKeyPressed(ImGuiKey_D, false))
    {
        DuplicateSelectedEntity();
    }
    // Ctrl+P：再生/停止、Ctrl+Shift+P：一時停止（Unityと同じ）
    if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_P, false))
    {
        if (io.KeyShift)
        {
            if (ProjectManager::IsPlaying())
            {
                ProjectManager::TogglePause();
            }
        }
        else
        {
            ProjectManager::TogglePlay();
        }
    }
    if (canSwitchGizmo && !io.KeyCtrl && !io.KeyAlt && m_SelectedEntity != g_kINVALID_ENTITY &&
        Registry::IsAlive(m_SelectedEntity) && ComponentManager::HasComponent<NameComponent>(m_SelectedEntity) &&
        !m_AssetBrowserFocused && // アセットブラウザにフォーカスがある間の F2 はアセットのリネーム
        ImGui::IsKeyPressed(ImGuiKey_F2, false))
    {
        BeginRename(m_SelectedEntity);
    }
    if (m_RenamingEntity != g_kINVALID_ENTITY && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        CancelRename();
    }
    DrawEditorMainMenu();
    // メインツールバー / ステータスバーはドックスペースより先に作り、作業領域から差し引く
    EditorShell::DrawToolbar();
    EditorShell::DrawStatusBar();
    DrawDockSpace();
    m_IsSceneViewHovered = false;
    m_SceneGizmoCapturesMouse = false;
    m_AssetBrowserFocused = false;
    BeginMaterialEditorFrame();
    AnimationPreviewWindow::BeginFrame();
    EditorWorkspaceManager::Get().Registry().DrawVisibleWindows();
    // パイメニュー / Shift+A / テンキー視点 / 視点アニメーション（ピッキングより前に入力を確保する）
    EditorShell::UpdateViewportInteractions();
    // 右クリックでの視点回転 / WASD 移動は、3D ビューポート上で押し始めたときだけ有効にする
    ViewportInputGate::Update(ImGui::IsMouseDown(ImGuiMouseButton_Right),
                              ImGui::IsMouseClicked(ImGuiMouseButton_Right) && m_IsSceneViewHovered &&
                                  !EditorShell::IsPieOpen());
    if (ViewportInputGate::g_RightLookActive)
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_None); // フライ中はカーソルを隠す（UE と同じ）
    }
    UpdateSceneNavigation();
    if (m_IsSceneViewHovered)
        PickEntityFromMouse();
    DrawPrimitiveSettingsWindow();
    DrawRenderFeatureWindows();
    DrawMaterialAssetClosePrompt();

    MaterialGraphEditor::SaveRecovery();
    EditorWorkspaceManager::Get().AutoSave();
    DrawScenePopups();
    EditorShell::DrawOverlays(); // コマンド検索 / 追加メニュー / ショートカット一覧 / 通知トースト
    FinalizeUndoCaptureIfIdle();
    UpdateSceneTracking();
    static double nextSettingsSave = 0.0;
    if (m_ProjectSettingsLoaded && !ProjectManager::IsPlaying() && !ImGui::IsAnyItemActive() &&
        ImGui::GetTime() >= nextSettingsSave)
    {
        SaveProjectSettings();
        if (!RenderingValidation::IsEnabled())
        {
            EditorPreferences::SaveIfChanged();
        }
        nextSettingsSave = ImGui::GetTime() + 2.0;
    }
}

void ImGuiManager::DrawDockSpace()
{
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGuiWindowFlags hostFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                                 ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("EditorDockSpace", nullptr, hostFlags);
    ImGui::PopStyleVar(3);

    // Workspace タブはメインメニューバー内（Blender のトップバーと同じ位置）に移動した
    auto& workspaces = EditorWorkspaceManager::Get();
    if (m_ResetEditorLayout)
    {
        workspaces.RequestReset();
        m_ResetEditorLayout = false;
    }
    workspaces.DrawDockSpace();
    ImGui::End();
}

void ImGuiManager::DrawEditorMainMenu()
{
    if (!ImGui::BeginMainMenuBar())
    {
        return;
    }
    auto& preferences = EditorPreferences::Get();
    ImGui::TextColored(EditorPreferences::Accent(), "◆");
    ImGui::SetItemTooltip("RenderingProject Editor");

    if (ImGui::BeginMenu("ファイル"))
    {
        DrawSceneFileMenu();
        ImGui::Separator();
        if (ImGui::MenuItem("シーンのマテリアル参照を保存"))
        {
            string error;
            filesystem::create_directories("Save");
            if (!MaterialManager::SaveSceneReferences("Save/scene_materials.json", error))
            {
                AddLog("Scene save failed: %s", error.c_str());
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("終了", "Alt+F4"))
        {
            RequestQuit();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("編集"))
    {
        if (ImGui::MenuItem("元に戻す", "Ctrl+Z", false, !m_UndoStack.empty() || m_HasPendingUndo))
        {
            Undo();
        }
        if (ImGui::MenuItem("やり直し", "Ctrl+Y", false, !m_RedoStack.empty()))
        {
            Redo();
        }
        ImGui::Separator();
        const bool hasSelection = m_SelectedEntity != g_kINVALID_ENTITY && Registry::IsAlive(m_SelectedEntity);
        if (ImGui::MenuItem("複製", "Ctrl+D", false, hasSelection))
        {
            DuplicateSelectedEntity();
        }
        if (ImGui::MenuItem("削除", "Delete", false, hasSelection))
        {
            DeleteSelectedEntity();
        }
        if (ImGui::MenuItem("選択解除", nullptr, false, hasSelection))
        {
            SelectEntity(g_kINVALID_ENTITY);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Undo History"))
        {
            EditorWorkspaceManager::Get().Registry().ShowEditorType("history");
        }
        if (ImGui::MenuItem("エディター設定", "Ctrl+,"))
        {
            EditorWorkspaceManager::Get().Registry().ShowEditorType("preferences");
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("作成"))
    {
        EditorShell::DrawAddMenuItems();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("表示"))
    {
        EditorShell::DrawViewMenuItems();
        ImGui::Separator();
        ImGui::MenuItem("メインツールバー", nullptr, &preferences.ShowToolbar);
        ImGui::MenuItem("ステータスバー", nullptr, &preferences.ShowStatusBar);
        ImGui::MenuItem("パネルヘッダー", nullptr, &preferences.ShowAreaHeaders);
        ImGui::MenuItem("通知トースト", nullptr, &preferences.ShowToasts);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("ウィンドウ"))
    {
        EditorWorkspaceManager::Get().Registry().DrawWindowMenu();
        ImGui::Separator();
        if (ImGui::BeginMenu("Workspace"))
        {
            auto& workspaces = EditorWorkspaceManager::Get();
            for (size_t i = 0; i < workspaces.WorkspaceCount(); ++i)
            {
                if (ImGui::MenuItem(workspaces.WorkspaceName(i).c_str(), nullptr, i == workspaces.ActiveWorkspace()))
                {
                    workspaces.RequestSwitch(i);
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Workspaceの初期レイアウトに戻す"))
            EditorWorkspaceManager::Get().RequestReset();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("ヘルプ"))
    {
        if (ImGui::MenuItem("ショートカット一覧", "F1"))
        {
            EditorShell::ToggleShortcutHelp();
        }
        if (ImGui::MenuItem("コマンド検索", "F3"))
        {
            EditorShell::OpenCommandPalette();
        }
        ImGui::EndMenu();
    }

    // Blender のトップバーと同じく、メニューの右に Workspace タブを並べる
    ImGui::SameLine(0.0f, 18.0f * preferences.UiScale);
    EditorWorkspaceManager::Get().DrawTabs();

    if (!preferences.ShowToolbar)
    {
        // ツールバー非表示時の再生ボタン
        ImGui::SameLine(0.0f, 12.0f * preferences.UiScale);
        if (ImGui::SmallButton(ProjectManager::IsPlaying() ? "■ 停止##MenuPlay" : "▶ 再生##MenuPlay"))
        {
            ProjectManager::TogglePlay();
        }
    }
    ImGui::SameLine();
    EditorShell::DrawMainMenuRightSide();
    ImGui::EndMainMenuBar();
}

void ImGuiManager::Draw(ID3D12GraphicsCommandList* commandList)
{
    ImGui::Render();
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList);
}

bool ImGuiManager::HandleWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui::GetCurrentContext() == nullptr)
    {
        return false;
    }
    return ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam) != 0;
}

void ImGuiManager::HandleDroppedFile(const char* path, bool hasPoint, float x, float y)
{
    if (path && path[0] != '\0')
    {
        m_DroppedFiles.push_back({path, hasPoint, ImVec2(x, y)});
    }
}

void ImGuiManager::AddLog(const char* fmt, ...)
{
    char buffer[1024]{};
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    const EditorShell::LogLevel level = EditorShell::ClassifyLog(buffer);
    {
        // 修正：ワーカースレッド（非同期の読み込み / 保存）からの AddLog と描画の競合を防ぐ
        std::lock_guard<std::recursive_mutex> lock(m_LogMutex);
        if (!m_Logs.empty() && m_Logs.back().Text == buffer)
        {
            // 同じメッセージの連続は 1 行にまとめる（UE の Message Log と同じ）
            ++m_Logs.back().Repeat;
        }
        else
        {
            LogEntry entry;
            entry.Text = buffer;
            entry.Level = static_cast<uint8_t>(level);
            entry.Time = static_cast<double>(GetTickCount64()) / 1000.0;
            SYSTEMTIME now{};
            GetLocalTime(&now);
            snprintf(entry.Clock, sizeof(entry.Clock), "%02u:%02u:%02u", static_cast<unsigned>(now.wHour),
                     static_cast<unsigned>(now.wMinute), static_cast<unsigned>(now.wSecond));
            m_Logs.push_back(std::move(entry));
            ++m_LogCounts[static_cast<size_t>(level) & 3];
            // 最適化：vector::erase(begin) の O(n) をやめ、deque の pop_front（O(1)）にする
            while (m_Logs.size() > kMaxLogEntries)
            {
                --m_LogCounts[m_Logs.front().Level & 3];
                m_Logs.pop_front();
            }
        }
        ++m_LogRevision;
    }
    EditorShell::PushToast(level, buffer);
    Debug::Log("%s\n", buffer);
}

void ImGuiManager::StyleModernSlim()
{
    // 配色・余白・角丸はエディター設定（Unreal / Blender / Unity / Midnight）から生成する
    EditorPreferences::ApplyNow();
}

void ImGuiManager::DrawAdjustmentPanel()
{
    if (m_ShowAdjustmentPanel)
    {
        ImGui::SetNextWindowSize(ImVec2(360.0f, 520.0f), ImGuiCond_FirstUseEver);
        if (EditorWindowRegistry::BeginEditorWindow("レンダーコントロール", &m_ShowAdjustmentPanel))
        {
            const float fps = World::GetFrameRate();
            ImVec4 healthyColor;
            if (fps >= 55.0f)
            {
                healthyColor = ImVec4(0.34f, 0.86f, 0.62f, 1.0f);
            }
            else
            {
                ImVec4 statusColor;
                if (fps >= 30.0f)
                {
                    statusColor = ImVec4(0.96f, 0.72f, 0.28f, 1.0f);
                }
                else
                {
                    statusColor = ImVec4(0.96f, 0.38f, 0.38f, 1.0f);
                }
                healthyColor = (statusColor);
            }
            ImGui::TextColored(ImVec4(0.28f, 0.78f, 0.92f, 1.0f), "DIRECTX 12  /  DEFERRED");
            ImGui::SameLine();
            ImGui::TextColored(healthyColor, "%.1f FPS", fps);
            ImGui::TextDisabled("%u x %u  |  internal %u x %u  |  %.2f ms", GraphicsDevice::GetWidth(),
                                GraphicsDevice::GetHeight(), GraphicsDevice::GetSceneWidth(),
                                GraphicsDevice::GetSceneHeight(), World::GetFrameTimeMs());

            ImGui::Spacing();
            ImGui::SeparatorText("映像パイプライン");
            m_cameraPostProcess = static_cast<int>(Camera::GetCameraPostProcess());
            if (m_cameraPostProcess < 0 || m_cameraPostProcess >= static_cast<int>(PostProcessType::COUNT))
            {
                m_cameraPostProcess = 0;
            }
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::Combo("##CameraPostProcess", &m_cameraPostProcess, m_cameraPostProcessModeItems,
                             IM_ARRAYSIZE(m_cameraPostProcessModeItems)))
            {
                Camera::SetCameraPostProcess(static_cast<PostProcessType>(m_cameraPostProcess));
            }

            m_antiAliasingMode = static_cast<int>(RendererState::m_AntiAliasingMode);
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::Combo("アンチエイリアシング", &m_antiAliasingMode, m_antiAliasingModeItems,
                             IM_ARRAYSIZE(m_antiAliasingModeItems)))
            {
                RendererState::m_AntiAliasingMode = static_cast<AntiAliasingMode>(m_antiAliasingMode);
                RendererState::m_TaaFrameIndex = 0;
            }

            DrawUpscaleControls();

            ImGui::Spacing();
            ImGui::SeparatorText("カラー");
            if (ImGui::Checkbox("HDR シーンカラー", &m_HdrEnabled))
            {
                RenderConfiguration::SetHdr(m_HdrEnabled);
            }
            ImGui::SameLine();
            ImGui::Checkbox("ACES", &m_ToneMapEnabled);
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::SliderFloat("露光", &m_Exposure, 0.01f, 10.0f, "%.2f", ImGuiSliderFlags_Logarithmic);

            float postProcessIntensity = Camera::GetCameraPostProcessIntensity();
            const PostProcessType activePostProcess = Camera::GetCameraPostProcess();
            ImGui::BeginDisabled(activePostProcess == PostProcessType::NONE);
            ImGui::SetNextItemWidth(-1.0f);
            float maxPostProcessIntensity;
            if (activePostProcess == PostProcessType::BLOOM)
            {
                maxPostProcessIntensity = 5.0f;
            }
            else
            {
                maxPostProcessIntensity = 1.0f;
            }
            if (ImGui::SliderFloat("エフェクト強度", &postProcessIntensity, 0.0f, maxPostProcessIntensity, "%.2f"))
            {
                Camera::SetCameraPostProcessIntensity(postProcessIntensity);
            }
            if (activePostProcess == PostProcessType::BLOOM)
            {
                float bloomThreshold = Camera::GetCameraBloomThreshold();
                float bloomSoftKnee = Camera::GetCameraBloomSoftKnee();
                float bloomRadius = Camera::GetCameraBloomRadius();
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::SliderFloat("Bloom しきい値", &bloomThreshold, 0.0f, 16.0f, "%.2f"))
                {
                    Camera::SetCameraBloomThreshold(bloomThreshold);
                }
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::SliderFloat("Bloom Soft Knee", &bloomSoftKnee, 0.0f, 1.0f, "%.2f"))
                {
                    Camera::SetCameraBloomSoftKnee(bloomSoftKnee);
                }
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::SliderFloat("Bloom 半径", &bloomRadius, 0.25f, 4.0f, "%.2f"))
                {
                    Camera::SetCameraBloomRadius(bloomRadius);
                }
            }
            ImGui::EndDisabled();

            ImGui::Spacing();
            ImGui::SeparatorText("フレーム制御");
            DrawFpsMeter();
            bool vsyncEnabled = World::IsVSyncEnabled();
            if (ImGui::Checkbox("垂直同期", &vsyncEnabled))
            {
                World::SetVSyncEnabled(vsyncEnabled);
            }
            ImGui::SameLine();
            bool fixedFrameRateEnabled = World::IsFixedFrameRateEnabled();
            if (ImGui::Checkbox("FPS 固定", &fixedFrameRateEnabled))
            {
                World::SetFixedFrameRateEnabled(fixedFrameRateEnabled);
            }
            int targetFrameRate = World::GetTargetFrameRate();
            ImGui::BeginDisabled(!fixedFrameRateEnabled);
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderInt("目標 FPS", &targetFrameRate, 15, 360))
            {
                World::SetTargetFrameRate(targetFrameRate);
            }
            ImGui::EndDisabled();

            ImGui::Spacing();
            if (ImGui::Button("GBuffer を表示", ImVec2(-1.0f, 0.0f)))
            {
                m_ShowGBufferWindow = true;
            }
        }
        ImGui::End();
    }
}
