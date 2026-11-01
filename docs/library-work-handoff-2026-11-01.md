# Externalライブラリ統合作業の中断記録

2026-11-01、ユーザーの指示により追加実装・検証を中断。変更は作業ツリーに保持し、コミット・pushは行っていない。全体の依頼は未完了。

## 採用済みの方針と実装

- 圧縮は速度とサイズを比較。既存キャッシュにはzstdを採用し、既存の非圧縮形式も読み込む。新規保存を非圧縮にする設定も維持。
- Tracyは任意のCPU計測として統合し、標準では無効。
- FBXアニメーションのACL経路を実装。実資産ではサイズが減る一方でサンプリングが遅く、標準は既存経路を維持。
- KTX2/Basis入力を接続。既存DDSとzstdのキャッシュ方式は比較結果から維持。
- glTF/GLB/VRMの主モデル読込にfastgltfとDracoを統合。SDK利用時は新経路を優先し、未対応機能はAssimpへ戻す。`DX12_USE_FASTGLTF=0`で既存のみを選択できる。

## fastgltfの最新状態

従来の比較文書にある静的CRTとシーンのdeep copyは旧方式。現在の標準は専用adapter DLLとSDKをRelease `/MD`で構築し、同じ共有CRTを使うAssimp DLLへシーンの所有権を渡す方式。アプリDebugのCRT設定は変更していない。DLL境界にはSTLオブジェクトを渡さない。ビルドスクリプトの`-Runtime Static`によるdeep copy方式も残している。

属性を直接格納し、既存ローダー探索の重複も削減。時刻配列の再利用案は計測で悪化したため採用していない。

同一プロセス内でファイル読込・後処理・解放まで測った最新中央値（ms）:

| 入力 | 既存Assimp | 変更前adapter | 現在の共有CRT adapter |
|---|---:|---:|---:|
| Box | 0.672350 | 0.580013 | 0.534120 |
| CesiumMan | 3.451180 | 4.086420 | 3.635430 |
| material fixture | 0.612483 | 0.518170 | 0.491500 |
| animated fixture | 0.631317 | 0.538717 | 0.493907 |

CesiumManは変更前adapter比で約11%改善したが、既存Assimpより約5.3%遅い。新経路の標準化にはDraco対応と既存Assimpが扱えない有効な入力への対応も含めて判断している。小さい入力はばらつきが大きい。GPU負荷・FPS・コールド起動の優位性を示す数値ではない。

別プロセスでCesiumManを1回読み込む3試行のprivate commit中央値では、ピークが旧adapterの4,980,736 bytesから4,132,864 bytesへ低下。既存Assimpは3,915,776 bytes。DLL、パーサー、アロケーターを含む値であり、シーンの生存メモリやVRAMの測定ではない。

## 確認済みの範囲

- fastgltfとDracoのDebug/Release consumer、SDK無効、Draco無効の試験に合格。
- 属性・スキン・アニメーションの比較、不正入力、途中失敗時の後始末を検証。
- 製品Debugビルドに成功。ただし環境の通常`/ZI`はC1902になるため、プロセス内だけ`_CL_=/Z7`を指定。通常のツールチェーン問題は未解決。
- 配置したadapter/Assimp DLLのハッシュ一致を確認。
- Dracoモデル、スキン付きモデル、省略inverse bind行列のglTFを含む起動試験で3フレームと終了処理を確認。画質や実ゲーム負荷の比較は未実施。

主な証拠は`bin/fastgltf-shared-product-build.log`、`bin/fastgltf-shared-parity.log`、`bin/fastgltf-shared-draco.log`、`bin/fastgltf-shared-release-tests.log`、`bin/fastgltf-shared-nodraco-tests.log`、`bin/fastgltf-shared-disabled-tests.log`、`bin/validation/fastgltf/optimization-shared-result.json`、`bin/validation/fastgltf/memory-result.json`、`bin/validation/startup/fastgltf-shared-desktop-Debug-1.log`。これらはローカル生成物。

## 再開時の残作業

- モデルのサムネイルは`source/rendering/thumbnailrenderer.cpp`で既存Assimpを直接使用しており、fastgltf/Draco未接続。独自IOによる外部依存ファイルの追跡とキャッシュ無効化を保った統合が必要。
- fastgltf/Draco比較文書、全体一覧、計画の古いCRT・deep copy・速度の記述を最新結果へ整合させる。本記録が中断時点の最新状態。
- STEP/CUBIC、morph animation等の未対応入力、より広い実資産、描画品質の比較。Draco単独形式や製品向けcook/exportは未接続。
- OCIO画質、Tracy接続時の計測とGPU対応、ShaderMakeの役割整理、CACAO/FSR2/XeSS、RTXDI/STBN/RTXTF、D3D12MA/DirectStorage、OMM/RTXCR/Streamline等、全体依頼の未完了項目。

次のユーザー指示を受けるまで追加作業は行わない。
