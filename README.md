**🚀 Luna Desktop**

A **pure Rust** Wayland compositor + custom `libwayland-client` implementation + macOS-style desktop shell (`luna-shell` / `ui/luna-shell.c`). Run GTK4 apps while replacing Xorg or Weston!

[![Sponsor](https://img.shields.io/badge/Sponsor%20this%20project-%E2%9D%A4%EF%B8%8F-white?logo=githubsponsors&logoColor=EA4AAA&labelColor=EA4AAA)](https://github.com/sponsors/yui0)

![Screenshot](ui/sample_01.png)
![Screenshot](ui/sample_02.png)
![Screenshot](screenshot.png)
![Screenshot](luna-shell.png)

---

## 🏷️ Product Names & Layers

| Name              | Actual Binary       | Role |
|-------------------|---------------------|------|
| **Luna Desktop**  | Full session        | The complete desktop environment users see after kernel boot |
| **luna-compositor** | `luna-compositor`  | DRM/KMS Wayland compositor (Xorg/Weston replacement) |
| **luna-shell**    | `ui/luna-shell.c`   | macOS-style menu bar, Dock, Launchpad & widgets (Luna UI engine) |
| **Luna UI**       | `ui/luna-ui.h` HTML/CSS engine | UI toolkit for shell & settings apps |
| **wayland-client-rs** | `libwayland_client.so` | Pure Rust client lib that GTK apps connect to |

**Internal codename:** Vespera
**User-facing name:** **Luna** (short, memorable, like GNOME/KDE)

## 🔄 Boot Flow (After Kernel)

```bash
systemd / init
  └─ luna-session
       ├─ luna-compositor  (--backend dri) ✨
       ├─ luna-shell       (luna-shell --desktop) 🌙
       └─ GTK Apps         (WAYLAND_DISPLAY + LD_PRELOAD=libwayland-client) 📱
```

## 🌙 luna-shell — the Luna Desktop shell

`ui/luna-shell.c` renders a full macOS-style desktop with the Luna UI engine alone:

- Translucent menu bar — crescent-moon Luna menu, clock, network & power status
- Dock with hover magnification, running-app indicators and Trash
- Launchpad app grid with incremental search (Super / F4)
- Control Center — toggles, brightness/volume sliders, CPU/RAM meters
- Desktop widgets — clock plus CPU/memory/disk stats read from /proc
- About This Luna, notification toasts, Shut Down / Restart / Log Out dialogs

Dock & Launchpad apps are overridable via `LUNA_APP_<NAME>` (e.g. `LUNA_APP_TERMINAL=foot`).
The layout (`ui/luna-shell.html` + `ui/luna-shell.css`) is embedded into the binary and can be
replaced with `LUNA_DESKTOP_LAYOUT` / `LUNA_DESKTOP_CSS` or `--layout` / `--css`.

Wayland protocol is used as an **internal bus**. No Weston, Mutter, or Xorg needed. GTK4 connects directly to the Vespera compositor.

## 🛠️ Try It on Your Dev Machine

```bash
cd vespera
make desktop              # 🚀 DRI + luna-shell (GPU console)
make desktop-soft         # 💻 Software backend (great for VMs)

# Launch with GTK apps
LUNA_APPS="target/release/hello-gtk" make desktop
```

## 📦 Production Install (Launch Desktop on tty1)

```bash
sudo make install PREFIX=/usr/local
sudo systemctl enable luna-desktop.service
sudo systemctl start luna-desktop.service
```

Works alongside `getty@tty1` auto-login. For manual testing with existing sessions, just run `luna-session`.

## 📁 Directory Structure

```
vespera/
├── rust-toolchain.toml
├── Cargo.toml
├── Makefile
├── run-gtk                    # 🌐 WebGL browser launcher
│
├── wayland-client-rs/         # Pure Rust libwayland-client
├── wayland-server-rs/         # Pure Rust Wayland server (no libwayland-server!)
└── hello-gtk/                 # Sample GTK4 app
```

## ⚡ Quick Start

### 🌐 **WebGL Mode** – Run GTK4 Apps in Browser

```bash
cd vespera

# Build + show hello-gtk in browser
./run-gtk

# Any GTK4 app
./run-gtk gtk4-demo
./run-gtk /usr/bin/your-gtk-app

# Custom port
PORT=9090 ./run-gtk
```

Open `http://localhost:8081/` → Real-time 1280×720 RGBA streaming via WebGL!
🖱️ Click & type directly in the browser — input goes to the GTK app.

### 💻 **Software Rendering Demo**

```bash
make demo
# luna-compositor runs in background
# GTK app connects via LD_PRELOAD
# Output saved to /tmp/luna-compositor.ppm every frame
```

### 🎮 **DRI / Hardware Backend**

```bash
cargo build -p wayland-server-rs --features dri
./target/debug/luna-compositor --backend dri
```

## 🛠️ Build Commands

```bash
cargo build                    # Software + DRI
cargo build --features webgl   # + WebGL backend

make build
make build-webgl
```

## 🎨 Luna UI CSS Engine (`ui/luna-ui.h`)

Single-header HTML/CSS → OpenGL renderer. Everything on screen is styled by CSS — no immediate-mode drawing.

**Selectors**: type / `.class` / `#id` / `*`, descendant & child (`>`) & sibling (`+`, `~`) combinators, `:hover` `:active` `:focus` `:focus-visible` `:focus-within`, `:first-child` `:last-child` `:nth-child(odd|even|An+B)`, `:not(...)`, `!important`, CSS variables (`var()`), `calc()`.

**Box**: flexbox (wrap, grow/shrink/basis, gaps, auto margins), grid (templates, areas, auto-flow), block flow, `position: static|relative|absolute|fixed|sticky`, `box-sizing`, min/max sizes, `overflow` + styled scrollbars + scroll-snap/smooth-scroll, `z-index`.

**Paint**: per-corner `border-radius`, borders, `linear-gradient` / `radial-gradient` (multi-stop), **multi-layer `box-shadow` with `inset` and spread**, `background-image: url()`, `opacity`, `transform: translate/scale` (px/%), `transition`, `@keyframes` animations.

**Text**: `font-size/weight`, `line-height`, `text-align`, `white-space`, `text-overflow: ellipsis`, `overflow-wrap`, **`letter-spacing`, `text-transform`, `text-decoration` (underline/line-through), `text-shadow`**, units `px` / `%` / `rem` / `em` / `pt`.

**Fast**: batched glyph rendering (one draw call per line), SDF shaders for rounded rects & Gaussian shadows with early-discard, dirty-flag relayout (layout only on change), viewport culling, cached z-order.

## 🧠 Design Highlights

### **wayland-server-rs** ✨
- Zero dependency on `libwayland-server`
- Full wire protocol compatibility
- Supports: `wl_compositor`, `wl_shm`, `wl_seat`, `xdg_wm_base`, `zwp_linux_dmabuf_v1` (v4)
- dmabuf path works with GTK4 using linear modifiers → easy CPU mapping

### **wayland-client-rs** 🔧
- Produces `libwayland_client.so.0` with proper SONAME
- Uses `#[unsafe(naked)]` assembly trampoline for `wl_proxy_marshal_flags`
- 79 symbols exported

### **WebGL Backend** 🌍 (feature = "webgl")
- Background TCP server
- Serves WebGL viewer on `/`
- WebSocket streaming of RGBA frames
- Real-time mouse/keyboard input forwarding:
  - `m X Y` → mouse move
  - `b BTN P` → button press/release
  - `k CODE P` → key press/release

---

**Ready to build the future of Linux desktops in pure Rust?** 🦀✨

Let’s make Lu Desktop the snappiest, most hackable desktop environment yet! 🚀
