/* A dma-buf plane (an IM candidate window rendered by another client) stacked
 * over pixel planes (window, SSD chrome strips): it must appear, and keep
 * appearing across many frames and after idle spells.  No DRM master needed.
 * cc -Wall -Wextra tests/gpu_popup.c src/render/gpu_shim.c -o /tmp/luna-gpu-popup \
 *    -lgbm -lEGL -lGLESv2
 * /tmp/luna-gpu-popup /dev/dri/cardN
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
/* Render, read the result back, and (optionally) keep a dma-buf of it. */
static int render(const struct plane *p, uint32_t n, struct plane *keep) {
  struct output out = {0};
  if (!luna_gpu_render_planes(gpu, p, n, &out)) { fprintf(stderr, "render_planes failed\n"); return 0; }
  int dma = gbm_bo_get_fd(out.bo), ok = 0;
  struct stat st = {0};
  if (dma >= 0 && fstat(dma, &st) == 0) {
    struct plane rp = {.fd=dma,.dev=st.st_dev,.ino=st.st_ino,.width=W,.height=H,
      .stride=out.stride,.offset=out.offset,.fourcc=out.fourcc,.modifier=out.modifier};
    ok = luna_gpu_read_plane(gpu, &rp, shot);
    if (keep && ok) { *keep = rp; keep->fd = dup(dma); }
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
  uint32_t *bg = malloc(W*H*4), *win = malloc(200*120*4), *bar = malloc(200*16*4);
  fill(bg, W*H, 0xff202020u); fill(win, 200*120, 0xff00aa00u); fill(bar, 200*16, 0xff0000ccu);
  struct plane pbg = {.fd=-1,.pixels=bg,.width=W,.height=H,.stride=W*4,.fourcc=0x34325258,.serial=1};
  struct plane pwin = {.fd=-1,.pixels=win,.width=200,.height=120,.stride=200*4,.fourcc=0x34325241,.serial=3,.x=10,.y=30};
  struct plane pbar = {.fd=-1,.pixels=bar,.width=200,.height=16,.stride=200*4,.fourcc=0x34325241,.serial=4,.x=10,.y=14};
  /* A "client" dma-buf of its own (not the compositor's recycled output BO):
   * transparent except a red rect, like a candidate window's buffer. */
  struct gbm_device *gd = gbm_create_device(fd);
  struct gbm_bo *bo = gd ? gbm_bo_create(gd, W, H, GBM_FORMAT_ARGB8888, GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING) : NULL;
  if (!bo) { fprintf(stderr, "cannot allocate a client dma-buf\n"); return 1; }
  uint32_t *img = calloc(W*H, 4);
  for (uint32_t y = 150; y < 190; y++) for (uint32_t x = 150; x < 270; x++) img[y*W+x] = 0xffff0000u;
  {
    uint32_t mstride = 0; void *mdata = NULL;
    uint8_t *map = gbm_bo_map(bo, 0, 0, W, H, GBM_BO_TRANSFER_WRITE, &mstride, &mdata);
    if (!map) { fprintf(stderr, "gbm_bo_map failed\n"); return 1; }
    for (uint32_t y = 0; y < H; y++) { for (uint32_t x = 0; x < W; x++) ((uint32_t *)(map + y*mstride))[x] = img[y*W+x]; }
    gbm_bo_unmap(bo, mdata);
  }
  struct stat st = {0};
  int dfd = gbm_bo_get_fd(bo);
  if (dfd < 0 || fstat(dfd, &st) != 0) { fprintf(stderr, "no dma-buf fd\n"); return 1; }
  struct plane dm = {.fd=dfd,.dev=st.st_dev,.ino=st.st_ino,.width=W,.height=H,.stride=gbm_bo_get_stride(bo),
    .offset=0,.fourcc=0x34325241,.modifier=gbm_bo_get_modifier(bo)};
  int ok = 1;

  /* Window + chrome as pixel planes, then the popup (dma-buf) on top. */
  struct plane stack[4] = { pbg, pwin, pbar, dm };
  for (int frame = 0; ok && frame < 5; frame++) {
    ok = render(stack, 4, NULL)
      && px(160, 160, 0xff0000, "popup rect visible over window/chrome")
      && px(50, 60, 0x00aa00, "transparent part of the popup keeps the window visible");
  }
  /* Idle spell (nothing drew the popup for > the texture sweep age), then it returns. */
  for (int i = 0; ok && i < 700; i++) ok = render(stack, 3, NULL);
  ok = ok && px(50, 60, 0x00aa00, "window drawn during idle");
  for (int frame = 0; ok && frame < 3; frame++)
    ok = render(stack, 4, NULL) && px(160, 160, 0xff0000, "popup visible again after idle");
  printf("dma-buf popup over pixel planes (stack, repeat, idle): %s\n", ok ? "PASS" : "FAIL");
  luna_gpu_destroy(gpu); close(fd);
  return !ok;
}
