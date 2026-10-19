#pragma once

// Procedural Studio ウィンドウ。フラクタル・プロシージャル・立体テクスチャ、モデル、
// フラクタルの水や雲を作成し、ソフトウェア描画のライブプレビューとファイル書き出しを行う。
class ProceduralStudioWindow
{
  public:
    static void Draw(bool* open);
    // プレビューテクスチャとディスクリプターを解放する。EditorDescriptors::Shutdown より前に呼ぶ。
    static void Shutdown();
};
