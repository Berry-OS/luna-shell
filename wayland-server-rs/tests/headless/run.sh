#!/bin/bash
# Compose a scene on the GPU of a card that is NOT driving a display, and
# measure the result.  No DRM master, no VT, no input devices: safe to run
# beside a live desktop (it uses a private XDG_RUNTIME_DIR).
#
#   tests/headless/run.sh [/dev/dri/card0]          # IM popup over a window, GPU
#   HEADLESS_GPU= tests/headless/run.sh             # same scene, CPU composition
#   WHIZ=1 tests/headless/run.sh                    # real whiz-im-wayland as the IME
#   cargo build --release -p wayland-server-rs --features dri,gpu   # first
#
# Prints how many pixels of the "window" (blue) and "popup" (red) colours reach
# the screenshot, and which composition path each frame took.  Results are left
# in $OUT (default /tmp/luna-headless): screenshot.ppm, compositor.log.
set -u
WHIZ_PID=; SCENE=
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd -- "$HERE/../.." && pwd)"
BIN="${LUNA_COMPOSITOR:-$ROOT/../target/release/luna-compositor}"
OUT="${OUT:-/tmp/luna-headless}"
GPU="${HEADLESS_GPU-${1:-/dev/dri/card0}}"
RT="$OUT/rt"; mkdir -p -m 700 "$RT"; rm -f "$RT"/test-1* "$OUT/scene.done"
env -u LIBGL_ALWAYS_SOFTWARE -u MESA_LOADER_DRIVER_OVERRIDE -u GALLIUM_DRIVER \
  XDG_RUNTIME_DIR="$RT" XDG_STATE_HOME="$OUT/state" LUNA_HEADLESS_GPU="$GPU" \
  LUNA_FRAME_TRACE=1 LUNA_FRAME_TRACE_PLANES=1 LUNA_TEST_INPUT=1 \
  timeout 90 "$BIN" --socket test-1 --backend software --screenshot "$OUT/screenshot.ppm" \
  --no-input --size 1280x800 >"$OUT/compositor.log" 2>&1 &
COMP=$!
trap 'kill $COMP $WHIZ_PID $SCENE 2>/dev/null' EXIT
sleep 3
if [ -n "${WHIZ:-}" ]; then
  env -u LIBGL_ALWAYS_SOFTWARE XDG_RUNTIME_DIR="$RT" WAYLAND_DISPLAY=test-1 LUNA_CLIENT_RENDERER=gpu \
    timeout 80 whiz-im-wayland >"$OUT/whiz.log" 2>&1 &
  WHIZ_PID=$!; sleep 2
fi
XDG_RUNTIME_DIR="$RT" WAYLAND_DISPLAY=test-1 HEADLESS_DONE="$OUT/scene.done" HEADLESS_NO_IM="${WHIZ:-}" \
  timeout 80 python3 -u "$HERE/scene.py" >"$OUT/scene.out" 2>&1 &
SCENE=$!
for _ in $(seq 1 100); do [ -f "$OUT/scene.done" ] && break; sleep 0.3; done
if [ -n "${WHIZ:-}" ]; then   # type "a" + Space into the IME
  python3 - "$RT" <<'P'
import socket,sys,time
def cmd(c):
    s=socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]+'/luna-shell/luna-shell.sock'); s.send((c+'\n').encode()); s.close(); time.sleep(0.15)
for code in (30,57):
    cmd('inject_key %d 1'%code); cmd('inject_key %d 0'%code)
P
fi
sleep 3
cp "$OUT/screenshot.ppm" "$OUT/final.ppm"; cat "$OUT/scene.out"
python3 - "$OUT/final.ppm" <<'P'
import sys
d=open(sys.argv[1],'rb').read(); parts=d.split(b'\n',3); px=parts[3]
def cnt(rgb): return sum(1 for i in range(0,len(px),3) if tuple(px[i:i+3])==rgb)
print('window px (blue):',cnt((0x20,0x30,0xc0)),'  popup px (red):',cnt((0xe0,0x20,0x20)))
P
echo "frames:"; grep -a "luna-frame\] path" "$OUT/compositor.log" | awk '{print $2}' | sort | uniq -c
