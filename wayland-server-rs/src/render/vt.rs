use std::ffi::CString;
use std::os::unix::io::RawFd;

const VT_GETMODE: libc::c_ulong = 0x5601;
const VT_SETMODE: libc::c_ulong = 0x5602;
const VT_RELDISP: libc::c_ulong = 0x5605;
const VT_ACTIVATE: libc::c_ulong = 0x5606;
const VT_PROCESS: u8 = 1;
const VT_ACKACQ: libc::c_int = 2;
const KDGETMODE: libc::c_ulong = 0x4b3b;
const KDSETMODE: libc::c_ulong = 0x4b3a;
const KDGKBMODE: libc::c_ulong = 0x4b44;
const KDSKBMODE: libc::c_ulong = 0x4b45;
const KD_GRAPHICS: libc::c_int = 1;
const K_OFF: libc::c_int = 4;

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct VtMode {
  mode: u8,
  waitv: u8,
  relsig: i16,
  acqsig: i16,
  frsig: i16,
}

pub struct VtSession {
  fd: RawFd,
  saved_mode: VtMode,
  saved_display: libc::c_int,
  saved_keyboard: libc::c_int,
  restored: bool,
}

impl VtSession {
  pub fn open(path: Option<&str>) -> std::io::Result<Self> {
    block_session_signals()?;
    let tty = path.unwrap_or("/dev/tty");
    let cpath = CString::new(tty).map_err(|_| std::io::Error::from(std::io::ErrorKind::InvalidInput))?;
    let fd = unsafe { libc::open(cpath.as_ptr(), libc::O_RDWR | libc::O_CLOEXEC | libc::O_NOCTTY) };
    if fd < 0 {
      return Err(std::io::Error::last_os_error());
    }

    // /dev/tty is a multiplexing device (major 5, minor 0), even when the
    // process' controlling terminal is a real Linux VT.  Checking st_rdev
    // therefore rejects the normal/default path unconditionally.  VT_GETMODE
    // is the authoritative test and returns ENOTTY for PTYs.
    let mut saved_mode = VtMode::default();
    if unsafe { libc::ioctl(fd, VT_GETMODE, &mut saved_mode) } != 0 {
      let error = std::io::Error::last_os_error();
      unsafe { libc::close(fd) };
      return Err(error);
    }
    let process_mode = VtMode {
      mode: VT_PROCESS,
      waitv: 0,
      relsig: libc::SIGUSR1 as i16,
      acqsig: libc::SIGUSR2 as i16,
      frsig: 0,
    };
    let mut saved_display = 0;
    let mut saved_keyboard = 0;
    if unsafe { libc::ioctl(fd, KDGETMODE, &mut saved_display) } != 0
      || unsafe { libc::ioctl(fd, KDGKBMODE, &mut saved_keyboard) } != 0 {
      let error = std::io::Error::last_os_error();
      unsafe { libc::close(fd) };
      return Err(error);
    }
    if unsafe { libc::ioctl(fd, VT_SETMODE, &process_mode) } != 0 {
      let error = std::io::Error::last_os_error();
      unsafe { libc::close(fd) };
      return Err(error);
    }
    let mut session = Self {
      fd,
      saved_mode,
      saved_display,
      saved_keyboard,
      restored: false,
    };
    // evdev owns input; the console must neither echo keys nor repaint its
    // text/cursor over KMS. Restore both modes even if initialization fails.
    if unsafe { libc::ioctl(fd, KDSETMODE, KD_GRAPHICS) } != 0
      || unsafe { libc::ioctl(fd, KDSKBMODE, K_OFF) } != 0 {
      let error = std::io::Error::last_os_error();
      session.restore();
      return Err(error);
    }
    Ok(session)
  }

  pub fn switch_to(&self, vt: u8) -> std::io::Result<()> {
    if vt == 0 || vt > 63 {
      return Err(std::io::Error::from(std::io::ErrorKind::InvalidInput));
    }
    if unsafe { libc::ioctl(self.fd, VT_ACTIVATE, vt as libc::c_int) } != 0 {
      return Err(std::io::Error::last_os_error());
    }
    Ok(())
  }

  pub fn release(&self) { unsafe { libc::ioctl(self.fd, VT_RELDISP, 1) }; }

  pub fn acknowledge_acquire(&self) { unsafe { libc::ioctl(self.fd, VT_RELDISP, VT_ACKACQ) }; }

  pub fn restore(&mut self) {
    if self.restored {
      return;
    }
    unsafe {
      libc::ioctl(self.fd, KDSKBMODE, self.saved_keyboard);
      libc::ioctl(self.fd, KDSETMODE, self.saved_display);
      libc::ioctl(self.fd, VT_SETMODE, &self.saved_mode);
    }
    self.restored = true;
  }
}

impl Drop for VtSession {
  fn drop(&mut self) {
    self.restore();
    unsafe { libc::close(self.fd) };
  }
}

pub fn block_session_signals() -> std::io::Result<()> {
  unsafe {
    let mut mask: libc::sigset_t = std::mem::zeroed();
    libc::sigemptyset(&mut mask);
    for signal in [libc::SIGINT, libc::SIGTERM, libc::SIGHUP, libc::SIGQUIT, libc::SIGUSR1, libc::SIGUSR2] {
      libc::sigaddset(&mut mask, signal);
    }
    if libc::pthread_sigmask(libc::SIG_BLOCK, &mask, std::ptr::null_mut()) != 0 {
      return Err(std::io::Error::last_os_error());
    }
  }
  Ok(())
}

pub fn create_signal_fd() -> std::io::Result<RawFd> {
  block_session_signals()?;
  unsafe {
    let mut mask: libc::sigset_t = std::mem::zeroed();
    libc::sigemptyset(&mut mask);
    for signal in [libc::SIGINT, libc::SIGTERM, libc::SIGHUP, libc::SIGQUIT, libc::SIGUSR1, libc::SIGUSR2] {
      libc::sigaddset(&mut mask, signal);
    }
    let fd = libc::signalfd(-1, &mask, libc::SFD_NONBLOCK | libc::SFD_CLOEXEC);
    if fd < 0 {
      Err(std::io::Error::last_os_error())
    } else {
      Ok(fd)
    }
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn vt_mode_matches_linux_abi() {
    assert_eq!(std::mem::size_of::<VtMode>(), 8);
  }
}
