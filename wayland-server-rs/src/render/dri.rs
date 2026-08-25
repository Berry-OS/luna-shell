/*
 * Copyright © 2026 Yuichiro Nakada / Project Vespera
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

use super::vt::VtSession;
use super::{monotonic_millis, probe_render_node, Backend, Framebuffer, InputEvent, PresentationFeedback};
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

#[repr(C)]
#[derive(Default)]
struct ModeCrtcPageFlip {
  crtc_id: u32,
  fb_id: u32,
  flags: u32,
  reserved: u32,
  user_data: u64,
}

const DRM_MODE_CONNECTED: u32 = 1;
const DRM_MODE_PAGE_FLIP_EVENT: u32 = 0x01;
const DRM_EVENT_FLIP_COMPLETE: u32 = 0x02;
const DRM_MODE_FB_MODIFIERS: u32 = 1 << 1;

/// Allow two nominal refresh periods for the completion event.  If the event
/// is still missing after that, `poll_presentation` verifies the CRTC's current
/// framebuffer instead of blindly declaring the flip complete.  The old
/// fixed 100 ms timeout produced a very visible 100 ms hitch and could release
/// scanout resources while the kernel still owned them.
const MIN_FLIP_EVENT_GRACE_MS: u64 = 20;
const DRM_MODE_FLAG_INTERLACE: u32 = 1 << 4;
const DRM_MODE_FLAG_DBLSCAN: u32 = 1 << 5;

/// Return the mode cadence with enough precision for wl_output and watchdog
/// pacing.  `vrefresh` is only an integer hint; deriving the value from the
/// pixel clock preserves 59.94/74.97-style modes instead of rounding them to
/// a different client cadence.
fn mode_refresh_millihz(mode: &ModeInfo) -> u32 {
  let mut denominator = (mode.htotal as u64).saturating_mul(mode.vtotal as u64);
  if mode.clock != 0 && denominator != 0 {
    let mut numerator = (mode.clock as u64).saturating_mul(1_000_000);
    if mode.flags & DRM_MODE_FLAG_INTERLACE != 0 {
      numerator = numerator.saturating_mul(2);
    }
    if mode.flags & DRM_MODE_FLAG_DBLSCAN != 0 {
      denominator = denominator.saturating_mul(2);
    }
    if mode.vscan > 1 {
      denominator = denominator.saturating_mul(mode.vscan as u64);
    }
    if denominator != 0 {
      let mhz = (numerator.saturating_add(denominator / 2)) / denominator;
      if mhz != 0 {
        return mhz.min(u32::MAX as u64) as u32;
      }
    }
  }
  if mode.vrefresh >= 10 {
    mode.vrefresh.saturating_mul(1000)
  } else {
    60_000
  }
}

fn flip_event_grace_ms(refresh_millihz: u32) -> u64 {
  // Some legacy drivers expose no usable timing.  Treat implausible values as
  // 60 Hz; using a near-zero refresh here would create a multi-second stall.
  let refresh = if refresh_millihz >= 10_000 { refresh_millihz as u64 } else { 60_000 };
  MIN_FLIP_EVENT_GRACE_MS.max((2_000_000 + refresh - 1) / refresh)
}

/// Scanout resources that may still be live in the CRTC until the pending flip
/// retires.  Releasing them any earlier removes a framebuffer the hardware is
/// reading from.
enum Retired {
  Scanout(ImportedScanout),
  /// Only the GPU composition path hands raw framebuffer ids over.
  #[cfg_attr(not(feature = "gpu"), allow(dead_code))]
  Fb(u32),
}

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
  vt: Option<VtSession>,
  input: Option<EvdevInput>,
  /// `dev_t` of the render node (`/dev/dri/renderD*`) that belongs to the very
  /// card this backend drives, or `None` when the card exposes no usable render
  /// node.  Advertised to clients as `zwp_linux_dmabuf_v1.main_device`.
  render_dev: Option<u64>,
  direct: Option<ImportedScanout>,
  /// Asynchronous page flips replace the blocking SetCrtc that used to run on
  /// every frame.  `flip_pending` gates the next composite so we never draw
  /// into a buffer the CRTC has not finished taking over.
  flip_pending: bool,
  /// `user_data` submitted with the outstanding page flip.  DRM events can be
  /// delivered late, so only the matching completion may clear the wait.
  pending_fb_id: u32,
  flip_started: std::time::Instant,
  use_page_flip: bool,
  /// Page-flip-less hardware uses front-buffer updates after one initial
  /// SetCrtc. This synthetic pending flag keeps wl_callback pacing near the
  /// display refresh rate without running a blocking modeset every frame.
  front_present_pending: bool,
  /// LUNA_PRESENT_TRACE=1: log submit->actual-scanout latency and watchdog
  /// recoveries without putting logging on the normal fast path.
  present_trace: bool,
  retire: Vec<Retired>,
  /// Last content copied into a scanout buffer, kept in ordinary RAM so a
  /// frame can be diffed without reading back write-combined video memory.
  shadow: Vec<u32>,
  /// Horizontal span each row's previous present wrote, as `[x0, x1)` with
  /// `x1 <= x0` meaning "clean".  The back buffer is one frame behind the
  /// front one, so a row must be re-copied over the union of what this frame
  /// and the previous one touched.  Tracking the span rather than a bool is
  /// what keeps a moving pointer or a blinking cursor from pushing a full
  /// scanline — several kilobytes of write-combined video memory — per row.
  row_span_prev: Vec<(u32, u32)>,
  /// Number of upcoming presents that must copy every row (startup, resize and
  /// VT resume, where the scanout buffers hold unknown content).
  full_copies: u8,
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
  modifier: u64,
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
    // Page-flip completions are read straight off this descriptor from the
    // event loop; a blocking read there would stall every client.
    unsafe {
      let flags = libc::fcntl(fd, libc::F_GETFL);
      if flags >= 0 {
        libc::fcntl(fd, libc::F_SETFL, flags | libc::O_NONBLOCK);
      }
    }
    let vt = match VtSession::open(tty) {
      Ok(vt) => Some(vt),
      Err(e) => {
        eprintln!(
          "[luna-compositor] vt: {} ({}); continuing without VT control",
          tty.unwrap_or("/dev/tty"),
          e
        );
        None
      }
    };
    let mut master = unsafe { ioctl(fd, io(0x1e)) } == 0;
    if !master {
      let err = std::io::Error::last_os_error();
      if err.raw_os_error() == Some(libc::EBUSY) {
        for _ in 0..20 {
          std::thread::sleep(std::time::Duration::from_millis(100));
          master = unsafe { ioctl(fd, io(0x1e)) } == 0;
          if master {
            break;
          }
        }
      }
      if !master {
        eprintln!(
          "[luna-compositor] dri: failed to acquire DRM master: {}",
          std::io::Error::last_os_error()
        );
        unsafe { libc::close(fd) };
        return None;
      }
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

  unsafe fn setup(fd: i32, vt: Option<VtSession>, input: Option<EvdevInput>) -> Option<Self> {
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
        flip_pending: false,
        pending_fb_id: 0,
        flip_started: std::time::Instant::now(),
        use_page_flip: true,
        front_present_pending: false,
        present_trace: std::env::var("LUNA_PRESENT_TRACE")
          .map(|v| !v.is_empty() && v != "0" && !v.eq_ignore_ascii_case("false") && !v.eq_ignore_ascii_case("off"))
          .unwrap_or(false),
        retire: Vec::new(),
        shadow: vec![0; (w as usize) * (h as usize)],
        row_span_prev: vec![(0, w); h as usize],
        full_copies: 2,
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

  fn refresh_millihz(&self) -> u32 { mode_refresh_millihz(&self.mode) }

  fn present(&mut self, fb: &Framebuffer) {
    self.present_damage(fb, fb.full_rect());
  }

  fn present_damage(&mut self, fb: &Framebuffer, damage: super::Rect) {
    if !self.active {
      return;
    }
    // Software-composited frames stay on the damage-aware dumb-buffer path.
    // Routing them through present_gpu() used to re-upload the entire
    // framebuffer as a GLES texture on every present — defeating damage
    // tracking and producing the regular console hitch.  GPU acceleration is
    // reserved for present_dmabufs() (zero-copy plane composition).
    // Draw into the back buffer, then flip.  Never rewrite the front buffer
    // while the CRTC is scanning it out (that was the window flicker).
    // If legacy KMS cannot queue page flips, the current front dumb
    // buffer is already scanned out. Updating it in place avoids the very
    // expensive per-frame SetCrtc fallback used previously.
    let back = if self.use_page_flip { 1 - self.front } else { self.front };
    if !self.copy_damaged(fb, back, damage) {
      // Shadow already matched: nothing reached write-combined scanout memory,
      // so a page flip would only stall the pipeline for identical pixels.
      return;
    }
    if !self.use_page_flip {
      self.front_present_pending = true;
      self.flip_started = std::time::Instant::now();
      return;
    }
    if unsafe { self.flip_to(back) } {
      self.front = back;
      if let Some(old) = self.direct.take() {
        self.retire.push(Retired::Scanout(old));
      }
      if !self.flip_pending {
        self.release_retired();
      }
    }
  }

  fn present_dmabuf(&mut self, buf: &ShmBuffer) -> bool {
    // Direct scanout would require another blocking SetCrtc when page flips
    // are unavailable. Fall back to software composition into the persistent
    // front buffer instead.
    if !self.active || !self.use_page_flip || buf.width != self.width as i32 || buf.height != self.height as i32 {
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
    // Scanning out a client buffer leaves our own dumb buffers untouched, so
    // the next software frame cannot trust their contents.
    self.invalidate_shadow();
    if let Some(old) = self.direct.replace(imported) {
      self.retire.push(Retired::Scanout(old));
    }
    if !self.flip_pending {
      self.release_retired();
    }
    true
  }

  fn present_dmabufs(
    &mut self,
    surfaces: &[(i32, i32, ShmBuffer)],
    extra_surface: Option<(i32, i32, &ShmBuffer)>,
    bitmap: Option<super::GpuBitmap<'_>>,
  ) -> bool {
    if !self.use_page_flip { return false; }
    #[cfg(feature = "gpu")]
    {
      let Some(output) = self.gpu.as_mut().and_then(|g| g.render_dmabufs(surfaces, extra_surface, bitmap)) else { return false };
      return self.commit_gpu_output(output.1, output.2);
    }
    #[cfg(not(feature = "gpu"))]
    { let _ = (surfaces, extra_surface, bitmap); false }
  }

  fn can_gpu_compose(&self) -> bool {
    #[cfg(feature = "gpu")]
    {
      self.gpu.is_some()
    }
    #[cfg(not(feature = "gpu"))]
    {
      false
    }
  }

  fn dmabuf_formats(&self) -> &[(u32, u64)] {
    #[cfg(feature = "gpu")]
    {
      self.gpu.as_ref().map(|g| g.dmabuf_formats()).unwrap_or(&[])
    }
    #[cfg(not(feature = "gpu"))]
    {
      &[]
    }
  }

  fn take_input_channel(&mut self) -> Option<(mpsc::Receiver<InputEvent>, RawFd)> { self.input.as_mut().and_then(EvdevInput::take_channel) }

  fn set_keyboard_leds(&self, leds: u8) {
    if let Some(input) = &self.input {
      input.set_leds(leds);
    }
  }

  fn drm_render_device(&self) -> Option<u64> { self.render_dev }

  fn event_fd(&self) -> Option<RawFd> { Some(self.fd) }

  /// Drain page-flip completions.  Reading them is what lets the next frame
  /// start; nothing else consumes this descriptor.  Crucially, this is also
  /// the point at which the server is allowed to emit wl_callback.done and
  /// wl_buffer.release for the submitted frame.
  fn dispatch_events(&mut self) -> Option<PresentationFeedback> {
    let mut buf = [0u8; 512];
    let mut completed = None;
    loop {
      let n = unsafe { libc::read(self.fd, buf.as_mut_ptr() as *mut c_void, buf.len()) };
      if n <= 0 {
        break;
      }
      let n = n as usize;
      let mut off = 0usize;
      while off + 8 <= n {
        let ty = u32::from_ne_bytes([buf[off], buf[off + 1], buf[off + 2], buf[off + 3]]);
        let len =
          u32::from_ne_bytes([buf[off + 4], buf[off + 5], buf[off + 6], buf[off + 7]]) as usize;
        if len < 8 || off + len > n {
          break;
        }
        let user_data = if len >= 16 {
          u64::from_ne_bytes([
            buf[off + 8], buf[off + 9], buf[off + 10], buf[off + 11],
            buf[off + 12], buf[off + 13], buf[off + 14], buf[off + 15],
          ])
        } else {
          0
        };
        if ty == DRM_EVENT_FLIP_COMPLETE
          && self.flip_pending
          && user_data == self.pending_fb_id as u64
        {
          completed = Some(self.complete_flip(false));
        }
        off += len;
      }
      if n < buf.len() {
        break;
      }
    }
    completed
  }

  fn present_busy(&self) -> bool {
    self.flip_pending || self.front_present_pending
  }

  fn poll_presentation(&mut self) -> Option<PresentationFeedback> {
    if self.front_present_pending {
      let refresh = mode_refresh_millihz(&self.mode).max(10_000) as u64;
      let frame_ms = ((1_000_000u64 + refresh - 1) / refresh).max(1);
      if self.flip_started.elapsed() < std::time::Duration::from_millis(frame_ms) {
        return None;
      }
      self.front_present_pending = false;
      return Some(PresentationFeedback { timestamp_ms: monotonic_millis() });
    }
    if !self.flip_pending {
      return None;
    }

    // Two nominal refresh periods tolerate a scheduling slip at vblank.  If
    // the event is still absent, do a non-modesetting GETCRTC query and only
    // declare completion when the kernel confirms the exact pending fb.
    let grace_ms = flip_event_grace_ms(mode_refresh_millihz(&self.mode));
    if self.flip_started.elapsed() < std::time::Duration::from_millis(grace_ms) {
      return None;
    }

    let mut crtc = ModeCrtc::default();
    crtc.crtc_id = self.crtc_id;
    let queried = unsafe { ioctl(self.fd, iowr::<ModeCrtc>(0xA1), &mut crtc) == 0 };
    if queried && crtc.fb_id == self.pending_fb_id {
      return Some(self.complete_flip(true));
    }
    None
  }

  fn deactivate(&mut self) {
    if !self.active {
      return;
    }
    // The CRTC is going away; no completion will ever arrive for a flip that
    // is still in flight, and the scanout buffers come back with unknown
    // contents.
    self.flip_pending = false;
    self.pending_fb_id = 0;
    self.release_retired();
    self.invalidate_shadow();
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
    self.vt.as_ref().map(VtSession::release);
    eprintln!("[luna-compositor] VT released");
  }

  fn activate(&mut self) {
    if self.active {
      return;
    }
    if let Some(vt) = &self.vt {
      vt.acknowledge_acquire();
    }
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
      self.invalidate_shadow();
      if let Some(input) = &self.input {
        input.set_active(true);
      }
      eprintln!("[luna-compositor] VT acquired");
    } else {
      eprintln!("[luna-compositor] failed to resume DRM after VT acquire");
    }
  }

  fn switch_vt(&mut self, vt_no: u8) {
    if let Some(vt) = &self.vt {
      if let Err(e) = vt.switch_to(vt_no) {
        eprintln!("[luna-compositor] VT_ACTIVATE({}) failed: {}", vt_no, e);
      }
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
  /// Retire one page flip after the kernel has confirmed scanout completion.
  /// `watchdog=true` means the event itself was missing and GETCRTC proved the
  /// new framebuffer is already current.
  fn complete_flip(&mut self, watchdog: bool) -> PresentationFeedback {
    let fb_id = self.pending_fb_id;
    let elapsed_ms = self.flip_started.elapsed().as_secs_f64() * 1000.0;
    self.flip_pending = false;
    self.pending_fb_id = 0;
    self.release_retired();

    if self.present_trace {
      eprintln!(
        "[luna-present] complete source={} fb={} latency={:.2}ms",
        if watchdog { "watchdog/GETCRTC" } else { "drm-event" },
        fb_id,
        elapsed_ms,
      );
    }

    PresentationFeedback { timestamp_ms: monotonic_millis() }
  }

  #[cfg(feature = "gpu")]
  #[allow(dead_code)] // Kept for experiments; software presents use copy_damaged.
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
    // GPU composition bypasses the dumb buffers entirely.
    self.invalidate_shadow();
    if old_fb != 0 { self.retire.push(Retired::Fb(old_fb)); }
    if let Some(old) = self.direct.take() { self.retire.push(Retired::Scanout(old)); }
    if !self.flip_pending { self.release_retired(); }
    true
  }

  unsafe fn restore_crtc(&mut self) {
    let cid = self.connector_id;
    self.saved_crtc.count_connectors = 1;
    self.saved_crtc.set_connectors_ptr = &cid as *const u32 as u64;
    ioctl(self.fd, iowr::<ModeCrtc>(0xA2), &mut self.saved_crtc);
  }

  unsafe fn set_luna_crtc(&mut self) -> bool {
    // A VT resume needs a real modeset, not a flip onto a CRTC we just lost.
    self.flip_pending = false;
    self.pending_fb_id = 0;
    self.set_crtc_fb(self.bufs[self.front].fb_id)
  }

  unsafe fn flip_to(&mut self, idx: usize) -> bool {
    self.flip_fb(self.bufs[idx].fb_id)
  }

  /// Hand `fb_id` to the CRTC.  Prefers an asynchronous page flip — SetCrtc
  /// runs the full modeset path and blocks until the change lands, which on
  /// every frame is what made the console session hitch.
  unsafe fn flip_fb(&mut self, fb_id: u32) -> bool {
    // The server gates every composite on present_busy().  If this ever fires,
    // do not fall through to blocking SetCrtc while a real flip still owns the
    // CRTC; simply reject this present and let the completion path wake us.
    if self.flip_pending {
      return false;
    }
    if self.use_page_flip && !self.flip_pending {
      let mut req = ModeCrtcPageFlip {
        crtc_id: self.crtc_id,
        fb_id,
        flags: DRM_MODE_PAGE_FLIP_EVENT,
        reserved: 0,
        user_data: fb_id as u64,
      };
      if ioctl(self.fd, iowr::<ModeCrtcPageFlip>(0xB0), &mut req) == 0 {
        self.flip_pending = true;
        self.pending_fb_id = fb_id;
        self.flip_started = std::time::Instant::now();
        if self.present_trace {
          let refresh_millihz = mode_refresh_millihz(&self.mode);
          eprintln!(
            "[luna-present] submit fb={} refresh={:.3}Hz",
            fb_id,
            refresh_millihz as f64 / 1000.0,
          );
        }
        return true;
      }
      let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
      // Only our own dumb buffers prove the driver cannot page flip at all.
      // An imported client dma-buf may simply be un-flippable (wrong modifier,
      // wrong format); that must not cost every later frame a blocking
      // SetCrtc, so give up on page flipping only for the scanout pair.
      let own = fb_id == self.bufs[0].fb_id || fb_id == self.bufs[1].fb_id;
      if own && err != libc::EBUSY {
        eprintln!(
          "[luna-compositor] dri: page flip unavailable ({}), switching to paced front-buffer updates",
          std::io::Error::from_raw_os_error(err)
        );
        self.use_page_flip = false;
      }
    }
    self.set_crtc_fb(fb_id)
  }

  unsafe fn set_crtc_fb(&mut self, fb_id: u32) -> bool {
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

  /// Release everything that was still on screen when the now-completed flip
  /// was issued.
  fn release_retired(&mut self) {
    for item in self.retire.drain(..) {
      match item {
        Retired::Scanout(s) => unsafe { s.release(self.fd) },
        Retired::Fb(id) => unsafe { remove_fb(self.fd, id) },
      }
    }
  }

  /// Copy `fb` into scanout buffer `back`, skipping scanlines that already
  /// hold the same pixels.  A pointer move or a blinking caret then writes a
  /// handful of rows to write-combined video memory instead of the whole
  /// screen.  Returns whether any scanout memory was written.
  fn copy_damaged(&mut self, fb: &Framebuffer, back: usize, damage: super::Rect) -> bool {
    let copy_w = fb.width.min(self.width) as usize;
    let copy_h = fb.height.min(self.height) as usize;
    if copy_w == 0 || copy_h == 0 {
      return false;
    }
    let src_stride = fb.width as usize;
    let pitch = self.bufs[back].pitch as usize;
    let dst_base = self.bufs[back].map;
    let shadow_stride = self.width as usize;
    let force_all = self.full_copies > 0;
    if force_all {
      self.full_copies -= 1;
    }

    let damage = damage.intersect(&super::Rect::new(0, 0, copy_w as i32, copy_h as i32));
    // Wide damage already names the changed span.  Walking every row looking
    // for leading/trailing equals costs a full-screen memcmp on a wallpaper
    // tick and was visible as the periodic hitch; keep the trim only for the
    // small rects (carets, clocks, cursors) where it actually saves bandwidth.
    let trim_edges = !force_all && !damage.is_empty() && (damage.x1 - damage.x0) <= 256;
    let mut wrote = false;
    for y in 0..copy_h {
      let prev = self.row_span_prev[y];
      let in_damage = y >= damage.y0 as usize && y < damage.y1 as usize;
      // The other dumb buffer can still be one frame behind in a row outside
      // this frame's damage. Visit it only while a previous span is pending.
      if !force_all && !in_damage && prev.1 <= prev.0 {
        continue;
      }
      let src = &fb.pixels[y * src_stride..y * src_stride + copy_w];
      let shadow = &mut self.shadow[y * shadow_stride..y * shadow_stride + copy_w];
      let mut span = (0u32, 0u32);
      if force_all {
        shadow.copy_from_slice(src);
        span = (0, copy_w as u32);
      } else if in_damage {
        let x0 = damage.x0 as usize;
        let x1 = damage.x1 as usize;
        let old = &mut shadow[x0..x1];
        let new = &src[x0..x1];
        if old != new {
          if trim_edges {
            let mut lo = 0usize;
            while old[lo] == new[lo] { lo += 1; }
            let mut hi = new.len();
            while old[hi - 1] == new[hi - 1] { hi -= 1; }
            old[lo..hi].copy_from_slice(&new[lo..hi]);
            span = ((x0 + lo) as u32, (x0 + hi) as u32);
          } else {
            old.copy_from_slice(new);
            span = (x0 as u32, x1 as u32);
          }
        }
      }
      // The back buffer trails the front one by a frame, so it also needs
      // whatever the previous present wrote into this row.
      let (mut x0, mut x1) = span;
      if prev.1 > prev.0 {
        if x1 > x0 {
          x0 = x0.min(prev.0);
          x1 = x1.max(prev.1);
        } else {
          x0 = prev.0;
          x1 = prev.1;
        }
      }
      if force_all {
        x0 = 0;
        x1 = copy_w as u32;
      }
      // A previous span was recorded against a possibly wider framebuffer.
      if x1 > copy_w as u32 {
        x1 = copy_w as u32;
      }
      if x1 > x0 {
        unsafe {
          std::ptr::copy_nonoverlapping(
            src.as_ptr().add(x0 as usize),
            dst_base.add(y * pitch + x0 as usize * 4) as *mut u32,
            (x1 - x0) as usize,
          );
        }
        wrote = true;
      }
      self.row_span_prev[y] = span;
    }
    wrote
  }

  /// Forget what the scanout buffers are believed to contain.
  fn invalidate_shadow(&mut self) {
    self.full_copies = 2;
    let w = self.width;
    for d in self.row_span_prev.iter_mut() {
      *d = (0, w);
    }
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
      if let Some(vt) = self.vt.as_mut() {
        vt.restore();
      }
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
    modifier: buf.dmabuf_modifier()?,
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
  fb.flags = DRM_MODE_FB_MODIFIERS;
  fb.modifier[0] = key.modifier;
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

  #[test]
  fn flip_event_grace_tracks_two_refresh_periods() {
    assert_eq!(flip_event_grace_ms(60_000), 34);
    assert_eq!(flip_event_grace_ms(30_000), 67);
    assert_eq!(flip_event_grace_ms(144_000), MIN_FLIP_EVENT_GRACE_MS);
    assert_eq!(flip_event_grace_ms(0), 34);
  }

  #[test]
  fn mode_refresh_uses_pixel_clock_precision() {
    let mut mode = ModeInfo::default();
    mode.clock = 148_500;
    mode.htotal = 2200;
    mode.vtotal = 1125;
    mode.vrefresh = 59; // prove the timing, not the rounded hint, wins
    assert_eq!(mode_refresh_millihz(&mode), 60_000);

    mode.clock = 148_352;
    assert!((mode_refresh_millihz(&mode) as i64 - 59_940).abs() <= 1);
  }
}
