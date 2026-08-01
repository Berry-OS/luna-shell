/*
 * Copyright © 2026 Yuichiro Nakada / Project Vespera
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */


use std::collections::VecDeque;
use std::os::unix::io::RawFd;
use std::env;
use std::io;
use libc::{
    AF_UNIX, SOCK_STREAM, SOCK_CLOEXEC,
    SOL_SOCKET, SCM_RIGHTS,
    iovec, msghdr, cmsghdr,
    sendmsg, recvmsg, MSG_NOSIGNAL, MSG_CMSG_CLOEXEC, MSG_DONTWAIT,
    connect, sockaddr_un, close,
    CMSG_SPACE, CMSG_FIRSTHDR, CMSG_NXTHDR, CMSG_LEN, CMSG_DATA,
    socket,
};
use std::os::raw::c_void;
use std::ffi::CString;

pub struct WaylandSocket {
    pub fd: RawFd,
    pub send_buf: Vec<u8>,
    pub send_fds: Vec<RawFd>,
    pub recv_buf: Vec<u8>,
    pub recv_fds: VecDeque<RawFd>,
}

impl WaylandSocket {
    pub fn connect(display_name: Option<&str>) -> io::Result<Self> {
        // Default to WAYLAND_DISPLAY when name is None (libwayland behavior).
        let wayland_env;
        let name = match display_name {
            Some(n) => n,
            None => {
                wayland_env = env::var("WAYLAND_DISPLAY").unwrap_or_else(|_| "wayland-0".into());
                &wayland_env
            }
        };
        // Absolute paths are used as-is (Firefox spawns a wayland-proxy and
        // reconnects with `$XDG_RUNTIME_DIR/wayland-proxy-<pid>`). Relative
        // names are resolved under XDG_RUNTIME_DIR — same as libwayland.
        let path = if name.starts_with('/') {
            name.to_string()
        } else {
            let runtime_dir = env::var("XDG_RUNTIME_DIR")
                .map_err(|_| io::Error::new(io::ErrorKind::NotFound, "XDG_RUNTIME_DIR not set"))?;
            format!("{}/{}", runtime_dir, name)
        };
        let path_c = CString::new(path.as_bytes())
            .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "invalid path"))?;
        let bytes = path_c.as_bytes_with_nul();
        if bytes.len() > 108 {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "socket path too long"));
        }

        let raw_fd = unsafe {
            socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)
        };
        if raw_fd < 0 {
            return Err(io::Error::last_os_error());
        }
        // Dup to fd >= 500 to avoid collisions when GTK closes low fds.
        let fd = unsafe {
            let high = libc::fcntl(raw_fd, libc::F_DUPFD_CLOEXEC, 500);
            close(raw_fd);
            if high < 0 { raw_fd } else { high }
        };

        let mut addr: sockaddr_un = unsafe { std::mem::zeroed() };
        addr.sun_family = AF_UNIX as _;
        unsafe {
            std::ptr::copy_nonoverlapping(
                bytes.as_ptr() as *const i8,
                addr.sun_path.as_mut_ptr(),
                bytes.len(),
            );
            let ret = connect(
                fd,
                &addr as *const sockaddr_un as *const _,
                std::mem::size_of_val(&addr) as _,
            );
            if ret < 0 {
                let e = io::Error::last_os_error();
                close(fd);
                return Err(e);
            }
        }
        eprintln!("[wl-socket] connected fd={} path={}", fd, path);

        Ok(WaylandSocket {
            fd,
            send_buf: Vec::new(),
            send_fds: Vec::new(),
            recv_buf: Vec::new(),
            recv_fds: VecDeque::new(),
        })
    }

    pub fn queue(&mut self, data: &[u8], fds: &[RawFd]) {
        self.send_buf.extend_from_slice(data);
        self.send_fds.extend_from_slice(fds);
    }

    pub fn flush(&mut self) -> i32 {
        if self.send_buf.is_empty() {
            return 0;
        }

        let mut total_sent = 0usize;

        while total_sent < self.send_buf.len() {
            let chunk = &self.send_buf[total_sent..];
            let fds_to_send: &[RawFd] = if total_sent == 0 && !self.send_fds.is_empty() {
                &self.send_fds
            } else {
                &[]
            };

            let sent = self.send_chunk(chunk, fds_to_send);
            if sent <= 0 {
                eprintln!("[wl-socket] sendmsg failed fd={}", self.fd);
                for &afd in &self.send_fds {
                    unsafe { libc::close(afd); }
                }
                self.send_fds.clear();
                break;
            }
            total_sent += sent as usize;
            if !fds_to_send.is_empty() {
                for &afd in &self.send_fds {
                    unsafe { libc::close(afd); }
                }
                self.send_fds.clear();
            }
        }

        self.send_buf.drain(..total_sent);
        total_sent as i32
    }

    fn send_chunk(&self, data: &[u8], fds: &[RawFd]) -> isize {
        unsafe {
            let fd_bytes = std::mem::size_of_val(fds);
            let ctrl_len = if fd_bytes > 0 {
                CMSG_SPACE(fd_bytes as _) as usize
            } else {
                0
            };
            // SCM_MAX_FD (28) needs well under 256 bytes including cmsghdr.
            let mut ctrl_buf = [0u8; 256];
            debug_assert!(ctrl_len <= ctrl_buf.len());

            let mut iov = iovec {
                iov_base: data.as_ptr() as *mut c_void,
                iov_len: data.len(),
            };
            let mut msg: msghdr = std::mem::zeroed();
            msg.msg_iov = &mut iov;
            msg.msg_iovlen = 1;

            if ctrl_len > 0 {
                msg.msg_control = ctrl_buf.as_mut_ptr() as *mut c_void;
                msg.msg_controllen = ctrl_len;
                let cmsg = CMSG_FIRSTHDR(&msg) as *mut cmsghdr;
                (*cmsg).cmsg_level = SOL_SOCKET;
                (*cmsg).cmsg_type  = SCM_RIGHTS;
                (*cmsg).cmsg_len   = CMSG_LEN(fd_bytes as _) as _;
                let dst = CMSG_DATA(cmsg) as *mut RawFd;
                std::ptr::copy_nonoverlapping(fds.as_ptr(), dst, fds.len());
            }

            sendmsg(self.fd, &msg, MSG_NOSIGNAL)
        }
    }

    pub fn recv(&mut self) -> io::Result<usize> {
        const BUF_SIZE: usize = 4096;
        const FD_MAX: usize = 28; // SCM_MAX_FD
        let fd_bytes = std::mem::size_of::<RawFd>() * FD_MAX;
        let ctrl_len = unsafe { CMSG_SPACE(fd_bytes as _) as usize };

        let mut data_buf = [0u8; BUF_SIZE];
        let mut ctrl_buf = [0u8; 256];
        debug_assert!(ctrl_len <= ctrl_buf.len());

        let (n, actual_ctrl_len) = unsafe {
            let mut iov = iovec {
                iov_base: data_buf.as_mut_ptr() as *mut c_void,
                iov_len:  BUF_SIZE,
            };
            let mut msg: msghdr = std::mem::zeroed();
            msg.msg_iov = &mut iov;
            msg.msg_iovlen = 1;
            msg.msg_control = ctrl_buf.as_mut_ptr() as *mut c_void;
            msg.msg_controllen = ctrl_len;
            let ret = recvmsg(self.fd, &mut msg, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
            (ret, msg.msg_controllen as usize)
        };

        if n < 0 {
            let e = io::Error::last_os_error();
            if e.kind() == io::ErrorKind::WouldBlock {
                return Ok(0);
            }
            return Err(e);
        }
        if n == 0 {
            return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "compositor closed connection"));
        }

        self.recv_buf.extend_from_slice(&data_buf[..n as usize]);

        // Collect fds from ancillary data.
        unsafe {
            let mut msg: msghdr = std::mem::zeroed();
            msg.msg_control = ctrl_buf.as_mut_ptr() as *mut c_void;
            msg.msg_controllen = actual_ctrl_len as _;

            let mut cmsg = CMSG_FIRSTHDR(&msg);
            while !cmsg.is_null() {
                if (*cmsg).cmsg_level == SOL_SOCKET && (*cmsg).cmsg_type == SCM_RIGHTS {
                    let data_len = (*cmsg).cmsg_len as usize - std::mem::size_of::<cmsghdr>();
                    let count = data_len / std::mem::size_of::<RawFd>();
                    let fds_ptr = CMSG_DATA(cmsg) as *const RawFd;
                    for i in 0..count {
                        self.recv_fds.push_back(*fds_ptr.add(i));
                    }
                }
                cmsg = CMSG_NXTHDR(&msg, cmsg);
            }
        }

        Ok(n as usize)
    }

    pub fn recv_blocking(&mut self) -> io::Result<usize> {
        loop {
            let n = self.recv()?;
            if n > 0 { return Ok(n); }
            // WouldBlock: block on the fd until it becomes readable instead of
            // busy-polling with a fixed sleep. poll() wakes the instant the
            // compositor writes, so input/frame events are dispatched with no
            // added latency (the old usleep(1000) added up to 1ms per event and
            // spun the CPU — the cause of "mossari" pointer/click handling).
            let mut pfd = libc::pollfd {
                fd: self.fd,
                events: libc::POLLIN,
                revents: 0,
            };
            let ret = unsafe { libc::poll(&mut pfd, 1, -1) };
            if ret < 0 {
                let e = io::Error::last_os_error();
                // Retry on EINTR; propagate anything else.
                if e.kind() == io::ErrorKind::Interrupted {
                    continue;
                }
                return Err(e);
            }
            if pfd.revents & (libc::POLLERR | libc::POLLHUP | libc::POLLNVAL) != 0 {
                return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "compositor connection closed"));
            }
        }
    }
}

impl Drop for WaylandSocket {
    fn drop(&mut self) {
        for &fd in &self.send_fds {
            unsafe { libc::close(fd); }
        }
        unsafe { close(self.fd); }
    }
}
