/*
 * Copyright © 2026 Yuichiro Nakada / Project Vespera
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */


use libc::{
    accept4, bind, c_void, close, cmsghdr, iovec, listen, msghdr, recvmsg, sendmsg, socket,
    sockaddr_un, unlink, AF_UNIX, CMSG_DATA, CMSG_FIRSTHDR, CMSG_LEN, CMSG_NXTHDR, CMSG_SPACE,
    MSG_CMSG_CLOEXEC, MSG_DONTWAIT, MSG_NOSIGNAL, SCM_RIGHTS, SOCK_CLOEXEC, SOCK_NONBLOCK,
    SOCK_STREAM, SOL_SOCKET,
};
use std::collections::VecDeque;
use std::env;
use std::ffi::CString;
use std::io;
use std::os::unix::io::RawFd;

pub struct Listener {
    pub fd: RawFd,
    path: CString,
}

impl Listener {
    pub fn bind(name: &str) -> io::Result<Self> {
        let runtime = env::var("XDG_RUNTIME_DIR").map_err(|_| {
            io::Error::new(io::ErrorKind::NotFound, "XDG_RUNTIME_DIR not set")
        })?;
        let path = format!("{}/{}", runtime, name);
        let path_c = CString::new(path.as_bytes())
            .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "invalid path"))?;
        let bytes = path_c.as_bytes_with_nul();
        if bytes.len() > 108 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "socket path too long",
            ));
        }

        // Remove stale socket file.
        unsafe { unlink(path_c.as_ptr()) };

        let fd = unsafe { socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0) };
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }

        let mut addr: sockaddr_un = unsafe { std::mem::zeroed() };
        addr.sun_family = AF_UNIX as _;
        unsafe {
            std::ptr::copy_nonoverlapping(
                bytes.as_ptr() as *const libc::c_char,
                addr.sun_path.as_mut_ptr(),
                bytes.len(),
            );
            if bind(
                fd,
                &addr as *const sockaddr_un as *const _,
                std::mem::size_of_val(&addr) as _,
            ) < 0
            {
                let e = io::Error::last_os_error();
                close(fd);
                return Err(e);
            }
            if listen(fd, 128) < 0 {
                let e = io::Error::last_os_error();
                close(fd);
                return Err(e);
            }
        }

        Ok(Listener { fd, path: path_c })
    }

    pub fn accept(&self) -> io::Result<Option<RawFd>> {
        let cfd = unsafe {
            accept4(
                self.fd,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                SOCK_CLOEXEC | SOCK_NONBLOCK,
            )
        };
        if cfd < 0 {
            let e = io::Error::last_os_error();
            if e.kind() == io::ErrorKind::WouldBlock {
                return Ok(None);
            }
            return Err(e);
        }
        Ok(Some(cfd))
    }
}

impl Drop for Listener {
    fn drop(&mut self) {
        unsafe {
            close(self.fd);
            unlink(self.path.as_ptr());
        }
    }
}

pub struct Conn {
    pub fd: RawFd,
    pub send_buf: Vec<u8>,
    pub send_fds: Vec<RawFd>,
    pub recv_buf: Vec<u8>,
    pub recv_fds: VecDeque<RawFd>,
    pub closed: bool,
}

impl Conn {
    pub fn new(fd: RawFd) -> Self {
        Conn {
            fd,
            send_buf: Vec::new(),
            send_fds: Vec::new(),
            recv_buf: Vec::new(),
            recv_fds: VecDeque::new(),
            closed: false,
        }
    }

    pub fn queue(&mut self, data: &[u8], fds: &[RawFd]) {
        self.send_buf.extend_from_slice(data);
        self.send_fds.extend_from_slice(fds);
    }

    pub fn flush(&mut self) {
        if self.send_buf.is_empty() && self.send_fds.is_empty() {
            return;
        }
        let mut sent_total = 0usize;
        while sent_total < self.send_buf.len() {
            let chunk = &self.send_buf[sent_total..];
            let fds: &[RawFd] = if sent_total == 0 { &self.send_fds } else { &[] };
            let n = self.send_chunk(chunk, fds);
            if n <= 0 {
                for &fd in &self.send_fds {
                    unsafe { libc::close(fd); }
                }
                self.send_fds.clear();
                break;
            }
            sent_total += n as usize;
            if !fds.is_empty() {
                // SCM_RIGHTS send complete; close our copy of the fd.
                for &fd in &self.send_fds {
                    unsafe { libc::close(fd); }
                }
                self.send_fds.clear();
            }
        }
        if sent_total == self.send_buf.len() {
            self.send_buf.clear();
        } else if sent_total != 0 {
            self.send_buf.copy_within(sent_total.., 0);
            self.send_buf.truncate(self.send_buf.len() - sent_total);
        }
    }

    fn send_chunk(&self, data: &[u8], fds: &[RawFd]) -> isize {
        unsafe {
            let fd_bytes = std::mem::size_of_val(fds);
            let ctrl_len = if fd_bytes > 0 {
                CMSG_SPACE(fd_bytes as _) as usize
            } else {
                0
            };
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
                msg.msg_controllen = ctrl_len as _;
                let cmsg = CMSG_FIRSTHDR(&msg) as *mut cmsghdr;
                (*cmsg).cmsg_level = SOL_SOCKET;
                (*cmsg).cmsg_type = SCM_RIGHTS;
                (*cmsg).cmsg_len = CMSG_LEN(fd_bytes as _) as _;
                let dst = CMSG_DATA(cmsg) as *mut RawFd;
                std::ptr::copy_nonoverlapping(fds.as_ptr(), dst, fds.len());
            }

            sendmsg(self.fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT)
        }
    }

    pub fn recv(&mut self) -> io::Result<usize> {
        const BUF: usize = 8192;
        const FD_MAX: usize = 28;
        let fd_bytes = std::mem::size_of::<RawFd>() * FD_MAX;
        let ctrl_len = unsafe { CMSG_SPACE(fd_bytes as _) as usize };

        let mut data_buf = [0u8; BUF];
        let mut ctrl_buf = [0u8; 256];
        debug_assert!(ctrl_len <= ctrl_buf.len());

        let (n, actual_ctrl_len) = unsafe {
            let mut iov = iovec {
                iov_base: data_buf.as_mut_ptr() as *mut c_void,
                iov_len: BUF,
            };
            let mut msg: msghdr = std::mem::zeroed();
            msg.msg_iov = &mut iov;
            msg.msg_iovlen = 1;
            msg.msg_control = ctrl_buf.as_mut_ptr() as *mut c_void;
            msg.msg_controllen = ctrl_len as _;
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
            self.closed = true;
            return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "client closed"));
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
                    let dlen = (*cmsg).cmsg_len as usize - std::mem::size_of::<cmsghdr>();
                    let count = dlen / std::mem::size_of::<RawFd>();
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
}

impl Drop for Conn {
    fn drop(&mut self) {
        for &fd in &self.send_fds {
            unsafe { libc::close(fd); }
        }
        unsafe { close(self.fd) };
    }
}
