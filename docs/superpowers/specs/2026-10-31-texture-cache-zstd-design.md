# 既存DDSキャッシュへのzstd追加設計（承認済み）

2026-10-31、ユーザーが方式の選択で「既存DDS＋zstdを優先（推奨）」、続いて本書への承認確認に「この設計を承認して実装を進める（推奨）」と回答した。承認は本書の範囲に適用する。

## 目的と根拠

既存のDirectXTexによるBC圧縮とDDS読込みを維持し、ディスク保存にだけ可逆圧縮を追加する。BC形式、GPU上のバイト、mip、sRGB、材質、法線XY、キャッシュキーを変えない。新たなKTX/Basis依存を追加しない。

[実測](../../rendering-technology-texture-storage-probe.md)では65DDS・441mipが全byte一致で復元され、level3でDDS122,108,212→43,981,032bytes（約63.98%減）。同一メモリからのDDS読込みは65件合計23.91ms→復元込み102.94msとなり、約79ms増えた。I/O・起動・FPSの改善は未測定。復元用の一時メモリも増える。

## 比較した方式

1. **既存DDS＋zstd（選択済み）**：既存BCの品質とロード資産を保ち、導入済みの静的zstd1.5.7を再利用する。現在の全mip一括読込みに合う。単一DDSの復元バッファが必要。
2. KTX2＋既存BC＋zstd：mip単位アクセスや他環境対応も進める場合の候補。別コンテナ/APIと依存・ライセンス集合の選定が必要。現時点ではKTX2そのものの生成・ロードは未測定。
3. Basisを先に比較：追加の容量効果を狙う候補だが、encode/transcode、色・法線角度の誤差を測る必要がある。BC5のchannel意味も既存RGと調整が必要。

## 旧形式と新形式

- 既存`Data/Cache/Textures/<key>.dds`は読み続け、キーとencoder versionを変えない。一括変換、旧DDSの掃除・削除は行わない。
- 新規保存では、DDS全体をzstd level3で圧縮し、新ヘッダー込みで元DDSより小さい場合だけ同じkeyの`<key>.dds.zst`へ保存する。それ以外は従来の`.dds`を保存する。旧DDSと新sidecarを両方必須にはしない。
- sidecarのヘッダーは32bytes、little-endian uint64を4個：magic `0x003154535A534444`（`DDSZST1\0`）、元DDS byte数、圧縮byte数、元DDS全体のFNV-1a 64bitチェックサム。zstd codecとversion1はmagicに含める。dictionary・worker threadは使わない。
- 元DDSは136bytes以上、**256MiB以下**を圧縮対象にする。圧縮byte数は正かつ元DDS以下。ヘッダー長との加算前に上限を確認し、ファイル長が32＋圧縮byte数と一致することを確かめる。256MiBを超えるDDSは従来保存へ戻り、元アセットの対応上限を狭めない。
- 将来の互換性のため、未知magic/versionはcache miss。新形式は既存DDSを復元する保存形式であり、GPU texture formatを表す別仕様は作らない。

## 読込みと失敗時の動作

`TryLoadCached`はEnabledとkeyの既存判定を維持した上で、sidecar、旧DDSの順に試す。

1. sidecarは確保前にヘッダー・上限・実ファイル長を確認する。
2. 単一zstdフレーム、既知かつ一致するcontent size、フレーム終端の一致、復元長、FNV checksumを確認する。切詰め、連結、余分な末尾、未知サイズ、skippable frame、破損は不採用。
3. 復元したDDSを従来と同じ`DDS_FLAGS_NONE`の`LoadFromDDSMemory`へ渡す。既存と同じ`IsCompressed`条件を満たした結果だけを採用する。新しい用途別の形式変換は加えない。
4. 不正なsidecarまたは確保/復元失敗なら旧DDSへ進む。両方失敗ならfalseを返し、既存のソース読込み・バックグラウンドBCキャッシュ生成へ戻す。壊れたsidecarだけを再生成用に除去してよい。正常な旧DDSは削除しない。既存の壊れた旧DDSへの自己修復動作も維持する。
5. staging用のmetadata/ScratchImageを使い、成功時にだけ呼出し側へ渡す。不正な中間結果をGPU uploadへ渡さず、失敗時は既存の呼出し側データを壊さない。

CacheHitsは採用した1件だけを加算する。既存のSourceBytes/CompressedBytesはBC変換の統計なので、その意味をディスクzstd容量へ変えない。キャッシュ全体を無効にするSettings.Enabledと、初回非圧縮利用＋バックグラウンド生成の既定は維持する。

## 保存とコードの境界

既存`texturecompression.inl`の`SaveCache`/`TryLoadCached`から、新しい小さなstorage helperへファイル操作を委譲する。ヘッダーとzstdの検証は別のcodec helperへ分け、単体でnative検証できる形にする。DXC shader cacheの形式・コードは変更しない。共通化のために既存shader cacheを再構成しない。

保存は既存ScratchImageから`SaveToDDSMemory`でDDSを作り、圧縮と比較を行う。対象上限超過、staging確保・DDSメモリ化・圧縮の失敗、非縮小の場合は従来の`SaveToDDSFile`経路へ戻る。どちらの形式もPID/TIDを付けた同じディレクトリの一時ファイルへ書き、closeの成功後に`MoveFileExW(..., MOVEFILE_REPLACE_EXISTING)`で置換する。失敗した一時ファイルは除去し、既存の正常なcacheを途中書込みで壊さない。

圧縮保存時に既存DDSを消さず、raw保存時に既存の正常なsidecarを消す操作も加えない。同じkeyの並行保存が起きても、読み手には完全な候補かmissだけが見えることを確認する。sidecarは独立した拡張子にし、従来DDSツールに圧縮データをDDSとして渡さない。

codecはOOM・zstdエラーをbest-effortの失敗として扱う。最大256MiBの復元/圧縮用バッファとScratchImageが重なる場合があるので、生存期間を短くし、パスごとに不要なコピーを避ける。小さい実コーパスでの成功だけを最大サイズ・実working-setの証拠にはしない。

## 受け入れ条件

- nativeテストで旧DDS、圧縮sidecar、縮まない入力、ヘッダー込み境界、欠落、異常な長さ/overflow/codec、フレーム破損/連結/未知内容長、checksum違いを検証する。壊れたsidecar＋正常な旧DDSは旧DDSへ復帰し、両方無効ならmissになる。
- 実DirectXTexを使い、成功後のformat・寸法・array/mip・BC本体を比較する。保存ファイルの検証はhelperを実際に呼び、模造のLoad/Storeだけで代用しない。並行保存とatomic置換、Settings.Enabled/key0、統計の意味も確認する。
- 65DDSのコピーを隔離した入力にして、製品helperの保存/読込み、ファイル総量、byte一致、CPU/I/O・追加一時メモリの範囲を測る。元cacheは変更しない。新ヘッダー込みの値で判断し、予備測定だけで容量/起動改善を確定しない。
- Debug全体ビルド、既存DXC cache/thumbnail関連検証、features84/fallback84、固定時計キャプチャの比較を行う。画素差があれば原因を調べる。変更と無関係な合格済み検証は再利用する。
- 独立レビューで重要な指摘を解消し、最終の容量・読込みコスト・ピークメモリの限界と結果を記録する。shader cache同様、容量効果と読込み増を明示して採否を判断する。

## 今回の対象外

KTX/Basisの組込み、BC encoder品質の変更、VRAM削減、streaming/mipの部分ロード、一般アセットarchive、既存DDSの一括変換・重複除外、UI・全体キャッシュ管理の追加は含めない。

## レビューと実装者判断

2026-10-31、独立したbuilder checkは「nothing important left」。追加のユーザー質問が必要な設計上の穴はなかった。実装者の判断として、別拡張子、32byteヘッダー、256MiBの圧縮対象上限、上限超過時の旧保存復帰を具体化した。これらも本書の承認範囲に含む。
