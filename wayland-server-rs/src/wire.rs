/*
 * Copyright © 2026 Yuichiro Nakada / Project Vespera
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */


use crate::types::{Arg, Fixed};
use std::collections::VecDeque;
use std::os::unix::io::RawFd;

pub const HEADER_SIZE: usize = 8;

pub struct RawMessage<'a> {
    pub object_id: u32,
    pub opcode: u16,
    pub size: usize,
    pub payload: &'a [u8],
}

pub fn decode_header(buf: &[u8]) -> Option<(RawMessage<'_>, usize)> {
    if buf.len() < HEADER_SIZE {
        return None;
    }
    let object_id = u32::from_ne_bytes(buf[0..4].try_into().ok()?);
    let word2 = u32::from_ne_bytes(buf[4..8].try_into().ok()?);
    let size = (word2 >> 16) as usize;
    let opcode = (word2 & 0xffff) as u16;
    if size < HEADER_SIZE || buf.len() < size {
        return None;
    }
    let payload = &buf[HEADER_SIZE..size];
    Some((
        RawMessage {
            object_id,
            opcode,
            size,
            payload,
        },
        size,
    ))
}

pub fn decode_args_into(
    sig: &str,
    payload: &[u8],
    fds: &mut VecDeque<RawFd>,
    out: &mut Vec<Arg>,
) {
    out.clear();
    let mut off = 0usize;

    macro_rules! u32at {
        () => {{
            if off + 4 > payload.len() {
                break;
            }
            let v = u32::from_ne_bytes(payload[off..off + 4].try_into().unwrap());
            off += 4;
            v
        }};
    }

    for ch in sig.bytes() {
        match ch {
            b'?' | b'0'..=b'9' => continue,
            b'i' => out.push(Arg::Int(u32at!() as i32)),
            b'u' => out.push(Arg::Uint(u32at!())),
            b'f' => out.push(Arg::Fixed(u32at!() as Fixed)),
            b'o' => out.push(Arg::Object(u32at!())),
            b'n' => out.push(Arg::NewId(u32at!())),
            b's' => {
                let len = u32at!() as usize;
                if len == 0 {
                    out.push(Arg::Str(None));
                    continue;
                }
                let padded = (len + 3) & !3;
                if off + padded > payload.len() {
                    break;
                }
                let raw = &payload[off..off + len - 1];
                out.push(Arg::Str(Some(String::from_utf8_lossy(raw).into_owned())));
                off += padded;
            }
            b'a' => {
                let len = u32at!() as usize;
                let padded = (len + 3) & !3;
                if off + padded > payload.len() {
                    break;
                }
                out.push(Arg::Array(payload[off..off + len].to_vec()));
                off += padded;
            }
            b'h' => out.push(Arg::Fd(fds.pop_front().unwrap_or(-1))),
            _ => {}
        }
    }
}

/// Append a complete message directly to a connection's persistent buffers.
/// This avoids three short-lived Vec allocations per Wayland event.
pub fn append_message(
    object_id: u32,
    opcode: u16,
    args: &[Arg],
    buf: &mut Vec<u8>,
    fds: &mut Vec<RawFd>,
) {
    let start = buf.len();
    buf.extend_from_slice(&object_id.to_ne_bytes());
    buf.extend_from_slice(&0u32.to_ne_bytes());
    for a in args {
        match a {
            Arg::Int(v) => buf.extend_from_slice(&v.to_ne_bytes()),
            Arg::Fixed(v) => buf.extend_from_slice(&v.to_ne_bytes()),
            Arg::Uint(v) | Arg::Object(v) | Arg::NewId(v) => {
                buf.extend_from_slice(&v.to_ne_bytes())
            }
            Arg::Str(s) => encode_string(buf, s.as_deref()),
            Arg::Array(data) => {
                buf.extend_from_slice(&(data.len() as u32).to_ne_bytes());
                buf.extend_from_slice(data);
                let pad = (4 - data.len() % 4) % 4;
                buf.extend(std::iter::repeat(0u8).take(pad));
            }
            Arg::Fd(fd) => fds.push(*fd),
        }
    }
    let size = (buf.len() - start) as u32;
    let word2 = (size << 16) | opcode as u32;
    buf[start + 4..start + 8].copy_from_slice(&word2.to_ne_bytes());
}

fn encode_string(buf: &mut Vec<u8>, s: Option<&str>) {
    match s {
        None => buf.extend_from_slice(&0u32.to_ne_bytes()),
        Some(s) => {
            let len = s.len() + 1;
            buf.extend_from_slice(&(len as u32).to_ne_bytes());
            buf.extend_from_slice(s.as_bytes());
            buf.push(0);
            let pad = (4 - len % 4) % 4;
            buf.extend(std::iter::repeat(0u8).take(pad));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn append_message_writes_header_and_padded_payload_in_place() {
        let mut bytes = Vec::with_capacity(64);
        let initial = bytes.capacity();
        let mut fds = Vec::new();
        append_message(
            7,
            3,
            &[Arg::Uint(42), Arg::Str(Some("ok".into()))],
            &mut bytes,
            &mut fds,
        );

        let (msg, used) = decode_header(&bytes).unwrap();
        assert_eq!(used, 20);
        assert_eq!(msg.object_id, 7);
        assert_eq!(msg.opcode, 3);
        assert_eq!(u32::from_ne_bytes(msg.payload[0..4].try_into().unwrap()), 42);
        assert_eq!(&msg.payload[4..], &[3, 0, 0, 0, b'o', b'k', 0, 0]);
        assert!(fds.is_empty());
        assert_eq!(bytes.capacity(), initial);
    }

    #[test]
    fn decode_args_reuses_existing_vector_storage() {
        let mut args = Vec::with_capacity(8);
        let capacity = args.capacity();
        let mut fds = VecDeque::new();
        decode_args_into("uu", &[1, 0, 0, 0, 2, 0, 0, 0], &mut fds, &mut args);
        assert!(matches!(args.as_slice(), [Arg::Uint(1), Arg::Uint(2)]));
        assert_eq!(args.capacity(), capacity);
    }
}
