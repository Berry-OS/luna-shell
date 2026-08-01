/*
 * Copyright © 2026 Yuichiro Nakada / Project Vespera
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

use super::vt::VtSession;
use super::{probe_render_node, Backend, Framebuffer, InputEvent};
use crate::input::evdev::EvdevInput;
use crate::shm::{ShmBuffer, FORMAT_ARGB8888, FORMAT_XRGB8888};
use libc::{c_void, ioctl, mmap, munmap, open, MAP_FAILED, MAP_SHARED, O_CLOEXEC, O_RDWR, PROT_READ, PROT_WRITE};
use std::ffi::CString;
use std::os::unix::io::RawFd;
use std::sync::mpsc;

// ioctl numbers (asm-generic/ioctl.h)
const DRM_BASE: u64 = 0x64; // 'd'
fn iowr<T>(nr: u64) -> u64 { (3u64 << 30) | (DRM_BASE << 8) | nr | ((std::mem::size_of::<T>() as u64) << 16) }
fn iow<T>(nr: u64) -> u64 { (1u64 << 30) | (DRM_BASE << 8) | nr | ((std::mem::size_of::<T>() as u64) << 16) }
fn io(nr: u64) -> u64 { (DRM_BASE << 8) | nr }

#[repr(C)]
#[derive(Default)]
struct ModeCardRes {
  fb_id_ptr: u64,
  crtc_id_ptr: u64,
  connector_id_ptr: u64,
  encoder_id_ptr: u64,
  count_fbs: u32,
  count_crtcs: u32,
  count_connectors: u32,
  count_encoders: u32,
  min_width: u32,
  max_width: u32,
  min_height: u32,
  max_height: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct ModeInfo {
  clock: u32,
  hdisplay: u16,
  hsync_start: u16,
  hsync_end: u16,
  htotal: u16,
  hskew: u16,
  vdisplay: u16,
  vsync_start: u16,
  vsync_end: u16,
  vtotal: u16,
  vscan: u16,
  vrefresh: u32,
  flags: u32,
  type_: u32,
  name: [u8; 32],
}
impl Default for ModeInfo {
  fn default() -> Self { unsafe { std::mem::zeroed() } }
}

#[repr(C)]
#[derive(Default)]
struct ModeGetConnector {
  encoders_ptr: u64,
  modes_ptr: u64,
  props_ptr: u64,
  prop_values_ptr: u64,
  count_modes: u32,
  count_props: u32,
  count_encoders: u32,
  encoder_id: u32,
  connector_id: u32,
  connector_type: u32,
  connector_type_id: u32,
  connection: u32,
  mm_width: u32,
  mm_height: u32,
  subpixel: u32,
  pad: u32,
}

#[repr(C)]
#[derive(Default)]
struct ModeGetEncoder {
  encoder_id: u32,
  encoder_type: u32,
  crtc_id: u32,
  possible_crtcs: u32,
  possible_clones: u32,
}

#[repr(C)]
#[derive(Default)]
struct ModeCreateDumb {
  height: u32,
  width: u32,
  bpp: u32,
  flags: u32,
  handle: u32,
  pitch: u32,
  size: u64,
}

#[repr(C)]
#[derive(Default)]
struct ModeMapDumb {
  handle: u32,
  pad: u32,
  offset: u64,
}

#[repr(C)]
#[derive(Default)]
struct ModeFbCmd {
  fb_id: u32,
  width: u32,
  height: u32,
  pitch: u32,
  bpp: u32,
  depth: u32,
  handle: u32,
}

#[repr(C)]
#[derive(Default)]
struct ModeDestroyDumb {
  handle: u32,
}

#[repr(C)]
struct GemClose {
  handle: u32,
  pad: u32,
}

#[repr(C)]
#[derive(Default)]
struct PrimeHandle {
  handle: u32,
  flags: u32,
  fd: i32,
}

#[repr(C)]
#[derive(Default)]
struct ModeFbCmd2 {
  fb_id: u32,
  width: u32,
  height: u32,
  pixel_format: u32,
  flags: u32,
  handles: [u32; 4],
  pitches: [u32; 4],
  offsets: [u32; 4],
  modifier: [u64; 4],
}

#[repr(C)]
struct ModeCrtc {
  set_connectors_ptr: u64,
  count_connectors: u32,
  crtc_id: u32,
  fb_id: u32,
  x: u32,
  y: u32,
  gamma_size: u32,
  mode_valid: u32,
  mode: ModeInfo,
}
impl Default for ModeCrtc {
  fn default() -> Self { unsafe { std::mem::zeroed() } }
}

const DRM_MODE_CONNECTED: u32 = 1;

pub struct DriBackend {
  fd: i32,
  /// Dual scanout buffers.  We always draw into `bufs[1 - front]`, then flip
  /// with SetCrtc so the front buffer is never rewritten mid-scanout (the
  /// single-buffer path tore / flickered on every software-cursor move).
  bufs: [ScanoutBuf; 2],
  front: usize,
  width: u32,
  height: u32,
  connector_id: u32,
  crtc_id: u32,
  mode: ModeInfo,
  saved_crtc: ModeCrtc,
  active: bool,
  master: bool,
  vt: VtSession,
  input: Option<EvdevInput>,
  /// `dev_t` of the render node (`/dev/dri/renderD*`) that belongs to the very
  /// card this backend drives, or `None` when the card exposes no usable render
  /// node.  Advertised to clients as `zwp_linux_dmabuf_v1.main_device`.
  render_dev: Option<u64>,
  direct: Option<ImportedScanout>,
  #[cfg(feature = "gpu")]
  gpu: Option<super::gpu::GpuComposer>,
  #[cfg(feature = "gpu")]
  gpu_fb: u32,
}

#[derive(Clone, Copy, PartialEq, Eq)]
struct ImportKey {
  dev: u64,
  ino: u64,
  width: u32,
  height: u32,
  stride: u32,
  offset: u32,
  format: u32,
}

struct ImportedScanout {
  key: ImportKey,
  fb_id: u32,
  handle: u32,
}

impl ImportedScanout {
  unsafe fn release(self, fd: RawFd) {
    remove_fb(fd, self.fb_id);
    close_gem(fd, self.handle);
  }
}

struct ScanoutBuf {
  map: *mut u8,
  map_len: usize,
  pitch: u32,
  fb_id: u32,
  handle: u32,
}

impl ScanoutBuf {
  fn empty() -> Self {
    Self {
      map: std::ptr::null_mut(),
      map_len: 0,
      pitch: 0,
      fb_id: 0,
      handle: 0,
    }
  }

  unsafe fn release(&mut self, fd: RawFd) {
    if !self.map.is_null() {
      munmap(self.map as *mut c_void, self.map_len);
      self.map = std::ptr::null_mut();
    }
    remove_fb(fd, self.fb_id);
    destroy_dumb(fd, self.handle);
    self.fb_id = 0;
    self.handle = 0;
    self.map_len = 0;
    self.pitch = 0;
  }
}

/// Resolve the render node that belongs to the card behind `card_fd`.
///
/// The card is identified by its own `dev_t` rather than by a hard-coded name:
/// on a multi-GPU machine `/dev/dri/card1` pairs with `renderD129`, not with
/// `renderD128`, and pointing clients at the wrong GPU is precisely what makes
/// Mesa's Wayland dmabuf path allocate nothing and hand a NULL image to
/// `create_wl_buffer()`.
///
/// `/sys/dev/char/<major>:<minor>/device/drm/` lists every node of the physical
/// device, so the render sibling is found there and then verified by opening it
/// — an unopenable node is worse than no node at all.
fn render_node_of(card_fd: i32) -> Option<u64> {
  let mut st: libc::stat = unsafe { std::mem::zeroed() };
  if unsafe { libc::fstat(card_fd, &mut st) } != 0 {
    return None;
  }
  let (major, minor) = (libc::major(st.st_rdev), libc::minor(st.st_rdev));
  let drm_dir = format!("/sys/dev/char/{}:{}/device/drm", major, minor);
  let entries = std::fs::read_dir(&drm_dir).ok()?;
  for entry in entries.flatten() {
    let name = entry.file_name().to_string_lossy().into_owned();
    if !name.starts_with("renderD") {
      continue;
    }
    let path = format!("/dev/dri/{}", name);
    if let Some(dev) = probe_render_node(&path) {
      eprintln!("[luna-compositor] dri: dmabuf main_device = {}", path);
      return Some(dev);
    }
  }
  eprintln!("[luna-compositor] dri: no usable render node under {}; dmabuf disabled", drm_dir);
  None
}

impl DriBackend {
  pub fn open_any(tty: Option<&str>, with_input: bool) -> Option<Self> {
    for n in 0..4 {
      let path = format!("/dev/dri/card{}", n);
      if let Some(b) = Self::open(&path, tty, with_input) {
        return Some(b);
      }
    }
    None
  }

  pub fn open(path: &str, tty: Option<&str>, with_input: bool) -> Option<Self> {
    let cpath = CString::new(path).ok()?;
    let fd = unsafe { open(cpath.as_ptr(), O_RDWR | O_CLOEXEC) };
    if fd < 0 {
      eprintln!("[luna-compositor] dri: open({}) failed: {}", path, std::io::Error::last_os_error());
      return None;
    }
    let vt = match VtSession::open(tty) {
      Ok(vt) => vt,
      Err(e) => {
        eprintln!("[luna-compositor] vt: failed to acquire {}: {}", tty.unwrap_or("/dev/tty"), e);
        unsafe { libc::close(fd) };
        return None;
      }
    };
    let master = unsafe { ioctl(fd, io(0x1e)) } == 0;
    if !master {
      eprintln!("[luna-compositor] dri: failed to acquire DRM master: {}", std::io::Error::last_os_error());
      unsafe { libc::close(fd) };
      return None;
    }
    let input = if with_input {
      match EvdevInput::start() {
        Ok(input) => Some(input),
        Err(e) => {
          eprintln!("[luna-compositor] input disabled: {}", e);
          None
        }
      }
    } else {
      None
    };
    match unsafe { Self::setup(fd, vt, input) } {
      Some(b) => {
        eprintln!(
          "[luna-compositor] dri: {} ready ({}x{}, pitch={}, double-buf)",
          path, b.width, b.height, b.bufs[0].pitch
        );
        Some(b)
      }
      None => {
        eprintln!(
          "[luna-compositor] dri: {} open ok but mode set failed \
                     (another compositor may hold DRM master)",
          path
        );
        unsafe { libc::close(fd) };
        None
      }
    }
  }

  unsafe fn setup(fd: i32, vt: VtSession, input: Option<EvdevInput>) -> Option<Self> {
    let mut res = ModeCardRes::default();
    if ioctl(fd, iowr::<ModeCardRes>(0xA0), &mut res) != 0 {
      return None;
    }
    let mut connectors = vec![0u32; res.count_connectors as usize];
    let mut encoders = vec![0u32; res.count_encoders as usize];
    let mut crtcs = vec![0u32; res.count_crtcs as usize];
    res.connector_id_ptr = connectors.as_mut_ptr() as u64;
    res.encoder_id_ptr = encoders.as_mut_ptr() as u64;
    res.crtc_id_ptr = crtcs.as_mut_ptr() as u64;
    res.fb_id_ptr = 0;
    if ioctl(fd, iowr::<ModeCardRes>(0xA0), &mut res) != 0 {
      return None;
    }

    for &cid in &connectors {
      let mut conn = ModeGetConnector::default();
      conn.connector_id = cid;
      if ioctl(fd, iowr::<ModeGetConnector>(0xA7), &mut conn) != 0 {
        continue;
      }
      if conn.connection != DRM_MODE_CONNECTED || conn.count_modes == 0 {
        continue;
      }
      let mut modes = vec![ModeInfo::default(); conn.count_modes as usize];
      let mut conn_encs = vec![0u32; conn.count_encoders as usize];
      let mut props = vec![0u32; conn.count_props as usize];
      let mut prop_vals = vec![0u64; conn.count_props as usize];
      conn.modes_ptr = modes.as_mut_ptr() as u64;
      conn.encoders_ptr = conn_encs.as_mut_ptr() as u64;
      conn.props_ptr = props.as_mut_ptr() as u64;
      conn.prop_values_ptr = prop_vals.as_mut_ptr() as u64;
      if ioctl(fd, iowr::<ModeGetConnector>(0xA7), &mut conn) != 0 {
        continue;
      }
      let mode = modes[0];

      /* Resolve encoder + CRTC.  conn.encoder_id is the currently-active
       * encoder (0 on a fresh boot before any compositor has run).  When it
       * is 0, or when the active encoder has no current CRTC, fall back to
       * searching conn_encs: try each encoder and pick the first CRTC its
       * possible_crtcs bitmask allows. */
      let crtc_id: u32;
      {
        let mut found: Option<u32> = None;
        let mut enc_candidates: Vec<u32> = Vec::new();
        if conn.encoder_id != 0 {
          enc_candidates.push(conn.encoder_id);
        }
        for &eid in &conn_encs {
          if eid != conn.encoder_id {
            enc_candidates.push(eid);
          }
        }
        'outer: for eid in enc_candidates {
          let mut enc = ModeGetEncoder::default();
          enc.encoder_id = eid;
          if ioctl(fd, iowr::<ModeGetEncoder>(0xA6), &mut enc) != 0 {
            continue;
          }
          if enc.crtc_id != 0 {
            found = Some(enc.crtc_id);
            break 'outer;
          }
          for (j, &crtc_candidate) in crtcs.iter().enumerate() {
            if enc.possible_crtcs & (1u32 << j) != 0 {
              found = Some(crtc_candidate);
              break 'outer;
            }
          }
        }
        match found {
          Some(id) => crtc_id = id,
          None => continue,
        }
      }
      let mut saved_crtc = ModeCrtc::default();
      saved_crtc.crtc_id = crtc_id;
      /* Best-effort save; on a fresh boot there may be nothing to restore. */
      let _ = ioctl(fd, iowr::<ModeCrtc>(0xA1), &mut saved_crtc);

      let w = mode.hdisplay as u32;
      let h = mode.vdisplay as u32;

      let mut bufs = [ScanoutBuf::empty(), ScanoutBuf::empty()];
      let mut ok = true;
      for buf in &mut bufs {
        let mut create = ModeCreateDumb::default();
        create.width = w;
        create.height = h;
        create.bpp = 32;
        if ioctl(fd, iowr::<ModeCreateDumb>(0xB2), &mut create) != 0 {
          ok = false;
          break;
        }

        let mut fbcmd = ModeFbCmd::default();
        fbcmd.width = w;
        fbcmd.height = h;
        fbcmd.bpp = 32;
        fbcmd.depth = 24;
        fbcmd.pitch = create.pitch;
        fbcmd.handle = create.handle;
        if ioctl(fd, iowr::<ModeFbCmd>(0xAE), &mut fbcmd) != 0 {
          destroy_dumb(fd, create.handle);
          ok = false;
          break;
        }

        let mut mapreq = ModeMapDumb::default();
        mapreq.handle = create.handle;
        if ioctl(fd, iowr::<ModeMapDumb>(0xB3), &mut mapreq) != 0 {
          remove_fb(fd, fbcmd.fb_id);
          destroy_dumb(fd, create.handle);
          ok = false;
          break;
        }
        let ptr = mmap(
          std::ptr::null_mut(),
          create.size as usize,
          PROT_READ | PROT_WRITE,
          MAP_SHARED,
          fd,
          mapreq.offset as i64,
        );
        if ptr == MAP_FAILED {
          remove_fb(fd, fbcmd.fb_id);
          destroy_dumb(fd, create.handle);
          ok = false;
          break;
        }
        buf.map = ptr as *mut u8;
        buf.map_len = create.size as usize;
        buf.pitch = create.pitch;
        buf.fb_id = fbcmd.fb_id;
        buf.handle = create.handle;
      }
      if !ok {
        unsafe {
          bufs[0].release(fd);
          bufs[1].release(fd);
        }
        continue;
      }

      let mut crtc = ModeCrtc::default();
      crtc.crtc_id = crtc_id;
      crtc.fb_id = bufs[0].fb_id;
      crtc.mode = mode;
      crtc.mode_valid = 1;
      crtc.count_connectors = 1;
      crtc.set_connectors_ptr = &cid as *const u32 as u64;
      if ioctl(fd, iowr::<ModeCrtc>(0xA2), &mut crtc) != 0 {
        unsafe {
          bufs[0].release(fd);
          bufs[1].release(fd);
        }
        continue;
      }

      eprintln!("[luna-compositor] dri: double-buffered scanout {}x{}", w, h);
      #[cfg(feature = "gpu")]
      let gpu = super::gpu::GpuComposer::new(fd, w, h);
      #[cfg(feature = "gpu")]
      eprintln!("[luna-compositor] dri: GPU compositor {}", if gpu.is_some() { "enabled" } else { "unavailable; CPU fallback" });
      return Some(DriBackend {
        fd,
        bufs,
        front: 0,
        width: w,
        height: h,
        connector_id: cid,
        crtc_id,
        mode,
        saved_crtc,
        active: true,
        master: true,
        vt,
        input,
        render_dev: render_node_of(fd),
        direct: None,
        #[cfg(feature = "gpu")]
        gpu,
        #[cfg(feature = "gpu")]
        gpu_fb: 0,
      });
    }
    None
  }
}

impl Backend for DriBackend {
  fn size(&self) -> (u32, u32) { (self.width, self.height) }

  fn present(&mut self, fb: &Framebuffer) {
    if !self.active {
      return;
    }
    #[cfg(feature = "gpu")]
    if self.present_gpu(fb) {
      return;
    }
    // Draw into the back buffer, then flip.  Never rewrite the front buffer
    // while the CRTC is scanning it out (that was the window flicker).
    let back = 1 - self.front;
    let copy_w = fb.width.min(self.width) as usize;
    let copy_h = fb.height.min(self.height) as usize;
    let src_stride = fb.width as usize;
    let pitch = self.bufs[back].pitch as usize;
    let dst_base = self.bufs[back].map;
    for y in 0..copy_h {
      let dst = unsafe { dst_base.add(y * pitch) as *mut u32 };
      let src = &fb.pixels[y * src_stride..y * src_stride + copy_w];
      unsafe {
        std::ptr::copy_nonoverlapping(src.as_ptr(), dst, copy_w);
      }
    }
    if unsafe { self.flip_to(back) } {
      self.front = back;
      if let Some(old) = self.direct.take() {
        unsafe { old.release(self.fd) };
      }
    }
  }

  fn present_dmabuf(&mut self, buf: &ShmBuffer) -> bool {
    if !self.active || buf.width != self.width as i32 || buf.height != self.height as i32 {
      return false;
    }
    let Some(dma_fd) = buf.dmabuf_fd() else { return false };
    let Some(key) = import_key(dma_fd, buf) else { return false };
    if self.direct.as_ref().map(|d| d.key) == Some(key) {
      return true;
    }
    let Some(imported) = (unsafe { import_scanout(self.fd, dma_fd, buf, key) }) else {
      return false;
    };
    if !unsafe { self.flip_fb(imported.fb_id) } {
      unsafe { imported.release(self.fd) };
      return false;
    }
    if let Some(old) = self.direct.replace(imported) {
      unsafe { old.release(self.fd) };
    }
    true
  }

  fn present_dmabufs(&mut self, surfaces: &[(i32, i32, ShmBuffer)], bitmap: Option<super::GpuBitmap<'_>>) -> bool {
    #[cfg(feature = "gpu")]
    {
      let Some(output) = self.gpu.as_mut().and_then(|g| g.render_dmabufs(surfaces, bitmap)) else { return false };
      return self.commit_gpu_output(output.1, output.2);
    }
    #[cfg(not(feature = "gpu"))]
    { let _ = surfaces; false }
  }

  fn take_input_channel(&mut self) -> Option<(mpsc::Receiver<InputEvent>, RawFd)> { self.input.as_mut().and_then(EvdevInput::take_channel) }

  fn drm_render_device(&self) -> Option<u64> { self.render_dev }

  fn deactivate(&mut self) {
    if !self.active {
      return;
    }
    if let Some(input) = &self.input {
      input.set_active(false);
    }
    unsafe {
      self.restore_crtc();
      if self.master {
        ioctl(self.fd, io(0x1f));
        self.master = false;
      }
    }
    self.active = false;
    self.vt.release();
    eprintln!("[luna-compositor] VT released");
  }

  fn activate(&mut self) {
    if self.active {
      return;
    }
    self.vt.acknowledge_acquire();
    unsafe {
      self.master = ioctl(self.fd, io(0x1e)) == 0;
      if self.master && self.set_luna_crtc() {
        self.active = true;
      }
    }
    if self.active {
      if let Some(old) = self.direct.take() {
        unsafe { old.release(self.fd) };
      }
      if let Some(input) = &self.input {
        input.set_active(true);
      }
      eprintln!("[luna-compositor] VT acquired");
    } else {
      eprintln!("[luna-compositor] failed to resume DRM after VT acquire");
    }
  }

  fn switch_vt(&mut self, vt: u8) {
    if let Err(e) = self.vt.switch_to(vt) {
      eprintln!("[luna-compositor] VT_ACTIVATE({}) failed: {}", vt, e);
    }
  }

  fn shutdown(&mut self) {
    if let Some(input) = &self.input {
      input.set_active(false);
    }
    self.input.take();
  }
}

impl DriBackend {
  #[cfg(feature = "gpu")]
  fn present_gpu(&mut self, fb: &Framebuffer) -> bool {
    let Some((_, handle, stride)) = self.gpu.as_mut().and_then(|g| g.render(fb)) else { return false };
    self.commit_gpu_output(handle, stride)
  }

  #[cfg(feature = "gpu")]
  fn commit_gpu_output(&mut self, handle: u32, stride: u32) -> bool {
    let mut cmd = ModeFbCmd { fb_id: 0, width: self.width, height: self.height,
      pitch: stride, bpp: 32, depth: 24, handle };
    if unsafe { ioctl(self.fd, iowr::<ModeFbCmd>(0xAE), &mut cmd) } != 0 {
      if let Some(g) = self.gpu.as_mut() { g.discard(); }
      return false;
    }
    if !unsafe { self.flip_fb(cmd.fb_id) } {
      unsafe { remove_fb(self.fd, cmd.fb_id) };
      if let Some(g) = self.gpu.as_mut() { g.discard(); }
      return false;
    }
    let old_fb = std::mem::replace(&mut self.gpu_fb, cmd.fb_id);
    if let Some(g) = self.gpu.as_mut() { g.commit(); }
    unsafe { remove_fb(self.fd, old_fb) };
    if let Some(old) = self.direct.take() { unsafe { old.release(self.fd) }; }
    true
  }

  unsafe fn restore_crtc(&mut self) {
    let cid = self.connector_id;
    self.saved_crtc.count_connectors = 1;
    self.saved_crtc.set_connectors_ptr = &cid as *const u32 as u64;
    ioctl(self.fd, iowr::<ModeCrtc>(0xA2), &mut self.saved_crtc);
  }

  unsafe fn set_luna_crtc(&mut self) -> bool {
    self.flip_to(self.front)
  }

  unsafe fn flip_to(&mut self, idx: usize) -> bool {
    self.flip_fb(self.bufs[idx].fb_id)
  }

  unsafe fn flip_fb(&mut self, fb_id: u32) -> bool {
    let cid = self.connector_id;
    let mut crtc = ModeCrtc::default();
    crtc.crtc_id = self.crtc_id;
    crtc.fb_id = fb_id;
    crtc.mode = self.mode;
    crtc.mode_valid = 1;
    crtc.count_connectors = 1;
    crtc.set_connectors_ptr = &cid as *const u32 as u64;
    ioctl(self.fd, iowr::<ModeCrtc>(0xA2), &mut crtc) == 0
  }
}

impl Drop for DriBackend {
  fn drop(&mut self) {
    if let Some(input) = &self.input {
      input.set_active(false);
    }
    unsafe {
      if self.active && self.master {
        self.restore_crtc();
      }
      self.bufs[0].release(self.fd);
      self.bufs[1].release(self.fd);
      if let Some(direct) = self.direct.take() {
        direct.release(self.fd);
      }
      #[cfg(feature = "gpu")]
      remove_fb(self.fd, self.gpu_fb);
      if self.master {
        ioctl(self.fd, io(0x1f));
        self.master = false;
      }
      self.vt.restore();
      libc::close(self.fd);
    }
  }
}

unsafe fn remove_fb(fd: RawFd, fb_id: u32) {
  if fb_id != 0 {
    let mut id = fb_id;
    ioctl(fd, iowr::<u32>(0xAF), &mut id);
  }
}

unsafe fn destroy_dumb(fd: RawFd, handle: u32) {
  if handle != 0 {
    let mut destroy = ModeDestroyDumb { handle };
    ioctl(fd, iowr::<ModeDestroyDumb>(0xB4), &mut destroy);
  }
}

unsafe fn close_gem(fd: RawFd, handle: u32) {
  if handle != 0 {
    let mut close = GemClose { handle, pad: 0 };
    ioctl(fd, iow::<GemClose>(0x09), &mut close);
  }
}

fn import_key(fd: RawFd, buf: &ShmBuffer) -> Option<ImportKey> {
  let mut st: libc::stat = unsafe { std::mem::zeroed() };
  if unsafe { libc::fstat(fd, &mut st) } != 0 {
    return None;
  }
  Some(ImportKey {
    dev: st.st_dev as u64,
    ino: st.st_ino as u64,
    width: buf.width.try_into().ok()?,
    height: buf.height.try_into().ok()?,
    stride: buf.stride.try_into().ok()?,
    offset: buf.offset.try_into().ok()?,
    format: buf.format,
  })
}

unsafe fn import_scanout(card_fd: RawFd, dma_fd: RawFd, buf: &ShmBuffer, key: ImportKey) -> Option<ImportedScanout> {
  let mut prime = PrimeHandle { handle: 0, flags: 0, fd: dma_fd };
  if ioctl(card_fd, iowr::<PrimeHandle>(0x2d), &mut prime) != 0 {
    return None;
  }
  let fourcc = match buf.format {
    FORMAT_ARGB8888 => 0x3432_5241, // AR24
    FORMAT_XRGB8888 => 0x3432_5258, // XR24
    _ => {
      close_gem(card_fd, prime.handle);
      return None;
    }
  };
  let mut fb = ModeFbCmd2::default();
  fb.width = key.width;
  fb.height = key.height;
  fb.pixel_format = fourcc;
  fb.handles[0] = prime.handle;
  fb.pitches[0] = key.stride;
  fb.offsets[0] = key.offset;
  if ioctl(card_fd, iowr::<ModeFbCmd2>(0xB8), &mut fb) != 0 {
    close_gem(card_fd, prime.handle);
    return None;
  }
  eprintln!(
    "[luna-compositor] dri: direct scanout enabled ({}x{}, pitch={})",
    key.width, key.height, key.stride
  );
  Some(ImportedScanout { key, fb_id: fb.fb_id, handle: prime.handle })
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn drm_abi_layouts_match_linux() {
    assert_eq!(std::mem::size_of::<ModeCardRes>(), 64);
    assert_eq!(std::mem::size_of::<ModeCrtc>(), 104);
    assert_eq!(iowr::<ModeCrtc>(0xA2), 0xc068_64a2);
    assert_eq!(std::mem::size_of::<PrimeHandle>(), 12);
    assert_eq!(std::mem::size_of::<ModeFbCmd2>(), 104);
    assert_eq!(iowr::<PrimeHandle>(0x2d), 0xc00c_642d);
    assert_eq!(iowr::<ModeFbCmd2>(0xB8), 0xc068_64b8);
    assert_eq!(iow::<GemClose>(0x09), 0x4008_6409);
  }
}
