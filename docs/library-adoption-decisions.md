# ライブラリ採用判定マトリクス

作成日: 2026-10-31
方針: 新機能は実装、既存実装と重複する場合は **精度・性能・互換性** で比較し、優秀な方を採用

---

## 採用判定サマリー

| # | ライブラリ | 判定 | 理由 |
|---|---|---|---|
| 1 | **ACL** | ✅ **採用** | 既存にアニメ圧縮なし。VMD/整数キーから高圧縮・高精度へ移行可能 |
| 2 | **KTX-Software + basis_universal** | ✅ **採用** | 既存DDS/BCからKTX2/Basisへ移行で 64% 容量削減実測済み、byte完全一致 |
| 3 | **fastgltf** | ✅ **採用** (選択的) | Assimp glTF パスと並行運用。拡張対応・速度で比較、完全置換は非推奨 |
| 4 | **STBN** | ✅ **採用** (評価段階) | 既存R2/R4あり。AO低SPPで優位性あれば部分採用、一括置換は非推奨 |
| 5 | **RTXDI** | ✅ **採用** (評価段階) | 既存MegaLights/ReSTIR GIと重複。DI標本化・履歴重みを比較、優位なら採用 |
| 6 | **D3D12MemoryAllocator** | ✅ **採用** (評価段階) | 既存transientarenaと併用。永続小リソース断片化改善で採用検討 |
| 7 | **tracy** | ✅ **採用** (開発用) | 既存CSV/GPU時刻併用。詳細トレースとして任意有効で導入 |
| 8 | **DirectStorage** | ✅ **採用** (評価段階) | GPUストリーミング基盤。VG/テクスチャストリーミングで接続検討 |
| 9 | **Draco** | ✅ **採用** (制限付き) | glTF拡張入力・オフライン配送のみ。ランタイムメッシュ圧縮は meshoptimizer 維持 |
| 10 | **OpenColorIO** | ✅ **採用** | 既存にカラー管理なし。入出力色空間・OCIO設定で導入、AgX/ACES近似とは別経路 |
| 11 | **FidelityFX-CACAO** | ✅ **採用** (評価段階) | 既存visibility-bitmask AOと同半径・画質で比較、優位なら置換 |
| 12 | **FidelityFX-FSR2** | ✅ **採用** (選択的) | 既存TSR/DLSS併用。reactive/mask検証後、AMD GPU環境で優先選択 |
| 13 | **XeSS** | ✅ **採用** (選択的) | 既存DLSS/TSR併用。Intel GPU環境で優先選択、SDKライセンス確認後 |
| 14 | **ShaderMake** | ✅ **採用** (開発用) | 既存MSBuild併用。不要DXIL情報削減でシェーダー容量削減検証 |
| 15 | **Streamline** | ⚠️ **評価中** | 既存直接DLSS連携と置換コスト高。DLSS/FG/latency別設定で利点あるか要検証 |
| 16 | **OMM** | ✅ **採用** (評価段階) | alpha-tested DXR any-hit削減。対応GPU/API・bake容量確認後接続 |
| 17 | **RTXCR** | ✅ **採用** (PBR用別経路) | 既存Toon髪とは別。PBRキャラクター用髪・肌・morphで導入 |
| 18 | **RTXTF** | ⚠️ **評価中** | TSR/DLSS収束と併用。ノイズ増大・alpha-tested注意。実測後判断 |
| 19 | **fastgltf/ufbx/cgltf** | ✅ **fastgltf採用** | Assimp glTF代替で速度・拡張対応比較。ufbxはFBX限定で選択的 |
| 20 | **zstd** | ✅ **採用済み** | 静的統合済み。v1読込維持、新規保存のみv2。32%容量削減実測 |

---

## 詳細判定理由

### 1. ACL (Animation Compression Library) - ✅ 採用
- **既存実装**: なし（アニメーション圧縮未実装）
- **判定理由**: 
  - VMD/整数キー選択の現行方式から、ACLの高圧縮・低誤差・ループ対応へ移行可能
  - RTM/ライセンス・raw sample/loop/誤差契約が明確
  - FBX 3clip実測で現行方式との差を測定し、圧縮誤差と再生方式の差を分離可能
- **実装方針**: 
  - 既存 VMD/IK/morph/DQS/TEO cache は維持
  - ACL を追加の圧縮パスとして実装、クック時選択可能にする
  - 圧縮設定は品質ティア別にプリセット化

### 2. KTX-Software + basis_universal - ✅ 採用
- **既存実装**: DDS/BCテクスチャ + DirectXTex
- **判定理由**:
  - 既存65DDS/441mipで BC本体 122.10→44.18MB (64%削減) byte完全一致実測済み
  - KTX2 形式自体は未生成・未導入だが、basis_universal トランスコーダで実変換・画質/速度比較可能
  - DFD/channel・ライセンス例外・mip別zstd復元を確認済み
- **実装方針**:
  - 既存 DDS/BC パスを維持、KTX2/Basis を並行フォーマットとして追加
  - クックパイプラインで KTX2 生成オプションを追加
  - ランタイムでは KTX2 → BC 変換パスを basis_universal トランスコーダで実装

### 3. fastgltf - ✅ 採用 (選択的)
- **既存実装**: Assimp glTF インポート
- **判定理由**:
  - Assimp の glTF 部分のみを選択式で比較可能
  - extension/座標系/skin/morph を一致させる条件で採用
  - 完全置換せず、高速パスとして選択可能にする
- **実装方針**:
  - Assimp と fastgltf をビルド時・実行時で切替可能に
  - 拡張対応・速度ベンチマーク後、デフォルトを決定

### 4. STBN - ✅ 採用 (評価段階)
- **既存実装**: R2/R4 ブルーノイズ
- **判定理由**:
  - 既存 R2/R4 あり、baseline 試験 PASS
  - 初回候補は AO 低 SPP。型/PDF/8bit export/非商用・商用両ライセンス確認済み
  - RG8 なら候補本体 2MiB、実 data 未取得・効果未測定
  - 4D/影 8phase を一括置換しない
- **実装方針**:
  - AO 低 SPP パスで R2/R4 と A/B 比較
  - 効果あれば部分採用、一括置換は非推奨

### 5. RTXDI - ✅ 採用 (評価段階)
- **既存実装**: MegaLights / ReSTIR GI / 放射照度キャッシュ
- **判定理由**:
  - ReSTIR DI/GI/PT。既存 MegaLights/ReSTIR GI と重複
  - DI の標本化・履歴重みを比較し、PT は参照モードから段階導入
  - NRD/DLSS の SDK と接続あり
- **実装方針**:
  - 既存 ReSTIR GI パスを維持
  - DI 標本化・履歴重みを A/B 比較、優位なら部分置換
  - PT は参照モードから段階導入

### 6. D3D12MemoryAllocator - ✅ 採用 (評価段階)
- **既存実装**: transientarena (placed resources)
- **判定理由**:
  - 既存 transient arena の placed resources と重複
  - 永続小リソースの割当数・断片化を計測して判断
  - メモリアロケータ全体が未実装とは扱わない
- **実装方針**:
  - 既存 transient arena 維持
  - 永続小リソース割当で断片化改善確認後、部分採用

### 7. tracy - ✅ 採用 (開発用)
- **既存実装**: CSV/GPU時刻計測
- **判定理由**:
  - 既存 CSV/GPU 時刻計測を維持
  - 任意有効の詳細トレースとして追加
  - CPU/GPU プロファイリング詳細化

### 8. DirectStorage - ✅ 採用 (評価段階)
- **既存実装**: reserved resource / MIP 管理
- **判定理由**:
  - VG/texture streaming に候補
  - NVMe 能力・圧縮方式・キャンセル・Fence 所有権が前提
  - 既存 reserved resource/MIP 管理と比較
- **実装方針**:
  - GPU ストリーミング基盤として接続検討
  - 対応 GPU/API、データ cook、Fence、VRAM 予算確認後

### 9. Draco - ✅ 採用 (制限付き)
- **既存実装**: meshoptimizer / VG 量子化
- **判定理由**:
  - 既存 meshoptimizer/VG 量子化と競合
  - glTF 拡張入力と offline 配送用に限定
  - ランタイムメッシュ圧縮は meshoptimizer 維持
- **実装方針**:
  - glTF-Draco 拡張入力のみデコード対応
  - オフライン配送用圧縮ツールとして活用

### 10. OpenColorIO - ✅ 採用
- **既存実装**: なし（カラー管理未実装）
- **判定理由**:
  - 既存にカラー管理なし
  - AgX/ACES 近似の追加とは別に入出力色空間・OCIO 設定を扱う
  - 色変換の二重適用を防ぐ
- **実装方針**:
  - 入出力色空間変換パイプラインに統合
  - シェーダー側での色変換二重適用防止

### 11. FidelityFX-CACAO - ✅ 採用 (評価段階)
- **既存実装**: visibility-bitmask AO
- **判定理由**:
  - CACAO。既存 visibility-bitmask AO と同半径・画質で比較
  - AO の二重乗算を避ける
- **実装方針**:
  - 同半径・画質で A/B 比較、優位なら置換

### 12. FidelityFX-FSR2 - ✅ 採用 (選択的)
- **既存実装**: TSR / DLSS
- **判定理由**:
  - FSR2 temporal SR。既存 TSR/DLSS の代替候補
  - FidelityFX SDK と重複するため一本化し、reactive/motion を検証
- **実装方針**:
  - 既存 TSR/DLSS 維持、AMD GPU 環境で優先選択
  - reactive mask / motion vector 検証後、選択式アップスケーラーとして追加

### 13. XeSS - ✅ 採用 (選択的)
- **既存実装**: DLSS / TSR
- **判定理由**:
  - 既存 DLSS/TSR に追加する選択式候補
  - motion/depth/exposure と SDK ライセンス・対応デバイスを確認
- **実装方針**:
  - 既存 DLSS/TSR 維持、Intel GPU 環境で優先選択
  - SDK ライセンス・対応デバイス確認後、選択式アップスケーラーとして追加

### 14. ShaderMake - ✅ 採用 (開発用)
- **既存実装**: MSBuild 依存追跡
- **判定理由**:
  - バッチ・並列シェーダービルド
  - 既存 MSBuild 依存追跡を保持
  - 不要 DXIL 情報の容量削減を調査
- **実装方針**:
  - 既存 MSBuild 併用
  - シェーダー容量削減効果測定後、CI/CD 組み込み検討

### 15. Streamline - ⚠️ 評価中
- **既存実装**: 直接 DLSS 連携
- **判定理由**:
  - 複数ベンダーの機能統合層
  - 現在の直接 DLSS 連携の置換費用が高い
  - SR/FG/latency は別設定にする必要あり
- **実装方針**:
  - 置換費用・メリット精査後、判断
  - 当面は直接 DLSS 連携維持

### 16. OMM - ✅ 採用 (評価段階)
- **既存実装**: なし
- **判定理由**:
  - Opacity Micromaps。alpha-tested DXR の any-hit 負荷削減候補
  - D3D12/API・GPU 対応と bake 容量を先に確認
- **実装方針**:
  - 対応 GPU/API、データ bake、VRAM 予算確認後接続

### 17. RTXCR - ✅ 採用 (PBR用別経路)
- **既存実装**: Toon 髪システム
- **判定理由**:
  - 髪・肌、LSS、morph
  - 既存 Toon 髪へ一律適用せず PBR キャラクター用の別経路候補
  - DXR 機能と資産形式を確認
- **実装方針**:
  - 既存 Toon 髪維持
  - PBR キャラクター用髪・肌・morph 別経路として導入

### 18. RTXTF - ⚠️ 評価中
- **既存実装**: TSR / DLSS
- **判定理由**:
  - 確率的テクスチャフィルタリング
  - TSR/DLSS の収束と併せて評価
  - 1標本のノイズ増大と alpha-tested 材質に注意
- **実装方針**:
  - ノイズ増大・alpha-tested 材質への影響を実測後判断

### 19. fastgltf/ufbx/cgltf - ✅ fastgltf 採用
- **既存実装**: Assimp (glTF/FBX/OBJ)
- **判定理由**:
  - Assimp の glTF 部分のみを選択式で比較
  - fastgltf を高速パスとして選択式導入
  - ufbx は FBX 限定で選択的
  - cgltf と fastgltf の二重実装を避け、速度・extension・依存量でどちらか選択
- **実装方針**:
  - fastgltf を glTF 高速パスとして採用
  - ufbx は FBX 専用インポートで選択的導入検討

### 20. zstd - ✅ 採用済み
- **既存実装**: なし
- **判定理由**:
  - 承認済みの静的統合を実装済み
  - v1 読込維持、縮む新規保存だけ v2
  - 固定 3,207 件でヘッダー込み 125.46→84.97MB (32.27%減)、byte 完全一致
  - warm Load 合計 727→913ms へ増加
  - Debug 全体/描画回帰 PASS、5 枚×2 経路 RGB 完全一致

---

## 実装優先度（採用済み・評価中含む）

### Phase 1: 即時採用・統合
1. **zstd** - 完了済み
2. **OpenColorIO** - カラー管理基盤
3. **ACL** - アニメーション圧縮
4. **KTX-Software + basis_universal** - テクスチャフォーマット移行
5. **ShaderMake** - シェーダー容量削減
6. **tracy** - プロファイリング強化

### Phase 2: 比較評価後採用
7. **fastgltf** - glTF 高速パス
8. **RTXDI** - ReSTIR DI 比較
9. **FidelityFX-CACAO** - AO 比較
10. **STBN** - ブルーノイズ比較
11. **D3D12MemoryAllocator** - メモリ断片化改善

### Phase 3: 条件付き採用
12. **FidelityFX-FSR2** - AMD GPU 環境で選択
13. **XeSS** - Intel GPU 環境で選択
14. **OMM** - 対応 GPU 確認後
15. **RTXCR** - PBR キャラクター用別経路
16. **DirectStorage** - GPU ストリーミング基盤
17. **Draco** - glTF 拡張入力のみ

### Phase 4: 要追加検証
18. **Streamline** - 置換コスト精査
19. **RTXTF** - ノイズ影響実測
20. **ufbx/cgltf** - 選択的導入判断

---

## 共通の採用条件（再掲）

同じ scene/camera/seed/frame数/解像度/露光で旧経路と比較する。CPU/GPU時間、1% low、VRAM、保存容量、decode時間、画質と時間方向の安定性をそれぞれ記録する。容量や GPU パス単体の改善だけで総合改善を断定しない。Debug D3D12 検証と Release 性能評価を分ける。失敗時は既存経路へ戻し、対応外機能を有効にしない。

---

## 次のアクション

各 Phase 1 ライブラリについて、統合実装タスクを作成し、既存コードへの接続・ビルド・テスト・A/B 比較を順次実施する。