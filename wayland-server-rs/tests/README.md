# GBM/EGL hardware regression probe

The display-independent window-management regression test is
`python3 tests/wm_protocol.py ../target/debug/luna-compositor` after
`cargo build -p wayland-server-rs --features dri,gpu` from `apps/luna-shell`.
It exercises restart restoration, popup reposition, minimize focus, session
takeover, and socket ownership using a private software compositor. See
`../../docs/wm-validation.md` for separate-VT testing beside Wayback and the
limits of hardware verification.

This probe opens the selected DRM device without acquiring DRM master or
changing a CRTC. It checks GPU initialization, advertised DMA-BUF import
formats, 120 GBM rendering/buffer-retirement cycles, ADDFB2 registration,
and exact pixel readback through explicit and implicit DMA-BUF imports.

From `wayland-server-rs`:

```sh
cc -Wall -Wextra tests/gpu_init.c src/render/gpu_shim.c \
  -o /tmp/luna-gpu-init -lgbm -lEGL -lGLESv2
env -u LIBGL_ALWAYS_SOFTWARE -u MESA_LOADER_DRIVER_OVERRIDE -u GALLIUM_DRIVER \
  /tmp/luna-gpu-init /dev/dri/card1
```

Choose the card used by the compositor. Radeon Tahiti with Mesa 26.1.8
reproduces both regressions: EGL's first matching config has a 10-bit native
visual incompatible with the XRGB8888 GBM surface, and the modifier query
succeeds with zero results despite working legacy DMA-BUF import. Explicit
LINEAR import is possible, but Chromium also needs the implicit modifier
entry to use legacy GBM allocation. Advertising LINEAR alone makes its
SharedImage allocations fail and eventually disables WebGL.

For browser integration verification, use a separate Wayland socket and the
same GPU without KMS scanout. Check that GPU compositing and WebGL are
`enabled`, the GPU process crash count remains zero, WebGL 1/2 read back the
expected pixels, and animated canvas rendering retains its context. Actual
KMS presentation and visible output still need a separate session check.

Chrome's `--enable-features=Vulkan` can independently fail on its Wayland
backend; this is visible in Chrome's `wayland_surface_factory.cc` error log.
Do not force this feature in the browser launcher to validate the ordinary
hardware-accelerated OpenGL path.

The compositor output must preserve GBM fourcc, pitch, offset and modifier
through the Rust/C boundary and ADDFB2. When DRM_CAP_ADDFB2_MODIFIERS is zero,
allocate the compositor's scanout buffers with GBM_BO_USE_LINEAR; opaque
legacy tile layouts must not be confused with linear scanout. This keeps
client rendering and composition on the GPU. If an SSD requires CPU
composition, read GPU-only client textures through a GL framebuffer rather
than dropping them or interpreting tiled bytes as scanlines.

On the affected Tahiti device, the revised scanout buffer has linear-aligned
metadata (array mode 1), can be mapped as linear pixels, and matches the GPU
rendered test pattern exactly. A separate 10-second VT test exercised actual
GPU composition and DRM page flips at 1920x1200; the user confirmed the four
color quadrants, grid and moving line displayed without corruption. This
validates that scanout path, not a full desktop session or its performance.

## wl_shm incremental upload (`gpu_shm.c`)

Same no-master rules as above, so it runs on a card with no connector (or
beside a live desktop).  It checks that a surface's texture is updated only
over the rows its damage names, survives a resize, and that 50 distinct
surfaces stay cached:

```sh
cc -Wall -Wextra tests/gpu_shm.c src/render/gpu_shim.c -o /tmp/luna-gpu-shm \
  -lgbm -lEGL -lGLESv2
env -u LIBGL_ALWAYS_SOFTWARE /tmp/luna-gpu-shm /dev/dri/card0
```

`LUNA_SHM_FULL_UPLOAD=1` makes the compositor upload whole buffers again, for
clients that under-report damage.

## Server-side chrome as GPU planes (`gpu_chrome.c`)

SSD titlebars, frames and focus rings are drawn once by the CPU into small
cached ARGB strips and composed by the GPU like any other plane, so a window
with server-side decorations no longer pushes the whole frame onto the CPU
path.  Same no-master rules as above:

```sh
cc -Wall -Wextra tests/gpu_chrome.c src/render/gpu_shim.c -o /tmp/luna-gpu-chrome \
  -lgbm -lEGL -lGLESv2
env -u LIBGL_ALWAYS_SOFTWARE /tmp/luna-gpu-chrome /dev/dri/card0
```

`LUNA_FRAME_TRACE=1` prints `path=gpu` / `path=sw(...)` per frame; software
composition is now only a fallback for a GPU render that fails.

## Headless GPU composition (`tests/headless/`)

The compositor can compose on the GPU of a card that is **not** driving a
display and write the result as a screenshot.  No DRM master, no VT, no input
devices and a private `XDG_RUNTIME_DIR`, so it is safe beside a live desktop
(`card0` on the development machine has no connector).  Real GL clients get
dma-bufs from that card's render node, exactly as on the DRM backend.

```sh
cargo build --release -p wayland-server-rs --features dri,gpu
tests/headless/run.sh                    # IM popup over a window, composed on card0
HEADLESS_GPU= tests/headless/run.sh      # same scene, CPU composition (compare)
WHIZ=1 tests/headless/run.sh             # real whiz-im-wayland as the IME
```

By hand: `luna-compositor --socket test-1 --backend software --headless-gpu
/dev/dri/card0 --screenshot out.ppm --no-input` (or `LUNA_HEADLESS_GPU=CARD`).
The environment's `LIBGL_ALWAYS_SOFTWARE` & co. are cleared for you; Mesa's GBM
swap crashes with them set.

Aids that only exist for tests:

- `LUNA_FRAME_TRACE=1` prints the path of every frame (`gpu`, `sw(...)`) and
  `LUNA_FRAME_TRACE_PLANES=1` additionally lists every GPU plane (position,
  size, format, dma-buf/modifier, serial and how many pixels are visible).
- `LUNA_TEST_INPUT=1` enables the shell command `inject_key CODE 1|0`, which
  feeds an evdev key through the keyboard path (drive an IME headlessly).
- `LUNA_SHM_FULL_UPLOAD=1`, `LUNA_DISABLE_DMABUF=1` switch the GPU path to
  whole-buffer uploads / wl_shm-only clients when bisecting.

`scene.py` speaks the raw Wayland wire protocol (no libraries): an xdg_toplevel
with a text-input and, unless `HEADLESS_NO_IM=1`, a fake input method that maps
a popup.  Add scenes next to it for other regressions.
