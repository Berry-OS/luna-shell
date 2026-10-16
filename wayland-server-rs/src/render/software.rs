/*
 * Copyright © 2026 Yuichiro Nakada / Project Vespera
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

use super::{Backend, Framebuffer};
use std::fs::File;
use std::io::Write;

pub struct SoftwareBackend {
  width: u32,
  height: u32,
  pub screenshot_path: Option<String>,
  frame: u64,
  fbdev: Option<File>,
  fb_bpp: u32,
  /// Test aid: compose with the GPU on a card that needs no DRM master and
  /// write the result as the screenshot (`LUNA_HEADLESS_GPU=/dev/dri/cardN`).
  #[cfg(all(target_os = "linux", feature = "gpu"))]
  gpu: Option<super::gpu::GpuComposer>,
  #[cfg(all(target_os = "linux", feature = "gpu"))]
  render_dev: Option<u64>,
}

impl SoftwareBackend {
  pub fn new(width: u32, height: u32) -> Self {
    SoftwareBackend {
      width,
      height,
      screenshot_path: None,
      frame: 0,
      fbdev: None,
      fb_bpp: 32,
      #[cfg(all(target_os = "linux", feature = "gpu"))]
      gpu: None,
      #[cfg(all(target_os = "linux", feature = "gpu"))]
      render_dev: None,
    }
  }

  #[cfg(all(target_os = "linux", feature = "gpu"))]
  pub fn with_headless_gpu(mut self, card: &str) -> Self {
    use std::os::unix::io::IntoRawFd;
    // A session that forces software GL (LIBGL_ALWAYS_SOFTWARE, as luna-session's
    // browser launchers do) makes Mesa's GBM surface swap call a NULL pointer.
    for var in ["LIBGL_ALWAYS_SOFTWARE", "MESA_LOADER_DRIVER_OVERRIDE", "GALLIUM_DRIVER"] {
      // SAFETY: done once at startup, before any other thread exists.
      unsafe { std::env::remove_var(var) };
    }
    match File::options().read(true).write(true).open(card) {
      Ok(f) => {
        use std::os::unix::io::AsRawFd;
        self.render_dev = super::dri::render_node_of(f.as_raw_fd());
        self.gpu = super::gpu::GpuComposer::new(f.into_raw_fd(), self.width, self.height);
        eprintln!("[luna-compositor] headless GPU composition on {}: {}", card, if self.gpu.is_some() { "ready" } else { "unavailable" });
      }
      Err(e) => eprintln!("[luna-compositor] headless GPU: cannot open {}: {}", card, e),
    }
    self
  }

  pub fn with_screenshot(mut self, path: impl Into<String>) -> Self {
    self.screenshot_path = Some(path.into());
    self
  }

  pub fn with_fbdev(mut self, path: &str) -> Self {
    match File::options().write(true).open(path) {
      Ok(f) => {
        eprintln!("[luna-compositor] software: writing frames to {}", path);
        self.fbdev = Some(f);
      }
      Err(e) => {
        eprintln!("[luna-compositor] software: cannot open fbdev {}: {}", path, e);
      }
    }
    self
  }

  fn write_ppm(&self, fb: &Framebuffer, path: &str) {
    let mut out = match File::create(path) {
      Ok(f) => std::io::BufWriter::new(f),
      Err(_) => return,
    };
    let _ = write!(out, "P6\n{} {}\n255\n", fb.width, fb.height);
    let mut row = Vec::with_capacity((fb.width * 3) as usize);
    for &px in &fb.pixels {
      row.push(((px >> 16) & 0xff) as u8);
      row.push(((px >> 8) & 0xff) as u8);
      row.push((px & 0xff) as u8);
    }
    let _ = out.write_all(&row);
  }

  fn write_fbdev(&mut self, fb: &Framebuffer) {
    let Some(f) = self.fbdev.as_mut() else { return };
    // Typical fbdev is 32bpp BGRA/XRGB; write 0xAARRGGBB little-endian.
    if self.fb_bpp == 32 {
      let mut bytes = Vec::with_capacity(fb.pixels.len() * 4);
      for &px in &fb.pixels {
        bytes.extend_from_slice(&px.to_le_bytes());
      }
      use std::io::Seek;
      let _ = f.seek(std::io::SeekFrom::Start(0));
      let _ = f.write_all(&bytes);
    }
  }
}

impl Backend for SoftwareBackend {
  fn size(&self) -> (u32, u32) { (self.width, self.height) }

  #[cfg(all(target_os = "linux", feature = "gpu"))]
  fn can_gpu_compose(&self) -> bool {
    self.gpu.is_some()
  }

  // GPU-only client buffers (dma-bufs) are read back through GL, as on the DRM backend.
  #[cfg(all(target_os = "linux", feature = "gpu"))]
  fn blit_buffer(&mut self, fb: &mut Framebuffer, buf: &crate::shm::ShmBuffer, x: i32, y: i32) {
    if buf.is_cpu_linear() && buf.upload_ptr().is_some() {
      fb.blit_shm(buf, x, y);
      return;
    }
    let clip = fb.clip();
    let need = super::Rect::new(clip.x0 - x, clip.y0 - y, clip.x1 - x, clip.y1 - y)
      .intersect(&super::Rect::new(0, 0, buf.width, buf.height));
    if let Some(pixels) = self.gpu.as_mut().and_then(|g| g.readback(buf, need)) {
      fb.blit_pixels_region(pixels, buf.width, buf.height, x, y, buf.format == crate::shm::FORMAT_XRGB8888, need);
    }
  }

  // Advertise the card's render node and importable formats so real EGL
  // clients hand over dma-bufs, as they do on the DRM backend.
  #[cfg(all(target_os = "linux", feature = "gpu"))]
  fn dmabuf_formats(&self) -> &[(u32, u64)] {
    self.gpu.as_ref().map(|g| g.dmabuf_formats()).unwrap_or(&[])
  }

  #[cfg(all(target_os = "linux", feature = "gpu"))]
  fn drm_render_device(&self) -> Option<u64> {
    self.render_dev
  }

  #[cfg(all(target_os = "linux", feature = "gpu"))]
  fn present_dmabufs(
    &mut self,
    surfaces: &[(i32, i32, crate::shm::ShmBuffer)],
    extra_surface: Option<(i32, i32, &crate::shm::ShmBuffer)>,
    bitmap: Option<super::GpuBitmap<'_>>,
  ) -> bool {
    let (w, h) = (self.width, self.height);
    let Some(gpu) = self.gpu.as_mut() else { return false };
    let Some(out) = gpu.render_dmabufs(surfaces, extra_surface, bitmap) else { return false };
    let pixels = gpu.read_output(&out, w, h);
    gpu.commit();
    let Some(pixels) = pixels else { return false };
    self.frame += 1;
    if let Some(path) = self.screenshot_path.clone() {
      let fb = Framebuffer { width: w, height: h, pixels, ..Framebuffer::new(0, 0) };
      self.write_ppm(&fb, &path);
    }
    true
  }

  fn present(&mut self, fb: &Framebuffer) {
    self.frame += 1;
    if let Some(path) = self.screenshot_path.clone() {
      self.write_ppm(fb, &path);
    }
    self.write_fbdev(fb);
  }
}
