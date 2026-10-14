#pragma once
#include <cstdint>
class MaterialValidation
{
public:
    static bool Enabled();
    static void BeforeFrame(uint64_t frame);
    static void AfterFrame(uint64_t frame);
    static bool Finish();
    static void BeforeEditorFrame();
    static void InjectDragPayload();
    static void CaptureDropTarget(const char* name, float left, float top, float right, float bottom);
};
