#include "pch.h"
#include "input.h"
#include <cstring>

BYTE Input::m_OldKeyState[256]{};
BYTE Input::m_CurrentKeyState[256]{};
POINT Input::m_OldMousePosition{};
POINT Input::m_CurrentMousePosition{};
POINT Input::m_MouseDelta{};
LONG Input::m_RawDeltaX = 0;
LONG Input::m_RawDeltaY = 0;
bool Input::m_RawMouse = false;

void Input::Init()
{
    memset(m_OldKeyState, 0, sizeof(m_OldKeyState));
    memset(m_CurrentKeyState, 0, sizeof(m_CurrentKeyState));
    GetCursorPos(&m_CurrentMousePosition);
    m_OldMousePosition = m_CurrentMousePosition;
    m_MouseDelta = {};
    // マウスの Raw Input を登録（hwndTarget = nullptr：フォーカスのあるウィンドウに WM_INPUT が届く）
    RAWINPUTDEVICE device{};
    device.usUsagePage = 0x01; // HID_USAGE_PAGE_GENERIC
    device.usUsage = 0x02;     // HID_USAGE_GENERIC_MOUSE
    device.dwFlags = 0;
    device.hwndTarget = nullptr;
    m_RawMouse = RegisterRawInputDevices(&device, 1, sizeof(device)) != FALSE;
    m_RawDeltaX = m_RawDeltaY = 0;
}

void Input::Uninit() {}

void Input::Update()
{
    memcpy(m_OldKeyState, m_CurrentKeyState, sizeof(m_OldKeyState));
    m_OldMousePosition = m_CurrentMousePosition;
    GetCursorPos(&m_CurrentMousePosition);
    if (m_RawMouse)
    {
        // 前の Update からの Raw Input の積算（フレーム間の動きを取りこぼさない）
        m_MouseDelta.x = m_RawDeltaX;
        m_MouseDelta.y = m_RawDeltaY;
    }
    else
    {
        m_MouseDelta.x = m_CurrentMousePosition.x - m_OldMousePosition.x;
        m_MouseDelta.y = m_CurrentMousePosition.y - m_OldMousePosition.y;
    }
    m_RawDeltaX = m_RawDeltaY = 0;

    static const int usedKeys[] = {
        'A',    'B',     'C',       'D',        'E',        'F',        'G',      'H',        'I',
        'J',    'K',     'L',       'M',        'N',        'O',        'P',      'Q',        'R',
        'S',    'T',     'U',       'V',        'W',        'X',        'Y',      'Z',        '0',
        '1',    '2',     '3',       '4',        '5',        '6',        '7',      '8',        '9',
        VK_UP,  VK_DOWN, VK_LEFT,   VK_RIGHT,   VK_ESCAPE,  VK_SPACE,   VK_SHIFT, VK_CONTROL, VK_MENU,
        VK_TAB, VK_BACK, VK_RETURN, VK_LBUTTON, VK_RBUTTON, VK_MBUTTON,
    };

    for (int key : usedKeys)
    {
        if ((GetAsyncKeyState(key) & 0x8000))
        {
            m_CurrentKeyState[key] = 0x80;
        }
        else
        {
            m_CurrentKeyState[key] = 0;
        }
    }
}

bool Input::IsKeyHeld(int key)
{
    if (key < 0 || key >= 256)
    {
        return false;
    }

    return (m_CurrentKeyState[key] & 0x80) != 0;
}

bool Input::IsKeyPress(int key)
{
    if (key < 0 || key >= 256)
    {
        return false;
    }

    const bool current = (m_CurrentKeyState[key] & 0x80) != 0;
    const bool old = (m_OldKeyState[key] & 0x80) != 0;
    return current && !old;
}

POINT Input::GetMouseDelta()
{
    return m_MouseDelta;
}


void Input::AddRawMouseDelta(LONG dx, LONG dy)
{
    m_RawDeltaX += dx;
    m_RawDeltaY += dy;
}

bool Input::UsesRawMouse()
{
    return m_RawMouse;
}
