# ライブ向けライト

ライトのインスペクターに「物理ベース」を追加した。

- オン: 従来の減衰・矩形LTCを維持する。既存シーンはこの設定で読み込む。
- オフ: ポイント／スポットは照射範囲の内側で距離による明るさの変化をなくし、末端1mだけ滑らかに減衰する。範囲1m未満では範囲全体が境界になる。スポットの照射角、法線、遮蔽、露出による変化は残る。
- 矩形のオフ時は中心方向で近似し、発光面積による強度の増加をなくす。LTCの面積積分から切り替わるので、オン／オフで同じ見た目を保証しない。

「照明経路」の Physical / Decal / Emission Only は別設定。Physical経路でも物理ベースをオフにでき、Forward・影の使用を維持できる。チェック値とカプセル寸法はシーン／Prefabの共通シリアライズに含まれる。既存ライトの保存値は変更しない。

## カプセルライト

追加メニューの「ライト > Capsule」、またはライトタイプから選べる。
ライト方向を軸として中心から両側に伸びる線分と、発光半径で近似する。
カプセル長・カプセル半径・範囲・色・強度を調整する。範囲は発光面からの距離。
新規作成時は物理ベースがオフ。カリングには長さと半径を加味する。

不透明／Forward／反射・GIの共通照明式で評価する。カプセルは従来のタイル経路を使用し、MegaLightsの確率的形状サンプリング・影・フォグには対応しない。長さ0・半径0ならポイントライトと同じ計算になる。

学園アイドルマスターの開発元公開資料で紹介されたデカールライトのうち、既存のポイント／矩形に不足していたカプセル形状を追加したもの。ゲーム固有のキャラクター専用照明やRenderTexture連動の完全再現ではない。

参考: https://speakerdeck.com/cyberagentdevelopers/xue-yuan-aidorumasutano-aidoruwoyorihui-kaseru-raiteingushou-fa

## 検証

```bash
bash scripts/test-stage-lighting.sh
DXC=/path/to/dxc bash scripts/test-stage-lighting.sh --shaders
bash scripts/test-live-toon.sh
```

Windowsの既存 `scripts/test-live-toon.ps1` にもStageの数値テストを追加。
ステージ専用スクリプトは、実際のcommon HLSL関数をC++で実行する統合数値テストと、Deferred／Atmosphere／Fog／ReflectionのDXCコンパイル（Forwardは既存live-toon検証）を実行する。シェーダー検証は実用設定の最適化（-O3）で実行する。
Windowsアプリ全体のMSBuildとD3D12実描画は別途実機確認が必要。

実装時の確認結果: 数値テストと本番関数・CPUパッキングの統合テスト、既存のセル／輪郭／スキニングテスト、ポータブルなPython回帰8本、DXC -O3の17バリアントが通過。Windows専用MSBuild契約テストはLinuxでは実行できない。
