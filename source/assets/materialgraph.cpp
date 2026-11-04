#include "pch.h"
#include "materialgraph.h"
#include "materialgraphmath.h"
#include "materialgraphnodes.h"
#include <json/json.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <cstdlib>
#include <limits>

namespace
{
    using Type = MaterialGraphType;
    using Kind = MaterialNodeKind;
    constexpr size_t MaxNodes = 2048;
    constexpr size_t MaxLinks = 8192;
    constexpr uint64_t MaxIdentity = (uint64_t{1} << 53) - 1;
    const std::vector<MaterialNodeDefinition> NodeDefinitions = {
        // 定数：入力ピンは成分ごと（未接続 = ノード上の値 node.Value[i]。接続 = 入力の値）
        {Kind::Constant, "Constant", Type::Scalar, {{"Value", Type::Scalar}}},
        {Kind::ScalarParameter, "Scalar Parameter", Type::Scalar, {}},
        {Kind::VectorParameter, "Vector Parameter", Type::Vector3, {}},
        {Kind::ColorParameter, "Color Parameter", Type::Vector4, {}},
        {Kind::TextureSample, "Texture Sample", Type::Vector4, {{"UV", Type::Vector2}}},
        {Kind::Add, "Add", Type::Numeric, {{"A", Type::Numeric}, {"B", Type::Numeric}}},
        {Kind::Subtract, "Subtract", Type::Numeric, {{"A", Type::Numeric}, {"B", Type::Numeric}}},
        {Kind::Multiply,
         "Multiply",
         Type::Numeric,
         {{"A", Type::Numeric, {1, 1, 1, 1}}, {"B", Type::Numeric, {1, 1, 1, 1}}}},
        {Kind::Divide, "Divide", Type::Numeric, {{"A", Type::Numeric}, {"B", Type::Numeric, {1, 1, 1, 1}}}},
        {Kind::Lerp,
         "Lerp",
         Type::Numeric,
         {{"A", Type::Numeric}, {"B", Type::Numeric, {1, 1, 1, 1}}, {"Alpha", Type::Scalar, {0.5f}}}},
        {Kind::Clamp,
         "Clamp",
         Type::Numeric,
         {{"Value", Type::Numeric}, {"Min", Type::Numeric}, {"Max", Type::Numeric, {1, 1, 1, 1}}}},
        {Kind::Smoothstep,
         "Smoothstep",
         Type::Numeric,
         {{"Min", Type::Numeric}, {"Max", Type::Numeric, {1, 1, 1, 1}}, {"Value", Type::Numeric}}},
        {Kind::Normalize, "Normalize", Type::Vector3, {{"Vector", Type::Vector3, {0, 0, 1}}}},
        {Kind::Dot, "Dot Product", Type::Scalar, {{"A", Type::Vector3, {0, 0, 1}}, {"B", Type::Vector3, {0, 0, 1}}}},
        {Kind::Fresnel, "Fresnel", Type::Scalar, {{"Exponent", Type::Scalar, {5}}}},
        {Kind::UV, "UV", Type::Vector2, {}},
        {Kind::WorldPosition, "World Position", Type::Vector3, {}},
        {Kind::WorldNormal, "World Normal", Type::Vector3, {}},
        {Kind::CameraVector, "Camera Vector", Type::Vector3, {}},
        {Kind::Time, "Time", Type::Scalar, {}},
        {Kind::Output,
         "Material Output",
         Type::Numeric,
         {{"Base Color", Type::Vector4, {1, 1, 1, 1}},
          {"Metallic", Type::Scalar},
          {"Roughness", Type::Scalar, {0.5f}},
          {"Normal", Type::Vector3, {0, 0, 1}},
          {"Emissive", Type::Vector3},
          {"Opacity", Type::Scalar, {1}},
          {"Opacity Mask", Type::Scalar, {1}},
          {"AO", Type::Scalar, {1}},
          // ---- エンジンのマテリアルパラメーター（2026-11 追加。未接続ならマテリアルアセットの値を保持する）
          {"Height", Type::Scalar, {1}},
          {"Normal Intensity", Type::Scalar, {1}},
          {"Parallax Depth", Type::Scalar, {0.05f}},
          {"Parallax Steps", Type::Scalar, {32}},
          {"IOR", Type::Scalar, {1.5f}},
          {"Transmission", Type::Scalar},
          {"Transmission Roughness", Type::Scalar, {0.02f}},
          {"Thickness", Type::Scalar, {0.1f}},
          {"Absorption", Type::Vector3},
          {"Shading Model Strength", Type::Scalar, {1}},
          // ---- 2026-11 追加：描画時評価（GPU）だけが使うピン。既存のピン番号 / 順序は変えず末尾へ追加
          {"Toon Shade", Type::Vector4, {0, 0, 0, 0}},
          {"Toon Rim", Type::Vector4, {0, 0, 0, 0}},
          {"Toon Hair Highlight", Type::Vector4, {0, 0, 0, 0}},
          {"Toon Face Shadow", Type::Vector4, {0, 0, 0, 0}},
          {"Mekakure Mask", Type::Scalar, {1}},
          {"Clear Coat", Type::Vector4, {0, 0, 0, 0}},
          {"Subsurface Tint", Type::Vector3, {0, 0, 0, 0}},
          {"Anisotropy Direction", Type::Vector4, {0, 0, 0, 0}}}},
        {Kind::Group, "Comment Group", Type::Numeric, {}},
        {Kind::Constant2Vector, "Constant2Vector", Type::Vector2, {{"R", Type::Scalar}, {"G", Type::Scalar}}},
        {Kind::Constant3Vector,
         "Constant3Vector",
         Type::Vector3,
         {{"R", Type::Scalar}, {"G", Type::Scalar}, {"B", Type::Scalar}}},
        {Kind::Constant4Vector,
         "Constant4Vector",
         Type::Vector4,
         {{"R", Type::Scalar}, {"G", Type::Scalar}, {"B", Type::Scalar}, {"A", Type::Scalar, {1, 1, 1, 1}}}},
        {Kind::TextureCoordinate, "TexCoord", Type::Vector2, {}},
        {Kind::Panner,
         "Panner",
         Type::Vector2,
         {{"Coordinate", Type::Vector2}, {"Time", Type::Scalar}, {"Speed", Type::Vector2, {0.1f, 0.0f}}}},
        {Kind::Rotator,
         "Rotator",
         Type::Vector2,
         {{"Coordinate", Type::Vector2},
          {"Time", Type::Scalar},
          {"Speed", Type::Scalar, {0.25f}},
          {"Center", Type::Vector2, {0.5f, 0.5f}}}},
        {Kind::Abs, "Abs", Type::Numeric, {{"Input", Type::Numeric}}},
        {Kind::Ceil, "Ceil", Type::Numeric, {{"Input", Type::Numeric}}},
        {Kind::Floor, "Floor", Type::Numeric, {{"Input", Type::Numeric}}},
        {Kind::Frac, "Frac", Type::Numeric, {{"Input", Type::Numeric}}},
        {Kind::Round, "Round", Type::Numeric, {{"Input", Type::Numeric}}},
        {Kind::Truncate, "Truncate", Type::Numeric, {{"Input", Type::Numeric}}},
        {Kind::Sign, "Sign", Type::Numeric, {{"Input", Type::Numeric}}},
        {Kind::OneMinus, "OneMinus", Type::Numeric, {{"Input", Type::Numeric}}},
        {Kind::Saturate, "Saturate", Type::Numeric, {{"Input", Type::Numeric}}},
        {Kind::SquareRoot, "SquareRoot", Type::Numeric, {{"Input", Type::Numeric, {1, 1, 1, 1}}}},
        {Kind::Sine, "Sine", Type::Numeric, {{"Input", Type::Numeric}}},
        {Kind::Cosine, "Cosine", Type::Numeric, {{"Input", Type::Numeric}}},
        {Kind::Tangent, "Tangent", Type::Numeric, {{"Input", Type::Numeric}}},
        {Kind::Arcsine, "Arcsine", Type::Numeric, {{"Input", Type::Numeric}}},
        {Kind::Arccosine, "Arccosine", Type::Numeric, {{"Input", Type::Numeric}}},
        {Kind::Arctangent, "Arctangent", Type::Numeric, {{"Input", Type::Numeric}}},
        {Kind::Exponential, "Exponential", Type::Numeric, {{"Input", Type::Numeric}}},
        {Kind::Logarithm2, "Logarithm2", Type::Numeric, {{"Input", Type::Numeric, {1, 1, 1, 1}}}},
        {Kind::DDX, "DDX", Type::Numeric, {{"Input", Type::Numeric}}},
        {Kind::DDY, "DDY", Type::Numeric, {{"Input", Type::Numeric}}},
        {Kind::Power, "Power", Type::Numeric, {{"Base", Type::Numeric, {1, 1, 1, 1}}, {"Exp", Type::Scalar, {2}}}},
        {Kind::Min, "Min", Type::Numeric, {{"A", Type::Numeric}, {"B", Type::Numeric}}},
        {Kind::Max, "Max", Type::Numeric, {{"A", Type::Numeric}, {"B", Type::Numeric}}},
        {Kind::Fmod, "Fmod", Type::Numeric, {{"A", Type::Numeric}, {"B", Type::Numeric, {1, 1, 1, 1}}}},
        {Kind::Step, "Step", Type::Numeric, {{"Y", Type::Numeric, {0.5f, 0.5f, 0.5f, 0.5f}}, {"X", Type::Numeric}}},
        {Kind::Arctangent2, "Arctangent2", Type::Numeric, {{"Y", Type::Numeric}, {"X", Type::Numeric, {1, 1, 1, 1}}}},
        {Kind::ConstantBiasScale,
         "ConstantBiasScale",
         Type::Numeric,
         {{"Input", Type::Numeric}, {"Bias", Type::Scalar, {1}}, {"Scale", Type::Scalar, {0.5f}}}},
        {Kind::CrossProduct,
         "Cross Product",
         Type::Vector3,
         {{"A", Type::Vector3, {1, 0, 0}}, {"B", Type::Vector3, {0, 1, 0}}}},
        {Kind::Distance, "Distance", Type::Scalar, {{"A", Type::Vector3}, {"B", Type::Vector3}}},
        {Kind::Length, "Length", Type::Scalar, {{"Input", Type::Numeric}}},
        {Kind::AppendVector, "Append", Type::Numeric, {{"A", Type::Numeric}, {"B", Type::Numeric}}},
        {Kind::ComponentMask, "Component Mask", Type::Numeric, {{"Input", Type::Numeric}}},
        {Kind::ReflectionVector, "Reflection Vector", Type::Vector3, {}},
        {Kind::Desaturation,
         "Desaturation",
         Type::Vector3,
         {{"Input", Type::Vector3, {1, 1, 1}}, {"Fraction", Type::Scalar, {1}}}},
        {Kind::If,
         "If",
         Type::Numeric,
         {{"A", Type::Scalar},
          {"B", Type::Scalar},
          {"A > B", Type::Numeric, {1, 1, 1, 1}},
          {"A == B", Type::Numeric},
          {"A < B", Type::Numeric}}},
        {Kind::SphereMask,
         "Sphere Mask",
         Type::Scalar,
         {{"A", Type::Vector3}, {"B", Type::Vector3}, {"Radius", Type::Scalar, {0.5f}}, {"Hardness", Type::Scalar, {0.5f}}}},
        {Kind::CheapContrast, "Cheap Contrast", Type::Scalar, {{"In", Type::Scalar}, {"Contrast", Type::Scalar, {1}}}},
        {Kind::Noise, "Noise", Type::Scalar, {{"Position", Type::Vector3}, {"Scale", Type::Scalar, {4}}}},
        {Kind::Reroute, "Reroute", Type::Numeric, {{"In", Type::Numeric}}},
        // ---- 2026-11 追加（UE のノード / マテリアル関数）
        {Kind::StaticSwitch, "Static Switch", Type::Numeric, {{"True", Type::Numeric, {1, 1, 1, 1}}, {"False", Type::Numeric}}},
        {Kind::Pi, "Pi", Type::Scalar, {}},
        {Kind::RemapValueRange,
         "Remap Value Range",
         Type::Numeric,
         {{"Input", Type::Numeric},
          {"Input Low", Type::Scalar},
          {"Input High", Type::Scalar, {1}},
          {"Target Low", Type::Scalar},
          {"Target High", Type::Scalar, {1}}}},
        {Kind::Posterize, "Posterize", Type::Numeric, {{"Input", Type::Numeric}, {"Steps", Type::Scalar, {4}}}},
        {Kind::HueShift, "Hue Shift", Type::Vector3, {{"Color", Type::Vector3, {1, 0, 0}}, {"Shift", Type::Scalar}}},
        {Kind::Blend,
         "Blend",
         Type::Vector3,
         {{"Base", Type::Vector3, {0.5f, 0.5f, 0.5f}}, {"Blend", Type::Vector3, {0.5f, 0.5f, 0.5f}}, {"Opacity", Type::Scalar, {1}}}},
        {Kind::LinearGradient, "Linear Gradient", Type::Scalar, {{"UV", Type::Vector2}}},
        {Kind::RadialGradient,
         "Radial Gradient",
         Type::Scalar,
         {{"UV", Type::Vector2}, {"Center", Type::Vector2, {0.5f, 0.5f}}, {"Radius", Type::Scalar, {0.5f}}, {"Density", Type::Scalar, {2}}}},
        {Kind::Voronoi,
         "Voronoi",
         Type::Scalar,
         {{"UV", Type::Vector2}, {"Scale", Type::Scalar, {8}}, {"Jitter", Type::Scalar, {1}}}},
        {Kind::NormalFromHeight,
         "Normal From Height",
         Type::Vector3,
         {{"Height", Type::Scalar, {0.5f}}, {"Strength", Type::Scalar, {0.05f}}}},
        {Kind::BlendAngleCorrectedNormals,
         "Blend Angle Corrected Normals",
         Type::Vector3,
         {{"Base Normal", Type::Vector3, {0, 0, 1}}, {"Additional Normal", Type::Vector3, {0, 0, 1}}}},
        {Kind::FlattenNormal, "Flatten Normal", Type::Vector3, {{"Normal", Type::Vector3, {0, 0, 1}}, {"Flatness", Type::Scalar}}},
        {Kind::BumpOffset,
         "Bump Offset",
         Type::Vector2,
         {{"Height", Type::Scalar, {0.5f}},
          {"Height Ratio", Type::Scalar, {0.05f}},
          {"Reference Plane", Type::Scalar, {0.5f}},
          {"Coordinate", Type::Vector2}}},
        {Kind::RotateAboutAxis,
         "Rotate About Axis",
         Type::Vector3,
         {{"Rotation Axis", Type::Vector3, {0, 0, 1}},
          {"Rotation Angle", Type::Scalar},
          {"Pivot Point", Type::Vector3},
          {"Position", Type::Vector3}}},
        {Kind::BlackBody, "Black Body", Type::Vector3, {{"Temp", Type::Scalar, {6500}}}},
        // ---- プロシージャルスタジオのノイズ
        {Kind::ProceduralNoise,
         "Procedural Noise",
         Type::Scalar,
         {{"UV", Type::Vector2},
          {"Z", Type::Scalar},
          {"Frequency", Type::Scalar, {4}},
          {"Direction", Type::Scalar},
          {"Anisotropy", Type::Scalar, {4}},
          {"Kernel Frequency", Type::Scalar, {2}},
          {"Phase", Type::Scalar}}},
        {Kind::FractalNoise,
         "Fractal Noise",
         Type::Scalar,
         {{"UV", Type::Vector2},
          {"Z", Type::Scalar},
          {"Frequency", Type::Scalar, {4}},
          {"Octaves", Type::Scalar, {5}},
          {"Lacunarity", Type::Scalar, {2}},
          {"Gain", Type::Scalar, {0.5f}},
          {"Warp Strength", Type::Scalar, {0.6f}},
          {"Ridge Offset", Type::Scalar, {1}}}},
        {Kind::NoiseCombine,
         "Noise Combine",
         Type::Scalar,
         {{"A", Type::Scalar, {0.5f}}, {"B", Type::Scalar, {0.5f}}, {"Factor", Type::Scalar, {0.5f}}}},
        {Kind::ConvertToNormalHeight,
         "Convert To Normal / Height",
         Type::Numeric, // 出力の切り替えで Scalar（Height）/ Vector3（Normal）
         {{"UV", Type::Vector2},
          {"Contrast", Type::Scalar, {1}},
          {"Bias", Type::Scalar, {0}},
          {"Normal Strength", Type::Scalar, {8}},
          {"Radius (UV)", Type::Scalar, {0.001f}}}},
        {Kind::ConvertToHeight,
         "Convert To Height",
         Type::Scalar,
         {{"UV", Type::Vector2}, {"Contrast", Type::Scalar, {1}}, {"Bias", Type::Scalar, {0}}}},
        {Kind::ConvertToNormal,
         "Convert To Normal",
         Type::Vector3,
         {{"UV", Type::Vector2},
          {"Contrast", Type::Scalar, {1}},
          {"Bias", Type::Scalar, {0}},
          {"Normal Strength", Type::Scalar, {8}},
          {"Radius (UV)", Type::Scalar, {0.001f}}}},
        {Kind::BreakVector, "Break Vector", Type::Numeric, {{"Vector", Type::Numeric}}},
        {Kind::ConvertToTexture, "Convert To Texture", Type::Vector4, {{"Value", Type::Numeric}, {"UV", Type::Vector2}}},
        // ---- ソリッドテクスチャ（入力の並びは Solid Texture / Solid Pattern で共通。色は Solid Texture のみ）
        {Kind::SolidTexture,
         "Solid Texture",
         Type::Vector3,
         {{"Position", Type::Vector3},
          {"Frequency", Type::Scalar, {3}},
          {"Octaves", Type::Scalar, {5}},
          {"Lacunarity", Type::Scalar, {2}},
          {"Gain", Type::Scalar, {0.5f}},
          {"Warp Strength", Type::Scalar, {0.6f}},
          {"Ridge Offset", Type::Scalar, {1}},
          {"Turbulence", Type::Scalar, {4}},
          {"Ring Count", Type::Scalar, {8}},
          {"Coverage", Type::Scalar, {0.5f}},
          {"Softness", Type::Scalar, {0.25f}},
          {"Contrast", Type::Scalar, {1}},
          {"Bias", Type::Scalar, {0}},
          {"Color A", Type::Vector3, {0.12f, 0.12f, 0.14f, 0}},
          {"Color B", Type::Vector3, {0.92f, 0.91f, 0.88f, 0}},
          {"Color C", Type::Vector3, {0.60f, 0.62f, 0.66f, 0}}}},
        {Kind::SolidPattern,
         "Solid Pattern",
         Type::Scalar,
         {{"Position", Type::Vector3},
          {"Frequency", Type::Scalar, {3}},
          {"Octaves", Type::Scalar, {5}},
          {"Lacunarity", Type::Scalar, {2}},
          {"Gain", Type::Scalar, {0.5f}},
          {"Warp Strength", Type::Scalar, {0.6f}},
          {"Ridge Offset", Type::Scalar, {1}},
          {"Turbulence", Type::Scalar, {4}},
          {"Ring Count", Type::Scalar, {8}},
          {"Coverage", Type::Scalar, {0.5f}},
          {"Softness", Type::Scalar, {0.25f}},
          {"Contrast", Type::Scalar, {1}},
          {"Bias", Type::Scalar, {0}}}},
        {Kind::SolidCoordinate,
         "Solid Coordinate",
         Type::Vector3,
         {{"Position", Type::Vector3}, {"Scale", Type::Scalar, {1}}, {"Offset", Type::Vector3}}},
        {Kind::ConvertToSolid,
         "Convert To Solid",
         Type::Vector4,
         {{"Value", Type::Numeric},
          {"Position", Type::Vector3},
          {"Scale", Type::Scalar, {1}},
          {"Octaves", Type::Scalar, {3}},
          {"Vertical Variation", Type::Scalar, {0.5f}},
          {"Sharpness", Type::Scalar, {4}},
          {"Randomness", Type::Scalar, {1}}}},
        {Kind::ThreeColorRamp,
         "Three Color Ramp",
         Type::Vector3,
         {{"T", Type::Scalar, {0.5f}},
          {"Color A", Type::Vector3, {0.05f, 0.07f, 0.12f, 0}},
          {"Color B", Type::Vector3, {0.85f, 0.82f, 0.76f, 0}},
          {"Color C", Type::Vector3, {0.45f, 0.28f, 0.12f, 0}}}},
        // ---- マテリアルシェーディング用の便利ノード
        {Kind::LocalPosition, "Local Position", Type::Vector3, {}},
        // ---- オブジェクト / カメラの情報
        {Kind::ObjectPosition, "Object Position", Type::Vector3, {}},
        {Kind::ObjectScale, "Object Scale", Type::Vector3, {}},
        {Kind::ObjectOrientation, "Object Orientation", Type::Vector3, {}},
        {Kind::ObjectRadius, "Object Radius", Type::Scalar, {}},
        {Kind::ObjectBounds, "Object Bounds", Type::Vector3, {}},
        {Kind::BoundingBoxUVW, "Bounding Box UVW", Type::Vector3, {}},
        {Kind::PerInstanceRandom, "Per Instance Random", Type::Scalar, {}},
        {Kind::TransformVector, "Transform Vector", Type::Vector3, {{"Vector", Type::Vector3, {0, 1, 0, 0}}}},
        {Kind::TransformPosition, "Transform Position", Type::Vector3, {{"Position", Type::Vector3}}},
        {Kind::CameraPosition, "Camera Position", Type::Vector3, {}},
        {Kind::PixelDepth, "Pixel Depth", Type::Scalar, {}},
        {Kind::ScreenPosition, "Screen Position", Type::Vector2, {}},
        {Kind::DitherOpacity, "Dither Opacity", Type::Scalar, {{"Opacity", Type::Scalar, {0.5f}}}},
        {Kind::DistanceFade, "Distance Fade", Type::Scalar, {{"Start", Type::Scalar, {5}}, {"End", Type::Scalar, {20}}}},
        {Kind::HeightGradient, "Height Gradient", Type::Scalar, {{"Start", Type::Scalar, {0}}, {"End", Type::Scalar, {1}}}},
        {Kind::VertexColor, "Vertex Color", Type::Vector4, {}},
        {Kind::CameraDistance, "Camera Distance", Type::Scalar, {}},
        {Kind::SlopeMask,
         "Slope Mask",
         Type::Scalar,
         {{"Up", Type::Vector3, {0, 1, 0, 0}}, {"Threshold", Type::Scalar, {0.7f}}, {"Softness", Type::Scalar, {0.1f}}}},
        {Kind::HeightLerp,
         "Height Lerp",
         Type::Numeric,
         {{"A", Type::Numeric},
          {"B", Type::Numeric, {1, 1, 1, 1}},
          {"Height", Type::Scalar, {0.5f}},
          {"Transition Phase", Type::Scalar, {0.5f}},
          {"Contrast", Type::Scalar, {0.5f}}}},
        {Kind::ColorAdjust,
         "Color Adjust",
         Type::Vector3,
         {{"Color", Type::Vector3, {1, 1, 1, 0}},
          {"Brightness", Type::Scalar, {1}},
          {"Contrast", Type::Scalar, {1}},
          {"Saturation", Type::Scalar, {1}}}},
        {Kind::TriplanarTexture,
         "Triplanar Texture",
         Type::Vector4,
         {{"Position", Type::Vector3}, {"Scale", Type::Scalar, {1}}, {"Sharpness", Type::Scalar, {4}}}},
        {Kind::NoiseLayers,
         "Noise Layers",
         Type::Scalar,
         {{"Layer 1", Type::Scalar},
          {"Weight 1", Type::Scalar, {1}},
          {"Layer 2", Type::Scalar},
          {"Weight 2", Type::Scalar, {1}},
          {"Layer 3", Type::Scalar},
          {"Weight 3", Type::Scalar, {1}},
          {"Layer 4", Type::Scalar},
          {"Weight 4", Type::Scalar, {1}}}},
        // ================= 2026-11 追加ノード（materialgraphnodes.h にパラメーターの意味） =================
        // ---- A. トゥーン / キャラクター（出力は V4：rgb = 色、a = 係数。ライトごとの評価は ToonNodeLighting.hlsli）
        {Kind::ToonLightRamp,
         "Toon Light Ramp",
         Type::Vector4,
         {{"Albedo", Type::Vector3, {1, 1, 1, 0}},
          {"Normal", Type::Vector3, {0, 0, 1, 0}},
          {"Light Dir", Type::Vector3, {0.40824829f, 0.81649658f, 0.40824829f, 0}},
          {"Steps", Type::Scalar, {3}},
          {"Threshold", Type::Scalar, {0.5f}},
          {"Smoothness", Type::Scalar, {0.04f}},
          {"Strength", Type::Scalar, {1}},
          {"Shade Color 1", Type::Vector3, {0.35f, 0.38f, 0.55f, 0}},
          {"Shade Color 2", Type::Vector3, {0.72f, 0.74f, 0.88f, 0}},
          {"Shade Color 3", Type::Vector3, {1, 1, 1, 0}},
          {"Mask", Type::Scalar, {1}}}},
        {Kind::FaceSdfShadow,
         "Face SDF Shadow",
         Type::Scalar,
         {{"UV", Type::Vector2},
          {"Light Dir", Type::Vector3, {0.40824829f, 0.81649658f, 0.40824829f, 0}},
          {"Face Forward", Type::Vector3, {0, 0, 1, 0}},
          {"Face Right", Type::Vector3, {1, 0, 0, 0}},
          {"Threshold", Type::Scalar, {0.5f}},
          {"Smoothness", Type::Scalar, {0.03f}},
          {"Flip", Type::Scalar, {0}},
          {"Shadow Color", Type::Vector3, {0.72f, 0.62f, 0.72f, 0}},
          {"Strength", Type::Scalar, {1}}}},
        {Kind::HairAnisotropicHighlight,
         "Hair Anisotropic Highlight",
         Type::Vector4,
         {{"Tangent", Type::Vector3, {1, 0, 0, 0}},
          {"Normal", Type::Vector3, {0, 0, 1, 0}},
          {"View Dir", Type::Vector3, {0, 0, 1, 0}},
          {"Light Dir", Type::Vector3, {0.40824829f, 0.81649658f, 0.40824829f, 0}},
          {"Anisotropy", Type::Scalar, {0.7f}},
          {"Roughness", Type::Scalar, {0.35f}},
          {"Highlight Width", Type::Scalar, {0.12f}},
          {"Highlight Shift", Type::Scalar, {0}},
          {"Highlight Strength", Type::Scalar, {1}},
          {"Mask", Type::Scalar, {1}},
          {"Color", Type::Vector3, {1, 1, 1, 0}}}},
        {Kind::DirectionalRimLight,
         "Directional Rim Light",
         Type::Vector4,
         {{"Normal", Type::Vector3, {0, 0, 1, 0}},
          {"View Dir", Type::Vector3, {0, 0, 1, 0}},
          {"Light Dir", Type::Vector3, {0.40824829f, 0.81649658f, 0.40824829f, 0}},
          {"Rim Color", Type::Vector3, {1, 1, 1, 0}},
          {"Rim Width", Type::Scalar, {0.35f}},
          {"Rim Power", Type::Scalar, {2}},
          {"Rim Strength", Type::Scalar, {1}},
          {"Directional Mask", Type::Scalar, {1}}}},
        {Kind::MatCap,
         "MatCap",
         Type::Vector3,
         {{"View Normal", Type::Vector3, {0, 0, 1, 0}},
          {"Rotation", Type::Scalar, {0}},
          {"Strength", Type::Scalar, {1}},
          {"Base", Type::Vector3, {1, 1, 1, 0}}}},
        {Kind::MekakureMask,
         "Mekakure Mask",
         Type::Scalar,
         {{"Hair Mask", Type::Scalar, {1}},
          {"Eye Mask", Type::Scalar, {1}},
          {"Opacity", Type::Scalar, {0.6f}},
          {"Fade Width", Type::Scalar, {0.25f}},
          {"Camera Dependence", Type::Scalar, {1}},
          {"Normal", Type::Vector3, {0, 0, 1, 0}},
          {"View Dir", Type::Vector3, {0, 0, 1, 0}}}},
        {Kind::MultiColorRamp, "Multi Color Ramp", Type::Vector3, {{"T", Type::Scalar, {0.5f}}}},
        {Kind::EyeHighlight,
         "Eye Highlight",
         Type::Vector4,
         {{"UV", Type::Vector2},
          {"View Dir", Type::Vector3, {0, 0, 1, 0}},
          {"Light Dir", Type::Vector3, {0.40824829f, 0.81649658f, 0.40824829f, 0}},
          {"Center", Type::Vector2, {0.55f, 0.4f}},
          {"Highlight Size", Type::Scalar, {0.12f}},
          {"Highlight Shape", Type::Scalar, {0}},
          {"Highlight Strength", Type::Scalar, {1}},
          {"Mask", Type::Scalar, {1}},
          {"Color", Type::Vector3, {1, 1, 1, 0}},
          {"Light Shift", Type::Scalar, {0.12f}}}},
        {Kind::MaterialFunctionCall,
         "Material Function Call",
         Type::Numeric,
         {{"In 0", Type::Numeric}, {"In 1", Type::Numeric}, {"In 2", Type::Numeric}, {"In 3", Type::Numeric},
          {"In 4", Type::Numeric}, {"In 5", Type::Numeric}, {"In 6", Type::Numeric}, {"In 7", Type::Numeric}}},
        {Kind::TextureParameter2D, "Texture Parameter 2D", Type::Vector4, {{"UV", Type::Vector2}}},
        // ---- B. 汎用 / PBR
        {Kind::TwoSidedSign, "Two Sided Sign", Type::Scalar, {}},
        {Kind::FlipbookUV,
         "Flipbook UV",
         Type::Vector2,
         {{"UV", Type::Vector2},
          {"Rows", Type::Scalar, {4}},
          {"Columns", Type::Scalar, {4}},
          {"Frame", Type::Scalar, {0}},
          {"FPS", Type::Scalar, {12}},
          {"Time", Type::Scalar}}},
        {Kind::FlowMap,
         "Flow Map",
         Type::Vector2,
         {{"UV", Type::Vector2},
          {"Time", Type::Scalar},
          {"Speed", Type::Scalar, {0.25f}},
          {"Strength", Type::Scalar, {0.1f}},
          {"Flow", Type::Vector2, {0, 0, 0, 0}}}},
        {Kind::RgbCurves, "RGB Curves", Type::Vector3, {{"Color", Type::Vector3, {0.5f, 0.5f, 0.5f, 0}}, {"Strength", Type::Scalar, {1}}}},
        {Kind::HsvAdjust,
         "HSV Adjust",
         Type::Vector3,
         {{"Color", Type::Vector3, {0.8f, 0.4f, 0.2f, 0}}, {"Hue", Type::Scalar, {0}}, {"Saturation", Type::Scalar, {1}}, {"Value", Type::Scalar, {1}}}},
        {Kind::PackedTextureChannels, "Packed Texture Channels", Type::Vector4, {{"UV", Type::Vector2}}},
        {Kind::DetailNormalBlend,
         "Detail Normal Blend",
         Type::Vector3,
         {{"Base Normal", Type::Vector3, {0, 0, 1, 0}},
          {"Detail Normal", Type::Vector3, {0, 0, 1, 0}},
          {"Intensity", Type::Scalar, {1}},
          {"Mask", Type::Scalar, {1}}}},
        {Kind::TriplanarNormalCorrected,
         "Triplanar Normal Corrected",
         Type::Vector3,
         {{"Position", Type::Vector3},
          {"World Normal", Type::Vector3, {0, 0, 1, 0}},
          {"Scale", Type::Scalar, {1}},
          {"Sharpness", Type::Scalar, {4}},
          {"Intensity", Type::Scalar, {1}}}},
        {Kind::CurvatureMask,
         "Curvature Mask",
         Type::Scalar,
         {{"Curvature", Type::Scalar, {0}}, {"Threshold", Type::Scalar, {0.1f}}, {"Softness", Type::Scalar, {0.1f}}}},
        {Kind::MaterialLayerBlend,
         "Material Layer Blend",
         Type::Vector3,
         {{"A Base Color", Type::Vector3, {0.5f, 0.5f, 0.5f, 0}}, {"A Metallic", Type::Scalar, {0}}, {"A Roughness", Type::Scalar, {0.5f}},
          {"A Normal", Type::Vector3, {0, 0, 1, 0}}, {"A AO", Type::Scalar, {1}}, {"A Emissive", Type::Vector3, {0, 0, 0, 0}},
          {"A Opacity", Type::Scalar, {1}}, {"A Height", Type::Scalar, {0.5f}},
          {"B Base Color", Type::Vector3, {0.5f, 0.5f, 0.5f, 0}}, {"B Metallic", Type::Scalar, {0}}, {"B Roughness", Type::Scalar, {0.5f}},
          {"B Normal", Type::Vector3, {0, 0, 1, 0}}, {"B AO", Type::Scalar, {1}}, {"B Emissive", Type::Vector3, {0, 0, 0, 0}},
          {"B Opacity", Type::Scalar, {1}}, {"B Height", Type::Scalar, {0.5f}},
          {"Blend Mask", Type::Scalar, {0.5f}}}},
        {Kind::ThinFilmInterference,
         "Thin Film Interference",
         Type::Vector3,
         {{"Film Thickness", Type::Scalar, {420}},
          {"IOR", Type::Scalar, {1.33f}},
          {"View Angle", Type::Scalar, {1}},
          {"Strength", Type::Scalar, {1}}}},
        {Kind::AnisotropyDirection,
         "Anisotropy Direction",
         Type::Vector4,
         {{"Tangent", Type::Vector3, {1, 0, 0, 0}},
          {"Direction", Type::Vector2, {1, 0, 0, 0}},
          {"Anisotropy", Type::Scalar, {0.5f}},
          {"Rotation", Type::Scalar, {0}},
          {"Normal", Type::Vector3, {0, 0, 1, 0}}}},
        {Kind::ClearCoatControl,
         "Clear Coat Control",
         Type::Vector4,
         {{"Strength", Type::Scalar, {1}}, {"Roughness", Type::Scalar, {0.05f}}, {"Mask", Type::Scalar, {1}}}},
        {Kind::SubsurfaceColor,
         "Subsurface Color",
         Type::Vector3,
         {{"Color", Type::Vector3, {1, 0.4f, 0.3f, 0}},
          {"Radius", Type::Scalar, {0.5f}},
          {"Strength", Type::Scalar, {1}},
          {"Mask", Type::Scalar, {1}}}},
        {Kind::ThicknessMap,
         "Thickness Map",
         Type::Scalar,
         {{"UV", Type::Vector2},
          {"Scale", Type::Scalar, {1}},
          {"Bias", Type::Scalar, {0}},
          {"Min", Type::Scalar, {0}},
          {"Max", Type::Scalar, {1}},
          {"Transmission", Type::Scalar, {1}}}},
        // ---- C. エフェクト / ライブ
        {Kind::SceneDepth, "Scene Depth", Type::Scalar, {{"UV", Type::Vector2}}},
        {Kind::DepthFade, "Depth Fade", Type::Scalar, {{"Opacity", Type::Scalar, {1}}, {"Fade Distance", Type::Scalar, {100}}}},
        {Kind::SceneColor, "Scene Color", Type::Vector3, {{"UV", Type::Vector2}, {"Offset", Type::Vector2, {0, 0, 0, 0}}}},
        {Kind::CustomHlsl,
         "Custom HLSL",
         Type::Numeric,
         {{"a", Type::Numeric}, {"b", Type::Numeric}, {"c", Type::Numeric}, {"d", Type::Numeric}}},
        {Kind::DynamicSwitch,
         "Dynamic Switch",
         Type::Numeric,
         {{"Condition", Type::Scalar, {0}}, {"True", Type::Numeric, {1, 1, 1, 1}}, {"False", Type::Numeric}}},
        {Kind::AudioSpectrum,
         "Audio Spectrum",
         Type::Scalar,
         {{"Frequency Band", Type::Scalar, {4}}, {"Smoothing", Type::Scalar, {0.5f}}, {"Gain", Type::Scalar, {1}}}},
        {Kind::BeatPhase,
         "Beat Phase",
         Type::Vector4,
         {{"BPM", Type::Scalar, {120}}, {"Offset", Type::Scalar, {0}}, {"Subdivision", Type::Scalar, {1}}, {"Time", Type::Scalar}}},
        {Kind::CurveAtlas, "Curve Atlas", Type::Scalar, {{"Time", Type::Scalar, {0}}, {"Row", Type::Scalar, {0}}}},
        {Kind::FunctionInput, "Function Input", Type::Numeric, {}},
        {Kind::FunctionOutput, "Function Output", Type::Numeric, {{"Result", Type::Numeric}}}};

    // 固定の出力スロット（Break Vector と同じく SplitPins として常に存在する）
    using Slot = MaterialGraph::OutputSlot;
    const Slot LayerSlots[] = {{"Base Color", Type::Vector3}, {"Metallic", Type::Scalar}, {"Roughness", Type::Scalar},
                               {"Normal", Type::Vector3},     {"AO", Type::Scalar},       {"Emissive", Type::Vector3},
                               {"Opacity", Type::Scalar},     {"Height", Type::Scalar}};
    const Slot PackedSlots[] = {{"R", Type::Scalar}, {"G", Type::Scalar}, {"B", Type::Scalar}, {"A", Type::Scalar}};
    const Slot BeatSlots[] = {{"Phase", Type::Scalar}, {"Beat Index", Type::Scalar}, {"Beat Time", Type::Scalar}, {"Pulse", Type::Scalar}};
    const Slot FlowSlots[] = {{"UV A", Type::Vector2}, {"UV B", Type::Vector2}, {"Blend", Type::Scalar}};
    const Slot ThicknessSlots[] = {{"Thickness", Type::Scalar}, {"Transmission", Type::Scalar}};
    const Slot FunctionSlots[] = {{"Out 0", Type::Numeric}, {"Out 1", Type::Numeric}, {"Out 2", Type::Numeric}, {"Out 3", Type::Numeric}};

    int Dimensions(Type type)
    {
        return type == Type::Numeric ? 1 : static_cast<int>(type);
    }

    bool Compatible(Type source, Type target)
    {
        // Vector3 → Vector4 はアルファ 1 を足して受け付ける（UE の Base Color に float3 を繋ぐのと同じ）
        return target == Type::Numeric || source == Type::Scalar || source == target ||
               (static_cast<int>(target) > 1 && static_cast<int>(source) > static_cast<int>(target)) ||
               (source == Type::Vector3 && target == Type::Vector4);
    }
}

std::span<const MaterialNodeDefinition> MaterialGraph::Definitions()
{
    return NodeDefinitions;
}

const MaterialNodeDefinition& MaterialGraph::Definition(MaterialNodeKind kind)
{
    for (const auto& definition : NodeDefinitions)
    {
        if (definition.Kind == kind)
        {
            return definition;
        }
    }
    return NodeDefinitions.front();
}

MaterialGraph MaterialGraph::NewFunction()
{
    MaterialGraph graph;
    graph.Settings.IsFunction = true;
    return graph;
}

std::span<const MaterialGraph::OutputSlot> MaterialGraph::FixedOutputSlots(MaterialNodeKind kind)
{
    switch (kind)
    {
        case Kind::MaterialLayerBlend:
            return LayerSlots;
        case Kind::PackedTextureChannels:
            return PackedSlots;
        case Kind::BeatPhase:
            return BeatSlots;
        case Kind::FlowMap:
            return FlowSlots;
        case Kind::ThicknessMap:
            return ThicknessSlots;
        case Kind::MaterialFunctionCall:
            return FunctionSlots;
        default:
            return {};
    }
}

bool MaterialGraph::HasFixedOutputs(MaterialNodeKind kind)
{
    return kind == Kind::BreakVector || !FixedOutputSlots(kind).empty();
}

MaterialGraphType MaterialGraph::SlotType(const MaterialGraphNode& node, size_t slot) const
{
    if (node.Kind == Kind::MaterialFunctionCall)
    {
        // 関数のシグネチャ（呼び出しノードに保存してある）から。無ければ Numeric（接続先の型に合わせる）
        if (slot < 4 && node.Params.size() > 1 + slot && node.Params[0] > static_cast<float>(slot))
        {
            return static_cast<Type>(std::clamp(static_cast<int>(node.Params[1 + slot]), 1, 4));
        }
        return Type::Scalar;
    }
    const auto slots = FixedOutputSlots(node.Kind);
    return slot < slots.size() ? slots[slot].Type : Type::Scalar;
}

MaterialGraph MaterialGraph::FromMaterial(const MaterialAssetData& material)
{
    // マテリアルアセットの現在の値・テクスチャから、同じ見た目になるグラフを組み立てる
    // （テクスチャ / POM / 法線強度も含む。エンジン側はテクスチャを直接使うのでベイクは発生しない）
    MaterialGraph graph;
    const auto output = graph.AddNode(Kind::Output, 720, 40);
    std::string error;
    const auto pinOf = [&](MaterialOutputPin pin) {
        return graph.FindNode(output)->InputPins[static_cast<size_t>(pin)];
    };
    const auto add = [&](Kind kind, const char* name, std::array<float, 4> value, float x, float y) {
        const auto id = graph.AddNode(kind, x, y);
        if (auto* node = graph.FindNode(id))
        {
            if (name)
            {
                node->Name = name;
            }
            if (kind != Kind::TextureSample && kind != Kind::ComponentMask)
            {
                node->Value = value;
            }
        }
        return id;
    };
    const auto outputOf = [&](uint64_t node) {
        const auto* found = graph.FindNode(node);
        return found ? found->OutputPin : 0;
    };
    const auto inputOf = [&](uint64_t node, size_t index) {
        const auto* found = graph.FindNode(node);
        return found && index < found->InputPins.size() ? found->InputPins[index] : 0;
    };
    const auto link = [&](uint64_t sourceNode, uint64_t targetPin) {
        graph.Connect(outputOf(sourceNode), targetPin, error);
    };
    const auto texture = [&](MaterialTextureSlot slot, MaterialSamplerType sampler, const char* name, float y) -> uint64_t {
        const AssetHandle handle = material.Textures[static_cast<size_t>(slot)];
        if (handle == 0)
        {
            return 0;
        }
        const auto id = add(Kind::TextureSample, name, {}, 40, y);
        if (auto* node = graph.FindNode(id))
        {
            node->Texture = handle;
            node->Value = {static_cast<float>(sampler), 0, 0, 0};
        }
        return id;
    };
    const auto mask = [&](uint64_t source, int channel, float y) {
        const auto id = add(Kind::ComponentMask, nullptr, {}, 300, y);
        if (auto* node = graph.FindNode(id))
        {
            node->Value = {channel == 0 ? 1.0f : 0.0f, channel == 1 ? 1.0f : 0.0f, channel == 2 ? 1.0f : 0.0f, 0.0f};
        }
        graph.Connect(outputOf(source), inputOf(id, 0), error);
        return id;
    };
    const auto multiply = [&](uint64_t a, uint64_t b, float y) {
        const auto id = add(Kind::Multiply, nullptr, {}, 480, y);
        graph.Connect(outputOf(a), inputOf(id, 0), error);
        graph.Connect(outputOf(b), inputOf(id, 1), error);
        return id;
    };

    float y = 0.0f;
    // Base Color（テクスチャ × 色）
    {
        const auto base = texture(MaterialTextureSlot::BaseColor, MaterialSamplerType::Color, "Base Color Texture", y);
        const auto color = add(Kind::ColorParameter, "Base Color", material.BaseColor, base ? 300.0f : 40.0f, base ? y + 120 : y);
        link(base ? multiply(base, color, y + 40) : color, pinOf(MaterialOutputPin::BaseColor));
        y += base ? 260.0f : 150.0f;
    }
    // Metallic / Roughness（エンジンの MR テクスチャは R = Metallic, G = Roughness）
    {
        const auto mr = texture(MaterialTextureSlot::MetallicRoughness, MaterialSamplerType::Linear, "Metallic Roughness", y);
        const auto metallic = add(Kind::ScalarParameter, "Metallic", {material.Metallic}, mr ? 300.0f : 40.0f, y + (mr ? 180.0f : 0.0f));
        const auto roughness = add(Kind::ScalarParameter, "Roughness", {material.Roughness}, mr ? 300.0f : 40.0f, y + (mr ? 260.0f : 100.0f));
        if (mr)
        {
            link(multiply(mask(mr, 0, y), metallic, y), pinOf(MaterialOutputPin::Metallic));
            link(multiply(mask(mr, 1, y + 90), roughness, y + 120), pinOf(MaterialOutputPin::Roughness));
            y += 340.0f;
        }
        else
        {
            link(metallic, pinOf(MaterialOutputPin::Metallic));
            link(roughness, pinOf(MaterialOutputPin::Roughness));
            y += 200.0f;
        }
    }
    // Normal（法線マップ）と強さ
    if (const auto normal = texture(MaterialTextureSlot::Normal, MaterialSamplerType::Normal, "Normal Map", y))
    {
        link(normal, pinOf(MaterialOutputPin::Normal));
        y += 200.0f;
    }
    if (material.NormalScale != 1.0f)
    {
        link(add(Kind::ScalarParameter, "Normal Intensity", {material.NormalScale}, 40, y), pinOf(MaterialOutputPin::NormalIntensity));
        y += 100.0f;
    }
    // Emissive（テクスチャ × 色 × 強度）
    {
        const auto emissiveTexture = texture(MaterialTextureSlot::Emissive, MaterialSamplerType::Color, "Emissive Texture", y);
        const std::array<float, 4> emissive = {material.EmissiveColor[0] * material.EmissiveIntensity,
                                               material.EmissiveColor[1] * material.EmissiveIntensity,
                                               material.EmissiveColor[2] * material.EmissiveIntensity, 0};
        const auto color = add(Kind::VectorParameter, "Emissive", emissive, emissiveTexture ? 300.0f : 40.0f,
                               emissiveTexture ? y + 120 : y);
        if (emissiveTexture)
        {
            const auto rgb = add(Kind::ComponentMask, nullptr, {}, 300, y);
            if (auto* node = graph.FindNode(rgb))
            {
                node->Value = {1, 1, 1, 0};
            }
            graph.Connect(outputOf(emissiveTexture), inputOf(rgb, 0), error);
            link(multiply(rgb, color, y + 40), pinOf(MaterialOutputPin::Emissive));
            y += 260.0f;
        }
        else
        {
            link(color, pinOf(MaterialOutputPin::Emissive));
            y += 120.0f;
        }
    }
    // AO
    if (const auto ao = texture(MaterialTextureSlot::AO, MaterialSamplerType::Linear, "Ambient Occlusion", y))
    {
        link(mask(ao, 0, y), pinOf(MaterialOutputPin::AO));
        y += 200.0f;
    }
    // Height（POM）と深さ / ステップ
    if (const auto height = texture(MaterialTextureSlot::Height, MaterialSamplerType::Linear, "Height", y))
    {
        link(mask(height, 0, y), pinOf(MaterialOutputPin::Height));
        y += 200.0f;
    }
    if (material.ParallaxDepth > 0.0f || material.Textures[static_cast<size_t>(MaterialTextureSlot::Height)] != 0)
    {
        link(add(Kind::ScalarParameter, "Parallax Depth", {material.ParallaxDepth}, 40, y), pinOf(MaterialOutputPin::ParallaxDepth));
        link(add(Kind::ScalarParameter, "Parallax Steps", {material.ParallaxSteps}, 40, y + 100), pinOf(MaterialOutputPin::ParallaxSteps));
        y += 200.0f;
    }
    // ---- マテリアルモード（Opaque / Masked / Translucent / Glass）と Details の設定
    const MaterialGraphMode mode = ModeFromMaterial(material);
    // Glass：屈折 / 透過 / 吸収（アセットの値をそのままパラメーターにする）
    if (mode == MaterialGraphMode::Glass)
    {
        link(add(Kind::ScalarParameter, "IOR", {material.IOR}, 40, y), pinOf(MaterialOutputPin::IOR));
        link(add(Kind::ScalarParameter, "Transmission", {material.Transmission}, 40, y + 100),
             pinOf(MaterialOutputPin::Transmission));
        link(add(Kind::ScalarParameter, "Transmission Roughness", {material.TransmissionRoughness}, 40, y + 200),
             pinOf(MaterialOutputPin::TransmissionRoughness));
        link(add(Kind::ScalarParameter, "Thickness", {material.Thickness}, 40, y + 300),
             pinOf(MaterialOutputPin::Thickness));
        link(add(Kind::VectorParameter, "Absorption",
                 {material.AbsorptionCoefficient[0], material.AbsorptionCoefficient[1],
                  material.AbsorptionCoefficient[2], 0},
                 40, y + 400),
             pinOf(MaterialOutputPin::Absorption));
        y += 520.0f;
    }
    // 拡張シェーディングモデル（クリアコート / 布 / 肌 / 髪 / 異方性）の強さ
    if (material.ShadingModel != 0)
    {
        link(add(Kind::ScalarParameter, "Shading Strength", {material.ShadingStrength}, 40, y),
             pinOf(MaterialOutputPin::ShadingStrength));
        y += 100.0f;
    }
    graph.SetMode(mode);
    graph.Settings.Cull = static_cast<int>(material.Cull);
    graph.Settings.ShadingModel = static_cast<int>((std::min)(material.ShadingModel, 5u));
    graph.Settings.ShadowMode = static_cast<int>(material.ShadowMode);
    if (mode == MaterialGraphMode::Masked)
    {
        graph.Settings.AlphaCutoff = std::clamp(material.AlphaCutoff, 0.0f, 1.0f);
    }
    graph.Revision = 0;
    return graph;
}

uint64_t MaterialGraph::AddNode(MaterialNodeKind kind, float x, float y)
{
    if (Nodes.size() >= MaxNodes || m_NextId + 32 >= MaxIdentity || !std::isfinite(x) || !std::isfinite(y))
    {
        return 0;
    }
    if (kind == Kind::Output && (Settings.IsFunction || std::any_of(Nodes.begin(), Nodes.end(), [](const auto& node) {
            return node.Kind == Kind::Output;
        })))
    {
        return 0;
    }
    if ((kind == Kind::FunctionInput || kind == Kind::FunctionOutput) && !Settings.IsFunction)
    {
        return 0; // Function Input / Output は Material Function のグラフだけ
    }
    const auto& definition = Definition(kind);
    MaterialGraphNode node;
    node.Id = m_NextId++;
    node.Kind = kind;
    node.Name = definition.Name;
    node.Position = {x, y};
    switch (kind)
    {
        case Kind::TextureCoordinate:
            node.Value = {1, 1, 0, 0}; // UTiling, VTiling
            break;
        case Kind::ComponentMask:
            node.Value = {1, 1, 0, 0}; // R, G
            break;
        case Kind::Constant4Vector:
            node.Value = {0, 0, 0, 1};
            break;
        case Kind::Group:
            node.Name = "Comment";
            node.Value = {0.30f, 0.42f, 0.60f, 1.0f}; // コメント色
            break;
        case Kind::StaticSwitch:
            node.Value = {1, 0, 0, 0}; // 既定は True
            break;
        case Kind::Pi:
            node.Value = {1, 0, 0, 0}; // Multiplier
            break;
        case Kind::Blend:
            node.Value = {static_cast<float>(MaterialBlendMode::Overlay), 0, 0, 0};
            break;
        case Kind::ProceduralNoise:
            node.Value = {0, 1337, 0, 0}; // Perlin、シード、タイルなし
            break;
        case Kind::FractalNoise:
            node.Value = {0, 0, 1337, 0}; // Perlin、fBm、シード、タイルなし
            break;
        case Kind::NoiseCombine:
            node.Value = {static_cast<float>(MaterialNoiseCombineMode::Mix), 1, 0, 0}; // Mix、0..1 に収める
            break;
        case Kind::ConvertToNormalHeight:
        case Kind::ConvertToHeight:
            node.Value = {0, 0, 0, 1}; // Height、輝度、反転なし
            break;
        case Kind::ConvertToNormal:
            node.Value = {1, 0, 0, 1}; // Normal、輝度、反転なし、Sobel
            break;
        case Kind::SolidTexture:
        case Kind::SolidPattern:
            node.Value = {static_cast<float>(MaterialSolidKind::Marble), 0, 0, 1337}; // 大理石、Perlin、fBm
            break;
        case Kind::TriplanarTexture:
            node.Value = {static_cast<float>(MaterialSamplerType::Color), 0, 0, 0};
            break;
        case Kind::ConvertToSolid:
            node.Value = {static_cast<float>(MaterialSolidConvertMethod::Stochastic),
                          static_cast<float>(MaterialSamplerType::Color), 1337, 0};
            break;
        case Kind::ConvertToTexture:
            node.Value = {static_cast<float>(MaterialSamplerType::Linear), 1, 0, 0}; // Linear、512px
            break;
        default:
            break;
    }
    MaterialGraphNodes::InitNode(node);
    for (size_t i = 0; i < definition.Inputs.size(); ++i)
    {
        node.InputPins.push_back(m_NextId++);
    }
    if (kind != Kind::Output && kind != Kind::Group && kind != Kind::FunctionOutput)
    {
        node.OutputPin = m_NextId++;
    }
    if (kind == Kind::BreakVector)
    {
        for (int component = 0; component < 4; ++component)
        {
            node.SplitPins.push_back(m_NextId++);
        }
    }
    for (size_t slot = 0; slot < FixedOutputSlots(kind).size(); ++slot)
    {
        node.SplitPins.push_back(m_NextId++);
    }
    const auto id = node.Id;
    Nodes.push_back(std::move(node));
    ++Revision;
    return id;
}

MaterialGraphNode* MaterialGraph::FindNode(uint64_t id)
{
    for (auto& node : Nodes)
    {
        if (node.Id == id)
        {
            return &node;
        }
    }
    return nullptr;
}

const MaterialGraphNode* MaterialGraph::FindNode(uint64_t id) const
{
    for (const auto& node : Nodes)
    {
        if (node.Id == id)
        {
            return &node;
        }
    }
    return nullptr;
}

const MaterialGraphNode* MaterialGraph::FindPinNode(uint64_t pin, bool* input, size_t* index) const
{
    if (pin == 0)
    {
        return nullptr;
    }
    for (const auto& node : Nodes)
    {
        if (node.OutputPin == pin)
        {
            if (input)
            {
                *input = false;
            }
            if (index)
            {
                *index = 0;
            }
            return &node;
        }
        for (size_t i = 0; i < node.SplitPins.size(); ++i)
        {
            if (node.SplitPins[i] == pin)
            {
                if (input)
                {
                    *input = false;
                }
                if (index)
                {
                    *index = i + 1; // 0 = まとめた出力、1〜4 = 成分
                }
                return &node;
            }
        }
        for (size_t i = 0; i < node.InputPins.size(); ++i)
        {
            if (node.InputPins[i] == pin)
            {
                if (input)
                {
                    *input = true;
                }
                if (index)
                {
                    *index = i;
                }
                return &node;
            }
        }
    }
    return nullptr;
}

const MaterialGraphLink* MaterialGraph::RawInputLink(uint64_t pin) const
{
    for (const auto& link : Links)
    {
        if (link.TargetPin == pin)
        {
            return &link;
        }
    }
    return nullptr;
}

const MaterialGraphLink* MaterialGraph::InputLink(uint64_t pin) const
{
    const auto* link = RawInputLink(pin);
    if (!link || (Settings.Mode < 0 && Settings.ShadingModel != 0))
    {
        return link; // 旧グラフ（モード未設定）はすべてのピンが有効
    }
    for (const auto& node : Nodes)
    {
        if (node.Kind != Kind::Output)
        {
            continue;
        }
        for (size_t i = 0; i < node.InputPins.size(); ++i)
        {
            if (node.InputPins[i] == pin)
            {
                return IsOutputPinActive(i) ? link : nullptr;
            }
        }
        break;
    }
    return link;
}

MaterialGraphMode MaterialGraph::ModeFromMaterial(const MaterialAssetData& material)
{
    if (material.Transmission > 0.0f)
    {
        return MaterialGraphMode::Glass;
    }
    switch (material.Surface)
    {
        case MaterialSurface::Masked:
            return MaterialGraphMode::Masked;
        case MaterialSurface::Transparent:
            return MaterialGraphMode::Translucent;
        default:
            return MaterialGraphMode::Opaque;
    }
}

const char* MaterialGraph::OutputPinInactiveReason(MaterialOutputPin pin, const MaterialGraphSettings& settings)
{
    using Pin = MaterialOutputPin;
    if (pin == Pin::ShadingStrength && settings.ShadingModel == 0)
    {
        return "Shading Model が Default Lit のときは使えません";
    }
    if (settings.Mode < 0)
    {
        return nullptr;
    }
    const auto mode = static_cast<MaterialGraphMode>(std::clamp(settings.Mode, 0, 3));
    const bool glassPin = pin == Pin::IOR || pin == Pin::Transmission || pin == Pin::TransmissionRoughness ||
                          pin == Pin::Thickness || pin == Pin::Absorption;
    const bool pomPin = pin == Pin::Height || pin == Pin::ParallaxDepth || pin == Pin::ParallaxSteps;
    switch (mode)
    {
        case MaterialGraphMode::Opaque:
            if (pin == Pin::Opacity || pin == Pin::OpacityMask)
            {
                return "Opaque では使えません（Masked / Translucent / Glass にしてください）";
            }
            if (glassPin)
            {
                return "屈折 / 透過は Glass モードだけで使えます";
            }
            break;
        case MaterialGraphMode::Masked:
            if (pin == Pin::Opacity)
            {
                return "Masked では Opacity Mask を使います";
            }
            if (glassPin)
            {
                return "屈折 / 透過は Glass モードだけで使えます";
            }
            break;
        case MaterialGraphMode::Translucent:
            if (pin == Pin::OpacityMask)
            {
                return "Translucent では Opacity を使います";
            }
            if (pomPin)
            {
                return "POM（Height / Parallax）は不透明（Opaque / Masked）だけで使えます";
            }
            if (glassPin)
            {
                return "屈折 / 透過は Glass モードだけで使えます";
            }
            break;
        case MaterialGraphMode::Glass:
            if (pin == Pin::OpacityMask)
            {
                return "Glass では Opacity を使います";
            }
            if (pomPin)
            {
                return "POM（Height / Parallax）は不透明（Opaque / Masked）だけで使えます";
            }
            break;
        default:
            break;
    }
    return nullptr;
}

bool MaterialGraph::IsOutputPinActive(MaterialOutputPin pin, const MaterialGraphSettings& settings)
{
    return OutputPinInactiveReason(pin, settings) == nullptr;
}

bool MaterialGraph::IsOutputPinActive(size_t index) const
{
    return index >= static_cast<size_t>(MaterialOutputPin::Count) ||
           IsOutputPinActive(static_cast<MaterialOutputPin>(index), Settings);
}

void MaterialGraph::SetMode(MaterialGraphMode mode)
{
    Settings.Mode = std::clamp(static_cast<int>(mode), 0, 3);
    switch (mode)
    {
        case MaterialGraphMode::Opaque:
            Settings.Surface = 0;
            break;
        case MaterialGraphMode::Masked:
            Settings.Surface = 1;
            break;
        default:
            Settings.Surface = 2;
            break;
    }
    ++Revision;
}

bool MaterialGraph::RemoveNode(uint64_t id)
{
    const auto* node = FindNode(id);
    if (!node || node->Kind == Kind::Output)
    {
        return false;
    }
    std::unordered_set<uint64_t> pins(node->InputPins.begin(), node->InputPins.end());
    pins.insert(node->OutputPin);
    pins.insert(node->SplitPins.begin(), node->SplitPins.end());
    std::erase_if(Links, [&](const auto& link) {
        return pins.contains(link.SourcePin) || pins.contains(link.TargetPin);
    });
    std::erase_if(Nodes, [&](const auto& candidate) {
        return candidate.Id == id;
    });
    ++Revision;
    return true;
}

bool MaterialGraph::RemoveLink(uint64_t id)
{
    if (std::erase_if(Links, [&](const auto& link) {
            return link.Id == id;
        }) == 0)
    {
        return false;
    }
    ++Revision;
    return true;
}

bool MaterialGraph::Reaches(uint64_t start, uint64_t target) const
{
    std::vector<uint64_t> pending{start};
    std::unordered_set<uint64_t> visited;
    while (!pending.empty())
    {
        const auto current = pending.back();
        pending.pop_back();
        if (current == target)
        {
            return true;
        }
        if (!visited.insert(current).second)
        {
            continue;
        }
        const auto* node = FindNode(current);
        if (!node)
        {
            continue;
        }
        for (const auto& link : Links)
        {
            if (node->OwnsOutput(link.SourcePin))
            {
                const auto* downstream = FindPinNode(link.TargetPin);
                if (downstream)
                {
                    pending.push_back(downstream->Id);
                }
            }
        }
    }
    return false;
}

MaterialGraphType MaterialGraph::OutputType(uint64_t id, std::vector<uint64_t>& visiting) const
{
    const auto* node = FindNode(id);
    if (!node || std::find(visiting.begin(), visiting.end(), id) != visiting.end())
    {
        return Type::Numeric;
    }
    const auto& definition = Definition(node->Kind);
    if (definition.OutputType != Type::Numeric)
    {
        return definition.OutputType;
    }
    auto linkedType = [&](size_t input, Type fallback) {
        const auto* link = input < node->InputPins.size() ? InputLink(node->InputPins[input]) : nullptr;
        const auto* source = link ? FindPinNode(link->SourcePin) : nullptr;
        return source ? SourcePinType(link->SourcePin, visiting) : fallback;
    };
    if (node->Kind == Kind::ConvertToNormalHeight)
    {
        return node->Value[0] > 0.5f ? Type::Vector3 : Type::Scalar;
    }
    if (node->Kind == Kind::MaterialFunctionCall)
    {
        return SlotType(*node, 0);
    }
    if (node->Kind == Kind::CustomHlsl || node->Kind == Kind::FunctionInput)
    {
        return static_cast<Type>(std::clamp(static_cast<int>(node->Value[node->Kind == Kind::CustomHlsl ? 1 : 0]), 1, 4));
    }
    if (node->Kind == Kind::FunctionOutput)
    {
        return Type::Numeric;
    }
    if (node->Kind == Kind::StaticSwitch)
    {
        visiting.push_back(id);
        const size_t selected = node->Value[0] > 0.5f ? 0 : 1;
        const Type result = linkedType(selected, Type::Scalar);
        visiting.pop_back();
        return result;
    }
    if (node->Kind == Kind::AppendVector)
    {
        visiting.push_back(id);
        const int count = Dimensions(linkedType(0, Type::Scalar)) + Dimensions(linkedType(1, Type::Scalar));
        visiting.pop_back();
        return static_cast<Type>(std::clamp(count, 1, 4));
    }
    if (node->Kind == Kind::ComponentMask)
    {
        int count = 0;
        for (float flag : node->Value)
        {
            count += flag > 0.5f ? 1 : 0;
        }
        return static_cast<Type>(std::clamp(count, 1, 4));
    }
    visiting.push_back(id);
    Type result = Type::Scalar;
    for (const auto pin : node->InputPins)
    {
        const auto* link = InputLink(pin);
        const auto* source = link ? FindPinNode(link->SourcePin) : nullptr;
        if (source)
        {
            result = static_cast<Type>(
                (std::max)(static_cast<int>(result), static_cast<int>(SourcePinType(link->SourcePin, visiting))));
        }
    }
    visiting.pop_back();
    return result;
}

MaterialGraphType MaterialGraph::ResolveOutputType(uint64_t node) const
{
    std::vector<uint64_t> visiting;
    return OutputType(node, visiting);
}

MaterialGraphType MaterialGraph::SourcePinType(uint64_t pin, std::vector<uint64_t>& visiting) const
{
    size_t index = 0;
    const auto* node = FindPinNode(pin, nullptr, &index);
    if (!node)
    {
        return Type::Numeric;
    }
    if (index > 0)
    {
        return SlotType(*node, index - 1); // 分割した成分は Scalar、固定スロットはスロットの型
    }
    return OutputType(node->Id, visiting);
}

int MaterialGraph::OutputComponent(uint64_t pin) const
{
    bool input = false;
    size_t index = 0;
    const auto* node = FindPinNode(pin, &input, &index);
    return node && !input && index > 0 ? static_cast<int>(index) - 1 : -1;
}

MaterialGraphType MaterialGraph::PinOutputType(uint64_t pin) const
{
    std::vector<uint64_t> visiting;
    return SourcePinType(pin, visiting);
}

bool MaterialGraph::CanSplitOutput(uint64_t id) const
{
    const auto* node = FindNode(id);
    if (!node || !node->SplitPins.empty() || node->OutputPin == 0 || node->Kind == Kind::Reroute ||
        node->Kind == Kind::Output || node->Kind == Kind::Group || HasFixedOutputs(node->Kind))
    {
        return false;
    }
    const int count = static_cast<int>(ResolveOutputType(id));
    return count >= 2 && count <= 4;
}

bool MaterialGraph::SplitOutput(uint64_t id, std::string& error, int forcedCount)
{
    const auto* target = FindNode(id);
    const bool forced = forcedCount >= 2 && forcedCount <= 4 && target && target->SplitPins.empty() &&
                        target->OutputPin != 0 && target->Kind != Kind::Reroute;
    if (!forced && !CanSplitOutput(id))
    {
        error = "float2 以上の出力だけ分割できます。";
        return false;
    }
    const int count = forced ? forcedCount : static_cast<int>(ResolveOutputType(id));
    if (m_NextId + count >= MaxIdentity)
    {
        error = "ピンの上限に達しています。";
        return false;
    }
    auto* node = FindNode(id);
    const uint64_t output = node->OutputPin;
    std::erase_if(Links, [&](const auto& link) {
        return link.SourcePin == output;
    });
    for (int component = 0; component < count; ++component)
    {
        node->SplitPins.push_back(m_NextId++);
    }
    ++Revision;
    return true;
}

bool MaterialGraph::RecombineOutput(uint64_t id)
{
    auto* node = FindNode(id);
    if (!node || node->SplitPins.empty() || HasFixedOutputs(node->Kind))
    {
        return false;
    }
    const std::unordered_set<uint64_t> pins(node->SplitPins.begin(), node->SplitPins.end());
    std::erase_if(Links, [&](const auto& link) {
        return pins.contains(link.SourcePin);
    });
    node->SplitPins.clear();
    ++Revision;
    return true;
}

bool MaterialGraph::IsPinValueEditable(MaterialNodeKind kind, size_t input)
{
    // 定数の成分ピンは node.Value[i] を直接編集する（PinValues は使わない）
    if (IsComponentConstant(kind) || kind == Kind::Reroute || kind == Kind::Group || kind == Kind::NoiseLayers ||
        kind == Kind::BreakVector)
    {
        return false;
    }
    const auto& definition = Definition(kind);
    if (input >= definition.Inputs.size())
    {
        return false;
    }
    if (kind == Kind::Output && input >= static_cast<size_t>(MaterialOutputPin::ToonShade))
    {
        return false; // 追加の陰影ピン：ノードを繋いだときだけ有効（直値は持たない）
    }
    const auto type = definition.Inputs[input].Type;
    if (type != Type::Scalar && type != Type::Numeric)
    {
        return false;
    }
    // 追加ノード：UV / 時刻 / 法線 / 視線などを既定で使うピンには出さない。Custom HLSL / Function の入力も直値を持てる
    if (MaterialGraphMath::DefaultInput(kind, input) != MaterialGraphMath::SpecialInput::None)
    {
        return false;
    }
    // UV / Time / World Position を既定で使うピンには出さない（UE と同じ）
    switch (kind)
    {
        case Kind::Panner:
        case Kind::Rotator:
            return input >= 2;
        case Kind::TextureSample:
            return input != 0;
        case Kind::BumpOffset:
        case Kind::RotateAboutAxis:
            return input != 3;
        default:
            return true;
    }
}

bool MaterialGraph::IsComponentConstant(MaterialNodeKind kind)
{
    return kind == Kind::Constant || kind == Kind::Constant2Vector || kind == Kind::Constant3Vector ||
           kind == Kind::Constant4Vector;
}

bool MaterialGraph::HasConnectedComponent(const MaterialGraphNode& node) const
{
    if (!IsComponentConstant(node.Kind))
    {
        return false;
    }
    for (const auto pin : node.InputPins)
    {
        if (InputLink(pin))
        {
            return true;
        }
    }
    return false;
}

bool MaterialGraph::IsPinVectorEditable(MaterialNodeKind kind, size_t input)
{
    if (kind != Kind::Output)
    {
        return false;
    }
    const auto& definition = Definition(kind);
    if (input >= definition.Inputs.size())
    {
        return false;
    }
    if (input >= static_cast<size_t>(MaterialOutputPin::ToonShade))
    {
        return false; // 追加の陰影ピンは直値を持たない（ノードを繋いだときだけ有効）
    }
    const auto type = definition.Inputs[input].Type;
    return type == Type::Vector2 || type == Type::Vector3 || type == Type::Vector4;
}

bool MaterialGraph::HasPinVector(const MaterialGraphNode& node, size_t input)
{
    return input < node.PinVectors.size() && std::isfinite(node.PinVectors[input][0]);
}

bool MaterialGraph::ClearPinOverride(MaterialGraphNode& node, size_t input)
{
    bool cleared = false;
    if (HasPinValue(node, input))
    {
        node.PinValues[input] = std::numeric_limits<float>::quiet_NaN();
        cleared = true;
    }
    if (HasPinVector(node, input))
    {
        node.PinVectors[input].fill(std::numeric_limits<float>::quiet_NaN());
        cleared = true;
    }
    return cleared;
}

bool MaterialGraph::HasOutputDirectValue(const MaterialGraphNode& output, size_t input) const
{
    if (output.Kind != Kind::Output || input >= output.InputPins.size() || InputLink(output.InputPins[input]) ||
        !IsOutputPinActive(input))
    {
        return false;
    }
    return (IsPinValueEditable(output.Kind, input) && HasPinValue(output, input)) ||
           (IsPinVectorEditable(output.Kind, input) && HasPinVector(output, input));
}

bool MaterialGraph::HasPinValue(const MaterialGraphNode& node, size_t input)
{
    return input < node.PinValues.size() && std::isfinite(node.PinValues[input]);
}

std::array<float, 4> MaterialGraph::UnconnectedInputValue(const MaterialGraphNode& node, size_t input)
{
    const auto& definition = Definition(node.Kind);
    std::array<float, 4> value{};
    if (input < definition.Inputs.size())
    {
        value = definition.Inputs[input].Default;
    }
    if (IsComponentConstant(node.Kind) && input < definition.Inputs.size() && input < node.Value.size())
    {
        value.fill(node.Value[input]); // 未接続の成分 = ノード上の値
        return value;
    }
    if (HasPinVector(node, input) && IsPinVectorEditable(node.Kind, input))
    {
        value = node.PinVectors[input]; // Material Output の色 / 法線の直値
        return value;
    }
    if (HasPinValue(node, input) && IsPinValueEditable(node.Kind, input))
    {
        value.fill(node.PinValues[input]); // Numeric の入力はスカラーとしてブロードキャストされる
    }
    return value;
}

bool MaterialGraph::CanConnect(uint64_t source, uint64_t target, std::string& error) const
{
    error.clear();
    bool sourceInput = false;
    bool targetInput = false;
    size_t inputIndex = 0;
    const auto* sourceNode = FindPinNode(source, &sourceInput);
    const auto* targetNode = FindPinNode(target, &targetInput, &inputIndex);
    if (!sourceNode || !targetNode || sourceInput || !targetInput)
    {
        error = "Connect an output pin to an input pin.";
        return false;
    }
    if (sourceNode->Id == targetNode->Id || Reaches(targetNode->Id, sourceNode->Id))
    {
        error = "This connection would create a cycle.";
        return false;
    }
    // 分割中のノードは成分ピンからだけ繋げる（まとめた出力は隠れている。Break Vector は常に分割）
    if (!sourceNode->SplitPins.empty() && source == sourceNode->OutputPin)
    {
        error = "This output is split. Connect from the X / Y / Z / W pins.";
        return false;
    }
    std::vector<uint64_t> visiting;
    if (!Compatible(SourcePinType(source, visiting), Definition(targetNode->Kind).Inputs[inputIndex].Type))
    {
        error = "Pin types are incompatible. Scalars may broadcast; vectors must have matching dimensions.";
        return false;
    }
    return true;
}

bool MaterialGraph::CanConnectActive(uint64_t source, uint64_t target, std::string& error) const
{
    if (!CanConnect(source, target, error))
    {
        return false;
    }
    size_t index = 0;
    const auto* node = FindPinNode(target, nullptr, &index);
    if (node && node->Kind == Kind::Output)
    {
        if (const char* reason = OutputPinInactiveReason(static_cast<MaterialOutputPin>(index), Settings))
        {
            error = reason;
            return false;
        }
    }
    return true;
}

bool MaterialGraph::Connect(uint64_t source, uint64_t target, std::string& error)
{
    if (Links.size() >= MaxLinks || m_NextId >= MaxIdentity || !CanConnect(source, target, error))
    {
        return false;
    }
    MaterialGraph candidate = *this;
    std::erase_if(candidate.Links, [&](const auto& link) {
        return link.TargetPin == target;
    });
    candidate.Links.push_back({candidate.m_NextId++, source, target});
    if (!candidate.Validate(error))
    {
        return false;
    }
    ++candidate.Revision;
    *this = std::move(candidate);
    return true;
}

bool MaterialGraph::ConnectUnchecked(uint64_t source, uint64_t target)
{
    if (Links.size() >= MaxLinks || m_NextId >= MaxIdentity || !FindPinNode(source) || !FindPinNode(target))
    {
        return false;
    }
    std::erase_if(Links, [&](const auto& link) {
        return link.TargetPin == target;
    });
    Links.push_back({m_NextId++, source, target});
    ++Revision;
    return true;
}

bool MaterialGraph::Validate(std::string& error) const
{
    error.clear();
    std::unordered_set<uint64_t> ids;
    size_t outputs = 0;
    const auto unique = [&](uint64_t id) {
        return id > 0 && id <= MaxIdentity && ids.insert(id).second;
    };
    for (const auto& node : Nodes)
    {
        const auto& definition = Definition(node.Kind);
        if (!unique(node.Id) || node.InputPins.size() != definition.Inputs.size() || definition.Kind != node.Kind)
        {
            error = "Invalid node identity, kind or pin count.";
            return false;
        }
        if (node.Kind == Kind::Output)
        {
            ++outputs;
        }
        if ((node.Kind == Kind::FunctionInput || node.Kind == Kind::FunctionOutput) != false && !Settings.IsFunction)
        {
            error = "Function Input / Function Output can only be used inside a Material Function.";
            return false;
        }
        if ((node.Kind == Kind::Output || node.Kind == Kind::Group || node.Kind == Kind::FunctionOutput) && node.OutputPin != 0)
        {
            error = "Material Output and comment groups cannot have output pins.";
            return false;
        }
        for (const auto pin : node.InputPins)
        {
            if (!unique(pin))
            {
                error = "Duplicate input pin identity.";
                return false;
            }
        }
        if (node.Kind != Kind::Output && node.Kind != Kind::Group && node.Kind != Kind::FunctionOutput && !unique(node.OutputPin))
        {
            error = "Invalid output pin identity.";
            return false;
        }
        if (node.SplitPins.size() > 8 || ((node.Kind == Kind::Output || node.Kind == Kind::Group ||
                                           node.Kind == Kind::Reroute) && !node.SplitPins.empty()) ||
            (!HasFixedOutputs(node.Kind) && node.SplitPins.size() > 4) ||
            (!FixedOutputSlots(node.Kind).empty() && node.SplitPins.size() != FixedOutputSlots(node.Kind).size()))
        {
            error = "Invalid split output pins.";
            return false;
        }
        for (const auto pin : node.SplitPins)
        {
            if (!unique(pin))
            {
                error = "Duplicate split pin identity.";
                return false;
            }
        }
        for (const float value : node.Value)
        {
            if (!std::isfinite(value))
            {
                error = "Non-finite node value.";
                return false;
            }
        }
        for (const float value : node.Position)
        {
            if (!std::isfinite(value))
            {
                error = "Non-finite node position.";
                return false;
            }
        }
        for (const float value : node.GroupSize)
        {
            if (!std::isfinite(value) || value <= 0)
            {
                error = "Invalid group size.";
                return false;
            }
        }
    }
    if (outputs != (Settings.IsFunction ? 0u : 1u))
    {
        error = Settings.IsFunction ? "A Material Function must not contain a Material Output."
                                    : "A material graph must have exactly one Material Output.";
        return false;
    }
    std::unordered_set<uint64_t> targets;
    for (const auto& node : Nodes)
    {
        if (node.Kind == Kind::AppendVector)
        {
            std::vector<uint64_t> visiting;
            int count = 0;
            for (const auto pin : node.InputPins)
            {
                const auto* link = InputLink(pin);
                const auto* source = link ? FindPinNode(link->SourcePin) : nullptr;
                count += source ? Dimensions(SourcePinType(link->SourcePin, visiting)) : 1;
            }
            if (count > 4)
            {
                error = "Append: the combined vector exceeds 4 components.";
                return false;
            }
        }
    }
    for (const auto& link : Links)
    {
        if (!unique(link.Id) || !targets.insert(link.TargetPin).second ||
            !CanConnect(link.SourcePin, link.TargetPin, error))
        {
            if (error.empty())
            {
                error = "Duplicate link identity or multiple connections to one input.";
            }
            return false;
        }
    }
    for (const auto& node : Nodes)
    {
        if (Definition(node.Kind).OutputType != Type::Numeric || node.Kind == Kind::Output ||
            node.Kind == Kind::AppendVector || node.Kind == Kind::ComponentMask ||
            IsConvertTextureNode(node.Kind) || node.Kind == Kind::MaterialFunctionCall ||
            node.Kind == Kind::CustomHlsl || node.Kind == Kind::FunctionInput || node.Kind == Kind::FunctionOutput)
        {
            continue;
        }
        Type vectorType = Type::Scalar;
        for (const auto pin : node.InputPins)
        {
            const auto* link = InputLink(pin);
            const auto* source = link ? FindPinNode(link->SourcePin) : nullptr;
            if (!source)
            {
                continue;
            }
            std::vector<uint64_t> visiting;
            const auto type = SourcePinType(link->SourcePin, visiting);
            if (type != Type::Scalar)
            {
                if (vectorType != Type::Scalar && vectorType != type)
                {
                    error = "Arithmetic vector inputs must have matching dimensions.";
                    return false;
                }
                vectorType = type;
            }
        }
    }
    return true;
}

std::string MaterialGraph::Serialize() const
{
    Json::Value document(Json::objectValue);
    document["version"] = 1;
    document["nodeNames"] = 2; // 2：Object Position = オブジェクト原点（旧 Object Position は Local Position）
    document["nodes"] = Json::Value(Json::arrayValue);
    document["links"] = Json::Value(Json::arrayValue);
    for (const auto& node : Nodes)
    {
        Json::Value value(Json::objectValue);
        value["id"] = Json::UInt64(node.Id);
        value["kind"] = Definition(node.Kind).Name;
        value["name"] = node.Name;
        value["texture"] = FormatAssetHandle(node.Texture);
        value["output"] = Json::UInt64(node.OutputPin);
        value["inputs"] = Json::Value(Json::arrayValue);
        for (const auto pin : node.InputPins)
        {
            value["inputs"].append(Json::UInt64(pin));
        }
        for (const float component : node.Value)
        {
            value["value"].append(component);
        }
        for (const float component : node.Position)
        {
            value["position"].append(component);
        }
        for (const float component : node.GroupSize)
        {
            value["size"].append(component);
        }
        if (node.Width > 0.0f)
        {
            value["width"] = node.Width;
        }
        if (node.Height > 0.0f)
        {
            value["height"] = node.Height;
        }
        if (!node.Comment.empty())
        {
            value["comment"] = node.Comment;
        }
        if (!node.Params.empty())
        {
            value["params"] = Json::Value(Json::arrayValue);
            for (const float component : node.Params)
            {
                value["params"].append(std::isfinite(component) ? component : 0.0f);
            }
        }
        if (!node.Text.empty())
        {
            value["text"] = node.Text;
        }
        if (!node.PinNames.empty())
        {
            value["pinNames"] = Json::Value(Json::arrayValue);
            for (const auto& name : node.PinNames)
            {
                value["pinNames"].append(name);
            }
        }
        if (node.Function != 0)
        {
            value["function"] = FormatAssetHandle(node.Function);
        }
        if (!node.SplitPins.empty())
        {
            value["split"] = Json::Value(Json::arrayValue);
            for (const auto pin : node.SplitPins)
            {
                value["split"].append(Json::UInt64(pin));
            }
        }
        {
            Json::Value pins(Json::objectValue);
            bool any = false;
            for (size_t i = 0; i < node.PinValues.size(); ++i)
            {
                if (std::isfinite(node.PinValues[i]))
                {
                    pins[std::to_string(i)] = node.PinValues[i];
                    any = true;
                }
            }
            Json::Value vectors(Json::objectValue);
            bool anyVector = false;
            for (size_t i = 0; i < node.PinVectors.size(); ++i)
            {
                if (HasPinVector(node, i))
                {
                    Json::Value components(Json::arrayValue);
                    for (const float component : node.PinVectors[i])
                    {
                        components.append(std::isfinite(component) ? component : 0.0f);
                    }
                    vectors[std::to_string(i)] = components;
                    anyVector = true;
                }
            }
            if (anyVector)
            {
                value["pinVectors"] = vectors;
            }
            if (any)
            {
                value["pinValues"] = pins;
            }
        }
        document["nodes"].append(std::move(value));
    }
    for (const auto& link : Links)
    {
        Json::Value value(Json::objectValue);
        value["id"] = Json::UInt64(link.Id);
        value["source"] = Json::UInt64(link.SourcePin);
        value["target"] = Json::UInt64(link.TargetPin);
        document["links"].append(std::move(value));
    }
    // グラフ設定（既定値と同じなら書かない：旧バージョンとの差分を最小にする）
    if (!(Settings == MaterialGraphSettings{}))
    {
        Json::Value settings(Json::objectValue);
        settings["materialMode"] = Settings.Mode;
        settings["surface"] = Settings.Surface;
        settings["cull"] = Settings.Cull;
        settings["shadingModel"] = Settings.ShadingModel;
        settings["shadowMode"] = Settings.ShadowMode;
        settings["alphaCutoff"] = Settings.AlphaCutoff;
        settings["bakeResolution"] = Settings.BakeResolution;
        settings["bakeTime"] = Settings.BakeTime;
        settings["autoBake"] = Settings.AutoBake;
        settings["liveUpdate"] = Settings.LiveUpdate;
        settings["runtimeEvaluation"] = Settings.RuntimeEvaluation;
        settings["solidSpace"] = Settings.SolidSpace;
        settings["liveResolution"] = Settings.LiveResolution;
        settings["liveRate"] = Settings.LiveRate;
        if (Settings.IsFunction)
        {
            settings["isFunction"] = true;
        }
        document["settings"] = std::move(settings);
    }
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    return Json::writeString(writer, document);
}

bool MaterialGraph::Deserialize(const std::string& contents, MaterialGraph& graph, std::string& error)
{
    Json::Value document;
    Json::CharReaderBuilder reader;
    std::istringstream input(contents);
    if (contents.size() > 8 * 1024 * 1024 || !Json::parseFromStream(reader, input, &document, &error) ||
        !document.isObject() || !document["version"].isInt() || document["version"].asInt() != 1 ||
        !document["nodes"].isArray() || document["nodes"].size() > MaxNodes || !document["links"].isArray() ||
        document["links"].size() > MaxLinks)
    {
        error = "Invalid or unsupported material graph document. " + error;
        return false;
    }
    MaterialGraph loaded;
    const auto readArray = [&](const Json::Value& source, auto& destination) {
        if (!source.isArray() || source.size() != destination.size())
        {
            return false;
        }
        for (Json::ArrayIndex i = 0; i < source.size(); ++i)
        {
            if (!source[i].isNumeric())
            {
                return false;
            }
            destination[i] = source[i].asFloat();
        }
        return true;
    };
    const int nodeNames = document.isMember("nodeNames") && document["nodeNames"].isInt() ? document["nodeNames"].asInt() : 1;
    for (const auto& value : document["nodes"])
    {
        if (!value.isObject() || !value["id"].isUInt64() || !value["kind"].isString() || !value["name"].isString() ||
            !value["texture"].isString() || !value["output"].isUInt64() || !value["inputs"].isArray())
        {
            error = "Malformed material graph node.";
            return false;
        }
        // 旧ファイル（nodeNames < 2）の Object Position は、今の Local Position
        std::string kindName = value["kind"].asString();
        if (nodeNames < 2 && kindName == "Object Position")
        {
            kindName = "Local Position";
        }
        const auto definition = std::find_if(NodeDefinitions.begin(), NodeDefinitions.end(), [&](const auto& item) {
            return kindName == item.Name;
        });
        if (definition == NodeDefinitions.end())
        {
            error = "Unknown node kind.";
            return false;
        }
        MaterialGraphNode node;
        node.Id = value["id"].asUInt64();
        node.Kind = definition->Kind;
        node.Name = value["name"].asString();
        if (nodeNames < 2 && node.Kind == Kind::LocalPosition && node.Name == "Object Position")
        {
            node.Name = "Local Position";
        }
        node.Texture = ParseAssetHandle(value["texture"].asString());
        node.OutputPin = value["output"].asUInt64();
        if (!readArray(value["value"], node.Value) || !readArray(value["position"], node.Position) ||
            !readArray(value["size"], node.GroupSize))
        {
            error = "Malformed node values or position.";
            return false;
        }
        // 追加項目（旧バージョンのファイルには無い）
        if (value.isMember("width") && value["width"].isNumeric())
        {
            const float width = value["width"].asFloat();
            node.Width = std::isfinite(width) ? std::clamp(width, 0.0f, 1024.0f) : 0.0f;
        }
        if (value.isMember("height") && value["height"].isNumeric())
        {
            const float height = value["height"].asFloat();
            node.Height = std::isfinite(height) ? std::clamp(height, 0.0f, 2048.0f) : 0.0f;
        }
        if (value.isMember("comment") && value["comment"].isString())
        {
            node.Comment = value["comment"].asString();
        }
        // 2026-11 追加の項目（旧ファイルには無い）
        if (value.isMember("params") && value["params"].isArray() && value["params"].size() <= 1024)
        {
            for (const auto& component : value["params"])
            {
                if (!component.isNumeric() || !std::isfinite(component.asFloat()))
                {
                    error = "Malformed node parameters.";
                    return false;
                }
                node.Params.push_back(component.asFloat());
            }
        }
        if (value.isMember("text") && value["text"].isString() && value["text"].asString().size() <= 16384)
        {
            node.Text = value["text"].asString();
        }
        if (value.isMember("pinNames") && value["pinNames"].isArray() && value["pinNames"].size() <= 16)
        {
            for (const auto& name : value["pinNames"])
            {
                if (name.isString() && name.asString().size() <= 64)
                {
                    node.PinNames.push_back(name.asString());
                }
            }
        }
        if (value.isMember("function") && value["function"].isString())
        {
            node.Function = ParseAssetHandle(value["function"].asString());
        }
        if (value.isMember("split") && value["split"].isArray())
        {
            for (const auto& pin : value["split"])
            {
                if (!pin.isUInt64())
                {
                    error = "Malformed split pin identity.";
                    return false;
                }
                node.SplitPins.push_back(pin.asUInt64());
            }
        }
        if (value.isMember("pinValues") && value["pinValues"].isObject())
        {
            const Json::Value& pins = value["pinValues"];
            for (const auto& key : pins.getMemberNames())
            {
                char* end = nullptr;
                const unsigned long index = std::strtoul(key.c_str(), &end, 10);
                if (!end || *end != '\0' || index >= definition->Inputs.size() || !pins[key].isNumeric())
                {
                    continue;
                }
                const float pinValue = pins[key].asFloat();
                if (!std::isfinite(pinValue))
                {
                    continue;
                }
                if (node.PinValues.size() <= index)
                {
                    node.PinValues.resize(index + 1, std::numeric_limits<float>::quiet_NaN());
                }
                node.PinValues[index] = pinValue;
            }
        }
        if (value.isMember("pinVectors") && value["pinVectors"].isObject())
        {
            const Json::Value& vectors = value["pinVectors"];
            for (const auto& key : vectors.getMemberNames())
            {
                char* end = nullptr;
                const unsigned long index = std::strtoul(key.c_str(), &end, 10);
                const Json::Value& components = vectors[key];
                if (!end || *end != '\0' || index >= definition->Inputs.size() || !components.isArray() ||
                    components.size() != 4)
                {
                    continue;
                }
                std::array<float, 4> vector{};
                bool valid = true;
                for (Json::ArrayIndex c = 0; c < 4; ++c)
                {
                    valid &= components[c].isNumeric() && std::isfinite(components[c].asFloat());
                    vector[c] = valid ? components[c].asFloat() : 0.0f;
                }
                if (!valid)
                {
                    continue;
                }
                if (node.PinVectors.size() <= index)
                {
                    std::array<float, 4> unset;
                    unset.fill(std::numeric_limits<float>::quiet_NaN());
                    node.PinVectors.resize(index + 1, unset);
                }
                node.PinVectors[index] = vector;
            }
        }
        for (const auto& pin : value["inputs"])
        {
            if (!pin.isUInt64())
            {
                error = "Malformed pin identity.";
                return false;
            }
            node.InputPins.push_back(pin.asUInt64());
        }
        loaded.Nodes.push_back(std::move(node));
    }
    for (const auto& value : document["links"])
    {
        if (!value.isObject() || !value["id"].isUInt64() || !value["source"].isUInt64() || !value["target"].isUInt64())
        {
            error = "Malformed material graph link.";
            return false;
        }
        loaded.Links.push_back({value["id"].asUInt64(), value["source"].asUInt64(), value["target"].asUInt64()});
    }
    if (document.isMember("settings") && document["settings"].isObject())
    {
        const auto& settings = document["settings"];
        const auto readInt = [&](const char* name, int& target, int minimum, int maximum) {
            if (settings[name].isInt())
            {
                target = std::clamp(settings[name].asInt(), minimum, maximum);
            }
        };
        const auto readFloat = [&](const char* name, float& target, float minimum, float maximum) {
            if (settings[name].isNumeric())
            {
                const float value = settings[name].asFloat();
                target = std::isfinite(value) ? std::clamp(value, minimum, maximum) : target;
            }
        };
        readInt("materialMode", loaded.Settings.Mode, -1, 3);
        readInt("surface", loaded.Settings.Surface, -1, 2);
        readInt("cull", loaded.Settings.Cull, -1, 2);
        readInt("shadingModel", loaded.Settings.ShadingModel, -1, 5);
        readInt("shadowMode", loaded.Settings.ShadowMode, -1, 4);
        readFloat("alphaCutoff", loaded.Settings.AlphaCutoff, -1.0f, 1.0f);
        readInt("bakeResolution", loaded.Settings.BakeResolution, 64, 2048);
        readFloat("bakeTime", loaded.Settings.BakeTime, 0.0f, 3600.0f);
        if (settings["runtimeEvaluation"].isBool())
        {
            loaded.Settings.RuntimeEvaluation = settings["runtimeEvaluation"].asBool();
        }
        readInt("solidSpace", loaded.Settings.SolidSpace, 0, 1);
        if (settings["liveUpdate"].isBool())
        {
            loaded.Settings.LiveUpdate = settings["liveUpdate"].asBool();
        }
        readInt("liveResolution", loaded.Settings.LiveResolution, 32, 1024);
        readFloat("liveRate", loaded.Settings.LiveRate, 1.0f, 120.0f);
        if (settings["autoBake"].isBool())
        {
            loaded.Settings.AutoBake = settings["autoBake"].asBool();
        }
        if (settings["isFunction"].isBool())
        {
            loaded.Settings.IsFunction = settings["isFunction"].asBool();
        }
    }
    uint64_t highest = 0;
    for (const auto& node : loaded.Nodes)
    {
        highest = (std::max)({highest, node.Id, node.OutputPin});
        for (const auto pin : node.InputPins)
        {
            highest = (std::max)(highest, pin);
        }
        for (const auto pin : node.SplitPins)
        {
            highest = (std::max)(highest, pin);
        }
    }
    for (const auto& link : loaded.Links)
    {
        highest = (std::max)(highest, link.Id);
    }
    // 旧バージョンのグラフ：後から入力ピンが増えたノード（Material Output の Height / POM など）にピンを足す
    for (auto& node : loaded.Nodes)
    {
        const auto& definition = Definition(node.Kind);
        while (node.InputPins.size() < definition.Inputs.size() && highest + 1 < MaxIdentity)
        {
            node.InputPins.push_back(++highest);
        }
        while (node.Kind == Kind::BreakVector && node.SplitPins.size() < 4 && highest + 1 < MaxIdentity)
        {
            node.SplitPins.push_back(++highest); // Break Vector は常に 4 成分
        }
        // 固定の出力スロットを持つノード（Packed Texture Channels / Beat Phase / Flow Map / ...）は常にスロット分のピンを持つ
        while (node.SplitPins.size() < FixedOutputSlots(node.Kind).size() && highest + 1 < MaxIdentity)
        {
            node.SplitPins.push_back(++highest);
        }
    }
    if (!loaded.Validate(error))
    {
        return false;
    }
    loaded.m_NextId = highest + 1;
    graph = std::move(loaded);
    return true;
}

bool MaterialGraph::Save(const std::filesystem::path& path, std::string& error) const
{
    return Validate(error) && WriteAssetFile(path, Serialize(), error);
}

bool MaterialGraph::Load(const std::filesystem::path& path, MaterialGraph& graph, std::string& error)
{
    std::error_code filesystemError;
    const auto size = std::filesystem::file_size(path, filesystemError);
    if (filesystemError || size > 8 * 1024 * 1024)
    {
        error = "Cannot read material graph: " + path.generic_string();
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input)
    {
        error = "Failed to read material graph: " + path.generic_string();
        return false;
    }
    return Deserialize(contents.str(), graph, error);
}






