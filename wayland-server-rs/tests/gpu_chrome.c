/* Server-side window chrome as GPU planes: small CPU-owned ARGB strips stacked
 * over a window plane.  Never modesets or takes DRM master, so it runs on any
 * card (including one with no connector) while a desktop is up.
 * cc -Wall -Wextra tests/gpu_chrome.c src/render/gpu_shim.c -o /tmp/luna-gpu-chrome \
 *    -lgbm -lEGL -lGLESv2
 * /tmp/luna-gpu-chrome /dev/dri/cardN
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <gbm.h>

#define W 320u
#define H 200u
struct output { void *bo; uint32_t handle, stride, fourcc, offset; uint64_t modifier; };
struct plane { int fd; const void *pixels; uint64_t dev,ino,serial,modifier; uint32_t width,height,stride,offset,fourcc; int32_t x,y; uint64_t key; uint32_t dirty_y0,dirty_y1; };
void *luna_gpu_create(int, uint32_t, uint32_t);
int luna_gpu_render_planes(void *, const struct plane *, uint32_t, struct output *);
int luna_gpu_read_plane(void *, const struct plane *, uint32_t *);
void luna_gpu_commit(void *);
void luna_gpu_destroy(void *);

static void *gpu; static uint32_t *shot;
static int render(const struct plane *p, uint32_t n) {
  struct output out = {0};
  if (!luna_gpu_render_planes(gpu, p, n, &out)) { fprintf(stderr, "render_planes failed\n"); return 0; }
  int dma = gbm_bo_get_fd(out.bo), ok = 0;
  struct stat st = {0};
  if (dma >= 0 && fstat(dma, &st) == 0) {
    struct plane rp = {.fd=dma,.dev=st.st_dev,.ino=st.st_ino,.width=W,.height=H,
      .stride=out.stride,.offset=out.offset,.fourcc=out.fourcc,.modifier=out.modifier};
    ok = luna_gpu_read_plane(gpu, &rp, shot);
  }
  if (dma >= 0) close(dma);
  luna_gpu_commit(gpu);
  return ok;
}
static int px(uint32_t x, uint32_t y, uint32_t want, const char *what) {
  uint32_t got = shot[y*W+x] & 0xffffff;
  if (got != want) { fprintf(stderr, "%s: %u,%u is %06x, want %06x\n", what, x, y, got, want); return 0; }
  return 1;
}
static void fill(uint32_t *p, uint32_t n, uint32_t c) { for (uint32_t i = 0; i < n; i++) p[i] = c; }

int main(int argc, char **argv) {
  if (argc != 2) { fprintf(stderr, "usage: %s /dev/dri/cardN\n", argv[0]); return 2; }
  int fd = open(argv[1], O_RDWR | O_CLOEXEC);
  if (fd < 0) { perror("open"); return 1; }
  gpu = luna_gpu_create(fd, W, H);
  if (!gpu) { fprintf(stderr, "GPU initialization failed\n"); return 1; }
  shot = malloc(W*H*4);
  uint32_t *bg = malloc(W*H*4), *win = malloc(100*60*4), *bar = malloc(100*16*4), *edge = malloc(3*76*4);
  fill(bg, W*H, 0xff202020u); fill(win, 100*60, 0xff00aa00u);
  fill(bar, 100*16, 0xffcc0000u); fill(edge, 3*76, 0xff0000ccu);
  /* A strip with transparent pixels must let the window show through. */
  for (uint32_t i = 0; i < 100*16; i += 2) bar[i] = 0;
  struct plane planes[4] = {
    {.fd=-1,.pixels=bg,.width=W,.height=H,.stride=W*4,.fourcc=0x34325258,.serial=1},
    {.fd=-1,.pixels=win,.width=100,.height=60,.stride=100*4,.fourcc=0x34325241,.serial=2,.x=20,.y=40},
    {.fd=-1,.pixels=bar,.width=100,.height=16,.stride=100*4,.fourcc=0x34325241,.serial=3,.x=20,.y=24},
    {.fd=-1,.pixels=edge,.width=3,.height=76,.stride=3*4,.fourcc=0x34325241,.serial=4,.x=17,.y=24},
  };
  int ok = render(planes, 4);
  ok = ok && px(5, 5, 0x202020, "background") && px(50, 60, 0x00aa00, "window")
          && px(21, 30, 0xcc0000, "titlebar opaque px") && px(20, 30, 0x202020, "titlebar transparent px shows background")
          && px(18, 60, 0x0000cc, "left edge");
  /* Chrome redrawn into the same storage with a new serial must re-upload. */
  fill(bar, 100*16, 0xff7700ffu);
  planes[2].serial = 5;
  ok = ok && render(planes, 4) && px(21, 30, 0x7700ff, "titlebar after redraw") && px(20, 30, 0x7700ff, "titlebar fully repainted");
  /* Unchanged serial: texture is reused, not re-read. */
  fill(bar, 100*16, 0xff112233u);
  ok = ok && render(planes, 4) && px(21, 30, 0x7700ff, "unchanged serial keeps cached texture");
  /* Chrome vanishing (no strip planes) leaves only window + background. */
  ok = ok && render(planes, 2) && px(21, 30, 0x202020, "no chrome") && px(50, 60, 0x00aa00, "window still there");
  printf("SSD chrome as GPU planes (alpha, redraw, cache, removal): %s\n", ok ? "PASS" : "FAIL");
  luna_gpu_destroy(gpu); close(fd);
  return !ok;
}
