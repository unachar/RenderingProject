# 既存BCテクスチャの容量・復元コスト比較

2026-10-31。KTX2/Basisの[ソース調査](rendering-technology-ktx-basis-source-audit-2026-10-31.md)に続き、現行DDSのBCデータを変えずにzstdで保存できるかを比較した。これは読み取り専用の実験で、テクスチャの製品キャッシュやロード処理は変更していない。

## 入力と方法

`Data/Cache/Textures`にある65件、122,108,212bytes、441mipを全件使用した。BC本体は122,098,992bytes。DirectXTexで読み込んだ形式はBC7_UNORM44件、BC7_UNORM_SRGB1件、BC5_UNORM7件、BC4_UNORM13件。形式から用途を断定しておらず、BC5の全件を法線と呼ばない。単一2D・非array・非cubeであることも確認した。

ファイルを変更せず、メモリ上で次の2通りをlevel1/3/9で比較した。

1. DDS全体を単一zstdフレームへ圧縮し、元のDDSバイト列へ戻す。
2. 各mipのBC本体を独立したzstdフレームへ圧縮し、各BCバイト列へ戻す。小さいmipのフレーム増分も含め、非圧縮との混在は行わない。

全ファイル・全mip・全圧縮levelを3回ずつ復元し、長さとmemcmpによる完全一致を確認した。さらにDDS全体の復元後に`DirectX::LoadFromDDSMemory`を通し、形式・寸法・mip数・subresource数・BC本体の完全一致を確認した。元DDSのメモリ読込みも同じDirectXTex APIを基準に使った。

取得済みzstd1.5.7の/MT静的Cオブジェクトと、既存DirectXTex ReleaseライブラリをMSVC /O2 /MTの実験プログラムへリンクした。外部DLLの導入はない。元ファイルのサイズ・更新時刻が各試行中に変わらないことも確認した。

後から入力SHA256を記録したところ、完全に同じDDSがあり、65件のうち50件が固有だった。50件へ重複除外してもDDS121,877,660→43,979,722bytes、63.9149%減であり、この集合では重複が主な削減要因ではなかった。hashはnative測定後に取得しており、将来の入力識別用である。元アセット名・利用頻度・現役/旧cacheの分類は未確認。

## 容量

|zstd level|DDS全体の圧縮結果|DDS全体の削減率|独立mipのBC圧縮結果|BC本体の削減率|
|---|---:|---:|---:|---:|
|1|49,345,644bytes|59.59%|49,345,944bytes|59.59%|
|3|43,981,032bytes|63.98%|44,178,806bytes|63.82%|
|9|39,916,349bytes|67.31%|40,448,075bytes|66.87%|

level3のBC形式別内訳：

|形式|件数|元のBC本体|独立mip zstd結果|
|---|---:|---:|---:|
|BC4_UNORM|13|7,757,648|2,819,133|
|BC5_UNORM|7|12,069,392|5,410,079|
|BC7_UNORM|44|98,175,376|33,326,420|
|BC7_UNORM_SRGB|1|4,096,576|2,623,174|

DDS丸ごとでは65件中1件、独立mipでは7件がlevel3で元サイズ以上になった。極小mipや入力はフレームの増分が効くため、全ファイルで必ず縮むとは扱わない。

独立mipの結果にKTX2のヘッダー、level index、DFD、アラインメントは含まれない。KTX2を生成・読み込みした結果でもなく、そのまま製品KTX2容量とは呼ばない。DDS丸ごとの結果にも、新しいキャッシュ保存ヘッダーが必要ならその増分は別に足す。

## CPUコスト

以下は65件合計。復元は同じメモリを使う3回の平均、圧縮は1回。I/O・GPU upload・起動・FPSは測っていない。codec単体は入出力バッファの確保とmemcmpを時間から除外した。DDSメモリ読込みはScratchImageの確保を含むが、入力/復元用バッファの確保は除外した。

|level|DDS全体の圧縮|DDS全体の復元|復元＋DDSメモリ読込み|
|---|---:|---:|---:|
|1|241.98ms|83.20ms|99.37ms|
|3|377.08ms|78.14ms|102.94ms|
|9|1,501.84ms|79.70ms|94.24ms|

非圧縮DDSのメモリ読込み基準は23.91ms。level3はこのCPU経路で約79.03ms増えた（1件平均約1.22ms）。一度の集合測定であり、level9の復元がlevel3より常に速いとは判断しない。level9はlevel3より約4.06MB小さい一方、圧縮は約4倍だった。

圧縮DDSを復元してDirectXTexへ渡す方式は、一時的なDDS復元バッファを追加する。最大の今回のDDSは22,369,796bytes。実際のピークworking setは測っておらず、製品化では確保上限、失敗時の復帰、入力/復元/ScratchImageの生存期間を確認する必要がある。復元したBC本体が同じなので、GPU上のVRAM容量は減らない。

## 判断と未実施事項

まず既存DDS＋zstdを追加する案は、測定した約64%の容量効果を、BC encoder・GPUフォーマット・sRGB・mip・材質の変更なしで得る候補になる。現行キャッシュは全mipを一度に読むため、今回の用途では単一DDSフレームとも整合する。ただしキャッシュ破損時のfallback、bounded decode、atomic保存、旧DDS互換、小さい場合の非圧縮維持を備えた別の製品設計が必要。実装承認済みのshader cache設計を、テクスチャへ自動的に広げてはいない。

KTX2＋既存BC＋zstdは、mip単位のアクセスや他環境への対応も進める際の比較候補になる。KTX固定版の`texture2.c`にあるzstd復元経路が、level indexに従って各levelを復元し、復元長を検証することも確認した。これだけで本プロジェクトへの統合や配布条件が完了したとは扱わない。

Basisの追加の容量効果、encode/transcode時間、色や法線角度の誤差は未測定。既存RGとBasis BC5のAlpha契約の違いも解決が必要。次の製品方針としてDDS＋zstd / KTX2＋BC＋zstd / Basis品質比較の選択をユーザーへ確認中で、製品コードの変更は回答後の設計・承認に従う。

## 再現と証拠

```
./scripts/probe-texture-storage.ps1 -Report <new-report.json>
python scripts/summarize-texture-storage.py <new-report.json> Data/Cache/Textures <summary.json>
```

入力キャッシュを上書きせず、native出力には未使用のレポート名を指定する。再現には既存VS C++ tools・DirectXTexライブラリが必要。Pythonは集計だけに使う。

- `tests/texture_storage_probe.cpp`：実験本体。初回のDDSメモリ読込み追加時にAPIのpointer型不一致でビルド失敗し、宣言に合わせて修正後に再ビルド・全65件合格。
- `bin/validation/technology/texture-storage-dds-load.log`：native exit0、全件・全mip byte一致。
- `bin/validation/technology/texture-storage-dds-load-20261031.json`：最終の形式・寸法・mip別入力とfile/level別測定値。
- `bin/validation/technology/texture-storage-summary-20261031.json`：集計、形式別内訳、入力SHA256と重複除外結果。
- 先行実験`texture-storage-20261031.json`も残す。メモリ読込み比較は含まれず、本書の時間表は最終実験に統一した。

出典：[固定版KTX texture2.cのzstd復元](https://github.com/KhronosGroup/KTX-Software/blob/00d67536657e0e1563d5ed94772c7bd1556b917b/lib/src/texture2.c#L2905)、既存DirectXTex/texturecompression実装、上記native実測。
