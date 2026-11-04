#include "pch.h"
#include "materialgraphext.h"
#include "materialgraphcustomhlsl.h"
#include "materialgraphmath.h"
#include "materialgraphnodes.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace MaterialGraphExt
{
    namespace
    {
        using namespace MaterialGraphNodes;
        using MaterialGraphEval::Context;
        using MaterialGraphEval::Value;
        namespace GM = MaterialGraphMath;

        Value MakeValue(const float* v, int n)
        {
            Value r;
            for (int i = 0; i < n && i < 4; ++i)
            {
                r.V[i] = v[i];
            }
            r.N = n;
            return r;
        }
        Value Scalar(float x)
        {
            Value r;
            r.V[0] = std::isfinite(x) ? x : 0.0f;
            r.N = 1;
            return r;
        }
        Value Vec2(float x, float y)
        {
            const float v[2] = {x, y};
            return MakeValue(v, 2);
        }
        Value Vec3(const float* v)
        {
            return MakeValue(v, 3);
        }
        Value Vec4(float x, float y, float z, float w)
        {
            const float v[4] = {x, y, z, w};
            return MakeValue(v, 4);
        }
        // 入力を n 成分へ（スカラーは全成分へ、足りない成分は 0）
        // out は n 個ぶんだけ書く（呼び出し側は float[3] / float[2] を渡すので、4 つ書くとスタックを壊す）
        void Get(const Value* in, size_t count, size_t i, int n, float* out)
        {
            for (int c = 0; c < n && c < 4; ++c)
            {
                out[c] = 0.0f;
            }
            if (i >= count)
            {
                return;
            }
            const Value& v = in[i];
            for (int c = 0; c < n && c < 4; ++c)
            {
                out[c] = v.N == 1 ? v.V[0] : (c < v.N ? v.V[c] : 0.0f);
            }
        }
        float S(const Value* in, size_t count, size_t i)
        {
            return i < count && std::isfinite(in[i].V[0]) ? in[i].V[0] : 0.0f;
        }

        bool Sample(const MaterialGraphEval::TextureSampler& sampler, AssetHandle texture, float u, float v, bool raw, float* out)
        {
            out[0] = out[1] = out[2] = out[3] = 0.0f;
            if (texture == 0 || !sampler)
            {
                return false;
            }
            return sampler(texture, u, v, raw, out);
        }

        void DecodeNormal(float* c)
        {
            float n[3] = {c[0] * 2.0f - 1.0f, c[1] * 2.0f - 1.0f, c[2] * 2.0f - 1.0f};
            if (c[2] <= 0.0f) // BC5 は Z を再構成する
            {
                n[2] = std::sqrt(std::clamp(1.0f - n[0] * n[0] - n[1] * n[1], 0.0f, 1.0f));
            }
            c[0] = n[0];
            c[1] = n[1];
            c[2] = n[2];
            c[3] = 1.0f;
        }

        std::shared_ptr<const MaterialGraphCustomHlsl::Program> CompileCustom(const MaterialGraphNode& node, const Value* in, size_t count)
        {
            static thread_local std::unordered_map<std::string, std::shared_ptr<const MaterialGraphCustomHlsl::Program>> cache;
            const int inputCount = std::clamp(static_cast<int>(node.Value[0]), 0, kMaxCustomInputs);
            const int outputDims = std::clamp(static_cast<int>(node.Value[1]), 1, 4);
            std::vector<std::string> names;
            std::vector<int> dims;
            std::string key = node.Text + "|" + std::to_string(outputDims);
            for (int i = 0; i < inputCount; ++i)
            {
                names.push_back(static_cast<size_t>(i) < node.PinNames.size() ? node.PinNames[static_cast<size_t>(i)] : std::string(1, static_cast<char>('a' + i)));
                dims.push_back(static_cast<size_t>(i) < count ? std::clamp(in[i].N, 1, 4) : 1);
                key += "|" + names.back() + ":" + std::to_string(dims.back());
            }
            const auto found = cache.find(key);
            if (found != cache.end())
            {
                return found->second;
            }
            if (cache.size() > 256)
            {
                cache.clear();
            }
            const auto compiled = MaterialGraphCustomHlsl::Compile(node.Text, names, dims, outputDims);
            cache[key] = compiled.Ok() ? compiled.Result : nullptr;
            return cache[key];
        }
    } // namespace

    float SyntheticSpectrum(int band, float time)
    {
        const float b = static_cast<float>(std::clamp(band, 0, MaterialGraphNodes::kAudioBands - 1));
        const float tilt = 1.0f - 0.65f * b / static_cast<float>(MaterialGraphNodes::kAudioBands - 1);
        return std::clamp((0.5f + 0.5f * std::sin(time * (1.7f + 0.13f * b) + b * 0.9f)) * tilt, 0.0f, 1.0f);
    }
    float HzToBand(float hz)
    {
        const float k = std::log((std::max)(hz, 20.0f) / 20.0f) / std::log(1000.0f) * static_cast<float>(MaterialGraphNodes::kAudioBands - 1);
        return std::clamp(k, 0.0f, static_cast<float>(MaterialGraphNodes::kAudioBands - 1));
    }
    float BandToHz(float band)
    {
        return 20.0f * std::pow(1000.0f, band / static_cast<float>(MaterialGraphNodes::kAudioBands - 1));
    }

    bool Evaluate(const MaterialGraphNode& node, const Context& ctx, const MaterialGraphEval::TextureSampler& sampler, const Value* in,
                  size_t n, uint32_t linked, Result& r)
    {
        const auto isLinked = [&](size_t i) { return ((linked >> i) & 1u) != 0; };
        const auto v3 = [&](size_t i, float* o) { Get(in, n, i, 3, o); };
        const auto v2 = [&](size_t i, float* o) { Get(in, n, i, 2, o); };
        const auto s = [&](size_t i) { return S(in, n, i); };
        const float up[3] = {0, 1, 0};
        const float forward[3] = {0, 0, 1};
        r = Result{};
        switch (node.Kind)
        {
            // ------------------------------------------------------------------ A. トゥーン
            case Kind::ToonLightRamp: {
                float albedo[3], normal[3], light[3], c1[3], c2[3], c3[3];
                v3(0, albedo);
                v3(1, normal);
                v3(2, light);
                v3(7, c1);
                v3(8, c2);
                v3(9, c3);
                Norm3(normal, forward);
                Norm3(light, up);
                const int toonClass = static_cast<int>(node.Value[1]);
                const float q = ToonBand(Dot3(normal, light), ToonWrap(toonClass), s(3), s(4), s(5));
                float color[3];
                float texel[4];
                if (node.Value[0] > 0.5f && Sample(sampler, node.Texture, std::clamp(q, 0.02f, 0.98f), 0.5f, false, texel))
                {
                    color[0] = texel[0];
                    color[1] = texel[1];
                    color[2] = texel[2];
                }
                else
                {
                    ToonRampColor(q, c1, c2, c3, color);
                }
                const float amount = Sat(s(6) * s(10));
                r.Primary = Vec4(Lerp(albedo[0], albedo[0] * color[0], amount), Lerp(albedo[1], albedo[1] * color[1], amount),
                                 Lerp(albedo[2], albedo[2] * color[2], amount), q);
                return true;
            }
            case Kind::FaceSdfShadow: {
                float uv[2], light[3], fwd[3], right[3];
                v2(0, uv);
                v3(1, light);
                v3(2, fwd);
                v3(3, right);
                float a[4], b[4];
                float sdfA = 0.5f, sdfB = 0.5f;
                if (Sample(sampler, node.Texture, uv[0], uv[1], true, a) && Sample(sampler, node.Texture, 1.0f - uv[0], uv[1], true, b))
                {
                    sdfA = a[0];
                    sdfB = b[0];
                }
                const float lit = FaceSdfLit(sdfA, sdfB, fwd, right, light, s(4), s(5), s(6) > 0.5f);
                r.Primary = Scalar(Lerp(1.0f, lit, Sat(s(8))));
                return true;
            }
            case Kind::HairAnisotropicHighlight: {
                float t[3], nn[3], vv[3], ll[3], color[3];
                v3(0, t);
                v3(1, nn);
                v3(2, vv);
                v3(3, ll);
                v3(10, color);
                Norm3(nn, forward);
                Norm3(vv, forward);
                Norm3(ll, up);
                const float h = HairHighlight(t, nn, vv, ll, s(4), s(5), s(6), s(7)) * s(8) * s(9);
                r.Primary = Vec4(color[0] * h, color[1] * h, color[2] * h, h);
                return true;
            }
            case Kind::DirectionalRimLight: {
                float nn[3], vv[3], ll[3], color[3];
                v3(0, nn);
                v3(1, vv);
                v3(2, ll);
                v3(3, color);
                Norm3(nn, forward);
                Norm3(vv, forward);
                Norm3(ll, up);
                const float rim = ToonRim(nn, vv, ll, s(4), s(5), s(7)) * s(6);
                r.Primary = Vec4(color[0] * rim, color[1] * rim, color[2] * rim, rim);
                return true;
            }
            case Kind::MatCap: {
                float vn[3], base[3];
                v3(0, vn);
                v3(3, base);
                Norm3(vn, forward);
                const float angle = s(1) * 6.28318531f;
                const float cs = std::cos(angle), sn = std::sin(angle);
                const float x = vn[0] * cs - vn[1] * sn;
                const float y = vn[0] * sn + vn[1] * cs;
                float texel[4];
                float mc[3];
                if (Sample(sampler, node.Texture, x * 0.5f + 0.5f, 1.0f - (y * 0.5f + 0.5f), false, texel))
                {
                    mc[0] = texel[0];
                    mc[1] = texel[1];
                    mc[2] = texel[2];
                }
                else
                {
                    // MatCap が無いときの標準の球面シェーディング（左上からの光）
                    const float l = Sat(0.5f + 0.5f * (x * 0.3f + y * 0.5f + vn[2] * 0.8f) / 1.0f);
                    mc[0] = mc[1] = mc[2] = 0.15f + 0.85f * l;
                }
                const int mode = static_cast<int>(node.Value[0]);
                const float strength = Sat(s(2));
                float out[3];
                for (int i = 0; i < 3; ++i)
                {
                    out[i] = Lerp(base[i], GM::BlendChannel(mode, base[i], mc[i]), strength);
                }
                r.Primary = Vec3(out);
                return true;
            }
            case Kind::MekakureMask: {
                float nn[3], vv[3];
                v3(5, nn);
                v3(6, vv);
                Norm3(nn, forward);
                Norm3(vv, forward);
                const float hair = Sat(s(0)), eye = Sat(s(1)), opacity = Sat(s(2));
                const float fadeWidth = (std::max)(s(3), 1e-3f);
                const float dependence = Sat(s(4));
                const float fade = Smooth(0.0f, fadeWidth, Sat(Dot3(nn, vv)));
                const float transmit = Lerp(1.0f, hair, opacity);       // 既存の MekakureMaskPS と同じ式
                const float effect = Lerp(1.0f, fade, dependence) * eye; // 横顔 / 後ろ向きでは効かない
                r.Primary = Scalar(Lerp(1.0f, transmit, effect));
                return true;
            }
            case Kind::MultiColorRamp: {
                float rgb[3];
                EvalRamp(RampKeys(node), RampMode(node), s(0), rgb);
                r.Primary = Vec3(rgb);
                return true;
            }
            case Kind::EyeHighlight: {
                float uv[2], vv[3], ll[3], center[2], color[3];
                v2(0, uv);
                v3(1, vv);
                v3(2, ll);
                v2(3, center);
                v3(8, color);
                Norm3(vv, forward);
                Norm3(ll, up);
                const float lx = Dot3(ll, ctx.Tangent), ly = Dot3(ll, ctx.Bitangent);
                const float vx = Dot3(vv, ctx.Tangent), vy = Dot3(vv, ctx.Bitangent);
                const float shift = s(9);
                const float ox = (lx - vx * 0.5f) * shift;
                const float oy = (ly - vy * 0.5f) * shift;
                const float h = EyeHighlightShape(uv[0] - center[0] - ox, uv[1] - center[1] - oy, s(4), s(5)) * s(6) * s(7);
                r.Primary = Vec4(color[0] * h, color[1] * h, color[2] * h, h);
                return true;
            }
            case Kind::MaterialFunctionCall:
                return false; // materialgraphlower.h でインライン展開してから評価する
            case Kind::TextureParameter2D: {
                float uv[2];
                v2(0, uv);
                const int sampleType = std::clamp(static_cast<int>(node.Value[0]), 0, 2);
                float texel[4];
                if (Sample(sampler, node.Texture, uv[0], uv[1], sampleType != 0, texel))
                {
                    if (sampleType == 1)
                    {
                        DecodeNormal(texel);
                    }
                    r.Primary = MakeValue(texel, 4);
                }
                else
                {
                    // 未設定のときのフォールバック（Params = RGBA）
                    const float fb[4] = {node.Params.size() > 0 ? node.Params[0] : 0.5f, node.Params.size() > 1 ? node.Params[1] : 0.5f,
                                         node.Params.size() > 2 ? node.Params[2] : 0.5f, node.Params.size() > 3 ? node.Params[3] : 1.0f};
                    r.Primary = MakeValue(fb, 4);
                }
                return true;
            }
            // ------------------------------------------------------------------ B. 汎用 / PBR
            case Kind::TwoSidedSign:
                r.Primary = Scalar(ctx.FrontFacing ? 1.0f : (node.Value[0] > 0.5f ? 0.0f : -1.0f));
                return true;
            case Kind::FlipbookUV: {
                float uv[2];
                v2(0, uv);
                // 行 / 列は 1〜4096 に制限（極端な入力で int が溢れない。GPU の MGX_Flipbook と同じ）
                const int rows = std::clamp(static_cast<int>(std::floor(std::clamp(s(1), 0.0f, 4096.0f) + 0.5f)), 1, 4096);
                const int cols = std::clamp(static_cast<int>(std::floor(std::clamp(s(2), 0.0f, 4096.0f) + 0.5f)), 1, 4096);
                const int frame = FlipbookFrame(s(5), s(4), s(3), rows * cols, static_cast<LoopMode>(std::clamp(static_cast<int>(node.Value[0]), 0, 2)));
                const int col = frame % cols;
                const int row = frame / cols;
                r.Primary = Vec2((uv[0] + static_cast<float>(col)) / static_cast<float>(cols), (uv[1] + static_cast<float>(row)) / static_cast<float>(rows));
                return true;
            }
            case Kind::FlowMap: {
                float uv[2], flow[2];
                v2(0, uv);
                float texel[4];
                if (isLinked(4))
                {
                    v2(4, flow);
                }
                else if (Sample(sampler, node.Texture, uv[0], uv[1], true, texel))
                {
                    flow[0] = texel[0] * 2.0f - 1.0f;
                    flow[1] = texel[1] * 2.0f - 1.0f;
                }
                else
                {
                    flow[0] = flow[1] = 0.0f;
                }
                const float t = s(1) * s(2);
                const float p0 = Frac(t);
                const float p1 = Frac(t + 0.5f);
                const float k = s(3);
                r.Slots[0] = Vec2(uv[0] - flow[0] * k * p0, uv[1] - flow[1] * k * p0);
                r.Slots[1] = Vec2(uv[0] - flow[0] * k * p1, uv[1] - flow[1] * k * p1);
                r.Slots[2] = Scalar(std::fabs(2.0f * p0 - 1.0f));
                r.SlotCount = 3;
                r.Primary = r.Slots[0];
                return true;
            }
            case Kind::RgbCurves: {
                float color[3];
                v3(0, color);
                const Interp mode = node.Value[0] > 0.5f ? Interp::Smooth : Interp::Linear;
                const auto master = CurveKeys(node.Params, 0);
                float out[3];
                for (int c = 0; c < 3; ++c)
                {
                    out[c] = EvalCurve(CurveKeys(node.Params, 1 + c), EvalCurve(master, color[c], mode), mode);
                    out[c] = Lerp(color[c], out[c], Sat(s(1)));
                }
                r.Primary = Vec3(out);
                return true;
            }
            case Kind::HsvAdjust: {
                float color[3], out[3];
                v3(0, color);
                HsvAdjust(color, s(1), s(2), s(3), out);
                r.Primary = Vec3(out);
                return true;
            }
            case Kind::PackedTextureChannels: {
                float uv[2];
                v2(0, uv);
                const int sampleType = std::clamp(static_cast<int>(node.Value[0]), 0, 2);
                float texel[4];
                if (!Sample(sampler, node.Texture, uv[0], uv[1], sampleType != 0, texel))
                {
                    texel[0] = texel[1] = texel[2] = texel[3] = 1.0f;
                }
                r.Primary = MakeValue(texel, 4);
                for (int c = 0; c < 4; ++c)
                {
                    r.Slots[c] = Scalar(texel[c]);
                }
                r.SlotCount = 4;
                return true;
            }
            case Kind::DetailNormalBlend: {
                float base[3], detail[3];
                v3(0, base);
                v3(1, detail);
                Norm3(base, forward);
                const float amount = s(2) * s(3);
                float d[3] = {detail[0] * amount, detail[1] * amount, detail[2]};
                Norm3(d, forward);
                // 既存の Blend Angle Corrected Normals と同じ式
                const float t[3] = {base[0], base[1], base[2] + 1.0f};
                const float u[3] = {-d[0], -d[1], d[2]};
                const float k = Dot3(t, u) / (std::max)(t[2], 1e-5f);
                float out[3] = {t[0] * k - u[0], t[1] * k - u[1], t[2] * k - u[2]};
                Norm3(out, forward);
                r.Primary = Vec3(out);
                return true;
            }
            case Kind::TriplanarNormalCorrected: {
                float p[3], nWorld[3];
                v3(0, p);
                v3(1, nWorld);
                Norm3(nWorld, forward);
                const float scale = s(2), sharp = (std::max)(s(3), 0.01f), intensity = s(4);
                const float q[3] = {p[0] * scale, p[1] * scale, p[2] * scale};
                float tn[3][4];
                const float uvs[3][2] = {{q[1], q[2]}, {q[0], q[2]}, {q[0], q[1]}};
                for (int a = 0; a < 3; ++a)
                {
                    if (!Sample(sampler, node.Texture, uvs[a][0], uvs[a][1], true, tn[a]))
                    {
                        tn[a][0] = tn[a][1] = 0.5f;
                        tn[a][2] = tn[a][3] = 1.0f;
                    }
                    DecodeNormal(tn[a]);
                    tn[a][0] *= intensity;
                    tn[a][1] *= intensity;
                }
                float w[3];
                float total = 0.0f;
                for (int a = 0; a < 3; ++a)
                {
                    w[a] = std::pow(std::fabs(nWorld[a]) + 1e-5f, sharp);
                    total += w[a];
                }
                for (float& x : w)
                {
                    x /= (std::max)(total, 1e-6f);
                }
                float world[3];
                TriplanarNormalWorld(tn[0], tn[1], tn[2], nWorld, w, world);
                if (node.Value[0] > 0.5f)
                {
                    r.Primary = Vec3(world);
                }
                else
                {
                    const float t[3] = {ctx.Tangent[0], ctx.Tangent[1], ctx.Tangent[2]};
                    const float b[3] = {ctx.Bitangent[0], ctx.Bitangent[1], ctx.Bitangent[2]};
                    float out[3] = {Dot3(world, t), Dot3(world, b), Dot3(world, nWorld)};
                    Norm3(out, forward);
                    r.Primary = Vec3(out);
                }
                return true;
            }
            case Kind::CurvatureMask:
                r.Primary = Scalar(CurvatureMask(s(0), s(1), s(2), std::clamp(static_cast<int>(node.Value[0]), 0, 2)));
                return true;
            case Kind::MaterialLayerBlend: {
                float mask = Sat(s(16));
                if (node.Value[0] > 0.5f)
                {
                    // 高さで境界を作る（Height Lerp と同じ考え方：高い方が先に残る）
                    const float ha = s(7), hb = s(15), depth = 0.2f;
                    const float ma = (std::max)(ha + (1.0f - mask), hb + mask) - depth;
                    const float wa = (std::max)(ha + (1.0f - mask) - ma, 0.0f);
                    const float wb = (std::max)(hb + mask - ma, 0.0f);
                    mask = wa + wb > 1e-6f ? wb / (wa + wb) : mask;
                }
                // 属性の並び：Base Color(3) Metallic Roughness Normal(3) AO Emissive(3) Opacity Height
                static const int widths[8] = {3, 1, 1, 3, 1, 3, 1, 1};
                for (int slot = 0; slot < 8; ++slot)
                {
                    float a[4], b[4], o[4];
                    Get(in, n, static_cast<size_t>(slot), widths[slot], a);
                    Get(in, n, static_cast<size_t>(slot + 8), widths[slot], b);
                    for (int c = 0; c < 4; ++c)
                    {
                        o[c] = Lerp(a[c], b[c], mask);
                    }
                    if (slot == 3)
                    {
                        Norm3(o, forward);
                    }
                    r.Slots[slot] = MakeValue(o, widths[slot]);
                }
                r.SlotCount = 8;
                r.Primary = r.Slots[0];
                return true;
            }
            case Kind::ThinFilmInterference: {
                float rgb[3];
                ThinFilm(s(2), s(0), s(1), s(3), rgb);
                r.Primary = Vec3(rgb);
                return true;
            }
            case Kind::AnisotropyDirection: {
                float t[3], nn[3], dir[2];
                v3(0, t);
                v2(1, dir);
                v3(4, nn);
                Norm3(nn, forward);
                const float proj = Dot3(nn, t);
                float tt[3] = {t[0] - nn[0] * proj, t[1] - nn[1] * proj, t[2] - nn[2] * proj};
                Norm3(tt, up);
                const float bb[3] = {nn[1] * tt[2] - nn[2] * tt[1], nn[2] * tt[0] - nn[0] * tt[2], nn[0] * tt[1] - nn[1] * tt[0]};
                float dl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1]);
                const float dx = dl > 1e-6f ? dir[0] / dl : 1.0f;
                const float dy = dl > 1e-6f ? dir[1] / dl : 0.0f;
                const float a = s(3) * 6.28318531f;
                const float cs = std::cos(a), sn = std::sin(a);
                const float rx = dx * cs - dy * sn, ry = dx * sn + dy * cs;
                float d[3] = {tt[0] * rx + bb[0] * ry, tt[1] * rx + bb[1] * ry, tt[2] * rx + bb[2] * ry};
                Norm3(d, tt);
                r.Primary = Vec4(d[0], d[1], d[2], std::clamp(s(2), -1.0f, 1.0f));
                return true;
            }
            case Kind::ClearCoatControl:
                r.Primary = Vec4(Sat(s(0) * s(2)), Sat(s(1)), 0.0f, 1.0f);
                return true;
            case Kind::SubsurfaceColor: {
                float color[3];
                v3(0, color);
                const float k = Sat(s(2) * s(3));
                const float thick = Lerp(1.0f, 0.4f, Sat(s(1)));
                const float out[3] = {color[0] * k * thick, color[1] * k * thick, color[2] * k * thick};
                r.Primary = Vec3(out);
                return true;
            }
            case Kind::ThicknessMap: {
                float uv[2];
                v2(0, uv);
                float texel[4];
                float v = 0.5f;
                if (Sample(sampler, node.Texture, uv[0], uv[1], true, texel))
                {
                    v = Sat(GM::ConvertChannel(texel, std::clamp(static_cast<int>(node.Value[1]), 0, GM::kConvertChannelCount - 1)));
                }
                if (node.Value[0] > 0.5f)
                {
                    v = 1.0f - v;
                }
                const float t = Sat(v * s(1) + s(2));
                r.Slots[0] = Scalar(Lerp(s(3), s(4), t));
                r.Slots[1] = Scalar(Sat(s(5)) * (1.0f - t));
                r.SlotCount = 2;
                r.Primary = r.Slots[0];
                return true;
            }
            // ------------------------------------------------------------------ C. エフェクト / ライブ
            case Kind::SceneDepth: {
                const float d = ctx.SceneDepth;
                if (node.Value[0] > 0.5f)
                {
                    // 深度バッファの値（標準 Z：far (d - near) / (d (far - near))、逆 Z は 1 - それ）
                    const float zn = ctx.NearPlane, zf = ctx.FarPlane;
                    const float z = Sat(zf * (d - zn) / (std::max)(d * (zf - zn), 1e-6f));
                    r.Primary = Scalar(ctx.ReverseZ ? 1.0f - z : z);
                }
                else
                {
                    r.Primary = Scalar(d);
                }
                return true;
            }
            case Kind::DepthFade: {
                const float fade = Sat((ctx.SceneDepth - ctx.PixelDepth) / (std::max)(s(1), 1e-4f));
                r.Primary = Scalar(fade * s(0));
                return true;
            }
            case Kind::SceneColor: {
                const float shade = 0.6f + 0.4f * Sat(ctx.UV[1]);
                const float c[3] = {ctx.SceneColor[0] * shade, ctx.SceneColor[1] * shade, ctx.SceneColor[2] * shade};
                r.Primary = Vec3(c);
                return true;
            }
            case Kind::CustomHlsl: {
                const auto program = CompileCustom(node, in, n);
                if (!program)
                {
                    r.Primary.N = std::clamp(static_cast<int>(node.Value[1]), 1, 4);
                    return false;
                }
                std::vector<std::array<float, 4>> inputs;
                for (size_t i = 0; i < program->InputNames.size(); ++i)
                {
                    std::array<float, 4> a{};
                    for (int c = 0; c < 4; ++c)
                    {
                        a[static_cast<size_t>(c)] = i < n ? (in[i].N == 1 ? in[i].V[0] : in[i].V[c]) : 0.0f;
                    }
                    inputs.push_back(a);
                }
                float out[4];
                MaterialGraphCustomHlsl::Evaluate(*program, inputs, out);
                r.Primary = MakeValue(out, program->OutputDims);
                return true;
            }
            case Kind::DynamicSwitch: {
                const bool condition = s(0) > node.Value[0];
                const int width = (std::max)(in[1].N, in[2].N);
                r.Primary = MaterialGraphEval::Promote(condition ? in[1] : in[2], width);
                return true;
            }
            case Kind::AudioSpectrum: {
                float band = s(0);
                if (node.Value[0] > 0.5f)
                {
                    band = HzToBand(band);
                }
                band = std::clamp(band, 0.0f, static_cast<float>(kAudioBands - 1));
                const int b0 = static_cast<int>(std::floor(band));
                const int b1 = (std::min)(b0 + 1, kAudioBands - 1);
                const float f = band - static_cast<float>(b0);
                const auto magnitude = [&](int b) {
                    return ctx.Audio ? ctx.Audio->Bands[b] : SyntheticSpectrum(b, ctx.Time);
                };
                r.Primary = Scalar(Lerp(magnitude(b0), magnitude(b1), f) * s(2));
                return true;
            }
            case Kind::BeatPhase: {
                const double time = isLinked(3) ? static_cast<double>(s(3)) : (ctx.AudioTime != 0.0 ? ctx.AudioTime : static_cast<double>(ctx.Time));
                const auto beat = ComputeBeat(time, s(0), s(1), s(2));
                r.Slots[0] = Scalar(beat.Phase);
                r.Slots[1] = Scalar(beat.BeatIndex);
                r.Slots[2] = Scalar(beat.BeatTime);
                r.Slots[3] = Scalar(beat.Pulse);
                r.SlotCount = 4;
                r.Primary = Vec4(beat.Phase, beat.BeatIndex, beat.BeatTime, beat.Pulse);
                return true;
            }
            case Kind::CurveAtlas: {
                const auto loop = static_cast<LoopMode>(std::clamp(static_cast<int>(node.Value[1]), 0, 2));
                const auto mode = static_cast<Interp>(std::clamp(static_cast<int>(node.Value[0]), 0, 2));
                const float rowCount = node.Texture != 0 ? (std::max)(1.0f, node.Value[2] < 1.0f ? 4.0f : node.Value[2])
                                                          : static_cast<float>((std::max)(1, static_cast<int>(AtlasRows(node).size())));
                const float row = std::clamp(s(1), 0.0f, rowCount - 1.0f);
                if (node.Texture != 0)
                {
                    // テクスチャのアトラス：行 = カーブ、チャンネル = Value[3]、行数 = Value[2]
                    const float rows = (std::max)(1.0f, node.Value[2] < 1.0f ? 4.0f : node.Value[2]);
                    const float u = ApplyLoop(s(0), loop, 0.0f, 1.0f);
                    const int channel = std::clamp(static_cast<int>(node.Value[3]), 0, 3);
                    float result = 0.0f;
                    float weightSum = 0.0f;
                    for (int i = 0; i < static_cast<int>(rows); ++i)
                    {
                        const float w = RowWeight(row, i);
                        if (w <= 0.0f)
                        {
                            continue;
                        }
                        float texel[4];
                        if (Sample(sampler, node.Texture, u, (static_cast<float>(i) + 0.5f) / rows, true, texel))
                        {
                            result += texel[channel] * w;
                            weightSum += w;
                        }
                    }
                    r.Primary = Scalar(weightSum > 0.0f ? result / weightSum : 0.0f);
                    return true;
                }
                const auto rows = AtlasRows(node);
                if (rows.empty())
                {
                    r.Primary = Scalar(0.0f);
                    return true;
                }
                // 時間は先頭の行の範囲でループさせる（行ごとの範囲は異なってもよい。ここでは 0〜最後のキー）
                float result = 0.0f;
                float weightSum = 0.0f;
                for (size_t i = 0; i < rows.size(); ++i)
                {
                    const float w = RowWeight(row, static_cast<int>(i));
                    if (w <= 0.0f || rows[i].empty())
                    {
                        continue;
                    }
                    const float x = ApplyLoop(s(0), loop, rows[i].front().X, rows[i].back().X);
                    float y[3];
                    EvalKeys(rows[i], x, mode, 1, y);
                    result += y[0] * w;
                    weightSum += w;
                }
                r.Primary = Scalar(weightSum > 0.0f ? result / weightSum : 0.0f);
                return true;
            }
            case Kind::FunctionInput: {
                // 関数グラフの中のプレビュー：既定値（Params）を出す
                const int dims = std::clamp(static_cast<int>(node.Value[0]), 1, 4);
                float v[4] = {0, 0, 0, 0};
                for (int i = 0; i < 4 && static_cast<size_t>(i) < node.Params.size(); ++i)
                {
                    v[i] = node.Params[static_cast<size_t>(i)];
                }
                r.Primary = MakeValue(v, dims);
                return true;
            }
            case Kind::FunctionOutput:
                if (n > 0)
                {
                    r.Primary = in[0]; // 関数内のプレビュー：渡される値
                }
                return true;
            default:
                return false;
        }
    }
} // namespace MaterialGraphExt
