# zstdによるシェーダーキャッシュ容量の予備測定

2026-10-30。100件調査のzstd候補を、既存DXCキャッシュの実データで追加調査した。製品ローダーや保存形式はまだ変更していない。

`scripts/benchmark-shader-cache-zstd.py` は明示指定したローカルDLLの公開C APIをctypesで呼び、v1キャッシュの32byteヘッダーを除いたobject＋diagnosticsをファイルごとに圧縮・展開する。元キャッシュには書き込まない。DLLのコピー、依存追加、インストールは行っていない。

利用したDLLは既存Gitに付属する `C:\Program Files\Git\mingw64\bin\libzstd.dll`、実行時versionStringは1.5.7。API契約は[公式v1.5.7ヘッダー](https://github.com/meta/zstd/blob/v1.5.7/lib/zstd.h)で確認した。

|level|元の本文|圧縮本文＋縮まない場合のraw保持|削減率|圧縮合計|展開合計|
|---|---:|---:|---:|---:|---:|
|1|118,097,810 bytes|86,264,078 bytes|27.0%|0.306秒|0.118秒|
|3|118,097,810 bytes|79,370,337 bytes|32.8%|0.531秒|0.130秒|
|9|118,097,810 bytes|77,573,381 bytes|34.3%|2.127秒|0.131秒|

各levelで3,054ファイルすべてについて展開後のbyte完全一致を確認した。元ヘッダー合計97,728 bytesは上の容量に含まない。結果JSONは `bin/validation/technology/zstd-cache-probe.json`。

これは単一回のメモリ上測定で、Python/ctypesの呼出し時間を含み、buffer確保・ディスクI/Oは時間計測外。旧・重複シェーダーバリアントを含む現在のキャッシュのsnapshotであり、配布用の最小セットの測定ではない。元ファイルの形式・長さは確認したが既存checksumは監査していない。別プロセスの新規キャッシュ追加を禁止した測定でもない。新しい保存形式のヘッダーコストは未定で、起動短縮やFPS向上を主張しない。

容量目的の初期候補はlevel3。level9の追加削減は約1.8MBで、圧縮時間は約4倍だった。採用前にはnativeの実ローダーで、圧縮なしの場合との差、全byte一致、旧v1読込、切詰め・不正frame・過大展開サイズ・checksum不一致で安全にcache missになること、原子的保存、圧縮が不利な場合のraw保持を検証する。旧キャッシュを一括削除する設計にはしない。

製品への採用時は公式sourceのcommit固定、BSD側LICENSEの確認と同梱、ビルド方法・配布依存・保存形式versionを決める。今回Git付属DLLを本エンジンの必須依存にしたわけではない。

再現コマンド:

```powershell
& 'C:\Users\songt\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe' scripts\benchmark-shader-cache-zstd.py --library 'C:\Program Files\Git\mingw64\bin\libzstd.dll' --output bin\validation\technology\zstd-cache-probe.json
```
