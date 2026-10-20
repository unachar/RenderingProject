#include "pch.h"
#include "main.h"
#include "resource.h"
#include "game.h"
#include "graphicsdevice.h"
#include "graphicslog.h"
#include "imguimanager.h"
#include <memory>
#include <shellapi.h>
#include "renderconfiguration.h"
#include "renderingvalidation.h"
#include "performancegovernor.h"
#include "renderbenchmark.h"
#include "proceduralworkgraph.h"
#include "rendercapture.h"

extern "C"
{
    __declspec(dllexport) extern const UINT D3D12SDKVersion = 715;
    __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}

static bool g_IsResizing = false;

static bool IsAutomatedBenchmark()
{
    char value[16]{};
    return GetEnvironmentVariableA("DX12_BENCHMARK_FRAMES", value, static_cast<DWORD>(size(value))) > 0 &&
           atoi(value) > 0;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (uMsg == WM_DROPFILES)
    {
        HDROP drop = reinterpret_cast<HDROP>(wParam);
        const UINT count = DragQueryFileA(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i)
        {
            char path[MAX_PATH]{};
            if (DragQueryFileA(drop, i, path, MAX_PATH) > 0)
            {
                ImGuiManager::HandleDroppedFile(path);
            }
        }
        DragFinish(drop);
        return 0;
    }

    // ×ボタン / Alt+F4：未保存のシーンがあれば「保存しますか？」を出す
    // （デバイスロスト時などにエンジンから送られる WM_CLOSE はそのまま閉じる）
    if (uMsg == WM_SYSCOMMAND && (wParam & 0xFFF0) == SC_CLOSE)
    {
        ImGuiManager::RequestQuit();
        return 0;
    }

    if (ImGuiManager::HandleWndProc(hwnd, uMsg, wParam, lParam))
    {
        return true;
    }

    if (uMsg == WM_DESTROY)
    {
        PostQuitMessage(0);
        return 0;
    }
    if (uMsg == WM_ENTERSIZEMOVE)
    {
        g_IsResizing = true;
        SetTimer(hwnd, 1, 16, NULL);
        return 0;
    }
    if (uMsg == WM_EXITSIZEMOVE)
    {
        g_IsResizing = false;
        KillTimer(hwnd, 1);
        RECT rc;
        GetClientRect(hwnd, &rc);
        UINT width = rc.right - rc.left;
        UINT height = rc.bottom - rc.top;
        if (width > 0 && height > 0)
        {
            RenderConfiguration::Resize(width, height);
        }
        return 0;
    }
    if (uMsg == WM_TIMER && g_IsResizing)
    {
        RECT rc;
        GetClientRect(hwnd, &rc);
        const UINT width = static_cast<UINT>(rc.right - rc.left);
        const UINT height = static_cast<UINT>(rc.bottom - rc.top);
        if (width > 0 && height > 0)
        {
            RenderConfiguration::Resize(width, height);
        }
        return 0;
    }
    if (uMsg == WM_SIZE)
    {
        if (!g_IsResizing && wParam != SIZE_MINIMIZED)
        {
            UINT width = LOWORD(lParam);
            UINT height = HIWORD(lParam);
            if (width > 0 && height > 0)
            {
                RenderConfiguration::Resize(width, height);
            }
        }
        return 0;
    }

    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

static bool IsPhysicsSmokeTest()
{
    char value[8]{};
    return GetEnvironmentVariableA("DX12_PHYSICS_SMOKE_TEST", value, static_cast<DWORD>(size(value))) > 0 &&
           atoi(value) != 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    const bool automatedBenchmark = IsAutomatedBenchmark();
    char rayTelemetry[8]{};
    GetEnvironmentVariableA("DX12_RAY_TELEMETRY", rayTelemetry, sizeof(rayTelemetry));
    RayTelemetry::SetEnabled(atoi(rayTelemetry) != 0);
    char startupSmokeValue[16]{};
    GetEnvironmentVariableA("DX12_STARTUP_SMOKE_FRAMES", startupSmokeValue,
                            static_cast<DWORD>(size(startupSmokeValue)));
    const int startupSmokeFrames = (max)(0, atoi(startupSmokeValue));
    char resizeSmokeValue[8]{};
    GetEnvironmentVariableA("DX12_RESIZE_SMOKE_TEST", resizeSmokeValue, static_cast<DWORD>(size(resizeSmokeValue)));
    const bool resizeSmoke = startupSmokeFrames > 0 && atoi(resizeSmokeValue) != 0;
    const bool renderingBenchmark = RenderBenchmark::IsEnabled();
    if (renderingBenchmark)
    {
        SetEnvironmentVariableA("DX12_VALIDATION_MODE", "1");
    }
    const bool automatedRun =
        automatedBenchmark || renderingBenchmark || IsPhysicsSmokeTest() || startupSmokeFrames > 0;
    if (automatedRun)
    {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    }
    char exePath[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);

    filesystem::path dir = filesystem::path(exePath).parent_path();
    filesystem::path runtimeRoot;
    for (int i = 0; i < 8 && !dir.empty(); ++i)
    {
        if (filesystem::exists(dir / "shader" / "hlsl" / "build") && filesystem::exists(dir / "asset"))
        {
            runtimeRoot = dir;
            break;
        }
        dir = dir.parent_path();
    }
    if (!runtimeRoot.empty())
    {
        SetCurrentDirectoryW(runtimeRoot.c_str());
    }
    WriteGraphicsLog("Step: WinMain begin\n");

    const char className[] = "DX12AI_WindowClass";
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = className;
    wc.hIcon =
        static_cast<HICON>(LoadImageA(hInstance, MAKEINTRESOURCEA(IDI_APP_ICON), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    RegisterClass(&wc);

    RECT rc = {0, 0, SCREEN_WIDTH, SCREEN_HEIGHT};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowEx(0, className, "DirectX12Window", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                               rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hInstance, nullptr);

    if (hwnd == nullptr)
    {
        return 0;
    }
    HICON appIcon =
        static_cast<HICON>(LoadImageA(hInstance, MAKEINTRESOURCEA(IDI_APP_ICON), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    if (appIcon)
    {
        SendMessage(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(appIcon));
        SendMessage(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(appIcon));
    }
    ShowWindow(hwnd, nCmdShow);
    WriteGraphicsLog("Step: window shown\n");
    DragAcceptFiles(hwnd, TRUE);

    if (!GraphicsDevice::Init(hwnd))
    {
        if (!automatedRun)
        {
            MessageBoxA(hwnd, "Failed to initialize renderer.", "Error", MB_OK | MB_ICONERROR);
        }
        return 1;
    }

    char proceduralValidation[8]{};
    if (GetEnvironmentVariableA("DX12_WORK_GRAPH_VALIDATE_PROCEDURAL", proceduralValidation, sizeof(proceduralValidation)) &&
        atoi(proceduralValidation) != 0)
    {
        const bool valid = ProceduralWorkGraph::ValidateAuthoringOutputs();
        GraphicsDevice::Uninit();
        return valid ? 0 : 2;
    }

    RenderProfiler::SetPipelineStatisticsForced(automatedRun);
    if (renderingBenchmark)
    {
        RenderProfiler::SetEnabled(true);
    }
    // 自動テストは固定品質で比較するため、品質の自動調整を止める。
    PerformanceGovernor::GetOptions().Enabled = !automatedRun;
    WriteGraphicsLog("Step: Game::Init\n");
    Game::Init();
    WriteGraphicsLog("Step: Game::Create\n");
    Game::Create();
    WriteGraphicsLog("Step: initialization complete; entering main loop\n");

    UINT64 completedFrames = 0;
    MSG msg = {0};
    while (msg.message != WM_QUIT)
    {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
            {
                break;
            }
        }

        if (msg.message == WM_QUIT)
        {
            break;
        }

        if (!automatedRun && (IsIconic(hwnd) || !IsWindowVisible(hwnd)))
        {
            MsgWaitForMultipleObjectsEx(0, nullptr, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            continue;
        }

        if (!automatedRun && GetForegroundWindow() != hwnd)
        {
            MsgWaitForMultipleObjectsEx(0, nullptr, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }

        if (completedFrames == 0)
        {
            WriteGraphicsLog("Step: first frame begin\n");
        }
        if (resizeSmoke &&
            (completedFrames == 8 || completedFrames == 40 || completedFrames == 72 || completedFrames == 104))
        {
            const UINT resizeWidths[] = {1919, 1200, 1920, 1200};
            const UINT resizeHeights[] = {677, 600, 1080, 600};
            const UINT step = static_cast<UINT>((completedFrames - 8) / 32);
            WriteGraphicsLog("Resize smoke: frame=%llu extent=%ux%u\n", completedFrames, resizeWidths[step],
                             resizeHeights[step]);
            RenderConfiguration::Resize(resizeWidths[step], resizeHeights[step]);
        }
        if (renderingBenchmark)
        {
            RenderBenchmark::BeforeFrame(completedFrames);
        }
        else
        {
            RenderingValidation::BeforeFrame(completedFrames);
        }
        Game::Run();
        if (renderingBenchmark)
        {
            RenderBenchmark::AfterFrame();
        }
        else
        {
            RenderingValidation::AfterFrame(completedFrames);
        }
        if (completedFrames == 0)
        {
            WriteGraphicsLog("Step: first frame complete\n");
        }
        ++completedFrames;
        if (RenderBenchmark::ShouldStop(completedFrames))
        {
            PostQuitMessage(0);
        }
        if (startupSmokeFrames > 0 && completedFrames >= static_cast<UINT64>(startupSmokeFrames))
        {
            PostQuitMessage(0);
        }
    }
    bool featuresPassed = renderingBenchmark ? RenderBenchmark::Finish() : RenderingValidation::Finish();
    if (startupSmokeFrames > 0 && completedFrames > 0)
    {
        GraphicsDevice::WaitForGpuIdle();
        RenderProfiler::DrainCompletedFrames([](const RenderProfiler::FrameSnapshot&) {});
        const auto& snapshot = RenderProfiler::GetLatest();
        WriteGraphicsLog("WorkGraph frame sample: frame=%llu graphs=%llu draws=%llu indirect=%llu CPU=%.3f GPU=%.3f ms\n",
                         snapshot.Serial, snapshot.Calls.WorkGraphDispatchCalls, snapshot.Calls.DrawCalls,
                         snapshot.Calls.ExecuteIndirectCalls, snapshot.CpuRenderMs, snapshot.GpuRenderMs);
        char capture[8]{};
        if (GetEnvironmentVariableA("DX12_WORK_GRAPH_CAPTURE", capture, sizeof(capture)) && atoi(capture) != 0)
        {
            featuresPassed &= RenderCapture::SaveScenePng(L"cache/workgraphs-scene.png");
        }
        featuresPassed &= RenderCapture::ReportGraphicsErrors() == 0;
    }
    Game::Uninit();
    if (!featuresPassed)
    {
        return 1;
    }
    if (startupSmokeFrames > 0)
    {
        if (completedFrames < static_cast<UINT64>(startupSmokeFrames))
        {
            WriteGraphicsLog("ERROR: startup smoke test interrupted before target frame count\n");
            return 1;
        }
        WriteGraphicsLog("Step: startup smoke test complete; shutdown complete\n");
    }
    return 0;
}
