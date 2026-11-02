#pragma once
// SequencerEditorBridge : SequencerWindow をエンジン（ImGuiManager / ECS / TimeLineSystem）に接続する。
class SequencerEditorBridge
{
  public:
    static void RegisterSettings(); // ImGui::CreateContext 直後（.cfg 読み込み前）に呼ぶ
    static void Draw(bool* open);
    static void Shutdown();
};
