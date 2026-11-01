# Dracoの実装と比較

> adapterの最新CRT構成と再計測結果は[引継ぎ記録](library-work-handoff-2026-11-01.md)を参照。以下の静的CRT構成は旧方式の記録。

## 採用

Dracoを`KHR_draco_mesh_compression`付きglTF/GLBの読込へ接続した。同梱Assimpでは圧縮Boxを読めないため、新しいadapterが入力機能を補う。glTF/GLB/VRMはSDKが利用可能ならfastgltf adapterを先に使い、未対応機能は既存Assimpへ戻す。`DX12_USE_FASTGLTF=0`は既存のみ、`1`は新経路を優先する。FBX/PMX/VMD等の形式は従来の読込を維持する。

glTFの選択方針を変更した理由は、既存読込が疎な頂点を誤読する例に加え、逆バインド行列を省略した有効なskin入力でアクセス違反になる例を確認したため。新経路は単位行列として扱う。これは[glTFのskin定義](https://raw.githubusercontent.com/KhronosGroup/glTF/main/specification/2.0/schema/skin.schema.json)の省略時の意味に対応する。既存失敗後の再試行だけではアクセス違反を回避できない。未知のmaterial等の拡張は従来どおりAssimpへfallbackする。

既存資産・geometry cacheのDraco化や量子化は自動で行わない。単体`.drc`入出力・製品のcook/export機能も未実装。

## 圧縮速度・容量・誤差

同じ16,641頂点・32,768三角形の曲面gridで、position/normal/UVと32bit indexを比較した。元データは925,728 bytes。最適化Release、実行順を回転した5passの中央値:

|codec|payload bytes|圧縮|展開|
|---|---:|---:|---:|
|zstd level 3|395,662|3.8280ms|1.1422ms|
|meshoptimizer vertex/index codec|243,654|2.4641ms|0.2953ms|
|Draco・量子化なし|540,906|9.5420ms|4.1713ms|
|Draco・position14/normal10/UV12bit|37,096|12.0763ms|5.8131ms|

この入力では、精度を落とさないgeometry codecとしてmeshoptimizerがDracoより小さく高速。Dracoの量子化版は約96%小さくなるが、展開は遅くなる。したがってDracoは圧縮入力への対応として採用し、既存cacheの一律置換には使わない。meshoptimizerのcodecを実際のcache形式へ採用するには、既存の頂点ID・属性・所有権契約を含めた統合比較が必要。

zstdは元バイト列と完全一致。meshoptimizerは全頂点バイトと、面順・向きを保持した三角形を照合した。index codecは三角形の先頭cornerを巡回させるため、indexバイト列の一致は要求しない。Dracoは頂点・面の並びを変えるため、grid対応点の全属性と、正規化してソートした全三角形の向きを照合した。量子化なしはfloat値の誤差ゼロ。量子化版の最大成分誤差はposition 0.0000305325、normal 0.00209758、UV 0.000122130。位置の数値は元の座標単位で、画面上の誤差や角度ではない。

圧縮時間にはDraco用meshの構築も含め、展開時間には出力確保を含む。整合性照合は計測区間外。ファイルI/O、glTF解析、cache header、GPU転送、FPS、process memoryの比較ではない。meshoptimizerは2streamの合計payloadであり、stream長等の保存headerは含まない。測定はこの合成曲面の結果で、実資産全般の優劣を証明しない。

実ファイルのBox.glbは非圧縮1,664 bytesに対してDraco版2,388 bytesで、小さいモデルでは圧縮版のほうが大きかった。両者の全三角形の向き・座標・法線を比較し、座標変換の前後でも一致を確認した。

同じBoxの製品`ImportModelScene`と解放を各100回×交互順5passで計測した中央値は、非圧縮・既存0.613394ms、非圧縮・新経路0.539763ms、Draco・既定選択0.553304ms、Draco・新経路明示0.545612ms。ファイルI/Oと後処理を含むwarm比較で、codec単体の表とは直接比較しない。小さいこのBoxではDracoを保存形式として選ぶメリットがない。以前の既存失敗後fallback方式の別実行値は`gltf-report.json`に残すが、負荷条件が異なるため現行方式との速度比には使わない。最終結果は`bin/validation/draco/gltf-report-final.json`。

## 実装・検証範囲

`gltfdraco.cpp`はextensionのunique attribute IDを使用し、Draco point→attribute mappingを展開する。元のglTF component typeとnormalized属性を保った独立accessor/bufferを生成し、三角形indexも展開して既存変換へ渡す。元accessorが共有されていても上書きしない。無効ID・重複ID・型不一致・count不一致・切断streamは失敗として戻す。resident/展開bufferの512MiB budgetはSDK内部のピークメモリ上限を保証するものではない。

実Boxに加え、unique IDをattribute配列indexと異なる値にしたSDK生成fixtureで、UV、normalized uint8 color、uint8 joint、normalized uint8 weightを検証する。重みは一度だけ正規化され、skinの対象・重み・省略時の単位bind行列を確認した。比較用Assimpは省略bindでクラッシュしたため、同じ意味の単位行列を明示したdense参照を使う。新しい既定経路では省略bindを含むplain glTFも正常に読めることを試験した。

`scripts/build-draco-dx12.ps1`は専用Release static SDKを構築する。ローカルヘッダーの実versionは1.5.9だが、manifestは1.5.6/mainで一致していない。`DRACO_GLTF_BITSTREAM=ON`、tools/tests/transcoderはOFFで使用するmesh codecを構築した。固定ローカルソースの結果であり、最新版の主張には使わない。

SDKは`FastGltfAdapter.dll`内へリンクし、製品DebugのSTLと混在させない。`build-fastgltf-adapter.ps1 -DisableDraco`は別名の`FastGltfAdapterNoDraco.dll`を作り、製品用DLLを上書きしない。Draco include/libなしで、plain glTFは読め、Draco inputは明示的に拒否することを確認した。DracoのLICENSEは製品へ配置する。

主な証拠: `bin/draco-sdk-build.log`、`bin/draco-adapter-final-build.log`、`bin/draco-gltf-{release,debug}-final.log`、`bin/draco-disabled-final-tests.log`、`bin/draco-all-sdk-disabled-regression.log`、`bin/draco-default-selection-{red,green}.log`、`bin/draco-default-optional-bind-green.log`、`bin/validation/draco/mesh-report.json`。

最終の製品Debugビルドも成功（`bin/draco-product-default-final-build.log`、ビルドプロセスのみ`/Z7`）。既定設定の隔離シーンで、Draco Box、スキン付きDraco、逆bind行列を省略したplain glTFを読み、3フレーム描画と正常終了を確認した（`bin/validation/startup/draco-default-final-desktop-Debug-1.log`、runner exit0）。ユーザーのシーン設定は復元した。このsmokeは画素比較や量子化品質の視覚判定ではない。

skin/morphの非圧縮属性との混在、材質拡張との併用、一般のDraco point cloud、製品cook/export、より広い実資産と実画面での量子化品質は引き続き確認・実装が必要。
