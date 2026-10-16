/*
 * Copyright © 2026 Yuichiro Nakada / Project Vespera
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

//! Rootless Xwayland: X11 clients appear as ordinary windows.
//!
//! Xwayland connects to us as a Wayland client and draws every X window into a
//! role-less `wl_surface`; `crate::xwm` is the X11 side that tells us which
//! window belongs to which surface.  Rather than a second window model, a
//! managed X window is given *synthetic* `xdg_surface` / `xdg_toplevel` objects
//! in Xwayland's object table, so stacking, placement, decorations, focus,
//! maximise and the taskbar all run through the existing xdg code.  Events for
//! those objects are never sent (see `Client::send`); configure and close are
//! translated to X11 requests instead.  Menus and tooltips (override-redirect)
//! become positioned popups.

use super::*;
use crate::xwm::{Kind, XEvent, Xwm};
use std::os::unix::process::CommandExt;

/// Object ids Xwayland never allocates (client range ends below this).
pub(super) const SYNTH_BASE: u32 = 0xfe00_0000;

pub(super) struct Xwl {
  child: std::process::Child,
  pub wm: Option<Xwm>,
  display_rd: RawFd,
  client_fd: RawFd,
  socket: String,
  /// (Xwayland client fd, wl_surface id) -> X window
  win_of: HashMap<(RawFd, u32), u32>,
  /// X window -> (fd, wl_surface id, unmanaged popup?)
  surf_of: HashMap<u32, (RawFd, u32, bool)>,
  next_synth: u32,
  /// WL_SURFACE_ID can overtake the wl_compositor.create_surface it refers to
  /// (two sockets); (window, surface, attempts) waiting for the surface.
  pending: Vec<(u32, u32, u8)>,
}

fn clear_cloexec(fd: RawFd) {
  unsafe {
    let fl = libc::fcntl(fd, libc::F_GETFD);
    libc::fcntl(fd, libc::F_SETFD, fl & !libc::FD_CLOEXEC);
  }
}

impl Xwl {
  /// Start Xwayland, or None when it is disabled or not installed.
  pub fn spawn(socket: &str) -> Option<Xwl> {
    if cfg!(test) || std::env::var_os("LUNA_NO_XWAYLAND").is_some() {
      return None;
    }
    let mut sv = [0i32; 2];
    let mut pp = [0i32; 2];
    unsafe {
      if libc::socketpair(libc::AF_UNIX, libc::SOCK_STREAM | libc::SOCK_CLOEXEC, 0, sv.as_mut_ptr()) != 0
        || libc::pipe2(pp.as_mut_ptr(), libc::O_CLOEXEC) != 0
      {
        return None;
      }
    }
    let (theirs, display_wr) = (sv[1], pp[1]);
    let mut cmd = std::process::Command::new("Xwayland");
    cmd.args(["-rootless", "-noreset", "-nolisten", "tcp", "-displayfd"])
      .arg(display_wr.to_string())
      .arg("-wm")
      .arg(theirs.to_string());
    // Shared-memory buffers: the lightest path; GL clients fall back to llvmpipe.
    if std::env::var_os("LUNA_XWAYLAND_GLAMOR").is_none() {
      cmd.arg("-shm");
    }
    cmd.env("WAYLAND_DISPLAY", socket)
      .env_remove("DISPLAY")
      .stdin(std::process::Stdio::null());
    unsafe {
      cmd.pre_exec(move || {
        clear_cloexec(theirs);
        clear_cloexec(display_wr);
        Ok(())
      });
    }
    let child = match cmd.spawn() {
      Ok(c) => c,
      Err(e) => {
        eprintln!("[luna-compositor] xwayland: {} (X11 apps disabled)", e);
        unsafe {
          for fd in [sv[0], sv[1], pp[0], pp[1]] {
            libc::close(fd);
          }
        }
        return None;
      }
    };
    unsafe {
      libc::close(theirs);
      libc::close(display_wr);
    }
    eprintln!("[luna-compositor] xwayland: started (pid {})", child.id());
    Some(Xwl {
      child,
      wm: Some(Xwm::new(sv[0])),
      display_rd: pp[0],
      client_fd: -1,
      socket: socket.to_string(),
      win_of: HashMap::new(),
      surf_of: HashMap::new(),
      next_synth: SYNTH_BASE,
      pending: Vec::new(),
    })
  }

  pub fn wm_fd(&self) -> Option<RawFd> { self.wm.as_ref().map(|w| w.fd()) }
  pub fn display_fd(&self) -> RawFd { self.display_rd }
}

impl Drop for Xwl {
  fn drop(&mut self) {
    if self.display_rd >= 0 {
      unsafe { libc::close(self.display_rd) };
    }
    self.wm = None;
    let _ = self.child.kill();
    let _ = self.child.wait();
    // Remove the advertised display so a stale value is never exported.
    if let Ok(rt) = std::env::var("XDG_RUNTIME_DIR") {
      let _ = std::fs::remove_file(format!("{}/luna-xdisplay-{}", rt, self.socket));
    }
  }
}

impl Server {
  pub(super) fn xwl_start(&mut self, socket: &str) {
    if let Some(x) = Xwl::spawn(socket) {
      self.epoll_add(x.display_fd());
      if let Some(fd) = x.wm_fd() {
        self.epoll_add(fd);
      }
      self.xwl = Some(x);
    }
  }

  pub(super) fn xwl_owns_fd(&self, fd: RawFd) -> bool {
    self.xwl.as_ref().map(|x| x.wm_fd() == Some(fd) || x.display_rd == fd).unwrap_or(false)
  }

  /// Dispatch an epoll wake-up for the X display pipe or the WM connection.
  pub(super) fn xwl_ready(&mut self, fd: RawFd) {
    let Some(x) = self.xwl.as_mut() else { return };
    if fd == x.display_rd {
      let mut buf = [0u8; 32];
      let n = unsafe { libc::read(fd, buf.as_mut_ptr() as *mut libc::c_void, buf.len()) };
      if n > 0 {
        let num = String::from_utf8_lossy(&buf[..n as usize]).trim().to_string();
        let display = format!(":{}", num);
        if let Ok(rt) = std::env::var("XDG_RUNTIME_DIR") {
          let path = format!("{}/luna-xdisplay-{}", rt, x.socket);
          let tmp = format!("{}.tmp", path);
          if std::fs::write(&tmp, &display).and_then(|_| std::fs::rename(&tmp, &path)).is_ok() {
            eprintln!("[luna-compositor] xwayland: display {} ({})", display, path);
          }
        }
      }
      unsafe {
        libc::epoll_ctl(self.epoll_fd, libc::EPOLL_CTL_DEL, fd, std::ptr::null_mut());
        libc::close(fd);
      }
      x.display_rd = -1;
      return;
    }
    let mut events = Vec::new();
    let alive = match x.wm.as_mut() {
      Some(wm) => wm.on_readable(&mut events),
      None => return,
    };
    for ev in events {
      self.xwl_event(ev);
    }
    // Pull in the Wayland requests that belong to the events just handled.
    if self.xwl_has_pending() {
      if let Some(cfd) = self.xwl_client() {
        self.recv_client(cfd);
      }
    }
    if !alive {
      eprintln!("[luna-compositor] xwayland: connection lost");
      if let Some(x) = self.xwl.as_ref() {
        if let Some(fd) = x.wm_fd() {
          unsafe { libc::epoll_ctl(self.epoll_fd, libc::EPOLL_CTL_DEL, fd, std::ptr::null_mut()) };
        }
      }
      self.xwl = None;
      self.dirty = true;
    }
  }

  /// The compositor-side client that Xwayland connected as.
  fn xwl_client(&mut self) -> Option<RawFd> {
    let x = self.xwl.as_mut()?;
    if x.client_fd >= 0 && self.clients.contains_key(&x.client_fd) {
      return Some(x.client_fd);
    }
    let pid = x.child.id() as i32;
    x.client_fd = self.clients.iter().find(|(_, c)| c.pid == pid).map(|(&fd, _)| fd).unwrap_or(-1);
    (x.client_fd >= 0).then_some(x.client_fd)
  }

  /// X window behind a managed (xdg-role) surface, if it is one.
  pub(super) fn xwl_window_of(&self, fd: RawFd, surface_id: u32) -> Option<u32> {
    let x = self.xwl.as_ref()?;
    let win = *x.win_of.get(&(fd, surface_id))?;
    (!x.surf_of.get(&win)?.2).then_some(win)
  }

  fn xwl_event(&mut self, ev: XEvent) {
    if self.xwl_trace {
      eprintln!("[luna-xwl] {:?}", ev);
    }
    match ev {
      XEvent::MapRequest { win } => {
        if let Some(wm) = self.xwl.as_mut().and_then(|x| x.wm.as_mut()) {
          wm.grant_map(win);
          wm.flush();
        }
      }
      XEvent::SurfaceId { win, surface } => self.xwl_associate(win, surface),
      XEvent::Props { win } => self.xwl_update_props(win),
      XEvent::Geometry { win } => self.xwl_popup_geometry(win),
      XEvent::Unmapped { win } | XEvent::Destroyed { win } => self.xwl_dissociate(win),
      XEvent::TrayDocked { icon, .. } => {
        // The shell reserves a slot in the menubar and reports its rectangle
        // back (`x11_tray_pos`); until then the icon stays off-screen.
        let name = self.xwl.as_ref().and_then(|x| x.wm.as_ref()).and_then(|m| m.windows.get(&icon))
          .map(|w| if w.app_id.is_empty() { "X11".to_string() } else { w.app_id.replace(' ', "_") })
          .unwrap_or_else(|| "X11".to_string());
        if let Some(ipc) = self.shell_ipc.as_mut() {
          ipc.handle_tray_command(&format!("tray_add x11:{} {} x11", icon, name));
        }
        self.shell_state_dirty = true;
      }
      XEvent::TrayGone { icon, .. } => {
        if let Some(ipc) = self.shell_ipc.as_mut() {
          ipc.handle_tray_command(&format!("tray_remove x11:{}", icon));
        }
        self.shell_state_dirty = true;
      }
      XEvent::Mapped { .. } => {}
    }
  }

  pub(super) fn xwl_has_pending(&self) -> bool {
    self.xwl.as_ref().map(|x| !x.pending.is_empty()).unwrap_or(false)
  }

  pub(super) fn xwl_retry_pending(&mut self) {
    let Some(x) = self.xwl.as_mut() else { return };
    for (win, surface, tries) in std::mem::take(&mut x.pending) {
      if tries < 100 {
        self.xwl_associate_try(win, surface, tries + 1);
      }
    }
  }

  fn xwl_associate(&mut self, win: u32, surface: u32) {
    self.xwl_associate_try(win, surface, 0);
  }

  fn xwl_associate_try(&mut self, win: u32, surface: u32, tries: u8) {
    let Some(fd) = self.xwl_client() else { return };
    let (kind, info) = {
      let Some(wm) = self.xwl.as_ref().and_then(|x| x.wm.as_ref()) else { return };
      let Some(w) = wm.windows.get(&win) else { return };
      (if w.override_redirect { Kind::Popup } else { wm.kind_of(win) }, w.clone())
    };
    let parent = if info.transient_for != 0 {
      self.xwl.as_ref().and_then(|x| x.surf_of.get(&info.transient_for)).map(|p| p.1)
    } else {
      None
    };
    let Some(mut client) = self.clients.remove(&fd) else { return };
    let ok = matches!(client.objects.get(&surface), Some(Object { role: Role::Surface(_), .. }));
    if !ok {
      self.clients.insert(fd, client);
      if let Some(x) = self.xwl.as_mut() {
        x.pending.push((win, surface, tries));
      }
      return;
    }
    let popup = kind == Kind::Popup;
    if self.xwl_trace {
      eprintln!("[luna-xwl] associate win={:#x} surface={} kind={:?} or={} at {},{} {}x{} title={:?}", win, surface, kind,
        info.override_redirect, info.x, info.y, info.w, info.h, info.title);
    }
    let mut mapped_now = false;
    if popup {
      if let Some(Object { role: Role::Surface(s), .. }) = client.objects.get_mut(&surface) {
        s.popup = true;
        s.x = info.x;
        s.y = info.y;
      }
      self.popup_stack.retain(|&(f, sid)| !(f == fd && sid == surface));
      self.popup_stack.push((fd, surface));
    } else {
      let (xdg, tl) = {
        let x = self.xwl.as_mut().unwrap();
        let a = x.next_synth;
        x.next_synth += 2;
        (a, a + 1)
      };
      client.objects.insert(xdg, Object::new(&protocol::XDG_SURFACE, 1, Role::XdgSurface { surface_id: surface, configured: true }));
      client.objects.insert(
        tl,
        Object::new(
          &protocol::XDG_TOPLEVEL,
          1,
          Role::XdgToplevel {
            xdg_surface_id: xdg,
            title: info.title.clone(),
            app_id: info.app_id.clone(),
            minimized: false,
            maximized: false,
            fullscreen: false,
            parent_surface_id: parent,
            saved_geom: None,
            tiled: 0,
            decoration_mode: if info.csd { 1 } else { 2 },
            min_w: 0,
            min_h: 0,
            max_w: 0,
            max_h: 0,
          },
        ),
      );
      if let Some(Object { role: Role::Surface(s), .. }) = client.objects.get_mut(&surface) {
        s.xdg_surface_id = Some(xdg);
        Self::xwl_apply_frame(s, &info);
        // Xwayland may have committed a buffer before we knew the role.  Run
        // the first-map path now, exactly as the xdg commit would have.
        mapped_now = s.mapped && s.current_buffer.is_some();
        if mapped_now {
          s.mapped = false;
        }
      }
      if mapped_now {
        if let Some(Object { role: Role::Surface(s), .. }) = client.objects.get_mut(&surface) {
          s.mapped = true;
        }
        self.track_mapped_toplevel(fd, surface, true);
        self.place_first_map(&mut client, fd, surface, false);
      }
    }
    self.clients.insert(fd, client);
    if let Some(x) = self.xwl.as_mut() {
      x.win_of.insert((fd, surface), win);
      x.surf_of.insert(win, (fd, surface, popup));
    }
    self.shell_state_dirty = true;
    self.dirty = true;
  }

  /// GTK draws a shadow around its content; only the content box is the window.
  fn xwl_apply_frame(s: &mut crate::object::Surface, w: &crate::xwm::XWin) {
    let [l, r, t, b] = w.frame;
    s.window_geom = (w.frame != [0; 4]).then(|| (l, t, (w.w as i32 - l - r).max(1), (w.h as i32 - t - b).max(1)));
  }

  fn xwl_dissociate(&mut self, win: u32) {
    let Some(x) = self.xwl.as_mut() else { return };
    if let Some((fd, sid, popup)) = x.surf_of.remove(&win) {
      x.win_of.remove(&(fd, sid));
      if popup {
        self.popup_stack.retain(|&(f, s)| !(f == fd && s == sid));
      } else if let Some(client) = self.clients.get_mut(&fd) {
        // Nobody will ever destroy the synthetic xdg objects: drop them, and
        // the surface's xdg role, so a re-map starts clean.
        let xdg: Vec<u32> = client.objects.iter().filter_map(|(&id, o)| match o.role {
          Role::XdgSurface { surface_id, .. } if surface_id == sid && id >= SYNTH_BASE => Some(id),
          _ => None,
        }).collect();
        client.objects.retain(|id, o| !(xdg.contains(id)
          || matches!(o.role, Role::XdgToplevel { xdg_surface_id, .. } if xdg.contains(&xdg_surface_id))));
        if let Some(Object { role: Role::Surface(s), .. }) = client.objects.get_mut(&sid) {
          s.xdg_surface_id = None;
        }
      }
      self.dirty = true;
      self.shell_state_dirty = true;
    }
  }

  fn xwl_update_props(&mut self, win: u32) {
    let Some(x) = self.xwl.as_ref() else { return };
    let Some(&(fd, sid, popup)) = x.surf_of.get(&win) else { return };
    if popup {
      return;
    }
    let Some(w) = x.wm.as_ref().and_then(|m| m.windows.get(&win)) else { return };
    let (title, app_id) = (w.title.clone(), w.app_id.clone());
    let info = w.clone();
    let Some(client) = self.clients.get_mut(&fd) else { return };
    if let Some(Object { role: Role::Surface(s), .. }) = client.objects.get_mut(&sid) {
      Self::xwl_apply_frame(s, &info);
    }
    let xdg = match client.objects.get(&sid) {
      Some(Object { role: Role::Surface(s), .. }) => s.xdg_surface_id,
      _ => None,
    };
    for obj in client.objects.values_mut() {
      if let Role::XdgToplevel { xdg_surface_id, title: t, app_id: a, .. } = &mut obj.role {
        if Some(*xdg_surface_id) == xdg {
          *t = title.clone();
          *a = app_id.clone();
        }
      }
    }
    self.shell_state_dirty = true;
    self.dirty = true;
  }

  /// An override-redirect window moved or resized itself.
  fn xwl_popup_geometry(&mut self, win: u32) {
    let Some(x) = self.xwl.as_ref() else { return };
    let Some(&(fd, sid, popup)) = x.surf_of.get(&win) else { return };
    if !popup {
      return;
    }
    let Some(w) = x.wm.as_ref().and_then(|m| m.windows.get(&win)) else { return };
    let (nx, ny) = (w.x, w.y);
    if let Some(Object { role: Role::Surface(s), .. }) = self.clients.get_mut(&fd).and_then(|c| c.objects.get_mut(&sid)) {
      if s.x != nx || s.y != ny {
        s.x = nx;
        s.y = ny;
        self.dirty = true;
      }
    }
  }

  /// Before each composite: tell X where the compositor put its windows, so
  /// popups the clients position relative to them land correctly.
  pub(super) fn xwl_sync(&mut self) {
    let Some(x) = self.xwl.as_mut() else { return };
    let Some(wm) = x.wm.as_mut() else { return };
    if !wm.ready {
      return;
    }
    for (&win, &(fd, sid, popup)) in &x.surf_of {
      if popup {
        continue;
      }
      if let Some(Object { role: Role::Surface(s), .. }) = self.clients.get(&fd).and_then(|c| c.objects.get(&sid)) {
        wm.place(win, s.x, s.y);
      }
    }
    wm.flush();
  }

  /// `send_toplevel_configure_for` for an X window: resize instead of xdg configure.
  pub(super) fn xwl_resize(&mut self, win: u32, w: i32, h: i32) {
    if let Some(wm) = self.xwl.as_mut().and_then(|x| x.wm.as_mut()) {
      if w > 0 && h > 0 {
        wm.resize(win, w as u32, h as u32);
        wm.flush();
      }
    }
  }

  pub(super) fn xwl_close(&mut self, win: u32) {
    if let Some(wm) = self.xwl.as_mut().and_then(|x| x.wm.as_mut()) {
      wm.close(win);
      wm.flush();
    }
  }
}

impl Server {
  pub(super) fn xwl_tray_bg(&mut self, rgb: u32) {
    if let Some(wm) = self.xwl.as_mut().and_then(|x| x.wm.as_mut()) {
      wm.set_tray_bg(rgb & 0xff_ffff);
      wm.flush();
    }
  }

  /// `x11_tray_pos ICON X Y W H` from the shell.
  pub(super) fn xwl_tray_pos(&mut self, icon: u32, x: i32, y: i32, w: u32, h: u32) {
    if let Some(wm) = self.xwl.as_mut().and_then(|x| x.wm.as_mut()) {
      wm.place_tray(icon, x, y, w.clamp(8, 64), h.clamp(8, 64));
      wm.flush();
    }
  }
}
