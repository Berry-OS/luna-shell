use super::Framebuffer;
use libc::{c_int, c_void};
use std::os::unix::io::RawFd;

#[repr(C)]
#[derive(Default)]
pub struct Output {
  pub bo: *mut c_void,
  pub handle: u32,
  pub stride: u32,
  pub fourcc: u32,
  pub offset: u32,
  pub modifier: u64,
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
  /// Non-zero for wl_shm planes: identifies the surface whose texture is
  /// updated incrementally.  `dirty_y0..dirty_y1` are the buffer rows that
  /// changed since that texture was last brought up to date.
  key: u64,
  dirty_y0: u32,
  dirty_y1: u32,
}

#[link(name = "luna_gpu", kind = "static")]
unsafe extern "C" {
  fn luna_gpu_create(fd: c_int, width: u32, height: u32) -> *mut c_void;
  fn luna_gpu_query_dmabuf_formats(ctx: *mut c_void, out: *mut DmabufFormat, capacity: u32) -> u32;
  fn luna_gpu_render(ctx: *mut c_void, pixels: *const u32, width: u32, height: u32, out: *mut Output) -> c_int;
  fn luna_gpu_render_planes(ctx: *mut c_void, planes: *const Plane, count: u32, out: *mut Output) -> c_int;
  fn luna_gpu_read_plane_rect(ctx: *mut c_void, plane: *const Plane, x: u32, y: u32, w: u32, h: u32, pixels: *mut u32) -> c_int;
  fn luna_gpu_commit(ctx: *mut c_void);
  fn luna_gpu_discard(ctx: *mut c_void);
  fn luna_gpu_destroy(ctx: *mut c_void);
}

/// `LUNA_SHM_FULL_UPLOAD=1` restores whole-buffer uploads for wl_shm surfaces,
/// for clients that under-report damage.
fn incremental_upload() -> bool {
  use std::sync::OnceLock;
  static ON: OnceLock<bool> = OnceLock::new();
  *ON.get_or_init(|| !std::env::var("LUNA_SHM_FULL_UPLOAD").map(|v| !v.is_empty() && v != "0").unwrap_or(false))
}

pub struct GpuComposer {
  ctx: *mut c_void,
  width: u32,
  height: u32,
  /// Reused for every composition.  Rebuilding this Vec at frame rate made
  /// allocator cleanup land unpredictably in later frames.
  planes: Vec<Plane>,
  /// Reused readback target (avoids a frame-sized allocation per blit).
  scratch: Vec<u32>,
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
      width,
      height,
      planes: Vec::new(),
      scratch: Vec::new(),
      dmabuf_formats,
    })
  }

  pub fn render(&mut self, fb: &Framebuffer) -> Option<Output> {
    let mut out = Output::default();
    let ok = unsafe { luna_gpu_render(self.ctx, fb.pixels.as_ptr(), fb.width, fb.height, &mut out) };
    (ok != 0).then_some(out)
  }

  fn push_plane(&mut self, x: i32, y: i32, b: &crate::shm::ShmBuffer) -> Option<()> {
    let width: u32 = b.width.try_into().ok()?;
    let stride: u32 = b.stride.try_into().ok()?;
    let fd = b.dmabuf_fd().unwrap_or(-1);
    let (dev, ino) = b.storage_id();
    // Padded rows are fine (the shim uploads with GL_UNPACK_ROW_LENGTH); a pitch
    // that is not a whole number of pixels or is narrower than a row is not.
    if fd < 0 && (stride % 4 != 0 || stride < width * 4) {
      return None;
    }
    let (key, dirty_y0, dirty_y1) = match (&b.surface_gpu, fd < 0, incremental_upload()) {
      (Some(g), true, true) => {
        let d = g.dirty.get();
        (g.id, d.y0.max(0) as u32, d.y1.clamp(0, i32::MAX) as u32)
      }
      _ => (0, 0, 0),
    };
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
      key,
      dirty_y0,
      dirty_y1,
    });
    Some(())
  }

  pub fn render_dmabufs(&mut self, surfaces: &[(i32, i32, crate::shm::ShmBuffer)], extra_surface: Option<(i32, i32, &crate::shm::ShmBuffer)>, bitmap: Option<super::GpuBitmap<'_>>) -> Option<Output> {
    self.planes.clear();
    let scene = || surfaces.iter().map(|(x,y,b)| (*x,*y,b)).chain(extra_surface);
    let first = scene().enumerate().filter_map(|(i, (x,y,b))| {
      (b.format == crate::shm::FORMAT_XRGB8888 && x <= 0 && y <= 0
        && x as i64 + b.width as i64 >= self.width as i64
        && y as i64 + b.height as i64 >= self.height as i64).then_some(i)
    }).last().unwrap_or(0);
    let (width, height) = (self.width, self.height);
    let visible = |x: i32, y: i32, b: &crate::shm::ShmBuffer|
      (x as i64) < width as i64 && (y as i64) < height as i64
        && x as i64 + b.width as i64 > 0 && y as i64 + b.height as i64 > 0;
    for (x, y, b) in scene().skip(first) {
      if visible(x, y, b) { self.push_plane(x, y, b)?; }
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
        key: 0,
        dirty_y0: 0,
        dirty_y1: 0,
      });
    }
    let mut out = Output::default();
    let ok = unsafe { luna_gpu_render_planes(self.ctx, self.planes.as_ptr(), self.planes.len() as u32, &mut out) };
    if ok != 0 {
      // Hidden textures were not uploaded. Preserve their damage until they
      // become visible, including all commits while covered or off-screen.
      for (x, y, b) in scene().skip(first) {
        if visible(x, y, b) {
          if let Some(g) = &b.surface_gpu { g.dirty.set(super::Rect::EMPTY); }
        }
      }
    }
    (ok != 0).then_some(out)
  }

  /// Read back `rect` (buffer coordinates) of `buf` into a scratch buffer that
  /// is reused across calls; the returned slice is tightly packed
  /// `rect` width * height.  Only the requested pixels cross the bus.
  pub fn readback(&mut self, buf: &crate::shm::ShmBuffer, rect: super::Rect) -> Option<&[u32]> {
    let rect = rect.intersect(&super::Rect::new(0, 0, buf.width, buf.height));
    if rect.is_empty() {
      return None;
    }
    let (w, h) = ((rect.x1 - rect.x0) as usize, (rect.y1 - rect.y0) as usize);
    let len = w.checked_mul(h)?;
    self.planes.clear();
    self.push_plane(0, 0, buf)?;
    if self.scratch.len() < len {
      self.scratch.resize(len, 0);
    }
    let ok = unsafe {
      luna_gpu_read_plane_rect(self.ctx, &self.planes[0], rect.x0 as u32, rect.y0 as u32, w as u32, h as u32, self.scratch.as_mut_ptr())
    };
    (ok != 0).then(|| &self.scratch[..len])
  }

  /// Read the frame just rendered by `render_dmabufs` back into ARGB pixels
  /// (headless/test use).  Call before `commit`.
  pub fn read_output(&mut self, out: &Output, width: u32, height: u32) -> Option<Vec<u32>> {
    unsafe extern "C" {
      fn gbm_bo_get_fd(bo: *mut c_void) -> c_int;
    }
    let fd = unsafe { gbm_bo_get_fd(out.bo) };
    if fd < 0 {
      return None;
    }
    let mut st: libc::stat = unsafe { std::mem::zeroed() };
    unsafe { libc::fstat(fd, &mut st) };
    let plane = Plane {
      fd,
      pixels: std::ptr::null(),
      dev: st.st_dev as u64,
      ino: st.st_ino as u64,
      serial: 0,
      modifier: out.modifier,
      width,
      height,
      stride: out.stride,
      offset: out.offset,
      fourcc: out.fourcc,
      x: 0,
      y: 0,
      key: 0,
      dirty_y0: 0,
      dirty_y1: 0,
    };
    let mut pixels = vec![0u32; (width * height) as usize];
    let ok = unsafe { luna_gpu_read_plane_rect(self.ctx, &plane, 0, 0, width, height, pixels.as_mut_ptr()) };
    unsafe { libc::close(fd) };
    (ok != 0).then_some(pixels)
  }

  pub fn commit(&mut self) { unsafe { luna_gpu_commit(self.ctx) } }
  pub fn discard(&mut self) { unsafe { luna_gpu_discard(self.ctx) } }

  pub fn dmabuf_formats(&self) -> &[(u32, u64)] { &self.dmabuf_formats }
}

impl Drop for GpuComposer {
  fn drop(&mut self) { unsafe { luna_gpu_destroy(self.ctx) } }
}
