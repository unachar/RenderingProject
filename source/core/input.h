#pragma once
#include "main.h"

class Input
{
  private:
    static BYTE m_OldKeyState[256];
    static BYTE m_CurrentKeyState[256];
    static POINT m_OldMousePosition;
    static POINT m_CurrentMousePosition;
    static POINT m_MouseDelta;

  public:
    static void Init();
    static void Uninit();
    static void Update();

    static bool IsKeyHeld(int key);
    static bool IsKeyPress(int key);
    static POINT GetMouseDelta();
    // Raw Input（WM_INPUT）のマウス移動量を積む。WindowProc から呼ぶ
    //  ・カーソル位置の差分と違い、カーソルの固定（ClipCursor）・画面端・ポインター加速の影響を受けない
    //  ・登録に成功していれば GetMouseDelta はこの積算値を返す（右クリックの視点移動が滑らかになる）
    static void AddRawMouseDelta(LONG dx, LONG dy);
    static bool UsesRawMouse();

  private:
    static LONG m_RawDeltaX;
    static LONG m_RawDeltaY;
    static bool m_RawMouse;
};

