/*
 * Copyright © 2026 Yuichiro Nakada / Project Vespera
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */


pub mod software;

#[cfg(all(target_os = "linux", feature = "dri"))]
pub mod dri;

#[cfg(all(target_os = "linux", feature = "gpu"))]
pub mod gpu;

#[cfg(all(target_os = "linux", feature = "dri"))]
pub mod vt;

#[cfg(target_arch = "wasm32")]
pub mod webgl;

#[cfg(all(not(target_arch = "wasm32"), feature = "webgl"))]
pub mod webgl_server;

use crate::shm::{ShmBuffer, FORMAT_XRGB8888};

pub struct GpuBitmap<'a> { pub x:i32, pub y:i32, pub width:u32, pub height:u32, pub pixels:&'a [u32] }

/// `stat()` + `open()` a DRM render node, returning its `dev_t` when both work.
///
/// A node that exists but cannot be opened is reported as unusable: handing its
/// `dev_t` to clients as `main_device` sends Mesa down a path it cannot finish.
#[cfg(not(target_arch = "wasm32"))]
pub fn probe_render_node(path: &str) -> Option<u64> {
    let cpath = std::ffi::CString::new(path).ok()?;
    let mut st: libc::stat = unsafe { std::mem::zeroed() };
    if unsafe { libc::stat(cpath.as_ptr(), &mut st) } != 0 {
        return None;
    }
    let fd = unsafe { libc::open(cpath.as_ptr(), libc::O_RDWR | libc::O_CLOEXEC) };
    if fd < 0 {
        eprintln!(
            "[luna-compositor] {} exists but cannot be opened: {}",
            path,
            std::io::Error::last_os_error()
        );
        return None;
    }
    unsafe { libc::close(fd) };
    Some(st.st_rdev as u64)
}

pub struct Framebuffer {
    pub width: u32,
    pub height: u32,
    pub pixels: Vec<u32>,
}

impl Framebuffer {
    pub fn new(width: u32, height: u32) -> Self {
        Framebuffer {
            width,
            height,
            pixels: vec![0xff10_1014; (width * height) as usize],
        }
    }

    pub fn clear(&mut self, argb: u32) {
        for p in self.pixels.iter_mut() {
            *p = argb;
        }
    }

    /// Blit SHM/dmabuf with clipping; dmabuf triggers DMA_BUF_IOCTL_SYNC around CPU reads.
    pub fn blit_shm(&mut self, buf: &ShmBuffer, dx: i32, dy: i32) {
        let src_x0 = (-dx).max(0);
        let src_y0 = (-dy).max(0);
        let dst_x0 = dx.max(0) as u32;
        let dst_y0 = dy.max(0) as u32;
        let copy_w = (buf.width - src_x0)
            .min(self.width as i32 - dst_x0 as i32)
            .max(0) as u32;
        let copy_h = (buf.height - src_y0)
            .min(self.height as i32 - dst_y0 as i32)
            .max(0) as u32;
        if copy_w == 0 || copy_h == 0 {
            return;
        }

        buf.begin_cpu_read();

        let stride = buf.stride as usize;
        let base_src = buf.offset + src_y0 as usize * stride + src_x0 as usize * 4;
        let opaque = buf.format == FORMAT_XRGB8888;

        for row in 0..copy_h as usize {
            let src_off = base_src + row * stride;
            let dst_start =
                (dst_y0 as usize + row) * self.width as usize + dst_x0 as usize;
            if let Some(src_row) = buf.pool.slice(src_off, copy_w as usize * 4) {
                let dst_row =
                    &mut self.pixels[dst_start..dst_start + copy_w as usize];
                if opaque {
                    // XRGB: the byte order already matches native-endian
                    // 0xAARRGGBB on supported little-endian targets.  Decode
                    // one word and force alpha; no per-channel shuffle.
                    for (i, dst) in dst_row.iter_mut().enumerate() {
                        let off = i * 4;
                        *dst = u32::from_le_bytes([
                            src_row[off], src_row[off + 1],
                            src_row[off + 2], 0xff,
                        ]);
                    }
                } else {
                    for (i, dst) in dst_row.iter_mut().enumerate() {
                        let v = u32::from_le_bytes([
                            src_row[i * 4],
                            src_row[i * 4 + 1],
                            src_row[i * 4 + 2],
                            src_row[i * 4 + 3],
                        ]);
                        *dst = blend(*dst, v);
                    }
                }
            }
        }

        buf.end_cpu_read();
    }

    /// Draw the embedded Aero arrow when no client cursor is available.
    pub fn blit_default_cursor(&mut self, x: i32, y: i32) {
        crate::cursor_aero::blit_default_cursor(self, x, y);
    }
}

#[inline]
fn blend(dst: u32, src: u32) -> u32 {
    // wl_shm ARGB8888 is premultiplied.  Straight-alpha blending made GTK
    // CSD (sakura titlebar/shadows) shimmer on every full composite.
    let a = (src >> 24) & 0xff;
    if a == 0xff {
        return src | 0xff00_0000;
    }
    if a == 0 {
        return dst;
    }
    let ia = 255 - a;
    let mix = |sh: u32| {
        let s = (src >> sh) & 0xff;
        let d = (dst >> sh) & 0xff;
        // Round instead of biasing every translucent composite downward.
        (s + (d * ia + 127) / 255).min(255)
    };
    0xff00_0000 | (mix(16) << 16) | (mix(8) << 8) | mix(0)
}

#[cfg(test)]
mod tests {
    use super::blend;

    #[test]
    fn premultiplied_source_over_preserves_channels() {
        assert_eq!(blend(0xff20_4060, 0x8080_0000), 0xff90_2030);
        assert_eq!(blend(0xff12_3456, 0x0000_0000), 0xff12_3456);
        assert_eq!(blend(0xff12_3456, 0xffab_cdef), 0xffab_cdef);
    }
}

#[cfg(not(target_arch = "wasm32"))]
#[derive(Debug, Clone)]
pub enum InputEvent {
    PointerMotion { x: f32, y: f32 },
    PointerRelative { dx: f32, dy: f32 },
    PointerButton { button: u32, pressed: bool },
    PointerAxis { axis: u32, value: f32 },
    Key { keycode: u32, pressed: bool },
    Reset,
    VtSwitch(u8),
}

pub trait Backend {
    fn size(&self) -> (u32, u32);
    fn present(&mut self, fb: &Framebuffer);

    /// Present a client dma-buf without touching its pixels. Returns false
    /// when the backend or scanout hardware cannot use this buffer directly.
    #[cfg(not(target_arch = "wasm32"))]
    fn present_dmabuf(&mut self, _buf: &ShmBuffer) -> bool {
        false
    }

    /// Composite ordered dma-buf surfaces on the GPU and scan out the result.
    #[cfg(not(target_arch = "wasm32"))]
    fn present_dmabufs(&mut self, _surfaces: &[(i32, i32, ShmBuffer)], _bitmap: Option<GpuBitmap<'_>>) -> bool {
        false
    }
    /// Take input channel once (WebGL backend): (receiver, eventfd for epoll wakeup).
    #[cfg(not(target_arch = "wasm32"))]
    fn take_input_channel(
        &mut self,
    ) -> Option<(std::sync::mpsc::Receiver<InputEvent>, std::os::unix::io::RawFd)> {
        None
    }

    #[cfg(not(target_arch = "wasm32"))]
    fn deactivate(&mut self) {}

    #[cfg(not(target_arch = "wasm32"))]
    fn activate(&mut self) {}

    #[cfg(not(target_arch = "wasm32"))]
    fn switch_vt(&mut self, _vt: u8) {}

    #[cfg(not(target_arch = "wasm32"))]
    fn shutdown(&mut self) {}

    /// `dev_t` of the DRM **render node** belonging to the GPU this backend is
    /// actually driving, used as `zwp_linux_dmabuf_v1.main_device`.
    ///
    /// Returning `None` means "no GPU behind this backend"; the server then
    /// keeps the dmabuf global hidden and clients fall back to `wl_shm`.  It is
    /// important never to report a *primary* node (`/dev/dri/cardN`) here: Mesa
    /// resolves `main_device` to a device it can render on, and a primary node
    /// that it cannot become DRM master of sends it down a half-initialised
    /// path where the EGL back buffer image ends up NULL.
    #[cfg(not(target_arch = "wasm32"))]
    fn drm_render_device(&self) -> Option<u64> {
        None
    }
}
