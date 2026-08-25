<div align="center">

# 🌙 Luna Desktop

### Wayland、Rust、そして HTML/CSS 駆動のネイティブシェルを核に構築された、軽量でハック可能な Linux デスクトップスタック。

Luna は **Rust 製 Wayland コンポジタ**、**Luna UI を搭載したネイティブ C デスクトップシェル**、そしてオプションの **Rust 製 `libwayland-client` 実装** を一つの実験的なデスクトップ環境にまとめたものです。

[English](README.md) · [プロジェクトをスポンサーする](https://github.com/sponsors/yui0)

[![License: MPL-2.0](https://img.shields.io/badge/License-MPL--2.0-blue.svg)](LICENSE)
[![Rust: nightly](https://img.shields.io/badge/Rust-nightly-orange.svg?logo=rust)](rust-toolchain.toml)
[![Wayland](https://img.shields.io/badge/display-Wayland-5b5b5b.svg)](https://wayland.freedesktop.org/)
[![Sponsor](https://img.shields.io/badge/Sponsor-%E2%9D%A4%EF%B8%8F-EA4AAA?logo=githubsponsors&logoColor=white)](https://github.com/sponsors/yui0)

</div>

Luna は理解できるほど小さく、テーマ変更が柔軟で、Wayland ワイヤプロトコルや DRM/KMS 出力からメニューバー、Dock、Launchpad、ウィジェット、アプリケーションテーマまで、Linux デスクトップスタック全体を実験できるほど低レベルに設計されています。

> **プロジェクトの状態:** Luna Desktop は活発に開発中です。開発用デスクトップ/セッションとしてすでに利用可能ですが、直接 DRM/evdev パスはまだ実験的であり、実機ではそのように扱ってください。

<!-- readme-screenshot-gallery:start -->
## ✨ デスクトップスキン

以下の画像はすべて **実際の `luna-shell` フレームバッファキャプチャ** です。シェルは X11/EGL キャプチャバックエンドでビルドされ、Xvfb 上で指定のスキンを使って実行されています。ブラウザのプレビューや再作成したモックアップではありません。通常のデスクトップは Luna 独自の KMS/Wayland ホスト経由で動作し、**GLFW は不要** です。

### Luna — デフォルト

<p align="center">
  <img src="docs/screenshots/luna-shell-skin-default.png" alt="Luna Desktop デフォルトスキン" width="96%">
</p>

| Nocturne Atelier | Windows XP |
| --- | --- |
| ![Nocturne Atelier スキン](docs/screenshots/luna-shell-skin-nocturne-atelier.png) | ![Windows XP スキン](docs/screenshots/luna-shell-skin-windows-xp.png) |

| Windows 95 | Classic Mac OS |
| --- | --- |
| ![Windows 95 スキン](docs/screenshots/luna-shell-skin-windows-95.png) | ![Classic Mac OS スキン](docs/screenshots/luna-shell-skin-classic-mac.png) |

| BeOS | Amiga Workbench |
| --- | --- |
| ![BeOS スキン](docs/screenshots/luna-shell-skin-beos.png) | ![Amiga Workbench スキン](docs/screenshots/luna-shell-skin-amiga-workbench.png) |

ギャラリーを再生成するには:

```bash
./tools/capture-shell-skins.sh
```
<!-- readme-screenshot-gallery:end -->

## 🚀 Luna の特徴は？

| | Luna Desktop |
|---|---|
| **フルスタックを自分で持つ** | Rust コンポジタが Wayland プロトコル、コンポジション、入力、DRM/KMS、オプションのブラウザストリーミングを担当します。 |
| **ネイティブシェル、Web 風スタイリング** | `luna-shell` はネイティブ C プログラムですが、Luna UI はブラウザを埋め込まずに HTML/CSS 風レイアウトからインターフェースを描画します。 |
| **壁紙以上のテーマ** | スキンはシェルのレイアウト、クロームの配置、ウィジェット、GTK スタイリング、Qt スタイリング、サーバーサイド装飾の色まで変更できます。 |
| **複数のレンダリングパス** | DRM/KMS 上で実行したり、開発/VM 用にソフトウェアバックエンドを使ったり、WebGL バックエンド経由で Wayland アプリをブラウザにストリーミングしたりできます。 |
| **Wayland 実装の遊び場** | リポジトリには Rust 製 Wayland サーバーと、プロトコル/ランタイム実験用のオプションの Rust `libwayland-client.so.0` 代替実装の両方が含まれます。 |

## ⚡ クイックスタート

Luna UI サブモジュールを含めてクローン:

```bash
git clone --recurse-submodules https://github.com/Berry-OS/luna-shell.git
cd luna-shell
```

プロジェクトは **Rust nightly** を使用し、ネイティブ C コンポーネントもビルドします。一般的な開発依存関係には GCC、make、pkg-config、Wayland 開発ツール（`wayland-scanner`）、DRM/GBM/EGL/OpenGL、libinput、libudev、xkbcommon、D-Bus、X11、OpenSSL の開発パッケージが含まれます。

### 推奨ビルド

デスクトップシェルは **ディストリビューション提供の `libwayland-client` をデフォルトで使用** します。これは RPM パッケージングでも使われるパスです。

```bash
make build-desktop-system
```

ユーザー/セッションが必要な DRM/入力デバイスへのアクセスを既に持っている場合、ワンコマンドの DRI パスは:

```bash
make desktop-system
```

ソフトウェア志向の開発セッション用:

```bash
make desktop-soft-system
```

直接 DRI セッションは現在、実際の Linux VT と直接の DRM/入力アクセスを想定しています。それが権限の昇格を必要とするマシンでは、まず通常ユーザーとしてビルドし、その後適切な権限で VT から既にビルドされたセッションを起動してください。例:

```bash
sudo LUNA_TTY=/dev/tty2 PROFILE=release ./luna-session
```

物理ハードウェアで新しいコンポジタをテストする際は、SSH などの復旧パスを確保しておいてください。

## 🧩 デスクトップに実際に含まれるもの

```text
luna-session
├── luna-compositor       Rust Wayland コンポジタ / ウィンドウマネージャ
├── luna-shell            ネイティブ C デスクトップシェル + Luna UI
├── luna-clipboard        Wayland クリップボードマネージャ
├── luna-tray-notify      トレイ / 通知ヘルパー
└── applications          GTK / Qt / SDL / その他の Wayland クライアント
```

| コンポーネント | 実装 | 役割 |
|---|---|---|
| **Luna Desktop** | フルセッション | ユーザーに提示される完全なデスクトップ環境。 |
| **`luna-compositor`** | Rust (`wayland-server-rs`) | Wayland サーバー、コンポジション、ウィンドウ管理、DRM/KMS、入力、代替レンダリングバックエンド。 |
| **`luna-shell`** | C + Luna UI | メニューバー/タスクバー、Dock、Launchpad、Control Center、ウィジェット、設定、デスクトップクローム。 |
| **Luna UI** | `ui/luna-ui` サブモジュール | シェルが使用するネイティブ HTML/CSS 風レイアウトと OpenGL レンダリングエンジン。 |
| **`luna-clipboard`** | C | Luna セッション用のクリップボード永続化/管理。 |
| **`luna-tray-notify`** | C | トレイ/通知互換性ヘルパー。 |
| **`wayland-client-rs`** | Rust | 開発と互換性実験用のオプションの libwayland-client 互換実装。 |

内部コードネーム **Vespera** はソース履歴の一部にまだ残っていますが、ユーザー向けデスクトップ名は **Luna** です。

## 🌙 シェル体験

`ui/luna-shell.c` がメインのデスクトップシェルです。Luna UI を使ってデスクトップを描画し、Wayland 経由でコンポジタと通信します。

現在のシェル機能には以下が含まれます:

- 半透明の Luna メニューバー / 代替タスクバーとデスクバーレイアウト
- ホバー拡大、実行中アプリインジケータ、Trash を備えた Dock
- アプリケーション発見、インクリメンタル検索、キーボードショートカットを備えた Launchpad
- Control Center とシステムステータス UI
- Wi-Fi、Ethernet、Bluetooth 連携
- 通知トーストとトレイ/SNI 連携
- デスクトップ時計、システム統計、天気ウィジェット
- モニタ/ディスプレイ連携
- 言語、キーボードレイアウト、NumLock 設定
- 複数レイアウト構成向けのキーボードレイアウト切り替え
- About This Luna、ログアウト、再起動、シャットダウンのフロー
- XDG ベースの設定、データ、状態、キャッシュ、ランタイムパス

アプリケーションはネイティブ Wayland クライアントとして起動されます。`luna-session` は GTK、Qt、Mozilla、SDL 向けの通常のデスクトップ環境変数をエクスポートし、アプリケーションが Luna コンポジタを直接使えるようにします。

### シェルアプリケーションの上書き

Dock と Launchpad のエントリは `LUNA_APP_<NAME>` 変数でリダイレクトできます:

```bash
LUNA_APP_TERMINAL=foot ./luna-session
```

シェルのレイアウトとスタイルシートも開発用に置き換え可能です:

```bash
LUNA_DESKTOP_LAYOUT=/path/to/layout.html \
LUNA_DESKTOP_CSS=/path/to/style.css \
./luna-shell --desktop
```

同等の `--layout` / `--css` コマンドラインオプションも利用できます。

## 🎨 スキンシステム

**System Settings → Appearance → Desktop Skin** を開くと、Luna 実行中にスキンを切り替えられます。選択したスキンは次の場所に保存されます:

```text
~/.config/luna-shell/settings.conf
```

同梱スキンは現在以下を含みます:

- **Luna / Default**
- **Nocturne Atelier**
- **Windows XP**
- **Windows 95**
- **Classic Mac OS**
- **BeOS**
- **Amiga Workbench**

スキンはシェルの色だけに限定されません。クロームの配置、GTK/Qt 連携、コンポジタの装飾設定も記述できます。

```text
my-skin/
├── skin.conf
├── style.css
├── layout.html
├── gtk/<ThemeName>/        # オプションの GTK 3/4 テーマ
│   ├── index.theme
│   ├── gtk-3.0/gtk.css
│   └── gtk-4.0/gtk.css
└── qt/
    ├── style.qss            # オプションの Qt スタイルシート
    └── colors.conf
```

最小限の `skin.conf` は次のようになります:

```ini
name=My Skin
description=A custom Luna desktop skin
css=style.css
layout=layout.html

# Shell chrome
chrome=taskbar          # taskbar | deskbar | menubar | luna
menubar_edge=bottom     # top | bottom
menubar_height=34
dock_mode=hidden        # float | hidden

# Application toolkit themes
gtk_theme=LunaWindows95
qt_style=Fusion
qt_qss=qt/style.qss

# Compositor/server-side decorations
titlebar_style=2        # 0 modern | 1 classic dots | 2 flat retro
titlebar_active=#000080
titlebar_inactive=#808080
titlebar_frame=#c0c0c0
prefer_ssd=1
```

例えば `chrome=taskbar` はシェルストリップをディスプレイの **下部** に移動し、フローティング Dock を非表示にできます。一方 `deskbar` / `menubar` は上部寄りクロームを維持します。これは CSS の位置指定だけではなく、ネイティブ Wayland レイヤーサーフェスのアンカー処理で行われます。

スキンがツールキットのメタデータを提供する場合、Luna はその外観を新しく起動した GTK/Qt アプリケーションに伝播し、コンポジタ側の装飾色も更新できます。

カスタムスキンは次の場所に配置できます:

```text
~/.local/share/luna-desktop/skins/
/usr/local/share/luna-desktop/skins/
/usr/share/luna-desktop/skins/
```

または `LUNA_SKIN_PATH` で指定したパス。

明示的にスキンを選択することも可能です:

```bash
luna-shell --desktop --skin windows-95
# または
LUNA_DESKTOP_SKIN=windows-95 luna-shell --desktop
```

スキン開発では、`skins/<name>/layout.html` をブラウザで開いて HTML/CSS 構造を反復し、同じファイルを Luna で使用してください。カスタムレイアウトは、ネイティブアクションが接続されたままになるよう、`luna-shell` が期待するシェル要素 ID を保持する必要があります。

## 🏗️ アーキテクチャ

```text
Linux kernel
   │
   ├─ DRM/KMS + evdev/libinput
   │
   ▼
┌───────────────────────────────┐
│ luna-compositor               │
│ Rust Wayland server           │
│                               │
│ software / DRI+GPU / WebGL    │
└──────────────┬────────────────┘
               │ Wayland
       ┌───────┼───────────────┐
       │       │               │
       ▼       ▼               ▼
  luna-shell  GTK/Qt apps  luna-clipboard
  C + Luna UI
       │
       └─ HTML/CSS-style shell layouts + skins
```

Wayland は内部デスクトップバスです。通常の Luna セッションでは、Luna とネイティブ Wayland アプリケーションの間に Weston、Mutter、Xorg は必要ありません。

### `wayland-server-rs`

コンポジタは `libwayland-server` をラップするのではなく、Rust で直接 Wayland ワイヤプロトコルとオブジェクトモデルを実装しています。

主な機能:

- `wl_compositor`、`wl_subcompositor`、`wl_shm`、`wl_seat`、`wl_output`、データデバイスサポート
- アプリケーションウィンドウ用の `xdg_wm_base`
- `zwp_linux_dmabuf_v1` v4 / dmabuf フィードバックサポート
- Luna シェル/ウィンドウ管理 IPC
- evdev 入力と VT 処理
- ソフトウェアコンポジション
- DRM/KMS 出力
- GPU アシストレンダリングパス
- WebGL/WebSocket ストリーミングバックエンド

### `wayland-client-rs`

リポジトリには、`libwayland-client.so.0` 互換ライブラリをビルドし、互換クライアントヘッダを同梱する実験的な Rust 実装も含まれます。

システムクライアント実装なしでデスクトップがどこまで行けるかをテストするのに有用ですが、**推奨される Luna シェルビルドには不要** です。

ベンダー提供クライアント実装に対して明示的にシェルをビルド/実行するには:

```bash
make desktop LUNA_WAYLAND_CLIENT=vendored
```

## 🖥️ レンダリングとテストモード

| コマンド | 目的 |
|---|---|
| `make desktop-system` | システム Wayland クライアントライブラリを使用したフル DRI デスクトップ。 |
| `make desktop-soft-system` | システム Wayland クライアントライブラリを使用したソフトウェアバックエンドデスクトップ。 |
| `make desktop LUNA_WAYLAND_CLIENT=vendored` | Rust クライアント実装をテストしながらの DRI デスクトップ。 |
| `make demo` | コンポジタを起動し、サンプル GTK アプリを接続。コンポジタスクリーンショットを `/tmp/luna-compositor.ppm` に書き出します。 |
| `make webgl` | サンプル GTK アプリをコンポジタのブラウザバックエンド経由でストリーミング。 |
| `make webgl APP=gtk4-demo` | 別の GTK アプリケーションを WebGL バックエンド経由で実行。 |
| `make rpm` | ソース tarball と RPM パッケージをビルド。 |

### WebGL モード

```bash
make webgl
# その後 http://localhost:8081/ を開く
```

またはヘルパーを直接使用:

```bash
./run-gtk
./run-gtk gtk4-demo
PORT=9090 ./run-gtk /usr/bin/your-gtk-app
```

ブラウザは WebSocket 経由で RGBA フレームを受信し、マウス、キーボード、ホイール入力をコンポジタに送り返します。

## 📦 ソースからのインストール

推奨されるシステム Wayland クライアントインストールの場合:

```bash
sudo make install-system PREFIX=/usr/local
```

ソースインストールは、専用の tty ベースセッション用に有効化できる `luna-desktop.service` ユニットも提供します:

```bash
sudo systemctl enable luna-desktop.service
sudo systemctl start luna-desktop.service
```

Luna の代替 `libwayland-client` を Luna プレフィックスの一部として意図的にインストールしたい場合は:

```bash
sudo make install PREFIX=/usr/local
```

Berry Linux のパッケージングはディストリビューションの Wayland クライアントライブラリを使用し、オプションのキオスク風 systemd ユニットを有効化するのではなく、ディスプレイマネージャが `luna-session` を起動することを想定しています。

## ⚠️ ネイティブ DRM/VT テストの注意点

DRI バックエンドはネストされたデスクトッププレビューではなく、実際のコンポジタパスです。DRM/KMS、アクティブな VT、物理入力デバイスの制御を取得できます。

- 直接 DRI テストには実際の Linux VT を使用
- `LUNA_TTY=/dev/ttyN` で VT を明示的に選択
- 物理入力が有効な場合、`Ctrl+Alt+F1` … `F12` で VT を切り替え可能
- Luna は通常シャットダウン時に DRM/KD/VT 状態を復元
- `SIGKILL` より通常の TERM/セッションログアウトを優先（殺されたプロセスはクリーンアップパスを実行できないため）
- 必要に応じて `LUNA_PHYSICAL_INPUT=0` で直接物理入力処理を無効化
- 問題のある GPU セットアップでは `LUNA_EGL_SOFTWARE=1` でソフトウェア EGL を強制可能

## 📁 リポジトリマップ

```text
luna-shell/
├── Cargo.toml
├── rust-toolchain.toml
├── Makefile
├── luna-session
├── luna-shell.spec
├── run-gtk
│
├── ui/
│   ├── luna-shell.c              # ネイティブデスクトップシェル
│   ├── luna-ui/                  # Luna UI git サブモジュール
│   ├── luna-wifi.h
│   ├── luna-ethernet.h
│   ├── luna-bluetooth.h
│   ├── luna-weather.h
│   ├── luna-monitor.h
│   └── protocols/
│
├── skins/                        # シェル + GTK + Qt テーマ
├── docs/screenshots/             # 生成された実シェルキャプチャ
├── clipboard/                    # クリップボードヘルパー
├── tray/                         # トレイ / 通知ヘルパー
├── systemd/                      # オプションのデスクトップサービス
├── udev/                         # デバイスルール
├── tools/                        # スクリーンショット/ギャラリーツール
│
├── wayland-server-rs/            # Rust Wayland コンポジタ
├── wayland-client-rs/            # オプションの Rust クライアントライブラリ
└── hello-gtk/                    # サンプル GTK アプリケーション
```

## 🛠️ 便利なビルドターゲット

```bash
cargo build
cargo build -p wayland-server-rs --features dri,gpu
cargo build --features webgl

make build
make build-dri
make build-webgl
make build-shell
make build-desktop-system
make clean
```

## 🤝 貢献

Luna は意図的に実験的です。プロトコル互換性、ハードウェアサポート、シェルの磨き上げ、アクセシビリティ、パフォーマンス、ドキュメント、アプリケーション互換性、スキン品質の向上に寄与する貢献を歓迎します。

シェルスキンを変更する際は、次のコマンドでスクリーンショットギャラリーを再現可能に保ってください:

```bash
./tools/capture-shell-skins.sh
```

## 📄 ライセンス

Luna Desktop は **Mozilla Public License 2.0 (MPL-2.0)** の下でリリースされています。[`LICENSE`](LICENSE) を参照してください。

## ❤️ Luna をサポートする

低レベルの Linux デスクトップ実験、カスタムデスクトップシェル、Wayland 内部、あるいは楽しすぎるレトロスキンを楽しむ方は、[GitHub Sponsors](https://github.com/sponsors/yui0) を通じて継続的な開発を支援できます。

---

<div align="center">

**ワイヤプロトコルからピクセルまで、デスクトップを構築する。** 🦀🌙

</div>
