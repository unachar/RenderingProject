#pragma once

// Procedural Studio window: fractal / procedural textures, solid textures, procedural models and
// fractal water / clouds with a live software-rendered model preview and file export.
class ProceduralStudioWindow
{
  public:
    static void Draw(bool* open);
    // Releases the preview texture and descriptor; call before EditorDescriptors::Shutdown.
    static void Shutdown();
};
