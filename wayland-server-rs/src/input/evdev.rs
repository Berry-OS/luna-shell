use crate::render::InputEvent;
use std::collections::{HashMap, HashSet};
use std::ffi::CString;
use std::mem::{size_of, size_of_val};
use std::os::unix::io::RawFd;
use std::sync::atomic::{AtomicU8, Ordering};
use std::sync::mpsc::{self, Receiver, Sender};
use std::sync::Arc;
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

const EV_SYN: u16 = 0x00;
const EV_KEY: u16 = 0x01;
const EV_REL: u16 = 0x02;
const EV_LED: u16 = 0x11;
const SYN_REPORT: u16 = 0x00;
const REL_X: u16 = 0x00;
const REL_Y: u16 = 0x01;
const REL_HWHEEL: u16 = 0x06;
const REL_WHEEL: u16 = 0x08;
const BTN_LEFT: u16 = 0x110;
const KEY_LEFTCTRL: u16 = 29;
const KEY_RIGHTCTRL: u16 = 97;
const KEY_LEFTALT: u16 = 56;
const KEY_RIGHTALT: u16 = 100;
const LED_NUML: u16 = 0x00;
const LED_CAPSL: u16 = 0x01;
const LED_SCROLLL: u16 = 0x02;
const EVIOCGRAB: libc::c_ulong = 0x4004_4590;

#[repr(C)]
#[derive(Clone, Copy)]
struct InputEventRaw {
  time: libc::timeval,
  type_: u16,
  code: u16,
  value: i32,
}

struct Device {
  fd: RawFd,
  keyboard: bool,
  pointer: bool,
  // LED output must not change the access mode of the input stream.  Some
  // evdev drivers stop delivering pointer events when their event node is
  // opened O_RDWR, so keep a separate optional descriptor for LED writes.
  led_fd: RawFd,
}

pub struct EvdevInput {
  rx: Option<Receiver<InputEvent>>,
  wake_fd: RawFd,
  wake_transferred: bool,
  stop_fd: RawFd,
  requested: Arc<AtomicU8>,
  applied: Arc<AtomicU8>,
  leds: Arc<AtomicU8>,
  thread: Option<JoinHandle<()>>,
}

impl EvdevInput {
  pub fn start() -> std::io::Result<Self> {
    let wake_fd = unsafe { libc::eventfd(0, libc::EFD_NONBLOCK | libc::EFD_CLOEXEC) };
    let stop_fd = unsafe { libc::eventfd(0, libc::EFD_NONBLOCK | libc::EFD_CLOEXEC) };
    if wake_fd < 0 || stop_fd < 0 {
      if wake_fd >= 0 {
        unsafe { libc::close(wake_fd) };
      }
      if stop_fd >= 0 {
        unsafe { libc::close(stop_fd) };
      }
      return Err(std::io::Error::last_os_error());
    }

    /* Input edges are state transitions, not optional samples.  The old
     * bounded channel used try_send() and silently discarded BTN_LEFT release
     * (and key release) whenever a render stall let motion fill its 512 slots.
     * That left the compositor's implicit/WM grab active indefinitely: Luna
     * dialogs kept following the pointer and every later click, including in
     * Firefox, was routed to the stale grab owner.
     *
     * Keep the producer non-blocking at shutdown without making events lossy:
     * an unbounded std channel disconnects cleanly and the compositor already
     * coalesces all consecutive motion in each received batch. */
    let (tx, rx) = mpsc::channel();
    let requested = Arc::new(AtomicU8::new(1));
    let applied = Arc::new(AtomicU8::new(1));
    let initial_leds = if std::env::var("LUNA_NUMLOCK")
      .map(|v| !v.is_empty() && v != "0" && !v.eq_ignore_ascii_case("false") && !v.eq_ignore_ascii_case("off"))
      .unwrap_or(true)
    { 1 } else { 0 };
    let leds = Arc::new(AtomicU8::new(initial_leds));
    let thread_requested = Arc::clone(&requested);
    let thread_applied = Arc::clone(&applied);
    let thread_leds = Arc::clone(&leds);
    let handle = thread::Builder::new().name("luna-evdev".into()).spawn(move || input_loop(tx, wake_fd, stop_fd, thread_requested, thread_applied, thread_leds))?;
    Ok(Self {
      rx: Some(rx),
      wake_fd,
      wake_transferred: false,
      stop_fd,
      requested,
      applied,
      leds,
      thread: Some(handle),
    })
  }

  pub fn take_channel(&mut self) -> Option<(Receiver<InputEvent>, RawFd)> {
    let channel = self.rx.take().map(|rx| (rx, self.wake_fd));
    if channel.is_some() {
      self.wake_transferred = true;
    }
    channel
  }

  pub fn set_active(&self, active: bool) {
    let state = if active { 1 } else { 0 };
    self.requested.store(state, Ordering::Release);
    wake(self.stop_fd);
    for _ in 0..100 {
      if self.applied.load(Ordering::Acquire) == state {
        break;
      }
      thread::sleep(Duration::from_millis(2));
    }
  }

  /// Bit 0 NumLock, bit 1 CapsLock, bit 2 ScrollLock.
  pub fn set_leds(&self, leds: u8) {
    self.leds.store(leds & 0x07, Ordering::Release);
    wake(self.stop_fd);
  }
}

impl Drop for EvdevInput {
  fn drop(&mut self) {
    self.requested.store(2, Ordering::Release);
    wake(self.stop_fd);
    if let Some(handle) = self.thread.take() {
      let _ = handle.join();
    }
    unsafe {
      libc::close(self.stop_fd);
      if !self.wake_transferred {
        libc::close(self.wake_fd);
      }
      // A transferred wake_fd is closed by Server after the thread stops.
    }
  }
}

fn input_loop(tx: Sender<InputEvent>, wake_fd: RawFd, stop_fd: RawFd, requested: Arc<AtomicU8>, applied: Arc<AtomicU8>, requested_leds: Arc<AtomicU8>) {
  let mut devices: HashMap<String, Device> = HashMap::new();
  let mut pressed = HashMap::<u16, u32>::new();
  let mut consumed_fn = HashSet::<u16>::new();
  let mut last_scan = Instant::now() - Duration::from_secs(2);
  let mut active = true;
  let mut leds = requested_leds.load(Ordering::Acquire);

  loop {
    if last_scan.elapsed() >= Duration::from_secs(1) {
      rescan(&mut devices, active, leds);
      last_scan = Instant::now();
    }

    let mut polls = Vec::with_capacity(devices.len() + 1);
    polls.push(libc::pollfd {
      fd: stop_fd,
      events: libc::POLLIN,
      revents: 0,
    });
    for dev in devices.values() {
      polls.push(libc::pollfd {
        fd: dev.fd,
        events: libc::POLLIN,
        revents: 0,
      });
    }
    let n = unsafe { libc::poll(polls.as_mut_ptr(), polls.len() as libc::nfds_t, 250) };
    if n < 0 {
      continue;
    }
    if polls[0].revents & libc::POLLIN != 0 {
      let mut value = 0u64;
      unsafe { libc::read(stop_fd, &mut value as *mut u64 as *mut libc::c_void, size_of::<u64>()) };
      let state = requested.load(Ordering::Acquire);
      if state == 2 {
        break;
      }
      let want_active = state == 1;
      if want_active != active {
        set_devices_grabbed(&devices, want_active);
        pressed.clear();
        consumed_fn.clear();
        emit(&tx, wake_fd, InputEvent::Reset);
        active = want_active;
        applied.store(state, Ordering::Release);
      }
      let want_leds = requested_leds.load(Ordering::Acquire);
      if want_leds != leds {
        leds = want_leds;
        set_devices_leds(&devices, leds);
      }
      continue;
    }

    let mut dead = HashSet::new();
    for poll in polls.iter().skip(1).filter(|p| p.revents != 0) {
      let Some((path, dev)) = devices.iter().find(|(_, d)| d.fd == poll.fd) else { continue };
      if poll.revents & (libc::POLLERR | libc::POLLHUP | libc::POLLNVAL) != 0 {
        dead.insert(path.clone());
        continue;
      }
      let mut events = [InputEventRaw {
        time: libc::timeval {
          tv_sec: 0,
          tv_usec: 0,
        },
        type_: 0,
        code: 0,
        value: 0,
      }; 32];
      let bytes = unsafe { libc::read(dev.fd, events.as_mut_ptr() as *mut libc::c_void, size_of::<InputEventRaw>() * events.len()) };
      if bytes <= 0 {
        if bytes < 0 && std::io::Error::last_os_error().kind() != std::io::ErrorKind::WouldBlock {
          dead.insert(path.clone());
        }
        continue;
      }
      if applied.load(Ordering::Acquire) == 0 {
        continue;
      }
      // A diagonal mouse move arrives as REL_X and REL_Y in the *same* evdev
      // report, and one read returns as many reports as the kernel had queued.
      // Forwarding each axis separately turned a 1 kHz mouse into 2000 channel
      // sends, 2000 eventfd writes and 2000 compositor wake-ups a second, each
      // one running a full pointer-focus hit test and flushing a client socket.
      // Accumulate within a report and emit one motion per SYN_REPORT; the
      // trailing flush covers a device that omits the terminating SYN.
      let mut acc_dx = 0.0f32;
      let mut acc_dy = 0.0f32;
      let flush_motion = |dx: &mut f32, dy: &mut f32| {
        if *dx != 0.0 || *dy != 0.0 {
          emit(&tx, wake_fd, InputEvent::PointerRelative { dx: *dx, dy: *dy });
          *dx = 0.0;
          *dy = 0.0;
        }
      };
      for raw in &events[..bytes as usize / size_of::<InputEventRaw>()] {
        if raw.type_ == EV_SYN && raw.code == SYN_REPORT {
          flush_motion(&mut acc_dx, &mut acc_dy);
        } else if dev.keyboard && raw.type_ == EV_KEY && raw.code < BTN_LEFT {
          handle_key(raw.code, raw.value, &mut pressed, &mut consumed_fn, &tx, wake_fd);
        } else if dev.pointer && raw.type_ == EV_KEY && raw.code >= BTN_LEFT {
          // A button edge must not overtake the motion that preceded it.
          flush_motion(&mut acc_dx, &mut acc_dy);
          emit(
            &tx,
            wake_fd,
            InputEvent::PointerButton {
              button: raw.code as u32,
              pressed: raw.value != 0,
            },
          );
        } else if dev.pointer && raw.type_ == EV_REL {
          match raw.code {
            REL_X => acc_dx += raw.value as f32,
            REL_Y => acc_dy += raw.value as f32,
            REL_WHEEL => {
              flush_motion(&mut acc_dx, &mut acc_dy);
              emit(
                &tx,
                wake_fd,
                InputEvent::PointerAxis {
                  axis: 0,
                  value: -(raw.value as f32) * 10.0,
                },
              );
            }
            REL_HWHEEL => {
              flush_motion(&mut acc_dx, &mut acc_dy);
              emit(
                &tx,
                wake_fd,
                InputEvent::PointerAxis {
                  axis: 1,
                  value: -(raw.value as f32) * 10.0,
                },
              );
            }
            _ => {}
          }
        }
      }
      flush_motion(&mut acc_dx, &mut acc_dy);
    }
    if !dead.is_empty() {
      for path in dead {
        if let Some(dev) = devices.remove(&path) {
          close_device(dev);
        }
      }
      pressed.clear();
      consumed_fn.clear();
      emit(&tx, wake_fd, InputEvent::Reset);
    }
  }

  for (_, dev) in devices {
    close_device(dev);
  }
}

fn handle_key(code: u16, value: i32, pressed: &mut HashMap<u16, u32>, consumed_fn: &mut HashSet<u16>, tx: &Sender<InputEvent>, wake_fd: RawFd) {
  if value == 2 {
    // Kernel auto-repeat.  Clients synthesize repeats from wl_keyboard.repeat_info.
    return;
  }
  let down = value != 0;
  let old_count = pressed.get(&code).copied().unwrap_or(0);
  if down {
    pressed.insert(code, old_count.saturating_add(1));
  } else if old_count <= 1 {
    pressed.remove(&code);
  } else {
    pressed.insert(code, old_count - 1);
  }

  if down && vt_number(code).is_some() {
    let ctrl = pressed.contains_key(&KEY_LEFTCTRL) || pressed.contains_key(&KEY_RIGHTCTRL);
    let alt = pressed.contains_key(&KEY_LEFTALT) || pressed.contains_key(&KEY_RIGHTALT);
    if ctrl && alt {
      consumed_fn.insert(code);
      emit(tx, wake_fd, InputEvent::VtSwitch(vt_number(code).unwrap()));
      return;
    }
  }
  if !down && consumed_fn.remove(&code) {
    return;
  }
  let modifier = matches!(code, KEY_LEFTCTRL | KEY_RIGHTCTRL | KEY_LEFTALT | KEY_RIGHTALT | 42 | 54);
  if modifier && ((down && old_count > 0) || (!down && old_count > 1)) {
    return;
  }
  emit(
    tx,
    wake_fd,
    InputEvent::Key {
      keycode: code as u32,
      pressed: down,
    },
  );
}

fn vt_number(code: u16) -> Option<u8> {
  match code {
    59..=68 => Some((code - 58) as u8),
    87 => Some(11),
    88 => Some(12),
    _ => None,
  }
}

fn emit(tx: &Sender<InputEvent>, wake_fd: RawFd, event: InputEvent) {
  if tx.send(event).is_ok() {
    let one: u64 = 1;
    unsafe {
      libc::write(wake_fd, &one as *const u64 as *const libc::c_void, size_of::<u64>());
    }
  }
}

fn wake(fd: RawFd) {
  let one: u64 = 1;
  unsafe {
    libc::write(fd, &one as *const u64 as *const libc::c_void, size_of::<u64>());
  }
}

fn set_devices_grabbed(devices: &HashMap<String, Device>, grabbed: bool) {
  for dev in devices.values() {
    unsafe { libc::ioctl(dev.fd, EVIOCGRAB, if grabbed { 1 } else { 0 }) };
  }
}

fn close_device(dev: Device) {
  unsafe {
    libc::ioctl(dev.fd, EVIOCGRAB, 0);
    libc::close(dev.fd);
    if dev.led_fd >= 0 {
      libc::close(dev.led_fd);
    }
  }
}

fn rescan(devices: &mut HashMap<String, Device>, active: bool, leds: u8) {
  let Ok(entries) = std::fs::read_dir("/dev/input") else { return };
  for entry in entries.flatten() {
    let name = entry.file_name();
    if !name.to_string_lossy().starts_with("event") {
      continue;
    }
    let path = entry.path().to_string_lossy().into_owned();
    if devices.contains_key(&path) {
      continue;
    }
    let Ok(cpath) = CString::new(path.as_bytes()) else { continue };
    let fd = unsafe { libc::open(cpath.as_ptr(), libc::O_RDONLY | libc::O_NONBLOCK | libc::O_CLOEXEC) };
    if fd < 0 {
      continue;
    }
    let keyboard = has_bit(fd, EV_KEY as u32, 30) && has_bit(fd, EV_KEY as u32, 28);
    let pointer = (has_bit(fd, EV_REL as u32, REL_X as usize) && has_bit(fd, EV_REL as u32, REL_Y as usize)) || has_bit(fd, EV_KEY as u32, BTN_LEFT as usize);
    if !keyboard && !pointer {
      unsafe { libc::close(fd) };
      continue;
    }
    // Grabbing prevents typed console input from being interpreted after Luna exits.
    if active && unsafe { libc::ioctl(fd, EVIOCGRAB, 1) } != 0 {
      eprintln!("[luna-compositor] input: cannot grab {}: {}", path, std::io::Error::last_os_error());
      unsafe { libc::close(fd) };
      continue;
    }
    eprintln!("[luna-compositor] input: {} keyboard={} pointer={}", path, keyboard, pointer);
    let led_fd = if keyboard {
      unsafe { libc::open(cpath.as_ptr(), libc::O_RDWR | libc::O_NONBLOCK | libc::O_CLOEXEC) }
    } else {
      -1
    };
    if led_fd >= 0 {
      write_leds(led_fd, leds);
    }
    devices.insert(
      path,
      Device {
        fd,
        keyboard,
        pointer,
        led_fd,
      },
    );
  }
}

fn set_devices_leds(devices: &HashMap<String, Device>, leds: u8) {
  for dev in devices.values().filter(|dev| dev.led_fd >= 0) {
    write_leds(dev.led_fd, leds);
  }
}

fn write_leds(fd: RawFd, leds: u8) {
  let zero = libc::timeval { tv_sec: 0, tv_usec: 0 };
  let events = [
    InputEventRaw { time: zero, type_: EV_LED, code: LED_NUML, value: (leds & 1 != 0) as i32 },
    InputEventRaw { time: zero, type_: EV_LED, code: LED_CAPSL, value: (leds & 2 != 0) as i32 },
    InputEventRaw { time: zero, type_: EV_LED, code: LED_SCROLLL, value: (leds & 4 != 0) as i32 },
    InputEventRaw { time: zero, type_: EV_SYN, code: SYN_REPORT, value: 0 },
  ];
  unsafe {
    libc::write(fd, events.as_ptr() as *const libc::c_void, size_of_val(&events));
  }
}

fn has_bit(fd: RawFd, ev: u32, bit: usize) -> bool {
  let mut bits = [0u8; 96];
  let request = ioc_read(b'E' as u64, 0x20 + ev as u64, bits.len() as u64);
  if unsafe { libc::ioctl(fd, request as libc::c_ulong, bits.as_mut_ptr()) } < 0 {
    return false;
  }
  bit / 8 < bits.len() && bits[bit / 8] & (1 << (bit % 8)) != 0
}

const fn ioc_read(ty: u64, nr: u64, size: u64) -> u64 { (2u64 << 30) | (size << 16) | (ty << 8) | nr }

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn maps_function_keys_to_vts() {
    assert_eq!(vt_number(59), Some(1));
    assert_eq!(vt_number(68), Some(10));
    assert_eq!(vt_number(87), Some(11));
    assert_eq!(vt_number(88), Some(12));
    assert_eq!(vt_number(57), None);
  }

  #[test]
  fn ctrl_alt_function_key_is_consumed_as_vt_switch() {
    let (tx, rx) = mpsc::channel();
    let wake_fd = unsafe { libc::eventfd(0, libc::EFD_NONBLOCK | libc::EFD_CLOEXEC) };
    let mut pressed = HashMap::new();
    let mut consumed = HashSet::new();
    handle_key(KEY_LEFTCTRL, 1, &mut pressed, &mut consumed, &tx, wake_fd);
    handle_key(KEY_LEFTALT, 1, &mut pressed, &mut consumed, &tx, wake_fd);
    handle_key(61, 1, &mut pressed, &mut consumed, &tx, wake_fd);
    handle_key(61, 0, &mut pressed, &mut consumed, &tx, wake_fd);

    assert!(matches!(
      rx.recv().unwrap(),
      InputEvent::Key {
        keycode: 29,
        pressed: true
      }
    ));
    assert!(matches!(
      rx.recv().unwrap(),
      InputEvent::Key {
        keycode: 56,
        pressed: true
      }
    ));
    assert!(matches!(rx.recv().unwrap(), InputEvent::VtSwitch(3)));
    assert!(rx.try_recv().is_err());
    unsafe { libc::close(wake_fd) };
  }

  #[test]
  fn input_burst_never_drops_button_release() {
    let (tx, rx) = mpsc::channel();
    let wake_fd = unsafe { libc::eventfd(0, libc::EFD_NONBLOCK | libc::EFD_CLOEXEC) };
    for _ in 0..1024 {
      emit(&tx, wake_fd, InputEvent::PointerRelative { dx: 1.0, dy: 0.0 });
    }
    emit(
      &tx,
      wake_fd,
      InputEvent::PointerButton {
        button: BTN_LEFT as u32,
        pressed: false,
      },
    );

    for _ in 0..1024 {
      assert!(matches!(rx.recv().unwrap(), InputEvent::PointerRelative { .. }));
    }
    assert!(matches!(
      rx.recv().unwrap(),
      InputEvent::PointerButton {
        button: 0x110,
        pressed: false
      }
    ));
    unsafe { libc::close(wake_fd) };
  }
}
