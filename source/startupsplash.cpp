#include "startupsplash.h"
#include <gdiplus.h>
#include <algorithm>
#include <cwchar>
#include <system_error>

#pragma comment(lib, "gdiplus.lib")

namespace
{
constexpr wchar_t WindowClass[] = L"DX12AI_StartupSplash";
constexpr int Width = 440;
constexpr int Height = 280;
constexpr int ImageSize = 128;

struct SplashView
{
    const std::atomic<int>& percent;
    ULONGLONG startedAt = GetTickCount64();
    Gdiplus::Bitmap frame{Width, Height, PixelFormat32bppPARGB};
    Gdiplus::Bitmap cat{ImageSize, ImageSize, PixelFormat32bppPARGB};
    bool hasImage = false;

    void LoadImage()
    {
        Gdiplus::Bitmap source(L"asset\\texture\\neko.png");
        const UINT width = source.GetWidth();
        const UINT height = source.GetHeight();
        if (width == 0 || height == 0)
        {
            return;
        }

        const float scale = static_cast<float>(ImageSize) / (std::max)(width, height);
        const float scaledWidth = width * scale;
        const float scaledHeight = height * scale;
        Gdiplus::Graphics graphics(&cat);
        graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        hasImage = graphics.DrawImage(&source, Gdiplus::RectF(
            (ImageSize - scaledWidth) / 2, (ImageSize - scaledHeight) / 2, scaledWidth, scaledHeight)) == Gdiplus::Ok;
    }

    void Paint(HDC dc)
    {
        using namespace Gdiplus;
        {
            Graphics graphics(&frame);
            graphics.Clear(Color(24, 27, 33));
            if (hasImage)
            {
                constexpr float revolutionMs = 2400.0f;
                const auto elapsed = (GetTickCount64() - startedAt) % 2400;
                graphics.SetInterpolationMode(InterpolationModeHighQualityBilinear);
                graphics.TranslateTransform(Width / 2.0f, 118.0f);
                graphics.RotateTransform(static_cast<float>(elapsed) * 360.0f / revolutionMs);
                graphics.DrawImage(&cat, -ImageSize / 2, -ImageSize / 2, ImageSize, ImageSize);
                graphics.ResetTransform();
            }

            wchar_t label[8]{};
            swprintf_s(label, L"%d%%", percent.load());
            Font font(L"Segoe UI", 24.0f, FontStyleRegular, UnitPixel);
            SolidBrush text(Color(240, 242, 246));
            StringFormat format;
            format.SetAlignment(StringAlignmentCenter);
            format.SetLineAlignment(StringAlignmentCenter);
            graphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
            graphics.DrawString(label, -1, &font, RectF(0, 216, Width, 44), &format, &text);
        }
        Graphics window(dc);
        window.DrawImage(&frame, 0, 0, Width, Height);
    }
};

LRESULT CALLBACK SplashWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_NCCREATE:
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
        break;
    case WM_PAINT:
    {
        auto* view = reinterpret_cast<SplashView*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        view->Paint(dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_CLOSE:
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void ShowSplash(HINSTANCE instance, HANDLE stopEvent, const std::atomic<int>& percent)
{
    WNDCLASSW windowClass{};
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = SplashWindowProc;
    windowClass.lpszClassName = WindowClass;
    if (!RegisterClassW(&windowClass))
    {
        return;
    }

    SplashView view{percent};
    view.LoadImage();
    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    const int x = workArea.left + (workArea.right - workArea.left - Width) / 2;
    const int y = workArea.top + (workArea.bottom - workArea.top - Height) / 2;
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, WindowClass, L"Starting DirectX12",
                                  WS_POPUP, x, y, Width, Height, nullptr, nullptr, instance, &view);
    if (window)
    {
        SetWindowPos(window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        UpdateWindow(window);
        // Pump messages independently while the renderer initializes on the main thread.
        for (;;)
        {
            const DWORD result = MsgWaitForMultipleObjectsEx(1, &stopEvent, 16, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (result == WAIT_OBJECT_0 || result == WAIT_FAILED)
            {
                break;
            }
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                DispatchMessageW(&message);
            }
            if (result == WAIT_TIMEOUT)
            {
                InvalidateRect(window, nullptr, FALSE);
            }
        }
        if (percent.load() == 100)
        {
            RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
            // Keep the final percentage visible briefly before showing the editor.
            Sleep(120);
        }
        DestroyWindow(window);
    }
    UnregisterClassW(WindowClass, instance);
}
}

StartupSplash::StartupSplash(HINSTANCE instance, bool enabled)
{
    if (!enabled)
    {
        return;
    }
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stopEvent_)
    {
        try
        {
            thread_ = std::thread(&StartupSplash::Run, this, instance);
        }
        catch (const std::system_error&)
        {
            Close();
        }
    }
}

StartupSplash::~StartupSplash()
{
    Close();
}

void StartupSplash::SetProgress(int percent)
{
    percent_.store(percent);
}

void StartupSplash::Complete()
{
    SetProgress(100);
    Close();
}

void StartupSplash::Close()
{
    if (stopEvent_)
    {
        SetEvent(stopEvent_);
        if (thread_.joinable())
        {
            thread_.join();
        }
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
}

void StartupSplash::Run(HINSTANCE instance)
{
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok)
    {
        ShowSplash(instance, stopEvent_, percent_);
        Gdiplus::GdiplusShutdown(token);
    }
}
