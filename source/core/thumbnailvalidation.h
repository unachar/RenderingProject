#pragma once
#include <cstdint>

class ThumbnailValidation
{
public:
    static bool Enabled();
    static void BeforeFrame(uint64_t frame);
    static void AfterFrame(uint64_t frame);
    static bool Finish();
    static void BeforeEditorFrame();
    static void CaptureAssetCell(const char* fileName, float x, float y, bool selected);
};

