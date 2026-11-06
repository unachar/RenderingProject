# DXCキャッシュのzstd静的統合

2026-10-31にユーザーが承認した[設計](superpowers/specs/2026-10-30-shader-cache-zstd-design.md)を実装した。目的はキャッシュのディスク容量削減。GPU性能・起動時間の改善とは扱わない。

## 保存・読込みの契約

- キャッシュキー、DXC引数、reflection/debug/cache無効化設定は変更しない。既存v1をそのまま読む。既存キャッシュを一括変換・削除しない。
- v1は32byteヘッダー＋非圧縮object/diagnostics。v2は48byteヘッダー（magic、codec=1、objectBytes、diagnosticBytes、storedBytes、checksumの6個のuint64、既存Windows x64のlittle-endian）＋単一zstdフレーム。
- level3で圧縮し、ヘッダー増分16bytesを含めても小さくなる場合だけv2を保存する。縮まない場合や圧縮失敗時はv1。既存の一時ファイル＋MoveFileExW置換を維持する。
- objectは1byte～64MiB、diagnosticsは0～1MiB。確保前にヘッダー、サイズ上限、ファイル長を確認し、既知かつ一致するフレーム内容長、単一フレーム境界、復元長、既存FNVチェックサムを検証する。未知サイズ・連結・末尾余分データ・skippable frame・破損はミス扱い。失敗時の出力は空で、DXCへ戻る。
- コーデックは`source/shadercachecodec.h`、ファイル操作は既存`source/dxcshadercache.h`に分けた。公共Load/Store/Hashの呼出し方は維持した。

## 依存とビルド

[zstd1.5.7の固定コミット](https://github.com/meta/zstd/tree/f8745da6ff1ad1e7bab384bd1f9d742439278e99)から公式生成器を使い、`External/zstd/zstd.c`を作成。元ソース74個のGit blob IDを固定コミットと照合し、配布ファイルを含む78個のSHA256を記録した。`scripts/vendor-zstd.py --check`でオフライン検証できる。

生成器が残す`../zstd.h`だけを隣接配置用の`zstd.h`へ調整した。取得したupstreamソースは未変更。再生成にはPythonのUTF-8モードを明示し、Windowsの既定cp932による文字変換を避けた。通常の製品ビルドにPythonやネットワークは不要。

BSDライセンスを選択し、出典・生成手順・ハッシュと同梱した。`CopyZstdLicense`ビルドターゲットが出力の`licenses/zstd-LICENSE.txt`へ配置し、内容ハッシュの一致を確認した。

製品はCとしてPCHなし、局所的な/O2でコンパイルし、CRTは既存設定を継承する。今回のDebugは/MTd。スタンドアロン検証用Cオブジェクトは/MTで別生成する。既存4個のDXC利用検証スクリプトにも静的オブジェクトをリンクした。dumpbinの製品依存一覧にzstd DLLはない。workerやdictionaryは要求していない。

Debug全体ビルドはVsDevCmdと一致させたMSBuild、/Z7、x64、/m:1、`IntDir=build\extension-debug\`でexit0。既存ImGuizmo等の警告と/ZI→/Z7上書き警告は残る。EXEは44,950,016→45,685,248bytes（+735,232bytes、約1.64%）。Releaseの全体ビルドは今回の検証対象外。

## 検証

`bin/validation/technology`配下にログと比較結果を保存した。

|対象|結果・記録|
|---|---|
|変更前のRED|`zstd-codec-red.log`：圧縮可能なStoreがv1のままであるため失敗|
|ネイティブcodec|`zstd-codec-green.log`：524 checks PASS、旧v1、縮小v2、非縮小fallback、ヘッダー増分境界、切詰め、上限/overflow値、codec、checksum、未知内容長、内容長不一致、連結/末尾/skippable、出力クリア、binary診断、無効Store|
|実DXC/PSO|`zstd-dxc-policy.log`：VS/PS/CS、debug/非debug、既存policy、9件の永続化とチェックサム、実PSO PASS|
|既存キャッシュ|`zstd-shader-cache.log`：include依存、variant、破損、依存欠落、キャッシュ利用不可 PASS|
|サムネイル|`zstd-thumbnail.log`：dispatch、並列コンパイルとcompiler分離 PASS|
|AA GPU|`zstd-aa-gpu.log`：CPU10201ケース、compute各128、raster各512、実visibility再構成 PASS|
|製品ビルド|`zstd-debug-build.log`：exit0。ライセンス配置ターゲットも別途実行しハッシュ一致|
|製品features|84frames PASS、D3D12 errors0/failures0。`zstd-features`へ保存。5枚のRGBは既存after-aa-featuresと完全一致（`zstd-feature-captures.json`）|
|製品fallback|84frames PASS、D3D12 errors0/failures0。`zstd-fallback`へ保存。5枚のRGBは既存after-aa-fallbackと完全一致（`zstd-fallback-captures.json`）。runner75411はexit0|

独立した読み取り専用レビューではコードの修正指摘なし。cold start、Release速度、スモーク結果はレビュー対象外であり、スモークの合格は上記の実行結果で別途確認した。

## 容量と読込みコスト

初回の探索実行`zstd-native-20261031/report.json`は3,207件すべてを復元一致、invalid0、全件縮小として記録した。ヘッダー込み125,458,634→84,967,209bytes（32.2747%減）。最初の速報3,055件は途中のフォルダー数を読んだ誤りで、最終reportに合わせて訂正した。

初回は入力走査と描画検証が一部重なった可能性があるため、その時間値は採用しない。描画プロセス終了後、保存したrawスナップショットを固定入力にして再測定した。最終結果は`zstd-native-fixed-20261031/report.json`、runner75435はexit0。3,207件、invalid0、全件byte一致、ヘッダー込み容量は上記と同じ125,458,634→84,967,209bytesだった。

|固定入力3,207件の合計|測定値|
|---|---:|
|元v1 Load、warm3回平均|727.458 ms|
|新Loadのv1経路、warm3回平均|702.369 ms|
|新Loadの圧縮経路、warm3回平均|912.591 ms|
|raw CPU decode、3回平均|147.925 ms|
|圧縮CPU decode、3回平均|348.053 ms|
|圧縮CPU、1回|919.104 ms|
|rawファイル単純読込み、1回|4,465.29 ms|
|圧縮ファイル単純読込み、1回|5,556.93 ms|
|新Store（圧縮・checksum・I/O・置換を含む）、1回|14,374.8 ms|

旧Loadに対する圧縮経路の増分は約185.1ms（約25.4%、1件平均約0.058ms）。単純読込みは書込み直後の別フェーズで、後続のwarm反復と条件が異なる。小さなファイルを多数開くコストも含まれ、バイト数から速度向上を推定しない。各passの個別値もreportに保持した。

比較器は既存cacheを書き換えず、出力は未使用の別ディレクトリに限定する。元のv1 Loadを基準実装として持ち、新Loadのv1互換・圧縮経路の双方をbyte比較する。CPU encode/decodeとファイル読込み、Storeの全体時間を分け、warm readは順序を交互にした3回を報告する。

32.27%の容量効果と、この集合で約185msのwarm読込み増を交換条件として、承認済み設計どおり新規保存時のbeneficial-only v2を採用する。これは容量向けの採用判断であり、速度向けの採用判断ではない。実測後の元キャッシュはv1が3,197件、v2が10件、未知magic0件（`zstd-original-cache-counts.json`）。既存v1を残したため、元フォルダー全体が32.27%縮んだという意味ではない。

この測定はシェーダーコンパイル自体、cold disk、起動全体、GPU時間、FPSを測っていない。旧版・重複variantを含む現在のキャッシュ集合であり、すべての利用者の削減率ではない。

100件全体の目標は継続中。この追加の完了を、全候補の実装完了とは扱わない。
