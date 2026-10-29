#pragma once

// Procedural Studio ウィンドウ。フラクタル・プロシージャル・立体テクスチャ、モデル、
// フラクタルの水や雲を作成し、ソフトウェア描画のライブプレビューとファイル書き出しを行う。
class ProceduralStudioWindow
{
  public:
    static void Draw(bool* open);
    // プレビューテクスチャとディスクリプターを解放する。EditorDescriptors::Shutdown より前に呼ぶ。
    static void Shutdown();
    // エクスプローラーからのドロップ（WM_DROPFILES）。x, y はメインウィンドウのクライアント座標。
    // 直前のフレームで表示した画像ドロップ領域の内側に落とされた画像なら読み込んで true を返す。
    // path は DragQueryFileA の値（ANSI コードページ）。
    static bool AcceptDroppedFile(const char* path, float x, float y);
};
