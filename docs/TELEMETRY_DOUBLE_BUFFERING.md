# Telemetry UI Double Buffering

## 目的

Hardware D3D12実行時に見えていた数値UIのちらつきを、WARPの低い更新頻度に頼らず描画構造で解消します。

## 旧経路

```text
SetWindowTextW
→ STATIC controlが背景を消去
→ 全文を再描画
```

背景消去と文字描画の間がDWMに合成されると、背景だけの瞬間が見えました。

## 新経路

```text
SetTelemetryText
→ telemetryText_を更新
→ InvalidateRect(panel, nullptr, FALSE)

WM_PAINT
→ CreateCompatibleDC
→ CreateCompatibleBitmap
→ 背景・枠・全文をmemory bitmapへ描画
→ BitBltで一括転送
```

`WM_ERASEBKGND`は処理済みとして返すため、Windowsによる先行背景消去はありません。

## 境界

この変更は`11_PlatformWin32`だけです。

- Frozen Package: 変更なし
- D3D12 Package Schema V6: 変更なし
- Compiler: 変更なし
- Runtime: 変更なし
- D3D12 Executor: 変更なし
- Launcher telemetry内容: 変更なし
- D3D12 render child HWND: 変更なし

数値UIは今までどおり100ms周期で更新されますが、各更新は完成済みの一枚絵として画面へ転送されます。
