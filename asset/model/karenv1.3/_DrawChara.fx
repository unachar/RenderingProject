////////////////////////////////////////////////////////////////////////////////////////////////
//
//  _Draw_Chara.fx
//  作成: 角砂糖
//
////////////////////////////////////////////////////////////////////////////////////////////////

#include "_Config.fxsub"

////////////////////////////////////////////////////////////////////////////////////////////////
// パラメータ宣言

float4x4 WorldViewProjMatrix : WORLDVIEWPROJECTION;
float4   MaterialDiffuse   : DIFFUSE  < string Object = "Geometry"; >;
float4   EdgeColor         : EDGECOLOR;

bool use_texture;

texture ObjectTexture: MATERIALTEXTURE;
sampler ObjTexSampler = sampler_state {
    texture = <ObjectTexture>;
    MINFILTER = LINEAR;
    MAGFILTER = LINEAR;
    MIPFILTER = LINEAR;
    ADDRESSU  = WRAP;
    ADDRESSV  = WRAP;
};

// 髪の描画記録先
shared texture2D HairTex : RENDERCOLORTARGET;

// 深度バッファ
texture2D HairDepthBuffer : RENDERDEPTHSTENCILTARGET <
    float2 ViewPortRatio = {1.0,1.0};
    string Format = "D24S8";
>;

////////////////////////////////////////////////////////////////////////////////////////////////
// シェーダー
float4 Basic_VS(float4 Pos : POSITION,
 float2 Tex : TEXCOORD0,
 out float2 oTex : TEXCOORD0) : POSITION
{
    oTex = Tex;
    return mul(Pos, WorldViewProjMatrix);
}

float4 Eye_PS(float2 Tex : TEXCOORD0, uniform bool subEye, uniform bool subHair) : COLOR
{
    float4 Color = use_texture ? tex2D(ObjTexSampler, Tex) : 1.0;
    Color.a *= MaterialDiffuse.a * subEye;
    if(subHair) clip(-1);
    return Color;
}

float4 Hair_PS(float2 Tex : TEXCOORD0, uniform bool subHair) : COLOR
{
    float Alpha = use_texture ? tex2D(ObjTexSampler, Tex).a : 1.0;
    Alpha *= MaterialDiffuse.a;
    return float4(subHair, 0, 0, Alpha);
}

float4 Edge_VS(float4 Pos : POSITION) : POSITION
{
    return mul(Pos, WorldViewProjMatrix);
}

float4 EdgeEye_PS(uniform bool subEye, uniform bool subHair) : COLOR
{
    if(subHair) clip(-1);
    EdgeColor.a *= subEye;
    return EdgeColor;
}

float4 EdgeHair_PS(uniform bool subHair) : COLOR
{
    EdgeColor.a *= subHair;
    return float4(subHair, 0, 0, EdgeColor.a);
}

////////////////////////////////////////////////////////////////////////////////////////////////
// テクニック

#define MAIN_TEC(name, mmdpass, eye, hair, subsetID) \
technique name < \
    string MMDPass = mmdpass; \
    string Subset = subsetID; \
    string Script =  \
        "RenderColorTarget0=;" \
        "RenderDepthStencilTarget=;" \
        "Pass=DrawEye;" \
        "RenderColorTarget0=HairTex;" \
        "RenderDepthStencilTarget=HairDepthBuffer;" \
        "Pass=DrawHair;"; \
    > { \
    pass DrawEye { \
        AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE; \
        VertexShader = compile vs_3_0 Basic_VS(); \
        PixelShader  = compile ps_3_0 Eye_PS(eye, hair); \
    } \
    pass DrawHair { \
        AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE; \
        VertexShader = compile vs_3_0 Basic_VS(); \
        PixelShader  = compile ps_3_0 Hair_PS(hair); \
    } \
}

#define EDGE_TEC(name, mmdpass, eye, hair, subsetID) \
technique name < \
    string MMDPass = mmdpass; \
    string Subset = subsetID; \
    string Script =  \
        "RenderColorTarget0=;" \
        "RenderDepthStencilTarget=;" \
        "Pass=DrawEye;" \
        "RenderColorTarget0=HairTex;" \
        "RenderDepthStencilTarget=HairDepthBuffer;" \
        "Pass=DrawHair;"; \
    > { \
    pass DrawEye { \
        AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE; \
        VertexShader = compile vs_3_0 Edge_VS(); \
        PixelShader  = compile ps_3_0 EdgeEye_PS(eye, hair); \
    } \
    pass DrawHair { \
        AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE; \
        VertexShader = compile vs_3_0 Edge_VS(); \
        PixelShader  = compile ps_3_0 EdgeHair_PS(hair); \
    } \
}

MAIN_TEC(EyeTec,      "object",    true,  false, EYE_SUBSET)
MAIN_TEC(EyeTecBS,    "object_ss", true,  false, EYE_SUBSET)
EDGE_TEC(EyeEdgeTec,  "edge",      true,  false, EYE_SUBSET)
MAIN_TEC(HairTec,     "object",    false, true,  HAIR_SUBSET)
MAIN_TEC(HairTecBS,   "object_ss", false, true,  HAIR_SUBSET)
EDGE_TEC(HairEdgeTec, "edge",      false, true,  HAIR_SUBSET)
MAIN_TEC(MainTec,     "object",    false, false, "0-")
MAIN_TEC(MainTecBS,   "object_ss", false, false, "0-")
EDGE_TEC(MainEdgeTec, "edge",      false, false, "0-")

technique EdgeTec < string MMDPass = "edge"; > { }
technique ShadowTec < string MMDPass = "shadow"; > { }
technique ZplotTec < string MMDPass = "zplot"; > { }
