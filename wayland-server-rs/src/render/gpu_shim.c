#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <libdrm/drm.h>

/* Slots are cheap (an EGLImage/texture name each); what is expensive is
 * re-importing a dma-buf every frame.  Every visible surface cycles through a
 * 2-3 buffer swapchain, so a handful of layers plus two or three windows
 * already needs well over 16 distinct buffers.  A cache that is too small
 * degenerates into create_image + glGenTextures + destroy for every plane of
 * every frame, which shows up as a regular hitch. */
#define LUNA_GPU_TEXTURE_CACHE 96
/* Only textures that own a private copy of the pixels (wl_shm uploads) count
 * against this budget.  dma-buf textures alias the client's memory and cost
 * nothing here; counting them made a full-screen wallpaper plus a full-screen
 * window (3 buffers each, ~9 MB apiece) overflow the budget on every frame. */
#define LUNA_GPU_TEXTURE_CACHE_BYTES (256ull * 1024ull * 1024ull)
/* A cached dma-buf image keeps the client's memory alive.  Drop entries that
 * have not been drawn for this many renders (destroyed or recycled buffers). */
#define LUNA_GPU_TEXTURE_MAX_AGE 600
#define LUNA_GPU_TEXTURE_SWEEP_PER_FRAME 2

struct luna_gpu_texture {
  uint64_t dev, ino, serial, modifier, used, bytes, last_frame, key;
  uintptr_t pixels;
  uint32_t width, height, stride, offset, fourcc;
  EGLImageKHR image;
  GLuint texture;
  int dmabuf;
};

struct luna_gpu {
  struct gbm_device *dev;
  struct gbm_surface *surface;
  struct gbm_bo *front, *pending;
  EGLDisplay display;
  EGLContext context;
  EGLSurface egl_surface;
  GLuint program, texture, vbo;
  GLint pos, uv, rect;
  uint32_t width, height;
  int linear_scanout;
  PFNEGLCREATEIMAGEKHRPROC create_image;
  PFNEGLDESTROYIMAGEKHRPROC destroy_image;
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_texture;
  struct luna_gpu_texture textures[LUNA_GPU_TEXTURE_CACHE];
  uint64_t texture_clock;
  uint64_t texture_bytes;
  uint64_t frame;
};
struct luna_gpu_output { void *bo; uint32_t handle, stride, fourcc, offset; uint64_t modifier; };
struct luna_gpu_format { uint32_t fourcc, pad; uint64_t modifier; };
struct luna_gpu_plane { int fd; const void *pixels; uint64_t dev,ino,serial,modifier; uint32_t width,height,stride,offset,fourcc; int32_t x,y; uint64_t key; uint32_t dirty_y0,dirty_y1; };

static GLuint shader(GLenum kind, const char *src) {
  GLuint s = glCreateShader(kind); GLint ok = 0;
  glShaderSource(s, 1, &src, NULL); glCompileShader(s);
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) { glDeleteShader(s); return 0; }
  return s;
}

static void texture_drop(struct luna_gpu *g, struct luna_gpu_texture *t) {
  if (t->texture) {
    if (g->texture_bytes >= t->bytes) g->texture_bytes -= t->bytes;
    else g->texture_bytes = 0;
    glDeleteTextures(1,&t->texture);
  }
  if (t->image!=EGL_NO_IMAGE_KHR && g->destroy_image) g->destroy_image(g->display,t->image);
  *t=(struct luna_gpu_texture){0};
}

/* Keep EGLImage/texture objects alive across frames.  dmabuf pixels then stay
 * zero-copy and SHM uploads update existing storage instead of feeding Mesa a
 * stream of allocations for deferred destruction.  Bound both entry count and
 * estimated texture bytes: a fixed 32-entry cache could retain hundreds of MiB
 * (or around a GiB with 4K buffers) after large windows had disappeared.
 * dev+ino prevents a recycled fd number from aliasing an old image. */
/* wl_shm surfaces own one texture for their whole life, whichever wl_buffer
 * they currently show.  It holds the previously committed content, so a new
 * commit only has to upload the rows its damage touched. */
#ifndef GL_UNPACK_ROW_LENGTH_EXT
#define GL_UNPACK_ROW_LENGTH_EXT 0x0CF2
#endif
/* wl_shm rows are often padded (Mesa software rendering aligns each row to 64
 * bytes), so the row pitch can exceed width*4.  Tell GL the pitch instead of
 * refusing the buffer: a refused plane sent the whole frame to the CPU. */
static void upload_rows(const struct luna_gpu_plane *p,uint32_t y0,uint32_t rows,const void *src) {
  uint32_t pitch=p->stride/4;
  int padded=pitch!=p->width;
  if(padded)glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT,(GLint)pitch);
  glTexSubImage2D(GL_TEXTURE_2D,0,0,y0,p->width,rows,GL_BGRA_EXT,GL_UNSIGNED_BYTE,src);
  if(padded)glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT,0);
}

static void shm_upload(const struct luna_gpu_plane *p,int incremental) {
  uint32_t y0=p->dirty_y0,y1=p->dirty_y1;
  if(y1>p->height)y1=p->height;
  if(incremental && y1>y0 && !(y0==0&&y1==p->height))
    upload_rows(p,y0,y1-y0,(const uint8_t*)p->pixels+(size_t)y0*p->stride);
  else
    upload_rows(p,0,p->height,p->pixels);
}

static GLuint texture_for_plane(struct luna_gpu *g,const struct luna_gpu_plane *p) {
  if(p->fd<0 && p->key){
    for(unsigned i=0;i<LUNA_GPU_TEXTURE_CACHE;i++){
      struct luna_gpu_texture *t=&g->textures[i];
      if(!t->texture || t->key!=p->key) continue;
      if(t->width==p->width&&t->height==p->height&&t->stride==p->stride&&t->fourcc==p->fourcc){
        glBindTexture(GL_TEXTURE_2D,t->texture);
        t->used=++g->texture_clock;
        t->last_frame=g->frame;
        if(t->serial!=p->serial){ shm_upload(p,1); t->serial=p->serial; }
        return t->texture;
      }
      texture_drop(g,t); /* resized: rebuild below with a full upload */
    }
  }
  uint64_t dev=p->fd>=0?p->dev:0,ino=p->fd>=0?p->ino:0;
  uintptr_t pixels=p->fd<0?(uintptr_t)p->pixels:0;
  uint64_t bytes=p->fd<0?(uint64_t)p->stride*(uint64_t)p->height:0;
  struct luna_gpu_texture *slot=NULL,*oldest=NULL;
  for(unsigned i=0;i<LUNA_GPU_TEXTURE_CACHE;i++){
    struct luna_gpu_texture *t=&g->textures[i];
    if(t->texture && t->key==p->key && t->dmabuf==(p->fd>=0) && t->dev==dev && t->ino==ino &&
       t->pixels==pixels && t->width==p->width && t->height==p->height &&
       t->stride==p->stride && t->offset==p->offset && t->fourcc==p->fourcc &&
       t->modifier==p->modifier){
      slot=t;break;
    }
    if(!t->texture){if(!slot)slot=t;continue;}
    if(!oldest||t->used<oldest->used)oldest=t;
  }
  if(slot && slot->texture){
    glBindTexture(GL_TEXTURE_2D,slot->texture);
    slot->used=++g->texture_clock;
    slot->last_frame=g->frame;
    if(p->fd<0 && slot->serial!=p->serial){
      /* Same incremental path as the keyed surface cache: only the rows the
       * client marked dirty cross the bus. */
      shm_upload(p,1);
      slot->serial=p->serial;
    }
    return slot->texture;
  }
  if(!slot){slot=oldest;texture_drop(g,slot);}

  /* Evict least-recently-used entries until the new texture fits the memory
   * budget.  If one texture alone is larger than the budget, keep that single
   * entry rather than thrashing on every frame. */
  while(bytes && g->texture_bytes &&
        (bytes>LUNA_GPU_TEXTURE_CACHE_BYTES ||
         g->texture_bytes>LUNA_GPU_TEXTURE_CACHE_BYTES-bytes)){
    struct luna_gpu_texture *victim=NULL;
    for(unsigned i=0;i<LUNA_GPU_TEXTURE_CACHE;i++){
      struct luna_gpu_texture *t=&g->textures[i];
      if(!t->texture || t==slot || t->dmabuf) continue;
      if(!victim||t->used<victim->used)victim=t;
    }
    if(!victim)break;
    texture_drop(g,victim);
  }

  slot->dev=dev;slot->ino=ino;slot->pixels=pixels;slot->width=p->width;
  slot->height=p->height;slot->stride=p->stride;slot->offset=p->offset;
  slot->fourcc=p->fourcc;slot->modifier=p->modifier;slot->dmabuf=p->fd>=0;
  slot->image=EGL_NO_IMAGE_KHR;
  slot->bytes=bytes;
  slot->key=p->fd<0?p->key:0;
  glGenTextures(1,&slot->texture);glBindTexture(GL_TEXTURE_2D,slot->texture);
  if(!slot->texture){*slot=(struct luna_gpu_texture){0};return 0;}
  g->texture_bytes+=bytes;
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
  if(p->fd>=0){
    EGLint a[]={EGL_WIDTH,(EGLint)p->width,EGL_HEIGHT,(EGLint)p->height,
      EGL_LINUX_DRM_FOURCC_EXT,(EGLint)p->fourcc,EGL_DMA_BUF_PLANE0_FD_EXT,p->fd,
      EGL_DMA_BUF_PLANE0_OFFSET_EXT,(EGLint)p->offset,EGL_DMA_BUF_PLANE0_PITCH_EXT,(EGLint)p->stride,
      EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,(EGLint)(uint32_t)p->modifier,
      EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,(EGLint)(uint32_t)(p->modifier>>32),EGL_NONE};
    slot->image=g->create_image(g->display,EGL_NO_CONTEXT,EGL_LINUX_DMA_BUF_EXT,NULL,a);
    if(slot->image==EGL_NO_IMAGE_KHR){texture_drop(g,slot);return 0;}
    g->image_texture(GL_TEXTURE_2D,slot->image);
  }else{
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,p->width,p->height,0,GL_BGRA_EXT,GL_UNSIGNED_BYTE,NULL);
  }
  slot->used=++g->texture_clock;
  slot->last_frame=g->frame;
  /* SHM content only changes when that wl_buffer is committed again.  Do not
   * upload every visible surface merely because some other surface damaged a
   * few pixels.  Cursor bitmaps use serial 0 and a stable pointer, so they are
   * uploaded once as well. */
  if(p->fd<0){
    shm_upload(p,0);
    slot->serial=p->serial;
  }
  return slot->texture;
}

/* Retire a couple of long-unused entries when the cache is under pressure.
 * Running this every frame tore down EGLImages during quiet desktop use and
 * showed up as regular console GC cost. */
static void texture_sweep(struct luna_gpu *g) {
  unsigned free_slots=0;
  for(unsigned i=0;i<LUNA_GPU_TEXTURE_CACHE;i++)
    if(!g->textures[i].texture) free_slots++;
  int pressure = free_slots < 8 ||
                 g->texture_bytes > (LUNA_GPU_TEXTURE_CACHE_BYTES * 3ull) / 4ull;
  if(!pressure) return;
  unsigned dropped=0;
  for(unsigned i=0;i<LUNA_GPU_TEXTURE_CACHE && dropped<LUNA_GPU_TEXTURE_SWEEP_PER_FRAME;i++){
    struct luna_gpu_texture *t=&g->textures[i];
    if(t->texture && g->frame-t->last_frame>LUNA_GPU_TEXTURE_MAX_AGE){
      texture_drop(g,t);dropped++;
    }
  }
}

void *luna_gpu_create(int fd, uint32_t w, uint32_t h) {
  struct luna_gpu *g = calloc(1, sizeof(*g));
  if (!g) return NULL;
  g->width=w; g->height=h; g->dev=gbm_create_device(fd);
  if (!g->dev) goto fail;
  /* If KMS cannot accept explicit modifiers, allocate a known linear
   * scanout layout instead of losing an opaque tiled layout at the FFI.
   * This concerns the display buffer; client textures remain GPU rendered. */
  struct drm_get_cap cap={DRM_CAP_ADDFB2_MODIFIERS,0};
  g->linear_scanout=ioctl(fd,DRM_IOCTL_GET_CAP,&cap)!=0 || !cap.value;
  uint32_t usage=GBM_BO_USE_SCANOUT|GBM_BO_USE_RENDERING;
  if(g->linear_scanout)usage|=GBM_BO_USE_LINEAR;
  g->surface=gbm_surface_create(g->dev,w,h,GBM_FORMAT_XRGB8888,usage);
  if (!g->surface) goto fail;
  PFNEGLGETPLATFORMDISPLAYEXTPROC platform=(void*)eglGetProcAddress("eglGetPlatformDisplayEXT");
  g->display=platform?platform(EGL_PLATFORM_GBM_KHR,g->dev,NULL):eglGetDisplay((void*)g->dev);
  if (g->display==EGL_NO_DISPLAY || !eglInitialize(g->display,NULL,NULL)) goto fail;
  eglBindAPI(EGL_OPENGL_ES_API);
  EGLint attrs[]={EGL_SURFACE_TYPE,EGL_WINDOW_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
                  EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_NONE};
  EGLConfig cfg=NULL; EGLint n=0, ctxa[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
  /* Component sizes are minima, and EGL sorts 10-bit configurations ahead
   * of 8-bit ones on some drivers. Match the GBM surface's native visual
   * explicitly: the first result can be XRGB2101010, which makes window
   * surface creation fail with EGL_BAD_MATCH for our XRGB8888 surface. */
  if (!eglChooseConfig(g->display,attrs,NULL,0,&n)||!n) goto fail;
  EGLConfig *configs=calloc((size_t)n,sizeof(*configs));
  if (!configs) goto fail;
  EGLint got=0;
  if (eglChooseConfig(g->display,attrs,configs,n,&got)) {
    for (EGLint i=0;i<got;i++) {
      EGLint visual=0;
      if (eglGetConfigAttrib(g->display,configs[i],EGL_NATIVE_VISUAL_ID,&visual) &&
          (uint32_t)visual==GBM_FORMAT_XRGB8888) { cfg=configs[i]; break; }
    }
  }
  free(configs);
  if (!cfg) goto fail;
  g->context=eglCreateContext(g->display,cfg,EGL_NO_CONTEXT,ctxa);
  g->egl_surface=eglCreateWindowSurface(g->display,cfg,(EGLNativeWindowType)g->surface,NULL);
  if (g->context==EGL_NO_CONTEXT||g->egl_surface==EGL_NO_SURFACE||
      !eglMakeCurrent(g->display,g->egl_surface,g->egl_surface,g->context)) goto fail;
  /* DRM page flips are the compositor's one and only vblank throttle. Leaving
   * the GBM EGL surface at its default swap interval can wait for one vblank
   * in eglSwapBuffers(), then wait for another when the returned BO is queued
   * on the CRTC. The two independent clocks periodically miss each other and
   * show up as a regular console hitch (and, on some drivers, steady 30 Hz).
   * Render immediately and let asynchronous page-flip completion pace us. */
  if (!eglSwapInterval(g->display,0)) goto fail;
  g->create_image=(void*)eglGetProcAddress("eglCreateImageKHR");
  g->destroy_image=(void*)eglGetProcAddress("eglDestroyImageKHR");
  g->image_texture=(void*)eglGetProcAddress("glEGLImageTargetTexture2DOES");
  const char *vs="attribute vec2 p;attribute vec2 t;uniform vec4 r;varying vec2 v;void main(){v=t;gl_Position=vec4(mix(r.xy,r.zw,p),0.,1.);}";
  const char *fs="precision mediump float;uniform sampler2D s;varying vec2 v;void main(){gl_FragColor=texture2D(s,v);}";
  GLuint a=shader(GL_VERTEX_SHADER,vs), b=shader(GL_FRAGMENT_SHADER,fs); GLint ok=0;
  if (!a||!b) goto fail;
  g->program=glCreateProgram(); glAttachShader(g->program,a); glAttachShader(g->program,b); glLinkProgram(g->program);
  glDeleteShader(a); glDeleteShader(b); glGetProgramiv(g->program,GL_LINK_STATUS,&ok); if(!ok) goto fail;
  g->pos=glGetAttribLocation(g->program,"p"); g->uv=glGetAttribLocation(g->program,"t");
  g->rect=glGetUniformLocation(g->program,"r");
  glGenTextures(1,&g->texture); glBindTexture(GL_TEXTURE_2D,g->texture);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
  /* Allocate mutable upload storage once.  glTexImage2D/glBufferData in every
   * frame made Mesa repeatedly retire driver allocations, producing periodic
   * cleanup stalls. */
  glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,w,h,0,GL_BGRA_EXT,GL_UNSIGNED_BYTE,NULL);
  /* Geometry never changes: place a unit quad in a static VBO and position
   * each plane with one uniform.  The old path uploaded 64 bytes and rebuilt
   * both attribute bindings once per visible surface, every frame. */
  static const float q[]={0,0,0,1, 1,0,1,1, 0,1,0,0, 1,1,1,0};
  glGenBuffers(1,&g->vbo); glBindBuffer(GL_ARRAY_BUFFER,g->vbo);
  glBufferData(GL_ARRAY_BUFFER,sizeof(q),q,GL_STATIC_DRAW);
  glEnableVertexAttribArray(g->pos); glEnableVertexAttribArray(g->uv);
  glVertexAttribPointer(g->pos,2,GL_FLOAT,GL_FALSE,16,(void*)0);
  glVertexAttribPointer(g->uv,2,GL_FLOAT,GL_FALSE,16,(void*)8);
  return g;
fail:
  if(g->display) eglTerminate(g->display);
  if(g->surface) gbm_surface_destroy(g->surface);
  if(g->dev) gbm_device_destroy(g->dev);
  free(g); return NULL;
}

/* Report only combinations importable as GL_TEXTURE_2D. Combinations marked
 * external_only would require GL_TEXTURE_EXTERNAL_OES and a different shader,
 * so advertising them would promise clients a path this compositor cannot
 * actually present. */
uint32_t luna_gpu_query_dmabuf_formats(struct luna_gpu *g,
                                      struct luna_gpu_format *out,
                                      uint32_t capacity) {
  if(!g||!g->create_image||!g->destroy_image||!g->image_texture)return 0;
  const char *ext=eglQueryString(g->display,EGL_EXTENSIONS);
  if(!ext||!strstr(ext,"EGL_EXT_image_dma_buf_import_modifiers"))return 0;
  PFNEGLQUERYDMABUFMODIFIERSEXTPROC query=
    (void*)eglGetProcAddress("eglQueryDmaBufModifiersEXT");
  if(!query)return 0;
  const EGLint formats[]={0x34325241,0x34325258}; /* AR24, XR24 */
  uint32_t total=0;
  for(unsigned f=0;f<sizeof(formats)/sizeof(formats[0]);f++){
    EGLint n=0;
    if(!query(g->display,formats[f],0,NULL,NULL,&n))continue;
    if(n<=0){
      /* Older radeonsi devices expose the modifier extension but enumerate
       * no modifiers. That does not rule out linear DMA-BUF import. Prove
       * allocation, export, EGL import and GL_TEXTURE_2D binding before
       * advertising it; an empty table otherwise disables GPU clients. */
      struct gbm_bo *probe=gbm_bo_create(g->dev,64,64,(uint32_t)formats[f],
                                       GBM_BO_USE_RENDERING|GBM_BO_USE_LINEAR);
      if(!probe)continue;
      int fd=gbm_bo_get_fd(probe);
      EGLImageKHR image=EGL_NO_IMAGE_KHR;
      GLuint texture=0;
      int usable=0;
      uint64_t modifier=gbm_bo_get_modifier(probe);
      if(fd>=0 && gbm_bo_get_plane_count(probe)==1 &&
         (modifier==0 || modifier==0x00ffffffffffffffull)){
        EGLint a[]={EGL_WIDTH,64,EGL_HEIGHT,64,EGL_LINUX_DRM_FOURCC_EXT,formats[f],
          EGL_DMA_BUF_PLANE0_FD_EXT,fd,EGL_DMA_BUF_PLANE0_OFFSET_EXT,0,
          EGL_DMA_BUF_PLANE0_PITCH_EXT,(EGLint)gbm_bo_get_stride(probe),
          EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,0,EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,0,EGL_NONE};
        image=g->create_image(g->display,EGL_NO_CONTEXT,EGL_LINUX_DMA_BUF_EXT,NULL,a);
        if(image!=EGL_NO_IMAGE_KHR){
          glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);
          g->image_texture(GL_TEXTURE_2D,image);
          usable=texture && glGetError()==GL_NO_ERROR;
        }
      }
      if(texture)glDeleteTextures(1,&texture);
      if(image!=EGL_NO_IMAGE_KHR)g->destroy_image(g->display,image);
      if(fd>=0)close(fd);
      gbm_bo_destroy(probe);
      if(usable){
        if(out&&total<capacity)out[total]=(struct luna_gpu_format){(uint32_t)formats[f],0,0};
        total++;
        /* These devices also lack GBM explicit-modifier allocation. Tell
         * clients they may use the legacy implicit allocation/import path;
         * LINEAR alone makes Chromium call create_with_modifiers(), fail
         * every SharedImage allocation and eventually disable WebGL. */
        if(out&&total<capacity)out[total]=(struct luna_gpu_format){(uint32_t)formats[f],0,0x00ffffffffffffffull};
        total++;
      }
      continue;
    }
    EGLuint64KHR *mods=calloc((size_t)n,sizeof(*mods));
    EGLBoolean *external=calloc((size_t)n,sizeof(*external));
    if(!mods||!external){free(mods);free(external);continue;}
    EGLint got=0;
    if(query(g->display,formats[f],n,mods,external,&got)){
      if(got>n)got=n;
      for(EGLint i=0;i<got;i++){
        if(external[i])continue;
        /* AR24/XR24 normally have one memory plane, but compression modifiers
         * may add an auxiliary metadata plane. The current ShmBuffer ABI is
         * single-plane, so probe and advertise ordinary tiled layouts only;
         * never promise a modifier whose params.add sequence we cannot hold. */
        uint64_t modifier=(uint64_t)mods[i];
        if(modifier==0x00ffffffffffffffull)continue; /* implicit/INVALID */
        if(modifier!=0){
          struct gbm_bo *probe=gbm_bo_create_with_modifiers(
            g->dev,256,256,(uint32_t)formats[f],&modifier,1);
          if(!probe)continue;
          int planes=gbm_bo_get_plane_count(probe);
          gbm_bo_destroy(probe);
          if(planes!=1)continue;
        }
        if(out&&total<capacity){
          out[total].fourcc=(uint32_t)formats[f];
          out[total].pad=0;
          out[total].modifier=modifier;
        }
        total++;
      }
    }
    free(mods);free(external);
  }
  return total;
}

/* Carry the allocation's complete layout into KMS registration. An
 * implicit modifier is not LINEAR unless we requested linear allocation. */
static int gpu_output(struct luna_gpu *g,struct luna_gpu_output *out) {
  struct gbm_bo *bo=g->pending;
  if(!bo || gbm_bo_get_plane_count(bo)!=1)return 0;
  uint64_t modifier=gbm_bo_get_modifier(bo);
  if(g->linear_scanout){
    if(modifier!=0 && modifier!=0x00ffffffffffffffull)return 0;
    modifier=0;
  }
  *out=(struct luna_gpu_output){bo,gbm_bo_get_handle(bo).u32,
    gbm_bo_get_stride(bo),gbm_bo_get_format(bo),gbm_bo_get_offset(bo,0),modifier};
  return 1;
}

int luna_gpu_render(struct luna_gpu *g,const uint32_t *pixels,uint32_t w,uint32_t h,struct luna_gpu_output *out) {
  if(!g||w!=g->width||h!=g->height||g->pending) return 0;
  glViewport(0,0,w,h); glUseProgram(g->program); glBindTexture(GL_TEXTURE_2D,g->texture);
  glTexSubImage2D(GL_TEXTURE_2D,0,0,0,w,h,GL_BGRA_EXT,GL_UNSIGNED_BYTE,pixels);
  glDisable(GL_BLEND); glUniform4f(g->rect,-1.f,-1.f,1.f,1.f);
  glDrawArrays(GL_TRIANGLE_STRIP,0,4);
  if(!eglSwapBuffers(g->display,g->egl_surface)) return 0;
  g->pending=gbm_surface_lock_front_buffer(g->surface); if(!g->pending) return 0;
  return gpu_output(g,out);
}

int luna_gpu_render_planes(struct luna_gpu *g,const struct luna_gpu_plane *p,uint32_t count,struct luna_gpu_output *out) {
  if(!g||!p||!count||g->pending||!g->create_image||!g->destroy_image||!g->image_texture)return 0;
  g->frame++;
  glViewport(0,0,g->width,g->height);
  /* A later opaque fullscreen plane hides the entire prefix. Do not even
   * import or upload those textures (wallpaper, covered browser windows). */
  uint32_t first=0;
  for(uint32_t i=0;i<count;i++) {
    if(p[i].fourcc==0x34325258 && p[i].x<=0 && p[i].y<=0 &&
       (int64_t)p[i].x+p[i].width>=g->width &&
       (int64_t)p[i].y+p[i].height>=g->height) first=i;
  }
  /* XR24 is opaque XRGB.  A fullscreen first plane already paints every
   * pixel, so the clear is pure bandwidth waste (Weston skips it too). */
  int first_opaque_full = p[first].fourcc==0x34325258 &&
      p[first].x<=0 && p[first].y<=0 &&
      (int64_t)p[first].x+p[first].width>=g->width &&
      (int64_t)p[first].y+p[first].height>=g->height;
  if(!first_opaque_full){
    glClearColor(.063f,.063f,.078f,1);
    glClear(GL_COLOR_BUFFER_BIT);
  }
  glUseProgram(g->program);
  glBlendFunc(GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
  int blending=0;
  for(uint32_t i=first;i<count;i++){
    if(p[i].x>=(int64_t)g->width || p[i].y>=(int64_t)g->height ||
       (int64_t)p[i].x+p[i].width<=0 || (int64_t)p[i].y+p[i].height<=0) continue;
    int opaque=p[i].fourcc==0x34325258; /* DRM_FORMAT_XRGB8888 / XR24 */
    if(opaque){
      if(blending){ glDisable(GL_BLEND); blending=0; }
    }else if(!blending){
      glEnable(GL_BLEND); blending=1;
    }
    if(!texture_for_plane(g,&p[i]))return 0;
    float l=2.f*p[i].x/g->width-1.f,r=2.f*(p[i].x+(int)p[i].width)/g->width-1.f;
    float t=1.f-2.f*p[i].y/g->height,b=1.f-2.f*(p[i].y+(int)p[i].height)/g->height;
    glUniform4f(g->rect,l,b,r,t);
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);
  }
  if(blending) glDisable(GL_BLEND);
  if(!eglSwapBuffers(g->display,g->egl_surface)) return 0;
  g->pending=gbm_surface_lock_front_buffer(g->surface);
  if(!g->pending) return 0;
  texture_sweep(g);
  return gpu_output(g,out);
}
/* A CPU-only decoration must not make GPU-only client surfaces vanish.
 * Read the imported texture through GL, never reinterpret tiled DMA memory. */
/* Read back the sub-rectangle (rx,ry,rw,rh) of a plane into `pixels`, tightly
 * packed rw*rh.  Row order matches a full-plane read, so any sub-rect equals
 * the same rows/columns of luna_gpu_read_plane(). */
int luna_gpu_read_plane_rect(struct luna_gpu *g,const struct luna_gpu_plane *p,
                             uint32_t rx,uint32_t ry,uint32_t rw,uint32_t rh,uint32_t *pixels) {
  if(!g||!p||!pixels||!rw||!rh||rx+rw>p->width||ry+rh>p->height)return 0;
  GLuint texture=texture_for_plane(g,p);
  if(!texture)return 0;
  GLint saved=0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING,&saved);
  GLuint fbo=0;
  glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
  int ok=glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE;
  if(ok){
    GLint pack=4;glGetIntegerv(GL_PACK_ALIGNMENT,&pack);
    glReadPixels((GLint)rx,(GLint)ry,(GLsizei)rw,(GLsizei)rh,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    ok=glGetError()==GL_NO_ERROR;
    if(ok)for(size_t i=0;i<(size_t)rw*rh;i++){
      uint32_t v=pixels[i];
      pixels[i]=(v&0xff00ff00u)|((v&0xffu)<<16)|((v>>16)&0xffu);
    }
  }
  glBindFramebuffer(GL_FRAMEBUFFER,(GLuint)saved);
  if(fbo)glDeleteFramebuffers(1,&fbo);
  return ok;
}

int luna_gpu_read_plane(struct luna_gpu *g,const struct luna_gpu_plane *p,uint32_t *pixels) {
  if(!p)return 0;
  return luna_gpu_read_plane_rect(g,p,0,0,p->width,p->height,pixels);
}

void luna_gpu_commit(struct luna_gpu *g){if(g->front)gbm_surface_release_buffer(g->surface,g->front);g->front=g->pending;g->pending=NULL;}
void luna_gpu_discard(struct luna_gpu *g){if(g->pending)gbm_surface_release_buffer(g->surface,g->pending);g->pending=NULL;}
void luna_gpu_destroy(struct luna_gpu *g){if(!g)return;if(g->pending)gbm_surface_release_buffer(g->surface,g->pending);if(g->front)gbm_surface_release_buffer(g->surface,g->front);for(unsigned i=0;i<LUNA_GPU_TEXTURE_CACHE;i++)texture_drop(g,&g->textures[i]);if(g->texture)glDeleteTextures(1,&g->texture);if(g->vbo)glDeleteBuffers(1,&g->vbo);if(g->program)glDeleteProgram(g->program);eglMakeCurrent(g->display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);if(g->egl_surface)eglDestroySurface(g->display,g->egl_surface);if(g->context)eglDestroyContext(g->display,g->context);if(g->display)eglTerminate(g->display);if(g->surface)gbm_surface_destroy(g->surface);if(g->dev)gbm_device_destroy(g->dev);free(g);}
