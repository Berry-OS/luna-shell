/*
 * Copyright © 2026 Yuichiro Nakada / Project Vespera
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

use std::collections::HashMap;
use std::ffi::CStr;
use std::os::raw::c_void;
use crate::interfaces::WlInterface;
use crate::proxy::Proxy;
use crate::socket::WaylandSocket;
use crate::types::wl_argument;
use crate::wire;

pub struct PendingEvent {
    pub proxy_id: u32,
    pub opcode:   u16,
    pub args:     Vec<wl_argument>,
}

#[repr(C)]
pub struct Display {
    /// MUST be first field (wl_display is a wl_proxy)
    pub proxy:      Proxy,
    pub socket:     WaylandSocket,
    pub objects:    HashMap<u32, *mut Proxy>,
    pub next_id:    u32,
    pub event_queue: Vec<PendingEvent>,
    pub error:      i32,
    pub error_msg:  Option<String>,
}

unsafe impl Send for Display {}
unsafe impl Sync for Display {}

impl Display {
    pub fn connect(name: Option<&str>) -> Result<*mut Self, i32> {
        let sock = WaylandSocket::connect(name).map_err(|_| libc::ENOENT)?;

        let display = Box::new(Display {
            proxy: Proxy::new(1, &crate::interfaces::wl_display_interface, 1, std::ptr::null_mut()),
            socket: sock,
            objects: HashMap::new(),
            next_id: 2,
            event_queue: Vec::new(),
            error: 0,
            error_msg: None,
        });

        let raw = Box::into_raw(display);
        unsafe {
            (*raw).proxy.display = raw;
            (*raw).objects.insert(1, raw as *mut Proxy);
        }
        Ok(raw)
    }

    pub fn alloc_id(&mut self) -> u32 {
        let id = self.next_id;
        self.next_id += 1;
        id
    }

    pub fn register(&mut self, proxy: *mut Proxy) {
        let id = unsafe { (*proxy).id };
        self.objects.insert(id, proxy);
    }

    pub fn lookup(&self, id: u32) -> Option<*mut Proxy> {
        self.objects.get(&id).copied()
    }

    pub unsafe fn send_request(
        &mut self,
        proxy: *mut Proxy,
        opcode: u16,
        iface: *const WlInterface,
        args: &[wl_argument],
    ) {
        let (payload, fds) = wire::encode_args(iface, opcode as u32, args);
        let msg = wire::build_message((*proxy).id, opcode, &payload);
        self.socket.queue(&msg, &fds);
    }

    pub fn flush(&mut self) -> i32 {
        self.socket.flush()
    }

    pub fn read_events(&mut self) -> i32 {
        match self.socket.recv() {
            Err(_) => {
                self.error = libc::EPIPE;
                -1
            }
            Ok(_) => {
                self.drain_recv_buf();
                0
            }
        }
    }

    pub fn drain_recv_buf(&mut self) {
        let mut buf_offset = 0usize;
        let buf = self.socket.recv_buf.clone(); // TODO: zero-copy

        loop {
            let remaining = &buf[buf_offset..];
            match wire::decode_one(remaining) {
                None => break,
                Some((msg, consumed)) => {
                    buf_offset += consumed;
                    self.dispatch_raw(msg.object_id, msg.opcode, &msg.payload);
                }
            }
        }
        self.socket.recv_buf.drain(..buf_offset);
    }

    fn dispatch_raw(&mut self, object_id: u32, opcode: u16, payload: &[u8]) {
        if object_id == 1 {
            match opcode {
                0 => {
                    self.error = -1;
                    eprintln!("[wl-client] wl_display.error received");
                }
                1 => {
                    if payload.len() >= 4 {
                        let del_id = u32::from_ne_bytes(payload[0..4].try_into().unwrap());
                        self.objects.remove(&del_id);
                    }
                }
                _ => {}
            }
            return;
        }

        let proxy_ptr = match self.objects.get(&object_id) {
            None => return,
            Some(&p) => p,
        };

        unsafe {
            let proxy = &*proxy_ptr;
            let iface = proxy.interface;
            if iface.is_null() {
                return;
            }
            let has_handler = proxy.dispatcher.is_some() || !proxy.implementation.is_null();
            let iface_ref = &*iface;
            if opcode as i32 >= iface_ref.event_count || iface_ref.events.is_null() {
                return;
            }
            let ev_msg = &*iface_ref.events.add(opcode as usize);
            let sig = CStr::from_ptr(ev_msg.signature).to_bytes();
            let mut args = wire::parse_event_args(sig, payload, &mut self.socket.recv_fds, &self.objects);
            /* Server-side new_id ('n'): ALWAYS create a proxy before dispatch
             * (libwayland ABI). Skipping this left raw ids in the arg union and
             * GTK segfaulted on wl_proxy_add_listener (pcmanfm). */
            let mut type_idx = 0usize;
            let mut arg_idx = 0usize;
            for &ch in sig {
                match ch {
                    b'?' | b'0'..=b'9' => continue,
                    b'n' => {
                        let new_id = args.get(arg_idx).map(|a| a.n).unwrap_or(0);
                        let new_iface = if !ev_msg.types.is_null() {
                            *ev_msg.types.add(type_idx)
                        } else {
                            std::ptr::null()
                        };
                        if new_id != 0 {
                            let use_iface = if !new_iface.is_null() {
                                new_iface
                            } else {
                                &crate::interfaces::wl_callback_interface as *const _
                            };
                            let ver = if !new_iface.is_null() {
                                (*new_iface).version as u32
                            } else {
                                1
                            };
                            let mut p = Proxy::new(new_id, use_iface, ver, self as *mut Display);
                            // Inherit sender queue (Mesa wayland-egl frame callbacks).
                            p.queue = proxy.queue;
                            let raw = Box::into_raw(Box::new(p));
                            self.register(raw);
                            args[arg_idx] = wl_argument { o: raw as *mut c_void };
                        }
                        type_idx += 1;
                        arg_idx += 1;
                    }
                    _ => {
                        type_idx += 1;
                        arg_idx += 1;
                    }
                }
            }
            if has_handler {
                dispatch_event(proxy_ptr, opcode, ev_msg as *const crate::types::wl_message, &mut args);
            }
        }
    }

    pub fn roundtrip(&mut self) -> i32 {
        unsafe {
            let cb_id = self.alloc_id();
            let cb = Box::new(Proxy::new(
                cb_id,
                &crate::interfaces::wl_callback_interface,
                1,
                self as *mut Display,
            ));
            let cb_raw = Box::into_raw(cb);
            self.register(cb_raw);

            let arg = wl_argument { n: cb_id };
            let display_proxy_ptr = &mut self.proxy as *mut Proxy;
            self.send_request(
                display_proxy_ptr,
                0,
                &crate::interfaces::wl_display_interface,
                &[arg],
            );
            self.flush();

            let done_flag = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
            let flag_clone = done_flag.clone();
            (*cb_raw).user_data = Box::into_raw(Box::new(flag_clone)) as *mut c_void;
            static ROUNDTRIP_VTABLE: [unsafe extern "C" fn(*mut c_void, *mut Proxy, u32); 1]
                = [roundtrip_done_listener];
            (*cb_raw).implementation = ROUNDTRIP_VTABLE.as_ptr() as *const c_void;
            (*cb_raw).dispatcher = None;

            let timeout = std::time::Instant::now() + std::time::Duration::from_secs(5);
            while !done_flag.load(std::sync::atomic::Ordering::Relaxed) {
                if std::time::Instant::now() > timeout {
                    return -1;
                }
                if self.socket.recv_blocking().is_err() {
                    return -1;
                }
                self.drain_recv_buf();
            }
        }
        0
    }
}

unsafe extern "C" fn roundtrip_done_listener(
    data: *mut c_void,
    _proxy: *mut Proxy,
    _serial: u32,
) {
    let flag = &*(data as *const std::sync::Arc<std::sync::atomic::AtomicBool>);
    flag.store(true, std::sync::atomic::Ordering::Relaxed);
}

unsafe fn dispatch_event(
    proxy: *mut Proxy,
    opcode: u16,
    message: *const crate::types::wl_message,
    args: &mut [wl_argument],
) {
    let proxy_ref = &*proxy;
    if let Some(disp) = proxy_ref.dispatcher {
        let _ = disp(
            proxy_ref.implementation,
            proxy as *mut c_void,
            opcode as u32,
            message,
            args.as_mut_ptr(),
        );
        return;
    }
    if !proxy_ref.implementation.is_null() {
        dispatch_listener(proxy, opcode, args);
    }
}

unsafe fn dispatch_listener(proxy: *mut Proxy, opcode: u16, args: &[wl_argument]) {
    let proxy_ref = &*proxy;
    if proxy_ref.implementation.is_null() { return; }

    type Fn0  = unsafe extern "C" fn(*mut c_void, *mut Proxy);
    type Fn1  = unsafe extern "C" fn(*mut c_void, *mut Proxy, u64);
    type Fn2  = unsafe extern "C" fn(*mut c_void, *mut Proxy, u64, u64);
    type Fn3  = unsafe extern "C" fn(*mut c_void, *mut Proxy, u64, u64, u64);
    type Fn4  = unsafe extern "C" fn(*mut c_void, *mut Proxy, u64, u64, u64, u64);
    type Fn5  = unsafe extern "C" fn(*mut c_void, *mut Proxy, u64, u64, u64, u64, u64);
    type Fn6  = unsafe extern "C" fn(*mut c_void, *mut Proxy, u64, u64, u64, u64, u64, u64);
    type Fn7  = unsafe extern "C" fn(*mut c_void, *mut Proxy, u64, u64, u64, u64, u64, u64, u64);
    type Fn8  = unsafe extern "C" fn(*mut c_void, *mut Proxy, u64, u64, u64, u64, u64, u64, u64, u64);
    type Fn9  = unsafe extern "C" fn(*mut c_void, *mut Proxy, u64, u64, u64, u64, u64, u64, u64, u64, u64);
    type Fn10 = unsafe extern "C" fn(*mut c_void, *mut Proxy, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64);
    type Fn11 = unsafe extern "C" fn(*mut c_void, *mut Proxy, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64);
    type Fn12 = unsafe extern "C" fn(*mut c_void, *mut Proxy, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64);

    let fn_table = proxy_ref.implementation as *const *const c_void;
    let fn_ptr   = *fn_table.add(opcode as usize);
    if fn_ptr.is_null() { return; }

    let ud = proxy_ref.user_data;
    #[inline(always)]
    unsafe fn a64(a: &wl_argument) -> u64 {
        std::mem::transmute::<wl_argument, u64>(std::ptr::read(a as *const wl_argument))
    }
    let n = args.len();
    let p0  = if n >  0 { a64(&args[0])  } else { 0 };
    let p1  = if n >  1 { a64(&args[1])  } else { 0 };
    let p2  = if n >  2 { a64(&args[2])  } else { 0 };
    let p3  = if n >  3 { a64(&args[3])  } else { 0 };
    let p4  = if n >  4 { a64(&args[4])  } else { 0 };
    let p5  = if n >  5 { a64(&args[5])  } else { 0 };
    let p6  = if n >  6 { a64(&args[6])  } else { 0 };
    let p7  = if n >  7 { a64(&args[7])  } else { 0 };
    let p8  = if n >  8 { a64(&args[8])  } else { 0 };
    let p9  = if n >  9 { a64(&args[9])  } else { 0 };
    let p10 = if n > 10 { a64(&args[10]) } else { 0 };
    let p11 = if n > 11 { a64(&args[11]) } else { 0 };

    match n {
        0  => std::mem::transmute::<_, Fn0 >(fn_ptr)(ud, proxy),
        1  => std::mem::transmute::<_, Fn1 >(fn_ptr)(ud, proxy, p0),
        2  => std::mem::transmute::<_, Fn2 >(fn_ptr)(ud, proxy, p0, p1),
        3  => std::mem::transmute::<_, Fn3 >(fn_ptr)(ud, proxy, p0, p1, p2),
        4  => std::mem::transmute::<_, Fn4 >(fn_ptr)(ud, proxy, p0, p1, p2, p3),
        5  => std::mem::transmute::<_, Fn5 >(fn_ptr)(ud, proxy, p0, p1, p2, p3, p4),
        6  => std::mem::transmute::<_, Fn6 >(fn_ptr)(ud, proxy, p0, p1, p2, p3, p4, p5),
        7  => std::mem::transmute::<_, Fn7 >(fn_ptr)(ud, proxy, p0, p1, p2, p3, p4, p5, p6),
        8  => std::mem::transmute::<_, Fn8 >(fn_ptr)(ud, proxy, p0, p1, p2, p3, p4, p5, p6, p7),
        9  => std::mem::transmute::<_, Fn9 >(fn_ptr)(ud, proxy, p0, p1, p2, p3, p4, p5, p6, p7, p8),
        10 => std::mem::transmute::<_, Fn10>(fn_ptr)(ud, proxy, p0, p1, p2, p3, p4, p5, p6, p7, p8, p9),
        11 => std::mem::transmute::<_, Fn11>(fn_ptr)(ud, proxy, p0, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10),
        _  => std::mem::transmute::<_, Fn12>(fn_ptr)(ud, proxy, p0, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11),
    }
}
