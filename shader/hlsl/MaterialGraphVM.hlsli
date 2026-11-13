#ifndef MATERIAL_GRAPH_VM_INCLUDED
#define MATERIAL_GRAPH_VM_INCLUDED
// =====================================================================================================
// マテリアルグラフの描画時評価（GPU）
//  ・CPU 側（MaterialGraphVm::Compile / materialgraphcompiler.cpp）がグラフを小さな命令列にし、
//    R32G32B32A32_FLOAT の 1 行テクスチャ（マテリアルのテクスチャ表）へ置く
//  ・このファイルの EvaluateMaterialGraphVm がそれを画素ごとに解釈する。3D 位置で評価するので、
//    ソリッドテクスチャ / ノイズが UV の継ぎ目や伸びに影響されない
//  ・フォワード / ディファード / GPU 駆動 / Visibility 解決のどの経路でも同じ関数を使う
//  ・グラフを編集しても再コンパイル不要（命令列とパラメーターを書き換えるだけ）
// 命令：(opcode, 出力レジスタ, 命令のテクセル数, 引数テクセル数) + 引数（レジスタ番号を 4 個ずつ）+ 即値
// レジスタ：float4。スカラーは全成分に同じ値が入っている（そのまま成分ごとの演算に使える）
// 呼び出し側の前提：MaterialSurface.hlsli（LoadMaterialTexture / AssetMaterialParameters）と
// ProceduralTexture.hlsli（TextureNoise）が先に include されていること
// =====================================================================================================

// スタンドアロン（MaterialGraphGpuCS：ノードプレビュー / ベイク）で使うときの差し替え
//  MG_VM_STANDALONE      … AssetMaterialParameters を使う関数を除き、テクスチャは常に SampleLevel で読む
//  MG_VM_PROGRAM_TEXTURE … プログラムを読むテクスチャ（既定はマテリアルのテクスチャ表）
//  MG_VM_ROW             … プログラムの行（既定 0）
//  MG_VM_TIME_OVERRIDE   … 時刻（既定はプログラムの [0].x）
//  MG_VM_CAMERA_DISTANCE(input) … Camera Distance / Distance Fade の距離（既定は |WorldPos - CameraPos|）
//  MG_VM_PIXEL_DEPTH(input)     … Pixel Depth（既定は View 行列での z）
//  ※ 発光の動的ベイク（MaterialGraphGpuCS のモード 5）が、オブジェクトと実カメラの関係を渡すのに使う
#ifndef MG_VM_ROW
#define MG_VM_ROW 0
#endif
#define MG_VM_REGS 24          // MaterialGraphVm::kMaxRegisters と一致させる
#define MG_VM_MAX_TEXELS 1024u // MaterialGraphVm::kMaxTexels
#define MG_VM_MAX_STEPS 512u

// opcode（materialgraphcompiler.cpp の Op と同じ番号）
#define MG_OP_CONST 1
#define MG_OP_INPUT 2
#define MG_OP_UNARY 3
#define MG_OP_BINARY 4
#define MG_OP_ARITH 5
#define MG_OP_COMPONENT 6
#define MG_OP_PROMOTE4 7
#define MG_OP_ZEROFILL 8
#define MG_OP_SWIZZLE 9
#define MG_OP_APPEND 10
#define MG_OP_DOT3 11
#define MG_OP_CROSS 12
#define MG_OP_LENGTH 13
#define MG_OP_DISTANCE3 14
#define MG_OP_NORMALIZE3 15
#define MG_OP_TEXCOORD 16
#define MG_OP_ROTATE2 17
#define MG_OP_DESATURATE 18
#define MG_OP_IF 19
#define MG_OP_SPHEREMASK 20
#define MG_OP_CHEAPCONTRAST 21
#define MG_OP_VALUENOISE 22
#define MG_OP_REMAP 23
#define MG_OP_POSTERIZE 24
#define MG_OP_HUESHIFT 25
#define MG_OP_BLEND 26
#define MG_OP_RADIALGRADIENT 27
#define MG_OP_NOISE 28
#define MG_OP_WORLEY 29
#define MG_OP_NOISECOMBINE 30
#define MG_OP_VORONOI 31
#define MG_OP_BLENDANGLE 32
#define MG_OP_FLATTEN 33
#define MG_OP_ROTATEAXIS 34
#define MG_OP_BLACKBODY 35
#define MG_OP_TEXSAMPLE 36
#define MG_OP_CONVERTHEIGHT 37
#define MG_OP_CONVERTNORMAL 38
#define MG_OP_SOLIDVOLUME 39
#define MG_OP_TRIPLANAR 40
#define MG_OP_THREERAMP 41
#define MG_OP_FRESNEL 42
#define MG_OP_COLORADJUST 43
#define MG_OP_SLOPEMASK 44
#define MG_OP_HEIGHTLERPALPHA 45
#define MG_OP_TRANSFORM 46
#define MG_OP_DITHER 47
#define MG_OP_EXPAND4 48
#define MG_OP_SOLIDSTOCHASTIC 49

struct MaterialGraphVmInput
{
    float3 WorldPos;
    float3 Normal; // ジオメトリの法線（ワールド、正規化済み）
    float3 CameraPos;
    float2 UV;
    float4 VertexColor;
    float4x4 ObjectToWorld; // オブジェクトの World 行列（行ベクトル：world = local * M）
    float3 BoundsCenter;    // ローカルの範囲
    float3 BoundsExtents;
    float InstanceRandom; // オブジェクトごとの 0〜1
    float2 PixelPos;      // 画面上の画素位置（SV_Position.xy）
};

struct MaterialGraphVmOutput
{
    bool Active;
    int4 Out0; // 出力レジスタ：Base Color, Metallic, Roughness, Normal（-1 = 未接続）
    int4 Out1; // Emissive, Opacity, Opacity Mask, AO
    int4 Out2; // Height, Normal Intensity, Transmission, -1
    float4 BaseColor;
    float Metallic;
    float Roughness;
    float3 Normal; // 接空間
    float3 Emissive;
    float Opacity;
    float OpacityMask;
    float AO;
    float Height;
    float NormalIntensity;
    float Transmission;
};

#ifndef MG_VM_STANDALONE
// マテリアルのテクスチャ表にあるプログラムの番号 + 1（0 = 描画時評価なし）。AssetTextures.w の bit 16-25
uint MaterialGraphProgramSlot(AssetMaterialParameters material)
{
    return material.Surface.z > 0.5f ? ((material.Textures.w >> 16u) & 1023u) : 0u;
}
#endif

// オブジェクト空間の位置（world = local * M。M の行 0〜2 が軸、行 3 が平行移動）
float3 MaterialGraphObjectPosition(float3 worldPos, float4x4 world)
{
    float3 r0 = world[0].xyz;
    float3 r1 = world[1].xyz;
    float3 r2 = world[2].xyz;
    float3 delta = worldPos - world[3].xyz;
    float determinant = dot(r0, cross(r1, r2));
    if (abs(determinant) < 1e-12f)
    {
        return worldPos;
    }
    return float3(dot(delta, cross(r1, r2)), dot(delta, cross(r2, r0)), dot(delta, cross(r0, r1))) / determinant;
}

// ---------------------------------------------------------------------------- オブジェクト情報
// オブジェクトごとの乱数（PCG ハッシュ）
float MaterialGraphInstanceRandom(uint seed)
{
    uint state = seed * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    word = (word >> 22u) ^ word;
    return (word & 0xFFFFFFu) / 16777216.0f;
}

// 範囲（Extents が 0 = 不明）が無いときは -1〜1、種が無いときはオブジェクトの位置から乱数を作る
void MaterialGraphSetObject(inout MaterialGraphVmInput input, float4x4 world, float3 center, float3 extents, uint seed,
                            bool hasSeed)
{
    input.ObjectToWorld = world;
    bool valid = any(extents > 0.0f);
    input.BoundsCenter = valid ? center : float3(0, 0, 0);
    input.BoundsExtents = valid ? max(extents, 1e-5f) : float3(1, 1, 1);
    if (!hasSeed)
    {
        uint3 bits = asuint(world[3].xyz);
        seed = bits.x ^ (bits.y * 0x9E3779B9u) ^ (bits.z * 0x85EBCA6Bu);
    }
    input.InstanceRandom = MaterialGraphInstanceRandom(seed);
}

// ---------------------------------------------------------------------------- ヘルパー（グラフのノードと同じ式）
float MGVM_Hash(float3 p)
{
    p = frac(p * 0.3183099f + 0.1f) * 17.0f;
    return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float MGVM_ValueNoise(float3 x)
{
    float3 i = floor(x);
    float3 f = frac(x);
    f = f * f * (3.0f - 2.0f * f);
    return lerp(lerp(lerp(MGVM_Hash(i), MGVM_Hash(i + float3(1, 0, 0)), f.x),
                     lerp(MGVM_Hash(i + float3(0, 1, 0)), MGVM_Hash(i + float3(1, 1, 0)), f.x), f.y),
                lerp(lerp(MGVM_Hash(i + float3(0, 0, 1)), MGVM_Hash(i + float3(1, 0, 1)), f.x),
                     lerp(MGVM_Hash(i + float3(0, 1, 1)), MGVM_Hash(i + float3(1, 1, 1)), f.x), f.y),
                f.z);
}

float3 MGVM_Blend(int mode, float3 a, float3 b)
{
    float3 screen2 = 1 - 2 * (1 - a) * (1 - b);
    float3 overlay = lerp(2 * a * b, screen2, step(0.5f, a));
    float3 hard = lerp(2 * a * b, screen2, step(0.5f, b));
    float3 soft = lerp(2 * a * b + a * a * (1 - 2 * b), sqrt(max(a, 0)) * (2 * b - 1) + 2 * a * (1 - b), step(0.5f, b));
    float3 dodge = min(a / max(1 - b, 1e-5f), 1);
    float3 burn = 1 - min((1 - a) / max(b, 1e-5f), 1);
    switch (mode)
    {
        case 1: return a * b;
        case 2: return 1 - (1 - a) * (1 - b);
        case 3: return overlay;
        case 4: return soft;
        case 5: return hard;
        case 6: return dodge;
        case 7: return burn;
        case 8: return a + b;
        case 9: return a + b - 1;
        case 10: return abs(a - b);
        case 11: return min(a, b);
        case 12: return max(a, b);
        case 13: return a - b;
        default: return b;
    }
}

float3 MGVM_HueShift(float3 c, float shift)
{
    float mx = max(c.r, max(c.g, c.b));
    float mn = min(c.r, min(c.g, c.b));
    float d = mx - mn;
    float h = 0;
    if (d > 1e-6f)
    {
        h = mx == c.r ? (c.g - c.b) / d : (mx == c.g ? 2 + (c.b - c.r) / d : 4 + (c.r - c.g) / d);
        h = frac(h / 6);
    }
    float3 hsv = float3(frac(h + shift), mx > 1e-6f ? d / mx : 0, mx);
    float3 k = frac(hsv.x + float3(1.0f, 2.0f / 3.0f, 1.0f / 3.0f)) * 6;
    float3 p = saturate(abs(k - 3) - 1);
    return hsv.z * (1 + (p - 1) * hsv.y);
}

float MGVM_Voronoi(float2 uv, float jitter, bool cellValue)
{
    float2 c = floor(uv);
    float best = 1e9f;
    float value = 0;
    [unroll] for (int j = -1; j <= 1; ++j)
    {
        [unroll] for (int i = -1; i <= 1; ++i)
        {
            float2 g = c + float2(i, j);
            float2 p = g + 0.5f + (float2(MGVM_Hash(float3(g, 1.5f)), MGVM_Hash(float3(g, 2.5f))) - 0.5f) * jitter;
            float2 d = p - uv;
            float dd = dot(d, d);
            if (dd < best)
            {
                best = dd;
                value = MGVM_Hash(float3(g, 3.5f));
            }
        }
    }
    return cellValue ? value : sqrt(best);
}

float3 MGVM_BlackBody(float kelvin)
{
    float t = clamp(kelvin, 1000, 40000) / 100;
    float3 c;
    if (t <= 66)
    {
        c = float3(255, 99.4708025861f * log(t) - 161.1195681661f, t <= 19 ? 0 : 138.5177312231f * log(t - 10) - 305.0447927307f);
    }
    else
    {
        c = float3(329.698727446f * pow(t - 60, -0.1332047592f), 288.1221695283f * pow(t - 60, -0.0755148492f), 255);
    }
    c = saturate(c / 255);
    return lerp(pow((c + 0.055f) / 1.055f, 2.4f), c / 12.92f, step(c, 0.04045f));
}

float MGVM_NoiseCombine(int mode, float a, float b, float factor, bool clampResult)
{
    float result = a;
    if (mode == 0)
    {
        result = lerp(a, b, factor);
    }
    else if (mode == 9)
    {
        float t = saturate((b - a) / max(factor, 1e-4f) * 0.5f + 0.5f);
        t = t * t * (3 - 2 * t);
        result = lerp(a, b, t);
    }
    else
    {
        float op = a;
        if (mode == 1) op = a + b;
        else if (mode == 2) op = a - b;
        else if (mode == 3) op = a * b;
        else if (mode == 4) op = 1 - (1 - a) * (1 - b);
        else if (mode == 5) op = a < 0.5f ? 2 * a * b : 1 - 2 * (1 - a) * (1 - b);
        else if (mode == 6) op = min(a, b);
        else if (mode == 7) op = max(a, b);
        else if (mode == 8) op = abs(a - b);
        result = lerp(a, op, factor);
    }
    return clampResult ? saturate(result) : result;
}

// グラフ用のテクスチャ読み込み。Visibility 解決（コンピュート）は暗黙微分が無いので最上位ミップを読む
float4 MGVM_Sample(uint heap, float2 uv, SamplerState s)
{
#if defined(SHADER_VISIBILITY_RESOLVE) || defined(MG_VM_STANDALONE)
    return LoadMaterialTexture(heap).SampleLevel(s, uv, 0);
#else
    return SampleMaterialBiasCommon(LoadMaterialTexture(heap), s, uv);
#endif
}

float MGVM_ConvertHeight(float4 c, int channel, bool invert, float contrast, float bias)
{
    float v = channel == 1 ? (c.r + c.g + c.b) / 3 : channel == 2 ? c.r : channel == 3 ? c.g : channel == 4 ? c.b :
              channel == 5 ? c.a : channel == 6 ? max(c.r, max(c.g, c.b)) : dot(c.rgb, float3(0.2126f, 0.7152f, 0.0722f));
    v = saturate(v);
    v = invert ? 1 - v : v;
    return saturate((v - 0.5f) * contrast + 0.5f + bias);
}

float MGVM_ConvertTap(uint heap, float2 uv, SamplerState s, int channel, bool invert, float contrast, float bias)
{
    return MGVM_ConvertHeight(LoadMaterialTexture(heap).SampleLevel(s, uv, 0), channel, invert, contrast, bias);
}

float4 MGVM_UnpackNormal(float4 v)
{
    float3 n = v.xyz * 2 - 1;
    if (v.z <= 0)
    {
        n.z = sqrt(saturate(1 - dot(n.xy, n.xy)));
    }
    return float4(n, 1);
}

uint MGVM_Seed(float lo, float hi)
{
    return (uint)lo | ((uint)hi << 16u);
}

// ---- ヘックス タイル（Heitz & Neyret 2018）。MaterialGraphMath::HexGrid と同じ格子 / 重み
uint MGVM_TileHash(int2 v, uint seed)
{
    return TextureNoise::Hash(seed ^ TextureNoise::Hash((uint)v.x * 0x8da6b343u ^ TextureNoise::Hash((uint)v.y * 0xd8163841u)));
}

// タイルごとにランダムな位置 / 回転でずらした 3 枚を混ぜる → 拡大しても同じ模様が並ばない
float4 MGVM_HexSample(uint heap, float2 uv, SamplerState s, uint seed, float randomness)
{
    float2 st = uv * 3.4641016f;
    float2 k = float2(st.x, -0.57735027f * st.x + 1.15470054f * st.y);
    int2 b = (int2)floor(k);
    float3 f = float3(frac(k), 0);
    f.z = 1 - f.x - f.y;
    float sg = f.z < 0 ? 1.0f : 0.0f;
    float s2 = 2 * sg - 1;
    int i = (int)sg;
    float3 w = pow(max(float3(-f.z * s2, sg - f.y * s2, sg - f.x * s2), 0.0f), 4.0f);
    w /= max(dot(w, 1), 1e-6f);
    int2 v[3] = {b + int2(i, i), b + int2(i, 1 - i), b + int2(1 - i, i)};
    Texture2D source = LoadMaterialTexture(heap);
#if !defined(SHADER_VISIBILITY_RESOLVE) && !defined(MG_VM_STANDALONE)
    float2 dx = ddx(uv);
    float2 dy = ddy(uv);
#endif
    float4 result = 0;
    [unroll] for (int n = 0; n < 3; ++n)
    {
        uint h = MGVM_TileHash(v[n], seed);
        float angle = TextureNoise::Unit(TextureNoise::Hash(h ^ 0x68bc21ebu)) * 6.28318531f * randomness;
        float c = cos(angle);
        float sn = sin(angle);
        float2x2 m = float2x2(c, -sn, sn, c);
        float2 q = mul(m, uv) + float2(TextureNoise::Unit(h), TextureNoise::Unit(TextureNoise::Hash(h)));
#if defined(SHADER_VISIBILITY_RESOLVE) || defined(MG_VM_STANDALONE)
        result += source.SampleLevel(s, q, 0) * w[n];
#else
        result += source.SampleGrad(s, q, mul(m, dx), mul(m, dy)) * w[n]; // 勾配も回転させる（ミップを正しく選ぶ）
#endif
    }
    return result;
}

// ---------------------------------------------------------------------------- 本体
MaterialGraphVmOutput EvaluateMaterialGraphVm(uint programSlot, MaterialGraphVmInput input, SamplerState materialSampler)
{
    MaterialGraphVmOutput output = (MaterialGraphVmOutput)0;
    output.Active = false;
    output.Out0 = int4(-1, -1, -1, -1);
    output.Out1 = int4(-1, -1, -1, -1);
    output.Out2 = int4(-1, -1, -1, -1);
    if (programSlot == 0u)
    {
        return output;
    }
#ifdef MG_VM_PROGRAM_TEXTURE
    Texture2D<float4> program = MG_VM_PROGRAM_TEXTURE;
#else
    Texture2D program = LoadMaterialTexture(programSlot - 1u);
#endif
    float4 header = program.Load(int3(0, MG_VM_ROW, 0));
    output.Out0 = (int4)program.Load(int3(1, MG_VM_ROW, 0));
    output.Out1 = (int4)program.Load(int3(2, MG_VM_ROW, 0));
    output.Out2 = (int4)program.Load(int3(3, MG_VM_ROW, 0));
    uint pc = (uint)max(header.y, 0.0f);
    uint end = min((uint)max(header.z, 0.0f), MG_VM_MAX_TEXELS);
#ifdef MG_VM_TIME_OVERRIDE
    float time = MG_VM_TIME_OVERRIDE;
#else
    float time = header.x;
#endif
    bool worldSolid = (((uint)header.w) & 2u) != 0u;

    float3 normal = SafeNormalizeCommon(input.Normal, float3(0, 1, 0));
    float3 viewDirection = SurfaceViewDirectionCommon(input.WorldPos, input.CameraPos);
    float3 localPos = MaterialGraphObjectPosition(input.WorldPos, input.ObjectToWorld);
    float3 objectScale = float3(length(input.ObjectToWorld[0].xyz), length(input.ObjectToWorld[1].xyz),
                                length(input.ObjectToWorld[2].xyz));
    // ソリッド（3 方向投影）の向き：座標と同じ空間の法線を使う（オブジェクト空間なら回しても模様が付いてくる）
    float3 solidNormal = worldSolid ? normal
                                    : SafeNormalizeCommon(mul((float3x3)input.ObjectToWorld, normal), normal); // n_local ∝ n_world · Mᵀ

    float4 R[MG_VM_REGS];
    [unroll] for (uint r = 0; r < MG_VM_REGS; ++r)
    {
        R[r] = 0;
    }

    [loop] for (uint stepIndex = 0; stepIndex < MG_VM_MAX_STEPS && pc < end; ++stepIndex)
    {
        float4 head = program.Load(int3((int)(pc), MG_VM_ROW, 0));
        uint op = (uint)head.x;
        uint dst = min((uint)max(head.y, 0.0f), MG_VM_REGS - 1u);
        uint instructionTexels = max((uint)head.z, 1u);
        uint argTexels = (uint)head.w;
        uint4 i0 = argTexels > 0u ? (uint4)clamp(program.Load(int3((int)(pc + 1u), MG_VM_ROW, 0)), 0.0f, MG_VM_REGS - 1.0f)
                                  : uint4(0, 0, 0, 0);
        float4 imm = program.Load(int3((int)(pc + 1u + argTexels), MG_VM_ROW, 0));
        float4 a = R[i0.x];
        float4 b = R[i0.y];
        float4 c = R[i0.z];
        float4 d = R[i0.w];
        float4 result = 0;
        switch (op)
        {
            case MG_OP_CONST:
                result = imm;
                break;
            case MG_OP_INPUT:
            {
                uint which = (uint)imm.x;
                if (which == 0u) result = float4(input.UV, 0, 0);
                else if (which == 1u) result = float4(input.WorldPos, 0);
                else if (which == 2u) result = float4(localPos, 0);
                else if (which == 3u) result = float4(worldSolid ? input.WorldPos : localPos, 0);
                else if (which == 4u) result = float4(normal, 0);
                else if (which == 5u) result = float4(viewDirection, 0);
                else if (which == 6u) result = time.xxxx;
                else if (which == 7u) result = float4(reflect(-viewDirection, normal), 0);
                else if (which == 8u) result = input.VertexColor;
#ifdef MG_VM_CAMERA_DISTANCE
                else if (which == 9u) result = MG_VM_CAMERA_DISTANCE(input).xxxx;
#else
                else if (which == 9u) result = length(input.WorldPos - input.CameraPos).xxxx;
#endif
                else if (which == 10u) result = float4(input.ObjectToWorld[3].xyz, 0);
                else if (which == 11u) result = float4(objectScale, 0);
                else if (which == 12u) result = float4(SafeNormalizeCommon(input.ObjectToWorld[1].xyz, float3(0, 1, 0)), 0);
                else if (which == 13u) result = length(input.BoundsExtents * objectScale).xxxx;
                else if (which == 14u) result = float4(input.BoundsCenter, 0);
                else if (which == 15u) result = float4(input.BoundsExtents * 2, 0);
                else if (which == 16u) result = float4(input.BoundsCenter - input.BoundsExtents, 0);
                else if (which == 17u) result = float4(input.BoundsCenter + input.BoundsExtents, 0);
                else if (which == 18u)
                    result = float4((localPos - (input.BoundsCenter - input.BoundsExtents)) / (input.BoundsExtents * 2), 0);
                else if (which == 19u) result = input.InstanceRandom.xxxx;
                else if (which == 20u) result = float4(input.CameraPos, 0);
#ifdef MG_VM_PIXEL_DEPTH
                else if (which == 21u) result = MG_VM_PIXEL_DEPTH(input).xxxx;
#else
                else if (which == 21u) result = mul(float4(input.WorldPos, 1), View).z.xxxx;
#endif
                else
                {
                    float4 clip = mul(mul(float4(input.WorldPos, 1), View), Projection);
                    float2 ndc = clip.xy / (abs(clip.w) > 1e-6f ? clip.w : 1e-6f);
                    result = float4(ndc * float2(0.5f, -0.5f) + 0.5f, 0, 0);
                }
                break;
            }
            case MG_OP_UNARY:
            {
                uint kind = (uint)imm.x; // MaterialNodeKind::Abs からの番号
                if (kind == 0u) result = abs(a);
                else if (kind == 1u) result = ceil(a);
                else if (kind == 2u) result = floor(a);
                else if (kind == 3u) result = frac(a);
                else if (kind == 4u) result = round(a);
                else if (kind == 5u) result = trunc(a);
                else if (kind == 6u) result = sign(a);
                else if (kind == 7u) result = 1 - a;
                else if (kind == 8u) result = saturate(a);
                else if (kind == 9u) result = sqrt(a);
                else if (kind == 10u) result = sin(a);
                else if (kind == 11u) result = cos(a);
                else if (kind == 12u) result = tan(a);
                else if (kind == 13u) result = asin(a);
                else if (kind == 14u) result = acos(a);
                else if (kind == 15u) result = atan(a);
                else if (kind == 16u) result = exp(a);
                else result = log2(a);
                break;
            }
            case MG_OP_BINARY:
            {
                uint kind = (uint)imm.x; // MaterialNodeKind::Power からの番号
                if (kind == 0u) result = pow(a, b);
                else if (kind == 1u) result = min(a, b);
                else if (kind == 2u) result = max(a, b);
                else if (kind == 3u) result = fmod(a, b);
                else if (kind == 4u) result = step(a, b);
                else result = atan2(a, b);
                break;
            }
            case MG_OP_ARITH:
            {
                uint kind = (uint)imm.x;
                if (kind == 0u) result = a + b;
                else if (kind == 1u) result = a - b;
                else if (kind == 2u) result = a * b;
                else if (kind == 3u) result = a / b;
                else if (kind == 4u) result = lerp(a, b, c);
                else if (kind == 5u) result = clamp(a, b, c);
                else if (kind == 6u) result = smoothstep(a, b, c);
                else if (kind == 7u) result = (a + b) * c;
                else
                {
                    // TextureNoise::Smooth と同じ（スタジオのソリッドテクスチャ）
                    float4 t = saturate((c - a) / max(b - a, 1e-6f));
                    result = t * t * (3 - 2 * t);
                }
                break;
            }
            case MG_OP_COMPONENT:
            {
                uint component = min((uint)imm.x, 3u);
                result = a[component].xxxx;
                break;
            }
            case MG_OP_PROMOTE4:
                result = float4(a.xyz, 1);
                break;
            case MG_OP_ZEROFILL:
            {
                uint count = (uint)imm.x;
                result = float4(a.x, count > 1u ? a.y : 0, count > 2u ? a.z : 0, count > 3u ? a.w : 0);
                break;
            }
            case MG_OP_SWIZZLE:
            {
                uint4 s = (uint4)imm;
                float4 source = a;
                result = float4(s.x < 4u ? source[s.x] : 0, s.y < 4u ? source[s.y] : 0, s.z < 4u ? source[s.z] : 0,
                                s.w < 4u ? source[s.w] : 0);
                break;
            }
            case MG_OP_APPEND:
            {
                uint na = (uint)imm.x;
                float values[8] = {a.x, a.y, a.z, a.w, b.x, b.y, b.z, b.w};
                [unroll] for (uint k = 0; k < 4u; ++k)
                {
                    uint source = k < na ? k : 4u + (k - na);
                    result[k] = k < na + (uint)imm.y ? values[min(source, 7u)] : 0.0f;
                }
                break;
            }
            case MG_OP_DOT3:
                result = dot(a.xyz, b.xyz).xxxx;
                break;
            case MG_OP_CROSS:
                result = float4(cross(a.xyz, b.xyz), 0);
                break;
            case MG_OP_LENGTH:
            {
                uint count = (uint)imm.x;
                float4 v = float4(a.x, count > 1u ? a.y : 0, count > 2u ? a.z : 0, count > 3u ? a.w : 0);
                result = length(v).xxxx;
                break;
            }
            case MG_OP_DISTANCE3:
                result = distance(a.xyz, b.xyz).xxxx;
                break;
            case MG_OP_NORMALIZE3:
                result = float4(normalize(a.xyz), 0);
                break;
            case MG_OP_TEXCOORD:
                result = float4(input.UV * imm.xy, 0, 0);
                break;
            case MG_OP_ROTATE2:
            {
                float s = sin(c.x);
                float co = cos(c.x);
                float2 delta = a.xy - b.xy;
                result = float4(b.xy + float2(delta.x * co - delta.y * s, delta.x * s + delta.y * co), 0, 0);
                break;
            }
            case MG_OP_DESATURATE:
                result = float4(lerp(a.xyz, dot(a.xyz, float3(0.3f, 0.59f, 0.11f)).xxx, b.x), 0);
                break;
            case MG_OP_IF:
            {
                uint4 i1 = (uint4)clamp(program.Load(int3((int)(pc + 2u), MG_VM_ROW, 0)), 0.0f, MG_VM_REGS - 1.0f);
                float4 lessValue = R[i1.x];
                result = a.x > b.x ? c : (a.x == b.x ? d : lessValue);
                break;
            }
            case MG_OP_SPHEREMASK:
            {
                float dd = distance(a.xyz, b.xyz) / max(c.x, 1e-5f);
                result = saturate((1.0f - dd) / max(1.0f - d.x, 1e-5f)).xxxx;
                break;
            }
            case MG_OP_CHEAPCONTRAST:
                result = saturate(lerp(-b.x, 1 + b.x, a.x)).xxxx;
                break;
            case MG_OP_VALUENOISE:
                result = MGVM_ValueNoise(a.xyz * b.x).xxxx;
                break;
            case MG_OP_REMAP:
            {
                uint4 i1 = (uint4)clamp(program.Load(int3((int)(pc + 2u), MG_VM_ROW, 0)), 0.0f, MG_VM_REGS - 1.0f);
                float4 outHigh = R[i1.x];
                result = d.x + (outHigh.x - d.x) * ((a - b.x) / (c.x - b.x));
                break;
            }
            case MG_OP_POSTERIZE:
                result = floor(a * max(b.x, 1)) / max(b.x, 1);
                break;
            case MG_OP_HUESHIFT:
                result = float4(MGVM_HueShift(a.xyz, b.x), 0);
                break;
            case MG_OP_BLEND:
                result = float4(a.xyz + (MGVM_Blend((int)imm.x, a.xyz, b.xyz) - a.xyz) * c.x, 0);
                break;
            case MG_OP_RADIALGRADIENT:
                result = pow(saturate(1 - distance(a.xy, b.xy) / max(c.x, 1e-5f)), max(d.x, 0)).xxxx;
                break;
            case MG_OP_NOISE:
            {
                // プロシージャルスタジオと同じ TextureNoise::Noise（呼び出しはここ 1 か所だけ）
                uint4 i1 = (uint4)clamp(program.Load(int3((int)(pc + 2u), MG_VM_ROW, 0)), 0.0f, MG_VM_REGS - 1.0f);
                uint4 i2 = (uint4)clamp(program.Load(int3((int)(pc + 3u), MG_VM_ROW, 0)), 0.0f, MG_VM_REGS - 1.0f);
                float4 imm1 = program.Load(int3((int)(pc + 2u + argTexels), MG_VM_ROW, 0));
                TextureNoise::NoiseSettings settings;
                settings.Frequency = max(b.x, 0.001f);
                settings.Octaves = clamp((int)c.x, 1, 12);
                settings.Lacunarity = d.x;
                settings.Gain = R[i1.x].x;
                settings.WarpStrength = R[i1.y].x;
                settings.RidgeOffset = R[i1.z].x;
                settings.Direction = R[i1.w].x;
                settings.Anisotropy = clamp(R[i2.x].x, 1.0f, 8.0f);
                settings.KernelFrequency = R[i2.y].x;
                settings.Phase = R[i2.z].x;
                settings.Tileable = imm1.x > 0.5f ? 1u : 0u;
                float3 p = clamp(a.xyz, -10000.0f, 10000.0f);
                result = TextureNoise::Noise(p, (uint)imm.x, (uint)imm.y, MGVM_Seed(imm.z, imm.w), settings).xxxx;
                break;
            }
            case MG_OP_WORLEY:
                result = float4(TextureNoise::Worley(a.xyz, MGVM_Seed(imm.x, imm.y), 0), 0, 0);
                break;
            case MG_OP_NOISECOMBINE:
                result = MGVM_NoiseCombine((int)imm.x, a.x, b.x, c.x, imm.y > 0.5f).xxxx;
                break;
            case MG_OP_VORONOI:
                result = MGVM_Voronoi(a.xy, b.x, imm.x > 0.5f).xxxx;
                break;
            case MG_OP_BLENDANGLE:
            {
                float3 t = a.xyz + float3(0, 0, 1);
                float3 u = b.xyz * float3(-1, -1, 1);
                result = float4(normalize(t * dot(t, u) / max(t.z, 1e-5f) - u), 0);
                break;
            }
            case MG_OP_FLATTEN:
                result = float4(normalize(a.xyz + (float3(0, 0, 1) - a.xyz) * b.x), 0);
                break;
            case MG_OP_ROTATEAXIS:
            {
                float3 axis = normalize(a.xyz);
                float3 closest = c.xyz + axis * dot(axis, d.xyz - c.xyz);
                float3 u = d.xyz - closest;
                float3 v = cross(axis, u);
                float angle = b.x * 6.28318531f;
                result = float4(closest + u * cos(angle) + v * sin(angle) - d.xyz, 0);
                break;
            }
            case MG_OP_BLACKBODY:
                result = float4(MGVM_BlackBody(a.x), 0);
                break;
            case MG_OP_TEXSAMPLE:
            {
                result = MGVM_Sample((uint)imm.x, a.xy, materialSampler);
                if ((uint)imm.y == 1u)
                {
                    result = MGVM_UnpackNormal(result);
                }
                break;
            }
            case MG_OP_CONVERTHEIGHT:
                result = MGVM_ConvertTap((uint)imm.x, a.xy, materialSampler, (int)imm.y, imm.z > 0.5f, b.x, c.x).xxxx;
                break;
            case MG_OP_CONVERTNORMAL:
            {
                uint4 i1 = (uint4)clamp(program.Load(int3((int)(pc + 2u), MG_VM_ROW, 0)), 0.0f, MG_VM_REGS - 1.0f);
                uint heap = (uint)imm.x;
                int channel = (int)imm.y;
                bool invert = imm.z > 0.5f;
                float contrast = b.x;
                float bias = c.x;
                float strength = d.x;
                float radius = max(R[i1.x].x, 1e-5f);
                float2 uv = a.xy;
                float l = MGVM_ConvertTap(heap, uv + float2(-radius, 0), materialSampler, channel, invert, contrast, bias);
                float rr = MGVM_ConvertTap(heap, uv + float2(radius, 0), materialSampler, channel, invert, contrast, bias);
                float up = MGVM_ConvertTap(heap, uv + float2(0, -radius), materialSampler, channel, invert, contrast, bias);
                float down = MGVM_ConvertTap(heap, uv + float2(0, radius), materialSampler, channel, invert, contrast, bias);
                float dx = rr - l;
                float dy = down - up;
                if (imm.w > 0.5f)
                {
                    float lu = MGVM_ConvertTap(heap, uv + float2(-radius, -radius), materialSampler, channel, invert, contrast, bias);
                    float ru = MGVM_ConvertTap(heap, uv + float2(radius, -radius), materialSampler, channel, invert, contrast, bias);
                    float ld = MGVM_ConvertTap(heap, uv + float2(-radius, radius), materialSampler, channel, invert, contrast, bias);
                    float rd = MGVM_ConvertTap(heap, uv + float2(radius, radius), materialSampler, channel, invert, contrast, bias);
                    dx = ((ru - lu) + dx * 2 + (rd - ld)) * 0.25f;
                    dy = ((ld - lu) + dy * 2 + (rd - ru)) * 0.25f;
                }
                result = float4(normalize(float3(-dx * strength, -dy * strength, 1)), 0);
                break;
            }
            case MG_OP_SOLIDVOLUME:
            {
                // ProcStudio::VolumeFromImage と同じ重ね方（imm：テクスチャ, シード下位, シード上位, グレー）
                Texture2D source = LoadMaterialTexture((uint)imm.x);
                uint seed = MGVM_Seed(imm.y, imm.z);
                int octaves = clamp((int)b.x, 1, 6);
                float3 p = a.xyz;
                float4 sum = 0;
                float norm = 0;
                float amplitude = 1;
                [loop] for (int o = 0; o < octaves; ++o)
                {
                    float scale = (float)(1u << (uint)o);
                    uint h = TextureNoise::Hash(seed + (uint)o * 977u);
                    float angle = p.y * 6.28318531f;
                    float swing = round(c.x * 2) * 0.5f / scale;
                    float ox = swing * cos(angle) + TextureNoise::Unit(h);
                    float oz = swing * sin(angle) + TextureNoise::Unit(TextureNoise::Hash(h));
                    float4 first = source.SampleLevel(materialSampler, float2(p.x * scale + ox, p.z * scale + oz), 0);
                    float4 second = source.SampleLevel(materialSampler, float2(p.z * scale - oz + 0.37f, p.y * scale + ox), 0);
                    sum += (first * 0.7f + second * 0.3f) * amplitude;
                    norm += amplitude;
                    amplitude *= 0.5f;
                }
                result = sum / max(norm, 1e-5f);
                if (imm.w > 0.5f)
                {
                    float gray = (result.r + result.g + result.b) / 3;
                    result = float4(gray, gray, gray, result.a);
                }
                break;
            }
            case MG_OP_SOLIDSTOCHASTIC:
            {
                // imm：テクスチャ, シード下位, シード上位, グレー
                uint heap = (uint)imm.x;
                uint seed = MGVM_Seed(imm.y, imm.z);
                float3 w = pow(abs(solidNormal) + 1e-5f, max(b.x, 0.01f));
                w /= dot(w, 1);
                float randomness = saturate(c.x);
                float3 p = a.xyz;
                result = MGVM_HexSample(heap, p.yz, materialSampler, TextureNoise::Hash(seed), randomness) * w.x +
                         MGVM_HexSample(heap, p.xz, materialSampler, TextureNoise::Hash(seed + 0x9E3779B9u), randomness) * w.y +
                         MGVM_HexSample(heap, p.xy, materialSampler, TextureNoise::Hash(seed + 0x3C6EF372u), randomness) * w.z;
                if (imm.w > 0.5f)
                {
                    float gray = (result.r + result.g + result.b) / 3;
                    result = float4(gray, gray, gray, result.a);
                }
                break;
            }
            case MG_OP_TRIPLANAR:
            {
                // imm：テクスチャ, グレー, サンプラー（1 = 法線）
                float3 w = pow(abs(solidNormal) + 1e-5f, max(b.x, 0.01f));
                w /= dot(w, 1);
                float3 p = a.xyz;
                result = MGVM_Sample((uint)imm.x, p.yz, materialSampler) * w.x +
                         MGVM_Sample((uint)imm.x, p.xz, materialSampler) * w.y +
                         MGVM_Sample((uint)imm.x, p.xy, materialSampler) * w.z;
                if (imm.y > 0.5f)
                {
                    float gray = (result.r + result.g + result.b) / 3;
                    result = float4(gray, gray, gray, result.a);
                }
                if ((uint)imm.z == 1u)
                {
                    result = MGVM_UnpackNormal(result);
                }
                break;
            }
            case MG_OP_THREERAMP:
                result = float4(a.x < 0.5f ? lerp(b.xyz, c.xyz, a.x * 2) : lerp(c.xyz, d.xyz, (a.x - 0.5f) * 2), 0);
                break;
            case MG_OP_FRESNEL:
                result = pow(1 - saturate(dot(normal, viewDirection)), a.x).xxxx;
                break;
            case MG_OP_COLORADJUST:
            {
                float3 color = (a.xyz * b.x - 0.5f) * c.x + 0.5f;
                float luminance = dot(color, float3(0.2126f, 0.7152f, 0.0722f));
                result = float4(lerp(luminance.xxx, color, d.x), 0);
                break;
            }
            case MG_OP_SLOPEMASK:
            {
                float softness = max(c.x, 1e-4f);
                float t = saturate((dot(normal, normalize(a.xyz)) - (b.x - softness)) / (2 * softness));
                result = (t * t * (3 - 2 * t)).xxxx;
                break;
            }
            case MG_OP_TRANSFORM:
            {
                uint mode = (uint)imm.x;
                float3x3 axes = (float3x3)input.ObjectToWorld;
                if (mode == 0u) result = float4(mul(a.xyz, axes), 0);
                else if (mode == 1u)
                    result = float4(MaterialGraphObjectPosition(a.xyz + input.ObjectToWorld[3].xyz, input.ObjectToWorld), 0);
                else if (mode == 2u) result = float4(mul(float4(a.xyz, 1), input.ObjectToWorld).xyz, 0);
                else if (mode == 3u) result = float4(MaterialGraphObjectPosition(a.xyz, input.ObjectToWorld), 0);
                else if (mode == 4u) result = float4(mul(a.xyz, (float3x3)View), 0);
                else result = float4(mul((float3x3)View, a.xyz), 0);
                break;
            }
            case MG_OP_DITHER:
            {
                uint2 p = (uint2)max(input.PixelPos, 0.0f) & 3u;
                const uint table[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
                float threshold = (table[(p.y << 2u) | p.x] + 0.5f) / 16.0f;
                result = (a.x > threshold ? 1.0f : 0.0f).xxxx;
                break;
            }
            case MG_OP_EXPAND4:
            {
                uint count = (uint)imm.x;
                result = count <= 1u ? float4(a.xxx, 1) : float4(a.x, a.y, count > 2u ? a.z : 0.0f, count > 3u ? a.w : 1.0f);
                break;
            }
            case MG_OP_HEIGHTLERPALPHA:
            {
                float x = saturate(a.x + b.x * 2 - 1);
                result = saturate(lerp(-c.x, 1 + c.x, x)).xxxx;
                break;
            }
            default:
                result = a;
                break;
        }
        R[dst] = result;
        pc += instructionTexels;
    }

    output.Active = true;
    output.BaseColor = output.Out0.x >= 0 ? R[min((uint)output.Out0.x, MG_VM_REGS - 1u)] : float4(1, 1, 1, 1);
    output.Metallic = output.Out0.y >= 0 ? R[min((uint)output.Out0.y, MG_VM_REGS - 1u)].x : 0.0f;
    output.Roughness = output.Out0.z >= 0 ? R[min((uint)output.Out0.z, MG_VM_REGS - 1u)].x : 0.5f;
    output.Normal = output.Out0.w >= 0 ? R[min((uint)output.Out0.w, MG_VM_REGS - 1u)].xyz : float3(0, 0, 1);
    output.Emissive = output.Out1.x >= 0 ? R[min((uint)output.Out1.x, MG_VM_REGS - 1u)].xyz : float3(0, 0, 0);
    output.Opacity = output.Out1.y >= 0 ? R[min((uint)output.Out1.y, MG_VM_REGS - 1u)].x : 1.0f;
    output.OpacityMask = output.Out1.z >= 0 ? R[min((uint)output.Out1.z, MG_VM_REGS - 1u)].x : 1.0f;
    output.AO = output.Out1.w >= 0 ? R[min((uint)output.Out1.w, MG_VM_REGS - 1u)].x : 1.0f;
    output.Height = output.Out2.x >= 0 ? R[min((uint)output.Out2.x, MG_VM_REGS - 1u)].x : 0.0f;
    output.NormalIntensity = output.Out2.y >= 0 ? R[min((uint)output.Out2.y, MG_VM_REGS - 1u)].x : 1.0f;
    output.Transmission = output.Out2.z >= 0 ? R[min((uint)output.Out2.z, MG_VM_REGS - 1u)].x : 0.0f;
    return output;
}


#ifndef MG_VM_STANDALONE
// ApplyAssetMaterial の直後に呼ぶ：色 / 金属度 / 粗さ / 発光 / AO / 不透明度を、グラフの値で置き換える
void ApplyMaterialGraphSurface(MaterialGraphVmOutput graph, AssetMaterialParameters material, inout float4 color,
                               inout float metallic, inout float roughness, inout float4 emissionAO)
{
    if (!graph.Active)
    {
        return;
    }
    if (graph.Out0.x >= 0)
    {
        color.rgb = graph.BaseColor.rgb;
    }
    if (graph.Out0.y >= 0)
    {
        metallic = saturate(graph.Metallic);
    }
    if (graph.Out0.z >= 0)
    {
        roughness = saturate(graph.Roughness);
    }
    if (graph.Out1.x >= 0)
    {
        emissionAO.rgb = max(graph.Emissive, 0.0f);
    }
    if (graph.Out1.w >= 0)
    {
        emissionAO.a = lerp(1.0f, saturate(graph.AO), material.Surface.y);
    }
    const uint surface = material.Textures.w & 3u;
    if (surface == 1u)
    {
        float mask = graph.Out1.z >= 0 ? graph.OpacityMask : (graph.Out0.x >= 0 ? graph.BaseColor.a : 1.0f);
#ifndef SHADER_VISIBILITY_RESOLVE
        clip(mask - material.Surface.w);
#endif
        color.a = 1.0f;
    }
    else if (surface == 2u)
    {
        if (graph.Out1.y >= 0)
        {
            color.a = saturate(graph.Opacity);
        }
        else if (graph.Out0.x >= 0)
        {
            color.a = saturate(graph.BaseColor.a);
        }
    }
}

// 法線を決めた後に呼ぶ：グラフの Normal（接空間）を、ジオメトリの法線に適用する
float3 ApplyMaterialGraphNormal(MaterialGraphVmOutput graph, AssetMaterialParameters material, float3 normal,
                                float3 geometryNormal, float3 worldPos, float2 uv)
{
    if (!graph.Active || graph.Out0.w < 0)
    {
        return normal;
    }
    float3 tangentNormal = ScaleAssetNormal(SafeNormalizeCommon(graph.Normal, float3(0, 0, 1)), material);
    return SafeNormalizeCommon(ApplyNormalMapCommon(geometryNormal, worldPos, uv, tangentNormal), normal);
}

#endif // MG_VM_STANDALONE

#endif


