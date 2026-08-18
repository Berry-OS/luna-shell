/*
 * Copyright © 2026 Yuichiro Nakada / Project Vespera
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

use wayland_server::render::software::SoftwareBackend;
use wayland_server::render::{Backend, Framebuffer};

#[cfg(all(target_os = "linux", feature = "dri"))]
use wayland_server::render::{GpuBitmap, InputEvent, PresentationFeedback, Rect};
#[cfg(all(target_os = "linux", feature = "dri"))]
use wayland_server::shm::ShmBuffer;

/// Keep the existing Server presentation contract synchronous from the
/// caller's point of view while DriBackend still uses asynchronous KMS page
/// flips internally.
///
/// Server emits wl_buffer.release / wl_callback.done immediately after a
/// Backend::present* call returns.  Returning from a raw DriBackend right after
/// DRM_IOCTL_MODE_PAGE_FLIP therefore lets clients start their next frame
/// before the previous one has actually reached vblank.  This wrapper waits
/// for the matching DRM_EVENT_FLIP_COMPLETE (or the guarded GETCRTC recovery)
/// before returning, so those Wayland events are naturally paced to scanout.
#[cfg(all(target_os = "linux", feature = "dri"))]
struct DriPacedBackend {
  inner: wayland_server::render::dri::DriBackend,
}

#[cfg(all(target_os = "linux", feature = "dri"))]
impl DriPacedBackend {
  fn new(inner: wayland_server::render::dri::DriBackend) -> Self {
    Self { inner }
  }

  fn wait_for_scanout(&mut self) {
    while self.inner.present_busy() {
      let mut readable = false;
      if let Some(fd) = self.inner.event_fd() {
        let mut pfd = libc::pollfd {
          fd,
          events: libc::POLLIN,
          revents: 0,
        };
        // A normal page flip wakes this immediately at vblank.  A short
        // timeout also gives DriBackend::poll_presentation() a chance to run
        // if a driver loses/delays the flip event.
        let rc = unsafe { libc::poll(&mut pfd, 1, 4) };
        readable = rc > 0 && (pfd.revents & libc::POLLIN) != 0;
      } else {
        std::thread::sleep(std::time::Duration::from_millis(1));
      }

      if readable {
        let _ = self.inner.dispatch_events();
      }
      if self.inner.present_busy() {
        let _ = self.inner.poll_presentation();
      }
    }
  }
}

#[cfg(all(target_os = "linux", feature = "dri"))]
impl Backend for DriPacedBackend {
  fn size(&self) -> (u32, u32) {
    self.inner.size()
  }

  fn present(&mut self, fb: &Framebuffer) {
    self.inner.present(fb);
    self.wait_for_scanout();
  }

  fn present_damage(&mut self, fb: &Framebuffer, damage: Rect) {
    self.inner.present_damage(fb, damage);
    self.wait_for_scanout();
  }

  fn present_dmabuf(&mut self, buf: &ShmBuffer) -> bool {
    let ok = self.inner.present_dmabuf(buf);
    if ok {
      self.wait_for_scanout();
    }
    ok
  }

  fn present_dmabufs(
    &mut self,
    surfaces: &[(i32, i32, ShmBuffer)],
    bitmap: Option<GpuBitmap<'_>>,
  ) -> bool {
    let ok = self.inner.present_dmabufs(surfaces, bitmap);
    if ok {
      self.wait_for_scanout();
    }
    ok
  }

  fn can_gpu_compose(&self) -> bool {
    self.inner.can_gpu_compose()
  }

  fn take_input_channel(
    &mut self,
  ) -> Option<(
    std::sync::mpsc::Receiver<InputEvent>,
    std::os::unix::io::RawFd,
  )> {
    self.inner.take_input_channel()
  }

  fn set_keyboard_leds(&self, leds: u8) {
    self.inner.set_keyboard_leds(leds);
  }

  fn event_fd(&self) -> Option<std::os::unix::io::RawFd> {
    // Normally no completion remains pending after present* returns, but keep
    // forwarding the descriptor so any late/stale kernel event can still be
    // drained by the main loop.
    self.inner.event_fd()
  }

  fn dispatch_events(&mut self) -> Option<PresentationFeedback> {
    self.inner.dispatch_events()
  }

  fn poll_presentation(&mut self) -> Option<PresentationFeedback> {
    self.inner.poll_presentation()
  }

  fn present_busy(&self) -> bool {
    self.inner.present_busy()
  }

  fn deactivate(&mut self) {
    self.inner.deactivate();
  }

  fn activate(&mut self) {
    self.inner.activate();
  }

  fn switch_vt(&mut self, vt: u8) {
    self.inner.switch_vt(vt);
  }

  fn shutdown(&mut self) {
    self.inner.shutdown();
  }

  fn drm_render_device(&self) -> Option<u64> {
    self.inner.drm_render_device()
  }
}

fn main() {
  let mut socket = "wayland-1".to_string();
  let mut backend_kind = "software".to_string();
  let mut screenshot: Option<String> = None;
  let mut fbdev: Option<String> = None;
  let mut width = 1280u32;
  let mut height = 720u32;
  let mut port = 8081u16;
  let mut tty: Option<String> = None;
  let mut with_input = true;

  let mut args = std::env::args().skip(1);
  while let Some(a) = args.next() {
    match a.as_str() {
      "--socket" => socket = args.next().unwrap_or(socket),
      "--backend" => backend_kind = args.next().unwrap_or(backend_kind),
      "--screenshot" => screenshot = args.next(),
      "--fbdev" => fbdev = args.next(),
      "--tty" => tty = args.next(),
      "--no-input" => with_input = false,
      "--port" => {
        if let Some(p) = args.next() {
          port = p.parse().unwrap_or(port);
        }
      }
      "--size" => {
        if let Some(s) = args.next() {
          if let Some((w, h)) = s.split_once('x') {
            width = w.parse().unwrap_or(width);
            height = h.parse().unwrap_or(height);
          }
        }
      }
      "-h" | "--help" => {
        eprintln!(
          "luna-compositor [--socket NAME] [--backend software|dri|webgl] \
                     [--screenshot PATH] [--fbdev DEV] [--size WxH] [--port PORT] \
                     [--tty /dev/ttyN] [--no-input]"
        );
        return;
      }
      _ => eprintln!("[luna-compositor] unknown argument: {}", a),
    }
  }
  #[cfg(not(all(target_os = "linux", feature = "dri")))]
  let _ = (&tty, with_input);

  let backend: Box<dyn Backend> = match backend_kind.as_str() {
    #[cfg(all(target_os = "linux", feature = "dri"))]
    "dri" => match wayland_server::render::dri::DriBackend::open_any(tty.as_deref(), with_input) {
      Some(b) => {
        eprintln!("[luna-compositor] using DRI (DRM/KMS) backend with vblank pacing");
        Box::new(DriPacedBackend::new(b))
      }
      None => {
        eprintln!("[luna-compositor] DRI init failed, falling back to software{}", if fbdev.is_some() { " + fbdev" } else { " (no --fbdev: nothing will appear on screen)" });
        Box::new(make_software(width, height, screenshot.clone(), fbdev.clone()))
      }
    },
    #[cfg(not(all(target_os = "linux", feature = "dri")))]
    "dri" => {
      eprintln!(
        "[luna-compositor] dri backend requires rebuild with feature=\"dri\"; \
                 using software"
      );
      Box::new(make_software(width, height, screenshot.clone(), fbdev.clone()))
    }
    #[cfg(feature = "webgl")]
    "webgl" => {
      eprintln!("[luna-compositor] using WebGL browser streaming backend");
      Box::new(wayland_server::render::webgl_server::WebGlServerBackend::new(width, height, port))
    }
    #[cfg(not(feature = "webgl"))]
    "webgl" => {
      eprintln!(
        "[luna-compositor] webgl backend requires rebuild with feature=\"webgl\" \
                 (cargo build --features webgl); using software"
      );
      Box::new(make_software(width, height, screenshot.clone(), fbdev.clone()))
    }
    _ => {
      if fbdev.is_none() && screenshot.is_none() {
        eprintln!(
          "[luna-compositor] software backend with no --fbdev/--screenshot: \
                     frames are composed but not shown on a display"
        );
      }
      Box::new(make_software(width, height, screenshot.clone(), fbdev.clone()))
    }
  };

  if let Err(e) = wayland_server::run(&socket, backend) {
    eprintln!("[luna-compositor] failed to start: {}", e);
    std::process::exit(1);
  }
}

fn make_software(w: u32, h: u32, screenshot: Option<String>, fbdev: Option<String>) -> SoftwareBackend {
  let mut b = SoftwareBackend::new(w, h);
  if let Some(p) = screenshot {
    b = b.with_screenshot(p);
  }
  if let Some(dev) = fbdev {
    b = b.with_fbdev(&dev);
  }
  b
}
