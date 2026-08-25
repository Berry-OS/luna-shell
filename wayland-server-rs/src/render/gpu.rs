use super::Framebuffer;
use libc::{c_int, c_void};
use std::os::unix::io::RawFd;

#[repr(C)]
#[derive(Default)]
struct Output {
  bo: *mut c_void,
  handle: u32,
  stride: u32,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct DmabufFormat {
  fourcc: u32,
  _pad: u32,
  modifier: u64,
}
#[repr(C)]
struct Plane {
  fd: c_int,
  pixels: *const u8,
  dev: u64,
  ino: u64,
  serial: u64,
  modifier: u64,
  width: u32,
  height: u32,
  stride: u32,
  offset: u32,
  fourcc: u32,
  x: i32,
  y: i32,
}

#[link(name = "luna_gpu", kind = "static")]
unsafe extern "C" {
  fn luna_gpu_create(fd: c_int, width: u32, height: u32) -> *mut c_void;
  fn luna_gpu_query_dmabuf_formats(ctx: *mut c_void, out: *mut DmabufFormat, capacity: u32) -> u32;
  fn luna_gpu_render(ctx: *mut c_void, pixels: *const u32, width: u32, height: u32, out: *mut Output) -> c_int;
  fn luna_gpu_render_planes(ctx: *mut c_void, planes: *const Plane, count: u32, out: *mut Output) -> c_int;
  fn luna_gpu_commit(ctx: *mut c_void);
  fn luna_gpu_discard(ctx: *mut c_void);
  fn luna_gpu_destroy(ctx: *mut c_void);
}

pub struct GpuComposer {
  ctx: *mut c_void,
  /// Reused for every composition.  Rebuilding this Vec at frame rate made
  /// allocator cleanup land unpredictably in later frames.
  planes: Vec<Plane>,
  dmabuf_formats: Vec<(u32, u64)>,
}

impl GpuComposer {
  pub fn new(fd: RawFd, width: u32, height: u32) -> Option<Self> {
    let p = unsafe { luna_gpu_create(fd, width, height) };
    if p.is_null() {
      return None;
    }
    // Two 32-bit RGB formats have far fewer than this many modifiers. A
    // single query avoids allocating/destroying GBM probe BOs twice.
    const MAX_FORMAT_MODIFIERS: u32 = 4096;
    let mut queried = vec![DmabufFormat::default(); MAX_FORMAT_MODIFIERS as usize];
    let written = unsafe { luna_gpu_query_dmabuf_formats(p, queried.as_mut_ptr(), MAX_FORMAT_MODIFIERS) }.min(MAX_FORMAT_MODIFIERS) as usize;
    let mut dmabuf_formats: Vec<(u32, u64)> = queried[..written].iter().map(|f| (f.fourcc, f.modifier)).collect();
    dmabuf_formats.sort_unstable();
    dmabuf_formats.dedup();
    Some(Self {
      ctx: p,
      planes: Vec::new(),
      dmabuf_formats,
    })
  }

  pub fn render(&mut self, fb: &Framebuffer) -> Option<(*mut c_void, u32, u32)> {
    let mut out = Output::default();
    let ok = unsafe { luna_gpu_render(self.ctx, fb.pixels.as_ptr(), fb.width, fb.height, &mut out) };
    (ok != 0).then_some((out.bo, out.handle, out.stride))
  }

  fn push_plane(&mut self, x: i32, y: i32, b: &crate::shm::ShmBuffer) -> Option<()> {
    let width: u32 = b.width.try_into().ok()?;
    let stride: u32 = b.stride.try_into().ok()?;
    let fd = b.dmabuf_fd().unwrap_or(-1);
    let (dev, ino) = b.storage_id();
    if fd < 0 && stride != width * 4 {
      return None;
    }
    self.planes.push(Plane {
      fd,
      pixels: if fd < 0 { b.upload_ptr()? } else { std::ptr::null() },
      dev,
      ino,
      serial: b.serial(),
      modifier: b.dmabuf_modifier().unwrap_or(0),
      width,
      height: b.height.try_into().ok()?,
      stride,
      offset: b.offset.try_into().ok()?,
      fourcc: if b.format == crate::shm::FORMAT_XRGB8888 { 0x34325258 } else { 0x34325241 },
      x,
      y,
    });
    Some(())
  }

  pub fn render_dmabufs(&mut self, surfaces: &[(i32, i32, crate::shm::ShmBuffer)], extra_surface: Option<(i32, i32, &crate::shm::ShmBuffer)>, bitmap: Option<super::GpuBitmap<'_>>) -> Option<(*mut c_void, u32, u32)> {
    self.planes.clear();
    for (x, y, b) in surfaces {
      self.push_plane(*x, *y, b)?;
    }
    if let Some((x, y, b)) = extra_surface {
      self.push_plane(x, y, b)?;
    }
    if let Some(b) = bitmap {
      self.planes.push(Plane {
        fd: -1,
        pixels: b.pixels.as_ptr() as *const u8,
        dev: 0,
        ino: 0,
        serial: 0,
        modifier: 0,
        width: b.width,
        height: b.height,
        stride: b.width * 4,
        offset: 0,
        fourcc: 0x34325241,
        x: b.x,
        y: b.y,
      });
    }
    let mut out = Output::default();
    let ok = unsafe { luna_gpu_render_planes(self.ctx, self.planes.as_ptr(), self.planes.len() as u32, &mut out) };
    (ok != 0).then_some((out.bo, out.handle, out.stride))
  }

  pub fn commit(&mut self) { unsafe { luna_gpu_commit(self.ctx) } }
  pub fn discard(&mut self) { unsafe { luna_gpu_discard(self.ctx) } }

  pub fn dmabuf_formats(&self) -> &[(u32, u64)] { &self.dmabuf_formats }
}

impl Drop for GpuComposer {
  fn drop(&mut self) { unsafe { luna_gpu_destroy(self.ctx) } }
}
