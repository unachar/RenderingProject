#include "pch.h"
#include "materialgraphnodes.h"

namespace MaterialGraphNodes
{
    void InitNode(MaterialGraphNode& node)
    {
        switch (node.Kind)
        {
            case Kind::ToonLightRamp:
                node.Value = {0.0f, static_cast<float>(ToonClass::Cloth), 0.0f, 0.0f}; // 色ランプ、衣装
                break;
            case Kind::MatCap:
                node.Value = {static_cast<float>(MaterialBlendMode::Normal), 0.0f, 0.0f, 0.0f};
                break;
            case Kind::MultiColorRamp:
                node.Params = DefaultRampParams();
                break;
            case Kind::MaterialFunctionCall:
                // シグネチャのキャッシュ：[出力数, 出力の成分数 x4, 入力数, 入力の成分数 x8]。関数を選ぶと更新される
                node.Params = {1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1};
                break;
            case Kind::TextureParameter2D:
                node.Value = {static_cast<float>(MaterialSamplerType::Color), 0.0f, 0.0f, 0.0f};
                node.Text = "TextureParam";
                node.Params = {0.5f, 0.5f, 0.5f, 1.0f}; // 未設定のときのフォールバック色（RGBA）
                break;
            case Kind::TwoSidedSign:
                node.Value = {0.0f, 0.0f, 0.0f, 0.0f}; // 0 = +1 / -1、1 = 1 / 0
                break;
            case Kind::FlipbookUV:
                node.Value = {static_cast<float>(LoopMode::Repeat), 0.0f, 0.0f, 0.0f};
                break;
            case Kind::FlowMap:
                node.Value = {static_cast<float>(MaterialSamplerType::Linear), 0.0f, 0.0f, 0.0f};
                break;
            case Kind::RgbCurves:
                node.Value = {static_cast<float>(Interp::Smooth), 0.0f, 0.0f, 0.0f};
                node.Params = DefaultCurveParams();
                break;
            case Kind::PackedTextureChannels:
                node.Value = {static_cast<float>(MaterialSamplerType::Linear), 0.0f, 0.0f, 0.0f}; // サンプラー、プリセット
                break;
            case Kind::TriplanarNormalCorrected:
                node.Value = {0.0f, 0.0f, 0.0f, 0.0f}; // 出力空間：0 = 接空間（Material Output の Normal 用）、1 = ワールド
                break;
            case Kind::CurvatureMask:
                node.Value = {0.0f, 0.0f, 0.0f, 0.0f};
                break;
            case Kind::MaterialLayerBlend:
                node.Value = {0.0f, 0.0f, 0.0f, 0.0f}; // 0 = マスクをそのまま、1 = Height でマスクを締める
                break;
            case Kind::ThicknessMap:
                node.Value = {0.0f, 2.0f, 0.0f, 0.0f}; // 反転なし、チャンネル R
                break;
            case Kind::SceneDepth:
            case Kind::SceneColor:
                node.Value = {0.0f, 0.0f, 0.0f, 0.0f};
                break;
            case Kind::CustomHlsl:
                node.Value = {2.0f, 1.0f, 0.0f, 0.0f}; // 入力 2 個、出力 float
                node.Text = "lerp(a, b, 0.5)";
                node.PinNames = {"a", "b", "c", "d"};
                break;
            case Kind::DynamicSwitch:
                node.Value = {0.5f, 0.0f, 0.0f, 0.0f}; // しきい値（Condition > しきい値で True）
                break;
            case Kind::AudioSpectrum:
                node.Value = {0.0f, 0.0f, 0.0f, 0.0f}; // 0 = バンド番号、1 = 周波数 Hz
                break;
            case Kind::CurveAtlas:
                node.Value = {static_cast<float>(Interp::Linear), static_cast<float>(LoopMode::Clamp), 0.0f, 0.0f};
                node.Params = DefaultAtlasParams();
                break;
            case Kind::FunctionInput:
                node.Value = {1.0f, 0.0f, 0.0f, 0.0f}; // 成分数、入力の番号
                node.Params = {0.0f, 0.0f, 0.0f, 0.0f}; // 既定値（関数内のプレビューと、未接続のときの値）
                node.Text = "Input";
                break;
            case Kind::FunctionOutput:
                node.Value = {1.0f, 0.0f, 0.0f, 0.0f}; // 成分数（宣言）、出力の番号
                node.Text = "Output";
                break;
            default:
                break;
        }
    }

    const char* CategoryName(Kind kind)
    {
        switch (kind)
        {
            case Kind::ToonLightRamp:
            case Kind::FaceSdfShadow:
            case Kind::HairAnisotropicHighlight:
            case Kind::DirectionalRimLight:
            case Kind::MatCap:
            case Kind::MekakureMask:
            case Kind::EyeHighlight:
                return "Toon";
            case Kind::MultiColorRamp:
            case Kind::RgbCurves:
            case Kind::HsvAdjust:
                return "Color";
            case Kind::MaterialFunctionCall:
            case Kind::FunctionInput:
            case Kind::FunctionOutput:
                return "Material Function";
            case Kind::TextureParameter2D:
            case Kind::PackedTextureChannels:
            case Kind::FlipbookUV:
            case Kind::FlowMap:
            case Kind::ThicknessMap:
                return "Texture";
            case Kind::TwoSidedSign:
            case Kind::CurvatureMask:
            case Kind::DynamicSwitch:
            case Kind::CustomHlsl:
                return "Utility";
            case Kind::DetailNormalBlend:
            case Kind::TriplanarNormalCorrected:
                return "Normal / Height";
            case Kind::MaterialLayerBlend:
            case Kind::ThinFilmInterference:
            case Kind::AnisotropyDirection:
            case Kind::ClearCoatControl:
            case Kind::SubsurfaceColor:
                return "Shading";
            case Kind::SceneDepth:
            case Kind::DepthFade:
            case Kind::SceneColor:
                return "Scene";
            case Kind::AudioSpectrum:
            case Kind::BeatPhase:
            case Kind::CurveAtlas:
                return "Live";
            default:
                return "Math";
        }
    }

    int FixedOutputCount(Kind kind)
    {
        switch (kind)
        {
            case Kind::PackedTextureChannels:
            case Kind::BeatPhase:
            case Kind::MaterialFunctionCall:
                return 4;
            case Kind::FlowMap:
                return 3;
            case Kind::ThicknessMap:
                return 2;
            case Kind::MaterialLayerBlend:
                return 8;
            default:
                return 0;
        }
    }
} // namespace MaterialGraphNodes
