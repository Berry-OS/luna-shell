/* Incremental wl_shm upload check; never modesets or takes DRM master, so it
 * runs on any card (including one with no connector) while a desktop is up.
 * cc -Wall -Wextra tests/gpu_shm.c src/render/gpu_shim.c -o /tmp/luna-gpu-shm \
 *    -lgbm -lEGL -lGLESv2
 * /tmp/luna-gpu-shm /dev/dri/cardN
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <gbm.h>

#define W 256u
#define H 128u
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
static int same(const uint32_t *want, uint32_t y0, uint32_t y1, const char *what) {
  for (uint32_t y = y0; y < y1; y++) for (uint32_t x = 0; x < W; x++)
    if ((shot[y*W+x] & 0xffffff) != (want[y*W+x] & 0xffffff)) {
      fprintf(stderr, "%s: mismatch at %u,%u: %06x != %06x\n", what, x, y, shot[y*W+x]&0xffffff, want[y*W+x]&0xffffff);
      return 0;
    }
  return 1;
}

int main(int argc, char **argv) {
  if (argc != 2) { fprintf(stderr, "usage: %s /dev/dri/cardN\n", argv[0]); return 2; }
  int fd = open(argv[1], O_RDWR | O_CLOEXEC);
  if (fd < 0) { perror("open"); return 1; }
  gpu = luna_gpu_create(fd, W, H);
  if (!gpu) { fprintf(stderr, "GPU initialization failed\n"); return 1; }
  uint32_t *a = malloc(W*H*4), *b = malloc(W*H*4); shot = malloc(W*H*4);
  for (uint32_t i = 0; i < W*H; i++) a[i] = 0xff000000u | ((i*7) & 0xffffff);
  struct plane p = {.fd=-1,.pixels=a,.width=W,.height=H,.stride=W*4,.fourcc=0x34325241,.key=42,.serial=1,.dirty_y0=0,.dirty_y1=H};
  int ok = render(&p, 1) && same(a, 0, H, "initial full upload");

  /* New buffer, honest damage rows 40..50: only that band may change. */
  for (uint32_t i = 0; ok && i < W*H; i++) b[i] = a[i];
  for (uint32_t y = 40; y < 50; y++) for (uint32_t x = 0; x < W; x++) b[y*W+x] = 0xff00ff00u;
  p.pixels = b; p.serial = 2; p.dirty_y0 = 40; p.dirty_y1 = 50;
  ok = ok && render(&p, 1) && same(b, 0, H, "incremental band");

  /* Dishonest content outside the band must NOT be uploaded: proves the band
   * path really is partial and that the texture is keyed per surface. */
  for (uint32_t x = 0; x < W; x++) { b[100*W+x] = 0xffff0000u; b[60*W+x] = 0xff0000ffu; }
  p.serial = 3; p.dirty_y0 = 60; p.dirty_y1 = 61;
  ok = ok && render(&p, 1) && same(b, 60, 61, "second band");
  for (uint32_t x = 0; ok && x < W; x++)
    if ((shot[100*W+x] & 0xffffff) == 0xff0000) { fprintf(stderr, "row outside damage was uploaded\n"); ok = 0; }

  /* Same serial: nothing uploaded even if memory changed. */
  b[10*W] = 0xffffffffu; p.dirty_y0 = 0; p.dirty_y1 = H;
  ok = ok && render(&p, 1) && (shot[10*W] & 0xffffff) != 0xffffff;

  /* Resize rebuilds the texture with a full upload. */
  p.width = W/2; p.stride = W/2*4; p.serial = 4; p.dirty_y0 = 0; p.dirty_y1 = 1;
  uint32_t *c = malloc(W*H*4/2);
  for (uint32_t i = 0; i < W/2*H; i++) c[i] = 0xff123456u;
  p.pixels = c;
  ok = ok && render(&p, 1) && (shot[(H-1)*W + 5] & 0xffffff) == 0x123456;

  /* Churn: far more distinct keyed surfaces than the old 16-slot cache. */
  for (unsigned round = 0; ok && round < 3; round++)
    for (uint64_t k = 100; ok && k < 150; k++) {
      struct plane q = {.fd=-1,.pixels=a,.width=W,.height=H,.stride=W*4,.fourcc=0x34325241,
        .key=k,.serial=round?9:1,.dirty_y0=0,.dirty_y1=H};
      ok = render(&q, 1);
    }
  printf("incremental wl_shm upload, resize, 50-surface churn: %s\n", ok ? "PASS" : "FAIL");
  luna_gpu_destroy(gpu); close(fd);
  return !ok;
}
