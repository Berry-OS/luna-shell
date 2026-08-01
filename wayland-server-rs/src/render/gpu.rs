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
struct Plane { fd: c_int, pixels:*const u8, width:u32, height:u32, stride:u32, offset:u32, fourcc:u32, x:i32, y:i32 }

#[link(name = "luna_gpu", kind = "static")]
unsafe extern "C" {
    fn luna_gpu_create(fd: c_int, width: u32, height: u32) -> *mut c_void;
    fn luna_gpu_render(ctx: *mut c_void, pixels: *const u32, width: u32, height: u32, out: *mut Output) -> c_int;
    fn luna_gpu_render_planes(ctx:*mut c_void, planes:*const Plane, count:u32, out:*mut Output)->c_int;
    fn luna_gpu_commit(ctx: *mut c_void);
    fn luna_gpu_discard(ctx: *mut c_void);
    fn luna_gpu_destroy(ctx: *mut c_void);
}

pub struct GpuComposer(*mut c_void);

impl GpuComposer {
    pub fn new(fd: RawFd, width: u32, height: u32) -> Option<Self> {
        let p = unsafe { luna_gpu_create(fd, width, height) };
        (!p.is_null()).then_some(Self(p))
    }

    pub fn render(&mut self, fb: &Framebuffer) -> Option<(*mut c_void, u32, u32)> {
        let mut out = Output::default();
        let ok = unsafe { luna_gpu_render(self.0, fb.pixels.as_ptr(), fb.width, fb.height, &mut out) };
        (ok != 0).then_some((out.bo, out.handle, out.stride))
    }

    pub fn render_dmabufs(&mut self, surfaces:&[(i32,i32,crate::shm::ShmBuffer)], bitmap:Option<super::GpuBitmap<'_>>) -> Option<(*mut c_void,u32,u32)> {
        let planes: Option<Vec<_>> = surfaces.iter().map(|(x,y,b)| {
            let width:u32=b.width.try_into().ok()?; let stride:u32=b.stride.try_into().ok()?;
            let fd=b.dmabuf_fd().unwrap_or(-1);
            if fd < 0 && stride != width*4 { return None; }
            Some(Plane{fd,pixels:if fd<0{b.upload_ptr()?}else{std::ptr::null()},width,height:b.height.try_into().ok()?,stride,offset:b.offset.try_into().ok()?,fourcc:if b.format==crate::shm::FORMAT_XRGB8888{0x34325258}else{0x34325241},x:*x,y:*y})
        }).collect();
        let mut planes=planes?;
        if let Some(b)=bitmap { planes.push(Plane{fd:-1,pixels:b.pixels.as_ptr() as *const u8,width:b.width,height:b.height,stride:b.width*4,offset:0,fourcc:0x34325241,x:b.x,y:b.y}); }
        let mut out=Output::default();
        let ok=unsafe{luna_gpu_render_planes(self.0,planes.as_ptr(),planes.len() as u32,&mut out)};
        (ok!=0).then_some((out.bo,out.handle,out.stride))
    }

    pub fn commit(&mut self) { unsafe { luna_gpu_commit(self.0) } }
    pub fn discard(&mut self) { unsafe { luna_gpu_discard(self.0) } }
}

impl Drop for GpuComposer {
    fn drop(&mut self) { unsafe { luna_gpu_destroy(self.0) } }
}
