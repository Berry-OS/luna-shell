/*
 * Copyright © 2026 Yuichiro Nakada / Project Vespera
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

//! Minimal X11 window-manager client for a rootless Xwayland.
//!
//! No libxcb: the wire protocol subset a rootless WM needs is small.  The
//! connection is the pre-authorised descriptor handed to `Xwayland -wm FD`.
//! Everything is non-blocking and driven by the compositor's epoll loop; the
//! compositor owns policy (placement, stacking, focus), this module only
//! speaks X and reports what happened.

use std::collections::HashMap;
use std::os::unix::io::RawFd;

// Predefined atoms (X11 core protocol).
const A_ATOM: u32 = 4;
const A_CARDINAL: u32 = 6;
const A_STRING: u32 = 31;
const A_WINDOW: u32 = 33;
const A_WM_NAME: u32 = 39;
const A_WM_CLASS: u32 = 67;
const A_WM_TRANSIENT_FOR: u32 = 68;

// Event masks.
const EM_STRUCTURE_NOTIFY: u32 = 1 << 17;
const EM_SUBSTRUCTURE_NOTIFY: u32 = 1 << 19;
const EM_SUBSTRUCTURE_REDIRECT: u32 = 1 << 20;
const EM_PROPERTY_CHANGE: u32 = 1 << 22;

macro_rules! atoms {
  ($($id:ident = $name:expr),* $(,)?) => {
    #[allow(non_camel_case_types, clippy::upper_case_acronyms)]
    #[derive(Clone, Copy)]
    #[repr(usize)]
    enum At { $($id),* }
    const ATOM_NAMES: &[&str] = &[$($name),*];
  };
}

atoms! {
  WM_PROTOCOLS = "WM_PROTOCOLS",
  WM_DELETE_WINDOW = "WM_DELETE_WINDOW",
  WM_STATE = "WM_STATE",
  NET_WM_NAME = "_NET_WM_NAME",
  UTF8_STRING = "UTF8_STRING",
  NET_WM_WINDOW_TYPE = "_NET_WM_WINDOW_TYPE",
  TYPE_NORMAL = "_NET_WM_WINDOW_TYPE_NORMAL",
  TYPE_DIALOG = "_NET_WM_WINDOW_TYPE_DIALOG",
  TYPE_UTILITY = "_NET_WM_WINDOW_TYPE_UTILITY",
  TYPE_SPLASH = "_NET_WM_WINDOW_TYPE_SPLASH",
  WL_SURFACE_ID = "WL_SURFACE_ID",
  MOTIF_WM_HINTS = "_MOTIF_WM_HINTS",
  GTK_FRAME_EXTENTS = "_GTK_FRAME_EXTENTS",
  NET_SUPPORTING_WM_CHECK = "_NET_SUPPORTING_WM_CHECK",
  NET_TRAY = "_NET_SYSTEM_TRAY_S0",
  NET_TRAY_OPCODE = "_NET_SYSTEM_TRAY_OPCODE",
  NET_TRAY_ORIENTATION = "_NET_SYSTEM_TRAY_ORIENTATION",
  NET_TRAY_VISUAL = "_NET_SYSTEM_TRAY_VISUAL",
  MANAGER = "MANAGER",
  XEMBED = "_XEMBED",
}

/// Window kinds a mapped, non-override-redirect window can have.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Kind {
  /// Ordinary managed top-level.
  Normal,
  /// Dialog / utility / splash: managed, but centred on its parent.
  Dialog,
  /// Menu, tooltip, dnd, …: placed exactly where the client asked.
  Popup,
}

#[derive(Default, Clone, Debug)]
pub struct XWin {
  pub x: i32,
  pub y: i32,
  pub w: u32,
  pub h: u32,
  pub override_redirect: bool,
  pub mapped: bool,
  pub title: String,
  pub app_id: String,
  pub transient_for: u32,
  pub types: Vec<u32>,
  pub delete_ok: bool,
  /// The client draws its own decorations (GTK CSD / Motif decorations=0).
  pub csd: bool,
  /// `_GTK_FRAME_EXTENTS`: left, right, top, bottom shadow around the content.
  pub frame: [i32; 4],
  /// wl_surface id (in Xwayland's client) once `WL_SURFACE_ID` arrived.
  pub surface: u32,
  /// Tray icon container, if this window was docked.
  pub tray_container: u32,
}

#[derive(Debug)]
pub enum XEvent {
  /// A window became known / changed shape (override-redirect windows move
  /// themselves; managed ones are placed by the compositor).
  Geometry { win: u32 },
  MapRequest { win: u32 },
  Mapped { win: u32 },
  Unmapped { win: u32 },
  Destroyed { win: u32 },
  SurfaceId { win: u32, surface: u32 },
  /// Title / class / transient-for / type changed.
  Props { win: u32 },
  /// `_NET_SYSTEM_TRAY_OPCODE` dock request; `icon` is already reparented into
  /// `container` and mapped.
  TrayDocked { icon: u32, container: u32 },
  /// A docked icon went away.
  TrayGone { icon: u32, container: u32 },
}

enum Pending {
  /// QueryExtension("Composite"): Xwayland creates surfaces only for windows
  /// the WM has redirected (manual), so this is what makes windows appear.
  Composite,
  Atom(usize),
  Prop(u32, u32), // window, property atom
}

pub struct Xwm {
  fd: RawFd,
  inbuf: Vec<u8>,
  outbuf: Vec<u8>,
  seq: u16,
  pending: HashMap<u16, Pending>,
  atoms: [u32; ATOM_NAMES.len()],
  atoms_left: usize,
  setup_done: bool,
  pub ready: bool,
  root: u32,
  id_base: u32,
  id_mask: u32,
  id_next: u32,
  pub screen_w: u32,
  pub screen_h: u32,
  tray_win: u32,
  /// Background of tray containers: the icon is opaque, so it should match the bar.
  tray_bg: u32,
  /// 32-bit TrueColor visual (0 = none) and its colormap: lets tray icons be translucent.
  argb_visual: u32,
  argb_cmap: u32,
  pub windows: HashMap<u32, XWin>,
  /// icon window -> container window
  tray: HashMap<u32, u32>,
  pub closed: bool,
}

fn trace() -> bool {
  static T: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
  *T.get_or_init(|| std::env::var_os("LUNA_XWL_TRACE").is_some())
}

fn pad4(n: usize) -> usize { (4 - n % 4) % 4 }

impl Xwm {
  pub fn new(fd: RawFd) -> Self {
    unsafe {
      let fl = libc::fcntl(fd, libc::F_GETFL);
      libc::fcntl(fd, libc::F_SETFL, fl | libc::O_NONBLOCK);
    }
    let mut x = Xwm {
      fd,
      inbuf: Vec::new(),
      outbuf: Vec::new(),
      seq: 0,
      pending: HashMap::new(),
      atoms: [0; ATOM_NAMES.len()],
      atoms_left: ATOM_NAMES.len(),
      setup_done: false,
      ready: false,
      root: 0,
      id_base: 0,
      id_mask: 0,
      id_next: 0,
      screen_w: 0,
      screen_h: 0,
      tray_win: 0,
      tray_bg: 0x0b0c18,
      argb_visual: 0,
      argb_cmap: 0,
      windows: HashMap::new(),
      tray: HashMap::new(),
      closed: false,
    };
    // Connection setup: little-endian, protocol 11.0, no authorisation.
    x.outbuf.extend_from_slice(&[b'l', 0, 11, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0][..12]);
    x.flush();
    x
  }

  pub fn fd(&self) -> RawFd { self.fd }

  // ---- request plumbing -------------------------------------------------

  fn req(&mut self, opcode: u8, data: u8, body: &[u8]) -> u16 {
    let len = (4 + body.len()) / 4;
    self.outbuf.push(opcode);
    self.outbuf.push(data);
    self.outbuf.extend_from_slice(&(len as u16).to_le_bytes());
    self.outbuf.extend_from_slice(body);
    self.seq = self.seq.wrapping_add(1);
    self.seq
  }

  pub fn flush(&mut self) {
    while !self.outbuf.is_empty() {
      let n = unsafe { libc::write(self.fd, self.outbuf.as_ptr() as *const libc::c_void, self.outbuf.len()) };
      if n > 0 {
        self.outbuf.drain(..n as usize);
      } else {
        let e = std::io::Error::last_os_error();
        if n < 0 && e.kind() != std::io::ErrorKind::WouldBlock && e.kind() != std::io::ErrorKind::Interrupted {
          self.closed = true;
          self.outbuf.clear();
        }
        break;
      }
    }
  }

  fn alloc_id(&mut self) -> u32 {
    let id = self.id_base | (self.id_next & self.id_mask);
    self.id_next += 1;
    id
  }

  fn atom(&self, a: At) -> u32 { self.atoms[a as usize] }

  fn u32s(v: &[u32]) -> Vec<u8> { v.iter().flat_map(|x| x.to_le_bytes()).collect() }

  // ---- requests -----------------------------------------------------------

  fn intern(&mut self, idx: usize) {
    let name = ATOM_NAMES[idx].as_bytes();
    let mut b = Vec::new();
    b.extend_from_slice(&(name.len() as u16).to_le_bytes());
    b.extend_from_slice(&[0, 0]);
    b.extend_from_slice(name);
    b.resize(b.len() + pad4(name.len()), 0);
    let seq = self.req(16, 0, &b);
    self.pending.insert(seq, Pending::Atom(idx));
  }

  fn change_attributes(&mut self, win: u32, event_mask: u32) {
    self.req(2, 0, &Self::u32s(&[win, 0x800, event_mask]));
  }

  pub fn map(&mut self, win: u32) { self.req(8, 0, &Self::u32s(&[win])); }

  fn get_property(&mut self, win: u32, prop: u32, ty: u32) {
    let seq = self.req(20, 0, &Self::u32s(&[win, prop, ty, 0, 256]));
    self.pending.insert(seq, Pending::Prop(win, prop));
  }

  fn refresh_props(&mut self, win: u32) {
    self.get_property(win, self.atom(At::NET_WM_NAME), self.atom(At::UTF8_STRING));
    self.get_property(win, A_WM_NAME, A_STRING);
    self.get_property(win, A_WM_CLASS, A_STRING);
    self.get_property(win, A_WM_TRANSIENT_FOR, A_WINDOW);
    self.get_property(win, self.atom(At::NET_WM_WINDOW_TYPE), A_ATOM);
    self.get_property(win, self.atom(At::WM_PROTOCOLS), A_ATOM);
    self.get_property(win, self.atom(At::MOTIF_WM_HINTS), 0);
    self.get_property(win, self.atom(At::GTK_FRAME_EXTENTS), A_CARDINAL);
  }

  /// ConfigureWindow with only the fields given.
  pub fn configure(&mut self, win: u32, x: Option<i32>, y: Option<i32>, w: Option<u32>, h: Option<u32>) {
    let mut mask = 0u16;
    let mut vals = Vec::new();
    if let Some(v) = x { mask |= 1; vals.push(v as u32); }
    if let Some(v) = y { mask |= 2; vals.push(v as u32); }
    if let Some(v) = w { mask |= 4; vals.push(v); }
    if let Some(v) = h { mask |= 8; vals.push(v); }
    if mask == 0 { return; }
    let mut b = win.to_le_bytes().to_vec();
    b.extend_from_slice(&mask.to_le_bytes());
    b.extend_from_slice(&[0, 0]);
    b.extend_from_slice(&Self::u32s(&vals));
    self.req(12, 0, &b);
    if let Some(wn) = self.windows.get_mut(&win) {
      if let Some(v) = x { wn.x = v; }
      if let Some(v) = y { wn.y = v; }
      if let Some(v) = w { wn.w = v; }
      if let Some(v) = h { wn.h = v; }
    }
  }

  fn send_event(&mut self, dest: u32, mask: u32, ev: &[u8; 32]) {
    let mut b = Self::u32s(&[dest, mask]);
    b.extend_from_slice(ev);
    self.req(25, 0, &b);
  }

  fn client_message(&mut self, dest: u32, mask: u32, win: u32, ty: u32, data: [u32; 5]) {
    let mut ev = [0u8; 32];
    ev[0] = 33;
    ev[1] = 32;
    ev[4..8].copy_from_slice(&win.to_le_bytes());
    ev[8..12].copy_from_slice(&ty.to_le_bytes());
    for (i, d) in data.iter().enumerate() {
      ev[12 + i * 4..16 + i * 4].copy_from_slice(&d.to_le_bytes());
    }
    self.send_event(dest, mask, &ev);
  }

  /// ICCCM: tell a client its real geometry after the WM moved it.
  fn synthetic_configure_notify(&mut self, win: u32) {
    let Some(w) = self.windows.get(&win) else { return };
    let (x, y, ww, hh) = (w.x, w.y, w.w, w.h);
    let mut ev = [0u8; 32];
    ev[0] = 22;
    ev[4..8].copy_from_slice(&win.to_le_bytes());
    ev[8..12].copy_from_slice(&win.to_le_bytes());
    ev[16..18].copy_from_slice(&(x as i16).to_le_bytes());
    ev[18..20].copy_from_slice(&(y as i16).to_le_bytes());
    ev[20..22].copy_from_slice(&(ww as u16).to_le_bytes());
    ev[22..24].copy_from_slice(&(hh as u16).to_le_bytes());
    self.send_event(win, 0x20000, &ev);
  }

  /// Ask the window to close; falls back to killing the client.
  pub fn close(&mut self, win: u32) {
    let delete_ok = self.windows.get(&win).map(|w| w.delete_ok).unwrap_or(false);
    if delete_ok {
      self.client_message(win, 0, win, self.atom(At::WM_PROTOCOLS), [self.atom(At::WM_DELETE_WINDOW), 0, 0, 0, 0]);
    } else {
      self.req(113, 0, &Self::u32s(&[win]));
    }
  }

  pub fn focus(&mut self, win: u32) {
    // revert-to = PointerRoot(1), CurrentTime
    self.req(42, 1, &Self::u32s(&[win, 0]));
  }

  /// ICCCM WM_STATE = NormalState; some toolkits wait for it.
  fn set_wm_state(&mut self, win: u32, state: u32) {
    let st = self.atom(At::WM_STATE);
    let mut b = Self::u32s(&[win, st, st]);
    b.extend_from_slice(&[32, 0, 0, 0]);
    b.extend_from_slice(&2u32.to_le_bytes());
    b.extend_from_slice(&Self::u32s(&[state, 0]));
    self.req(18, 0, &b);
  }

  /// Grant a MapRequest.
  pub fn grant_map(&mut self, win: u32) {
    self.set_wm_state(win, 1);
    self.map(win);
  }

  /// Answer a ConfigureRequest from a managed window: keep the position the
  /// compositor chose, accept the size, and confirm with a synthetic event.
  pub fn grant_configure(&mut self, win: u32, w: Option<u32>, h: Option<u32>) {
    if let Some(wn) = self.windows.get_mut(&win) {
      if let Some(v) = w { wn.w = v; }
      if let Some(v) = h { wn.h = v; }
    }
    let (x, y) = self.windows.get(&win).map(|w| (w.x, w.y)).unwrap_or((0, 0));
    self.configure(win, Some(x), Some(y), w, h);
    self.synthetic_configure_notify(win);
  }

  /// Compositor moved a managed window: tell X (and the client).
  pub fn place(&mut self, win: u32, x: i32, y: i32) {
    let moved = self.windows.get(&win).map(|w| w.x != x || w.y != y).unwrap_or(false);
    if moved {
      self.configure(win, Some(x), Some(y), None, None);
      self.synthetic_configure_notify(win);
    }
  }

  /// Compositor resized a managed window.
  pub fn resize(&mut self, win: u32, w: u32, h: u32) {
    let same = self.windows.get(&win).map(|x| x.w == w && x.h == h).unwrap_or(true);
    if !same {
      self.configure(win, None, None, Some(w), Some(h));
      self.synthetic_configure_notify(win);
    }
  }

  /// Recolour the container backgrounds (RGB) and repaint them.
  pub fn set_tray_bg(&mut self, rgb: u32) {
    if rgb == self.tray_bg || self.argb_cmap != 0 {
      return;
    }
    self.tray_bg = rgb;
    let containers: Vec<u32> = self.tray.values().copied().collect();
    for c in containers {
      self.req(2, 0, &Self::u32s(&[c, 0x2, rgb]));
      self.req(61, 1, &Self::u32s(&[c, 0, 0]));
    }
  }

  /// Put a docked tray icon's container at a screen rectangle (the compositor
  /// draws the container's surface there; the icon fills it).
  pub fn place_tray(&mut self, icon: u32, x: i32, y: i32, w: u32, h: u32) {
    let Some(&container) = self.tray.get(&icon) else { return };
    self.configure(container, Some(x), Some(y), Some(w), Some(h));
    self.configure(icon, Some(0), Some(0), Some(w), Some(h));
  }

  // ---- setup --------------------------------------------------------------

  fn finish_setup(&mut self) {
    self.ready = true;
    self.change_attributes(
      self.root,
      EM_SUBSTRUCTURE_REDIRECT | EM_SUBSTRUCTURE_NOTIFY | EM_PROPERTY_CHANGE,
    );
    // Root cursor: the arrow, not the X server's default "X".  Xwayland maps
    // this glyph to the user's Xcursor theme.
    let font = self.alloc_id();
    let cursor = self.alloc_id();
    let mut f = font.to_le_bytes().to_vec();
    f.extend_from_slice(&6u16.to_le_bytes());
    f.extend_from_slice(&[0, 0]);
    f.extend_from_slice(b"cursor\0\0");
    self.req(45, 0, &f);
    let mut c = Self::u32s(&[cursor, font, font]);
    c.extend_from_slice(&68u16.to_le_bytes()); // XC_left_ptr
    c.extend_from_slice(&69u16.to_le_bytes()); // its mask
    for v in [0u16, 0, 0, 0xffff, 0xffff, 0xffff] {
      c.extend_from_slice(&v.to_le_bytes());
    }
    self.req(94, 0, &c);
    self.req(2, 0, &Self::u32s(&[self.root, 0x4000, cursor]));
    let name = b"Composite";
    let mut b = Vec::new();
    b.extend_from_slice(&(name.len() as u16).to_le_bytes());
    b.extend_from_slice(&[0, 0]);
    b.extend_from_slice(name);
    b.resize(b.len() + pad4(name.len()), 0);
    let seq = self.req(98, 0, &b);
    self.pending.insert(seq, Pending::Composite);
    for i in 0..ATOM_NAMES.len() {
      self.intern(i);
    }
  }

  /// Runs once every atom reply is in: advertise ourselves and own the tray.
  fn atoms_ready(&mut self) {
    if trace() {
      eprintln!("[luna-xwl] atoms ready {:?}", self.atoms);
    }
    // _NET_SUPPORTING_WM_CHECK on root pointing at a dummy window, as EWMH
    // requires of a WM; toolkits probe for it before trusting the WM.
    let check = self.alloc_id();
    let mut b = vec![0u8, 0, 0, 0]; // depth copy-from-parent
    b.truncate(0);
    b.extend_from_slice(&Self::u32s(&[check, self.root]));
    b.extend_from_slice(&0i16.to_le_bytes());
    b.extend_from_slice(&0i16.to_le_bytes());
    b.extend_from_slice(&1u16.to_le_bytes());
    b.extend_from_slice(&1u16.to_le_bytes());
    b.extend_from_slice(&0u16.to_le_bytes());
    b.extend_from_slice(&2u16.to_le_bytes()); // InputOnly
    b.extend_from_slice(&0u32.to_le_bytes()); // visual: copy
    b.extend_from_slice(&0x200u32.to_le_bytes()); // override-redirect
    b.extend_from_slice(&1u32.to_le_bytes());
    self.req(1, 0, &b);
    let prop = self.atom(At::NET_SUPPORTING_WM_CHECK);
    for w in [self.root, check] {
      let mut p = Self::u32s(&[w, prop, A_WINDOW]);
      p.extend_from_slice(&[32, 0, 0, 0]);
      p.extend_from_slice(&1u32.to_le_bytes());
      p.extend_from_slice(&check.to_le_bytes());
      self.req(18, 0, &p);
    }
    // System tray: owner of _NET_SYSTEM_TRAY_S0.
    self.tray_win = check;
    if self.argb_visual != 0 {
      self.argb_cmap = self.alloc_id();
      self.req(78, 0, &Self::u32s(&[self.argb_cmap, self.root, self.argb_visual]));
      let v = self.atom(At::NET_TRAY_VISUAL);
      let mut p = Self::u32s(&[check, v, 32]); // type VISUALID
      p.extend_from_slice(&[32, 0, 0, 0]);
      p.extend_from_slice(&1u32.to_le_bytes());
      p.extend_from_slice(&self.argb_visual.to_le_bytes());
      self.req(18, 0, &p);
    }
    self.change_attributes(check, EM_STRUCTURE_NOTIFY);
    self.req(22, 0, &Self::u32s(&[check, self.atom(At::NET_TRAY), 0]));
    self.client_message(
      self.root,
      EM_SUBSTRUCTURE_NOTIFY,
      self.root,
      self.atom(At::MANAGER),
      [0, self.atom(At::NET_TRAY), check, 0, 0],
    );
    let o = self.atom(At::NET_TRAY_ORIENTATION);
    let mut p = Self::u32s(&[check, o, A_CARDINAL]);
    p.extend_from_slice(&[32, 0, 0, 0]);
    p.extend_from_slice(&1u32.to_le_bytes());
    p.extend_from_slice(&0u32.to_le_bytes()); // horizontal
    self.req(18, 0, &p);
  }

  // ---- tray ---------------------------------------------------------------

  fn dock(&mut self, icon: u32, out: &mut Vec<XEvent>) {
    if self.tray.contains_key(&icon) {
      return;
    }
    let container = self.alloc_id();
    let mut b = Self::u32s(&[container, self.root]);
    b.extend_from_slice(&(-64i16).to_le_bytes());
    b.extend_from_slice(&(-64i16).to_le_bytes());
    b.extend_from_slice(&24u16.to_le_bytes());
    b.extend_from_slice(&24u16.to_le_bytes());
    b.extend_from_slice(&0u16.to_le_bytes());
    b.extend_from_slice(&1u16.to_le_bytes()); // InputOutput
    let depth = if self.argb_cmap != 0 {
      // Translucent icons: 32-bit window, transparent background.  A window
      // whose depth differs from its parent needs an explicit border pixel
      // and colormap.
      b.extend_from_slice(&self.argb_visual.to_le_bytes());
      b.extend_from_slice(&(0x2u32 | 0x8 | 0x200 | 0x800 | 0x2000).to_le_bytes());
      b.extend_from_slice(&Self::u32s(&[0, 0, 1, EM_SUBSTRUCTURE_NOTIFY | EM_STRUCTURE_NOTIFY, self.argb_cmap]));
      32u8
    } else {
      b.extend_from_slice(&0u32.to_le_bytes());
      b.extend_from_slice(&(0x2u32 | 0x200 | 0x800).to_le_bytes()); // back-pixel | override-redirect | event-mask
      b.extend_from_slice(&Self::u32s(&[self.tray_bg, 1, EM_SUBSTRUCTURE_NOTIFY | EM_STRUCTURE_NOTIFY]));
      0u8
    };
    self.req(1, depth, &b);
    self.windows.insert(
      container,
      XWin { x: -64, y: -64, w: 24, h: 24, override_redirect: true, tray_container: 0, ..Default::default() },
    );
    // Reparent the icon into the container, size it, and let it know.
    self.req(7, 0, &Self::u32s(&[icon, container, 0]));
    self.change_attributes(icon, EM_STRUCTURE_NOTIFY | EM_PROPERTY_CHANGE);
    self.configure(icon, Some(0), Some(0), Some(24), Some(24));
    self.client_message(icon, 0, icon, self.atom(At::XEMBED), [0, 0, 0, container, 0]);
    self.map(icon);
    self.map(container);
    self.tray.insert(icon, container);
    self.windows.entry(icon).or_default().tray_container = container;
    out.push(XEvent::TrayDocked { icon, container });
  }

  // ---- reading ------------------------------------------------------------

  /// Drain the socket and decode.  Returns false once the connection is gone.
  pub fn on_readable(&mut self, out: &mut Vec<XEvent>) -> bool {
    let mut tmp = [0u8; 8192];
    loop {
      let n = unsafe { libc::read(self.fd, tmp.as_mut_ptr() as *mut libc::c_void, tmp.len()) };
      if n > 0 {
        self.inbuf.extend_from_slice(&tmp[..n as usize]);
      } else if n == 0 {
        self.closed = true;
        break;
      } else {
        let e = std::io::Error::last_os_error();
        if e.kind() == std::io::ErrorKind::Interrupted {
          continue;
        }
        if e.kind() != std::io::ErrorKind::WouldBlock {
          self.closed = true;
        }
        break;
      }
    }
    self.decode(out);
    self.flush();
    !self.closed
  }

  fn decode(&mut self, out: &mut Vec<XEvent>) {
    let mut off = 0usize;
    loop {
      let buf = &self.inbuf[off..];
      if !self.setup_done {
        if buf.len() < 8 {
          break;
        }
        let total = 8 + 4 * u16::from_le_bytes([buf[6], buf[7]]) as usize;
        if buf.len() < total {
          break;
        }
        let ok = buf[0] == 1;
        let block = buf[..total].to_vec();
        off += total;
        if !ok {
          eprintln!("[luna-compositor] xwm: connection refused");
          self.closed = true;
          break;
        }
        self.parse_setup(&block);
        self.setup_done = true;
        self.finish_setup();
        continue;
      }
      if buf.len() < 32 {
        break;
      }
      let kind = buf[0] & 0x7f;
      let mut total = 32usize;
      if kind == 1 || kind == 35 {
        total += 4 * u32::from_le_bytes([buf[4], buf[5], buf[6], buf[7]]) as usize;
      }
      if buf.len() < total {
        break;
      }
      let msg = buf[..total].to_vec();
      off += total;
      match kind {
        0 => {
          if trace() {
            eprintln!("[luna-xwl] X error code={} seq={} bad={:#x} op={}", msg[1], u16::from_le_bytes([msg[2], msg[3]]), u32::from_le_bytes([msg[4], msg[5], msg[6], msg[7]]), msg[10]);
          }
        }
        1 => self.on_reply(&msg, out),
        35 => {}
        _ => {
          if trace() {
            eprintln!("[luna-xwl] raw event {}", kind);
          }
          self.on_event(kind, &msg, out)
        }
      }
    }
    self.inbuf.drain(..off);
  }

  fn parse_setup(&mut self, b: &[u8]) {
    let u32at = |o: usize| u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]]);
    let u16at = |o: usize| u16::from_le_bytes([b[o], b[o + 1]]);
    self.id_base = u32at(12);
    self.id_mask = u32at(16);
    let vendor = u16at(24) as usize;
    let nfmt = b[29] as usize;
    let screen = 40 + vendor + pad4(vendor) + nfmt * 8;
    self.root = u32at(screen);
    self.screen_w = u16at(screen + 20) as u32;
    self.screen_h = u16at(screen + 22) as u32;
    // Depths follow the fixed screen fields: find a 32-bit TrueColor visual.
    let ndepths = b[screen + 39] as usize;
    let mut o = screen + 40;
    for _ in 0..ndepths {
      if o + 8 > b.len() {
        break;
      }
      let depth = b[o];
      let nvis = u16at(o + 2) as usize;
      o += 8;
      for _ in 0..nvis {
        if o + 24 > b.len() {
          break;
        }
        if depth == 32 && b[o + 4] == 4 && self.argb_visual == 0 {
          self.argb_visual = u32at(o);
        }
        o += 24;
      }
    }
  }

  fn on_reply(&mut self, m: &[u8], out: &mut Vec<XEvent>) {
    let seq = u16::from_le_bytes([m[2], m[3]]);
    let Some(p) = self.pending.remove(&seq) else { return };
    match p {
      Pending::Composite => {
        if m[8] != 0 {
          let major = m[9];
          self.req(major, 0, &Self::u32s(&[0, 4])); // CompositeQueryVersion
          self.req(major, 2, &[&Self::u32s(&[self.root])[..], &[1, 0, 0, 0]].concat()); // RedirectSubwindows, Manual
        } else {
          eprintln!("[luna-compositor] xwm: Composite extension missing; X windows will not appear");
        }
      }
      Pending::Atom(idx) => {
        self.atoms[idx] = u32::from_le_bytes([m[8], m[9], m[10], m[11]]);
        self.atoms_left -= 1;
        if self.atoms_left == 0 {
          self.atoms_ready();
        }
      }
      Pending::Prop(win, prop) => {
        let format = m[1] as usize;
        let ty = u32::from_le_bytes([m[8], m[9], m[10], m[11]]);
        let n = u32::from_le_bytes([m[16], m[17], m[18], m[19]]) as usize;
        let data = &m[32..];
        let bytes = n * format / 8;
        if ty == 0 || data.len() < bytes {
          return;
        }
        let data = data[..bytes].to_vec();
        let Some(w) = self.windows.get_mut(&win) else { return };
        let u32_at = |i: usize| u32::from_le_bytes([data[i * 4], data[i * 4 + 1], data[i * 4 + 2], data[i * 4 + 3]]);
        if prop == self.atoms[At::NET_WM_NAME as usize] {
          w.title = String::from_utf8_lossy(&data).into_owned();
        } else if prop == A_WM_NAME {
          if w.title.is_empty() {
            w.title = data.iter().map(|&c| c as char).collect();
          }
        } else if prop == A_WM_CLASS {
          // "instance\0class\0": the class is the stable application id.
          let mut parts = data.split(|&c| c == 0);
          let inst = parts.next().unwrap_or(&[]);
          let class = parts.next().unwrap_or(inst);
          w.app_id = String::from_utf8_lossy(class).to_lowercase();
        } else if prop == A_WM_TRANSIENT_FOR {
          w.transient_for = if bytes >= 4 { u32_at(0) } else { 0 };
        } else if prop == self.atoms[At::NET_WM_WINDOW_TYPE as usize] {
          w.types = (0..bytes / 4).map(u32_at).collect();
        } else if prop == self.atoms[At::MOTIF_WM_HINTS as usize] {
          // flags(1<<1)=DECORATIONS, decorations=0: no WM frame wanted.
          w.csd = bytes >= 12 && u32_at(0) & 2 != 0 && u32_at(2) == 0;
        } else if prop == self.atoms[At::GTK_FRAME_EXTENTS as usize] {
          if bytes >= 16 {
            w.frame = [u32_at(0) as i32, u32_at(1) as i32, u32_at(2) as i32, u32_at(3) as i32];
            w.csd = true;
          }
        } else if prop == self.atoms[At::WM_PROTOCOLS as usize] {
          let del = self.atoms[At::WM_DELETE_WINDOW as usize];
          w.delete_ok = (0..bytes / 4).any(|i| u32_at(i) == del);
        }
        out.push(XEvent::Props { win });
      }
    }
  }

  fn on_event(&mut self, kind: u8, m: &[u8], out: &mut Vec<XEvent>) {
    let u32at = |o: usize| u32::from_le_bytes([m[o], m[o + 1], m[o + 2], m[o + 3]]);
    let i16at = |o: usize| i16::from_le_bytes([m[o], m[o + 1]]) as i32;
    let u16at = |o: usize| u16::from_le_bytes([m[o], m[o + 1]]) as u32;
    match kind {
      16 => {
        // CreateNotify (children of root only matter; others are our tray).
        let (parent, win) = (u32at(4), u32at(8));
        if parent != self.root || self.windows.contains_key(&win) {
          return;
        }
        self.windows.insert(
          win,
          XWin {
            x: i16at(12),
            y: i16at(14),
            w: u16at(16),
            h: u16at(18),
            override_redirect: m[22] != 0,
            ..Default::default()
          },
        );
        self.change_attributes(win, EM_PROPERTY_CHANGE);
        self.refresh_props(win);
        out.push(XEvent::Geometry { win });
      }
      17 => {
        let win = u32at(8);
        if let Some(c) = self.tray.remove(&win) {
          out.push(XEvent::TrayGone { icon: win, container: c });
        }
        if self.windows.remove(&win).is_some() {
          out.push(XEvent::Destroyed { win });
        }
      }
      18 => {
        let win = u32at(8);
        if let Some(w) = self.windows.get_mut(&win) {
          w.mapped = false;
        }
        out.push(XEvent::Unmapped { win });
      }
      19 => {
        let win = u32at(8);
        if let Some(w) = self.windows.get_mut(&win) {
          w.mapped = true;
        }
        out.push(XEvent::Mapped { win });
      }
      20 => {
        let win = u32at(8);
        self.windows.entry(win).or_default();
        out.push(XEvent::MapRequest { win });
      }
      22 => {
        // ConfigureNotify: only unmanaged windows move on their own.
        let win = u32at(8);
        if let Some(w) = self.windows.get_mut(&win) {
          if w.override_redirect || w.tray_container != 0 {
            w.x = i16at(16);
            w.y = i16at(18);
            w.w = u16at(20);
            w.h = u16at(22);
            out.push(XEvent::Geometry { win });
          }
        }
      }
      23 => {
        // ConfigureRequest from a managed window.
        let win = u32at(8);
        let mask = u16at(26);
        let w = (mask & 4 != 0).then(|| u16at(20));
        let h = (mask & 8 != 0).then(|| u16at(22));
        if self.tray.contains_key(&win) {
          self.configure(win, Some(0), Some(0), Some(24), Some(24));
          return;
        }
        // A window the compositor has not placed yet keeps its requested
        // position; afterwards the compositor owns it.
        let first = self.windows.get(&win).map(|x| x.surface == 0).unwrap_or(true);
        if first {
          let wn = self.windows.entry(win).or_default();
          if mask & 1 != 0 { wn.x = i16at(16); }
          if mask & 2 != 0 { wn.y = i16at(18); }
        }
        self.grant_configure(win, w, h);
        out.push(XEvent::Geometry { win });
      }
      28 => {
        let (win, atom) = (u32at(4), u32at(8));
        if self.windows.contains_key(&win) {
          self.get_property(win, atom, 0);
        }
      }
      33 => {
        let (win, ty) = (u32at(4), u32at(8));
        if trace() {
          eprintln!("[luna-xwl] clientmsg win={:#x} type={}", win, ty);
        }
        if ty == self.atom(At::WL_SURFACE_ID) {
          let surface = u32at(12);
          if let Some(w) = self.windows.get_mut(&win) {
            w.surface = surface;
          }
          out.push(XEvent::SurfaceId { win, surface });
        } else if ty == self.atom(At::NET_TRAY_OPCODE) && u32at(16) == 0 {
          let icon = u32at(20);
          let mut ev = Vec::new();
          self.dock(icon, &mut ev);
          out.extend(ev);
        }
      }
      _ => {}
    }
  }
}

impl Drop for Xwm {
  fn drop(&mut self) {
    unsafe { libc::close(self.fd) };
  }
}

impl Xwm {
  /// Classify a window from its `_NET_WM_WINDOW_TYPE` list.
  pub fn kind_of(&self, win: u32) -> Kind {
    let Some(w) = self.windows.get(&win) else { return Kind::Normal };
    let dialog = [At::TYPE_DIALOG, At::TYPE_UTILITY, At::TYPE_SPLASH].map(|a| self.atoms[a as usize]);
    let normal = self.atom(At::TYPE_NORMAL);
    if w.types.is_empty() || w.types.contains(&normal) {
      return if w.transient_for != 0 { Kind::Dialog } else { Kind::Normal };
    }
    if w.types.iter().any(|t| dialog.contains(t)) {
      return Kind::Dialog;
    }
    // Menus, tooltips, dnd, notifications, docks, …
    Kind::Popup
  }
}
