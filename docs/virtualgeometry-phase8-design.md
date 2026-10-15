# フェーズ8: LOD安定化と2パスOcclusion（設計案）

状態: 2026-10-15、ユーザー承認済み。実装計画は `docs/superpowers/plans/2026-10-15-virtualgeometry-phase8.md`。最新指示により実装を追加し、ビルド・テスト・実機検証は省略。達成を検証済みとは扱わない。詳細は `docs/virtualgeometry-phase8-validation.md`。フェーズ7の変更とユーザーの設定変更を保持する。

## 目的

既存資料のフェーズ8（hysteresis / two-pass occlusion）を実装する。LOD境界でカメラが小さく動いたときの切替往復を減らし、前フレームの遮蔽情報が古くても必要なgeometryが消え続けないようにする。Pageの未到着・Evict中はフェーズ7の完全な常駐cutを維持する。

## 選択肢

1. ヒステリシスだけを追加する。変更は小さいが、前フレームHi-Zで誤って遮蔽されたgeometryの復帰を解決しない。
2. ヒステリシスとnative geometry用2パス再判定を追加する（推奨）。履歴とRenderGraphの依存を増やすが、既存資料のフェーズ8を満たす。
3. 遮蔽履歴を使わず毎フレーム全量判定する。時間的な誤判定を避けられるが、遮蔽による描画負荷削減が弱くなる。

## LOD履歴

instanceとassetの同一性を確認できるgroup単位の希望refine状態をGPUで保持する。希望状態と実際に常駐しているcutを分離し、Page到着待ちを粗LOD希望として記録しない。

初期案はTarget Errorを中心に10%の幅を設ける。粗→細は上側閾値を超えたとき、細→粗は下側閾値を下回ったときだけ許可する。初回は既存閾値、Force Hierarchy Levelは履歴を迂回する。幅0で従来の選択に戻せる。

instanceの削除・ID再利用、asset変更、native切替、カメラcut、容量変更で履歴を無効化する。履歴bufferにも既存frame fenceによる寿命管理を適用する。履歴が無効・容量不足でも不完全なcutを公開しない。

## 2パス描画

1. 前フレームHi-Zで可視と判断できたgeometryを第1パスで描画する。
2. 第1パス完了後の現在depthからHi-Zを構築する。
3. 第1パスで遮蔽扱いにした候補だけを現在Hi-Zと現在transformで再判定する。
4. 再判定で可視になったgeometryを第2パスで追加描画する。

古いHi-Zで遮蔽されたgroupは、その下の候補を失わないよう再判定対象として保存する。全候補のLOD選択と常駐親への置換は同じ規則を使う。instanceごとにcut出力と再判定領域を予約してからnative成功を公開する。容量不足では従来描画へinstance単位で戻し、部分nativeとの重複を防ぐ。

history無効時は第1パスで保守的に描画する。移動objectを前フレーム位置だけで遮蔽しない。第1・第2パスのselected clusterを重複排除し、opaqueとvelocityのinstance/geometry選択を一致させる。Page requestは両パスを通じてdedupし、frame feedbackは第2パス後に回収する。

既存のRendererSettingsの2パス設定はnative対応の証拠ではない。native traversal・Mesh Shader・Hi-Z生成のRenderGraph依存を確認し、従来経路への影響と循環依存がないよう明示的に接続する。

## 表示と計測

LOD履歴の有効化と幅、native 2パスの有効化を設定可能にする。LOD切替数、第1パス可視数、再判定候補数、第2パス復帰数、容量Fallback数、追加GPU時間、履歴/候補buffer bytesを表示する。同期DebugReadbackを通常frameの統計取得に使わない。

## 受け入れ条件

- 閾値付近の往復では切替数が減り、十分な接近・離反では正しく細分化・粗化する。
- Page部分到着、Evict、再到着でも完全なcutとnative/従来排他を維持する。
- 遮蔽物の移動・除去、camera teleport、history無効化後に表示抜けが残らない。
- 第1/第2パスの重複、親子同時描画、範囲外書込がない。候補overflow時も穴を作らない。
- CPU参照とproduction GPU比較、小規模の実画面・depth・velocity比較を行う。
- 通常frameで同期待ちを追加せず、Debug layer error 0。既存Phase 6/7を回帰確認し、その後3000万〜1億source triangleで負荷を測る。

## 境界

Material binningは既存資料どおりフェーズ9。圧縮、DirectStorage、Hierarchy pagingは含めない。フェーズ7で未検証の10億triangleはこのGPUの常駐metadata容量制約を引き継ぐ。任意の制作assetに対する完全な見た目の保証は実測したfixtureと区別する。

自己確認: 希望LODと常駐cutを分離し、遮蔽groupの候補消失、候補overflow、ID再利用、両描画パスの排他、feedback時点を設計上の検証項目として明示した。詳細buffer layoutとpass順序は設計承認後の実装計画で固定する。
