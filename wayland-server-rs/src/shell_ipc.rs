/*
 * Copyright © 2026 Yuichiro Nakada / Project Vespera
 *
 * Luna shell IPC — exposes running windows + tray icons to luna-shell via
 * a JSON state file and a Unix domain command socket.
 */

use crate::object::{Object, Role};
use crate::server::Client;
use std::collections::{HashMap, HashSet};
use std::ffi::CString;
use std::fmt::Write as _;
use std::io::Write;
use std::os::unix::io::RawFd;
use std::path::PathBuf;

pub const STATE_FILE: &str = "luna-shell/state.json";
pub const CMD_SOCKET: &str = "luna-shell.sock";
pub const ACTION_SOCKET: &str = "luna-shell/action.sock";

/// Connection-scoped shell handle. Wayland object ids are scoped to one
/// client, so exporting a bare wl_surface id lets two applications alias each
/// other. Packing the connection fd keeps the IPC representation allocation
/// free and makes every shell action unambiguous.
#[inline]
pub fn window_id(client_fd: RawFd, surface_id: u32) -> u64 {
  (((client_fd as u32 as u64).wrapping_add(1)) << 32) | surface_id as u64
}

#[inline]
pub fn split_window_id(id: u64) -> (RawFd, u32) {
  (((id >> 32) as u32).wrapping_sub(1) as RawFd, id as u32)
}

#[derive(Clone, Debug)]
pub struct TrayItem {
  pub id: String,
  pub label: String,
  pub icon: String,
  pub tooltip: String,
}

#[derive(Clone, Debug)]
pub struct WindowInfo {
  pub id: u64,
  pub title: String,
  pub app_id: String,
  pub x: i32,
  pub y: i32,
  pub focused: bool,
  pub minimized: bool,
  pub maximized: bool,
  pub fullscreen: bool,
}

pub struct ShellIpc {
  cmd_fd: RawFd,
  runtime_dir: PathBuf,
  extra_tray: Vec<TrayItem>,
  last_export: u64,
  /// Reused across exports.  The snapshot is rebuilt on every window title,
  /// focus or map change; churning a fresh String for each of the dozens of
  /// `format!`s it used to run made those moments a burst of allocator work on
  /// the compositor's own thread.
  out: String,
  key_scratch: String,
  /// Reused by collect_windows / collect_tray so a focus change does not
  /// allocate a fresh Vec of WindowInfo/TrayItem (and drop the previous one)
  /// on the compositor thread.
  windows_scratch: Vec<WindowInfo>,
  tray_scratch: Vec<TrayItem>,
}

impl ShellIpc {
  pub fn open() -> Option<Self> {
    let runtime = std::env::var("XDG_RUNTIME_DIR").ok()?;
    let dir = PathBuf::from(&runtime).join("luna-shell");
    std::fs::create_dir_all(&dir).ok()?;

    let sock_path = dir.join("luna-shell.sock");
    let _ = std::fs::remove_file(&sock_path);
    let cpath = CString::new(sock_path.to_string_lossy().as_bytes()).ok()?;

    let fd = unsafe { libc::socket(libc::AF_UNIX, libc::SOCK_STREAM | libc::SOCK_NONBLOCK | libc::SOCK_CLOEXEC, 0) };
    if fd < 0 {
      return None;
    }

    let mut addr: libc::sockaddr_un = unsafe { std::mem::zeroed() };
    addr.sun_family = libc::AF_UNIX as u16;
    let path = cpath.as_bytes_with_nul();
    if path.len() > addr.sun_path.len() {
      unsafe { libc::close(fd) };
      return None;
    }
    unsafe {
      std::ptr::copy_nonoverlapping(path.as_ptr() as *const i8, addr.sun_path.as_mut_ptr(), path.len());
    }

    let r = unsafe { libc::bind(fd, &addr as *const libc::sockaddr_un as *const libc::sockaddr, std::mem::size_of::<libc::sockaddr_un>() as u32) };
    if r != 0 {
      unsafe { libc::close(fd) };
      return None;
    }
    if unsafe { libc::listen(fd, 8) } != 0 {
      unsafe { libc::close(fd) };
      return None;
    }

    eprintln!("[luna-compositor] luna-shell IPC: {}/{}", runtime, CMD_SOCKET);

    Some(ShellIpc {
      cmd_fd: fd,
      runtime_dir: PathBuf::from(runtime),
      extra_tray: Vec::new(),
      last_export: 0,
      out: String::new(),
      key_scratch: String::new(),
      windows_scratch: Vec::new(),
      tray_scratch: Vec::new(),
    })
  }

  pub fn cmd_fd(&self) -> RawFd { self.cmd_fd }

  /// Push a one-shot UI action to luna-shell without rewriting state.json.
  /// The shell binds a datagram socket and executes these immediately, so
  /// global shortcuts stay responsive even while a GTK client owns keyboard
  /// focus and the state poller is debouncing geometry writes.
  pub fn send_shell_action(&self, action: &str) {
    let action = action.trim();
    if action.is_empty() || action.len() >= 48 {
      return;
    }
    let path = self.runtime_dir.join(ACTION_SOCKET);
    let cpath = match CString::new(path.to_string_lossy().as_bytes()) {
      Ok(p) => p,
      Err(_) => return,
    };
    let fd = unsafe { libc::socket(libc::AF_UNIX, libc::SOCK_DGRAM | libc::SOCK_CLOEXEC, 0) };
    if fd < 0 {
      return;
    }
    let mut addr: libc::sockaddr_un = unsafe { std::mem::zeroed() };
    addr.sun_family = libc::AF_UNIX as u16;
    let path = cpath.as_bytes_with_nul();
    if path.len() > addr.sun_path.len() {
      unsafe { libc::close(fd) };
      return;
    }
    unsafe {
      std::ptr::copy_nonoverlapping(path.as_ptr() as *const i8, addr.sun_path.as_mut_ptr(), path.len());
    }
    let msg = format!("{action}\n");
    unsafe {
      libc::sendto(
        fd,
        msg.as_ptr() as *const libc::c_void,
        msg.len(),
        libc::MSG_NOSIGNAL,
        &addr as *const libc::sockaddr_un as *const libc::sockaddr,
        std::mem::size_of::<libc::sockaddr_un>() as u32,
      );
      libc::close(fd);
    }
  }

  pub fn accept_commands(&mut self) -> Vec<String> {
    let mut out = Vec::new();
    loop {
      let client = unsafe { libc::accept4(self.cmd_fd, std::ptr::null_mut(), std::ptr::null_mut(), libc::SOCK_CLOEXEC) };
      if client < 0 {
        break;
      }
      let mut buf = [0u8; 512];
      let n = unsafe { libc::read(client, buf.as_mut_ptr() as *mut libc::c_void, buf.len()) };
      unsafe { libc::close(client) };
      if n <= 0 {
        continue;
      }
      if let Ok(s) = std::str::from_utf8(&buf[..n as usize]) {
        for line in s.lines() {
          let line = line.trim();
          if !line.is_empty() {
            out.push(line.to_string());
          }
        }
      }
    }
    out
  }

  pub fn handle_tray_command(&mut self, cmd: &str) {
    let mut parts = cmd.split_whitespace();
    match parts.next() {
      Some("tray_add") => {
        let id = parts.next().unwrap_or("").to_string();
        let label = parts.next().unwrap_or("").to_string();
        let icon = parts.next().unwrap_or("dot").to_string();
        if id.is_empty() {
          return;
        }
        self.extra_tray.retain(|t| t.id != id);
        self.extra_tray.push(TrayItem {
          id,
          label: label.clone(),
          icon,
          tooltip: label,
        });
      }
      Some("tray_remove") => {
        if let Some(id) = parts.next() {
          self.extra_tray.retain(|t| t.id != id);
        }
      }
      _ => {}
    }
  }

  pub   fn collect_windows_into(
    clients: &HashMap<RawFd, Client>,
    focused_fd: RawFd,
    focused_surface: u32,
    windows: &mut Vec<WindowInfo>,
  ) {
    windows.clear();
    for (&fd, client) in clients {
      for (&surface_id, obj) in &client.objects {
        let Role::Surface(s) = &obj.role else { continue };
        if s.popup || s.subsurface_parent.is_some() {
          continue;
        }

        // Modeless shell dialogs (settings/about/…) are layer-shell surfaces
        // with a `luna.dialog.*` namespace.  List them next to xdg_toplevels so
        // Alt+Tab / the menubar window chips can reach them.
        if let Some(lsid) = s.layer_surface_id {
          let Some(Object {
            role: Role::LayerSurface { namespace, .. },
            ..
          }) = client.objects.get(&lsid)
          else {
            continue;
          };
          if !is_windowed_layer_namespace(namespace) {
            continue;
          }
          if !s.mapped && !surface_has_subsurface_content(client, surface_id) {
            continue;
          }
          let (title, app_id) = windowed_layer_meta(namespace);
          windows.push(WindowInfo {
            id: window_id(fd, surface_id),
            title,
            app_id,
            x: s.x,
            y: s.y,
            focused: fd == focused_fd && surface_id == focused_surface,
            minimized: false,
            maximized: false,
            fullscreen: false,
          });
          continue;
        }

        if s.xdg_surface_id.is_none() {
          continue;
        }
        // Firefox may keep the xdg_toplevel parent buffer-less while content
        // lives on subsurfaces — still list it in the shell taskbar.
        if !s.mapped && !surface_has_subsurface_content(client, surface_id) {
          continue;
        }
        let xdg_id = s.xdg_surface_id.unwrap();
        let (title, app_id, minimized, maximized, fullscreen) = toplevel_meta(client, xdg_id);
        if is_shell_surface(&title, &app_id) {
          continue;
        }
        if title.is_empty() && app_id.is_empty() {
          continue;
        }
        windows.push(WindowInfo {
          id: window_id(fd, surface_id),
          title: if title.is_empty() { app_id.clone() } else { title },
          app_id,
          x: s.x,
          y: s.y,
          focused: fd == focused_fd && surface_id == focused_surface,
          minimized,
          maximized,
          fullscreen,
        });
      }
    }
    windows.sort_by(|a, b| a.title.cmp(&b.title));
  }

  pub fn collect_windows(
    clients: &HashMap<RawFd, Client>,
    focused_fd: RawFd,
    focused_surface: u32,
  ) -> Vec<WindowInfo> {
    let mut windows = Vec::new();
    Self::collect_windows_into(clients, focused_fd, focused_surface, &mut windows);
    windows
  }

  fn collect_tray_into(
    &mut self,
    clients: &HashMap<RawFd, Client>,
    focused_fd: RawFd,
    focused_surface: u32,
    tray: &mut Vec<TrayItem>,
  ) {
    tray.clear();
    tray.extend(self.extra_tray.iter().cloned());
    let mut seen = HashSet::new();
    for t in tray.iter() {
      seen.insert(t.id.clone());
    }

    for (&fd, client) in clients {
      for (&surface_id, obj) in &client.objects {
        let Role::Surface(s) = &obj.role else { continue };
        if s.popup || s.xdg_surface_id.is_none() || s.subsurface_parent.is_some() {
          continue;
        }
        if !s.mapped && !surface_has_subsurface_content(client, surface_id) {
          continue;
        }
        let xdg_id = s.xdg_surface_id.unwrap();
        let (title, app_id, _, _, _) = toplevel_meta(client, xdg_id);
        if is_shell_surface(&title, &app_id) {
          continue;
        }
        // Built in a scratch buffer; only the keys that survive the dedup are
        // promoted to owned strings.
        self.key_scratch.clear();
        if app_id.is_empty() {
          let _ = write!(self.key_scratch, "win:{}", window_id(fd, surface_id));
        } else {
          let _ = write!(self.key_scratch, "app:{}", app_id);
        }
        if seen.contains(self.key_scratch.as_str()) {
          continue;
        }
        let key = self.key_scratch.clone();
        seen.insert(key.clone());
        let label = if !app_id.is_empty() {
          app_id.clone()
        } else if !title.is_empty() {
          title.clone()
        } else {
          "App".to_string()
        };
        tray.push(TrayItem {
          id: key,
          label: label.clone(),
          icon: tray_icon_for(&app_id),
          tooltip: if !title.is_empty() { title } else { label },
        });
      }
    }

    if focused_surface != 0 && focused_fd >= 0 {
      self.key_scratch.clear();
      let _ = write!(self.key_scratch, "win:{}", window_id(focused_fd, focused_surface));
      for t in tray.iter_mut() {
        if t.id == self.key_scratch {
          t.icon.push_str("_active");
        }
      }
    }
  }

  pub fn export_state(
    &mut self,
    clients: &HashMap<RawFd, Client>,
    focused_fd: RawFd,
    focused_surface: u32,
    force: bool,
    pending_menu: &mut Option<(RawFd, u32, i32, i32)>,
    switcher: &Option<(usize, Vec<(RawFd, u32)>)>,
  ) {
    let mut windows = std::mem::take(&mut self.windows_scratch);
    let mut tray = std::mem::take(&mut self.tray_scratch);
    Self::collect_windows_into(clients, focused_fd, focused_surface, &mut windows);
    self.collect_tray_into(clients, focused_fd, focused_surface, &mut tray);

    let hash = simple_hash(&windows, &tray)
      ^ pending_menu
        .map(|(fd, sid, x, y)| window_id(fd, sid) ^ (x as u64).rotate_left(17) ^ (y as u64).rotate_left(41))
        .unwrap_or(0)
      ^ switcher
        .as_ref()
        .map(|(i, ids)| {
          ids.iter().fold(*i as u64 ^ ids.len() as u64, |h, &(fd, sid)| {
            h.rotate_left(7) ^ window_id(fd, sid)
          })
        })
        .unwrap_or(0);
    if !force && hash == self.last_export {
      self.windows_scratch = windows;
      self.tray_scratch = tray;
      return;
    }
    self.last_export = hash;

    let path = self.runtime_dir.join(STATE_FILE);
    if let Some(parent) = path.parent() {
      let _ = std::fs::create_dir_all(parent);
    }

    // The snapshot is line-oriented and tab-separated, so tabs and newlines in
    // client-supplied titles have to go; `push_sanitized` does that in place
    // rather than allocating a replaced copy per field.
    fn push_sanitized(out: &mut String, s: &str) {
      for c in s.chars() {
        out.push(if c == '\t' || c == '\n' || c == '\r' { ' ' } else { c });
      }
    }

    let mut out = std::mem::take(&mut self.out);
    out.clear();
    for w in &windows {
      let _ = write!(out, "W\t{}\t", w.id);
      push_sanitized(&mut out, &w.title);
      out.push('\t');
      push_sanitized(&mut out, &w.app_id);
      let _ = write!(
        out,
        "\t{}\t{}\t{}\t{}\t{}\t{}\n",
        w.focused as u8,
        w.minimized as u8,
        w.maximized as u8,
        w.fullscreen as u8,
        w.x,
        w.y,
      );
    }
    for t in &tray {
      out.push_str("T\t");
      push_sanitized(&mut out, &t.id);
      out.push('\t');
      push_sanitized(&mut out, &t.label);
      out.push('\t');
      push_sanitized(&mut out, &t.icon);
      out.push('\t');
      push_sanitized(&mut out, &t.tooltip);
      out.push('\n');
    }
    if let Some((fd, sid, x, y)) = pending_menu.take() {
      let _ = write!(out, "M\t{}\t{}\t{}\n", window_id(fd, sid), x, y);
    }
    if let Some((idx, ids)) = switcher {
      let _ = write!(out, "S\t{}\t", idx);
      for (i, &(fd, sid)) in ids.iter().enumerate() {
        if i > 0 {
          out.push(',');
        }
        let _ = write!(out, "{}", window_id(fd, sid));
      }
      out.push('\n');
    }

    if let Ok(mut f) = std::fs::File::create(&path) {
      let _ = f.write_all(out.as_bytes());
    }
    self.out = out;
    self.windows_scratch = windows;
    self.tray_scratch = tray;
  }
}

impl Drop for ShellIpc {
  fn drop(&mut self) {
    if self.cmd_fd >= 0 {
      unsafe { libc::close(self.cmd_fd) };
    }
  }
}

pub fn toplevel_meta(client: &Client, xdg_surface_id: u32) -> (String, String, bool, bool, bool) {
  for obj in client.objects.values() {
    if let Role::XdgToplevel {
      xdg_surface_id: xs,
      title,
      app_id,
      minimized,
      maximized,
      fullscreen,
      ..
    } = &obj.role
    {
      if *xs == xdg_surface_id {
        return (
          title.clone(),
          app_id.clone(),
          *minimized,
          *maximized,
          *fullscreen,
        );
      }
    }
  }
  (String::new(), String::new(), false, false, false)
}

pub fn is_shell_surface(title: &str, app_id: &str) -> bool {
  let t = title.to_ascii_lowercase();
  let a = app_id.to_ascii_lowercase();
  t.contains("luna desktop") || t.contains("luna-shell") || a.contains("luna-shell") || a.contains("glfw")
}

/// Layer-shell namespace used by luna-shell for modeless dialogs that should
/// behave like ordinary windows (stacking + Alt+Tab).
pub fn is_windowed_layer_namespace(namespace: &str) -> bool {
  namespace.starts_with("luna.dialog.")
}

pub fn windowed_layer_meta(namespace: &str) -> (String, String) {
  let key = namespace.strip_prefix("luna.dialog.").unwrap_or(namespace);
  let title = match key {
    "settings" => "Settings",
    "about" => "About Luna",
    "net_detail" => "Network",
    "confirm" => "Confirm",
    other => other,
  };
  (title.to_string(), format!("luna.{}", key))
}

fn surface_has_subsurface_content(client: &Client, surface_id: u32) -> bool {
  for obj in client.objects.values() {
    if let Role::Subsurface {
      surface_id: child,
      parent_id,
      ..
    } = &obj.role
    {
      if *parent_id != surface_id {
        continue;
      }
      if let Some(Object {
        role: Role::Surface(s),
        ..
      }) = client.objects.get(child)
      {
        if s.mapped && s.current_buffer.is_some() {
          return true;
        }
      }
      if surface_has_subsurface_content(client, *child) {
        return true;
      }
    }
  }
  false
}

fn tray_icon_for(app_id: &str) -> String {
  let a = app_id.to_ascii_lowercase();
  if a.contains("terminal") || a.contains("foot") || a.contains("sakura")
    || a.contains("kitty") || a.contains("alacritty")
  {
    "terminal".into()
  } else if a.contains("firefox") || a.contains("browser") || a.contains("chrome")
    || a.contains("chromium") || a.contains("epiphany")
  {
    "browser".into()
  } else if a.contains("nautilus") || a.contains("files") || a.contains("thunar")
    || a.contains("pcmanfm") || a.contains("luna-fm")
  {
    "files".into()
  } else if a.contains("gedit") || a.contains("editor") || a.contains("code")
    || a.contains("luna-editor")
  {
    "editor".into()
  } else if a.contains("music") || a.contains("rhythmbox") || a.contains("vlc") {
    "music".into()
  } else if a.contains("settings") || a.contains("control") {
    "settings".into()
  } else if a.contains("gtk") || a.contains("demo") {
    "gtk".into()
  } else {
    "app".into()
  }
}

fn simple_hash(windows: &[WindowInfo], tray: &[TrayItem]) -> u64 {
  let mut h: u64 = 0xcbf29ce484222325;
  for w in windows {
    h = h.wrapping_mul(0x100000001b3);
    h ^= w.id;
    h = h.wrapping_mul(0x100000001b3);
    h ^= w.focused as u64;
    h = h.wrapping_mul(0x100000001b3);
    h ^= w.minimized as u64;
    h = h.wrapping_mul(0x100000001b3);
    h ^= w.maximized as u64;
    h = h.wrapping_mul(0x100000001b3);
    h ^= w.fullscreen as u64;
    h = h.wrapping_mul(0x100000001b3);
    h ^= w.x as u64;
    h = h.wrapping_mul(0x100000001b3);
    h ^= w.y as u64;
    for b in w.title.as_bytes() {
      h = h.wrapping_mul(0x100000001b3);
      h ^= *b as u64;
    }
    for b in w.app_id.as_bytes() {
      h = h.wrapping_mul(0x100000001b3);
      h ^= *b as u64;
    }
  }
  for t in tray {
    for field in [&t.id, &t.label, &t.icon, &t.tooltip] {
      for b in field.as_bytes() {
        h = h.wrapping_mul(0x100000001b3);
        h ^= *b as u64;
      }
    }
  }
  h
}

#[cfg(test)]
mod tests {
  use super::{split_window_id, window_id};

  #[test]
  fn shell_window_ids_are_client_scoped_and_reversible() {
    let a = window_id(7, 42);
    let b = window_id(8, 42);
    assert_ne!(a, b);
    assert_eq!(split_window_id(a), (7, 42));
    assert_eq!(split_window_id(b), (8, 42));
  }
}
