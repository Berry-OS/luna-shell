#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define LUNA_GPU_TEXTURE_CACHE 16
#define LUNA_GPU_TEXTURE_CACHE_BYTES (64ull * 1024ull * 1024ull)

struct luna_gpu_texture {
  uint64_t dev, ino, serial, modifier, used, bytes;
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
  PFNEGLCREATEIMAGEKHRPROC create_image;
  PFNEGLDESTROYIMAGEKHRPROC destroy_image;
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_texture;
  struct luna_gpu_texture textures[LUNA_GPU_TEXTURE_CACHE];
  uint64_t texture_clock;
  uint64_t texture_bytes;
};
struct luna_gpu_output { void *bo; uint32_t handle, stride; };
struct luna_gpu_format { uint32_t fourcc, pad; uint64_t modifier; };
struct luna_gpu_plane { int fd; const void *pixels; uint64_t dev,ino,serial,modifier; uint32_t width,height,stride,offset,fourcc; int32_t x,y; };

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
static GLuint texture_for_plane(struct luna_gpu *g,const struct luna_gpu_plane *p) {
  uint64_t dev=p->fd>=0?p->dev:0,ino=p->fd>=0?p->ino:0;
  uintptr_t pixels=p->fd<0?(uintptr_t)p->pixels:0;
  uint64_t bytes=(uint64_t)p->stride*(uint64_t)p->height;
  struct luna_gpu_texture *slot=NULL,*oldest=NULL;
  for(unsigned i=0;i<LUNA_GPU_TEXTURE_CACHE;i++){
    struct luna_gpu_texture *t=&g->textures[i];
    if(t->texture && t->dmabuf==(p->fd>=0) && t->dev==dev && t->ino==ino &&
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
    if(p->fd<0 && slot->serial!=p->serial){
      glTexSubImage2D(GL_TEXTURE_2D,0,0,0,p->width,p->height,GL_BGRA_EXT,GL_UNSIGNED_BYTE,p->pixels);
      slot->serial=p->serial;
    }
    return slot->texture;
  }
  if(!slot){slot=oldest;texture_drop(g,slot);}

  /* Evict least-recently-used entries until the new texture fits the memory
   * budget.  If one texture alone is larger than the budget, keep that single
   * entry rather than thrashing on every frame. */
  while(g->texture_bytes &&
        (bytes>LUNA_GPU_TEXTURE_CACHE_BYTES ||
         g->texture_bytes>LUNA_GPU_TEXTURE_CACHE_BYTES-bytes)){
    struct luna_gpu_texture *victim=NULL;
    for(unsigned i=0;i<LUNA_GPU_TEXTURE_CACHE;i++){
      struct luna_gpu_texture *t=&g->textures[i];
      if(!t->texture || t==slot) continue;
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
  /* SHM content only changes when that wl_buffer is committed again.  Do not
   * upload every visible surface merely because some other surface damaged a
   * few pixels.  Cursor bitmaps use serial 0 and a stable pointer, so they are
   * uploaded once as well. */
  if(p->fd<0){
    glTexSubImage2D(GL_TEXTURE_2D,0,0,0,p->width,p->height,GL_BGRA_EXT,GL_UNSIGNED_BYTE,p->pixels);
    slot->serial=p->serial;
  }
  return slot->texture;
}

void *luna_gpu_create(int fd, uint32_t w, uint32_t h) {
  struct luna_gpu *g = calloc(1, sizeof(*g));
  if (!g) return NULL;
  g->width=w; g->height=h; g->dev=gbm_create_device(fd);
  if (!g->dev) goto fail;
  g->surface=gbm_surface_create(g->dev,w,h,GBM_FORMAT_XRGB8888,
                                GBM_BO_USE_SCANOUT|GBM_BO_USE_RENDERING);
  if (!g->surface) goto fail;
  PFNEGLGETPLATFORMDISPLAYEXTPROC platform=(void*)eglGetProcAddress("eglGetPlatformDisplayEXT");
  g->display=platform?platform(EGL_PLATFORM_GBM_KHR,g->dev,NULL):eglGetDisplay((void*)g->dev);
  if (g->display==EGL_NO_DISPLAY || !eglInitialize(g->display,NULL,NULL)) goto fail;
  eglBindAPI(EGL_OPENGL_ES_API);
  EGLint attrs[]={EGL_SURFACE_TYPE,EGL_WINDOW_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
                  EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_NONE};
  EGLConfig cfg; EGLint n=0, ctxa[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
  if (!eglChooseConfig(g->display,attrs,&cfg,1,&n)||!n) goto fail;
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
  if(!g||!g->create_image||!g->image_texture)return 0;
  const char *ext=eglQueryString(g->display,EGL_EXTENSIONS);
  if(!ext||!strstr(ext,"EGL_EXT_image_dma_buf_import_modifiers"))return 0;
  PFNEGLQUERYDMABUFMODIFIERSEXTPROC query=
    (void*)eglGetProcAddress("eglQueryDmaBufModifiersEXT");
  if(!query)return 0;
  const EGLint formats[]={0x34325241,0x34325258}; /* AR24, XR24 */
  uint32_t total=0;
  for(unsigned f=0;f<sizeof(formats)/sizeof(formats[0]);f++){
    EGLint n=0;
    if(!query(g->display,formats[f],0,NULL,NULL,&n)||n<=0)continue;
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

int luna_gpu_render(struct luna_gpu *g,const uint32_t *pixels,uint32_t w,uint32_t h,struct luna_gpu_output *out) {
  if(!g||w!=g->width||h!=g->height||g->pending) return 0;
  glViewport(0,0,w,h); glUseProgram(g->program); glBindTexture(GL_TEXTURE_2D,g->texture);
  glTexSubImage2D(GL_TEXTURE_2D,0,0,0,w,h,GL_BGRA_EXT,GL_UNSIGNED_BYTE,pixels);
  glDisable(GL_BLEND); glUniform4f(g->rect,-1.f,-1.f,1.f,1.f);
  glDrawArrays(GL_TRIANGLE_STRIP,0,4);
  if(!eglSwapBuffers(g->display,g->egl_surface)) return 0;
  g->pending=gbm_surface_lock_front_buffer(g->surface); if(!g->pending) return 0;
  out->bo=g->pending; out->handle=gbm_bo_get_handle(g->pending).u32; out->stride=gbm_bo_get_stride(g->pending); return 1;
}

int luna_gpu_render_planes(struct luna_gpu *g,const struct luna_gpu_plane *p,uint32_t count,struct luna_gpu_output *out) {
  if(!g||!p||!count||g->pending||!g->create_image||!g->destroy_image||!g->image_texture)return 0;
  glViewport(0,0,g->width,g->height);glClearColor(.063f,.063f,.078f,1);glClear(GL_COLOR_BUFFER_BIT);
  glUseProgram(g->program);glEnable(GL_BLEND);glBlendFunc(GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
  for(uint32_t i=0;i<count;i++){
    if(!texture_for_plane(g,&p[i]))return 0;
    float l=2.f*p[i].x/g->width-1.f,r=2.f*(p[i].x+(int)p[i].width)/g->width-1.f;
    float t=1.f-2.f*p[i].y/g->height,b=1.f-2.f*(p[i].y+(int)p[i].height)/g->height;
    glUniform4f(g->rect,l,b,r,t);
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);
  }
  if(!eglSwapBuffers(g->display,g->egl_surface)) return 0;
  g->pending=gbm_surface_lock_front_buffer(g->surface);
  if(!g->pending) return 0;
  out->bo=g->pending;out->handle=gbm_bo_get_handle(g->pending).u32;out->stride=gbm_bo_get_stride(g->pending);return 1;
}
void luna_gpu_commit(struct luna_gpu *g){if(g->front)gbm_surface_release_buffer(g->surface,g->front);g->front=g->pending;g->pending=NULL;}
void luna_gpu_discard(struct luna_gpu *g){if(g->pending)gbm_surface_release_buffer(g->surface,g->pending);g->pending=NULL;}
void luna_gpu_destroy(struct luna_gpu *g){if(!g)return;if(g->pending)gbm_surface_release_buffer(g->surface,g->pending);if(g->front)gbm_surface_release_buffer(g->surface,g->front);for(unsigned i=0;i<LUNA_GPU_TEXTURE_CACHE;i++)texture_drop(g,&g->textures[i]);if(g->texture)glDeleteTextures(1,&g->texture);if(g->vbo)glDeleteBuffers(1,&g->vbo);if(g->program)glDeleteProgram(g->program);eglMakeCurrent(g->display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);if(g->egl_surface)eglDestroySurface(g->display,g->egl_surface);if(g->context)eglDestroyContext(g->display,g->context);if(g->display)eglTerminate(g->display);if(g->surface)gbm_surface_destroy(g->surface);if(g->dev)gbm_device_destroy(g->dev);free(g);}
