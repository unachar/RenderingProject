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
};

