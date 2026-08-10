# Vespera / Luna Desktop

pure Rust 製 Wayland コンポジタ + libwayland-client 実装 + macOS 風デスクトップシェル (`luna-shell` / `ui/luna-shell.c`)。GTK4 アプリを動かしつつ、Xorg や Weston 等の既存コンポジタの代わりに起動できる。

## 製品名とレイヤー

| 名前 | 実体 | 役割 |
|------|------|------|
| **Luna Desktop** | セッション全体 | カーネル起動後にユーザが触るデスクトップ環境 |
| **luna-compositor** | `luna-compositor` | DRM/KMS 上の Wayland コンポジタ（Xorg/Weston の代替） |
| **luna-shell** | `ui/luna-shell.c` | メニューバー・Dock・Launchpad 等の macOS 風シェル UI（Luna UI エンジン） |
| **Luna UI** | `ui/luna-ui.h` の HTML/CSS エンジン | シェルや設定アプリの UI ツールキット |
| **wayland-client-rs** | `libwayland_client.so` | GTK 等が接続する Wayland クライアント実装 |

内部コード名は **Vespera** のまま。ユーザー向けには **Luna** で統一する。

## 起動フロー（カーネル後）

```
systemd / init
  └─ luna-session
       ├─ luna-compositor  (--backend dri)
       ├─ luna-shell       (--desktop, Wayland クライアント)
       └─ GTK アプリ       (WAYLAND_DISPLAY + LD_PRELOAD=libwayland-client)
```

## luna-shell（デスクトップシェル）

`ui/luna-shell.c` は Luna UI エンジン単体で macOS 風のデスクトップ環境を描画する:

- 半透明メニューバー（三日月ロゴの Luna メニュー・時計・ネットワーク/電源状態）
- Dock（ホバー拡大アニメーション・起動中インジケータ・Trash）
- Launchpad（アプリグリッド + インクリメンタル検索, Super/F4 で開閉）
- コントロールセンター（トグル・輝度/音量スライダー・CPU/RAM メーター）
- デスクトップウィジェット（時計・CPU/メモリ/ディスク統計, /proc から取得）
- About This Luna・通知トースト・Shut Down/Restart/Log Out 確認ダイアログ

Dock / Launchpad のアプリは環境変数 `LUNA_APP_<NAME>`（例: `LUNA_APP_TERMINAL=foot`）で差し替えられる。レイアウトは `ui/luna-shell.html` + `ui/luna-shell.css` を実行ファイルへ埋め込み、`LUNA_DESKTOP_LAYOUT` / `LUNA_DESKTOP_CSS` または `--layout` / `--css` で外部ファイルに差し替え可能。

### デスクトップスキン

**System Settings → Appearance → Desktop Skin** から、シェルを終了せずに
スキンを切り替えられる。選択内容は `~/.config/luna-shell/settings.conf`
へ保存される。`skins/` には **Windows XP**、**Windows 95**、**BeOS**、
**Classic Mac OS**、**Amiga Workbench**、**Nocturne Atelier** を収録している。

### 言語・キーボードロック

**System Settings → Language & Region** ではセッションの言語・地域ロケールと
ログイン時の NumLock を変更できる。言語は変更後に起動したアプリへ即時反映し、
すべてのアプリには次回ログイン時に反映する。NumLock / CapsLock / ScrollLock は
Wayland の locked modifier と実キーボードのLEDへ同期されるため、テンキーと各ロックキーを
通常どおり使用できる。キーボード配列は **Keyboard** で変更でき、複数配列では
Alt+Shift で切り替える。

スキンは次の外部ファイルで構成する:

```text
my-skin/
├── skin.conf
├── style.css
├── layout.html        # ブラウザで編集し、同じファイルをシェルが読む
├── gtk/<ThemeName>/   # 任意: GTK 3/4 テーマ
│   ├── index.theme
│   ├── gtk-3.0/gtk.css
│   └── gtk-4.0/gtk.css
└── qt/
    ├── style.qss      # 任意: Qt スタイルシート
    └── colors.conf
```

`layout.html` が開発用の本体。共有ベース CSS とスキン用 CSS を `<link>` し、
通常のブラウザで開いた見た目がそのままデスクトップになるようにする:

```html
<link rel="stylesheet" href="../_base/luna-shell.css">
<link rel="stylesheet" href="style.css">
```

`skins/<name>/layout.html` をブラウザで開いて HTML/CSS を直し、同じファイルを
テーマとして使う。luna-shell は既存の HTML/CSS エンジンでその `<link>` を読む。
共有ベースは `skins/_base/`（スキンとしては扱わない）。アイコンフォントは
`skins/fonts/`（`_base` からは `../fonts`）に置く。`layout.html` があれば
自動で採用され、`skin.conf` の `layout=` で名前を上書きできる。

```ini
name=My Skin
description=Appearance に表示する短い説明
css=style.css
layout=layout.html
# 配置（Wayland レイヤー表面に即時反映）:
chrome=taskbar          # taskbar | deskbar | menubar | luna
menubar_edge=bottom     # top | bottom
menubar_height=34
dock_mode=hidden        # float | hidden
# ツールキットテーマ（新規起動のアプリ向け。起動中の GTK/Qt は再起動が必要）:
gtk_theme=LunaWindows95
qt_style=Fusion
qt_qss=qt/style.qss
# コンポジタ SSD:
titlebar_style=2        # 0 modern | 1 classic dots | 2 flat retro
titlebar_active=#000080
titlebar_inactive=#808080
titlebar_frame=#c0c0c0
prefer_ssd=1            # クライアント未指定時にサーバ装飾を勧める
```

`chrome=taskbar` はメニューバーを画面**下**に固定し（Windows XP / 95）、
フローティング Dock を隠してウィンドウ一覧をタスクバーに載せる。
CSS だけでは Wayland 上のメニューバー位置は変わらないため、これらのキーで
レイヤー表面のアンカーを付け替える。

スキン切替時は GTK テーマを `~/.local/share/themes/` にリンクし、
`settings.ini` / `GTK_THEME` / `QT_STYLE_OVERRIDE` / `LUNA_QT_QSS` を更新、
SSD 色をコンポジタへ送る。トレイやスタートメニューは chrome の辺（下タスクバー
なら上向き、上メニューバーなら下向き）に合わせて開く。

配置先は `~/.local/share/luna-desktop/skins/`、
`/usr/local/share/luna-desktop/skins/`、`/usr/share/luna-desktop/skins/`、
または `LUNA_SKIN_PATH` で指定したディレクトリ。CSS と chrome 配置は即時反映。
カスタム HTML レイアウトは次回ログイン時に読み込まれ、ネイティブ操作との接続を
保つため `ui/luna-shell.html` の要素 ID を維持する必要がある。
`--skin NAME` または `LUNA_DESKTOP_SKIN=NAME` でも選択できる。

Wayland プロトコルは **内部バス** として使う。Weston/Mutter/Xorg は不要。GTK4 は Vespera コンポジタへ直接接続する。

### 開発マシンで試す

```bash
cd vespera
make desktop              # DRI + luna-shell（GPU コンソール）
make desktop-soft         # software バックエンド（VM 向け）

# GTK アプリも一緒に起動
LUNA_APPS="target/release/hello-gtk" make desktop
```

### 本番インストール（tty1 でデスクトップ起動）

```bash
sudo make install PREFIX=/usr/local
sudo systemctl enable luna-desktop.service
sudo systemctl start luna-desktop.service
```

`getty@tty1` と競合するため、コンソール自動ログイン運用向け。既存の Xorg/Wayland セッションと併用する場合は `luna-session` を手動実行する。

DRIセッションは現在、rootで実Linux VTから起動する。systemd unitは
`LUNA_TTY=/dev/tty1` を設定し、終了時に元のCRTC・KD・VT状態を復元する。
手動テストではSSHを確保した上で、起動元を明示する:

```bash
sudo LUNA_TTY=/dev/tty2 ./luna-session
```

物理キーボードとマウスはevdevから取得する。`Ctrl+Alt+F1`〜`F12` で
別VTへ切り替わり、復帰時にDRM masterと入力を再取得する。入力を無効化する
場合は `LUNA_PHYSICAL_INPUT=0` を指定する。非root実行には将来
libseat/logindによるデバイス権限委譲が必要であり、`input`/`video` グループを
広く付与する運用は推奨しない。

画面生成に失敗するGPUでは、起動前からsoftware EGLを指定できる:

```bash
LUNA_EGL_SOFTWARE=1 LUNA_USE_SYSTEM_WAYLAND=1 ./luna-session
```

緊急時はSSHから `systemctl stop luna-desktop` を実行する。SysRqが有効なら
`Alt+SysRq+r`、`Alt+SysRq+k` も利用できる。SIGKILLやカーネル停止では
プロセス内の復元処理を実行できないため、通常はTERMで停止する。

## ディレクトリ構成

```
vespera/
├── rust-toolchain.toml              # nightly (naked_functions のため)
├── Cargo.toml                       # ワークスペース
├── Makefile                         # make run / make server / make demo / make webgl
├── run-gtk                          # WebGL ブラウザ表示スクリプト
│
├── wayland-client-rs/               # pure Rust libwayland-client 実装 (GTK にリンク)
│   └── src/
│       ├── lib.rs                   # #[unsafe(naked)] トランポリン
│       ├── types.rs                 # wl_argument / wl_array / wl_list 等 C 互換型
│       ├── interfaces.rs            # wl_display_interface 等 全静的データ
│       ├── wire.rs                  # Wayland ワイヤプロトコル encode/decode
│       ├── socket.rs                # Unix ソケット + SCM_RIGHTS fd 受け渡し
│       ├── proxy.rs                 # WlProxy 構造体
│       ├── display.rs               # WlDisplay (イベントキュー・roundtrip)
│       └── ffi.rs                   # wl_display_*/wl_proxy_*/wl_list_* C 関数
│
├── wayland-server-rs/               # pure Rust Wayland コンポジタ (libwayland-server 不使用)
│   └── src/
│       ├── lib.rs                   # run() エントリ・モジュール公開
│       ├── bin/server.rs            # luna-compositor バイナリ (引数解釈)
│       ├── types.rs                 # 所有型 Arg (C union を使わない安全な引数表現)
│       ├── wire.rs                  # ワイヤ decode(リクエスト)/encode(イベント)
│       ├── protocol.rs              # 全インターフェースのシグネチャ表 (純 Rust 定数)
│       ├── socket.rs                # listen/accept + SCM_RIGHTS fd 受信
│       ├── shm.rs                   # wl_shm プール mmap・ピクセル読み出し
│       ├── object.rs                # Object/Role/Surface (= wl_resource 相当)
│       ├── server.rs                # epoll ループ・global 管理・リクエスト処理・合成
│       └── render/
│           ├── mod.rs               # Backend trait + Framebuffer + アルファ合成
│           ├── software.rs          # CPU 合成 → PPM / Linux fbdev
│           ├── dri.rs               # DRM/KMS dumb buffer (feature = "dri", 生 ioctl)
│           └── webgl_server.rs      # WebSocket 経由でブラウザ WebGL へ配信 (feature = "webgl")
│
└── hello-gtk/                       # サンプル GTK4 アプリ
```

## クイックスタート

### GTK4 アプリをブラウザで表示する (WebGL モード)

```bash
cd vespera

# hello-gtk をブラウザに表示 (ビルドも行う)
./run-gtk

# 任意の GTK4 アプリを表示
./run-gtk gtk4-demo
./run-gtk /usr/bin/some-gtk-app

# ポート変更
PORT=9090 ./run-gtk
```

ブラウザで `http://localhost:8081/` を開くと 1280×720 RGBA フレームが WebGL にリアルタイムストリーミングされる。キャンバスをクリックするとマウス・キーボード操作がアプリへ転送される。

Makefile 経由でも起動できる:

```bash
make webgl                    # hello-gtk
make webgl APP=gtk4-demo      # 任意のアプリ
make webgl APP=/usr/bin/foo PORT=9090
```

### コンポジタ + GTK (ソフトウェアレンダリング)

```bash
make demo
# 内部動作:
#   luna-compositor --socket wayland-1 をバックグラウンド起動
#   WAYLAND_DISPLAY=wayland-1 LD_PRELOAD=libwayland-client.so.0 で hello-gtk を接続
#   合成結果は /tmp/luna-compositor.ppm に毎フレーム書き出される
```

### DRI (DRM/KMS) バックエンド

```bash
cargo build -p wayland-server-rs --features dri,gpu
./target/debug/luna-compositor --backend dri  # KMS が使える Linux コンソール上で
```

### Rust 製 libwayland-client のみ差し替えてアプリを起動

```bash
make run
# LD_LIBRARY_PATH + LD_PRELOAD で libwayland_client.so を優先ロードする
```

## ビルド

```bash
# 通常ビルド (software / dri バックエンド)
cargo build

# WebGL バックエンドを含むビルド
cargo build --features webgl

# make 経由
make build          # software
make build-webgl    # webgl
```

## 設計メモ

### wayland-server-rs

- `libwayland-server` に一切依存せず、ワイヤプロトコルとオブジェクト管理を自前実装。互換性は「C ABI」ではなく「Wayland ワイヤプロトコル」レベルで担保する。
- 公開 global: `wl_compositor` / `wl_subcompositor` / `wl_shm` / `wl_seat` / `wl_output` / `wl_data_device_manager` / `xdg_wm_base` / `zwp_linux_dmabuf_v1` (v4)。GTK4 の SHM 経路と dmabuf 経路の両方を満たす。
- dmabuf (GTK4 が優先): `zwp_linux_dmabuf_v1` v4 + feedback を実装。`format_table` を memfd で提供し `main_device` を `/dev/dri/renderD128` から検出。LINEAR modifier の AR24/XR24 のみ広告 → GTK は CPU-mappable な linear dmabuf を確保するので EGL/GBM 無しで mmap して合成できる。
- バックエンドは `software` / `dri` / `webgl_server` を差し替え可能 (`Backend` trait)。

### wayland-client-rs

- `libwayland-client.so.0` という SONAME を持つ cdylib を生成。
- `include/` に libwayland 互換ヘッダをベンダー済み。C アプリは `libwayland-dev` 不要で、`luna-wayland-client.mk` または `wayland-client.pc` 経由でコンパイル／リンクする。
- `wl_proxy_marshal_flags`（可変引数）は x86_64 System V ABI の `#[unsafe(naked)]` アセンブリトランポリンで実装。
- `*const WlInterface` の静的配列には `Spa<N>` ラッパーで `Sync` を実装。
- 79 シンボルを `.dynsym` にエクスポート。

### WebGL バックエンド (feature = "webgl")

- バックグラウンドスレッドが TCP ポートを listen。
- `GET /` → WebGL ビューア HTML を返す。
- `GET /ws` (Upgrade: websocket) → WebSocket で RGBA フレームをストリーミング。
- ブラウザから送られたマウス/キーボードイベントを WebSocket で受信し、Wayland 入力イベントとして GTK アプリへ転送する。
- 入力プロトコル (テキストメッセージ):
  - `m X Y` — マウス移動 (0.0〜1.0 正規化座標)
  - `b BTN P` — ボタン (Linux BTN_* コード, P=1押下/0解放)
  - `a AXIS V` — ホイール (axis=0縦 1横)
  - `k CODE P` — キー (evdev スキャンコード, P=1押下/0解放)
