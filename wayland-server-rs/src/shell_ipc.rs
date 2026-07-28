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
use std::io::Write;
use std::os::unix::io::RawFd;
use std::path::PathBuf;

pub const STATE_FILE: &str = "luna-shell/state.json";
pub const CMD_SOCKET: &str = "luna-shell.sock";

#[derive(Clone, Debug)]
pub struct TrayItem {
  pub id: String,
  pub label: String,
  pub icon: String,
  pub tooltip: String,
}

#[derive(Clone, Debug)]
pub struct WindowInfo {
  pub surface_id: u32,
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
    })
  }

  pub fn cmd_fd(&self) -> RawFd { self.cmd_fd }

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

  pub fn collect_windows(
    clients: &HashMap<RawFd, Client>,
    focused_fd: RawFd,
    focused_surface: u32,
  ) -> Vec<WindowInfo> {
    let mut windows = Vec::new();
    for (&fd, client) in clients {
      for (&surface_id, obj) in &client.objects {
        let Role::Surface(s) = &obj.role else { continue };
        if s.popup || s.xdg_surface_id.is_none() || s.subsurface_parent.is_some() {
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
          surface_id,
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
    windows
  }

  pub fn collect_tray(
    &self,
    clients: &HashMap<RawFd, Client>,
    focused_fd: RawFd,
    focused_surface: u32,
  ) -> Vec<TrayItem> {
    let mut tray = self.extra_tray.clone();
    let mut seen = HashSet::new();
    for t in &tray {
      seen.insert(t.id.clone());
    }

    for client in clients.values() {
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
        let key = if app_id.is_empty() { format!("win:{}", surface_id) } else { format!("app:{}", app_id) };
        if seen.contains(&key) {
          continue;
        }
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
      for t in &mut tray {
        if t.id.ends_with(&format!(":{}", focused_surface)) {
          t.icon = format!("{}_active", t.icon);
        }
      }
    }
    tray
  }

  pub fn export_state(
    &mut self,
    clients: &HashMap<RawFd, Client>,
    focused_fd: RawFd,
    focused_surface: u32,
    force: bool,
    pending_menu: &mut Option<(u32, i32, i32)>,
    switcher: &Option<(usize, Vec<u32>)>,
  ) {
    let windows = Self::collect_windows(clients, focused_fd, focused_surface);
    let tray = self.collect_tray(clients, focused_fd, focused_surface);

    let hash = simple_hash(&windows, &tray)
      ^ pending_menu.map(|(id, x, y)| id as u64 ^ (x as u64) << 16 ^ (y as u64) << 32).unwrap_or(0)
      ^ switcher
        .as_ref()
        .map(|(i, ids)| *i as u64 ^ ids.len() as u64)
        .unwrap_or(0);
    if !force && hash == self.last_export {
      return;
    }
    self.last_export = hash;

    let path = self.runtime_dir.join(STATE_FILE);
    if let Some(parent) = path.parent() {
      let _ = std::fs::create_dir_all(parent);
    }

    let mut out = String::new();
    for w in &windows {
      out.push_str(&format!(
        "W\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n",
        w.surface_id,
        w.title.replace('\t', " ").replace('\n', " "),
        w.app_id.replace('\t', " "),
        if w.focused { 1 } else { 0 },
        if w.minimized { 1 } else { 0 },
        if w.maximized { 1 } else { 0 },
        if w.fullscreen { 1 } else { 0 },
        w.x,
        w.y,
      ));
    }
    for t in &tray {
      out.push_str(&format!(
        "T\t{}\t{}\t{}\t{}\n",
        t.id.replace('\t', " "),
        t.label.replace('\t', " "),
        t.icon.replace('\t', " "),
        t.tooltip.replace('\t', " ").replace('\n', " "),
      ));
    }
    if let Some((id, x, y)) = pending_menu.take() {
      out.push_str(&format!("M\t{}\t{}\t{}\n", id, x, y));
    }
    if let Some((idx, ids)) = switcher {
      let list: Vec<String> = ids.iter().map(|id| id.to_string()).collect();
      out.push_str(&format!("S\t{}\t{}\n", idx, list.join(",")));
    }

    if let Ok(mut f) = std::fs::File::create(&path) {
      let _ = f.write_all(out.as_bytes());
    }
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
  if a.contains("terminal") || a.contains("foot") || a.contains("sakura") {
    "terminal".into()
  } else if a.contains("firefox") || a.contains("browser") || a.contains("chrome") {
    "browser".into()
  } else if a.contains("nautilus") || a.contains("files") {
    "files".into()
  } else if a.contains("gedit") || a.contains("editor") {
    "editor".into()
  } else if a.contains("music") {
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
    h ^= w.surface_id as u64;
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
  }
  for t in tray {
    for b in t.id.as_bytes() {
      h = h.wrapping_mul(0x100000001b3);
      h ^= *b as u64;
    }
  }
  h
}
