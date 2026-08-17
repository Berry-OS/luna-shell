#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/docs/screenshots"
BUILD="$ROOT/.readme-shell-capture"
DISPLAY_NUM="${LUNA_CAPTURE_DISPLAY:-:99}"

SKINS=(
  default
  nocturne-atelier
  windows-xp
  windows-95
  classic-mac
  beos
  amiga-workbench
)

mkdir -p "$OUT" "$BUILD/home"
rm -f "$OUT"/luna-shell-skin-*.png

cleanup() {
  if [[ -n "${XVFB_PID:-}" ]]; then
    kill "$XVFB_PID" 2>/dev/null || true
    wait "$XVFB_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

echo "[capture] building luna-shell itself (X11/EGL backend, no GLFW)"
chmod +x "$ROOT/ui/gen_include.sh"
make -C "$ROOT" luna-shell LUNA_WAYLAND_CLIENT=system

Xvfb "$DISPLAY_NUM" -screen 0 1440x900x24 +extension GLX +render -noreset >"$BUILD/xvfb.log" 2>&1 &
XVFB_PID=$!

for _ in $(seq 1 50); do
  if DISPLAY="$DISPLAY_NUM" xdpyinfo >/dev/null 2>&1; then break; fi
  sleep 0.1
done
DISPLAY="$DISPLAY_NUM" xdpyinfo >/dev/null

for skin in "${SKINS[@]}"; do
  png="$OUT/luna-shell-skin-${skin}.png"
  log="$BUILD/${skin}.log"
  home="$BUILD/home-${skin}"
  rm -rf "$home"
  mkdir -p "$home"

  echo "[capture] luna-shell --skin $skin"
  set +e
  timeout 8s env -u WAYLAND_DISPLAY \
    DISPLAY="$DISPLAY_NUM" \
    HOME="$home" \
    XDG_CONFIG_HOME="$home/.config" \
    XDG_DATA_HOME="$home/.local/share" \
    XDG_STATE_HOME="$home/.local/state" \
    XDG_CACHE_HOME="$home/.cache" \
    LUNA_EGL_SOFTWARE=1 \
    LIBGL_ALWAYS_SOFTWARE=1 \
    "$ROOT/luna-shell" --desktop --skin "$skin" --screenshot "$png" \
    >"$log" 2>&1
  status=$?
  set -e

  if [[ ! -s "$png" ]]; then
    echo "[capture] failed for skin=$skin (status=$status)" >&2
    cat "$log" >&2
    exit 1
  fi
  echo "[capture] wrote $(basename "$png") ($(stat -c%s "$png") bytes)"
done

python3 "$ROOT/tools/update-shell-skin-gallery.py"

echo "[capture] generated shell skin gallery:"
ls -lh "$OUT"/luna-shell-skin-*.png
