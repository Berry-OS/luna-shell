# Window management changes and validation

The compositor is in `wayland-server-rs/src`; the desktop shell is in
`ui/luna-shell.c`. The shared UI headers live in `../luna-ui`.

## Reference comparison

Compared with [Wayback's output frame handler](https://github.com/wayback-x11/wayback/blob/main/wayback-compositor/wayback-compositor.c),
which uses wlroots scene composition and sends frame callbacks from the output
frame event. Luna keeps its existing event-driven DRM page-flip pacing and
incremental SHM texture uploads. It now omits the scene prefix hidden by an
opaque fullscreen plane and skips off-screen planes before texture import or
upload. Damage on omitted SHM surfaces survives until they become visible.
This reduces unnecessary work; a measured speedup needs a real GPU session.

Compared session restoration with the upstream
[xdg-session-management XML](https://github.com/wayland-mirror/wayland-protocols/blob/main/staging/xdg-session-management/xdg-session-management-v1.xml).
The first empty commit now starts toplevel configuration, after the client has
supplied its restore request. The restored event precedes the initial
configure, whose dimensions come from the saved visible geometry. Mapping
then restores the origin. State is saved before NULL-buffer unmapping and
on shutdown. Replaced and removed session handles become inert instead of
losing their protocol object IDs. Both xdg and the older xx interface remain
available; the integration probe exercises xdg.

Other changes cover popup reposition acknowledgements, popup placement
relative to the parent's visible geometry, partial input-region subtraction,
and client-requested minimize handing keyboard focus to the previous window.

## Test without switching the desktop

From `apps/luna-shell`:

```sh
cargo test -p wayland-server-rs --features dri,gpu
cargo build -p wayland-server-rs --features dri,gpu
python3 wayland-server-rs/tests/wm_protocol.py target/debug/luna-compositor
```

The protocol test launches software compositors on a private temporary socket
and state directory. It checks socket ownership against a second compositor,
and checks screenshot pixels while changing a cursor without moving the
pointer or repainting the window (including a late first cursor buffer).
The cursor fast path falls back to one scene rebuild if retained content is
unavailable, rather than discarding a requested glyph change.
It also checks
initial configure timing, restoration across compositor restart, position,
size, state surviving unmap, popup reposition order and token, minimize focus,
and session takeover. It does not open input devices or change a VT.

The shared shell styles let the task strip shrink and scroll horizontally
while keeping the clock on-screen. An ordinary mouse wheel over that strip
also scrolls it. The layout test loads the real HTML and styles for seven
skins, at six widths from 480 to 1920, with a long application title and eight
tasks, and checks that the last task can be reached by scrolling:

```sh
cc -O0 -w tools/test-menubar-layout.c -o /tmp/luna-menubar-test -lGL -lm
/tmp/luna-menubar-test
```

## Test beside Wayback on a separate VT

Choose an **unused** VT. For VT 2:

```sh
make build-desktop-system
sudo env LUNA_REBUILD=0 ./luna-session --vt 2
```

`openvt` refuses occupied VTs, switches to the selected VT, waits for Luna,
and returns to the previous VT on exit. This session uses `wayland-luna-2`,
so it does not take Wayback's socket. Use **Ctrl+Alt+F2** to return to Luna;
use Ctrl+Alt+Fn for the VT where Wayback is running. Ctrl+F2 alone is not
intercepted because applications use it. Logout or Ctrl+Alt+Backspace ends Luna.

VT startup saves console display, keyboard, and VT modes, selects graphics
mode, and disables console key interpretation because evdev owns input.
Shutdown restores all saved modes. When Luna releases its VT, it stops
compositing and holds new frame callbacks; acquiring the VT requests a fresh
frame. This prevents its hidden desktop from continuously rendering while
Wayback is active.

Source launches now rebuild incrementally by default. Set `LUNA_REBUILD=0`
when deliberately testing an existing build; `LUNA_BIN` selects prebuilt
binaries. `XDG_CURRENT_DESKTOP` defaults to Luna rather than inheriting the
parent desktop; use `LUNA_CURRENT_DESKTOP` to override it. GTK's default
`GDK_SCALE` is 1, since that setting uses an integer scale.

For timing, start with `LUNA_FRAME_TRACE=1` and compare `path=gpu` versus
`path=sw(...)` and frame durations. Turn tracing off for normal use. Browser
checks need an independent test profile/process attached to Luna's socket;
an already running browser can route a launch back to its existing desktop.

## Limits of this verification

The test environment has no `/dev/dri`, so GPU pixels/performance, DRM master
handoff and visible VT switching were not exercised here. Existing GPU probes
are described in `wayland-server-rs/tests/README.md`. Firefox and Chrome were
not run here; their supported protocol and profile settings must be checked
in the actual session. This change is not a claim of complete Wayland
coverage: surface scale/transform and synchronized subsurface transactions,
among other advertised functionality, still need separate implementation and
integration work.

## X11 apps and XEmbed tray icons (rootless Xwayland)

The compositor starts a rootless `Xwayland` (shm buffers; `LUNA_XWAYLAND_GLAMOR=1`
enables glamor, `LUNA_NO_XWAYLAND=1` disables it) and acts as its window manager
through a small built-in X11 client (`wayland-server-rs/src/xwm.rs`, no libxcb):

- Xwayland only creates surfaces for windows the WM redirected, so the WM
  redirects the root's subwindows (Composite, manual) and matches
  `WL_SURFACE_ID` messages to surfaces.
- A managed X window gets synthetic `xdg_surface`/`xdg_toplevel` objects, so
  stacking, placement, decorations, focus, maximise and the taskbar reuse the
  xdg code.  Configure → `ConfigureWindow`, close → `WM_DELETE_WINDOW`.
  GTK CSD windows (`_GTK_FRAME_EXTENTS` / Motif hints) get no server titlebar.
- Menus, tooltips and other override-redirect windows are popups at their X
  coordinates; before each composite the WM tells X where it put its windows.
- The WM owns `_NET_SYSTEM_TRAY_S0` with an ARGB visual.  Docked icons are
  listed to the shell as `x11:<win>` tray items; the shell reserves a menubar
  slot and reports its rectangle (`x11_tray_pos`), where the compositor draws
  the icon.  GtkStatusIcon apps need the X11 backend: `luna-shell` sets
  `GDK_BACKEND=x11` for the apps in `LUNA_X11_APPS` (default: clipit,
  parcellite, connman-ui-gtk, pasystray, volumeicon, …).
- `luna-session` waits for `$XDG_RUNTIME_DIR/luna-xdisplay-<socket>` and
  exports `DISPLAY`.  `LUNA_XWL_TRACE=1` logs X events.

Headless check (no VT): run the compositor with `--backend software`, start a
client with `DISPLAY=$(cat $XDG_RUNTIME_DIR/luna-xdisplay-<socket>)
GDK_BACKEND=x11 gtk3-demo`.
