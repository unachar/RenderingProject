# KTX2 / Basis Universalの統合前調査

100件の調査表の57・58番を対象に、既存BCキャッシュとの接点を確認した。KTX/Basisの製品組込み、変換器のビルド、Basisの実画像の誤差・速度測定はまだ行っていない。後続の[既存BCの可逆圧縮比較](rendering-technology-texture-storage-probe.md)では全65件・441mipのbyte復元一致とCPUコストを測定した。

固定した対象はBasis Universal `073eb6a0a34a23d77e75e54f41dcd9653a52da83`、KTX-Software `00d67536657e0e1563d5ed94772c7bd1556b917b`。rootライセンス、Basis公開transcoderヘッダー、KTXのlib CMakeとbasis_transcode.cppを読んだ。Basisの巨大なtranscoder.cpp本体はコネクターの取得制限で読めていない。

既存実装の`source/assets/texturecompression.inl`はColorをBC7、NormalとTwoChannelをBC5、SingleChannelをBC4へDirectXTexで圧縮し、`Data/Cache/Textures`のDDSを再利用する。ソースのパス・サイズ・更新時刻・用途・encoder版でキーを分け、保存は一時ファイルから置換する。同期読込みの既定は初回非圧縮利用＋バックグラウンド生成。現在のDDSは65件、合計122,108,212 bytesだった。この数値は容量の棚卸しであり、Basis導入による削減見積りではない。

Basis公開APIにはBC4/BC5/BC7への変換とKTX2読込みがある。ただし`cTFBC5_RG`はRとAlphaからXYを作り、AlphaがなければYが255になると明記されている。既存のRG法線をそのまま渡せるとは扱わない。KTX側はDFDからETC1SのAAA/GGG、UASTCのRGBA/RRRGを読み、第二成分の意味を区別する。法線・2chではencoder側のchannel指定と実際の復元XYを確認する試験が必要になる。

BasisヘッダーのKTX2 zstdサポートは既定ONで、`BASISD_SUPPORT_KTX2_ZSTD`で切替える。KTX側にもzstd依存がある。shader cacheへ導入したzstdと将来共有できるかは、各固定版のビルドとAPIの確認が必要で、依存が既に解決したとは扱わない。

KTXのrootライセンスは、独自部分の多くがApache-2.0である一方、複数の依存ライセンスがあると説明する。特に`external/etcdec/etcdec.cxx`は別のEricsson条件であり、`LIBKTX_FEATURE_ETC_UNPACK=OFF`で省略できる。CMakeではETC unpackとGL/Vulkan uploadが既定ONだった。DirectX 12向けの評価では、必要なコンテナ読込みと変換部分を絞る案を先に比較する。Basis rootはApache-2.0。個々の依存を含めた配布条件の確認は、実際に選んだファイル集合に対して行う。

Basisに関する次の実験候補は、既存DDSを基準に、代表的なColor・Normal・Single/TwoChannelを少数選び、保存容量、encode時間、BCへのtranscode時間、ピークメモリ、mip/sRGB/法線XYの復元誤差を比較すること。GPU上で同じBC形式・寸法・mip数へ戻す場合、コンテナが小さくなってもVRAM削減とは扱わない。追加の不可逆変換を伴うため、zstdのbyte完全一致評価とは基準を分ける。

追加でKTX `lib/src/texture2.c`のzstd復元も読んだ。level indexを使って各levelを個別に復元し、期待した復元長を検証する。後続比較の独立mipフレームはこの粒度を参考にしたが、KTX2を実際に生成・ロードした試験ではない。

出典：

- [Basis公開transcoder API](https://github.com/BinomialLLC/basis_universal/blob/073eb6a0a34a23d77e75e54f41dcd9653a52da83/transcoder/basisu_transcoder.h)
- [Basis LICENSE](https://github.com/BinomialLLC/basis_universal/blob/073eb6a0a34a23d77e75e54f41dcd9653a52da83/LICENSE)
- [KTX LICENSEと例外](https://github.com/KhronosGroup/KTX-Software/blob/00d67536657e0e1563d5ed94772c7bd1556b917b/LICENSE.md)
- [KTX lib CMake](https://github.com/KhronosGroup/KTX-Software/blob/00d67536657e0e1563d5ed94772c7bd1556b917b/lib/CMakeLists.txt)
- [KTX Basis変換](https://github.com/KhronosGroup/KTX-Software/blob/00d67536657e0e1563d5ed94772c7bd1556b917b/lib/src/basis_transcode.cpp)
