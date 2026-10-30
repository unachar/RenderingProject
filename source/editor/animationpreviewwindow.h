#pragma once

class AnimationPreviewWindow
{
  public:
    static void Draw(bool* open);
    static void Render();
    static void Shutdown();
    static void OpenForSelection();

  private:
    static void ApplyToSelection();
};
