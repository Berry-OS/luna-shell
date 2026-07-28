/*
 * Copyright © 2026 Yuichiro Nakada / Project Vespera
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */


use std::os::raw::{c_char, c_void};
use std::ffi::CStr;
use crate::interfaces::WlInterface;
use crate::types::{wl_argument, wl_array};

pub const WAYLAND_HEADER_SIZE: usize = 8;

pub struct RawMessage {
    pub object_id: u32,
    pub opcode: u16,
    pub size: usize,
    pub payload: Vec<u8>,
}

pub fn decode_one(buf: &[u8]) -> Option<(RawMessage, usize)> {
    if buf.len() < WAYLAND_HEADER_SIZE {
        return None;
    }
    let object_id = u32::from_ne_bytes(buf[0..4].try_into().ok()?);
    let word2     = u32::from_ne_bytes(buf[4..8].try_into().ok()?);
    let size      = (word2 >> 16) as usize;
    let opcode    = (word2 & 0xffff) as u16;

    if size < WAYLAND_HEADER_SIZE || buf.len() < size {
        return None;
    }
    let payload = buf[WAYLAND_HEADER_SIZE..size].to_vec();
    Some((RawMessage { object_id, opcode, size, payload }, size))
}

pub unsafe fn encode_args(
    iface: *const WlInterface,
    opcode: u32,
    args: &[wl_argument],
) -> (Vec<u8>, Vec<i32>) {
    if iface.is_null() {
        return (Vec::new(), Vec::new());
    }
    let iface = &*iface;
    if opcode as i32 >= iface.method_count {
        return (Vec::new(), Vec::new());
    }
    let msg = &*iface.methods.add(opcode as usize);
    let sig = CStr::from_ptr(msg.signature).to_bytes();
    encode_by_sig(sig, args)
}

pub unsafe fn encode_by_sig(sig: &[u8], args: &[wl_argument]) -> (Vec<u8>, Vec<i32>) {
    let mut buf = Vec::new();
    let mut fds = Vec::new();
    let mut arg_idx = 0usize;

    for &ch in sig {
        match ch {
            b'?' | b'0'..=b'9' => continue,
            b'i' => {
                buf.extend_from_slice(&args[arg_idx].i.to_ne_bytes());
                arg_idx += 1;
            }
            b'u' => {
                buf.extend_from_slice(&args[arg_idx].u.to_ne_bytes());
                arg_idx += 1;
            }
            b'f' => {
                buf.extend_from_slice(&args[arg_idx].f.to_ne_bytes());
                arg_idx += 1;
            }
            b'o' => {
                let ptr = args[arg_idx].o as *mut crate::proxy::Proxy;
                let id: u32 = if ptr.is_null() { 0 } else { (*ptr).id };
                buf.extend_from_slice(&id.to_ne_bytes());
                arg_idx += 1;
            }
            b'n' => {
                buf.extend_from_slice(&args[arg_idx].n.to_ne_bytes());
                arg_idx += 1;
            }
            b's' => {
                encode_string(&mut buf, args[arg_idx].s);
                arg_idx += 1;
            }
            b'a' => {
                let arr_ptr = args[arg_idx].a;
                if arr_ptr.is_null() {
                    buf.extend_from_slice(&0u32.to_ne_bytes());
                } else {
                    let arr = &*arr_ptr;
                    let sz = arr.size as u32;
                    buf.extend_from_slice(&sz.to_ne_bytes());
                    let data = std::slice::from_raw_parts(arr.data as *const u8, arr.size);
                    buf.extend_from_slice(data);
                    let pad = (4 - arr.size % 4) % 4;
                    buf.extend(std::iter::repeat(0u8).take(pad));
                }
                arg_idx += 1;
            }
            b'h' => {
                // Dup fd: GTK4 may close after marshal; we close dup after SCM_RIGHTS send.
                let fd_val = args[arg_idx].h;
                let dup_fd = libc::fcntl(fd_val, libc::F_DUPFD_CLOEXEC, 0);
                fds.push(if dup_fd >= 0 { dup_fd } else { fd_val });
                arg_idx += 1;
            }
            _ => {}
        }
    }
    (buf, fds)
}

unsafe fn encode_string(buf: &mut Vec<u8>, s: *const c_char) {
    if s.is_null() {
        buf.extend_from_slice(&0u32.to_ne_bytes());
        return;
    }
    let bytes = CStr::from_ptr(s).to_bytes_with_nul();
    let len = bytes.len() as u32;
    buf.extend_from_slice(&len.to_ne_bytes());
    buf.extend_from_slice(bytes);
    let pad = (4 - bytes.len() % 4) % 4;
    buf.extend(std::iter::repeat(0u8).take(pad));
}

pub fn build_message(object_id: u32, opcode: u16, payload: &[u8]) -> Vec<u8> {
    let size = (WAYLAND_HEADER_SIZE + payload.len()) as u32;
    let word2 = (size << 16) | (opcode as u32);
    let mut msg = Vec::with_capacity(size as usize);
    msg.extend_from_slice(&object_id.to_ne_bytes());
    msg.extend_from_slice(&word2.to_ne_bytes());
    msg.extend_from_slice(payload);
    msg
}


pub unsafe fn parse_event_args(
    sig: &[u8],
    payload: &[u8],
    fds: &mut std::collections::VecDeque<i32>,
    objects: &std::collections::HashMap<u32, *mut crate::proxy::Proxy>,
) -> Vec<wl_argument> {
    let mut out: Vec<wl_argument> = Vec::new();
    let mut offset = 0usize;

    macro_rules! read_u32 {
        () => {{
            if offset + 4 > payload.len() { break; }
            let v = u32::from_ne_bytes(payload[offset..offset+4].try_into().unwrap());
            offset += 4;
            v
        }};
    }
    macro_rules! read_i32 {
        () => { read_u32!() as i32 };
    }

    'outer: for &ch in sig {
        match ch {
            b'?' | b'0'..=b'9' => continue,
            // Zero the full union before writing a narrow field so later
            // transmute-to-u64 (dispatch_listener) never leaks stack garbage
            // into the high 32 bits of pointer-sized args.
            b'i' => {
                let mut a = wl_argument { n: 0 };
                a.i = read_i32!();
                out.push(a);
            }
            b'u' => out.push(wl_argument { u: read_u32!() }),
            b'f' => {
                let mut a = wl_argument { n: 0 };
                a.f = read_i32!();
                out.push(a);
            }
            b'o' => {
                let id = read_u32!();
                let ptr = if id == 0 {
                    std::ptr::null_mut()
                } else {
                    objects.get(&id).copied().unwrap_or(std::ptr::null_mut()) as *mut c_void
                };
                out.push(wl_argument { o: ptr });
            }
            b'n' => out.push(wl_argument { n: read_u32!() }),
            b's' => {
                let len = read_u32!() as usize;
                if len == 0 {
                    out.push(wl_argument { s: std::ptr::null() });
                    continue;
                }
                if offset + len > payload.len() { break 'outer; }
                // Leak CString copy for C string lifetime.
                let s = std::ffi::CString::from_vec_with_nul(
                    payload[offset..offset+len].to_vec()
                ).unwrap_or_default();
                let ptr = s.into_raw();
                out.push(wl_argument { s: ptr });
                let pad_len = len + (4 - len % 4) % 4;
                offset += pad_len.min(payload.len() - offset.min(payload.len()));
            }
            b'a' => {
                let sz = read_u32!() as usize;
                let arr = Box::new(wl_array {
                    size: sz,
                    alloc: sz,
                    data: if sz > 0 {
                        let mut v: Vec<u8> = payload[offset..offset+sz].to_vec();
                        let ptr = v.as_mut_ptr() as *mut c_void;
                        std::mem::forget(v);
                        ptr
                    } else {
                        std::ptr::null_mut()
                    },
                });
                let pad = (4 - sz % 4) % 4;
                offset += sz + pad;
                out.push(wl_argument { a: Box::into_raw(arr) });
            }
            b'h' => {
                let mut a = wl_argument { n: 0 };
                a.h = fds.pop_front().unwrap_or(-1);
                out.push(a);
            }
            _ => {}
        }
    }
    out
}
