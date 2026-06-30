#include "pch.h"
#include "main.h"
#include "resource.h"
#include "game.h"
#include "rendererdraw.h"
#include "renderercore.h"
#include "imguimanager.h"
#include <memory>
#include <shellapi.h>

extern "C"
{
	__declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
	__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}

static bool g_IsResizing = false;

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
			RendererCore::Resize(width, height);
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
			RendererCore::Resize(width, height);
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
				RendererCore::Resize(width, height);
			}
		}
		return 0;
	}

	return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
	char exePath[MAX_PATH]{};
	GetModuleFileNameA(nullptr, exePath, MAX_PATH);
	string dir = string(exePath);
	for (int i = 0; i < 3; ++i)
	{
		size_t pos = dir.find_last_of("\\/");
		if (pos != string::npos) dir = dir.substr(0, pos);
	}
	if (!dir.empty()) SetCurrentDirectoryA(dir.c_str());

	const char className[] = "DX12AI_WindowClass";
	WNDCLASSA wc = { 0 };
	wc.lpfnWndProc = WindowProc;
	wc.hInstance = hInstance;
	wc.lpszClassName = className;
	wc.hIcon = static_cast<HICON>(LoadImageA(hInstance, MAKEINTRESOURCEA(IDI_APP_ICON), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
	RegisterClass(&wc);

	
	RECT rc = { 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT };
	AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

	HWND hwnd = CreateWindowEx(0, className, "DirectX12Window",
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
		nullptr, nullptr, hInstance, nullptr);

	if (hwnd == nullptr)
	{
		return 0;
	}
	HICON appIcon = static_cast<HICON>(LoadImageA(hInstance, MAKEINTRESOURCEA(IDI_APP_ICON), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
	if (appIcon)
	{
		SendMessage(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(appIcon));
		SendMessage(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(appIcon));
	}
	ShowWindow(hwnd, nCmdShow);
	DragAcceptFiles(hwnd, TRUE);

	if (!RendererCore::Init(hwnd))
	{
		MessageBoxA(hwnd, "Failed to initialize renderer.", "Error", MB_OK | MB_ICONERROR);
		return 0;
	}

	Game::Init();
	Game::Create();
	
	MSG msg = { 0 };
	while (msg.message != WM_QUIT)
	{
		if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		else
		{
			Game::Run();
		}
	}
	Game::Uninit();
	return 0;
}

