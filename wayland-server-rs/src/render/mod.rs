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

/// Screen-space rectangle, `x1`/`y1` exclusive.  An empty rect (`x1 <= x0`)
/// means "nothing to draw".
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Rect {
    pub x0: i32,
    pub y0: i32,
    pub x1: i32,
    pub y1: i32,
}

impl Rect {
    pub const EMPTY: Rect = Rect { x0: 0, y0: 0, x1: 0, y1: 0 };

    pub fn new(x0: i32, y0: i32, x1: i32, y1: i32) -> Self {
        Rect { x0, y0, x1, y1 }
    }

    pub fn is_empty(&self) -> bool {
        self.x1 <= self.x0 || self.y1 <= self.y0
    }

    pub fn union(&self, o: &Rect) -> Rect {
        if self.is_empty() {
            return *o;
        }
        if o.is_empty() {
            return *self;
        }
        Rect {
            x0: self.x0.min(o.x0),
            y0: self.y0.min(o.y0),
            x1: self.x1.max(o.x1),
            y1: self.y1.max(o.y1),
        }
    }

    pub fn intersect(&self, o: &Rect) -> Rect {
        Rect {
            x0: self.x0.max(o.x0),
            y0: self.y0.max(o.y0),
            x1: self.x1.min(o.x1),
            y1: self.y1.min(o.y1),
        }
    }

    /// Translate a protocol-supplied rectangle without wrapping at the i32
    /// boundary. Wayland clients commonly use INT32_MAX damage extents to mean
    /// "the whole surface", so adding a positive window origin must saturate.
    pub fn translated(&self, dx: i32, dy: i32) -> Rect {
        Rect {
            x0: self.x0.saturating_add(dx),
            y0: self.y0.saturating_add(dy),
            x1: self.x1.saturating_add(dx),
            y1: self.y1.saturating_add(dy),
        }
    }

    pub fn area(&self) -> i64 {
        if self.is_empty() {
            0
        } else {
            (self.x1 - self.x0) as i64 * (self.y1 - self.y0) as i64
        }
    }
}

pub struct Framebuffer {
    pub width: u32,
    pub height: u32,
    pub pixels: Vec<u32>,
    /// Every write below goes through this rectangle.  A full composite sets it
    /// to the whole screen; a damage-limited one narrows it so untouched pixels
    /// keep the values the previous composite left behind, which is what turns
    /// a blinking terminal cursor from a whole-desktop repaint into a few
    /// hundred pixels of work.
    clip: Rect,
}

impl Framebuffer {
    pub fn new(width: u32, height: u32) -> Self {
        Framebuffer {
            width,
            height,
            pixels: vec![0xff10_1014; (width * height) as usize],
            clip: Rect::new(0, 0, width as i32, height as i32),
        }
    }

    pub fn full_rect(&self) -> Rect {
        Rect::new(0, 0, self.width as i32, self.height as i32)
    }

    pub fn clip(&self) -> Rect {
        self.clip
    }

    /// Restrict drawing to `r` (clamped to the framebuffer).
    pub fn set_clip(&mut self, r: Rect) {
        self.clip = r.intersect(&self.full_rect());
    }

    pub fn reset_clip(&mut self) {
        self.clip = self.full_rect();
    }

    #[inline]
    pub fn in_clip(&self, x: i32, y: i32) -> bool {
        x >= self.clip.x0 && x < self.clip.x1 && y >= self.clip.y0 && y < self.clip.y1
    }

    /// Write one pixel if it is inside the clip and the framebuffer.
    #[inline]
    pub fn put(&mut self, x: i32, y: i32, argb: u32) {
        if !self.in_clip(x, y) {
            return;
        }
        let i = y as usize * self.width as usize + x as usize;
        if i < self.pixels.len() {
            self.pixels[i] = argb;
        }
    }

    /// Blend one premultiplied-ARGB pixel if it is inside the clip.
    #[inline]
    pub fn blend_px(&mut self, x: i32, y: i32, src: u32) {
        if !self.in_clip(x, y) {
            return;
        }
        let i = y as usize * self.width as usize + x as usize;
        if i < self.pixels.len() {
            self.pixels[i] = blend(self.pixels[i], src);
        }
    }

    pub fn clear(&mut self, argb: u32) {
        let c = self.clip;
        if c.is_empty() {
            return;
        }
        if c == self.full_rect() {
            for p in self.pixels.iter_mut() {
                *p = argb;
            }
            return;
        }
        let stride = self.width as usize;
        for y in c.y0 as usize..c.y1 as usize {
            let row = y * stride;
            self.pixels[row + c.x0 as usize..row + c.x1 as usize].fill(argb);
        }
    }

    /// Blit SHM/dmabuf with clipping; dmabuf triggers DMA_BUF_IOCTL_SYNC around CPU reads.
    pub fn blit_shm(&mut self, buf: &ShmBuffer, dx: i32, dy: i32) {
        // Intersect the destination rect with the active clip up front, so a
        // surface entirely outside the damaged area costs nothing at all — not
        // even the dma-buf CPU-access ioctls.
        let clip = self.clip;
        let dst = Rect::new(dx, dy, dx.saturating_add(buf.width), dy.saturating_add(buf.height))
            .intersect(&clip)
            .intersect(&self.full_rect());
        if dst.is_empty() {
            return;
        }
        let src_x0 = dst.x0 - dx;
        let src_y0 = dst.y0 - dy;
        let dst_x0 = dst.x0 as u32;
        let dst_y0 = dst.y0 as u32;
        let copy_w = (dst.x1 - dst.x0) as u32;
        let copy_h = (dst.y1 - dst.y0) as u32;

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
                    // XRGB already matches native 0xAARRGGBB byte order on LE;
                    // force opaque alpha without a per-channel shuffle.
                    let n = copy_w as usize;
                    if src_off % 4 == 0 && src_row.len() >= n * 4 {
                        // Single pass: load + OR alpha.  A memcpy followed by a
                        // second walk over the same cache lines was measurable
                        // on full-screen wallpaper damage.
                        unsafe {
                            let src_w = src_row.as_ptr() as *const u32;
                            let dst_w = dst_row.as_mut_ptr();
                            let mut i = 0usize;
                            while i + 4 <= n {
                                let a = *src_w.add(i) | 0xff00_0000;
                                let b = *src_w.add(i + 1) | 0xff00_0000;
                                let c = *src_w.add(i + 2) | 0xff00_0000;
                                let d = *src_w.add(i + 3) | 0xff00_0000;
                                *dst_w.add(i) = a;
                                *dst_w.add(i + 1) = b;
                                *dst_w.add(i + 2) = c;
                                *dst_w.add(i + 3) = d;
                                i += 4;
                            }
                            while i < n {
                                *dst_w.add(i) = *src_w.add(i) | 0xff00_0000;
                                i += 1;
                            }
                        }
                    } else {
                        for (dst, src) in dst_row.iter_mut().zip(src_row.chunks_exact(4)) {
                            let word = u32::from_le_bytes(src.try_into().unwrap());
                            *dst = word | 0xff00_0000;
                        }
                    }
                } else {
                    // Skip the blend math for runs that are fully transparent
                    // or fully opaque — the common case for CSD shadows and
                    // terminal glyphs sitting on mostly-empty ARGB buffers.
                    for (dst, src) in dst_row.iter_mut().zip(src_row.chunks_exact(4)) {
                        let word = u32::from_le_bytes(src.try_into().unwrap());
                        let a = word >> 24;
                        if a == 0 {
                            continue;
                        }
                        *dst = if a == 0xff { word } else { blend(*dst, word) };
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
    use super::{blend, Framebuffer, Rect};

    #[test]
    fn premultiplied_source_over_preserves_channels() {
        assert_eq!(blend(0xff20_4060, 0x8080_0000), 0xff90_2030);
        assert_eq!(blend(0xff12_3456, 0x0000_0000), 0xff12_3456);
        assert_eq!(blend(0xff12_3456, 0xffab_cdef), 0xffab_cdef);
    }

    #[test]
    fn rect_union_ignores_empty_operands() {
        let a = Rect::new(10, 10, 20, 20);
        assert_eq!(a.union(&Rect::EMPTY), a);
        assert_eq!(Rect::EMPTY.union(&a), a);
        assert_eq!(a.union(&Rect::new(0, 5, 12, 8)), Rect::new(0, 5, 20, 20));
    }

    #[test]
    fn rect_intersect_of_disjoint_is_empty() {
        assert!(Rect::new(0, 0, 4, 4).intersect(&Rect::new(9, 9, 12, 12)).is_empty());
        assert_eq!(
            Rect::new(0, 0, 10, 10).intersect(&Rect::new(4, 4, 20, 6)),
            Rect::new(4, 4, 10, 6)
        );
    }

    #[test]
    fn rect_translation_saturates_full_surface_damage() {
        let damage = Rect::new(0, 0, i32::MAX, i32::MAX);
        assert_eq!(
            damage.translated(120, 80),
            Rect::new(120, 80, i32::MAX, i32::MAX)
        );
    }

    #[test]
    fn clear_only_touches_the_clip() {
        let mut fb = Framebuffer::new(4, 4);
        fb.clear(0);
        fb.set_clip(Rect::new(1, 1, 3, 2));
        fb.clear(0xffff_ffff);
        let row = |y: usize| &fb.pixels[y * 4..y * 4 + 4];
        assert_eq!(row(0), [0, 0, 0, 0]);
        assert_eq!(row(1), [0, 0xffff_ffff, 0xffff_ffff, 0]);
        assert_eq!(row(2), [0, 0, 0, 0]);
    }

    #[test]
    fn put_outside_the_clip_is_dropped() {
        let mut fb = Framebuffer::new(4, 4);
        fb.clear(0);
        fb.set_clip(Rect::new(2, 2, 4, 4));
        fb.put(0, 0, 0xffff_ffff);
        fb.put(2, 2, 0xffff_ffff);
        assert_eq!(fb.pixels[0], 0);
        assert_eq!(fb.pixels[2 * 4 + 2], 0xffff_ffff);
    }

    #[test]
    fn set_clip_is_clamped_and_reset_restores_the_screen() {
        let mut fb = Framebuffer::new(4, 4);
        fb.set_clip(Rect::new(-5, -5, 100, 100));
        assert_eq!(fb.clip(), Rect::new(0, 0, 4, 4));
        fb.set_clip(Rect::new(1, 1, 2, 2));
        fb.reset_clip();
        assert_eq!(fb.clip(), fb.full_rect());
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

/// Completion feedback for one asynchronous display presentation.
///
/// `timestamp_ms` is in CLOCK_MONOTONIC milliseconds (wrapping to u32, just
/// like Wayland input/frame timestamps).  Synchronous backends never need to
/// construct this: the server completes their frame callbacks immediately
/// after `present*()` returns.
#[derive(Clone, Copy, Debug)]
pub struct PresentationFeedback {
    pub timestamp_ms: u32,
}

/// Monotonic timestamp shared by input events and presentation feedback.
#[cfg(not(target_arch = "wasm32"))]
pub fn monotonic_millis() -> u32 {
    let mut ts: libc::timespec = unsafe { std::mem::zeroed() };
    if unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut ts) } != 0 {
        return 0;
    }
    let ms = (ts.tv_sec.max(0) as u64)
        .saturating_mul(1000)
        .saturating_add((ts.tv_nsec.max(0) as u64) / 1_000_000);
    ms as u32
}

pub trait Backend {
    fn size(&self) -> (u32, u32);

    /// Nominal output refresh rate in millihertz, as advertised by wl_output.
    /// Backends without a physical mode keep the traditional 60 Hz default.
    fn refresh_millihz(&self) -> u32 {
        60_000
    }

    fn present(&mut self, fb: &Framebuffer);

    /// Present a framebuffer whose changed pixels are bounded by `damage`.
    /// Backends that cannot make use of the hint retain the old behaviour.
    fn present_damage(&mut self, fb: &Framebuffer, _damage: Rect) {
        self.present(fb);
    }

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

    /// True when `present_dmabufs` has a chance of succeeding.  The software
    /// composite path skips building the GPU plane list entirely when this is
    /// false — that list cloned every visible buffer handle on every frame
    /// even on hosts with no GBM/EGL composer.
    #[cfg(not(target_arch = "wasm32"))]
    fn can_gpu_compose(&self) -> bool {
        false
    }
    /// Take input channel once (WebGL backend): (receiver, eventfd for epoll wakeup).
    #[cfg(not(target_arch = "wasm32"))]
    fn take_input_channel(
        &mut self,
    ) -> Option<(std::sync::mpsc::Receiver<InputEvent>, std::os::unix::io::RawFd)> {
        None
    }

    /// Synchronize physical keyboard indicators: Num, Caps, Scroll in bits 0..2.
    #[cfg(not(target_arch = "wasm32"))]
    fn set_keyboard_leds(&self, _leds: u8) {}

    /// Descriptor the event loop must watch for presentation completions
    /// (DRM page-flip events).  `None` means presentation is synchronous.
    #[cfg(not(target_arch = "wasm32"))]
    fn event_fd(&self) -> Option<std::os::unix::io::RawFd> {
        None
    }

    /// Consume whatever `event_fd` signalled.  Asynchronous backends return
    /// feedback only when the frame has actually reached scanout (for DRM,
    /// DRM_EVENT_FLIP_COMPLETE), not when it was merely submitted.
    #[cfg(not(target_arch = "wasm32"))]
    fn dispatch_events(&mut self) -> Option<PresentationFeedback> {
        None
    }

    /// Recover an asynchronous completion whose event was lost/delayed.
    /// Called after the event-loop watchdog timeout.  The default backend has
    /// no asynchronous presentation and therefore nothing to recover.
    #[cfg(not(target_arch = "wasm32"))]
    fn poll_presentation(&mut self) -> Option<PresentationFeedback> {
        None
    }

    /// True while a previous `present` is still being taken over by the
    /// scanout hardware.  Compositing again before it clears would overwrite
    /// pixels the display is about to show.
    #[cfg(not(target_arch = "wasm32"))]
    fn present_busy(&self) -> bool {
        false
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
