#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <stdint.h>
#include <stdlib.h>

#define LUNA_GPU_TEXTURE_CACHE 32

struct luna_gpu_texture {
  uint64_t dev, ino, serial, used;
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
  GLint pos, uv;
  uint32_t width, height;
  PFNEGLCREATEIMAGEKHRPROC create_image;
  PFNEGLDESTROYIMAGEKHRPROC destroy_image;
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_texture;
  struct luna_gpu_texture textures[LUNA_GPU_TEXTURE_CACHE];
  uint64_t texture_clock;
};
struct luna_gpu_output { void *bo; uint32_t handle, stride; };
struct luna_gpu_plane { int fd; const void *pixels; uint64_t dev,ino,serial; uint32_t width,height,stride,offset,fourcc; int32_t x,y; };

static GLuint shader(GLenum kind, const char *src) {
  GLuint s = glCreateShader(kind); GLint ok = 0;
  glShaderSource(s, 1, &src, NULL); glCompileShader(s);
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) { glDeleteShader(s); return 0; }
  return s;
}

static void texture_drop(struct luna_gpu *g, struct luna_gpu_texture *t) {
  if (t->texture) glDeleteTextures(1,&t->texture);
  if (t->image!=EGL_NO_IMAGE_KHR && g->destroy_image) g->destroy_image(g->display,t->image);
  *t=(struct luna_gpu_texture){0};
}

/* Keep EGLImage/texture objects alive across frames.  dmabuf pixels then stay
 * zero-copy and SHM uploads update existing storage instead of feeding Mesa a
 * stream of allocations for deferred destruction.  The cache is deliberately
 * bounded; dev+ino prevents a recycled fd number from aliasing an old image. */
static GLuint texture_for_plane(struct luna_gpu *g,const struct luna_gpu_plane *p) {
  uint64_t dev=p->fd>=0?p->dev:0,ino=p->fd>=0?p->ino:0;
  uintptr_t pixels=p->fd<0?(uintptr_t)p->pixels:0;
  struct luna_gpu_texture *slot=NULL,*oldest=NULL;
  for(unsigned i=0;i<LUNA_GPU_TEXTURE_CACHE;i++){
    struct luna_gpu_texture *t=&g->textures[i];
    if(t->texture && t->dmabuf==(p->fd>=0) && t->dev==dev && t->ino==ino &&
       t->pixels==pixels && t->width==p->width && t->height==p->height &&
       t->stride==p->stride && t->offset==p->offset && t->fourcc==p->fourcc){
      slot=t;break;
    }
    if(!t->texture){if(!slot)slot=t;continue;}
    if(!oldest||t->used<oldest->used)oldest=t;
  }
  if(!slot){slot=oldest;texture_drop(g,slot);}
  int fresh=!slot->texture;
  if(fresh){
    slot->dev=dev;slot->ino=ino;slot->pixels=pixels;slot->width=p->width;
    slot->height=p->height;slot->stride=p->stride;slot->offset=p->offset;
    slot->fourcc=p->fourcc;slot->dmabuf=p->fd>=0;slot->image=EGL_NO_IMAGE_KHR;
    glGenTextures(1,&slot->texture);glBindTexture(GL_TEXTURE_2D,slot->texture);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    if(p->fd>=0){
      EGLint a[]={EGL_WIDTH,(EGLint)p->width,EGL_HEIGHT,(EGLint)p->height,
        EGL_LINUX_DRM_FOURCC_EXT,(EGLint)p->fourcc,EGL_DMA_BUF_PLANE0_FD_EXT,p->fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,(EGLint)p->offset,EGL_DMA_BUF_PLANE0_PITCH_EXT,(EGLint)p->stride,EGL_NONE};
      slot->image=g->create_image(g->display,EGL_NO_CONTEXT,EGL_LINUX_DMA_BUF_EXT,NULL,a);
      if(slot->image==EGL_NO_IMAGE_KHR){texture_drop(g,slot);return 0;}
      g->image_texture(GL_TEXTURE_2D,slot->image);
    }else{
      glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,p->width,p->height,0,GL_BGRA_EXT,GL_UNSIGNED_BYTE,NULL);
    }
  }else glBindTexture(GL_TEXTURE_2D,slot->texture);
  slot->used=++g->texture_clock;
  /* SHM content only changes when that wl_buffer is committed again.  Do not
   * upload every visible surface merely because some other surface damaged a
   * few pixels.  Cursor bitmaps use serial 0 and a stable pointer, so they are
   * uploaded once as well. */
  if(p->fd<0 && (fresh || slot->serial!=p->serial)){
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
  g->create_image=(void*)eglGetProcAddress("eglCreateImageKHR");
  g->destroy_image=(void*)eglGetProcAddress("eglDestroyImageKHR");
  g->image_texture=(void*)eglGetProcAddress("glEGLImageTargetTexture2DOES");
  const char *vs="attribute vec2 p;attribute vec2 t;varying vec2 v;void main(){v=t;gl_Position=vec4(p,0.,1.);}";
  const char *fs="precision mediump float;uniform sampler2D s;varying vec2 v;void main(){gl_FragColor=texture2D(s,v);}";
  GLuint a=shader(GL_VERTEX_SHADER,vs), b=shader(GL_FRAGMENT_SHADER,fs); GLint ok=0;
  if (!a||!b) goto fail;
  g->program=glCreateProgram(); glAttachShader(g->program,a); glAttachShader(g->program,b); glLinkProgram(g->program);
  glDeleteShader(a); glDeleteShader(b); glGetProgramiv(g->program,GL_LINK_STATUS,&ok); if(!ok) goto fail;
  g->pos=glGetAttribLocation(g->program,"p"); g->uv=glGetAttribLocation(g->program,"t");
  glGenTextures(1,&g->texture); glBindTexture(GL_TEXTURE_2D,g->texture);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
  /* Allocate mutable upload storage once.  glTexImage2D/glBufferData in every
   * frame made Mesa repeatedly retire driver allocations, producing periodic
   * cleanup stalls. */
  glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,w,h,0,GL_BGRA_EXT,GL_UNSIGNED_BYTE,NULL);
  glGenBuffers(1,&g->vbo); glBindBuffer(GL_ARRAY_BUFFER,g->vbo);
  glBufferData(GL_ARRAY_BUFFER,16*sizeof(float),NULL,GL_DYNAMIC_DRAW);
  return g;
fail:
  if(g->display) eglTerminate(g->display);
  if(g->surface) gbm_surface_destroy(g->surface);
  if(g->dev) gbm_device_destroy(g->dev);
  free(g); return NULL;
}

int luna_gpu_render(struct luna_gpu *g,const uint32_t *pixels,uint32_t w,uint32_t h,struct luna_gpu_output *out) {
  if(!g||w!=g->width||h!=g->height||g->pending) return 0;
  static const float q[]={-1,-1,0,1, 1,-1,1,1, -1,1,0,0, 1,1,1,0};
  glViewport(0,0,w,h); glUseProgram(g->program); glBindTexture(GL_TEXTURE_2D,g->texture);
  glTexSubImage2D(GL_TEXTURE_2D,0,0,0,w,h,GL_BGRA_EXT,GL_UNSIGNED_BYTE,pixels);
  glBindBuffer(GL_ARRAY_BUFFER,g->vbo); glBufferSubData(GL_ARRAY_BUFFER,0,sizeof(q),q);
  glEnableVertexAttribArray(g->pos); glEnableVertexAttribArray(g->uv);
  glVertexAttribPointer(g->pos,2,GL_FLOAT,GL_FALSE,16,(void*)0);
  glVertexAttribPointer(g->uv,2,GL_FLOAT,GL_FALSE,16,(void*)8);
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
    float q[]={l,b,0,1,r,b,1,1,l,t,0,0,r,t,1,0};
    glBindBuffer(GL_ARRAY_BUFFER,g->vbo);glBufferSubData(GL_ARRAY_BUFFER,0,sizeof(q),q);
    glEnableVertexAttribArray(g->pos);glEnableVertexAttribArray(g->uv);glVertexAttribPointer(g->pos,2,GL_FLOAT,0,16,0);glVertexAttribPointer(g->uv,2,GL_FLOAT,0,16,(void*)8);
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
