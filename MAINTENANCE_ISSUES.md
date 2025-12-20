# CAN 送受信ライブラリのサンプルとして整備・メンテナンスすべき課題一覧

このリポジトリが「CAN の受信・送信を管理するライブラリのサンプル（C 言語）」として
使われることを前提に、現状で整備・メンテナンスすべき課題を整理した一覧です。

## ドキュメント/利用手順
1. **README が実質空**
   - **概要**: 現在 `README.md` がタイトルのみで、ライブラリの目的、対応環境、ビルド・実行手順が不明。
   - **影響**: 初見の利用者が使い方に辿り着けない。
   - **該当ファイル**: `README.md`

2. **SocketCAN/vcan セットアップ手順の説明不足**
   - **概要**: `scripts/` に vcan 作成スクリプトがあるが使い方が明文化されていない。
   - **影響**: 実行前の環境準備が不明で動作検証が難しい。
   - **該当ファイル**: `scripts/create_vcan.sh`, `scripts/init_vcan.sh`, `scripts/setup.sh`, `README.md`

3. **公開 API の使い方/制約の文書化不足**
   - **概要**: `include/can_manager.h` に API はあるが、サンプルコードでの推奨利用方法や制約
     (例: `base_period_ms` の取り扱い、`add_canfd_frame` の `period_ms` 制約など) が文書化されていない。
   - **影響**: 誤った使い方による不具合が発生しやすい。
   - **該当ファイル**: `include/can_manager.h`, `src/can/*.c`, `sample/*.c`

4. **ライセンス/著作権表記の未整備**
   - **概要**: ライセンスファイルが存在しないため、再利用・配布条件が不明。
   - **影響**: 外部への提供・利用に支障が出る。
   - **該当ファイル**: (新規) `LICENSE` など

## ビルド・テスト
5. **ビルド対象の整理と入口の統一**
   - **概要**: ルート `makefile` はライブラリ + `src/main.c` をビルドするが、
     サンプル (`sample/`) との関係が README で説明されていない。
   - **影響**: どの成果物が「サンプル」なのか分かりにくい。
   - **該当ファイル**: `makefile`, `sample/makefile`, `src/main.c`, `README.md`

6. **テスト対象の偏り**
   - **概要**: `tests/` は `can_manager` 中心で、`can_rx_queue` や `can_tx`/`can_rx` スレッド挙動の
     直接検証が少ない。
   - **影響**: 受信キューや送信/受信スレッドの回帰検知が弱い。
   - **該当ファイル**: `tests/src/test_can_manager.c`, `src/can/can_rx_queue.c`, `src/can/can_tx.c`, `src/can/can_rx.c`

7. **CI の未整備**
   - **概要**: 単体テストは `tests/makefile` で実行可能だが、CI (GitHub Actions など) が未設定。
   - **影響**: 変更による品質劣化の検出が遅れる。
   - **該当ファイル**: (新規) `.github/workflows/*.yml`

## 実装品質/安全性
8. **スレッド同期の方針が不明確**
   - **概要**: `tx_set_enabled`/`tx_set_brs` はロックなしでフラグ操作しており、
     `can_tx.c` の送信スレッドと競合する可能性がある。
   - **影響**: データ競合や不定動作の潜在リスク。
   - **該当ファイル**: `src/can/can_manager.c`, `src/can/can_tx.c`, `src/can/can_internal.h`

9. **I/O エラー処理の方針が曖昧**
   - **概要**: 送信 (`write`) の失敗は現状無視される箇所が多く、ログ/再送の方針がない。
   - **影響**: 失敗時に原因究明が難しい。
   - **該当ファイル**: `src/can/can_tx.c`

10. **入力バリデーションの不足**
    - **概要**: `open_canfd` で `base_period_ms` が 0/負数でも処理される、
      `can_rx_queue_*` は NULL 引数を想定していない、など。
    - **影響**: 誤った引数でクラッシュ/不正動作の可能性。
    - **該当ファイル**: `src/can/can_manager.c`, `src/can/can_rx_queue.c`

11. **公開 API とサンプルの乖離**
    - **概要**: `sample/` に独自実装が多数あり、`can_manager` API を使うサンプルが少ない。
    - **影響**: ライブラリの「使い方サンプル」としての価値が薄れる。
    - **該当ファイル**: `sample/*.c`, `include/can_manager.h`, `src/can/*.c`

## コード整理
12. **ヘッダ/API の整理と命名の統一**
    - **概要**: `tx_update_payload` の非推奨 API が残っているが、
      非推奨の明示・代替 API の例示が不足。
    - **影響**: 古い API が使われ続ける。
    - **該当ファイル**: `include/can_manager.h`, `src/can/can_manager.c`

13. **定数・制約値の明文化**
    - **概要**: `MAX_TX_OBJECTS` などの制約はコード内にあるが、
      README や API ドキュメントでの説明がない。
    - **影響**: 利用者が制約に気付かず設計を誤る。
    - **該当ファイル**: `src/can/can_internal.h`, `include/can_manager.h`, `README.md`

---

上記は、サンプルライブラリとしての「使いやすさ」「安全性」「保守性」を高めるための
優先候補です。実際の整備順序は、利用者の要件（ドキュメント重視か、品質重視か）に
応じて決めるのが適切です。
