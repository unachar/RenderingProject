# GPU Driven Rendering — Phase 3

## 実装

既存GPU SceneとCullingを維持し、Opaque Static Mesh向けに以下の経路を追加した。

`GPU Scene Update → GPU Scene Culling → Compact Visible Instances → Generate Indirect Commands → Opaque ExecuteIndirect`

- Commandは24 bytes。Instance IDのroot constantと`DRAW_INDEXED`を持つCommand Signatureに対応する。
- Cullingの既存visibility flagsをGPU上でcompactし、countをGPUでreset／atomic加算する。Visible ID、count、commandの通常readbackはない。
- Mesh登録時に共有VB／IBへGeometryを追加する。Mesh IDに対応する16-byte geometry tableからGPUがdraw argumentsを取得する。頂点のmaterial part変更も共有Geometryへ反映する。
- GPU InstanceのWorld、Material ID、既存raw Materialデータ、既存descriptor heapのtexture arrayをShaderから参照する。SM5.1を維持し、texture indexには`NonUniformResourceIndex`を使用する。
- Instance／Material SRV、Visible UAV→SRV、Command UAV→INDIRECT、Count、VB／IBの状態と依存関係をRenderGraphへ宣言する。
- 3 frame slotsのCommand／Count／Visible buffersを保持し、GPU Sceneのcapacityに合わせて必要時だけ拡張する。既存fence retirementを再利用する。
- 対象EntityをCPU opaqueの列挙から除くため、変更通知でCPU fallback一覧を更新する。Shadow／Velocityの既存処理は維持する。

## 対象と操作

ImGuiのGPU Scene付近にある **GPU Driven Rendering / Enabled** で切り替える。
使用条件は **Deferred mode、Compute GBuffer無効、Manual PBR、Opaque、標準Shader、Indexed Static Mesh**。

Forward、Visibility／Compute GBuffer、Auto material、Transparent、Skinned、Sky、Environment、LOD、頂点変形、custom Shaderなどは既存CPU経路へ戻る。GPU対象判定に影響するShader／Name／ProceduralMotionのComponent API編集もGPU Sceneへ通知する。

既存Engineの既定Compute GBuffer modeではCPU fallbackとなる。後続Phaseで別PSO／Passへ拡張するための入口を残す。

UIにはcapacity、buffer memory、ExecuteIndirect API回数、CPU submission比較、Culling／Command Generation／ExecuteIndirectのGPU時間を表示する。
Command数と先頭16件のInstance ID／Mesh ID／draw argumentsは **Debug command readback** を有効にすると確認できる。このdebug機能はGPU待機を伴う。無効時はcountをGPUに保持する。

## 検証

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/validate-gpudriven.ps1 -Configuration Debug
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/validate-gpudriven.ps1 -Configuration Release
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/validate-gpudriven.ps1 -Configuration Debug -GpuValidation
```

CPUテストはCommand layout、capacity拡張、geometry offsets、整数overflowを検証する。
84-frame実行試験は100／1,000／10,000 Instance、camera急移動、削除／再生成、空Scene、previous transform、texture／material、CPU fallbackへの移行と復帰を検証する。
通常経路のreadbackを追加せず、試験専用のreadbackでcompact count、live IDの一意性、draw arguments、API呼び出し1回を確認する。

5つの独立したmaterial sectionsを持つ10,000 Entityで **50,000 Static Mesh instances／commands** を試験する。既存ECSの16,384 Entity上限は変更しない。50,000 Entityの試験とは区別する。
CPU／GPUの5つのGBufferを画素単位で比較する。Opaque sortingは行わないため、比較fixtureは同じ深度の異なるmaterial面が重ならないよう配置する。

GPU Based Validationはこの環境で初回フレームが240秒以内に完了せず、CPU開始／GPU開始の双方でタイムアウトした。完了条件のGPU Based Validation合格は未確認。ログは`build/gpudriven/runtime-Debug-gbv.log`へ保存する。通常Debug Layer試験とは区別する。

### 2026-10-12 実測結果

| 検証 | 結果 |
|---|---|
| C++／HLSL build、layout／capacity／overflowテスト | Debug／Release 合格 |
| Phase 3 runtime | Debug／Release、各84 frames、失敗0 |
| GBuffer比較 | 各構成6回、各900,000画素、差分0 |
| 50,000 commands | capacity 65,536、一意なlive Instance ID、draw arguments一致、ExecuteIndirect 1回 |
| Debug Layer | Debug試験、D3D12 errors 0。Releaseではdebug queue無効 |
| 既存GPU Scene／描画機能の回帰 | Debug features、84 frames合格、D3D12 errors 0 |
| GPU Based Validation | 初回フレームでtimeout、未合格 |

10,000 Instanceの`GBuffer / Opaque` CPU処理時間を、安定した各4フレームの平均で比較した。

| 構成 | CPU Driven | GPU Driven |
|---|---:|---:|
| Debug | 507.8745 ms | 15.2344 ms |
| Release | 58.0857 ms | 0.2087 ms |

50,000 Instance時のRelease GPU profiler値は、Culling 0.0280 ms、Command Generation 0.0215 ms、ExecuteIndirect 1.4787 ms。
この小さいStatic Mesh fixtureでの同一環境比較であり、実際のSceneやMeshの性能値ではない。

証拠ログは`build/gpudriven/runtime-Debug.log`、`runtime-Release.log`、`runtime-Debug-gbv.log`、`build/gpuscene/Debug-stress-features/init_log.txt`。
初回Release増分buildは既存中間ファイルの`LNK1103`で失敗したが、`/Z7`を指定したクリーン再ビルドと再実行で合格した。再現時に中間ファイルの問題が出た場合はスクリプトの`-Rebuild`を使用する。

実装・通常Debug Layer検証は完了した。Phase 3仕様の全完了条件を満たしたと判断するには、GPU Based Validationを完了できる環境での追加確認が必要。
