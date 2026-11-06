# OptiX denoiser

描画設定の「ノイズ除去を使用」を有効にし、「GI・反射デノイザー」で NVIDIA NRD / NVIDIA OptiX AI (HDR) を選択する。設定は `raytracing_denoiser_type` に保存される。既定はNRD。影とAOは引き続きNRDを使用する。

OptiXはGIの間接照明と反射のHDR放射輝度に適用する。法線とニュートラルなアルベドをガイドに渡す（入力は表面色を掛ける前の照明）。発光色・強度・テクスチャの変更も通常のGI入力を通じて反映される。D3D12と同じGPUをLUIDで選び、共有GPUバッファとフェンスでCUDAと同期する。通常のフレーム処理にCPUへの画像読み戻しはない。

この実装はHDR空間モデルで、OptiXの時間方向モデルやモーションベクトルによる履歴再投影は使用していない。NRDのチェッカーボード入力に代えて全画素をトレースするため、速度やちらつきの面でNRDが有利な場合がある。低サンプル数、遠距離の細い形状、深度や法線の不整合が原因のノイズまで解消する保証はない。

SDK未導入、対応GPUなし、初期化失敗の場合はNRDへフォールバックし、UIに理由を表示する。実行中のOptiX失敗はそのフレームのGIを空間フィルター、反射を未除去画像とし、次フレームからNRDへ戻す。D3D12コマンドリストの送信・再開自体に失敗した場合は、無効なリストで描画を続けずログを残して終了する。

## ビルド

`scripts/install-optix-sdk.ps1` は公式OptiX 9.0.0とCUDA cudart 12.9.79のアーカイブをSHA256検証し、ヘッダーとライセンスをプロジェクト内へ展開する。CUDA Toolkitのシステムインストールは不要。x64ビルドはヘッダーがあれば自動的に有効化する。無効化はMSBuildの `/p:EnableOptix=false`。実行時には対応するNVIDIAドライバーが必要。

- [NVIDIA OptiX headers](https://github.com/NVIDIA/optix-dev)
- [OptiX denoiser API](https://raytracing-docs.nvidia.com/optix9/api/group__optix__host__api__denoiser.html)
- [CUDA graphics interop](https://docs.nvidia.com/cuda/cuda-programming-guide/04-special-topics/graphics-interop.html)

## 検証

Debug x64の最終ビルド、およびNRD・OptiX双方の218フレーム検証に成功し、D3D12エラーはいずれも0件。OptiX/CUDA共有処理は実GPUで実行した。SDK導入スクリプトも検証済みアーカイブを再利用して実行確認した。

`scripts/validate-gi.ps1 -Optix` で実際のGPUを使用する。218フレームで発光色・強度・テクスチャ変更、遮蔽、発光体移動、GI/反射併用、遠距離カメラ、NRD→OptiXの実行中切り替えを検証する。`-SkipBuild` は既存バイナリーを再利用する。結果と画像は `build/gi-tests` に保存される。ユーザー提供の部屋そのものを再現した検証ではない。

2026-10-16、RTX 3050 Laptop GPU、内部解像度600×300の小規模検証シーンでは、GIパスの4点計測がNRDで0.546〜0.596ms、OptiXで5.226〜6.419msだった。共有TLAS構築・ガイド生成は含まない。OptiX側は全画素トレースとGPU間API同期も含むため、デノイザー単体の比較ではない。フレーム12〜23の収束中の相対時間RMSもNRD約0.0010に対しOptiX約0.0315で、このシーンではNRDの方が時間方向に安定した。定常状態や実際の部屋の画質を表す数値ではない。
