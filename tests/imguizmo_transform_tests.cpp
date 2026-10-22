#include <DirectXMath.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include "rendering/scenedepth.h"

// Exercise the actual ray and drag code, including its private interaction state.
#include "../External/ImGuizmo/ImGuizmo.cpp"

using namespace DirectX;

static void CheckFinite(const ImGuizmo::vec_t& value)
{
    assert(std::isfinite(value.x));
    assert(std::isfinite(value.y));
    assert(std::isfinite(value.z));
    assert(std::isfinite(value.w));
}

static void CheckDrag(const XMMATRIX& projection, const XMMATRIX& view, ImGuizmo::MODE mode,
                      ImGuizmo::OPERATION operation)
{
    using namespace ImGuizmo;
    auto& context = gContext;
    auto& io = ImGui::GetIO();
    XMFLOAT4X4 viewMatrix;
    XMFLOAT4X4 projectionMatrix;
    XMStoreFloat4x4(&viewMatrix, view);
    XMStoreFloat4x4(&projectionMatrix, projection);

    // Keep the model in front of either camera; nonuniform scale matches placed assets.
    XMFLOAT4X4 modelMatrix;
    const XMMATRIX model = XMMatrixScaling(0.5f, 2.0f, 3.0f) * XMMatrixRotationRollPitchYaw(0.2f, 0.3f, 0.1f) *
                           XMMatrixTranslation(0.0f, 0.0f, 5.0f) * XMMatrixInverse(nullptr, view);
    XMStoreFloat4x4(&modelMatrix, model);
    context.mbUsing = false;
    io.MousePos = ImVec2(440.0f, 300.0f);
    ComputeContext(&viewMatrix._11, &projectionMatrix._11, &modelMatrix._11, mode);
    CheckFinite(context.mRayOrigin);
    CheckFinite(context.mRayVector);
    assert(std::abs(context.mRayVector.Length() - 1.0f) < 1e-5f);
    assert(Dot(context.mRayVector, context.mCameraDir) > 0.0f);

    context.mTranslationPlan = BuildPlan(context.mModel.v.position, -context.mCameraDir);
    const float distance = IntersectRayPlane(context.mRayOrigin, context.mRayVector, context.mTranslationPlan);
    const vec_t hit = context.mRayOrigin + context.mRayVector * distance;
    CheckFinite(hit);
    context.mRelativeOrigin = (hit - context.mModel.v.position) * (1.0f / context.mScreenFactor);
    context.mRotationVectorSource = Normalized(hit - context.mModel.v.position);
    context.mRotationAngleOrigin = ComputeAngleOnPlan();
    context.mTranslationLastDelta.Set(0.0f);
    context.mbUsing = true;
    context.mEditingID = context.GetCurrentID();
    context.mCurrentHandleType = MT_MOVE_SCREEN;
    if (operation == ROTATE)
    {
        context.mCurrentHandleType = MT_ROTATE_SCREEN;
    }
    else if (operation == SCALE)
    {
        context.mCurrentHandleType = MT_SCALE_XYZ;
        context.mScale.Set(1.0f);
        context.mScaleLast.Set(1.0f);
        context.mScaleValueOrigin = context.mModelScaleOrigin;
        context.mSaveMousePosx = io.MousePos.x;
    }
    io.MouseDown[0] = true;
    io.MousePos = ImVec2(470.0f, 330.0f);
    ComputeContext(&viewMatrix._11, &projectionMatrix._11, &modelMatrix._11, mode);
    // The headless fixture supplies a drag directly without a platform window hover event.
    context.mbMouseOver = true;
    MOVETYPE type = MT_NONE;
    bool changed = false;
    if (operation == ROTATE)
    {
        changed = HandleRotation(&modelMatrix._11, nullptr, ROTATE, type, nullptr);
    }
    else if (operation == SCALE)
    {
        changed = HandleScale(&modelMatrix._11, nullptr, SCALE, type, nullptr);
    }
    else
    {
        changed = HandleTranslation(&modelMatrix._11, nullptr, TRANSLATE, type, nullptr);
    }
    assert(changed);
    const matrix_t& result = reinterpret_cast<const matrix_t&>(modelMatrix);
    for (float value : result.m16)
    {
        assert(std::isfinite(value));
    }
    float translation[3];
    float rotation[3];
    float scale[3];
    DecomposeMatrixToComponents(result.m16, translation, rotation, scale);
    for (int axis = 0; axis < 3; ++axis)
    {
        assert(std::isfinite(translation[axis]));
        assert(std::isfinite(rotation[axis]));
        assert(std::isfinite(scale[axis]));
    }
    float scaleFactor = 1.0f;
    if (operation == SCALE)
    {
        scaleFactor = 1.3f;
    }
    assert(std::abs(scale[0] - 0.5f * scaleFactor) < 1e-4f);
    assert(std::abs(scale[1] - 2.0f * scaleFactor) < 1e-4f);
    assert(std::abs(scale[2] - 3.0f * scaleFactor) < 1e-4f);
    context.mbUsing = false;
    io.MouseDown[0] = false;
}

int main()
{
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(800, 600);
    unsigned char* pixels;
    int width;
    int height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    ImGui::NewFrame();
    ImGui::Begin("Gizmo regression");
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(0, 0, 800, 600);

    int cases = 0;
    for (float nearClip : {0.01f, 0.1f, 1.0f})
    {
        for (const XMMATRIX& projection : {SceneDepth::InfiniteReverseZPerspectiveLH(XM_PIDIV4, 4.0f / 3.0f, nearClip),
                                           XMMatrixPerspectiveFovLH(XM_PIDIV4, 4.0f / 3.0f, 10000.0f, nearClip),
                                           XMMatrixPerspectiveFovLH(XM_PIDIV4, 4.0f / 3.0f, nearClip, 10000.0f)})
        {
            for (const XMMATRIX& view : {XMMatrixIdentity(), XMMatrixLookAtLH(XMVectorSet(3, 2, -7, 1), XMVectorZero(),
                                                                              XMVectorSet(0, 1, 0, 0))})
            {
                for (ImGuizmo::MODE mode : {ImGuizmo::LOCAL, ImGuizmo::WORLD})
                {
                    for (ImGuizmo::OPERATION operation : {ImGuizmo::TRANSLATE, ImGuizmo::ROTATE, ImGuizmo::SCALE})
                    {
                        ImGuizmo::MODE effectiveMode = mode;
                        if (operation == ImGuizmo::SCALE)
                        {
                            effectiveMode = ImGuizmo::LOCAL;
                        }
                        CheckDrag(projection, view, effectiveMode, operation);
                        ++cases;
                    }
                }
            }
        }
    }
    ImGui::End();
    ImGui::EndFrame();
    ImGui::DestroyContext();
    std::printf("ImGuizmo finite ray/translation/rotation/scale tests: PASS (%d cases)\n", cases);
}
