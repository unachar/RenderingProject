#include "pch.h"
#include "input.h"
#include <cstring>

BYTE Input::m_OldKeyState[256]{};
BYTE Input::m_CurrentKeyState[256]{};
POINT Input::m_OldMousePosition{};
POINT Input::m_CurrentMousePosition{};
POINT Input::m_MouseDelta{};

void Input::Init()
{
	memset(m_OldKeyState, 0, sizeof(m_OldKeyState));
	memset(m_CurrentKeyState, 0, sizeof(m_CurrentKeyState));
	GetCursorPos(&m_CurrentMousePosition);
	m_OldMousePosition = m_CurrentMousePosition;
	m_MouseDelta = {};
}

void Input::Uninit()
{
}

void Input::Update()
{
	memcpy(m_OldKeyState, m_CurrentKeyState, sizeof(m_OldKeyState));
	m_OldMousePosition = m_CurrentMousePosition;
	GetCursorPos(&m_CurrentMousePosition);
	m_MouseDelta.x = m_CurrentMousePosition.x - m_OldMousePosition.x;
	m_MouseDelta.y = m_CurrentMousePosition.y - m_OldMousePosition.y;

	static const int usedKeys[] = {
		'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
		'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
		'0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
		VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_ESCAPE, VK_SPACE, VK_SHIFT, VK_CONTROL,
		VK_MENU, VK_TAB, VK_BACK, VK_RETURN,
		VK_LBUTTON, VK_RBUTTON, VK_MBUTTON,
	};

	for (int key : usedKeys)
	{
		m_CurrentKeyState[key] = (GetAsyncKeyState(key) & 0x8000) ? 0x80 : 0;
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

