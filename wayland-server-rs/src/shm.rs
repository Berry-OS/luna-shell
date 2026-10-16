/*
 * Copyright © 2026 Yuichiro Nakada / Project Vespera
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */


use libc::{c_void, mmap, munmap, MAP_FAILED, MAP_SHARED, PROT_READ};
use std::os::unix::io::RawFd;
use std::cell::Cell;
use std::rc::Rc;
use std::sync::atomic::{AtomicU64, Ordering};

static NEXT_CONTENT_SERIAL: AtomicU64 = AtomicU64::new(1);
static NEXT_SURFACE_GPU_ID: AtomicU64 = AtomicU64::new(1);

/// Per-surface bookkeeping for incremental GPU uploads of wl_shm content.
///
/// A surface keeps one GPU texture no matter how many wl_buffers its client
/// cycles through.  That texture always holds the previously committed
/// content, so the damage a commit reports (the difference from the previous
/// commit) is exactly what has to be re-uploaded -- not the whole buffer.
/// `dirty` accumulates that damage until a GPU composite has consumed it, so
/// commits that were never presented (or a composite that fell back to the CPU
/// path) cannot leave the texture stale.
pub struct SurfaceGpu {
    pub id: u64,
    pub dirty: Cell<crate::render::Rect>,
}

impl SurfaceGpu {
    pub fn new() -> Rc<Self> {
        Rc::new(SurfaceGpu {
            id: NEXT_SURFACE_GPU_ID.fetch_add(1, Ordering::Relaxed),
            dirty: Cell::new(crate::render::Rect::EMPTY),
        })
    }
}

pub const FORMAT_ARGB8888: u32 = 0;
pub const FORMAT_XRGB8888: u32 = 1;

// DMA_BUF_IOCTL_SYNC: required before/after CPU reads of dmabuf (not SHM).
#[repr(C)]
struct DmaBufSync {
    flags: u64,
}
const DMA_BUF_SYNC_READ: u64 = 1 << 0;
const DMA_BUF_SYNC_START: u64 = 0 << 2;
const DMA_BUF_SYNC_END: u64 = 1 << 2;
// _IOW('b', 0, struct dma_buf_sync) = (WRITE<<30)|('b'<<8)|nr|(size<<16)
const DMA_BUF_IOCTL_SYNC: u64 =
    (1 << 30) | (0x62u64 << 8) | 0 | ((std::mem::size_of::<DmaBufSync>() as u64) << 16);

pub struct ShmPool {
    /// CPU mapping. A dma-buf may legitimately be GPU-only and therefore not
    /// mmap-able; in that case this stays NULL while `fd` remains valid for
    /// direct scanout / EGLImage import by the GPU compositor.
    ptr: *mut c_void,
    size: usize,
    fd: RawFd,
    /// Stable backing-object identity, captured once instead of calling fstat
    /// for every plane in every GPU-composited frame.
    storage_dev: u64,
    storage_ino: u64,
    /// dmabuf pools need DMA_BUF_IOCTL_SYNC around CPU access
    is_dmabuf: bool,
    /// CPU-owned pixels (compositor-drawn chrome); `ptr` points into it and
    /// there is no fd to unmap or close.
    owned: Option<Vec<u32>>,
}

impl ShmPool {
    /// A pool backed by compositor-owned ARGB pixels.
    pub fn from_pixels(pixels: Vec<u32>) -> Rc<ShmPool> {
        let size = pixels.len() * 4;
        let ptr = pixels.as_ptr() as *mut c_void;
        Rc::new(ShmPool {
            ptr,
            size,
            fd: -1,
            storage_dev: 0,
            storage_ino: 0,
            is_dmabuf: false,
            owned: Some(pixels),
        })
    }

    pub fn map(fd: RawFd, size: usize) -> Option<Rc<ShmPool>> {
        Self::map_inner(fd, size, false, true)
    }

    pub fn map_dmabuf(fd: RawFd, size: usize, cpu_linear: bool) -> Option<Rc<ShmPool>> {
        Self::map_inner(fd, size, true, cpu_linear)
    }

    fn map_inner(fd: RawFd, size: usize, is_dmabuf: bool, cpu_linear: bool) -> Option<Rc<ShmPool>> {
        if size == 0 {
            return None;
        }
        let mapped = if !is_dmabuf || cpu_linear {
            unsafe { mmap(std::ptr::null_mut(), size, PROT_READ, MAP_SHARED, fd, 0) }
        } else {
            MAP_FAILED
        };
        let ptr = if mapped == MAP_FAILED {
            if !is_dmabuf {
                unsafe { libc::close(fd) };
                return None;
            }
            /* GPU-local dma-bufs often cannot be CPU-mapped. Keep ownership
             * of the fd: the DRI backend can still import it through PRIME or
             * EGL_EXT_image_dma_buf_import without ever touching its pixels. */
            std::ptr::null_mut()
        } else {
            mapped
        };
        let mut stat: libc::stat = unsafe { std::mem::zeroed() };
        let have_identity = unsafe { libc::fstat(fd, &mut stat) } == 0;
        Some(Rc::new(ShmPool {
            ptr,
            size,
            fd,
            storage_dev: if have_identity { stat.st_dev as u64 } else { 0 },
            storage_ino: if have_identity { stat.st_ino as u64 } else { 0 },
            is_dmabuf,
            owned: None,
        }))
    }

    /// Notify dmabuf CPU read boundaries; no-op for SHM.
    fn dma_sync(&self, start: bool) {
        if !self.is_dmabuf || self.ptr.is_null() {
            return;
        }
        let phase = if start {
            DMA_BUF_SYNC_START
        } else {
            DMA_BUF_SYNC_END
        };
        let mut arg = DmaBufSync {
            flags: DMA_BUF_SYNC_READ | phase,
        };
        // Retry on EINTR/EAGAIN per dma-buf convention.
        loop {
            let r = unsafe { libc::ioctl(self.fd, DMA_BUF_IOCTL_SYNC as _, &mut arg) };
            if r == 0 {
                break;
            }
            let e = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
            if e != libc::EINTR && e != libc::EAGAIN {
                break;
            }
        }
    }

    pub fn begin_cpu_read(&self) {
        self.dma_sync(true);
    }
    pub fn end_cpu_read(&self) {
        self.dma_sync(false);
    }

    pub fn size(&self) -> usize {
        self.size
    }

    /// Borrow the owned dma-buf fd for DRM PRIME import. The pool keeps the
    /// descriptor alive for the complete wl_buffer lifetime.
    pub fn dmabuf_fd(&self) -> Option<RawFd> {
        self.is_dmabuf.then_some(self.fd)
    }

    pub fn storage_id(&self) -> (u64, u64) {
        (self.storage_dev, self.storage_ino)
    }

    pub fn slice(&self, offset: usize, len: usize) -> Option<&[u8]> {
        if self.ptr.is_null() || offset.checked_add(len)? > self.size {
            return None;
        }
        Some(unsafe { std::slice::from_raw_parts((self.ptr as *const u8).add(offset), len) })
    }
}

impl Drop for ShmPool {
    fn drop(&mut self) {
        if self.owned.is_some() {
            return;
        }
        unsafe {
            if !self.ptr.is_null() {
                munmap(self.ptr, self.size);
            }
            libc::close(self.fd);
        }
    }
}

#[derive(Clone)]
pub struct ShmBuffer {
    pub pool: Rc<ShmPool>,
    pub offset: usize,
    pub width: i32,
    pub height: i32,
    pub stride: i32,
    pub format: u32,
    /// DRM format modifier for dma-buf storage. wl_shm is always linear.
    pub modifier: u64,
    /// Assigned a unique serial whenever this wl_buffer is committed. GPU
    /// composition uses it to skip uploading unchanged SHM storage.
    pub content_serial: Rc<Cell<u64>>,
    /// Set by the surface commit that makes this buffer current (wl_shm only).
    pub surface_gpu: Option<Rc<SurfaceGpu>>,
}

impl ShmBuffer {
    pub fn dmabuf_fd(&self) -> Option<RawFd> {
        self.pool.dmabuf_fd()
    }

    pub fn dmabuf_modifier(&self) -> Option<u64> {
        self.pool.dmabuf_fd().map(|_| self.modifier)
    }

    pub fn is_cpu_linear(&self) -> bool {
        !self.pool.is_dmabuf || self.modifier == 0
    }

    pub fn storage_id(&self) -> (u64, u64) {
        self.pool.storage_id()
    }

    pub fn mark_updated(&self) {
        self.content_serial.set(NEXT_CONTENT_SERIAL.fetch_add(1, Ordering::Relaxed));
    }

    pub fn serial(&self) -> u64 {
        self.content_serial.get()
    }

    pub fn upload_ptr(&self) -> Option<*const u8> {
        let len = (self.stride as usize).checked_mul(self.height as usize)?;
        Some(self.pool.slice(self.offset, len)?.as_ptr())
    }
    /// Begin CPU read (dmabuf issues SYNC_START). Pair with end_cpu_read().
    pub fn begin_cpu_read(&self) {
        self.pool.begin_cpu_read();
    }

    pub fn end_cpu_read(&self) {
        self.pool.end_cpu_read();
    }

    #[inline]
    pub fn pixel(&self, x: i32, y: i32) -> Option<u32> {
        // A tiled/compressed dma-buf cannot be interpreted as linear CPU rows
        // even when its fd happens to be mmap-able.
        if self.pool.is_dmabuf && self.modifier != 0 {
            return None;
        }
        if x < 0 || y < 0 || x >= self.width || y >= self.height {
            return None;
        }
        let off = self.offset + (y as usize) * (self.stride as usize) + (x as usize) * 4;
        let px = self.pool.slice(off, 4)?;
        let v = u32::from_le_bytes([px[0], px[1], px[2], px[3]]);
        match self.format {
            // wl_shm stores BGRA little-endian as 0xAARRGGBB
            FORMAT_ARGB8888 => Some(v),
            FORMAT_XRGB8888 => Some(v | 0xff00_0000),
            _ => Some(v | 0xff00_0000),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn separate_buffers_reusing_storage_have_distinct_upload_serials() {
        let fd = unsafe { libc::memfd_create(b"luna-serial-test\0".as_ptr().cast(), libc::MFD_CLOEXEC) };
        assert!(fd >= 0);
        assert_eq!(unsafe { libc::ftruncate(fd, 4) }, 0);
        let pool = ShmPool::map(fd, 4).unwrap();
        let first = ShmBuffer { pool, offset: 0, width: 1, height: 1, stride: 4,
            format: FORMAT_XRGB8888, modifier: 0, content_serial: Rc::new(Cell::new(0)), surface_gpu: None };
        let mut second = first.clone();
        second.content_serial = Rc::new(Cell::new(0));
        first.mark_updated();
        second.mark_updated();
        assert_eq!(first.upload_ptr(), second.upload_ptr());
        assert_ne!(first.serial(), second.serial());
        let retained = first.clone();
        first.mark_updated();
        assert_eq!(first.serial(), retained.serial());
        assert_ne!(first.serial(), second.serial());
    }
}
