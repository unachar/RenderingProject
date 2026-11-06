# KTX2 / Basis実装と比較

## 採用結果

KTX2を新しい画像入力形式として実装した。既存のDDS＋zstdキャッシュは維持する。同一65件ではKTX2への変換による容量・CPU読込の優位性がなく、既存画像の自動変換は行わない。

|形式|合計bytes|resident-buffer CPU読込中央値|
|---|---:|---:|
|DDS|122,108,212|27.5864ms|
|native KTX2|122,122,760|54.4022ms|
|KTX2＋zstd|44,202,502|150.343ms|

入力は`bin/validation/technology/library-comparison-texture-before-20261101/input`の隔離コピー。最適化ビルドで5回、実行順を回転し、各回の全画素バイトとメタデータを計測区間外で照合した。native KTX2とzstd KTX2はBCデータを含め完全一致。変換時間は65件合計でnative 67.0229ms、zstd 484.514ms。

zstd KTX2はDDSより約63.8%小さいが、同じ入力の既存DDS＋zstdキャッシュ43,983,070 bytesより219,432 bytes大きい。上表はファイルI/O・payload hash・GPU転送を含まない。既存キャッシュの読込測定にはI/Oと整合性確認があるため、別レポートの絶対時間とは直接比較しない。FPS、VRAM、cold起動の改善を示す結果ではない。

## 実装範囲

- `KtxBasis::Load/LoadFile/Encode`は所有権のあるDirectXTex画像を返す。2D mip、layer、cube、sRGB、DDS alpha modeを保持し、失敗時は出力を変更しない。
- native BC1 RGBA、BC2/3/4/5/6H/7、RGBA8・対応float形式を変換せず格納する。奇数サイズのBCも検証した。Vulkan BC1 RGBはDXGI BC1とのalpha意味の違いを避けるため拒否する。
- KTX2内のETC1S/UASTC LDRカラーはSDKのBasis transcoderでBC7へ変換する。sRGBを保持する。これは非可逆変換で、既存画像を自動変換しない。
- TextureManagerのファイル・メモリ入力、エディターの画像分類・選択フィルター、サムネイルCPU decodeへ接続した。マテリアルは既存Texture2D契約に合わせ、array/cubeを拒否する。汎用adapterでは対応する。
- decode前に寸法・mip・array・総画像量を制限する。1D/3D、非標準orientation、非identity swizzle、未対応formatは明示的に拒否する。

Basis法線・data mapのchannel契約、2component画像、単体`.basis`ファイルは未実装。Basis画質試験は16×16定色RGBAのsRGB領域での誤差確認に限られ、実資産での視覚品質比較やGPU表示検証は残る。

## SDKと検証

`scripts/build-ktx-dx12.ps1`は既存SDKを残し、`External/KTX-Software/build-dx12`と`install-dx12`へRelease共有DLLを構築する。GL/Vulkan uploadとETC unpackを無効化し、使用するimage/transcode APIを残す。生成version文字列はGit tag不在により`0.0.0-noversion`となるため、最新SDKとは扱わずExternalの固定ソースを使う。DLL・KTX/Basisライセンス類は出力先へ配置する。

Debug CRTとSDK DLLのmalloc/freeを混在させないよう、encodeはアプリ側vectorへ書き込むstreamを使用する。premultiplied alphaは独自キーだけでなく標準DFDフラグへ記録し、独立したSDK readerでも検証した。

`scripts/test-ktx-image.ps1`のRelease、`-Configuration Debug`、`-Disabled`に合格。11形式、mip/array/cube、奇数寸法、圧縮有無、所有権、破損・切断・過大入力、Unicodeパス、Basisカラー、標準alpha情報、製品のfile/memory/thumbnail/classification関数を実行した。SDK無効版はSDK include/libなしでfallbackを検証した。

Debug製品全体ビルドも成功。この環境のPDB生成C1902を回避するためビルドプロセスだけ`_CL_=/Z7`を指定した。通常Debug設定が修復されたという意味ではない。

主な証拠: `bin/ktx-image-final-tests.log`、`bin/ktx-image-debug-tests.log`、`bin/ktx-image-disabled-tests.log`、`bin/ktx-image-final-build.log`、`bin/validation/ktx/report.json`。

専用SDK DLLを配置した製品も3フレームを描画して正常終了した（`bin/validation/startup/ktx-image-desktop-Debug-1.log`、runner exit0）。これはDLL読込・通常描画・終了のsmokeであり、KTX2実資産のGPU表示品質の証明には使わない。
