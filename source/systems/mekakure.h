#pragma once
#include <d3d12.h>

// メカクレ（前髪越しに目・眉を透かす）。ScenePasses::EndHdrTransparentPass から、半透明（OIT の合成を含む）を
// 描き終えた直後に呼ぶ。実装は modelsystem.cpp（モデルの描画データを共有するため）。
//  1. 髪マスク：メカクレ対象キャラの髪を R8 マスクへ描く（髪テクスチャのアルファ / 専用マスク）
//  2. 目の重ね描き：目・眉・まつ毛を深度だけ手前へずらして、マスク × 不透明度でシーンへ合成
namespace Mekakure
{
    void Render(ID3D12GraphicsCommandList* commandList, D3D12_CPU_DESCRIPTOR_HANDLE sceneRtv,
                D3D12_CPU_DESCRIPTOR_HANDLE depthDsv, const D3D12_VIEWPORT& viewport, const D3D12_RECT& scissor);
    void Release();
}
