/* Hardware regression probe; never modesets or takes DRM master.
 * cc tests/gpu_init.c src/render/gpu_shim.c -o /tmp/luna-gpu-init \
 *    -lgbm -lEGL -lGLESv2
 * /tmp/luna-gpu-init /dev/dri/cardN
 * A driver offering XRGB2101010 before XRGB8888 reproduces the old failure.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <libdrm/drm.h>
#include <gbm.h>

struct output { void *bo; uint32_t handle, stride, fourcc, offset; uint64_t modifier; };
struct plane { int fd; const void *pixels; uint64_t dev,ino,serial,modifier; uint32_t width,height,stride,offset,fourcc; int32_t x,y; uint64_t key; uint32_t dirty_y0,dirty_y1; };
struct format { uint32_t fourcc, pad; uint64_t modifier; };
void *luna_gpu_create(int, uint32_t, uint32_t);
uint32_t luna_gpu_query_dmabuf_formats(void *, struct format *, uint32_t);
int luna_gpu_render(void *, const uint32_t *, uint32_t, uint32_t, struct output *);
int luna_gpu_read_plane(void *, const struct plane *, uint32_t *);
void luna_gpu_commit(void *);
void luna_gpu_destroy(void *);

int main(int argc, char **argv) {
  if (argc != 2) { fprintf(stderr, "usage: %s /dev/dri/cardN\n", argv[0]); return 2; }
  int fd = open(argv[1], O_RDWR | O_CLOEXEC);
  if (fd < 0) { perror("open DRM device"); return 1; }
  void *gpu = luna_gpu_create(fd, 640, 480);
  if (!gpu) { fprintf(stderr, "GPU initialization failed\n"); close(fd); return 1; }
  uint32_t formats = luna_gpu_query_dmabuf_formats(gpu, NULL, 0);
  uint32_t *pixels = calloc(640 * 480, sizeof(*pixels));
  uint32_t *readback = calloc(640 * 480, sizeof(*readback));
  int ok = pixels != NULL && readback != NULL && formats > 0;
  for (unsigned frame = 0; ok && frame < 120; frame++) {
    for (unsigned i = 0; i < 640 * 480; i++) pixels[i] = 0xff000000u | (frame << 16) | ((i / 640 & 255) << 8) | (i % 640 & 255);
    struct output out = {0};
    ok = luna_gpu_render(gpu, pixels, 640, 480, &out) && out.bo && out.stride >= 640 * 4;
    if (ok) {
      struct drm_mode_fb_cmd2 fb = {0};
      fb.width=640;fb.height=480;fb.pixel_format=out.fourcc;
      fb.handles[0]=out.handle;fb.pitches[0]=out.stride;fb.offsets[0]=out.offset;
      if(out.modifier!=0 && out.modifier!=0x00ffffffffffffffull){
        fb.flags=DRM_MODE_FB_MODIFIERS;fb.modifier[0]=out.modifier;
      }
      ok=ioctl(fd,DRM_IOCTL_MODE_ADDFB2,&fb)==0;
      if(ok)ioctl(fd,DRM_IOCTL_MODE_RMFB,&fb.fb_id);
      else perror("ADDFB2");
      if(ok && frame%30==0){
        int dma_fd=gbm_bo_get_fd(out.bo);
        struct stat st={0};
        if(dma_fd<0 || fstat(dma_fd,&st)!=0)ok=0;
        else {
          struct plane plane={.fd=dma_fd,.dev=st.st_dev,.ino=st.st_ino,
            .width=640,.height=480,.stride=out.stride,.offset=out.offset,
            .fourcc=out.fourcc,.modifier=out.modifier};
          for(unsigned mode=0;ok && mode<2;mode++){
            if(mode==1)plane.modifier=0x00ffffffffffffffull;
            ok=luna_gpu_read_plane(gpu,&plane,readback);
            for(unsigned i=0;ok && i<640*480;i++)
              if((readback[i]&0xffffff)!=(pixels[i]&0xffffff)){
                fprintf(stderr,"pixel mismatch frame=%u index=%u: %08x != %08x\n",frame,i,readback[i],pixels[i]);ok=0;
              }
          }
        }
        if(dma_fd>=0)close(dma_fd);
      }
      luna_gpu_commit(gpu);
    }
  }
  printf("dmabuf format/modifier pairs: %u; 120 GBM/KMS frames and DMA-BUF pixel checks: %s\n", formats, ok ? "PASS" : "FAIL");
  free(readback);
  free(pixels);
  luna_gpu_destroy(gpu);
  close(fd);
  return !ok;
}
