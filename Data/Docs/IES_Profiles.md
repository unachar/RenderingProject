# IES 配光

ライトごとに IES ファイルを指定し、環境設定から一括で有効・無効にする。
IES は最大値を 1 に正規化した配光マスクとして扱い、明るさは既存のライト強度で調整する。

## 使い方

1. 環境設定の「IES配光」で「IESプロファイルを有効化」をオンにする。
2. Point / Spot / Rect ライトのインスペクターで「IESプロファイル」を指定する。
3. 非対称の配光は「IES回転」で軸回りに回転させる。配光の正面は「ライト方向」に一致する。

サンプルは `asset/Lighting/IES/NarrowBeam.ies` と `Asymmetric.ies`。
プロジェクトで作成した合成配光であり、実在する照明器具の測定値ではない。
スポットのコーン、距離減衰、矩形の片面発光、影、マテリアル応答は引き続き適用する。

## 対応と保存

- LM-63 の Type C、`TILT=NONE` を読み込む。
- 水平角は回転対称、0–90、0–180、0–360 度に対応する。不等間隔の角度を線形補間する。
- Type A / B、傾斜補正付きファイル、不正な数値・角度・切れたデータは理由をログへ出し、従来の配光へ戻す。
- プロファイル未指定時とグローバル無効時は従来の配光を使用する。
- 環境設定は `ies_profiles_enabled`、シーンのライトは `iesProfile` と `iesRotation` を保存する。
  これらの項目がない既存シーンも読み込める。
- 平行光源と Decal / Volume 専用ライトには適用しない。

## 実装と検証

CPU で検証した配光を 256×128 の線形 R32_FLOAT テクスチャに変換し、既存のテクスチャヒープへ格納する。
同じファイルのデータは共有し、通常のディファード・Forward、MegaLights、フロクセルフォグで共通の角度計算を使う。
方位角の継ぎ目は折り返し、極方向はクランプする。
読み込み済みファイルは実行中にキャッシュする。ファイル内容を編集した後の再読み込みにはアプリの再起動が必要。

- `scripts/test-ies-profiles.ps1`: 解析、対称性、不等間隔補間、正規化、不正ファイル、ベイク、サンプル。
- `scripts/test-editor-config.ps1`: スイッチの初期値、変更通知、保存値の復元、不正な設定値。
- `scripts/validate-materials.ps1 -Transparency`: 実際の GPU 受光値で有効・無効、未指定、読込失敗、
  非対称配光の回転、通常描画と MegaLights の一致を確認する。

ファイル形式の参照:
[Autodesk: IES Standard File Format](https://help.autodesk.com/view/ACDLT/2026/ENU/?caas=caas%2Fdocumentation%2FACD%2F2014%2FENU%2Ffiles%2FGUID-45CAAF1C-7C9D-49A7-B18D-00CA5E2ED380-htm.html)
