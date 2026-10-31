# DXCキャッシュのzstd圧縮設計（承認済み）

2026-10-31、ユーザーが「この設計を承認して進める」と回答。承認はこの設計の範囲であり、後続の別設計を一括承認したものではない。

目的は既存DXCキャッシュのディスク容量を減らすこと。初回Aのreflection削減とは独立した後続変更で、GPU処理・shader bytecode・キャッシュキーは変えない。現在の実データ3,054件ではlevel3で本文が約32.8%縮み、全byte復元一致を確認した。詳細は[予備測定](../../rendering-technology-zstd-cache-probe.md)。起動短縮やFPS改善は未実測。

## 方針と選択肢

ユーザーが選んだ「既存DirectX 12実装へ段階的に追加」の方式を継続する。既存形式の読込を維持し、縮む新規キャッシュだけ圧縮保存する。キャッシュシステムの一括置換や既存ファイルの一括変換は行わない。

実装者提案は、zstd公開C APIを静的に組み込む方法。既存GitのDLLを製品から参照する方法もあるが、Gitの導入場所と配布先の環境に依存するため採用しない。現行のraw保存を維持する選択肢は依存を増やさないが、予備測定で見えた容量削減を得られない。静的同梱の最終採否はこの文書の承認対象。

## 依存と配置

zstd v1.5.7、commit `f8745da6ff1ad1e7bab384bd1f9d742439278e99` を固定する。調査時の[最新release API](https://api.github.com/repos/meta/zstd/releases/latest)もv1.5.7を返した。公式の[single-file生成手順](https://github.com/meta/zstd/blob/f8745da6ff1ad1e7bab384bd1f9d742439278e99/build/single_file_libs/README.md)を使い、生成したCソースをCとして、PCHなし・追加runtime DLLなしでビルドする。既存Git DLLは測定専用で、コピーしない。

`External/zstd`へ必要なソースと公開header、BSD側LICENSE、出典・生成手順・hashを記録する。ビルド時にネットワーク取得やPython生成を要求しない。生成元に戻れる手順は残す。[LICENSE](https://github.com/meta/zstd/blob/f8745da6ff1ad1e7bab384bd1f9d742439278e99/LICENSE)のsource/binary再配布条件を守る。GPUコードや資産フォーマットは対象外。

## 保存・読込契約

- `DxcShaderCache::CachePath`とargsのhashは維持。v1 magic・32byte header・raw object/diagnosticsを引き続き読める。
- 新v2 headerは6個のlittle-endian uint64（48byte）：別magic、codec=1(zstd)、object bytes、diagnostics bytes、保存payload bytes、復元後の既存checksum。payloadはobject＋diagnosticsの1つのzstd frame。
- `Store`はlevel3を試す。v2 headerを含めた総量がv1より小さい場合のみv2を保存し、それ以外や圧縮失敗時は既存v1を保存する。旧ファイルを削除して再生成させない。
- `Load`はmagicで分岐。object最大64MiB、diagnostics最大1MiBの上限、加算overflow、file長、codec、frameサイズ、宣言展開サイズを確保・展開の前に検査する。単一frameだけを受け付け、連結frameや末尾余剰も拒否する。
- 展開先は宣言済みの合計長に限定し、展開後byte数と既存checksumを検査してから成功を返す。破損・不明形式・不正サイズ・展開失敗はcache missとして既存コンパイル経路へ戻る。失敗payloadからPSOを作らない。
- 一時ファイル＋既存の原子的置換を維持。`DX12_DISABLE_SHADER_CACHE`、reflection保持指定、debug情報の契約を変えない。古い実行ファイルはv2を読めずcache missになるが、キャッシュキー・既存v1の互換性は維持する。

## 検証と採用条件

実ローダーのnative試験でv1/v2のbyte完全一致、縮まない入力のv1保持、VS/PS/CSの実DXC出力、保存・読戻し・PSO作成を確認する。切詰め、各長さの不整合、不明codec、過大サイズ、壊れたframe、連結frame、checksum不一致を拒否し再compileできることも試す。

同一キャッシュsnapshotをnativeで比較し、headerとraw fallback込みの総容量、圧縮・読込時間、EXE増分を記録する。Python probeの時間を製品時間として流用しない。Debug全体build、既存policy試験、features/fallback smokeを実行する。容量が有意に縮まず、または読込に不釣合いな増分が出る場合は既定採用せず結果を残す。AAや他の後続候補の完了を、この変更だけで主張しない。

## 実装者が決める範囲

helper分割、生成ソースのローカルファイル名、native runner構成、境界fixtureは可読性を優先して決める。辞書、マルチスレッド圧縮、常駐worker、UI、アニメーション/モデル/VG/BCの再圧縮、cache evictionは追加しない。新しい使用形式やruntime DLLが必要になった場合は、この設計を更新する。
