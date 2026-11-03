#pragma once
// ============================================================================
// ViewportInputGate：エディターカメラの右クリック操作（視点回転 + WASD/QE 移動）を
// 「3D ビューポート上で右ボタンを押し始めたとき」だけに限定する。
//  ・ImGuiManager::Update がビューポートのホバー状態を見て毎フレーム更新する
//  ・押し始めた後はボタンを離すまで継続（ドラッグ中にビューポート外へ出ても止まらない。UE と同じ）
//  ・エディター UI が無い実行形態（検証 / ランタイムのみ）では従来どおり右ボタンの状態をそのまま使う
// ============================================================================
#include "input.h"

namespace ViewportInputGate
{
    inline bool g_EditorDrivesInput = false; // エディターが毎フレーム状態を更新しているか
    inline bool g_RightLookActive = false;   // ビューポートで右ボタンを押し始め、まだ離していない
    // #3 エディターカメラのフライ速度の倍率（ビューポートのカメラ速度ボタン / 右ドラッグ + ホイール）
    inline float g_EditorCameraSpeedScale = 1.0f;

    inline bool CameraLookActive()
    {
        return g_EditorDrivesInput ? g_RightLookActive : Input::IsKeyHeld(VK_RBUTTON);
    }

    // rightDown: 右ボタンが押されているか / rightPressedInViewport: このフレームにビューポート上で押されたか
    inline void Update(bool rightDown, bool rightPressedInViewport)
    {
        g_EditorDrivesInput = true;
        if (!rightDown)
        {
            g_RightLookActive = false;
        }
        else if (rightPressedInViewport)
        {
            g_RightLookActive = true;
        }
    }
} // namespace ViewportInputGate

