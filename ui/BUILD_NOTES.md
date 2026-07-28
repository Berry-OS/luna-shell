# luna-shell: GLFW を撤去した KMS/Wayland ネイティブ版

## 変更点

`luna-shell.c` から GLFW を完全に撤去し、実行時に自動選択される2つのバックエンドに置き換えました。

- **KMS/DRM + GBM + EGL + libinput** — `WAYLAND_DISPLAY` が無い、素のコンソール(dri直叩き)向け。今回の segfault の原因だったパスです。
- **Wayland + EGL + wl_seat** — `WAYLAND_DISPLAY` がある場合。**Wayback を含む**あらゆる Wayland コンポジタで動作します(Wayback は wlroots ベースのコンポジタで、Wayland ソケットを提供するため)。

`luna-ui.h` はもともと host-neutral 設計(`LunaPlatform` 経由で time/proc/cursor/close を受け取る、`LUNA_KEY_*` は GLFW 値と一致するがGLFW非依存)だったので、`luna-ui.h` 自体は無変更です。`#define LUNA_UI_GLFW` を外し、GLFWヘッダの代わりに `<GL/gl.h>` を include するだけで済みました。

## ビルドに必要なパッケージ (Ubuntu/Debian)

```sh
sudo apt install libdrm-dev libgbm-dev libegl1-mesa-dev libgl1-mesa-dev \
                 libinput-dev libudev-dev libxkbcommon-dev \
                 libwayland-dev wayland-protocols pkg-config
```

## xdg-shell プロトコルの生成 (ビルド時に1回)

```sh
wayland-scanner client-header \
  /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
  xdg-shell-client-protocol.h
wayland-scanner private-code \
  /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
  xdg-shell-protocol.c
```

## コンパイル例

```sh
gcc -O2 -Wall luna-shell.c xdg-shell-protocol.c -o luna-shell \
    -I/usr/include/drm \
    $(pkg-config --cflags --libs wayland-client wayland-egl xkbcommon libdrm gbm egl libinput libudev) \
    -lGL -lm

  # コンソール専用（依存最小、X11不要）
  gcc -Os -Wall luna-shell.c xdg-shell-protocol.c -o luna-shell \
      -I/usr/include/drm \
      -lwayland-client -lwayland-egl -lxkbcommon \
      -ldrm -lgbm -lEGL -lGL -linput -ludev -lm

  # Xorg対応あり
  gcc -Os -Wall -DLUNA_BACKEND_X11 luna-shell.c xdg-shell-protocol.c -o luna-shell \
      -I/usr/include/drm \
      -lwayland-client -lwayland-egl -lxkbcommon \
      -ldrm -lgbm -lEGL -lGL -linput -ludev -lm -lX11
```

このリポジトリの `stub_harness.c`(検証用に作った、luna-ui.h の実装を持たないダミー版)は
上記フラグで実際に `-fsyntax-only` と完全ビルド+リンクの両方を確認済みです
(本物の `luna-ui.h` は stb_truetype.h / stb_image.h / stb_image_write.h / cssparser.h
に依存しますが、これらは今回アップロードされていないため、backend部分だけを切り出して
実際のヘッダ/ライブラリに対して型チェック・リンクを行いました)。

## 実行時の権限について

- **KMS/DRM モード**: `/dev/dri/cardN` への書き込み権限と、DRMマスター権限が必要です。
  `seatd` または `logind` セッション経由が正攻法(`video` グループだけでは
  libinput の `libinput_udev_assign_seat` が失敗することがあります)。うまく
  行かない場合はまず root で試して切り分けてください。
- **Wayland モード**: 通常の Wayland クライアントと同じ権限で動きます。

## 既知の制限・今後の課題

1. **カーソルの見た目**: どちらのバックエンドも `set_cursor` はスタブです
   (`kms_backend_set_cursor` / `wl_backend_set_cursor`)。マウス位置追跡・
   クリック・ホバーは正常に動きますが、OSレベルの矢印カーソル画像は出ません。
   - KMS: DRMカーソルプレーン (`drmModeSetCursor2`/`drmModeMoveCursor`) を追加するか、
   - Wayland: `wl_cursor_theme` + shm バッファで `wl_pointer.set_cursor` を呼ぶか、
   - もしくは Luna UI 側でカーソルを自前描画するのが一番シンプルかもしれません。
2. **キーリピート**: 現状は押しっぱなしでの自動リピートを実装していません
   (libinput はリピートを送ってこないので、必要ならタイマーで自前実装)。
3. **`--size WxH`**: KMS モードでは無視されます(常にディスプレイのネイティブ
   モードを使用するため)。
4. マルチモニタ・ホットプラグ・HiDPI スケーリングは未対応(元のGLFW版もHiDPIは
   `glfwGetFramebufferSize`任せだったので同等)。
