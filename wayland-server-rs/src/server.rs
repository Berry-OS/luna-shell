/*
 * Copyright © 2026 Yuichiro Nakada / Project Vespera
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

use crate::object::{DmabufParams, DmabufPlane, Object, Role, Surface};
use crate::protocol::{self, Interface};
use crate::render::{probe_render_node, Backend, Framebuffer, InputEvent};
use crate::shell_ipc::ShellIpc;
use crate::shm::{ShmBuffer, ShmPool, FORMAT_ARGB8888, FORMAT_XRGB8888};
use crate::socket::{Conn, Listener};
use crate::types::Arg;
use crate::wire;
use std::collections::{HashMap, HashSet};
use std::ffi::CString;
use std::os::unix::io::RawFd;
use std::sync::mpsc;
use std::time::{SystemTime, UNIX_EPOCH};

// DRM fourcc / modifier for dmabuf
const DRM_FORMAT_ARGB8888: u32 = 0x3432_5241; // 'AR24'
const DRM_FORMAT_XRGB8888: u32 = 0x3432_5258; // 'XR24'
const DRM_FORMAT_MOD_LINEAR: u64 = 0;
const SERVER_ID_BASE: u32 = 0xff00_0000;

/// xdg_toplevel.state
const TOPLEVEL_STATE_MAXIMIZED: u32 = 1;
const TOPLEVEL_STATE_FULLSCREEN: u32 = 2;
const TOPLEVEL_STATE_RESIZING: u32 = 3;
const TOPLEVEL_STATE_ACTIVATED: u32 = 4;
const TOPLEVEL_STATE_TILED_LEFT: u32 = 5;
const TOPLEVEL_STATE_TILED_RIGHT: u32 = 6;

/// xdg_toplevel.resize_edge bits
const RESIZE_EDGE_TOP: u32 = 1;
const RESIZE_EDGE_BOTTOM: u32 = 2;
const RESIZE_EDGE_LEFT: u32 = 4;
const RESIZE_EDGE_RIGHT: u32 = 8;

/// Interactive window-manager grab (xdg_toplevel.move / .resize).
#[derive(Clone, Debug)]
enum WmGrab {
  None,
  Move {
    fd: RawFd,
    surface_id: u32,
    grab_px: i32,
    grab_py: i32,
    orig_x: i32,
    orig_y: i32,
  },
  Resize {
    fd: RawFd,
    surface_id: u32,
    edges: u32,
    grab_px: i32,
    grab_py: i32,
    orig_x: i32,
    orig_y: i32,
    orig_w: i32,
    orig_h: i32,
  },
}

struct Global {
  name: u32,
  interface: &'static Interface,
  version: u32,
}

pub struct Client {
  pub conn: Conn,
  pub objects: HashMap<u32, Object>,
  server_next_id: u32,
}

impl Client {
  fn new(fd: RawFd) -> Self {
    let mut objects = HashMap::new();
    objects.insert(1, Object::new(&protocol::WL_DISPLAY, 1, Role::Display));
    Client {
      conn: Conn::new(fd),
      objects,
      server_next_id: SERVER_ID_BASE,
    }
  }

  fn alloc_server_id(&mut self) -> u32 {
    let id = self.server_next_id;
    self.server_next_id = self.server_next_id.wrapping_add(1);
    id
  }

  pub fn send(&mut self, object_id: u32, opcode: u16, args: &[Arg]) {
    let (payload, fds) = wire::encode_args(args);
    let msg = wire::build_message(object_id, opcode, &payload);
    self.conn.queue(&msg, &fds);
  }

  fn post_error(&mut self, object_id: u32, code: u32, msg: &str) {
    eprintln!("[luna-compositor] post_error obj={} code={} msg={:?}", object_id, code, msg);
    self.send(1, 0, &[Arg::Object(object_id), Arg::Uint(code), Arg::Str(Some(msg.to_string()))]);
    self.conn.flush();
    self.conn.closed = true;
  }
}

pub struct Server {
  listener: Listener,
  clients: HashMap<RawFd, Client>,
  globals: Vec<Global>,
  backend: Box<dyn Backend>,
  fb: Framebuffer,
  serial: u32,
  dirty: bool,
  frame_done: Vec<(RawFd, u32)>,
  buffer_release: Vec<(RawFd, u32)>,
  epoll_fd: RawFd,
  signal_fd: RawFd,
  dmabuf_format_table: RawFd,
  dmabuf_format_table_size: usize,
  dmabuf_main_device: u64,

  input_rx: Option<mpsc::Receiver<InputEvent>>,
  input_wake_fd: RawFd,
  ptr_entered: bool,
  ptr_client_fd: RawFd,
  ptr_surface_id: u32,
  ptr_x: f32,
  ptr_y: f32,
  /// Client cursor from wl_pointer.set_cursor.  When the client passes a
  /// NULL surface we clear the client glyph and fall back to the built-in
  /// arrow — never leave the pointer permanently invisible.
  cursor_client_fd: RawFd,
  cursor_surface_id: u32,
  cursor_hot_x: i32,
  cursor_hot_y: i32,
  kbd_entered: bool,
  kbd_client_fd: RawFd,
  kbd_surface_id: u32,
  kbd_mods: u32,
  pressed_keys: HashSet<u32>,
  active_text_input: Option<(RawFd, u32, u32)>,

  shell_ipc: Option<ShellIpc>,
  focused_client_fd: RawFd,
  focused_surface_id: u32,
  shell_state_dirty: bool,
  /// Set by Ctrl+Alt+Backspace (session Zap) so run() exits cleanly.
  session_quit: bool,
  /// Bottom→top stacking order of mapped xdg_toplevel surfaces.
  window_stack: Vec<(RawFd, u32)>,
  /// Last pointer button-press serial (for xdg_toplevel.move/resize).
  last_button_serial: u32,
  last_button_pressed: bool,
  wm_grab: WmGrab,
  /// Deferred WM actions while the requesting client is outside `clients`.
  pending_activate: Option<u32>,
  pending_resize_configure: Option<(RawFd, u32, i32, i32)>,
  pending_configure: Vec<(RawFd, u32, Option<(i32, i32)>, bool)>,
  /// set_cursor arrived before the cursor surface had a buffer.
  pending_cursor: Option<(RawFd, u32)>,
  /// Clipboard selection: (owner client fd, wl_data_source id).
  selection: Option<(RawFd, u32)>,
  /// Propagate selection to all wl_data_devices after the requesting client is re-inserted.
  pending_selection_broadcast: bool,
  /// PRIMARY selection (middle-click paste).
  primary_selection: Option<(RawFd, u32)>,
  pending_primary_broadcast: bool,
  /// Compiled XKB keymap text (NUL-terminated) sent to every wl_keyboard.
  keymap_bytes: Vec<u8>,
  /// Pending window-menu request for luna-shell (surface_id, x, y).
  pending_shell_menu: Option<(u32, i32, i32)>,
  /// Alt+Tab switcher: (selected index, surface ids in cycle order).
  switcher: Option<(usize, Vec<u32>)>,
  /// SSD titlebar double-click → maximize (last click time_ms, surface_id).
  ssd_last_click: Option<(u32, u32)>,
  /// Super+D show-desktop: saved surface ids currently minimized by us.
  show_desktop_ids: Vec<u32>,
}

impl Server {
  pub fn new(socket_name: &str, backend: Box<dyn Backend>) -> std::io::Result<Self> {
    let listener = Listener::bind(socket_name)?;
    let (w, h) = backend.size();
    let epoll_fd = unsafe { libc::epoll_create1(libc::EPOLL_CLOEXEC) };
    if epoll_fd < 0 {
      return Err(std::io::Error::last_os_error());
    }
    let signal_fd = create_signal_fd()?;

    let (table_fd, table_size) = create_format_table();
    let main_device = detect_drm_device(backend.drm_render_device()).unwrap_or(0);

    let mut server = Server {
      listener,
      clients: HashMap::new(),
      globals: Vec::new(),
      backend,
      fb: Framebuffer::new(w, h),
      serial: 1,
      dirty: true,
      frame_done: Vec::new(),
      buffer_release: Vec::new(),
      epoll_fd,
      signal_fd,
      dmabuf_format_table: table_fd,
      dmabuf_format_table_size: table_size,
      dmabuf_main_device: main_device,

      input_rx: None,
      input_wake_fd: -1,
      ptr_entered: false,
      ptr_client_fd: -1,
      ptr_surface_id: 0,
      ptr_x: 0.5,
      ptr_y: 0.5,
      cursor_client_fd: -1,
      cursor_surface_id: 0,
      cursor_hot_x: 1,
      cursor_hot_y: 1,
      kbd_entered: false,
      kbd_client_fd: -1,
      kbd_surface_id: 0,
      kbd_mods: 0,
      pressed_keys: HashSet::new(),
      active_text_input: None,

      shell_ipc: ShellIpc::open(),
      focused_client_fd: -1,
      focused_surface_id: 0,
      shell_state_dirty: true,
      session_quit: false,
      window_stack: Vec::new(),
      last_button_serial: 0,
      last_button_pressed: false,
      wm_grab: WmGrab::None,
      pending_activate: None,
      pending_resize_configure: None,
      pending_configure: Vec::new(),
      pending_cursor: None,
      selection: None,
      pending_selection_broadcast: false,
      primary_selection: None,
      pending_primary_broadcast: false,
      keymap_bytes: build_xkb_keymap(None, None, None),
      pending_shell_menu: None,
      switcher: None,
      ssd_last_click: None,
      show_desktop_ids: Vec::new(),
    };

    if let Some(ref ipc) = server.shell_ipc {
      server.epoll_add(ipc.cmd_fd());
    }

    if let Some((rx, wake_fd)) = server.backend.take_input_channel() {
      server.input_rx = Some(rx);
      server.input_wake_fd = wake_fd;
      server.epoll_add(wake_fd);
    }

    server.epoll_add(server.listener.fd);
    server.epoll_add(server.signal_fd);

    // Advertise dmabuf only when feedback can be completed.  Mesa requests
    // both default and per-surface feedback, so a half-usable global makes
    // eglCreateWindowSurface fail instead of falling back to wl_shm.
    let mut advertised: Vec<(&'static Interface, u32)> = vec![
      (&protocol::WL_COMPOSITOR, 6),
      (&protocol::WL_SUBCOMPOSITOR, 1),
      (&protocol::WL_SHM, 1),
      (&protocol::WL_SEAT, 7),
      (&protocol::WL_OUTPUT, 4),
      (&protocol::WL_DATA_DEVICE_MANAGER, 3),
      (&protocol::ZWP_PRIMARY_SELECTION_DEVICE_MANAGER_V1, 1),
      (&protocol::ZXDG_DECORATION_MANAGER_V1, 1),
      (&protocol::ZWP_TEXT_INPUT_MANAGER_V3, 1),
      (&protocol::ZWP_INPUT_METHOD_MANAGER_V2, 1),
      (&protocol::ZWP_VIRTUAL_KEYBOARD_MANAGER_V1, 1),
      (&protocol::XDG_WM_BASE, 5),
      (&protocol::ZWLR_LAYER_SHELL_V1, 4),
    ];
    if server.dmabuf_format_table >= 0 && server.dmabuf_main_device != 0 {
      advertised.push((&protocol::ZWP_LINUX_DMABUF_V1, 4));
    } else {
      eprintln!(
        "[luna-compositor] dmabuf feedback unavailable; using wl_shm \
         (clients render through llvmpipe)"
      );
    }
    for (i, (iface, ver)) in advertised.iter().enumerate() {
      server.globals.push(Global {
        name: (i + 1) as u32,
        interface: iface,
        version: *ver,
      });
    }

    Ok(server)
  }

  fn epoll_add(&self, fd: RawFd) {
    let mut ev = libc::epoll_event {
      events: libc::EPOLLIN as u32,
      u64: fd as u64,
    };
    unsafe { libc::epoll_ctl(self.epoll_fd, libc::EPOLL_CTL_ADD, fd, &mut ev) };
  }

  fn epoll_del(&self, fd: RawFd) { unsafe { libc::epoll_ctl(self.epoll_fd, libc::EPOLL_CTL_DEL, fd, std::ptr::null_mut()) }; }

  fn next_serial(&mut self) -> u32 {
    let s = self.serial;
    self.serial = self.serial.wrapping_add(1);
    s
  }

  pub fn run(&mut self) {
    let mut events = vec![libc::epoll_event { events: 0, u64: 0 }; 64];
    let mut running = true;
    while running {
      if self.session_quit {
        eprintln!("[luna-compositor] session quit (Ctrl+Alt+Backspace)");
        break;
      }
      let timeout = if self.dirty { 0 } else { -1 };
      let n = unsafe { libc::epoll_wait(self.epoll_fd, events.as_mut_ptr(), events.len() as i32, timeout) };
      if n < 0 {
        let e = std::io::Error::last_os_error();
        if e.kind() == std::io::ErrorKind::Interrupted {
          continue;
        }
        break;
      }

      // Prefer client sockets over the input wake fd so xdg_toplevel.move /
      // set_cursor responses to a prior button/enter are applied before we
      // drain a release or the next motion that would clear grab state.
      let mut input_ready = false;
      for ev in events.iter().take(n as usize) {
        let fd = ev.u64 as RawFd;
        if fd == self.listener.fd {
          self.accept_clients();
        } else if fd == self.signal_fd {
          running = self.process_signals();
          if !running {
            break;
          }
        } else if Some(fd) == self.shell_ipc.as_ref().map(|i| i.cmd_fd()) {
          self.handle_shell_commands();
        } else if fd == self.input_wake_fd && self.input_wake_fd >= 0 {
          let mut buf = [0u8; 8];
          unsafe { libc::read(self.input_wake_fd, buf.as_mut_ptr() as *mut libc::c_void, 8) };
          input_ready = true;
        } else {
          self.recv_client(fd);
        }
      }
      if input_ready {
        self.process_input_events();
      }

      if self.dirty {
        self.composite_and_present();
        self.dirty = false;
      }

      if self.shell_state_dirty {
        self.export_shell_state(false);
      }

      for c in self.clients.values_mut() {
        c.conn.flush();
      }
    }
  }

  fn accept_clients(&mut self) {
    loop {
      match self.listener.accept() {
        Ok(Some(fd)) => {
          self.epoll_add(fd);
          self.clients.insert(fd, Client::new(fd));
          eprintln!("[luna-compositor] client connected (fd={})", fd);
        }
        Ok(None) => break,
        Err(_) => break,
      }
    }
  }

  fn recv_client(&mut self, fd: RawFd) {
    let mut client = match self.clients.remove(&fd) {
      Some(c) => c,
      None => return,
    };

    let mut drop_client = false;
    loop {
      match client.conn.recv() {
        Ok(0) => break,
        Ok(_) => {}
        Err(_) => {
          drop_client = true;
          break;
        }
      }
    }

    while !client.conn.closed {
      let (msg, consumed) = match wire::decode_header(&client.conn.recv_buf) {
        Some(x) => x,
        None => break,
      };
      client.conn.recv_buf.drain(..consumed);
      self.handle_request(&mut client, msg);
    }

    if drop_client || client.conn.closed {
      self.epoll_del(fd);
      eprintln!("[luna-compositor] client disconnected (fd={})", fd);
      self.dirty = true;
      if self.ptr_client_fd == fd {
        self.ptr_entered = false;
        self.ptr_client_fd = -1;
      }
      if self.cursor_client_fd == fd {
        self.cursor_client_fd = -1;
        self.cursor_surface_id = 0;
      }
      if self.kbd_client_fd == fd {
        self.kbd_entered = false;
        self.kbd_client_fd = -1;
      }
      if self.active_text_input.map(|v| v.0) == Some(fd) {
        self.active_text_input = None;
        self.deactivate_input_methods(None);
      }
      if self.focused_client_fd == fd {
        self.focused_client_fd = -1;
        self.focused_surface_id = 0;
      }
      self.window_stack.retain(|(f, _)| *f != fd);
      if matches!(self.wm_grab, WmGrab::Move { fd: gfd, .. } | WmGrab::Resize { fd: gfd, .. } if gfd == fd) {
        self.wm_grab = WmGrab::None;
      }
      if self.selection.map(|(sfd, _)| sfd) == Some(fd) {
        self.selection = None;
        self.pending_selection_broadcast = true;
      }
      if self.primary_selection.map(|(sfd, _)| sfd) == Some(fd) {
        self.primary_selection = None;
        self.pending_primary_broadcast = true;
      }
      self.shell_state_dirty = true;
      drop(client);
      self.flush_selection_broadcast();
      self.flush_primary_broadcast();
    } else {
      client.conn.flush();
      self.clients.insert(fd, client);
      self.flush_wm_actions();
      self.flush_selection_broadcast();
      self.flush_primary_broadcast();
    }
  }

  fn flush_wm_actions(&mut self) {
    if let Some(sid) = self.pending_activate.take() {
      self.activate_surface(sid);
    }
    if let Some((fd, sid, w, h)) = self.pending_resize_configure.take() {
      self.send_toplevel_configure_for(fd, sid, Some((w, h)), true);
    }
    let configures = std::mem::take(&mut self.pending_configure);
    for (fd, sid, size, resizing) in configures {
      self.send_toplevel_configure_for(fd, sid, size, resizing);
    }
  }

  fn handle_request(&mut self, client: &mut Client, msg: wire::RawMessage) {
    let iface = match client.objects.get(&msg.object_id) {
      Some(o) => o.interface,
      None => {
        eprintln!("[luna-compositor] unknown object id={} op={}", msg.object_id, msg.opcode);
        return;
      }
    };
    let sig = iface.request_sig(msg.opcode).unwrap_or("");
    let args = wire::decode_args(sig, &msg.payload, &mut client.conn.recv_fds);
    let id = msg.object_id;

    match iface.name {
      "wl_display" => self.req_display(client, id, msg.opcode, &args),
      "wl_registry" => self.req_registry(client, id, msg.opcode, &args),
      "wl_compositor" => self.req_compositor(client, msg.opcode, &args),
      "wl_subcompositor" => self.req_subcompositor(client, msg.opcode, &args),
      "wl_subsurface" => self.req_subsurface(client, id, msg.opcode, &args),
      "wl_shm" => self.req_shm(client, msg.opcode, &args),
      "wl_shm_pool" => self.req_shm_pool(client, id, msg.opcode, &args),
      "wl_buffer" => self.req_simple_destroy(client, id, msg.opcode, 0),
      "wl_surface" => self.req_surface(client, id, msg.opcode, &args),
      "wl_region" => self.req_region(client, id, msg.opcode),
      "wl_seat" => self.req_seat(client, msg.opcode, &args),
      "wl_pointer" | "wl_keyboard" => self.req_input_device(client, id, msg.opcode, &args),
      "wl_output" => {}
      "wl_data_device_manager" => self.req_ddm(client, msg.opcode, &args),
      "wl_data_source" => self.req_data_source(client, id, msg.opcode, &args),
      "wl_data_device" => self.req_data_device(client, id, msg.opcode, &args),
      "wl_data_offer" => self.req_data_offer(client, id, msg.opcode, &args),
      "zwp_primary_selection_device_manager_v1" => self.req_primary_manager(client, id, msg.opcode, &args),
      "zwp_primary_selection_device_v1" => self.req_primary_device(client, id, msg.opcode, &args),
      "zwp_primary_selection_source_v1" => self.req_primary_source(client, id, msg.opcode, &args),
      "zwp_primary_selection_offer_v1" => self.req_primary_offer(client, id, msg.opcode, &args),
      "zxdg_decoration_manager_v1" => self.req_decoration_manager(client, id, msg.opcode, &args),
      "zxdg_toplevel_decoration_v1" => self.req_toplevel_decoration(client, id, msg.opcode, &args),
      "zwp_text_input_manager_v3" => self.req_text_input_manager(client, id, msg.opcode, &args),
      "zwp_text_input_v3" => self.req_text_input(client, id, msg.opcode, &args),
      "zwp_input_method_manager_v2" => self.req_input_method_manager(client, id, msg.opcode, &args),
      "zwp_input_method_v2" => self.req_input_method(client, id, msg.opcode, &args),
      "zwp_input_popup_surface_v2" => self.req_input_popup_surface(client, id, msg.opcode),
      "zwp_input_method_keyboard_grab_v2" => self.req_input_method_keyboard_grab(client, id, msg.opcode),
      "zwp_virtual_keyboard_manager_v1" => self.req_virtual_keyboard_manager(client, id, msg.opcode, &args),
      "zwp_virtual_keyboard_v1" => self.req_virtual_keyboard(client, id, msg.opcode, &args),
      "xdg_wm_base" => self.req_wm_base(client, id, msg.opcode, &args),
      "xdg_positioner" => self.req_positioner(client, id, msg.opcode, &args),
      "xdg_surface" => self.req_xdg_surface(client, id, msg.opcode, &args),
      "xdg_toplevel" => self.req_xdg_toplevel(client, id, msg.opcode, &args),
      "xdg_popup" => self.req_xdg_popup(client, id, msg.opcode, &args),
      "zwp_linux_dmabuf_v1" => self.req_dmabuf(client, id, msg.opcode, &args),
      "zwp_linux_buffer_params_v1" => self.req_dmabuf_params(client, id, msg.opcode, &args),
      "zwp_linux_dmabuf_feedback_v1" => self.req_simple_destroy(client, id, msg.opcode, 0),
      "zwlr_layer_shell_v1" => self.req_layer_shell(client, id, msg.opcode, &args),
      "zwlr_layer_surface_v1" => self.req_layer_surface(client, id, msg.opcode, &args),
      _ => {}
    }
  }

  fn req_display(&mut self, client: &mut Client, id: u32, opcode: u16, args: &[Arg]) {
    match opcode {
      0 => {
        let cb = args.get(0).map(|a| a.as_object()).unwrap_or(0);
        if cb == 0 {
          return;
        }
        client.objects.insert(cb, Object::new(&protocol::WL_CALLBACK, 1, Role::Callback));
        let serial = self.next_serial();
        client.send(cb, 0, &[Arg::Uint(serial)]);
        client.send(1, 1, &[Arg::Uint(cb)]);
        client.objects.remove(&cb);
      }
      1 => {
        let reg = args.get(0).map(|a| a.as_object()).unwrap_or(0);
        if reg == 0 {
          return;
        }
        client.objects.insert(reg, Object::new(&protocol::WL_REGISTRY, 1, Role::Registry));
        for g in &self.globals {
          client.send(reg, 0, &[Arg::Uint(g.name), Arg::Str(Some(g.interface.name.to_string())), Arg::Uint(g.version)]);
        }
      }
      _ => {
        let _ = id;
      }
    }
  }

  fn req_registry(&mut self, client: &mut Client, _id: u32, opcode: u16, args: &[Arg]) {
    if opcode != 0 {
      return;
    }
    let name = args.get(0).map(|a| a.as_uint()).unwrap_or(0);
    let iface_name = args.get(1).and_then(|a| a.as_str()).unwrap_or("");
    let req_ver = args.get(2).map(|a| a.as_uint()).unwrap_or(1);
    let new_id = args.get(3).map(|a| a.as_object()).unwrap_or(0);
    if new_id == 0 {
      return;
    }
    let global = self.globals.iter().find(|g| g.name == name);
    let iface = match (global, protocol::by_name(iface_name)) {
      (Some(g), _) => g.interface,
      (None, Some(i)) => i,
      _ => {
        client.post_error(1, 0, "bind: unknown global");
        return;
      }
    };
    let version = req_ver.min(iface.version);
    let role = role_for(iface.name);
    client.objects.insert(new_id, Object::new(iface, version, role));

    self.init_global_object(client, new_id, iface.name, version);
  }

  fn init_global_object(&mut self, client: &mut Client, id: u32, iface: &str, version: u32) {
    match iface {
      "wl_shm" => {
        client.send(id, 0, &[Arg::Uint(FORMAT_ARGB8888)]);
        client.send(id, 0, &[Arg::Uint(FORMAT_XRGB8888)]);
      }
      "wl_seat" => {
        client.send(id, 0, &[Arg::Uint(0x3)]);
        client.send(id, 1, &[Arg::Str(Some("seat0".into()))]);
      }
      "wl_output" => {
        let (w, h) = self.backend.size();
        // Physical size must match pixel size at ~96 DPI.  The old hardcoded
        // 300×200 mm made a 1920×1080 panel look ~163 DPI, so GTK/Firefox
        // drew text and chrome ~1.7× larger than under a typical Xorg session.
        // Override with LUNA_OUTPUT_DPI (e.g. 110) when a denser panel is wanted.
        let dpi = std::env::var("LUNA_OUTPUT_DPI")
          .ok()
          .and_then(|s| s.parse::<f32>().ok())
          .filter(|d| *d >= 48.0 && *d <= 480.0)
          .unwrap_or(96.0);
        let phys_w = ((w as f32) * 25.4 / dpi).round().max(1.0) as i32;
        let phys_h = ((h as f32) * 25.4 / dpi).round().max(1.0) as i32;
        client.send(
          id,
          0,
          &[
            Arg::Int(0),
            Arg::Int(0),
            Arg::Int(phys_w),
            Arg::Int(phys_h),
            Arg::Int(0),
            Arg::Str(Some("berry-lab".into())),
            Arg::Str(Some("Vespera".into())),
            Arg::Int(0),
          ],
        );
        client.send(id, 1, &[Arg::Uint(0x3), Arg::Int(w as i32), Arg::Int(h as i32), Arg::Int(60000)]);
        client.send(id, 3, &[Arg::Int(1)]); // scale = 1
        if version >= 4 {
          client.send(id, 4, &[Arg::Str(Some("LUNA-1".into()))]);
          client.send(id, 5, &[Arg::Str(Some(format!("Luna {w}x{h} @{dpi:.0}DPI")))]);
        }
        client.send(id, 2, &[]);
        eprintln!(
          "[luna-compositor] wl_output: {}x{} px, {}x{} mm (~{:.0} DPI), scale=1",
          w, h, phys_w, phys_h, dpi
        );
      }
      "zwp_linux_dmabuf_v1" => {
        // Pre-v4 clients learn formats via format/modifier events; v4+ uses get_default_feedback.
        if version < 4 {
          for &fmt in &[DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888] {
            client.send(id, 0, &[Arg::Uint(fmt)]);
            let m = DRM_FORMAT_MOD_LINEAR;
            client.send(
              id,
              1, // modifier(format, mod_hi, mod_lo)
              &[Arg::Uint(fmt), Arg::Uint((m >> 32) as u32), Arg::Uint(m as u32)],
            );
          }
        }
      }
      _ => {}
    }
  }

  fn req_compositor(&mut self, client: &mut Client, opcode: u16, args: &[Arg]) {
    match opcode {
      0 => {
        let nid = args.get(0).map(|a| a.as_object()).unwrap_or(0);
        if nid != 0 {
          client.objects.insert(nid, Object::new(&protocol::WL_SURFACE, 6, Role::Surface(Surface::default())));
        }
      }
      1 => {
        let nid = args.get(0).map(|a| a.as_object()).unwrap_or(0);
        if nid != 0 {
          client.objects.insert(nid, Object::new(&protocol::WL_REGION, 1, Role::Region { has_rects: false }));
        }
      }
      _ => {}
    }
  }

  fn req_subcompositor(&mut self, client: &mut Client, opcode: u16, args: &[Arg]) {
    if opcode == 1 {
      // get_subsurface(id, surface, parent)
      let nid = args.get(0).map(|a| a.as_object()).unwrap_or(0);
      let surf = args.get(1).map(|a| a.as_object()).unwrap_or(0);
      let parent = args.get(2).map(|a| a.as_object()).unwrap_or(0);
      if nid == 0 || surf == 0 || parent == 0 {
        return;
      }
      client.objects.insert(
        nid,
        Object::new(
          &protocol::WL_SUBSURFACE,
          1,
          Role::Subsurface {
            surface_id: surf,
            parent_id: parent,
            x: 0,
            y: 0,
            sync: true,
          },
        ),
      );
      if let Some(Object {
        role: Role::Surface(s),
        ..
      }) = client.objects.get_mut(&surf)
      {
        s.subsurface_parent = Some(parent);
      }
      self.dirty = true;
    }
  }

  fn req_subsurface(&mut self, client: &mut Client, id: u32, opcode: u16, args: &[Arg]) {
    match opcode {
      0 => {
        // destroy — unlink child from parent; child surface remains usable.
        let child = match client.objects.get(&id) {
          Some(Object {
            role: Role::Subsurface { surface_id, .. },
            ..
          }) => *surface_id,
          _ => {
            client.objects.remove(&id);
            return;
          }
        };
        if let Some(Object {
          role: Role::Surface(s),
          ..
        }) = client.objects.get_mut(&child)
        {
          s.subsurface_parent = None;
        }
        client.objects.remove(&id);
        self.dirty = true;
      }
      1 => {
        // set_position(x, y)
        let x = args.get(0).map(|a| a.as_int()).unwrap_or(0);
        let y = args.get(1).map(|a| a.as_int()).unwrap_or(0);
        if let Some(Object {
          role: Role::Subsurface {
            x: sx, y: sy, ..
          },
          ..
        }) = client.objects.get_mut(&id)
        {
          *sx = x;
          *sy = y;
        }
        self.dirty = true;
      }
      2 | 3 => {
        // place_above / place_below — stack order ignored; draw in id order.
      }
      4 => {
        if let Some(Object {
          role: Role::Subsurface { sync, .. },
          ..
        }) = client.objects.get_mut(&id)
        {
          *sync = true;
        }
      }
      5 => {
        if let Some(Object {
          role: Role::Subsurface { sync, .. },
          ..
        }) = client.objects.get_mut(&id)
        {
          *sync = false;
        }
      }
      _ => {}
    }
  }

  fn req_shm(&mut self, client: &mut Client, opcode: u16, args: &[Arg]) {
    if opcode == 0 {
      let nid = args.get(0).map(|a| a.as_object()).unwrap_or(0);
      let fd = args.get(1).map(|a| a.as_fd()).unwrap_or(-1);
      let size = args.get(2).map(|a| a.as_int()).unwrap_or(0);
      let pool = ShmPool::map(fd, size.max(0) as usize);
      if nid != 0 {
        client.objects.insert(nid, Object::new(&protocol::WL_SHM_POOL, 1, Role::ShmPool { pool }));
      }
    }
  }

  fn req_shm_pool(&mut self, client: &mut Client, id: u32, opcode: u16, args: &[Arg]) {
    match opcode {
      0 => {
        let nid = args.get(0).map(|a| a.as_object()).unwrap_or(0);
        let offset = args.get(1).map(|a| a.as_int()).unwrap_or(0);
        let w = args.get(2).map(|a| a.as_int()).unwrap_or(0);
        let h = args.get(3).map(|a| a.as_int()).unwrap_or(0);
        let stride = args.get(4).map(|a| a.as_int()).unwrap_or(0);
        let format = args.get(5).map(|a| a.as_uint()).unwrap_or(0);

        let pool_rc = match client.objects.get(&id) {
          Some(Object {
            role: Role::ShmPool { pool: Some(p) },
            ..
          }) => p.clone(),
          _ => return,
        };
        if nid != 0 {
          let buf = ShmBuffer {
            pool: pool_rc,
            offset: offset.max(0) as usize,
            width: w,
            height: h,
            stride,
            format,
          };
          client.objects.insert(nid, Object::new(&protocol::WL_BUFFER, 1, Role::Buffer(buf)));
        }
      }
      1 => {
        client.objects.remove(&id);
      }
      _ => {}
    }
  }

  fn req_surface(&mut self, client: &mut Client, id: u32, opcode: u16, args: &[Arg]) {
    match opcode {
      0 => {
        if self.active_text_input.map(|v| (v.0, v.2)) == Some((client.conn.fd, id)) {
          self.active_text_input = None;
          self.deactivate_input_methods(None);
        }
        if self.focused_client_fd == client.conn.fd && self.focused_surface_id == id {
          self.focused_client_fd = -1;
          self.focused_surface_id = 0;
        }
        if self.cursor_client_fd == client.conn.fd && self.cursor_surface_id == id {
          self.cursor_client_fd = -1;
          self.cursor_surface_id = 0; // fall back to default arrow
        }
        if self.pending_cursor == Some((client.conn.fd, id)) {
          self.pending_cursor = None;
        }
        self.track_mapped_toplevel(client.conn.fd, id, false);
        client.objects.remove(&id);
        self.dirty = true;
      }
      1 => {
        let buf = args.get(0).map(|a| a.as_object()).unwrap_or(0);
        if let Some(Object {
          role: Role::Surface(s),
          ..
        }) = client.objects.get_mut(&id)
        {
          s.pending_buffer = if buf == 0 { None } else { Some(buf) };
          s.pending_attach = true;
        }
      }
      3 => {
        let cb = args.get(0).map(|a| a.as_object()).unwrap_or(0);
        if cb != 0 {
          client.objects.insert(cb, Object::new(&protocol::WL_CALLBACK, 1, Role::Callback));
          if let Some(Object {
            role: Role::Surface(s),
            ..
          }) = client.objects.get_mut(&id)
          {
            s.frame_callbacks.push(cb);
          }
        }
      }
      5 => {
        // set_input_region: NULL(0) = full surface, region_id = specific region
        let region_id = args.get(0).map(|a| a.as_object()).unwrap_or(0);
        let accepts = if region_id == 0 {
          true
        } else {
          matches!(client.objects.get(&region_id), Some(Object { role: Role::Region { has_rects: true }, .. }))
        };
        if let Some(Object { role: Role::Surface(s), .. }) = client.objects.get_mut(&id) {
          s.accepts_input = accepts;
        }
      }
      6 => self.surface_commit(client, id),
      _ => {}
    }
  }

  fn surface_commit(&mut self, client: &mut Client, id: u32) {
    let (attach, pending_buf, frames) = match client.objects.get_mut(&id) {
      Some(Object {
        role: Role::Surface(s),
        ..
      }) => {
        let frames = std::mem::take(&mut s.frame_callbacks);
        (s.pending_attach, s.pending_buffer, frames)
      }
      _ => return,
    };

    let fd = client.conn.fd;
    let new_buffer = if attach {
      match pending_buf {
        None => None,
        Some(bid) => match client.objects.get(&bid) {
          Some(Object {
            role: Role::Buffer(b),
            ..
          }) => {
            let clone = b.clone();
            // Defer wl_buffer.release until after composite_and_present so
            // Mesa EGL doesn't free the buffer while eglSwapBuffers is still
            // waiting for the wl_callback.done event.
            self.buffer_release.push((fd, bid));
            Some(clone)
          }
          _ => None,
        },
      }
    } else {
      None
    };

    let (was_mapped, is_toplevel) = match client.objects.get(&id) {
      Some(Object { role: Role::Surface(s), .. }) => (
        s.mapped,
        s.xdg_surface_id.is_some() && !s.popup && !s.input_method_popup && s.layer_surface_id.is_none(),
      ),
      _ => (false, false),
    };

    if let Some(Object {
      role: Role::Surface(s),
      ..
    }) = client.objects.get_mut(&id)
    {
      if attach {
        // Atomic buffer swap: never clear before installing the replacement.
        // `new_buffer == None` is an intentional unmap (attach NULL).
        s.mapped = new_buffer.is_some();
        if let Some(buf) = new_buffer {
          s.current_buffer = Some(buf);
        } else {
          s.current_buffer = None;
        }
      }
      s.pending_attach = false;
      s.pending_buffer = None;
    }

    // Track xdg_toplevel surfaces in the WM stacking list.
    if is_toplevel {
      let mapped = matches!(
        client.objects.get(&id),
        Some(Object { role: Role::Surface(s), .. }) if s.mapped
      );
      self.track_mapped_toplevel(fd, id, mapped);
      // First map of a toplevel — raise, place in usable area, activate.
      if mapped && attach && !was_mapped {
        let cascade = (self.window_stack.len().saturating_sub(1) as i32) * 28;
        let (buf_w, buf_h) = match client.objects.get(&id) {
          Some(Object {
            role: Role::Surface(s),
            ..
          }) => s
            .current_buffer
            .as_ref()
            .map(|b| (b.width, b.height))
            .unwrap_or((640, 480)),
          _ => (640, 480),
        };
        let (ux, uy, uw, uh) = self.usable_area();
        if let Some(Object {
          role: Role::Surface(s),
          ..
        }) = client.objects.get_mut(&id)
        {
          // Absolute coords: centre in usable area, cascade so stacks don't overlap.
          if s.x == 0 && s.y == 0 {
            s.x = ux + ((uw - buf_w) / 2).max(0) + cascade;
            s.y = uy + ((uh - buf_h) / 2).max(0) + cascade;
          }
        }
        self.raise_surface(fd, id);
        self.pending_activate = Some(id);
        self.shell_state_dirty = true;
      } else if was_mapped && !mapped {
        self.shell_state_dirty = true;
      }
    }

    // Firefox / WebRender: content often lives on a desync subsurface while the
    // xdg_toplevel parent stays buffer-less.  Promote the parent into the WM
    // stack as soon as any child has pixels so the window is visible + focused.
    let parent_of_sub = match client.objects.get(&id) {
      Some(Object {
        role: Role::Surface(s),
        ..
      }) => s.subsurface_parent,
      _ => None,
    };
    if let Some(parent_id) = parent_of_sub {
      let child_mapped = matches!(
        client.objects.get(&id),
        Some(Object {
          role: Role::Surface(s),
          ..
        }) if s.mapped
      );
      let parent_is_toplevel = matches!(
        client.objects.get(&parent_id),
        Some(Object {
          role: Role::Surface(s),
          ..
        }) if s.xdg_surface_id.is_some() && !s.popup && s.layer_surface_id.is_none()
      );
      if child_mapped && parent_is_toplevel {
        let parent_was = self.window_stack.iter().any(|&(f, s)| f == fd && s == parent_id);
        self.track_mapped_toplevel(fd, parent_id, true);
        if !parent_was {
          let cascade = (self.window_stack.len().saturating_sub(1) as i32) * 28;
          let (buf_w, buf_h) = Self::surface_tree_size(client, parent_id)
            .unwrap_or((960, 640));
          let (ux, uy, uw, uh) = self.usable_area();
          if let Some(Object {
            role: Role::Surface(s),
            ..
          }) = client.objects.get_mut(&parent_id)
          {
            if s.x == 0 && s.y == 0 {
              s.x = ux + ((uw - buf_w) / 2).max(0) + cascade;
              s.y = uy + ((uh - buf_h) / 2).max(0) + cascade;
            }
          }
          self.raise_surface(fd, parent_id);
          self.pending_activate = Some(parent_id);
          self.shell_state_dirty = true;
        }
      }
    }

    // Cursor surface got its first buffer after set_cursor — adopt it now.
    if let Some((cfd, csid)) = self.pending_cursor {
      if cfd == fd && csid == id {
        let ready = matches!(
          client.objects.get(&id),
          Some(Object { role: Role::Surface(s), .. })
            if s.current_buffer.as_ref().map(|b| b.width > 0 && b.height > 0).unwrap_or(false)
        );
        if ready && matches!(self.wm_grab, WmGrab::None) && (!self.ptr_entered || self.ptr_client_fd == fd) {
          self.cursor_client_fd = fd;
          self.cursor_surface_id = id;
          self.pending_cursor = None;
        }
      }
    }

    for cb in frames {
      self.frame_done.push((fd, cb));
    }

    if let Some(popup_id) = client.objects.iter().find_map(|(&oid, obj)| match obj.role {
      Role::InputPopupSurface { surface_id, .. } if surface_id == id => Some(oid),
      _ => None,
    }) {
      self.position_one_input_method_popup(client, popup_id);
    }

    // Layer surface: send configure on first commit if not yet configured
    let layer_info = {
      let lsid = match client.objects.get(&id) {
        Some(Object { role: Role::Surface(s), .. }) => s.layer_surface_id,
        _ => None,
      };
      lsid.and_then(|lsid| match client.objects.get(&lsid) {
        Some(Object {
          role: Role::LayerSurface { anchor, size_w, size_h, margin_top, margin_right, margin_bottom, margin_left, configured, .. },
          ..
        }) if !configured => Some((lsid, *anchor, *size_w, *size_h, *margin_top, *margin_right, *margin_bottom, *margin_left)),
        _ => None,
      })
    };
    if let Some((lsid, anchor, size_w, size_h, mt, mr, mb, ml)) = layer_info {
      let (bw, bh) = self.backend.size();
      let (_, _, conf_w, conf_h) = layer_surface_rect(bw, bh, anchor, size_w, size_h, mt, mr, mb, ml);
      let serial = self.next_serial();
      client.send(lsid, 0, &[Arg::Uint(serial), Arg::Uint(conf_w), Arg::Uint(conf_h)]);
      if let Some(Object { role: Role::LayerSurface { configure_serial, .. }, .. }) = client.objects.get_mut(&lsid) {
        *configure_serial = serial;
      }
    }

    self.dirty = true;
    // Do NOT mark shell_state_dirty on every buffer commit — VTE/sakura
    // blinks the cursor ~2 Hz and that was rewriting state.json + menubar
    // every blink (visible as terminal flicker).  Map/unmap/title/focus
    // paths set shell_state_dirty themselves.
  }

  fn surface_is_minimized(&self, client: &Client, surface_id: u32) -> bool {
    let Some(Object {
      role: Role::Surface(s),
      ..
    }) = client.objects.get(&surface_id)
    else {
      return false;
    };
    let Some(xdg_id) = s.xdg_surface_id else { return false };
    for obj in client.objects.values() {
      if let Role::XdgToplevel {
        xdg_surface_id,
        minimized,
        ..
      } = &obj.role
      {
        if *xdg_surface_id == xdg_id {
          return *minimized;
        }
      }
    }
    false
  }

  /// Bounding size of a surface plus its subsurface tree (for first-map place).
  fn surface_tree_size(client: &Client, surface_id: u32) -> Option<(i32, i32)> {
    fn walk(client: &Client, sid: u32, depth: u32) -> (i32, i32) {
      if depth > 8 {
        return (0, 0);
      }
      let mut w = 0i32;
      let mut h = 0i32;
      if let Some(Object {
        role: Role::Surface(s),
        ..
      }) = client.objects.get(&sid)
      {
        if let Some(buf) = &s.current_buffer {
          w = buf.width;
          h = buf.height;
        }
      }
      for obj in client.objects.values() {
        if let Role::Subsurface {
          surface_id: child,
          parent_id,
          x,
          y,
          ..
        } = &obj.role
        {
          if *parent_id != sid {
            continue;
          }
          let (cw, ch) = walk(client, *child, depth + 1);
          w = w.max(*x + cw);
          h = h.max(*y + ch);
        }
      }
      (w, h)
    }
    let (w, h) = walk(client, surface_id, 0);
    if w > 0 && h > 0 {
      Some((w, h))
    } else {
      None
    }
  }

  /// True when this surface or any subsurface descendant has a mapped buffer.
  fn surface_tree_has_content(client: &Client, surface_id: u32) -> bool {
    fn walk(client: &Client, sid: u32, depth: u32) -> bool {
      if depth > 8 {
        return false;
      }
      if let Some(Object {
        role: Role::Surface(s),
        ..
      }) = client.objects.get(&sid)
      {
        if s.mapped && s.current_buffer.is_some() {
          return true;
        }
      }
      for obj in client.objects.values() {
        if let Role::Subsurface {
          surface_id: child,
          parent_id,
          ..
        } = &obj.role
        {
          if *parent_id == sid && walk(client, *child, depth + 1) {
            return true;
          }
        }
      }
      false
    }
    walk(client, surface_id, 0)
  }

  /// Collect SHM blits for a surface and its subsurface children (Firefox content path).
  fn collect_surface_tree_blits(
    client: &Client,
    surface_id: u32,
    ox: i32,
    oy: i32,
    depth: u32,
  ) -> Vec<(i32, i32, crate::shm::ShmBuffer)> {
    let mut out = Vec::new();
    fn walk(
      client: &Client,
      sid: u32,
      ox: i32,
      oy: i32,
      depth: u32,
      out: &mut Vec<(i32, i32, crate::shm::ShmBuffer)>,
    ) {
      if depth > 8 {
        return;
      }
      if let Some(Object {
        role: Role::Surface(s),
        ..
      }) = client.objects.get(&sid)
      {
        if let Some(buf) = &s.current_buffer {
          if s.mapped {
            out.push((ox, oy, buf.clone()));
          }
        }
      }
      let children: Vec<(u32, i32, i32)> = client
        .objects
        .values()
        .filter_map(|o| match &o.role {
          Role::Subsurface {
            surface_id: child,
            parent_id,
            x,
            y,
            ..
          } if *parent_id == sid => Some((*child, *x, *y)),
          _ => None,
        })
        .collect();
      for (child, sx, sy) in children {
        walk(client, child, ox + sx, oy + sy, depth + 1, out);
      }
    }
    walk(client, surface_id, ox, oy, depth, &mut out);
    out
  }

  fn blit_collected(fb: &mut crate::render::Framebuffer, blits: &[(i32, i32, crate::shm::ShmBuffer)]) {
    for (x, y, buf) in blits {
      fb.blit_shm(buf, *x, *y);
    }
  }

  fn export_shell_state(&mut self, force: bool) {
    if let Some(ref mut ipc) = self.shell_ipc {
      ipc.export_state(
        &self.clients,
        self.focused_client_fd,
        self.focused_surface_id,
        force,
        &mut self.pending_shell_menu,
        &self.switcher,
      );
    }
    self.shell_state_dirty = false;
  }

  fn handle_shell_commands(&mut self) {
    let cmds = match self.shell_ipc.as_mut() {
      Some(ipc) => ipc.accept_commands(),
      None => return,
    };
    for cmd in cmds {
      if cmd.starts_with("tray_") {
        if let Some(ref mut ipc) = self.shell_ipc {
          ipc.handle_tray_command(&cmd);
        }
        self.shell_state_dirty = true;
        continue;
      }
      let mut parts = cmd.split_whitespace();
      match (parts.next(), parts.next()) {
        (Some("activate"), Some(id)) => {
          if let Ok(sid) = id.parse::<u32>() {
            self.activate_surface(sid);
          }
        }
        (Some("minimize"), Some(id)) => {
          if let Ok(sid) = id.parse::<u32>() {
            self.minimize_surface(sid);
          }
        }
        (Some("maximize"), Some(id)) => {
          if let Ok(sid) = id.parse::<u32>() {
            self.maximize_surface(sid, true);
          }
        }
        (Some("unmaximize"), Some(id)) => {
          if let Ok(sid) = id.parse::<u32>() {
            self.maximize_surface(sid, false);
          }
        }
        (Some("toggle_maximize"), Some(id)) => {
          if let Ok(sid) = id.parse::<u32>() {
            self.toggle_maximize_surface(sid);
          }
        }
        (Some("fullscreen"), Some(id)) => {
          if let Ok(sid) = id.parse::<u32>() {
            self.fullscreen_surface(sid, true);
          }
        }
        (Some("unfullscreen"), Some(id)) => {
          if let Ok(sid) = id.parse::<u32>() {
            self.fullscreen_surface(sid, false);
          }
        }
        (Some("tile_left"), Some(id)) => {
          if let Ok(sid) = id.parse::<u32>() {
            self.tile_surface(sid, 1);
          }
        }
        (Some("tile_right"), Some(id)) => {
          if let Ok(sid) = id.parse::<u32>() {
            self.tile_surface(sid, 2);
          }
        }
        (Some("center"), Some(id)) => {
          if let Ok(sid) = id.parse::<u32>() {
            self.center_surface(sid);
          }
        }
        (Some("move"), Some(id)) => {
          if let (Ok(sid), Some(xs), Some(ys)) = (id.parse::<u32>(), parts.next(), parts.next()) {
            if let (Ok(x), Ok(y)) = (xs.parse::<i32>(), ys.parse::<i32>()) {
              self.move_surface(sid, x, y);
            }
          }
        }
        (Some("close"), Some(id)) => {
          if let Ok(sid) = id.parse::<u32>() {
            self.close_surface(sid);
          }
        }
        (Some("keymap"), layout) => {
          let layout = layout.map(|s| s.to_string());
          let variant = parts.next().map(|s| s.to_string());
          let options = parts.next().map(|s| s.to_string());
          self.reload_keymap(layout.as_deref(), variant.as_deref(), options.as_deref());
        }
        _ => {}
      }
    }
  }

  /// Look up the client that owns an xdg_toplevel wl_surface.
  ///
  /// Wayland object ids are **per-client**.  A naive "first client that has
  /// this id" walk collides with luna-shell (many layer-shell surfaces at low
  /// ids) and makes click-to-focus / taskbar activate / Alt+F4 pick the wrong
  /// client — or decide the surface is not a toplevel at all.
  fn client_for_xdg_surface(&self, surface_id: u32) -> Option<(RawFd, &Client)> {
    let is_tl = |client: &Client| Self::surface_is_xdg_toplevel_in(client, surface_id);
    if self.focused_client_fd >= 0 {
      if let Some(client) = self.clients.get(&self.focused_client_fd) {
        if is_tl(client) {
          return Some((self.focused_client_fd, client));
        }
      }
    }
    for &(fd, sid) in self.window_stack.iter().rev() {
      if sid != surface_id {
        continue;
      }
      if let Some(client) = self.clients.get(&fd) {
        if is_tl(client) {
          return Some((fd, client));
        }
      }
    }
    for (&fd, client) in &self.clients {
      if is_tl(client) {
        return Some((fd, client));
      }
    }
    None
  }

  fn surface_is_xdg_toplevel_in(client: &Client, surface_id: u32) -> bool {
    let xdg_id = match client.objects.get(&surface_id) {
      Some(Object {
        role: Role::Surface(s),
        ..
      }) => {
        if s.popup || s.input_method_popup || s.layer_surface_id.is_some() {
          return false;
        }
        s.xdg_surface_id
      }
      _ => return false,
    };
    xdg_id
      .map(|xid| {
        client.objects.values().any(|obj| {
          matches!(obj.role, Role::XdgToplevel { xdg_surface_id, .. } if xdg_surface_id == xid)
        })
      })
      .unwrap_or(false)
  }

  fn layer_keyboard_mode(&self, fd: RawFd, surface_id: u32) -> u32 {
    let Some(client) = self.clients.get(&fd) else { return 0 };
    for obj in client.objects.values() {
      if let Role::LayerSurface {
        surface_id: sid,
        keyboard,
        ..
      } = &obj.role
      {
        if *sid == surface_id {
          return *keyboard;
        }
      }
    }
    0
  }

  fn cycle_focused_window(&mut self, reverse: bool) {
    let windows: Vec<u32> = if let Some((_, ids)) = &self.switcher {
      ids.clone()
    } else {
      // Prefer stacking order (MRU-ish: top of stack last = most recent).
      let stack_ids: Vec<u32> = self
        .window_stack
        .iter()
        .rev()
        .filter_map(|&(fd, sid)| {
          let client = self.clients.get(&fd)?;
          if Self::surface_is_xdg_toplevel_in(client, sid) && !self.surface_is_minimized(client, sid)
          {
            let (title, app_id, _, _, _) = {
              let xdg = match client.objects.get(&sid)? {
                Object {
                  role: Role::Surface(s),
                  ..
                } => s.xdg_surface_id?,
                _ => return None,
              };
              crate::shell_ipc::toplevel_meta(client, xdg)
            };
            if crate::shell_ipc::is_shell_surface(&title, &app_id) {
              return None;
            }
            Some(sid)
          } else {
            None
          }
        })
        .collect();
      if stack_ids.is_empty() {
        ShellIpc::collect_windows(
          &self.clients,
          self.focused_client_fd,
          self.focused_surface_id,
        )
        .into_iter()
        .filter(|w| !w.minimized)
        .map(|w| w.surface_id)
        .collect()
      } else {
        stack_ids
      }
    };
    if windows.is_empty() {
      return;
    }
    let cur_idx = self
      .switcher
      .as_ref()
      .map(|(i, _)| *i)
      .or_else(|| windows.iter().position(|&id| id == self.focused_surface_id))
      .unwrap_or(0);
    let next_idx = if reverse {
      if cur_idx == 0 {
        windows.len() - 1
      } else {
        cur_idx - 1
      }
    } else if self.switcher.is_none() {
      // First Alt+Tab: highlight next (not current).
      (cur_idx + 1) % windows.len()
    } else {
      (cur_idx + 1) % windows.len()
    };
    self.switcher = Some((next_idx, windows));
    self.shell_state_dirty = true;
  }

  fn commit_switcher(&mut self) {
    if let Some((idx, ids)) = self.switcher.take() {
      if let Some(&sid) = ids.get(idx) {
        self.activate_surface(sid);
      }
      self.shell_state_dirty = true;
    }
  }

  /// Super+D: minimize every visible toplevel, or restore the set we hid.
  fn toggle_show_desktop(&mut self) {
    if !self.show_desktop_ids.is_empty() {
      let ids = std::mem::take(&mut self.show_desktop_ids);
      for sid in ids {
        self.activate_surface(sid);
      }
      self.shell_state_dirty = true;
      self.dirty = true;
      return;
    }
    let ids: Vec<u32> = self
      .window_stack
      .iter()
      .filter_map(|&(fd, sid)| {
        let client = self.clients.get(&fd)?;
        if !Self::surface_is_xdg_toplevel_in(client, sid) {
          return None;
        }
        if self.surface_is_minimized(client, sid) {
          return None;
        }
        let (title, app_id, _, _, _) = {
          let xdg = match client.objects.get(&sid)? {
            Object {
              role: Role::Surface(s),
              ..
            } => s.xdg_surface_id?,
            _ => return None,
          };
          crate::shell_ipc::toplevel_meta(client, xdg)
        };
        if crate::shell_ipc::is_shell_surface(&title, &app_id) {
          return None;
        }
        Some(sid)
      })
      .collect();
    if ids.is_empty() {
      return;
    }
    for &sid in &ids {
      self.minimize_surface(sid);
    }
    self.show_desktop_ids = ids;
    self.shell_state_dirty = true;
    self.dirty = true;
  }

  fn activate_surface(&mut self, surface_id: u32) {
    let Some((fd, _)) = self.client_for_xdg_surface(surface_id) else { return };
    let xdg_id = match self.clients.get(&fd).and_then(|c| c.objects.get(&surface_id)) {
      Some(Object {
        role: Role::Surface(s),
        ..
      }) => s.xdg_surface_id,
      _ => None,
    };
    let mut was_minimized = false;
    if let Some(xdg_id) = xdg_id {
      for obj in self.clients.get_mut(&fd).unwrap().objects.values_mut() {
        if let Role::XdgToplevel {
          xdg_surface_id,
          minimized,
          ..
        } = &mut obj.role
        {
          if *xdg_surface_id == xdg_id {
            was_minimized = *minimized;
            *minimized = false;
          }
        }
      }
    }
    let prev_fd = self.focused_client_fd;
    let prev_sid = self.focused_surface_id;
    let focus_changed = prev_fd != fd || prev_sid != surface_id;
    // Raising an already-top window re-stacks transient children and can
    // flash the frame; only raise when focus actually moves (or restore).
    if focus_changed || was_minimized {
      self.raise_surface(fd, surface_id);
    }
    self.focused_client_fd = fd;
    self.focused_surface_id = surface_id;
    if focus_changed || was_minimized {
      self.dirty = true;
      self.shell_state_dirty = true;
    }
    // Do NOT send xdg_toplevel.configure for ACTIVATED-only changes.
    // GTK/Firefox treat every configure as full damage → visible flicker on
    // Alt-Tab / click-to-focus.  Keyboard enter is enough for input focus;
    // decoration chrome updates on the next real resize/maximize configure.
    if was_minimized {
      self.send_toplevel_configure_for(fd, surface_id, None, false);
    }
    self.ensure_kbd_entered(fd, surface_id);
  }

  fn raise_surface(&mut self, fd: RawFd, surface_id: u32) {
    self.window_stack.retain(|&(f, s)| !(f == fd && s == surface_id));
    self.window_stack.push((fd, surface_id));
    // Keep transient children above their parent.
    let children: Vec<(RawFd, u32)> = self
      .clients
      .iter()
      .flat_map(|(&cfd, client)| {
        client.objects.iter().filter_map(move |(&oid, obj)| {
          let Role::Surface(s) = &obj.role else { return None };
          if !s.mapped || s.popup {
            return None;
          }
          let xdg = s.xdg_surface_id?;
          let parent = client.objects.values().find_map(|o| match &o.role {
            Role::XdgToplevel {
              xdg_surface_id,
              parent_surface_id,
              ..
            } if *xdg_surface_id == xdg => *parent_surface_id,
            _ => None,
          })?;
          if parent == surface_id {
            Some((cfd, oid))
          } else {
            None
          }
        })
      })
      .collect();
    for (cfd, cid) in children {
      self.window_stack.retain(|&(f, s)| !(f == cfd && s == cid));
      self.window_stack.push((cfd, cid));
    }
  }

  fn stack_index(&self, fd: RawFd, surface_id: u32) -> i32 {
    self.window_stack
      .iter()
      .position(|&(f, s)| f == fd && s == surface_id)
      .map(|i| i as i32)
      .unwrap_or(0)
  }

  fn track_mapped_toplevel(&mut self, fd: RawFd, surface_id: u32, mapped: bool) {
    if mapped {
      if !self.window_stack.iter().any(|&(f, s)| f == fd && s == surface_id) {
        self.window_stack.push((fd, surface_id));
      }
    } else {
      self.window_stack.retain(|&(f, s)| !(f == fd && s == surface_id));
      if matches!(
        &self.wm_grab,
        WmGrab::Move { fd: gfd, surface_id: gsid, .. }
          | WmGrab::Resize { fd: gfd, surface_id: gsid, .. }
          if *gfd == fd && *gsid == surface_id
      ) {
        self.wm_grab = WmGrab::None;
      }
    }
  }

  fn encode_states(states: &[u32]) -> Vec<u8> {
    let mut out = Vec::with_capacity(states.len() * 4);
    for s in states {
      out.extend_from_slice(&s.to_ne_bytes());
    }
    out
  }

  fn toplevel_states_for(&self, fd: RawFd, surface_id: u32, resizing: bool) -> Vec<u32> {
    let mut states = Vec::new();
    let meta = self.clients.get(&fd).and_then(|client| {
      let xdg_id = match client.objects.get(&surface_id) {
        Some(Object { role: Role::Surface(s), .. }) => s.xdg_surface_id?,
        _ => return None,
      };
      client.objects.values().find_map(|obj| match &obj.role {
        Role::XdgToplevel {
          xdg_surface_id,
          maximized,
          fullscreen,
          tiled,
          ..
        } if *xdg_surface_id == xdg_id => Some((*maximized, *fullscreen, *tiled)),
        _ => None,
      })
    });
    if let Some((maximized, fullscreen, tiled)) = meta {
      if maximized {
        states.push(TOPLEVEL_STATE_MAXIMIZED);
      }
      if fullscreen {
        states.push(TOPLEVEL_STATE_FULLSCREEN);
      }
      if tiled == 1 {
        states.push(TOPLEVEL_STATE_TILED_LEFT);
      } else if tiled == 2 {
        states.push(TOPLEVEL_STATE_TILED_RIGHT);
      }
    }
    if resizing {
      states.push(TOPLEVEL_STATE_RESIZING);
    }
    if self.focused_client_fd == fd && self.focused_surface_id == surface_id {
      states.push(TOPLEVEL_STATE_ACTIVATED);
    }
    states
  }

  /// Screen rectangle available for xdg_toplevels after layer-shell exclusive
  /// zones (menubar / dock).  Fullscreen ignores this; maximize uses it.
  fn usable_area(&self) -> (i32, i32, i32, i32) {
    let (bw, bh) = self.backend.size();
    let mut top = 0i32;
    let mut bottom = 0i32;
    let mut left = 0i32;
    let mut right = 0i32;
    const A_TOP: u32 = 1;
    const A_BOT: u32 = 2;
    const A_LEFT: u32 = 4;
    const A_RIGHT: u32 = 8;
    for client in self.clients.values() {
      for obj in client.objects.values() {
        let Role::LayerSurface {
          exclusive_zone,
          anchor,
          margin_top,
          margin_right,
          margin_bottom,
          margin_left,
          ..
        } = &obj.role
        else {
          continue;
        };
        if *exclusive_zone <= 0 {
          continue;
        }
        let zone = *exclusive_zone;
        let top_only = (anchor & A_TOP) != 0 && (anchor & A_BOT) == 0;
        let bot_only = (anchor & A_BOT) != 0 && (anchor & A_TOP) == 0;
        let left_only = (anchor & A_LEFT) != 0 && (anchor & A_RIGHT) == 0;
        let right_only = (anchor & A_RIGHT) != 0 && (anchor & A_LEFT) == 0;
        if top_only {
          top = top.max(zone + margin_top);
        } else if bot_only {
          bottom = bottom.max(zone + margin_bottom);
        }
        if left_only {
          left = left.max(zone + margin_left);
        } else if right_only {
          right = right.max(zone + margin_right);
        }
      }
    }
    let w = (bw as i32 - left - right).max(64);
    let h = (bh as i32 - top - bottom).max(64);
    (left, top, w, h)
  }

  /// Set toplevel on-screen top-left to (screen_x, screen_y).
  /// Toplevel `Surface.{x,y}` are absolute screen coordinates (not
  /// center-relative) so buffer size changes during resize/maximize do not
  /// jump the window — that jump was the main Firefox flicker source.
  fn set_surface_screen_pos(&mut self, fd: RawFd, surface_id: u32, screen_x: i32, screen_y: i32, _width: i32, _height: i32) {
    if let Some(Object {
      role: Role::Surface(s),
      ..
    }) = self.clients.get_mut(&fd).and_then(|c| c.objects.get_mut(&surface_id))
    {
      s.x = screen_x;
      s.y = screen_y;
    }
  }

  /// Send xdg_toplevel.configure + xdg_surface.configure for a mapped toplevel.
  /// `size_override` forces width/height (resize / maximize); otherwise keep current buffer size (0,0 = client decides).
  fn send_toplevel_configure_for(&mut self, fd: RawFd, surface_id: u32, size_override: Option<(i32, i32)>, resizing: bool) {
    let (xdg_id, cur_w, cur_h, tid, maxed, full, tiled) = {
      let Some(client) = self.clients.get(&fd) else { return };
      let (xdg_id, cur_w, cur_h) = match client.objects.get(&surface_id) {
        Some(Object { role: Role::Surface(s), .. }) => {
          let xdg = match s.xdg_surface_id {
            Some(id) => id,
            None => return,
          };
          let (w, h) = s.current_buffer.as_ref().map(|b| (b.width, b.height)).unwrap_or((0, 0));
          (xdg, w, h)
        }
        _ => return,
      };
      let tid = client.objects.iter().find_map(|(&oid, obj)| match &obj.role {
        Role::XdgToplevel { xdg_surface_id, .. } if *xdg_surface_id == xdg_id => Some(oid),
        _ => None,
      });
      let Some(tid) = tid else { return };
      let flags = client.objects.values().find_map(|obj| match &obj.role {
        Role::XdgToplevel {
          xdg_surface_id,
          maximized,
          fullscreen,
          tiled,
          ..
        } if *xdg_surface_id == xdg_id => Some((*maximized, *fullscreen, *tiled)),
        _ => None,
      });
      let (maxed, full, tiled) = flags.unwrap_or((false, false, 0));
      (xdg_id, cur_w, cur_h, tid, maxed, full, tiled)
    };
    let (bw, bh) = self.backend.size();
    let (ux, uy, uw, uh) = self.usable_area();
    let (mut width, mut height) = size_override.unwrap_or((0, 0));
    if full {
      width = bw as i32;
      height = bh as i32;
      self.set_surface_screen_pos(fd, surface_id, 0, 0, width, height);
    } else if maxed {
      let ssd_extra = if Self::uses_ssd(self.toplevel_decoration_mode(fd, surface_id)) {
        28
      } else {
        0
      };
      width = uw;
      height = (uh - ssd_extra).max(64);
      self.set_surface_screen_pos(fd, surface_id, ux, uy + ssd_extra, width, height);
    } else if tiled == 1 || tiled == 2 {
      let ssd_extra = if Self::uses_ssd(self.toplevel_decoration_mode(fd, surface_id)) {
        28
      } else {
        0
      };
      width = uw / 2;
      height = (uh - ssd_extra).max(64);
      let tx = if tiled == 1 { ux } else { ux + uw - width };
      self.set_surface_screen_pos(fd, surface_id, tx, uy + ssd_extra, width, height);
    } else if size_override.is_none() && cur_w > 0 && cur_h > 0 {
      width = 0;
      height = 0;
    }
    let states = Self::encode_states(&self.toplevel_states_for(fd, surface_id, resizing));
    let serial = self.next_serial();
    if let Some(client) = self.clients.get_mut(&fd) {
      client.send(tid, 0, &[Arg::Int(width), Arg::Int(height), Arg::Array(states)]);
      client.send(xdg_id, 0, &[Arg::Uint(serial)]);
      client.conn.flush();
    }
  }

  fn screen_ptr(&self) -> (i32, i32) {
    let (bw, bh) = self.backend.size();
    ((self.ptr_x * bw as f32) as i32, (self.ptr_y * bh as f32) as i32)
  }

  fn surface_geometry(&self, fd: RawFd, surface_id: u32) -> Option<(i32, i32, i32, i32)> {
    let client = self.clients.get(&fd)?;
    let Object { role: Role::Surface(s), .. } = client.objects.get(&surface_id)? else { return None };
    if let Some((gx, gy, gw, gh)) = s.window_geom {
      if gw > 0 && gh > 0 {
        return Some((s.x + gx, s.y + gy, gw, gh));
      }
    }
    if let Some(buf) = s.current_buffer.as_ref() {
      return Some((s.x, s.y, buf.width, buf.height));
    }
    // Buffer-less toplevel with subsurface content (Firefox).
    let (w, h) = Self::surface_tree_size(client, surface_id)?;
    Some((s.x, s.y, w, h))
  }

  /// Full client buffer rect in absolute screen coordinates (includes CSD shadows).
  fn surface_buffer_rect(&self, fd: RawFd, surface_id: u32) -> Option<(i32, i32, i32, i32)> {
    let client = self.clients.get(&fd)?;
    let Object {
      role: Role::Surface(s),
      ..
    } = client.objects.get(&surface_id)?
    else {
      return None;
    };
    let buf = s.current_buffer.as_ref()?;
    Some((s.x, s.y, buf.width, buf.height))
  }

  fn update_wm_grab(&mut self) {
    let (px, py) = self.screen_ptr();
    match self.wm_grab.clone() {
      WmGrab::None => {}
      WmGrab::Move {
        fd,
        surface_id,
        grab_px,
        grab_py,
        orig_x,
        orig_y,
      } => {
        if let Some(Object { role: Role::Surface(s), .. }) =
          self.clients.get_mut(&fd).and_then(|c| c.objects.get_mut(&surface_id))
        {
          s.x = orig_x + (px - grab_px);
          s.y = orig_y + (py - grab_py);
        }
        self.dirty = true;
      }
      WmGrab::Resize {
        fd,
        surface_id,
        edges,
        grab_px,
        grab_py,
        orig_x,
        orig_y,
        orig_w,
        orig_h,
      } => {
        let dx = px - grab_px;
        let dy = py - grab_py;
        // Ignore sub-pixel / click jitter so a bare frame click never resizes.
        if dx.abs() < 3 && dy.abs() < 3 {
          return;
        }
        let mut new_w = orig_w;
        let mut new_h = orig_h;
        let mut new_x = orig_x;
        let mut new_y = orig_y;
        if edges & RESIZE_EDGE_LEFT != 0 {
          new_w = (orig_w - dx).max(64);
          new_x = orig_x + (orig_w - new_w);
        } else if edges & RESIZE_EDGE_RIGHT != 0 {
          new_w = (orig_w + dx).max(64);
        }
        if edges & RESIZE_EDGE_TOP != 0 {
          new_h = (orig_h - dy).max(64);
          new_y = orig_y + (orig_h - new_h);
        } else if edges & RESIZE_EDGE_BOTTOM != 0 {
          new_h = (orig_h + dy).max(64);
        }
        // Honour client min/max size (xdg_toplevel.set_min_size / set_max_size).
        let (min_w, min_h, max_w, max_h) = self
          .clients
          .get(&fd)
          .and_then(|c| {
            let xdg = match c.objects.get(&surface_id)? {
              Object {
                role: Role::Surface(s),
                ..
              } => s.xdg_surface_id?,
              _ => return None,
            };
            c.objects.values().find_map(|o| match &o.role {
              Role::XdgToplevel {
                xdg_surface_id,
                min_w,
                min_h,
                max_w,
                max_h,
                ..
              } if *xdg_surface_id == xdg => Some((*min_w, *min_h, *max_w, *max_h)),
              _ => None,
            })
          })
          .unwrap_or((0, 0, 0, 0));
        let floor_w = min_w.max(64);
        let floor_h = min_h.max(64);
        if new_w < floor_w {
          if edges & RESIZE_EDGE_LEFT != 0 {
            new_x -= floor_w - new_w;
          }
          new_w = floor_w;
        }
        if new_h < floor_h {
          if edges & RESIZE_EDGE_TOP != 0 {
            new_y -= floor_h - new_h;
          }
          new_h = floor_h;
        }
        if max_w > 0 && new_w > max_w {
          if edges & RESIZE_EDGE_LEFT != 0 {
            new_x += new_w - max_w;
          }
          new_w = max_w;
        }
        if max_h > 0 && new_h > max_h {
          if edges & RESIZE_EDGE_TOP != 0 {
            new_y += new_h - max_h;
          }
          new_h = max_h;
        }
        if let Some(Object { role: Role::Surface(s), .. }) =
          self.clients.get_mut(&fd).and_then(|c| c.objects.get_mut(&surface_id))
        {
          s.x = new_x;
          s.y = new_y;
        }
        // One configure per frame via pending — flooding Firefox/GTK with
        // per-motion configures causes full-window damage flicker.
        let skip = matches!(
          self.pending_resize_configure,
          Some((pfd, psid, pw, ph)) if pfd == fd && psid == surface_id && pw == new_w && ph == new_h
        );
        if !skip {
          self.pending_resize_configure = Some((fd, surface_id, new_w, new_h));
        }
        self.dirty = true;
      }
    }
  }

  fn end_wm_grab(&mut self) {
    let grab = std::mem::replace(&mut self.wm_grab, WmGrab::None);
    match grab {
      WmGrab::None => {}
      WmGrab::Move { fd, surface_id, .. } => {
        let (px, py) = self.screen_ptr();
        let (ux, uy, uw, uh) = self.usable_area();
        let edge = 24i32;
        let near_left = px <= ux + edge;
        let near_right = px >= ux + uw - edge;
        let near_top = py <= uy + edge;
        if near_top && !near_left && !near_right {
          self.maximize_surface(surface_id, true);
        } else if near_left {
          self.tile_surface(surface_id, 1);
        } else if near_right {
          self.tile_surface(surface_id, 2);
        } else if let Some((ox, oy, w, h)) = self.surface_geometry(fd, surface_id) {
          // Was this window tiled/maximized?  Dragging away restores floating
          // size once — plain moves must NOT send configure (GTK/Firefox
          // full-damage every configure → flicker).
          let (was_max, was_tiled) = self.clients.get(&fd).and_then(|c| {
            let xdg = match c.objects.get(&surface_id)? {
              Object { role: Role::Surface(s), .. } => s.xdg_surface_id?,
              _ => return None,
            };
            c.objects.values().find_map(|o| match &o.role {
              Role::XdgToplevel {
                xdg_surface_id,
                maximized,
                tiled,
                ..
              } if *xdg_surface_id == xdg => Some((*maximized, *tiled)),
              _ => None,
            })
          }).unwrap_or((false, 0));
          let min_visible = 48i32;
          let mut nx = ox;
          let mut ny = oy;
          if nx + w < ux + min_visible {
            nx = ux + min_visible - w;
          }
          if nx > ux + uw - min_visible {
            nx = ux + uw - min_visible;
          }
          if ny < uy {
            ny = uy;
          }
          if ny > uy + uh - min_visible {
            ny = uy + uh - min_visible;
          }
          if nx != ox || ny != oy {
            self.set_surface_screen_pos(fd, surface_id, nx, ny, w, h);
          }
          if was_max || was_tiled != 0 {
            self.clear_tile_flags(fd, surface_id);
            let size = self.restore_saved_geom(fd, surface_id);
            self.send_toplevel_configure_for(fd, surface_id, size, false);
          }
          self.dirty = true;
        } else {
          self.dirty = true;
        }
      }
      WmGrab::Resize {
        fd,
        surface_id,
        orig_w,
        orig_h,
        ..
      } => {
        // Drop a no-op click on the outer frame without configure — GTK/Firefox
        // full-damage every configure and that looked like random flashing.
        let moved = matches!(
          self.pending_resize_configure,
          Some((pfd, psid, ..)) if pfd == fd && psid == surface_id
        );
        let size = self
          .surface_geometry(fd, surface_id)
          .map(|(_, _, w, h)| (w, h))
          .unwrap_or((orig_w, orig_h));
        if moved || size != (orig_w, orig_h) {
          // Final configure without RESIZING bit.
          self.send_toplevel_configure_for(fd, surface_id, Some(size), false);
        }
        self.pending_resize_configure = None;
        self.dirty = true;
      }
    }
  }

  /// Cancel an interactive resize and restore the geometry that was active
  /// when the grab started.  Keyboard events normally belong to the focused
  /// client during a WM grab, so this must be handled by the compositor rather
  /// than expecting luna-shell (or the client) to see Escape.
  fn cancel_wm_grab(&mut self) {
    let grab = std::mem::replace(&mut self.wm_grab, WmGrab::None);
    match grab {
      WmGrab::Resize {
        fd,
        surface_id,
        orig_x,
        orig_y,
        orig_w,
        orig_h,
        ..
      } => {
        if let Some(Object { role: Role::Surface(s), .. }) =
          self.clients.get_mut(&fd).and_then(|c| c.objects.get_mut(&surface_id))
        {
          s.x = orig_x;
          s.y = orig_y;
        }
        self.pending_resize_configure = None;
        self.send_toplevel_configure_for(fd, surface_id, Some((orig_w, orig_h)), false);
        self.dirty = true;
      }
      // Escape only cancels resizing.  A move has no provisional client size
      // to roll back, so leave it at its current position.
      grab => {
        self.wm_grab = grab;
      }
    }
  }

  fn save_geom_if_needed(&mut self, fd: RawFd, surface_id: u32) {
    let geom = self.surface_geometry(fd, surface_id);
    let xdg_id = match self.clients.get(&fd).and_then(|c| c.objects.get(&surface_id)) {
      Some(Object {
        role: Role::Surface(s),
        ..
      }) => s.xdg_surface_id,
      _ => None,
    };
    let Some(xdg_id) = xdg_id else { return };
    let Some(geom) = geom else { return };
    if let Some(client) = self.clients.get_mut(&fd) {
      for obj in client.objects.values_mut() {
        if let Role::XdgToplevel {
          xdg_surface_id,
          maximized,
          fullscreen,
          tiled,
          saved_geom,
          ..
        } = &mut obj.role
        {
          if *xdg_surface_id == xdg_id && !*maximized && !*fullscreen && *tiled == 0 && saved_geom.is_none() {
            *saved_geom = Some(geom);
          }
        }
      }
    }
  }

  fn restore_saved_geom(&mut self, fd: RawFd, surface_id: u32) -> Option<(i32, i32)> {
    let xdg_id = match self.clients.get(&fd).and_then(|c| c.objects.get(&surface_id)) {
      Some(Object {
        role: Role::Surface(s),
        ..
      }) => s.xdg_surface_id?,
      _ => return None,
    };
    let saved = self.clients.get_mut(&fd).and_then(|client| {
      client.objects.values_mut().find_map(|obj| match &mut obj.role {
        Role::XdgToplevel {
          xdg_surface_id,
          saved_geom,
          ..
        } if *xdg_surface_id == xdg_id => {
          let g = saved_geom.take();
          g
        }
        _ => None,
      })
    })?;
    let (x, y, w, h) = saved;
    self.set_surface_screen_pos(fd, surface_id, x, y, w, h);
    Some((w, h))
  }

  fn clear_tile_flags(&mut self, fd: RawFd, surface_id: u32) {
    let xdg_id = match self.clients.get(&fd).and_then(|c| c.objects.get(&surface_id)) {
      Some(Object {
        role: Role::Surface(s),
        ..
      }) => s.xdg_surface_id,
      _ => None,
    };
    let Some(xdg_id) = xdg_id else { return };
    if let Some(client) = self.clients.get_mut(&fd) {
      for obj in client.objects.values_mut() {
        if let Role::XdgToplevel {
          xdg_surface_id,
          maximized,
          fullscreen,
          tiled,
          ..
        } = &mut obj.role
        {
          if *xdg_surface_id == xdg_id {
            *maximized = false;
            *fullscreen = false;
            *tiled = 0;
          }
        }
      }
    }
  }

  fn tile_surface(&mut self, surface_id: u32, side: u32) {
    let Some((fd, _)) = self.client_for_xdg_surface(surface_id) else { return };
    self.save_geom_if_needed(fd, surface_id);
    let xdg_id = match self.clients.get(&fd).and_then(|c| c.objects.get(&surface_id)) {
      Some(Object {
        role: Role::Surface(s),
        ..
      }) => s.xdg_surface_id,
      _ => None,
    };
    if let Some(xdg_id) = xdg_id {
      for obj in self.clients.get_mut(&fd).unwrap().objects.values_mut() {
        if let Role::XdgToplevel {
          xdg_surface_id,
          maximized,
          fullscreen,
          minimized,
          tiled,
          ..
        } = &mut obj.role
        {
          if *xdg_surface_id == xdg_id {
            *maximized = false;
            *fullscreen = false;
            *minimized = false;
            *tiled = side;
          }
        }
      }
    }
    self.raise_surface(fd, surface_id);
    self.focused_client_fd = fd;
    self.focused_surface_id = surface_id;
    self.send_toplevel_configure_for(fd, surface_id, None, false);
    self.dirty = true;
    self.shell_state_dirty = true;
  }

  /// Place a floating toplevel at the usable-area center (clears tile/max).
  fn center_surface(&mut self, surface_id: u32) {
    let Some((fd, _)) = self.client_for_xdg_surface(surface_id) else { return };
    self.clear_tile_flags(fd, surface_id);
    let xdg_id = match self.clients.get(&fd).and_then(|c| c.objects.get(&surface_id)) {
      Some(Object {
        role: Role::Surface(s),
        ..
      }) => s.xdg_surface_id,
      _ => None,
    };
    if let Some(xdg_id) = xdg_id {
      if let Some(client) = self.clients.get_mut(&fd) {
        for obj in client.objects.values_mut() {
          if let Role::XdgToplevel {
            xdg_surface_id,
            maximized,
            fullscreen,
            minimized,
            tiled,
            ..
          } = &mut obj.role
          {
            if *xdg_surface_id == xdg_id {
              *maximized = false;
              *fullscreen = false;
              *minimized = false;
              *tiled = 0;
            }
          }
        }
      }
    }
    let (ux, uy, uw, uh) = self.usable_area();
    let (cw, ch) = self
      .surface_geometry(fd, surface_id)
      .map(|(_, _, w, h)| (w.max(64), h.max(64)))
      .unwrap_or(((uw / 2).max(320), (uh / 2).max(240)));
    let nx = ux + (uw - cw).max(0) / 2;
    let ny = uy + (uh - ch).max(0) / 2;
    self.set_surface_screen_pos(fd, surface_id, nx, ny, cw, ch);
    self.raise_surface(fd, surface_id);
    self.focused_client_fd = fd;
    self.focused_surface_id = surface_id;
    self.send_toplevel_configure_for(fd, surface_id, Some((cw, ch)), false);
    self.dirty = true;
    self.shell_state_dirty = true;
  }

  /// Move a floating toplevel to an absolute usable-area position.
  fn move_surface(&mut self, surface_id: u32, x: i32, y: i32) {
    let Some((fd, _)) = self.client_for_xdg_surface(surface_id) else { return };
    self.clear_tile_flags(fd, surface_id);
    let _ = self.restore_saved_geom(fd, surface_id);

    let xdg_id = match self.clients.get(&fd).and_then(|c| c.objects.get(&surface_id)) {
      Some(Object {
        role: Role::Surface(s),
        ..
      }) => s.xdg_surface_id,
      _ => None,
    };
    if let Some(xdg_id) = xdg_id {
      if let Some(client) = self.clients.get_mut(&fd) {
        for obj in client.objects.values_mut() {
          if let Role::XdgToplevel {
            xdg_surface_id,
            maximized,
            fullscreen,
            minimized,
            tiled,
            ..
          } = &mut obj.role
          {
            if *xdg_surface_id == xdg_id {
              *maximized = false;
              *fullscreen = false;
              *minimized = false;
              *tiled = 0;
            }
          }
        }
      }
    }

    let (ux, uy, uw, uh) = self.usable_area();
    let (cw, ch) = self
      .surface_geometry(fd, surface_id)
      .map(|(_, _, w, h)| (w.max(64), h.max(64)))
      .unwrap_or(((uw / 2).max(320), (uh / 2).max(240)));
    let max_x = ux + (uw - cw).max(0);
    let max_y = uy + (uh - ch).max(0);
    let nx = x.clamp(ux, max_x);
    let ny = y.clamp(uy, max_y);

    self.set_surface_screen_pos(fd, surface_id, nx, ny, cw, ch);
    self.raise_surface(fd, surface_id);
    self.focused_client_fd = fd;
    self.focused_surface_id = surface_id;
    self.send_toplevel_configure_for(fd, surface_id, Some((cw, ch)), false);
    self.dirty = true;
    self.shell_state_dirty = true;
  }

  fn minimize_surface(&mut self, surface_id: u32) {
    let Some((fd, _)) = self.client_for_xdg_surface(surface_id) else { return };
    let xdg_id = match self.clients.get(&fd).unwrap().objects.get(&surface_id) {
      Some(Object {
        role: Role::Surface(s),
        ..
      }) => s.xdg_surface_id,
      _ => None,
    };
    if let Some(xdg_id) = xdg_id {
      for obj in self.clients.get_mut(&fd).unwrap().objects.values_mut() {
        if let Role::XdgToplevel {
          xdg_surface_id,
          minimized,
          ..
        } = &mut obj.role
        {
          if *xdg_surface_id == xdg_id {
            *minimized = true;
          }
        }
      }
    }
    if self.focused_client_fd == fd && self.focused_surface_id == surface_id {
      self.focused_client_fd = -1;
      self.focused_surface_id = 0;
      self.ptr_entered = false;
      self.kbd_entered = false;
      self.clear_text_input_focus();
    }
    self.dirty = true;
    self.shell_state_dirty = true;
  }

  fn maximize_surface(&mut self, surface_id: u32, maximize: bool) {
    let Some((fd, _)) = self.client_for_xdg_surface(surface_id) else { return };
    if maximize {
      self.save_geom_if_needed(fd, surface_id);
    }
    let xdg_id = match self.clients.get(&fd).and_then(|c| c.objects.get(&surface_id)) {
      Some(Object {
        role: Role::Surface(s),
        ..
      }) => s.xdg_surface_id,
      _ => None,
    };
    if let Some(xdg_id) = xdg_id {
      for obj in self.clients.get_mut(&fd).unwrap().objects.values_mut() {
        if let Role::XdgToplevel {
          xdg_surface_id,
          maximized,
          fullscreen,
          minimized,
          tiled,
          ..
        } = &mut obj.role
        {
          if *xdg_surface_id == xdg_id {
            *maximized = maximize;
            if maximize {
              *fullscreen = false;
              *minimized = false;
              *tiled = 0;
            }
          }
        }
      }
    }
    self.raise_surface(fd, surface_id);
    self.focused_client_fd = fd;
    self.focused_surface_id = surface_id;
    let size = if maximize {
      None
    } else {
      self.restore_saved_geom(fd, surface_id)
    };
    self.send_toplevel_configure_for(fd, surface_id, size, false);
    self.dirty = true;
    self.shell_state_dirty = true;
  }

  fn fullscreen_surface(&mut self, surface_id: u32, full: bool) {
    let Some((fd, _)) = self.client_for_xdg_surface(surface_id) else { return };
    if full {
      self.save_geom_if_needed(fd, surface_id);
    }
    let xdg_id = match self.clients.get(&fd).and_then(|c| c.objects.get(&surface_id)) {
      Some(Object {
        role: Role::Surface(s),
        ..
      }) => s.xdg_surface_id,
      _ => None,
    };
    if let Some(xdg_id) = xdg_id {
      for obj in self.clients.get_mut(&fd).unwrap().objects.values_mut() {
        if let Role::XdgToplevel {
          xdg_surface_id,
          maximized,
          fullscreen,
          minimized,
          tiled,
          ..
        } = &mut obj.role
        {
          if *xdg_surface_id == xdg_id {
            *fullscreen = full;
            if full {
              *maximized = false;
              *minimized = false;
              *tiled = 0;
            }
          }
        }
      }
    }
    self.raise_surface(fd, surface_id);
    self.focused_client_fd = fd;
    self.focused_surface_id = surface_id;
    let size = if full {
      None
    } else {
      self.restore_saved_geom(fd, surface_id)
    };
    self.send_toplevel_configure_for(fd, surface_id, size, false);
    self.dirty = true;
    self.shell_state_dirty = true;
  }

  fn toggle_maximize_surface(&mut self, surface_id: u32) {
    let maxed = self
      .client_for_xdg_surface(surface_id)
      .and_then(|(fd, _)| {
        let client = self.clients.get(&fd)?;
        let xdg = match client.objects.get(&surface_id)? {
          Object {
            role: Role::Surface(s),
            ..
          } => s.xdg_surface_id?,
          _ => return None,
        };
        client.objects.values().find_map(|o| match &o.role {
          Role::XdgToplevel {
            xdg_surface_id,
            maximized,
            ..
          } if *xdg_surface_id == xdg => Some(*maximized),
          _ => None,
        })
      })
      .unwrap_or(false);
    self.maximize_surface(surface_id, !maxed);
  }

  fn close_surface(&mut self, surface_id: u32) {
    let Some((fd, _)) = self.client_for_xdg_surface(surface_id) else { return };
    let toplevel_id = {
      let client = match self.clients.get(&fd) {
        Some(c) => c,
        None => return,
      };
      let xdg_id = match client.objects.get(&surface_id) {
        Some(Object {
          role: Role::Surface(s),
          ..
        }) => s.xdg_surface_id,
        _ => None,
      };
      xdg_id.and_then(|xid| {
        client.objects.iter().find_map(|(&oid, obj)| {
          if let Role::XdgToplevel { xdg_surface_id, .. } = &obj.role {
            if *xdg_surface_id == xid {
              return Some(oid);
            }
          }
          None
        })
      })
    };
    if let Some(tid) = toplevel_id {
      if let Some(client) = self.clients.get_mut(&fd) {
        client.send(tid, 0, &[]); // xdg_toplevel.close
        client.conn.flush();
      }
    }
    self.shell_state_dirty = true;
  }

  fn req_seat(&mut self, client: &mut Client, opcode: u16, args: &[Arg]) {
    let iface = match opcode {
      0 => &protocol::WL_POINTER,
      1 => &protocol::WL_KEYBOARD,
      _ => return,
    };
    let nid = args.get(0).map(|a| a.as_object()).unwrap_or(0);
    if nid != 0 {
      let role = if opcode == 0 { Role::Pointer } else { Role::Keyboard };
      client.objects.insert(nid, Object::new(iface, iface.version, role));

      // GTK4 crashes without a wl_keyboard keymap event.
      if opcode == 1 {
        self.send_keymap(client, nid);
      }
    }
  }

  fn send_keymap(&self, client: &mut Client, keyboard_id: u32) {
    Self::send_keymap_data(client, keyboard_id, &self.keymap_bytes);
  }

  fn send_keymap_data(client: &mut Client, keyboard_id: u32, data: &[u8]) {
    let name = std::ffi::CString::new("xkb-keymap").unwrap();
    let fd = unsafe { libc::memfd_create(name.as_ptr(), libc::MFD_CLOEXEC | libc::MFD_ALLOW_SEALING) };
    if fd < 0 {
      return;
    }
    let mut off = 0usize;
    while off < data.len() {
      let n = unsafe { libc::write(fd, data[off..].as_ptr() as *const libc::c_void, data.len() - off) };
      if n <= 0 {
        unsafe { libc::close(fd) };
        return;
      }
      off += n as usize;
    }
    client.send(keyboard_id, 0, &[Arg::Uint(1), Arg::Fd(fd), Arg::Uint(data.len() as u32)]);

    let repeat_opcode = match client.objects.get(&keyboard_id) {
      Some(Object {
        role: Role::InputMethodKeyboardGrab { .. },
        ..
      }) => 3,
      _ => 5,
    };
    let rate = std::env::var("LUNA_KEY_REPEAT_RATE")
      .ok()
      .and_then(|s| s.parse::<i32>().ok())
      .filter(|r| *r >= 0 && *r <= 200)
      .unwrap_or(25);
    let delay = std::env::var("LUNA_KEY_REPEAT_DELAY")
      .ok()
      .and_then(|s| s.parse::<i32>().ok())
      .filter(|d| *d >= 0 && *d <= 5000)
      .unwrap_or(600);
    client.send(keyboard_id, repeat_opcode, &[Arg::Int(rate), Arg::Int(delay)]);
  }

  fn reload_keymap(&mut self, layout: Option<&str>, variant: Option<&str>, options: Option<&str>) {
    self.keymap_bytes = build_xkb_keymap(layout, variant, options);
    let data = self.keymap_bytes.clone();
    let targets: Vec<(RawFd, u32)> = self
      .clients
      .iter()
      .flat_map(|(&fd, c)| {
        c.objects
          .iter()
          .filter_map(move |(&id, o)| match o.role {
            Role::Keyboard | Role::InputMethodKeyboardGrab { .. } => Some((fd, id)),
            _ => None,
          })
      })
      .collect();
    for (fd, kid) in targets {
      if let Some(client) = self.clients.get_mut(&fd) {
        Self::send_keymap_data(client, kid, &data);
        client.conn.flush();
      }
    }
    eprintln!(
      "[luna-compositor] keymap reloaded ({} bytes)",
      data.len()
    );
  }

  fn req_input_device(&mut self, client: &mut Client, id: u32, opcode: u16, args: &[Arg]) {
    let iface_name = client.objects.get(&id).map(|o| o.interface.name);
    match iface_name {
      Some("wl_pointer") if opcode == 0 => {
        // set_cursor(serial, surface?, hotspot_x, hotspot_y)
        // Strict focus: only the client that currently owns pointer focus may
        // change the cursor.  A looser "!ptr_entered || fd match" guard let
        // luna-shell's animated refresh (and leave-time set_cursor(NULL)) race
        // against GTK and leave the pointer permanently blank on client windows.
        if !matches!(self.wm_grab, WmGrab::None) {
          return; // compositor owns the cursor during move/resize
        }
        if !self.ptr_entered || self.ptr_client_fd != client.conn.fd {
          return;
        }
        let _serial = args.get(0).map(|a| a.as_uint()).unwrap_or(0);
        let surf = args.get(1).map(|a| a.as_object()).unwrap_or(0);
        let hx = args.get(2).map(|a| a.as_int()).unwrap_or(0);
        let hy = args.get(3).map(|a| a.as_int()).unwrap_or(0);
        if surf == 0 {
          // Client released the cursor role — keep the pointer visible via
          // the compositor's default arrow (never go permanently blank).
          self.cursor_client_fd = -1;
          self.cursor_surface_id = 0;
          self.pending_cursor = None;
        } else {
          // Only adopt the client glyph once it has pixels; otherwise keep
          // the default arrow (fixes "cursor vanishes until I type").
          let has_pixels = matches!(
            client.objects.get(&surf),
            Some(Object { role: Role::Surface(s), .. })
              if s.current_buffer.as_ref().map(|b| b.width > 0 && b.height > 0).unwrap_or(false)
          );
          if has_pixels {
            self.cursor_client_fd = client.conn.fd;
            self.cursor_surface_id = surf;
            self.cursor_hot_x = hx;
            self.cursor_hot_y = hy;
            self.pending_cursor = None;
          } else {
            self.cursor_client_fd = -1;
            self.cursor_surface_id = 0;
            // Remember hotspot for when the buffer arrives on the next commit.
            self.cursor_hot_x = hx;
            self.cursor_hot_y = hy;
            self.pending_cursor = Some((client.conn.fd, surf));
          }
        }
        self.dirty = true;
      }
      Some("wl_pointer") if opcode == 1 => {
        client.objects.remove(&id);
      }
      Some("wl_keyboard") if opcode == 0 => {
        client.objects.remove(&id);
      }
      _ => {}
    }
  }

  fn req_ddm(&mut self, client: &mut Client, opcode: u16, args: &[Arg]) {
    match opcode {
      0 => {
        // create_data_source(new_id)
        let nid = args.get(0).map(|a| a.as_object()).unwrap_or(0);
        if nid == 0 {
          return;
        }
        client.objects.insert(
          nid,
          Object::new(
            &protocol::WL_DATA_SOURCE,
            3,
            Role::DataSource {
              mime_types: Vec::new(),
            },
          ),
        );
      }
      1 => {
        // get_data_device(new_id, seat)
        let nid = args.get(0).map(|a| a.as_object()).unwrap_or(0);
        let seat_id = args.get(1).map(|a| a.as_object()).unwrap_or(0);
        if nid == 0 {
          return;
        }
        client.objects.insert(
          nid,
          Object::new(
            &protocol::WL_DATA_DEVICE,
            3,
            Role::DataDevice { seat_id },
          ),
        );
        self.emit_selection_to_device(client, nid);
      }
      _ => {}
    }
  }

  fn req_data_source(&mut self, client: &mut Client, id: u32, opcode: u16, args: &[Arg]) {
    match opcode {
      0 => {
        let mime = args.get(0).and_then(|a| a.as_str()).unwrap_or("").to_string();
        if mime.is_empty() {
          return;
        }
        if let Some(Object {
          role: Role::DataSource { mime_types },
          ..
        }) = client.objects.get_mut(&id)
        {
          if !mime_types.iter().any(|m| m == &mime) {
            mime_types.push(mime);
          }
        }
        // Mime offers can arrive after set_selection; rebroadcast once ready.
        if self.selection == Some((client.conn.fd, id)) {
          self.pending_selection_broadcast = true;
        }
      }
      1 => {
        let owned = self.selection == Some((client.conn.fd, id));
        client.objects.remove(&id);
        if owned {
          self.selection = None;
          let devices: Vec<u32> = client
            .objects
            .iter()
            .filter_map(|(&did, o)| matches!(o.role, Role::DataDevice { .. }).then_some(did))
            .collect();
          for did in devices {
            client.send(did, 5, &[Arg::Object(0)]);
          }
          self.pending_selection_broadcast = true;
        }
      }
      _ => {}
    }
  }

  fn req_data_device(&mut self, client: &mut Client, id: u32, opcode: u16, args: &[Arg]) {
    match opcode {
      0 => { /* start_drag — not yet */ }
      1 => {
        // set_selection(source?, serial)
        let source = args.get(0).map(|a| a.as_object()).unwrap_or(0);
        let fd = client.conn.fd;
        if source == 0 {
          // Only the current selection owner may clear the seat clipboard.
          // A non-owner null set_selection must NOT wipe other clients' view
          // of the offer (that made Ctrl+V fail after focus changes).
          let clearing = self.selection.map(|(sfd, _)| sfd == fd).unwrap_or(false);
          if clearing {
            self.selection = None;
            self.pending_selection_broadcast = true;
          }
          return;
        }
        if !matches!(
          client.objects.get(&source),
          Some(Object {
            role: Role::DataSource { .. },
            ..
          })
        ) {
          return;
        }
        let prev = self.selection.replace((fd, source));
        if let Some((pfd, pid)) = prev {
          if !(pfd == fd && pid == source) {
            if pfd == fd {
              client.send(pid, 2, &[]); // cancelled
            } else if let Some(prev_client) = self.clients.get_mut(&pfd) {
              prev_client.send(pid, 2, &[]);
              prev_client.conn.flush();
            }
          }
        }
        // Do not emit here: `client` is outside `self.clients` during request
        // handling.  One broadcast after re-insert reaches every data_device
        // exactly once (a prior emit+broadcast double-offer confused GTK paste).
        self.pending_selection_broadcast = true;
      }
      2 => {
        client.objects.remove(&id);
      }
      _ => {}
    }
  }

  fn req_data_offer(&mut self, client: &mut Client, id: u32, opcode: u16, args: &[Arg]) {
    match opcode {
      1 => {
        // receive(mime_type, fd)
        let mime = args.get(0).and_then(|a| a.as_str()).unwrap_or("").to_string();
        let pipe_fd = args.get(1).map(|a| a.as_fd()).unwrap_or(-1);
        if pipe_fd < 0 {
          return;
        }
        let (source_fd, source_id) = match client.objects.get(&id) {
          Some(Object {
            role: Role::DataOffer {
              source_fd,
              source_id,
              ..
            },
            ..
          }) if *source_id != 0 => (*source_fd, *source_id),
          _ => {
            unsafe { libc::close(pipe_fd) };
            return;
          }
        };
        if source_fd == client.conn.fd {
          client.send(source_id, 1, &[Arg::Str(Some(mime)), Arg::Fd(pipe_fd)]);
        } else if let Some(src) = self.clients.get_mut(&source_fd) {
          src.send(source_id, 1, &[Arg::Str(Some(mime)), Arg::Fd(pipe_fd)]);
          src.conn.flush();
        } else {
          unsafe { libc::close(pipe_fd) };
        }
      }
      2 => {
        client.objects.remove(&id);
      }
      _ => {}
    }
  }

  /// Resolve current clipboard mime list. `current` is the client being
  /// handled (outside `self.clients` during request processing).
  fn selection_mimes(&self, current: Option<&Client>) -> Option<(RawFd, u32, Vec<String>)> {
    let (source_fd, source_id) = self.selection?;
    let mimes = if let Some(c) = current.filter(|c| c.conn.fd == source_fd) {
      match c.objects.get(&source_id) {
        Some(Object {
          role: Role::DataSource { mime_types },
          ..
        }) => mime_types.clone(),
        _ => return None,
      }
    } else {
      match self.clients.get(&source_fd).and_then(|c| c.objects.get(&source_id)) {
        Some(Object {
          role: Role::DataSource { mime_types },
          ..
        }) => mime_types.clone(),
        _ => return None,
      }
    };
    if mimes.is_empty() {
      None
    } else {
      Some((source_fd, source_id, mimes))
    }
  }

  fn emit_selection_to_device(&mut self, client: &mut Client, device_id: u32) {
    match self.selection_mimes(Some(client)) {
      Some((source_fd, source_id, mimes)) => {
        let offer_id = client.alloc_server_id();
        client.objects.insert(
          offer_id,
          Object::new(
            &protocol::WL_DATA_OFFER,
            3,
            Role::DataOffer {
              source_fd,
              source_id,
              mime_types: mimes.clone(),
            },
          ),
        );
        client.send(device_id, 0, &[Arg::NewId(offer_id)]); // data_offer
        for mime in mimes {
          client.send(offer_id, 0, &[Arg::Str(Some(mime))]);
        }
        client.send(device_id, 5, &[Arg::Object(offer_id)]); // selection
      }
      None => {
        client.send(device_id, 5, &[Arg::Object(0)]);
      }
    }
  }

  fn flush_selection_broadcast(&mut self) {
    if !self.pending_selection_broadcast {
      return;
    }
    self.pending_selection_broadcast = false;
    let targets: Vec<RawFd> = self.clients.keys().copied().collect();
    for fd in targets {
      self.emit_selection_to_client_fd(fd);
    }
  }

  /// Push the current seat selection to every wl_data_device on `fd`.
  /// Called on set_selection broadcast and again on keyboard enter — GTK
  /// often drops a stale offer across focus changes, so re-offer on enter
  /// is what makes Ctrl+V reliable after switching windows from a VT session.
  fn emit_selection_to_client_fd(&mut self, fd: RawFd) {
    // Never re-offer CLIPBOARD to the owning client — toolkit local cache
    // handles same-process paste; the echo made luna-clipboard drop GTK offers.
    if self.selection.map(|(sfd, _)| sfd == fd).unwrap_or(false) {
      return;
    }
    let has_selection = self.selection.is_some();
    let snapshot = self.selection.and_then(|(sfd, sid)| {
      self.clients.get(&sfd).and_then(|c| match c.objects.get(&sid) {
        Some(Object {
          role: Role::DataSource { mime_types },
          ..
        }) if !mime_types.is_empty() => Some((sfd, sid, mime_types.clone())),
        _ => None,
      })
    });
    // Source set but mimes not offered yet — wait for offer() rebroadcast.
    if has_selection && snapshot.is_none() {
      return;
    }
    let devices: Vec<u32> = match self.clients.get(&fd) {
      Some(c) => c
        .objects
        .iter()
        .filter_map(|(&id, o)| matches!(o.role, Role::DataDevice { .. }).then_some(id))
        .collect(),
      None => return,
    };
    if devices.is_empty() {
      return;
    }
    let Some(client) = self.clients.get_mut(&fd) else { return };
    // Keep stale offers until the client destroys them (with delete_id).
    // Silently removing IDs without delete_id confuses libwayland / GTK.
    match &snapshot {
      Some((source_fd, source_id, mimes)) => {
        for &did in &devices {
          let offer_id = client.alloc_server_id();
          client.objects.insert(
            offer_id,
            Object::new(
              &protocol::WL_DATA_OFFER,
              3,
              Role::DataOffer {
                source_fd: *source_fd,
                source_id: *source_id,
                mime_types: mimes.clone(),
              },
            ),
          );
          client.send(did, 0, &[Arg::NewId(offer_id)]);
          for mime in mimes {
            client.send(offer_id, 0, &[Arg::Str(Some(mime.clone()))]);
          }
          client.send(did, 5, &[Arg::Object(offer_id)]);
        }
      }
      None => {
        for &did in &devices {
          client.send(did, 5, &[Arg::Object(0)]);
        }
      }
    }
    client.conn.flush();
  }

  /* ── PRIMARY selection (middle-click paste) ── */

  fn req_primary_manager(&mut self, client: &mut Client, id: u32, opcode: u16, args: &[Arg]) {
    match opcode {
      0 => {
        // create_source(new_id)
        let nid = args.get(0).map(|a| a.as_object()).unwrap_or(0);
        if nid == 0 {
          return;
        }
        client.objects.insert(
          nid,
          Object::new(
            &protocol::ZWP_PRIMARY_SELECTION_SOURCE_V1,
            1,
            Role::PrimarySelectionSource {
              mime_types: Vec::new(),
            },
          ),
        );
      }
      1 => {
        // get_device(new_id, seat)
        let nid = args.get(0).map(|a| a.as_object()).unwrap_or(0);
        let seat_id = args.get(1).map(|a| a.as_object()).unwrap_or(0);
        if nid == 0 {
          return;
        }
        client.objects.insert(
          nid,
          Object::new(
            &protocol::ZWP_PRIMARY_SELECTION_DEVICE_V1,
            1,
            Role::PrimarySelectionDevice { seat_id },
          ),
        );
        self.emit_primary_to_device(client, nid);
      }
      2 => {
        client.objects.remove(&id);
      }
      _ => {}
    }
  }

  fn req_primary_source(&mut self, client: &mut Client, id: u32, opcode: u16, args: &[Arg]) {
    match opcode {
      0 => {
        let mime = args.get(0).and_then(|a| a.as_str()).unwrap_or("").to_string();
        if mime.is_empty() {
          return;
        }
        if let Some(Object {
          role: Role::PrimarySelectionSource { mime_types },
          ..
        }) = client.objects.get_mut(&id)
        {
          if !mime_types.iter().any(|m| m == &mime) {
            mime_types.push(mime);
          }
        }
        // Mime offers can arrive after set_selection; rebroadcast once ready.
        if self.primary_selection == Some((client.conn.fd, id)) {
          self.pending_primary_broadcast = true;
        }
      }
      1 => {
        let owned = self.primary_selection == Some((client.conn.fd, id));
        client.objects.remove(&id);
        if owned {
          self.primary_selection = None;
          let devices: Vec<u32> = client
            .objects
            .iter()
            .filter_map(|(&did, o)| matches!(o.role, Role::PrimarySelectionDevice { .. }).then_some(did))
            .collect();
          for did in devices {
            client.send(did, 1, &[Arg::Object(0)]);
          }
          self.pending_primary_broadcast = true;
        }
      }
      _ => {}
    }
  }

  fn req_primary_device(&mut self, client: &mut Client, id: u32, opcode: u16, args: &[Arg]) {
    match opcode {
      0 => {
        // set_selection(source?, serial)
        let source = args.get(0).map(|a| a.as_object()).unwrap_or(0);
        let fd = client.conn.fd;
        if source == 0 {
          let clearing = self.primary_selection.map(|(sfd, _)| sfd == fd).unwrap_or(false);
          if clearing {
            self.primary_selection = None;
            self.pending_primary_broadcast = true;
          }
          return;
        }
        if !matches!(
          client.objects.get(&source),
          Some(Object {
            role: Role::PrimarySelectionSource { .. },
            ..
          })
        ) {
          return;
        }
        let prev = self.primary_selection.replace((fd, source));
        if let Some((pfd, pid)) = prev {
          if !(pfd == fd && pid == source) {
            if pfd == fd {
              client.send(pid, 1, &[]); // cancelled
            } else if let Some(prev_client) = self.clients.get_mut(&pfd) {
              prev_client.send(pid, 1, &[]);
              prev_client.conn.flush();
            }
          }
        }
        self.pending_primary_broadcast = true;
      }
      1 => {
        client.objects.remove(&id);
      }
      _ => {}
    }
  }

  fn req_primary_offer(&mut self, client: &mut Client, id: u32, opcode: u16, args: &[Arg]) {
    match opcode {
      0 => {
        // receive(mime_type, fd)
        let mime = args.get(0).and_then(|a| a.as_str()).unwrap_or("").to_string();
        let pipe_fd = args.get(1).map(|a| a.as_fd()).unwrap_or(-1);
        if pipe_fd < 0 {
          return;
        }
        let (source_fd, source_id) = match client.objects.get(&id) {
          Some(Object {
            role: Role::PrimarySelectionOffer {
              source_fd,
              source_id,
              ..
            },
            ..
          }) if *source_id != 0 => (*source_fd, *source_id),
          _ => {
            unsafe { libc::close(pipe_fd) };
            return;
          }
        };
        if source_fd == client.conn.fd {
          client.send(source_id, 0, &[Arg::Str(Some(mime)), Arg::Fd(pipe_fd)]);
        } else if let Some(src) = self.clients.get_mut(&source_fd) {
          src.send(source_id, 0, &[Arg::Str(Some(mime)), Arg::Fd(pipe_fd)]);
          src.conn.flush();
        } else {
          unsafe { libc::close(pipe_fd) };
        }
      }
      1 => {
        client.objects.remove(&id);
      }
      _ => {}
    }
  }

  fn primary_mimes(&self, current: Option<&Client>) -> Option<(RawFd, u32, Vec<String>)> {
    let (source_fd, source_id) = self.primary_selection?;
    let mimes = if let Some(c) = current.filter(|c| c.conn.fd == source_fd) {
      match c.objects.get(&source_id) {
        Some(Object {
          role: Role::PrimarySelectionSource { mime_types },
          ..
        }) => mime_types.clone(),
        _ => return None,
      }
    } else {
      match self.clients.get(&source_fd).and_then(|c| c.objects.get(&source_id)) {
        Some(Object {
          role: Role::PrimarySelectionSource { mime_types },
          ..
        }) => mime_types.clone(),
        _ => return None,
      }
    };
    if mimes.is_empty() {
      None
    } else {
      Some((source_fd, source_id, mimes))
    }
  }

  fn emit_primary_to_device(&mut self, client: &mut Client, device_id: u32) {
    match self.primary_mimes(Some(client)) {
      Some((source_fd, source_id, mimes)) => {
        let offer_id = client.alloc_server_id();
        client.objects.insert(
          offer_id,
          Object::new(
            &protocol::ZWP_PRIMARY_SELECTION_OFFER_V1,
            1,
            Role::PrimarySelectionOffer {
              source_fd,
              source_id,
              mime_types: mimes.clone(),
            },
          ),
        );
        client.send(device_id, 0, &[Arg::NewId(offer_id)]); // data_offer
        for mime in mimes {
          client.send(offer_id, 0, &[Arg::Str(Some(mime))]);
        }
        client.send(device_id, 1, &[Arg::Object(offer_id)]); // selection
      }
      None => {
        client.send(device_id, 1, &[Arg::Object(0)]);
      }
    }
  }

  fn flush_primary_broadcast(&mut self) {
    if !self.pending_primary_broadcast {
      return;
    }
    self.pending_primary_broadcast = false;
    let targets: Vec<RawFd> = self.clients.keys().copied().collect();
    for fd in targets {
      self.emit_primary_to_client_fd(fd);
    }
  }

  fn emit_primary_to_client_fd(&mut self, fd: RawFd) {
    // Same-process paste uses the toolkit cache; echoing PRIMARY to the owner
    // races with set_selection and can drop a just-selected offer.
    if self.primary_selection.map(|(sfd, _)| sfd == fd).unwrap_or(false) {
      return;
    }
    let has_selection = self.primary_selection.is_some();
    let snapshot = self.primary_selection.and_then(|(sfd, sid)| {
      self.clients.get(&sfd).and_then(|c| match c.objects.get(&sid) {
        Some(Object {
          role: Role::PrimarySelectionSource { mime_types },
          ..
        }) if !mime_types.is_empty() => Some((sfd, sid, mime_types.clone())),
        _ => None,
      })
    });
    if has_selection && snapshot.is_none() {
      return;
    }
    let devices: Vec<u32> = match self.clients.get(&fd) {
      Some(c) => c
        .objects
        .iter()
        .filter_map(|(&id, o)| matches!(o.role, Role::PrimarySelectionDevice { .. }).then_some(id))
        .collect(),
      None => return,
    };
    if devices.is_empty() {
      return;
    }
    let Some(client) = self.clients.get_mut(&fd) else { return };
    // Keep stale offers until the client destroys them (with delete_id).
    match &snapshot {
      Some((source_fd, source_id, mimes)) => {
        for &did in &devices {
          let offer_id = client.alloc_server_id();
          client.objects.insert(
            offer_id,
            Object::new(
              &protocol::ZWP_PRIMARY_SELECTION_OFFER_V1,
              1,
              Role::PrimarySelectionOffer {
                source_fd: *source_fd,
                source_id: *source_id,
                mime_types: mimes.clone(),
              },
            ),
          );
          client.send(did, 0, &[Arg::NewId(offer_id)]);
          for mime in mimes {
            client.send(offer_id, 0, &[Arg::Str(Some(mime.clone()))]);
          }
          client.send(did, 1, &[Arg::Object(offer_id)]);
        }
      }
      None => {
        for &did in &devices {
          client.send(did, 1, &[Arg::Object(0)]);
        }
      }
    }
    client.conn.flush();
  }

  /// SSD only when the client explicitly negotiated server mode (2).
  /// Mode 0 (unset / never negotiated) and mode 1 (CSD) draw no compositor
  /// chrome — GTK always paints its own HeaderBar/shadows; forcing SSD on
  /// top produced double titlebars plus a 3px frame around CSD margins.
  #[inline]
  fn uses_ssd(decoration_mode: u32) -> bool {
    decoration_mode == 2
  }

  fn toplevel_decoration_mode(&self, fd: RawFd, surface_id: u32) -> u32 {
    let Some(client) = self.clients.get(&fd) else { return 0 };
    let xdg = match client.objects.get(&surface_id) {
      Some(Object {
        role: Role::Surface(s),
        ..
      }) => match s.xdg_surface_id {
        Some(id) => id,
        None => return 0,
      },
      _ => return 0,
    };
    client
      .objects
      .values()
      .find_map(|o| match &o.role {
        Role::XdgToplevel {
          xdg_surface_id,
          decoration_mode,
          ..
        } if *xdg_surface_id == xdg => Some(*decoration_mode),
        _ => None,
      })
      .unwrap_or(0)
  }

  fn draw_ssd_titlebar(&mut self, win_x: i32, win_y: i32, win_w: i32) {
    const BAR_H: i32 = 28;
    let bar_y = win_y - BAR_H;
    let bar_color: u32 = 0xff2a_2a32;
    let bar_hi: u32 = 0xff3a_3a46;
    let close_c: u32 = 0xffe8_4a4a;
    let min_c: u32 = 0xffe8_c04a;
    let max_c: u32 = 0xff4a_c86a;
    let fw = self.fb.width as i32;
    let fh = self.fb.height as i32;
    for y in bar_y.max(0)..(bar_y + BAR_H).min(fh) {
      let row_c = if y == bar_y { bar_hi } else { bar_color };
      for x in win_x.max(0)..(win_x + win_w).min(fw) {
        let i = (y as u32 * self.fb.width + x as u32) as usize;
        if i < self.fb.pixels.len() {
          self.fb.pixels[i] = row_c;
        }
      }
    }
    let draw_dot = |fb: &mut Framebuffer, cx: i32, cy: i32, color: u32| {
      let fw = fb.width as i32;
      let fh = fb.height as i32;
      for dy in -5i32..=5 {
        for dx in -5i32..=5 {
          if dx * dx + dy * dy <= 25 {
            let x = cx + dx;
            let y = cy + dy;
            if x >= 0 && y >= 0 && x < fw && y < fh {
              let i = (y as u32 * fb.width + x as u32) as usize;
              fb.pixels[i] = color;
            }
          }
        }
      }
    };
    let cy = bar_y + BAR_H / 2;
    // Traffic lights: close / minimize / maximize
    draw_dot(&mut self.fb, win_x + 16, cy, close_c);
    draw_dot(&mut self.fb, win_x + 36, cy, min_c);
    draw_dot(&mut self.fb, win_x + 56, cy, max_c);
  }

  /// Visible window frame so the user can see (and grab) resize borders.
  fn draw_window_frame(&mut self, win_x: i32, win_y: i32, win_w: i32, win_h: i32, ssd: bool) {
    const BAR_H: i32 = 28;
    const FRAME: i32 = 3;
    let frame_c: u32 = 0xff5a_5a72;
    let top = if ssd { win_y - BAR_H } else { win_y };
    let left = win_x - FRAME;
    let right = win_x + win_w + FRAME;
    let bottom = win_y + win_h + FRAME;
    let fw = self.fb.width as i32;
    let fh = self.fb.height as i32;
    let put = |fb: &mut Framebuffer, x: i32, y: i32, c: u32| {
      if x < 0 || y < 0 || x >= fw || y >= fh {
        return;
      }
      let i = (y as u32 * fb.width + x as u32) as usize;
      if i < fb.pixels.len() {
        fb.pixels[i] = c;
      }
    };
    // Top / bottom edges
    for x in left.max(0)..right.min(fw) {
      for t in 0..FRAME {
        put(&mut self.fb, x, top - 1 - t, frame_c);
        put(&mut self.fb, x, bottom - FRAME + t, frame_c);
      }
    }
    // Left / right edges (include titlebar height when SSD)
    for y in top.max(0)..bottom.min(fh) {
      for t in 0..FRAME {
        put(&mut self.fb, left + t, y, frame_c);
        put(&mut self.fb, right - 1 - t, y, frame_c);
      }
    }
  }

  fn hit_ssd(&self, px: i32, py: i32) -> Option<(&'static str, RawFd, u32)> {
    const BAR_H: i32 = 28;
    for &(fd, sid) in self.window_stack.iter().rev() {
      let Some(client) = self.clients.get(&fd) else { continue };
      if self.surface_is_minimized(client, sid) {
        continue;
      }
      if !Self::uses_ssd(self.toplevel_decoration_mode(fd, sid)) {
        continue;
      }
      let Some((ox, oy, w, _h)) = self.surface_geometry(fd, sid) else { continue };
      let bar_y = oy - BAR_H;
      if px >= ox && px < ox + w && py >= bar_y && py < oy {
        let lx = px - ox;
        if (lx - 16).abs() <= 8 {
          return Some(("close", fd, sid));
        }
        if (lx - 36).abs() <= 8 {
          return Some(("min", fd, sid));
        }
        if (lx - 56).abs() <= 8 {
          return Some(("max", fd, sid));
        }
        return Some(("move", fd, sid));
      }
    }
    None
  }

  /// Hit-test a resize border *outside* floating toplevels (visible frame + rim).
  /// Must NEVER steal clicks inside the client buffer — that turned ordinary
  /// clicks into resize grabs and broke drag-to-select / copy.
  fn hit_resize_edge(&self, px: i32, py: i32) -> Option<(RawFd, u32, u32)> {
    // Match draw_window_frame FRAME (3) with a little grab slack.
    const BORDER: i32 = 5;
    for &(fd, sid) in self.window_stack.iter().rev() {
      let Some(client) = self.clients.get(&fd) else { continue };
      if self.surface_is_minimized(client, sid) {
        continue;
      }
      let xdg = match client.objects.get(&sid) {
        Some(Object {
          role: Role::Surface(s),
          ..
        }) => match s.xdg_surface_id {
          Some(id) => id,
          None => continue,
        },
        _ => continue,
      };
      let (maxed, full) = match client.objects.values().find_map(|o| match &o.role {
        Role::XdgToplevel {
          xdg_surface_id,
          maximized,
          fullscreen,
          ..
        } if *xdg_surface_id == xdg => Some((*maximized, *fullscreen)),
        _ => None,
      }) {
        Some(f) => f,
        None => continue,
      };
      if maxed || full {
        continue;
      }
      // Use the full buffer rect (not window_geometry).  window_geometry is the
      // content box *inside* CSD shadows — treating its inset rim as resize
      // stole clicks from the real client surface.
      let Some((ox, oy, w, h)) = self.surface_buffer_rect(fd, sid) else { continue };
      let ssd = Self::uses_ssd(self.toplevel_decoration_mode(fd, sid));
      // SSD titlebar sits above the buffer — treat it as part of the window.
      let top_extra = if ssd { 28 } else { 0 };
      let inner_l = ox;
      let inner_r = ox + w;
      let inner_t = oy - top_extra;
      let inner_b = oy + h;
      let outer_l = inner_l - BORDER;
      let outer_r = inner_r + BORDER;
      let outer_t = inner_t - BORDER;
      let outer_b = inner_b + BORDER;
      if px < outer_l || px >= outer_r || py < outer_t || py >= outer_b {
        continue;
      }
      // Strictly inside the window (buffer + SSD titlebar) → client / SSD hit.
      if px >= inner_l && px < inner_r && py >= inner_t && py < inner_b {
        continue;
      }
      let mut edges = 0u32;
      if px < inner_l {
        edges |= RESIZE_EDGE_LEFT;
      } else if px >= inner_r {
        edges |= RESIZE_EDGE_RIGHT;
      }
      if py < inner_t {
        edges |= RESIZE_EDGE_TOP;
      } else if py >= inner_b {
        edges |= RESIZE_EDGE_BOTTOM;
      }
      if edges != 0 {
        return Some((fd, sid, edges));
      }
    }
    None
  }

  fn req_decoration_manager(&mut self, client: &mut Client, id: u32, opcode: u16, args: &[Arg]) {
    match opcode {
      0 => {
        client.objects.remove(&id);
      }
      1 => {
        // get_toplevel_decoration(id, toplevel)
        let nid = args.get(0).map(|a| a.as_object()).unwrap_or(0);
        let toplevel = args.get(1).map(|a| a.as_object()).unwrap_or(0);
        if nid == 0 || toplevel == 0 {
          return;
        }
        client.objects.insert(
          nid,
          Object::new(
            &protocol::ZXDG_TOPLEVEL_DECORATION_V1,
            1,
            Role::ToplevelDecoration {
              toplevel_id: toplevel,
              mode: 1, // prefer client CSD
            },
          ),
        );
        // Prefer client-side decorations (GTK HeaderBar / shadows).  Firefox
        // and Qt that want SSD call set_mode(2) and still get compositor chrome.
        client.send(nid, 0, &[Arg::Uint(1)]);
        if let Some(Object {
          role: Role::XdgToplevel {
            decoration_mode, ..
          },
          ..
        }) = client.objects.get_mut(&toplevel)
        {
          *decoration_mode = 1;
        }
      }
      _ => {}
    }
  }

  fn req_toplevel_decoration(&mut self, client: &mut Client, id: u32, opcode: u16, args: &[Arg]) {
    match opcode {
      0 => {
        client.objects.remove(&id);
      }
      1 => {
        // set_mode(mode) 1=client 2=server
        let mode = args.get(0).map(|a| a.as_uint()).unwrap_or(1);
        let mode = if mode == 2 { 2 } else { 1 };
        let toplevel = match client.objects.get(&id) {
          Some(Object {
            role: Role::ToplevelDecoration { toplevel_id, .. },
            ..
          }) => *toplevel_id,
          _ => return,
        };
        if let Some(Object {
          role: Role::ToplevelDecoration { mode: m, .. },
          ..
        }) = client.objects.get_mut(&id)
        {
          *m = mode;
        }
        if let Some(Object {
          role: Role::XdgToplevel {
            decoration_mode, ..
          },
          ..
        }) = client.objects.get_mut(&toplevel)
        {
          *decoration_mode = mode;
        }
        client.send(id, 0, &[Arg::Uint(mode)]);
        self.dirty = true;
      }
      2 => {
        // unset_mode → compositor preference (CSD)
        let toplevel = match client.objects.get(&id) {
          Some(Object {
            role: Role::ToplevelDecoration { toplevel_id, .. },
            ..
          }) => *toplevel_id,
          _ => return,
        };
        if let Some(Object {
          role: Role::ToplevelDecoration { mode: m, .. },
          ..
        }) = client.objects.get_mut(&id)
        {
          *m = 1;
        }
        if let Some(Object {
          role: Role::XdgToplevel {
            decoration_mode, ..
          },
          ..
        }) = client.objects.get_mut(&toplevel)
        {
          *decoration_mode = 1;
        }
        client.send(id, 0, &[Arg::Uint(1)]);
        self.dirty = true;
      }
      _ => {}
    }
  }

  fn req_text_input_manager(&mut self, client: &mut Client, id: u32, opcode: u16, args: &[Arg]) {
    match opcode {
      0 => {
        client.objects.remove(&id);
      }
      1 => {
        let nid = args.get(0).map(|a| a.as_object()).unwrap_or(0);
        let seat_id = args.get(1).map(|a| a.as_object()).unwrap_or(0);
        if nid == 0 {
          return;
        }
        let surface_id = if self.focused_client_fd == client.conn.fd && self.focused_surface_id != 0 && client.objects.contains_key(&self.focused_surface_id) { Some(self.focused_surface_id) } else { None };
        client.objects.insert(
          nid,
          Object::new(
            &protocol::ZWP_TEXT_INPUT_V3,
            1,
            Role::TextInput {
              seat_id,
              surface_id,
              enabled: false,
              pending_enabled: None,
              surrounding_text: String::new(),
              cursor: 0,
              anchor: 0,
              text_change_cause: 0,
              content_hint: 0,
              content_purpose: 0,
              cursor_rect: (0, 0, 0, 0),
              commit_serial: 0,
            },
          ),
        );
        if let Some(surface_id) = surface_id {
          client.send(nid, 0, &[Arg::Object(surface_id)]);
        }
      }
      _ => {}
    }
  }

  fn req_text_input(&mut self, client: &mut Client, id: u32, opcode: u16, args: &[Arg]) {
    if opcode == 0 {
      if self.active_text_input.map(|v| (v.0, v.1)) == Some((client.conn.fd, id)) {
        self.active_text_input = None;
        self.deactivate_input_methods(None);
      }
      client.objects.remove(&id);
      return;
    }

    let mut commit = false;
    if let Some(Object {
      role: Role::TextInput {
        pending_enabled,
        surrounding_text,
        cursor,
        anchor,
        text_change_cause,
        content_hint,
        content_purpose,
        cursor_rect,
        ..
      },
      ..
    }) = client.objects.get_mut(&id)
    {
      match opcode {
        1 => *pending_enabled = Some(true),
        2 => *pending_enabled = Some(false),
        3 => {
          *surrounding_text = args.get(0).and_then(|a| a.as_str()).unwrap_or("").to_string();
          *cursor = args.get(1).map(|a| a.as_int()).unwrap_or(0);
          *anchor = args.get(2).map(|a| a.as_int()).unwrap_or(0);
        }
        4 => *text_change_cause = args.get(0).map(|a| a.as_uint()).unwrap_or(0),
        5 => {
          *content_hint = args.get(0).map(|a| a.as_uint()).unwrap_or(0);
          *content_purpose = args.get(1).map(|a| a.as_uint()).unwrap_or(0);
        }
        6 => *cursor_rect = (args.get(0).map(|a| a.as_int()).unwrap_or(0), args.get(1).map(|a| a.as_int()).unwrap_or(0), args.get(2).map(|a| a.as_int()).unwrap_or(0), args.get(3).map(|a| a.as_int()).unwrap_or(0)),
        7 => commit = true,
        _ => {}
      }
    }
    if !commit {
      return;
    }

    let (surface_id, enabled) = match client.objects.get_mut(&id) {
      Some(Object {
        role: Role::TextInput {
          surface_id,
          enabled,
          pending_enabled,
          commit_serial,
          ..
        },
        ..
      }) => {
        if let Some(value) = pending_enabled.take() {
          *enabled = value;
        }
        *commit_serial = commit_serial.wrapping_add(1);
        (*surface_id, *enabled)
      }
      _ => return,
    };
    let focused = self.focused_client_fd == client.conn.fd && surface_id == Some(self.focused_surface_id);
    if enabled && focused {
      let old = self.active_text_input.replace((client.conn.fd, id, surface_id.unwrap_or(0)));
      if old.map(|v| (v.0, v.1)) != Some((client.conn.fd, id)) {
        if old.is_some() {
          self.deactivate_input_methods(None);
        }
        self.activate_input_methods(client, id);
      } else {
        self.send_text_state_to_input_methods(client, id);
      }
      self.position_input_method_popups_for(client, id);
    } else if self.active_text_input.map(|v| (v.0, v.1)) == Some((client.conn.fd, id)) {
      self.active_text_input = None;
      self.deactivate_input_methods(None);
    }
  }

  fn req_input_method_manager(&mut self, client: &mut Client, id: u32, opcode: u16, args: &[Arg]) {
    match opcode {
      0 => {
        // input-method-v2 declares get_input_method(seat, new_id), unlike
        // text-input-v3's get_text_input(new_id, seat).
        let seat_id = args.get(0).map(|a| a.as_object()).unwrap_or(0);
        let nid = args.get(1).map(|a| a.as_object()).unwrap_or(0);
        if nid == 0 {
          return;
        }
        client.objects.insert(
          nid,
          Object::new(
            &protocol::ZWP_INPUT_METHOD_V2,
            1,
            Role::InputMethod {
              seat_id,
              pending_commit: None,
              pending_preedit: None,
              pending_delete: None,
            },
          ),
        );
        if let Some((target_fd, target_id, _)) = self.active_text_input {
          client.send(nid, 0, &[]);
          if let Some(target) = self.clients.get(&target_fd) {
            self.send_text_state(client, nid, target, target_id);
          }
        }
      }
      1 => {
        client.objects.remove(&id);
      }
      _ => {}
    }
  }

  fn req_input_method(&mut self, client: &mut Client, id: u32, opcode: u16, args: &[Arg]) {
    match opcode {
      0 => {
        if let Some(Object {
          role: Role::InputMethod { pending_commit, .. },
          ..
        }) = client.objects.get_mut(&id)
        {
          *pending_commit = Some(args.get(0).and_then(|a| a.as_str()).unwrap_or("").to_string());
        }
      }
      1 => {
        if let Some(Object {
          role: Role::InputMethod {
            pending_preedit, ..
          },
          ..
        }) = client.objects.get_mut(&id)
        {
          *pending_preedit = Some((args.get(0).and_then(|a| a.as_str()).unwrap_or("").to_string(), args.get(1).map(|a| a.as_int()).unwrap_or(0), args.get(2).map(|a| a.as_int()).unwrap_or(0)));
        }
      }
      2 => {
        if let Some(Object {
          role: Role::InputMethod { pending_delete, .. },
          ..
        }) = client.objects.get_mut(&id)
        {
          *pending_delete = Some((args.get(0).map(|a| a.as_uint()).unwrap_or(0), args.get(1).map(|a| a.as_uint()).unwrap_or(0)));
        }
      }
      3 => self.forward_input_method_commit(client, id),
      4 => {
        let nid = args.get(0).map(|a| a.as_object()).unwrap_or(0);
        let surface_id = args.get(1).map(|a| a.as_object()).unwrap_or(0);
        if nid == 0 || surface_id == 0 {
          return;
        }
        if let Some(Object {
          role: Role::Surface(surface),
          ..
        }) = client.objects.get_mut(&surface_id)
        {
          surface.input_method_popup = true;
          surface.popup = true;
        } else {
          client.post_error(id, 0, "input popup requires a wl_surface");
          return;
        }
        client.objects.insert(
          nid,
          Object::new(
            &protocol::ZWP_INPUT_POPUP_SURFACE_V2,
            1,
            Role::InputPopupSurface {
              surface_id,
              input_method_id: id,
            },
          ),
        );
        self.position_one_input_method_popup(client, nid);
      }
      5 => {
        let nid = args.get(0).map(|a| a.as_object()).unwrap_or(0);
        if nid != 0 {
          client.objects.insert(
            nid,
            Object::new(
              &protocol::ZWP_INPUT_METHOD_KEYBOARD_GRAB_V2,
              1,
              Role::InputMethodKeyboardGrab {
                input_method_id: id,
              },
            ),
          );
          self.send_keymap(client, nid);
        }
      }
      6 => {
        let popup_ids: Vec<u32> = client
          .objects
          .iter()
          .filter_map(|(&oid, obj)| match obj.role {
            Role::InputPopupSurface {
              input_method_id, ..
            } if input_method_id == id => Some(oid),
            _ => None,
          })
          .collect();
        for popup_id in popup_ids {
          self.req_input_popup_surface(client, popup_id, 0);
        }
        client.objects.remove(&id);
      }
      _ => {}
    }
  }

  fn req_input_popup_surface(&mut self, client: &mut Client, id: u32, opcode: u16) {
    if opcode != 0 {
      return;
    }
    let surface_id = match client.objects.get(&id) {
      Some(Object {
        role: Role::InputPopupSurface { surface_id, .. },
        ..
      }) => *surface_id,
      _ => return,
    };
    if let Some(Object {
      role: Role::Surface(surface),
      ..
    }) = client.objects.get_mut(&surface_id)
    {
      surface.input_method_popup = false;
      surface.popup = false;
      surface.mapped = false;
    }
    client.objects.remove(&id);
    self.dirty = true;
  }

  fn req_input_method_keyboard_grab(&mut self, client: &mut Client, id: u32, opcode: u16) {
    if opcode == 0 {
      client.objects.remove(&id);
    }
  }

  fn req_virtual_keyboard_manager(&mut self, client: &mut Client, id: u32, opcode: u16, args: &[Arg]) {
    match opcode {
      0 => {
        let authorized = client.objects.values().any(|obj| matches!(obj.role, Role::InputMethod { .. }));
        if !authorized {
          client.post_error(id, 0, "virtual keyboard requires an input-method object");
          return;
        }
        let seat_id = args.get(0).map(|a| a.as_object()).unwrap_or(0);
        let new_id = args.get(1).map(|a| a.as_object()).unwrap_or(0);
        if new_id == 0 {
          return;
        }
        client.objects.insert(
          new_id,
          Object::new(
            &protocol::ZWP_VIRTUAL_KEYBOARD_V1,
            1,
            Role::VirtualKeyboard {
              seat_id,
              keymap_set: false,
            },
          ),
        );
      }
      1 => {
        client.objects.remove(&id);
      }
      _ => {}
    }
  }

  fn req_virtual_keyboard(&mut self, client: &mut Client, id: u32, opcode: u16, args: &[Arg]) {
    match opcode {
      0 => {
        let fd = args.get(1).map(|a| a.as_fd()).unwrap_or(-1);
        if fd >= 0 {
          unsafe { libc::close(fd) };
        }
        if let Some(Object {
          role: Role::VirtualKeyboard { keymap_set, .. },
          ..
        }) = client.objects.get_mut(&id)
        {
          *keymap_set = true;
        }
      }
      1 => {
        if !Self::virtual_keyboard_has_keymap(client, id) {
          client.post_error(id, 0, "virtual keyboard key sent before keymap");
          return;
        }
        let time = args.get(0).map(|a| a.as_uint()).unwrap_or(0);
        let key = args.get(1).map(|a| a.as_uint()).unwrap_or(0);
        let state = args.get(2).map(|a| a.as_uint()).unwrap_or(0);
        self.forward_virtual_key(client, time, key, state);
      }
      2 => {
        if !Self::virtual_keyboard_has_keymap(client, id) {
          client.post_error(id, 0, "virtual keyboard modifiers sent before keymap");
          return;
        }
        let depressed = args.get(0).map(|a| a.as_uint()).unwrap_or(0);
        let latched = args.get(1).map(|a| a.as_uint()).unwrap_or(0);
        let locked = args.get(2).map(|a| a.as_uint()).unwrap_or(0);
        let group = args.get(3).map(|a| a.as_uint()).unwrap_or(0);
        self.forward_virtual_modifiers(client, depressed, latched, locked, group);
      }
      3 => {
        client.objects.remove(&id);
      }
      _ => {}
    }
  }

  fn virtual_keyboard_has_keymap(client: &Client, id: u32) -> bool {
    matches!(
      client.objects.get(&id),
      Some(Object {
        role: Role::VirtualKeyboard {
          keymap_set: true,
          ..
        },
        ..
      })
    )
  }

  fn keyboard_id(client: &Client) -> Option<u32> { client.objects.iter().find_map(|(&id, obj)| matches!(obj.role, Role::Keyboard).then_some(id)) }

  fn forward_virtual_key(&mut self, virtual_client: &mut Client, time: u32, key: u32, state: u32) {
    let target_fd = self.focused_client_fd;
    let surface_id = self.focused_surface_id;
    if target_fd < 0 || surface_id == 0 {
      return;
    }
    let serial = self.next_serial();
    if target_fd == virtual_client.conn.fd {
      if let Some(keyboard_id) = Self::keyboard_id(virtual_client) {
        virtual_client.send(keyboard_id, 3, &[Arg::Uint(serial), Arg::Uint(time), Arg::Uint(key), Arg::Uint(state)]);
      }
      return;
    }
    self.ensure_kbd_entered(target_fd, surface_id);
    if let Some(target) = self.clients.get_mut(&target_fd) {
      if let Some(keyboard_id) = Self::keyboard_id(target) {
        target.send(keyboard_id, 3, &[Arg::Uint(serial), Arg::Uint(time), Arg::Uint(key), Arg::Uint(state)]);
        target.conn.flush();
      }
    }
  }

  fn forward_virtual_modifiers(&mut self, virtual_client: &mut Client, depressed: u32, latched: u32, locked: u32, group: u32) {
    let target_fd = self.focused_client_fd;
    let surface_id = self.focused_surface_id;
    if target_fd < 0 || surface_id == 0 {
      return;
    }
    self.kbd_mods = depressed;
    let serial = self.next_serial();
    if target_fd == virtual_client.conn.fd {
      if let Some(keyboard_id) = Self::keyboard_id(virtual_client) {
        virtual_client.send(keyboard_id, 4, &[Arg::Uint(serial), Arg::Uint(depressed), Arg::Uint(latched), Arg::Uint(locked), Arg::Uint(group)]);
      }
      return;
    }
    self.ensure_kbd_entered(target_fd, surface_id);
    if let Some(target) = self.clients.get_mut(&target_fd) {
      if let Some(keyboard_id) = Self::keyboard_id(target) {
        target.send(keyboard_id, 4, &[Arg::Uint(serial), Arg::Uint(depressed), Arg::Uint(latched), Arg::Uint(locked), Arg::Uint(group)]);
        target.conn.flush();
      }
    }
  }

  fn text_input_state(client: &Client, id: u32) -> Option<(String, u32, u32, u32, u32, u32, (i32, i32, i32, i32))> {
    match client.objects.get(&id) {
      Some(Object {
        role: Role::TextInput {
          surrounding_text,
          cursor,
          anchor,
          text_change_cause,
          content_hint,
          content_purpose,
          cursor_rect,
          ..
        },
        ..
      }) => Some((surrounding_text.clone(), (*cursor).max(0) as u32, (*anchor).max(0) as u32, *text_change_cause, *content_hint, *content_purpose, *cursor_rect)),
      _ => None,
    }
  }

  fn send_text_state(&self, im: &mut Client, im_id: u32, target: &Client, target_id: u32) {
    let Some((text, cursor, anchor, cause, hint, purpose, _)) = Self::text_input_state(target, target_id) else {
      return;
    };
    im.send(im_id, 2, &[Arg::Str(Some(text)), Arg::Uint(cursor), Arg::Uint(anchor)]);
    im.send(im_id, 3, &[Arg::Uint(cause)]);
    im.send(im_id, 4, &[Arg::Uint(hint), Arg::Uint(purpose)]);
    im.send(im_id, 5, &[]);
  }

  fn activate_input_methods(&mut self, target: &Client, target_id: u32) {
    let Some((text, cursor, anchor, cause, hint, purpose, _)) = Self::text_input_state(target, target_id) else {
      return;
    };
    for im in self.clients.values_mut() {
      let ids: Vec<u32> = im.objects.iter().filter_map(|(&oid, obj)| matches!(obj.role, Role::InputMethod { .. }).then_some(oid)).collect();
      for im_id in ids {
        im.send(im_id, 0, &[]);
        im.send(im_id, 2, &[Arg::Str(Some(text.clone())), Arg::Uint(cursor), Arg::Uint(anchor)]);
        im.send(im_id, 3, &[Arg::Uint(cause)]);
        im.send(im_id, 4, &[Arg::Uint(hint), Arg::Uint(purpose)]);
        im.send(im_id, 5, &[]);
      }
    }
  }

  fn send_text_state_to_input_methods(&mut self, target: &Client, target_id: u32) {
    let Some((text, cursor, anchor, cause, hint, purpose, _)) = Self::text_input_state(target, target_id) else {
      return;
    };
    for im in self.clients.values_mut() {
      let ids: Vec<u32> = im.objects.iter().filter_map(|(&oid, obj)| matches!(obj.role, Role::InputMethod { .. }).then_some(oid)).collect();
      for im_id in ids {
        im.send(im_id, 2, &[Arg::Str(Some(text.clone())), Arg::Uint(cursor), Arg::Uint(anchor)]);
        im.send(im_id, 3, &[Arg::Uint(cause)]);
        im.send(im_id, 4, &[Arg::Uint(hint), Arg::Uint(purpose)]);
        im.send(im_id, 5, &[]);
      }
    }
  }

  fn deactivate_input_methods(&mut self, _current: Option<&mut Client>) {
    for im in self.clients.values_mut() {
      let ids: Vec<u32> = im.objects.iter().filter_map(|(&oid, obj)| matches!(obj.role, Role::InputMethod { .. }).then_some(oid)).collect();
      for id in ids {
        im.send(id, 1, &[]);
        im.send(id, 5, &[]);
      }
    }
    self.dirty = true;
  }

  fn forward_input_method_commit(&mut self, im: &mut Client, im_id: u32) {
    let (commit, preedit, delete) = match im.objects.get_mut(&im_id) {
      Some(Object {
        role: Role::InputMethod {
          pending_commit,
          pending_preedit,
          pending_delete,
          ..
        },
        ..
      }) => (pending_commit.take(), pending_preedit.take(), pending_delete.take()),
      _ => return,
    };
    let Some((target_fd, target_id, _)) = self.active_text_input else {
      return;
    };
    let serial = if target_fd == im.conn.fd {
      Self::forward_to_text_input(im, target_id, commit, preedit, delete)
    } else if let Some(target) = self.clients.get_mut(&target_fd) {
      Self::forward_to_text_input(target, target_id, commit, preedit, delete)
    } else {
      None
    };
    if serial.is_none() {
      self.active_text_input = None;
    }
  }

  fn forward_to_text_input(target: &mut Client, target_id: u32, commit: Option<String>, preedit: Option<(String, i32, i32)>, delete: Option<(u32, u32)>) -> Option<u32> {
    let serial = match target.objects.get(&target_id) {
      Some(Object {
        role: Role::TextInput { commit_serial, .. },
        ..
      }) => *commit_serial,
      _ => return None,
    };
    if let Some((text, begin, end)) = preedit {
      target.send(target_id, 2, &[Arg::Str(Some(text)), Arg::Int(begin), Arg::Int(end)]);
    }
    if let Some(text) = commit {
      target.send(target_id, 3, &[Arg::Str(Some(text))]);
    }
    if let Some((before, after)) = delete {
      target.send(target_id, 4, &[Arg::Uint(before), Arg::Uint(after)]);
    }
    target.send(target_id, 5, &[Arg::Uint(serial)]);
    Some(serial)
  }

  fn text_input_screen_rect(&self, target: &Client, target_id: u32) -> Option<(i32, i32, i32, i32)> {
    let (_, _, surface_id) = self.active_text_input?;
    let (_, _, _, _, _, _, (x, y, w, h)) = Self::text_input_state(target, target_id)?;
    let surface = match target.objects.get(&surface_id) {
      Some(Object {
        role: Role::Surface(surface),
        ..
      }) => surface,
      _ => return None,
    };
    let _buffer = surface.current_buffer.as_ref()?;
    let ox = surface.x;
    let oy = surface.y;
    Some((ox + x, oy + y, w, h))
  }

  fn position_input_method_popups_for(&mut self, target: &Client, target_id: u32) {
    let Some(rect) = self.text_input_screen_rect(target, target_id) else {
      return;
    };
    let (bw, bh) = self.backend.size();
    for im in self.clients.values_mut() {
      Self::position_popups_in_client(im, rect, bw as i32, bh as i32);
    }
    self.dirty = true;
  }

  fn position_one_input_method_popup(&mut self, im: &mut Client, popup_id: u32) {
    let Some((target_fd, target_id, _)) = self.active_text_input else {
      return;
    };
    let Some(target) = self.clients.get(&target_fd) else {
      return;
    };
    let Some(rect) = self.text_input_screen_rect(target, target_id) else {
      return;
    };
    if !im.objects.contains_key(&popup_id) {
      return;
    }
    let (bw, bh) = self.backend.size();
    Self::position_popups_in_client(im, rect, bw as i32, bh as i32);
    self.dirty = true;
  }

  fn position_popups_in_client(im: &mut Client, rect: (i32, i32, i32, i32), output_w: i32, output_h: i32) {
    let popups: Vec<(u32, u32)> = im
      .objects
      .iter()
      .filter_map(|(&oid, obj)| match obj.role {
        Role::InputPopupSurface { surface_id, .. } => Some((oid, surface_id)),
        _ => None,
      })
      .collect();
    for (popup_id, surface_id) in popups {
      let mut popup_origin = (rect.0, rect.1 + rect.3);
      if let Some(Object {
        role: Role::Surface(surface),
        ..
      }) = im.objects.get_mut(&surface_id)
      {
        let (popup_w, popup_h) = surface.current_buffer.as_ref().map(|b| (b.width, b.height)).unwrap_or((0, 0));
        let below = rect.1 + rect.3;
        surface.x = rect.0.clamp(0, (output_w - popup_w).max(0));
        surface.y = if below + popup_h <= output_h { below } else { (rect.1 - popup_h).max(0) };
        popup_origin = (surface.x, surface.y);
      }
      im.send(popup_id, 0, &[Arg::Int(rect.0 - popup_origin.0), Arg::Int(rect.1 - popup_origin.1), Arg::Int(rect.2), Arg::Int(rect.3)]);
    }
  }

  fn req_wm_base(&mut self, client: &mut Client, id: u32, opcode: u16, args: &[Arg]) {
    let base_ver = client
      .objects
      .get(&id)
      .map(|o| o.version)
      .unwrap_or(1);
    match opcode {
      0 => {
        client.objects.remove(&id);
      }
      1 => {
        let nid = args.get(0).map(|a| a.as_object()).unwrap_or(0);
        if nid != 0 {
          client.objects.insert(
            nid,
            Object::new(
              &protocol::XDG_POSITIONER,
              base_ver.min(protocol::XDG_POSITIONER.version),
              Role::Positioner {
                size_w: 0,
                size_h: 0,
                anchor_x: 0,
                anchor_y: 0,
                anchor_w: 0,
                anchor_h: 0,
                offset_x: 0,
                offset_y: 0,
                anchor: 0,
                gravity: 0,
                constraint_adjustment: 0,
              },
            ),
          );
        }
      }
      2 => {
        let nid = args.get(0).map(|a| a.as_object()).unwrap_or(0);
        let surf = args.get(1).map(|a| a.as_object()).unwrap_or(0);
        if nid != 0 {
          client.objects.insert(
            nid,
            Object::new(
              &protocol::XDG_SURFACE,
              base_ver.min(protocol::XDG_SURFACE.version),
              Role::XdgSurface {
                surface_id: surf,
                configured: false,
              },
            ),
          );
          if let Some(Object {
            role: Role::Surface(s),
            ..
          }) = client.objects.get_mut(&surf)
          {
            s.xdg_surface_id = Some(nid);
          }
        }
      }
      3 => {}
      _ => {}
    }
  }

  fn req_xdg_surface(&mut self, client: &mut Client, id: u32, opcode: u16, args: &[Arg]) {
    match opcode {
      0 => {
        client.objects.remove(&id);
      }
      1 => {
        let nid = args.get(0).map(|a| a.as_object()).unwrap_or(0);
        if nid != 0 {
          // Inherit xdg_wm_base / xdg_surface negotiated version so we only
          // emit wm_capabilities (v5+) when the client actually supports it.
          let xdg_ver = client
            .objects
            .get(&id)
            .map(|o| o.version)
            .unwrap_or(1)
            .min(protocol::XDG_TOPLEVEL.version);
          client.objects.insert(
            nid,
            Object::new(
              &protocol::XDG_TOPLEVEL,
              xdg_ver,
              Role::XdgToplevel {
                xdg_surface_id: id,
                title: String::new(),
                app_id: String::new(),
                minimized: false,
                maximized: false,
                fullscreen: false,
                parent_surface_id: None,
                saved_geom: None,
                tiled: 0,
                decoration_mode: 0, // unset — no SSD until client requests it
                min_w: 0,
                min_h: 0,
                max_w: 0,
                max_h: 0,
              },
            ),
          );
          // Advertise WM capabilities so GTK enables maximize/minimize actions
          // on CSD HeaderBar / WindowControls (window.minimize / toggle-maximized).
          if xdg_ver >= 5 {
            let caps: Vec<u8> = {
              let mut v = Vec::new();
              for c in [1u32, 2, 3, 4] {
                // window_menu, maximize, fullscreen, minimize
                v.extend_from_slice(&c.to_ne_bytes());
              }
              v
            };
            client.send(nid, 3, &[Arg::Array(caps)]); // wm_capabilities
          }
          // Hint the largest useful size (menubar/dock exclusive zone).
          if xdg_ver >= 4 {
            let (_, _, uw, uh) = self.usable_area();
            client.send(nid, 2, &[Arg::Int(uw), Arg::Int(uh)]); // configure_bounds
          }
          // width/height 0 = client picks its own size (xdg-shell).  Forcing a
          // usable-area fraction made every newly mapped app the wrong size.
          let states: Vec<u8> = Vec::new();
          client.send(
            nid,
            0, // xdg_toplevel.configure(width, height, states)
            &[Arg::Int(0), Arg::Int(0), Arg::Array(states)],
          );
          let serial = self.next_serial();
          client.send(id, 0, &[Arg::Uint(serial)]);
          client.conn.flush();
        }
      }
      2 => {
        let nid = args.get(0).map(|a| a.as_object()).unwrap_or(0);
        let parent_xdg_id = args.get(1).map(|a| a.as_object()).unwrap_or(0);
        let pos_id = args.get(2).map(|a| a.as_object()).unwrap_or(0);

        // Positioner geometry, including anchor/gravity/constraint so the popup
        // lands where GTK/Qt actually asked for it (context menus, submenus,
        // combo boxes… each use different anchor/gravity).
        let pos = match client.objects.get(&pos_id) {
          Some(Object {
            role: Role::Positioner {
              size_w, size_h, anchor_x, anchor_y, anchor_w, anchor_h,
              offset_x, offset_y, anchor, gravity, constraint_adjustment,
            },
            ..
          }) => {
            let pw = if *size_w > 0 { *size_w } else { (*anchor_w).max(4) };
            let ph = if *size_h > 0 { *size_h } else { (*anchor_h).max(4) };
            (*anchor_x, *anchor_y, *anchor_w, *anchor_h, *offset_x, *offset_y,
             pw, ph, *anchor, *gravity, *constraint_adjustment)
          }
          _ => (0, 0, 0, 0, 0, 0, 200, 200, 0, 0, 0),
        };
        let (a_x, a_y, a_w, a_h, off_x, off_y, pop_w, pop_h, p_anchor, p_gravity, p_constraint) = pos;

        let (bw, bh) = self.backend.size();

        // On-screen top-left of the parent surface. A popup parent is already
        // stored in absolute coordinates; a toplevel parent is centered.
        let (parent_screen_x, parent_screen_y) = if parent_xdg_id != 0 {
          let parent_surf_id = match client.objects.get(&parent_xdg_id) {
            Some(Object {
              role: Role::XdgSurface { surface_id, .. },
              ..
            }) => *surface_id,
            _ => 0,
          };
          if parent_surf_id != 0 {
            match client.objects.get(&parent_surf_id) {
              Some(Object {
                role: Role::Surface(s),
                ..
              }) => {
                if s.popup {
                  (s.x, s.y)
                } else if let Some(buf) = &s.current_buffer {
                  let _ = buf;
                  (s.x, s.y)
                } else {
                  (s.x, s.y)
                }
              }
              _ => (0, 0),
            }
          } else {
            (0, 0)
          }
        } else {
          (0, 0)
        };

        let (rel_x, rel_y) = popup_rel_position(
          a_x, a_y, a_w, a_h, off_x, off_y, pop_w, pop_h,
          p_anchor, p_gravity, p_constraint,
          parent_screen_x, parent_screen_y, bw as i32, bh as i32,
        );

        let abs_x = parent_screen_x + rel_x;
        let abs_y = parent_screen_y + rel_y;

        if nid != 0 {
          client.objects.insert(nid, Object::new(&protocol::XDG_POPUP, 5, Role::XdgPopup { xdg_surface_id: id }));

          let surf_id = match client.objects.get(&id) {
            Some(Object {
              role: Role::XdgSurface { surface_id, .. },
              ..
            }) => *surface_id,
            _ => 0,
          };
          if let Some(Object {
            role: Role::Surface(s),
            ..
          }) = client.objects.get_mut(&surf_id)
          {
            s.x = abs_x;
            s.y = abs_y;
            s.popup = true;
          }

          // xdg_popup.configure uses parent-relative coordinates (protocol spec).
          client.send(nid, 0, &[Arg::Int(rel_x), Arg::Int(rel_y), Arg::Int(pop_w), Arg::Int(pop_h)]);
          let serial = self.next_serial();
          client.send(id, 0, &[Arg::Uint(serial)]);
        }
      }
      3 => {
        // set_window_geometry(x, y, width, height) — content box excl. CSD shadows
        let gx = args.get(0).map(|a| a.as_int()).unwrap_or(0);
        let gy = args.get(1).map(|a| a.as_int()).unwrap_or(0);
        let gw = args.get(2).map(|a| a.as_int()).unwrap_or(0);
        let gh = args.get(3).map(|a| a.as_int()).unwrap_or(0);
        let surface_id = match client.objects.get(&id) {
          Some(Object {
            role: Role::XdgSurface { surface_id, .. },
            ..
          }) => *surface_id,
          _ => return,
        };
        if let Some(Object {
          role: Role::Surface(s),
          ..
        }) = client.objects.get_mut(&surface_id)
        {
          s.window_geom = if gw > 0 && gh > 0 {
            Some((gx, gy, gw, gh))
          } else {
            None
          };
        }
        self.dirty = true;
      }
      4 => {
        if let Some(Object {
          role: Role::XdgSurface { configured, .. },
          ..
        }) = client.objects.get_mut(&id)
        {
          *configured = true;
        }
      }
      _ => {}
    }
  }

  fn req_xdg_toplevel(&mut self, client: &mut Client, id: u32, opcode: u16, args: &[Arg]) {
    match opcode {
      0 => {
        // destroy — drop stack entry for the underlying surface
        let surface_id = client.objects.get(&id).and_then(|obj| match &obj.role {
          Role::XdgToplevel { xdg_surface_id, .. } => client.objects.get(xdg_surface_id).and_then(|o| match &o.role {
            Role::XdgSurface { surface_id, .. } => Some(*surface_id),
            _ => None,
          }),
          _ => None,
        });
        if let Some(sid) = surface_id {
          let fd = client.conn.fd;
          self.track_mapped_toplevel(fd, sid, false);
        }
        client.objects.remove(&id);
        self.dirty = true;
        self.shell_state_dirty = true;
      }
      1 => {
        // set_parent(parent?)
        let parent_tl = args.get(0).map(|a| a.as_object()).unwrap_or(0);
        let parent_surf = if parent_tl == 0 {
          None
        } else {
          self.toplevel_surface_id(client, parent_tl)
        };
        if let Some(Object {
          role: Role::XdgToplevel {
            parent_surface_id, ..
          },
          ..
        }) = client.objects.get_mut(&id)
        {
          *parent_surface_id = parent_surf;
        }
        if let (Some(child), Some(parent)) = (self.toplevel_surface_id(client, id), parent_surf) {
          let fd = client.conn.fd;
          if let (Some(pg), Some((_, _, cw, ch))) =
            (self.surface_geometry(fd, parent), self.surface_geometry(fd, child))
          {
            let (px, py, pw, ph) = pg;
            let nx = px + (pw - cw) / 2;
            let ny = py + (ph - ch) / 2;
            self.set_surface_screen_pos(fd, child, nx, ny, cw, ch);
            self.raise_surface(fd, parent);
            self.raise_surface(fd, child);
            self.dirty = true;
          }
        }
      }
      2 => {
        let title = args.get(0).and_then(|a| a.as_str()).unwrap_or("").to_string();
        if let Some(Object {
          role: Role::XdgToplevel { title: t, .. },
          ..
        }) = client.objects.get_mut(&id)
        {
          if *t != title {
            *t = title;
            self.shell_state_dirty = true;
          }
        }
      }
      3 => {
        let app_id = args.get(0).and_then(|a| a.as_str()).unwrap_or("").to_string();
        if let Some(Object {
          role: Role::XdgToplevel { app_id: a, .. },
          ..
        }) = client.objects.get_mut(&id)
        {
          if *a != app_id {
            *a = app_id;
            self.shell_state_dirty = true;
          }
        }
      }
      4 => {
        // show_window_menu(seat, serial, x, y)
        let x = args.get(2).map(|a| a.as_int()).unwrap_or(0);
        let y = args.get(3).map(|a| a.as_int()).unwrap_or(0);
        if let Some(sid) = self.toplevel_surface_id(client, id) {
          let fd = client.conn.fd;
          self.window_stack.retain(|&(f, s)| !(f == fd && s == sid));
          self.window_stack.push((fd, sid));
          self.focused_client_fd = fd;
          self.focused_surface_id = sid;
          let (sx, sy) = self
            .surface_geometry(fd, sid)
            .map(|(ox, oy, _, _)| (ox + x, oy + y))
            .unwrap_or((x, y));
          self.pending_shell_menu = Some((sid, sx, sy));
          self.dirty = true;
          self.shell_state_dirty = true;
          self.pending_activate = Some(sid);
        }
      }
      5 => {
        // move(seat, serial) — start interactive move.
        let _seat = args.get(0).map(|a| a.as_object()).unwrap_or(0);
        let serial = args.get(1).map(|a| a.as_uint()).unwrap_or(0);
        // Serial is the proof of the implicit grab.  Requiring the button to
        // still be down races with release events already queued in evdev —
        // title-bar drag then becomes a no-op under console input.
        if serial == 0 || serial != self.last_button_serial {
          return;
        }
        let Some(surface_id) = self.toplevel_surface_id(client, id) else { return };
        let (orig_x, orig_y) = match client.objects.get(&surface_id) {
          Some(Object { role: Role::Surface(s), .. }) => (s.x, s.y),
          _ => return,
        };
        let fd = client.conn.fd;
        let (px, py) = self.screen_ptr();
        self.window_stack.retain(|&(f, s)| !(f == fd && s == surface_id));
        self.window_stack.push((fd, surface_id));
        self.focused_client_fd = fd;
        self.focused_surface_id = surface_id;
        self.wm_grab = WmGrab::Move {
          fd,
          surface_id,
          grab_px: px,
          grab_py: py,
          orig_x,
          orig_y,
        };
        self.cursor_client_fd = -1;
        self.cursor_surface_id = 0;
        self.pending_cursor = None;
        self.dirty = true;
        self.shell_state_dirty = true;
      }
      6 => {
        // resize(seat, serial, edges)
        let serial = args.get(1).map(|a| a.as_uint()).unwrap_or(0);
        let edges = args.get(2).map(|a| a.as_uint()).unwrap_or(0);
        if serial == 0 || serial != self.last_button_serial || edges == 0 {
          return;
        }
        let Some(surface_id) = self.toplevel_surface_id(client, id) else { return };
        let (bw, bh) = self.backend.size();
        let (orig_x, orig_y, orig_w, orig_h) = match client.objects.get(&surface_id) {
          Some(Object { role: Role::Surface(s), .. }) => {
            let (w, h) = s.current_buffer.as_ref().map(|b| (b.width, b.height)).unwrap_or((0, 0));
            if w <= 0 || h <= 0 {
              return;
            }
            let _ = (bw, bh);
            (s.x, s.y, w, h)
          }
          _ => return,
        };
        let fd = client.conn.fd;
        let (px, py) = self.screen_ptr();
        self.window_stack.retain(|&(f, s)| !(f == fd && s == surface_id));
        self.window_stack.push((fd, surface_id));
        self.focused_client_fd = fd;
        self.focused_surface_id = surface_id;
        self.wm_grab = WmGrab::Resize {
          fd,
          surface_id,
          edges,
          grab_px: px,
          grab_py: py,
          orig_x,
          orig_y,
          orig_w,
          orig_h,
        };
        self.cursor_client_fd = -1;
        self.cursor_surface_id = 0;
        self.dirty = true;
        self.shell_state_dirty = true;
        self.pending_resize_configure = Some((fd, surface_id, orig_w, orig_h));
      }
      7 => {
        // set_max_size(width, height) — 0 means unlimited
        let w = args.get(0).map(|a| a.as_int()).unwrap_or(0).max(0);
        let h = args.get(1).map(|a| a.as_int()).unwrap_or(0).max(0);
        if let Some(Object {
          role: Role::XdgToplevel {
            max_w, max_h, ..
          },
          ..
        }) = client.objects.get_mut(&id)
        {
          *max_w = w;
          *max_h = h;
        }
      }
      8 => {
        // set_min_size(width, height)
        let w = args.get(0).map(|a| a.as_int()).unwrap_or(0).max(0);
        let h = args.get(1).map(|a| a.as_int()).unwrap_or(0).max(0);
        if let Some(Object {
          role: Role::XdgToplevel {
            min_w, min_h, ..
          },
          ..
        }) = client.objects.get_mut(&id)
        {
          *min_w = w;
          *min_h = h;
        }
      }
      9 => {
        // set_maximized
        if let Some(sid) = self.toplevel_surface_id(client, id) {
          let fd = client.conn.fd;
          let geom = match client.objects.get(&sid) {
            Some(Object {
              role: Role::Surface(s),
              ..
            }) => s.current_buffer.as_ref().map(|buf| {
              (
                s.x,
                s.y,
                buf.width,
                buf.height,
              )
            }),
            _ => None,
          };
          if let Some(Object {
            role: Role::XdgToplevel {
              maximized,
              fullscreen,
              minimized,
              tiled,
              saved_geom,
              ..
            },
            ..
          }) = client.objects.get_mut(&id)
          {
            if let Some(g) = geom {
              if !*maximized && !*fullscreen && *tiled == 0 && saved_geom.is_none() {
                *saved_geom = Some(g);
              }
            }
            *maximized = true;
            *fullscreen = false;
            *minimized = false;
            *tiled = 0;
          }
          self.pending_configure.push((fd, sid, None, false));
          self.dirty = true;
          self.shell_state_dirty = true;
        }
      }
      10 => {
        // unset_maximized
        if let Some(sid) = self.toplevel_surface_id(client, id) {
          let fd = client.conn.fd;
          let restored = if let Some(Object {
            role: Role::XdgToplevel {
              maximized,
              tiled,
              saved_geom,
              ..
            },
            ..
          }) = client.objects.get_mut(&id)
          {
            *maximized = false;
            *tiled = 0;
            saved_geom.take()
          } else {
            None
          };
          if let Some((x, y, w, h)) = restored {
            if let Some(Object {
              role: Role::Surface(s),
              ..
            }) = client.objects.get_mut(&sid)
            {
              s.x = x;
              s.y = y;
            }
            self.pending_configure.push((fd, sid, Some((w, h)), false));
          } else {
            self.pending_configure.push((fd, sid, Some((0, 0)), false));
          }
          self.dirty = true;
          self.shell_state_dirty = true;
        }
      }
      11 => {
        // set_fullscreen(output?)
        if let Some(sid) = self.toplevel_surface_id(client, id) {
          let fd = client.conn.fd;
          let geom = match client.objects.get(&sid) {
            Some(Object {
              role: Role::Surface(s),
              ..
            }) => s.current_buffer.as_ref().map(|buf| {
              (
                s.x,
                s.y,
                buf.width,
                buf.height,
              )
            }),
            _ => None,
          };
          if let Some(Object {
            role: Role::XdgToplevel {
              maximized,
              fullscreen,
              minimized,
              tiled,
              saved_geom,
              ..
            },
            ..
          }) = client.objects.get_mut(&id)
          {
            if let Some(g) = geom {
              if !*maximized && !*fullscreen && *tiled == 0 && saved_geom.is_none() {
                *saved_geom = Some(g);
              }
            }
            *fullscreen = true;
            *maximized = false;
            *minimized = false;
            *tiled = 0;
          }
          self.pending_configure.push((fd, sid, None, false));
          self.dirty = true;
          self.shell_state_dirty = true;
        }
      }
      12 => {
        // unset_fullscreen
        if let Some(sid) = self.toplevel_surface_id(client, id) {
          let fd = client.conn.fd;
          let restored = if let Some(Object {
            role: Role::XdgToplevel {
              fullscreen,
              saved_geom,
              ..
            },
            ..
          }) = client.objects.get_mut(&id)
          {
            *fullscreen = false;
            saved_geom.take()
          } else {
            None
          };
          if let Some((x, y, w, h)) = restored {
            if let Some(Object {
              role: Role::Surface(s),
              ..
            }) = client.objects.get_mut(&sid)
            {
              s.x = x;
              s.y = y;
            }
            self.pending_configure.push((fd, sid, Some((w, h)), false));
          } else {
            self.pending_configure.push((fd, sid, Some((0, 0)), false));
          }
          self.dirty = true;
          self.shell_state_dirty = true;
        }
      }
      13 => {
        // set_minimized
        if let Some(sid) = self.toplevel_surface_id(client, id) {
          let fd = client.conn.fd;
          if let Some(Object {
            role: Role::XdgToplevel { minimized, .. },
            ..
          }) = client.objects.get_mut(&id)
          {
            *minimized = true;
          }
          if self.focused_client_fd == fd && self.focused_surface_id == sid {
            self.focused_client_fd = -1;
            self.focused_surface_id = 0;
            self.ptr_entered = false;
            self.kbd_entered = false;
          }
          self.dirty = true;
          self.shell_state_dirty = true;
        }
      }
      _ => {}
    }
  }

  fn toplevel_surface_id(&self, client: &Client, toplevel_id: u32) -> Option<u32> {
    let xdg_id = match client.objects.get(&toplevel_id) {
      Some(Object { role: Role::XdgToplevel { xdg_surface_id, .. }, .. }) => *xdg_surface_id,
      _ => return None,
    };
    match client.objects.get(&xdg_id) {
      Some(Object { role: Role::XdgSurface { surface_id, .. }, .. }) => Some(*surface_id),
      _ => None,
    }
  }

  fn req_positioner(&mut self, client: &mut Client, id: u32, opcode: u16, args: &[Arg]) {
    match opcode {
      0 => {
        client.objects.remove(&id);
      }
      1 => {
        let w = args.get(0).map(|a| a.as_int()).unwrap_or(0);
        let h = args.get(1).map(|a| a.as_int()).unwrap_or(0);
        if let Some(Object {
          role: Role::Positioner { size_w, size_h, .. },
          ..
        }) = client.objects.get_mut(&id)
        {
          *size_w = w;
          *size_h = h;
        }
      }
      2 => {
        let x = args.get(0).map(|a| a.as_int()).unwrap_or(0);
        let y = args.get(1).map(|a| a.as_int()).unwrap_or(0);
        let w = args.get(2).map(|a| a.as_int()).unwrap_or(0);
        let h = args.get(3).map(|a| a.as_int()).unwrap_or(0);
        if let Some(Object {
          role: Role::Positioner {
            anchor_x,
            anchor_y,
            anchor_w,
            anchor_h,
            ..
          },
          ..
        }) = client.objects.get_mut(&id)
        {
          *anchor_x = x;
          *anchor_y = y;
          *anchor_w = w;
          *anchor_h = h;
        }
      }
      3 => {
        let a = args.get(0).map(|a| a.as_uint()).unwrap_or(0);
        if let Some(Object {
          role: Role::Positioner { anchor, .. },
          ..
        }) = client.objects.get_mut(&id)
        {
          *anchor = a;
        }
      }
      4 => {
        let g = args.get(0).map(|a| a.as_uint()).unwrap_or(0);
        if let Some(Object {
          role: Role::Positioner { gravity, .. },
          ..
        }) = client.objects.get_mut(&id)
        {
          *gravity = g;
        }
      }
      5 => {
        let ca = args.get(0).map(|a| a.as_uint()).unwrap_or(0);
        if let Some(Object {
          role: Role::Positioner { constraint_adjustment, .. },
          ..
        }) = client.objects.get_mut(&id)
        {
          *constraint_adjustment = ca;
        }
      }
      6 => {
        let x = args.get(0).map(|a| a.as_int()).unwrap_or(0);
        let y = args.get(1).map(|a| a.as_int()).unwrap_or(0);
        if let Some(Object {
          role: Role::Positioner {
            offset_x, offset_y, ..
          },
          ..
        }) = client.objects.get_mut(&id)
        {
          *offset_x = x;
          *offset_y = y;
        }
      }
      _ => {}
    }
  }

  fn req_xdg_popup(&mut self, client: &mut Client, id: u32, opcode: u16, _args: &[Arg]) {
    match opcode {
      0 => {
        if let Some(Object {
          role: Role::XdgPopup { xdg_surface_id },
          ..
        }) = client.objects.get(&id)
        {
          let xdg_surf = *xdg_surface_id;
          if let Some(Object {
            role: Role::XdgSurface { surface_id, .. },
            ..
          }) = client.objects.get(&xdg_surf)
          {
            let sid = *surface_id;
            if let Some(Object {
              role: Role::Surface(s),
              ..
            }) = client.objects.get_mut(&sid)
            {
              s.popup = false;
              s.mapped = false;
            }
          }
        }
        client.objects.remove(&id);
        self.dirty = true;
      }
      1 => {}
      2 => {}
      _ => {}
    }
  }

  fn req_region(&mut self, client: &mut Client, id: u32, opcode: u16) {
    match opcode {
      0 => { client.objects.remove(&id); }
      1 => { // add rect — region is no longer empty
        if let Some(Object { role: Role::Region { has_rects }, .. }) = client.objects.get_mut(&id) {
          *has_rects = true;
        }
      }
      _ => {}
    }
  }

  fn req_layer_shell(&mut self, client: &mut Client, _id: u32, opcode: u16, args: &[Arg]) {
    match opcode {
      0 => { // get_layer_surface(new_id, surface, ?output, layer, namespace)
        let nid    = args.get(0).map(|a| a.as_object()).unwrap_or(0);
        let surf   = args.get(1).map(|a| a.as_object()).unwrap_or(0);
        let layer  = args.get(3).map(|a| a.as_uint()).unwrap_or(0);
        if nid == 0 { return; }
        if let Some(Object { role: Role::Surface(s), .. }) = client.objects.get_mut(&surf) {
          s.layer_surface_id = Some(nid);
        }
        client.objects.insert(nid, Object::new(
          &protocol::ZWLR_LAYER_SURFACE_V1, 4,
          Role::LayerSurface {
            surface_id: surf, layer, anchor: 0xf, // all edges by default
            exclusive_zone: 0, size_w: 0, size_h: 0,
            margin_top: 0, margin_right: 0, margin_bottom: 0, margin_left: 0,
            keyboard: 0, configure_serial: 0, configured: false,
          },
        ));
      }
      1 => {} // destroy shell — no-op (surfaces already created)
      _ => {}
    }
  }

  fn req_layer_surface(&mut self, client: &mut Client, id: u32, opcode: u16, args: &[Arg]) {
    match opcode {
      0 => { // set_size(width, height)
        if let Some(Object { role: Role::LayerSurface { size_w, size_h, .. }, .. }) = client.objects.get_mut(&id) {
          *size_w = args.get(0).map(|a| a.as_uint()).unwrap_or(0);
          *size_h = args.get(1).map(|a| a.as_uint()).unwrap_or(0);
        }
      }
      1 => { // set_anchor(anchor)
        if let Some(Object { role: Role::LayerSurface { anchor, .. }, .. }) = client.objects.get_mut(&id) {
          *anchor = args.get(0).map(|a| a.as_uint()).unwrap_or(0);
        }
      }
      2 => { // set_exclusive_zone(zone)
        if let Some(Object { role: Role::LayerSurface { exclusive_zone, .. }, .. }) = client.objects.get_mut(&id) {
          *exclusive_zone = args.get(0).map(|a| a.as_int()).unwrap_or(0);
        }
      }
      3 => { // set_margin(top, right, bottom, left)
        if let Some(Object { role: Role::LayerSurface { margin_top, margin_right, margin_bottom, margin_left, .. }, .. }) = client.objects.get_mut(&id) {
          *margin_top    = args.get(0).map(|a| a.as_int()).unwrap_or(0);
          *margin_right  = args.get(1).map(|a| a.as_int()).unwrap_or(0);
          *margin_bottom = args.get(2).map(|a| a.as_int()).unwrap_or(0);
          *margin_left   = args.get(3).map(|a| a.as_int()).unwrap_or(0);
        }
      }
      4 => { // set_keyboard_interactivity(ki)
        if let Some(Object { role: Role::LayerSurface { keyboard, .. }, .. }) = client.objects.get_mut(&id) {
          *keyboard = args.get(0).map(|a| a.as_uint()).unwrap_or(0);
        }
      }
      5 => {} // get_popup — skip (popups will position themselves via xdg_popup)
      6 => { // ack_configure(serial)
        if let Some(Object { role: Role::LayerSurface { configured, .. }, .. }) = client.objects.get_mut(&id) {
          *configured = true;
        }
      }
      7 => { // destroy
        let surf_id = match client.objects.get(&id) {
          Some(Object { role: Role::LayerSurface { surface_id, .. }, .. }) => *surface_id,
          _ => { client.objects.remove(&id); return; }
        };
        if let Some(Object { role: Role::Surface(s), .. }) = client.objects.get_mut(&surf_id) {
          s.layer_surface_id = None;
        }
        client.objects.remove(&id);
        self.dirty = true;
      }
      8 => { // set_layer(layer)
        if let Some(Object { role: Role::LayerSurface { layer, .. }, .. }) = client.objects.get_mut(&id) {
          *layer = args.get(0).map(|a| a.as_uint()).unwrap_or(0);
        }
      }
      _ => {}
    }
  }

  fn req_simple_destroy(&mut self, client: &mut Client, id: u32, opcode: u16, destroy_op: u16) {
    if opcode == destroy_op {
      client.objects.remove(&id);
      self.dirty = true;
    }
  }

  fn req_dmabuf(&mut self, client: &mut Client, id: u32, opcode: u16, args: &[Arg]) {
    match opcode {
      0 => {
        client.objects.remove(&id);
      }
      1 => {
        let nid = args.get(0).map(|a| a.as_object()).unwrap_or(0);
        if nid != 0 {
          client.objects.insert(nid, Object::new(&protocol::ZWP_LINUX_BUFFER_PARAMS_V1, 4, Role::DmabufParams(DmabufParams::default())));
        }
      }
      2 => {
        let nid = args.get(0).map(|a| a.as_object()).unwrap_or(0);
        if nid != 0 {
          self.create_feedback(client, nid);
        }
      }
      3 => {
        let nid = args.get(0).map(|a| a.as_object()).unwrap_or(0);
        if nid != 0 {
          self.create_feedback(client, nid);
        }
      }
      _ => {}
    }
  }

  fn create_feedback(&mut self, client: &mut Client, id: u32) {
    client.objects.insert(id, Object::new(&protocol::ZWP_LINUX_DMABUF_FEEDBACK_V1, 4, Role::DmabufFeedback));

    let dev = self.dmabuf_main_device.to_ne_bytes().to_vec();

    if self.dmabuf_format_table >= 0 {
      // Conn owns and closes every queued SCM_RIGHTS fd after send.  Keep the
      // server's table fd persistent and transfer a fresh duplicate for each
      // default/surface feedback request.
      let table_fd = unsafe { libc::fcntl(self.dmabuf_format_table, libc::F_DUPFD_CLOEXEC, 0) };
      if table_fd < 0 {
        client.post_error(id, 0, "failed to duplicate dmabuf format table");
        return;
      }
      client.send(id, 1, &[Arg::Fd(table_fd), Arg::Uint(self.dmabuf_format_table_size as u32)]);
    }
    client.send(id, 2, &[Arg::Array(dev.clone())]);

    client.send(id, 4, &[Arg::Array(dev)]); // tranche_target_device
    let indices: Vec<u8> = [0u16, 1u16].iter().flat_map(|v| v.to_ne_bytes()).collect();
    client.send(id, 5, &[Arg::Array(indices)]); // tranche_formats
    client.send(id, 6, &[Arg::Uint(0)]);
    client.send(id, 3, &[]); // tranche_done
    client.send(id, 0, &[]);
  }

  fn req_dmabuf_params(&mut self, client: &mut Client, id: u32, opcode: u16, args: &[Arg]) {
    match opcode {
      0 => {
        client.objects.remove(&id); // destroy (Drop closes unconsumed fds)
      }
      1 => {
        let fd = args.get(0).map(|a| a.as_fd()).unwrap_or(-1);
        let plane_idx = args.get(1).map(|a| a.as_uint()).unwrap_or(0);
        let offset = args.get(2).map(|a| a.as_uint()).unwrap_or(0);
        let stride = args.get(3).map(|a| a.as_uint()).unwrap_or(0);
        let mod_hi = args.get(4).map(|a| a.as_uint()).unwrap_or(0);
        let mod_lo = args.get(5).map(|a| a.as_uint()).unwrap_or(0);
        let modifier = ((mod_hi as u64) << 32) | mod_lo as u64;
        if let Some(Object {
          role: Role::DmabufParams(p),
          ..
        }) = client.objects.get_mut(&id)
        {
          p.planes.push(DmabufPlane {
            fd,
            plane_idx,
            offset,
            stride,
            modifier,
          });
        } else if fd >= 0 {
          unsafe { libc::close(fd) };
        }
      }
      2 => {
        let w = args.get(0).map(|a| a.as_int()).unwrap_or(0);
        let h = args.get(1).map(|a| a.as_int()).unwrap_or(0);
        let fmt = args.get(2).map(|a| a.as_uint()).unwrap_or(0);
        match self.dmabuf_build(client, id, w, h, fmt) {
          Some(buf) => {
            let bid = client.alloc_server_id();
            client.objects.insert(bid, Object::new(&protocol::WL_BUFFER, 1, Role::Buffer(buf)));
            client.send(id, 0, &[Arg::NewId(bid)]); // created(buffer)
          }
          None => client.send(id, 1, &[]),
        }
      }
      3 => {
        let bid = args.get(0).map(|a| a.as_object()).unwrap_or(0);
        let w = args.get(1).map(|a| a.as_int()).unwrap_or(0);
        let h = args.get(2).map(|a| a.as_int()).unwrap_or(0);
        let fmt = args.get(3).map(|a| a.as_uint()).unwrap_or(0);
        match self.dmabuf_build(client, id, w, h, fmt) {
          Some(buf) if bid != 0 => {
            client.objects.insert(bid, Object::new(&protocol::WL_BUFFER, 1, Role::Buffer(buf)));
          }
          _ => client.post_error(id, 0, "dmabuf import failed"),
        }
      }
      _ => {}
    }
  }

  fn dmabuf_build(&mut self, client: &mut Client, params_id: u32, w: i32, h: i32, format: u32) -> Option<ShmBuffer> {
    let planes = match client.objects.get_mut(&params_id) {
      Some(Object {
        role: Role::DmabufParams(p),
        ..
      }) => {
        if p.used {
          return None;
        }
        p.used = true;
        std::mem::take(&mut p.planes)
      }
      _ => return None,
    };

    let internal = match format {
      DRM_FORMAT_ARGB8888 => Some(FORMAT_ARGB8888),
      DRM_FORMAT_XRGB8888 => Some(FORMAT_XRGB8888),
      _ => None,
    };
    let reject = if planes.len() != 1 {
      Some(format!("{} planes (only single-plane is supported)", planes.len()))
    } else if w <= 0 || h <= 0 {
      Some(format!("bad size {}x{}", w, h))
    } else if planes[0].modifier != DRM_FORMAT_MOD_LINEAR {
      Some(format!("modifier {:#018x} (only DRM_FORMAT_MOD_LINEAR is supported)", planes[0].modifier))
    } else if internal.is_none() {
      Some(format!("fourcc {:#010x} (only ARGB8888/XRGB8888 are supported)", format))
    } else {
      None
    };

    if let Some(why) = reject {
      // Silence here used to surface as a segfault inside the *client*: the
      // import fails, create_immed raises a fatal protocol error, and Mesa
      // then walks into create_wl_buffer() with a NULL image.  Say what was
      // rejected so the mismatch is visible from the compositor's own log.
      eprintln!("[luna-compositor] dmabuf import rejected: {}", why);
      for p in &planes {
        unsafe { libc::close(p.fd) };
      }
      return None;
    }

    let pl = &planes[0];
    let size = pl.offset as usize + pl.stride as usize * h as usize;
    // map_dmabuf takes fd ownership; pool uses DMA_BUF_IOCTL_SYNC on CPU reads.
    let pool = match ShmPool::map_dmabuf(pl.fd, size) {
      Some(pool) => pool,
      None => {
        // The client's GPU allocated a buffer this compositor cannot read from
        // the CPU (VRAM placement, or a driver without dmabuf mmap support).
        // Software compositing has no way forward, so point at the way out.
        eprintln!(
          "[luna-compositor] dmabuf mmap failed ({}x{}, stride={}); \
           leave LUNA_ENABLE_DMABUF unset (default) to force wl_shm",
          w, h, pl.stride
        );
        return None;
      }
    };
    Some(ShmBuffer {
      pool,
      offset: pl.offset as usize,
      width: w,
      height: h,
      stride: pl.stride as i32,
      format: internal.unwrap(),
    })
  }

  fn composite_and_present(&mut self) {
    let (w, h) = self.backend.size();
    if self.fb.width != w || self.fb.height != h {
      self.fb = Framebuffer::new(w, h);
    }
    self.fb.clear(0xff10_1014);

    // Input-method popups belong above ordinary application windows, but not
    // above the shell's own modal layers.  In particular, luna-shell uses
    // OVERLAY for its launcher, settings and confirmation surfaces; letting a
    // candidate window paint after those surfaces made the two independent
    // clients appear to tear through one another when they overlapped.
    //
    // BACKGROUND(0) < BOTTOM(1) < toplevels < TOP(2) < IM/xdg popups < OVERLAY(3)
    let mut layers: [Vec<(RawFd, u32, i32, i32)>; 4] = Default::default();
    let mut toplevels: Vec<(RawFd, u32, i32, i32)> = Vec::new();
    let mut popups: Vec<(RawFd, u32, i32, i32)> = Vec::new();

    for (&fd, client) in &self.clients {
      // Collect layer surface geometry for this client
      let ls_map: HashMap<u32, (u32, i32, i32)> = client.objects.iter().filter_map(|(_, obj)| {
        if let Role::LayerSurface { surface_id, layer, anchor, size_w, size_h, margin_top, margin_right, margin_bottom, margin_left, .. } = &obj.role {
          let (x, y, _, _) = layer_surface_rect(w, h, *anchor, *size_w, *size_h, *margin_top, *margin_right, *margin_bottom, *margin_left);
          Some((*surface_id, (*layer, x, y)))
        } else { None }
      }).collect();

      for (&surface_id, obj) in &client.objects {
        if let Role::Surface(s) = &obj.role {
          // Subsurface children are painted with their parent tree — never alone.
          if s.subsurface_parent.is_some() {
            continue;
          }

          if let Some((layer, dx, dy)) = ls_map.get(&surface_id).copied() {
            if !s.mapped && !Self::surface_tree_has_content(client, surface_id) {
              continue;
            }
            // Layer shell surface
            if (layer as usize) < 4 {
              layers[layer as usize].push((fd, surface_id, dx, dy));
            }
          } else if s.input_method_popup {
            if s.mapped && s.current_buffer.is_some() && self.active_text_input.is_some() {
              popups.push((fd, surface_id, s.x, s.y));
            }
          } else if s.popup {
            if s.mapped && s.current_buffer.is_some() {
              popups.push((fd, surface_id, s.x, s.y));
            }
          } else if s.xdg_surface_id.is_some() {
            if self.surface_is_minimized(client, surface_id) {
              continue;
            }
            // Parent may be buffer-less while Firefox paints on subsurfaces.
            if !s.mapped && !Self::surface_tree_has_content(client, surface_id) {
              continue;
            }
            toplevels.push((fd, surface_id, s.x, s.y));
          }
        }
      }
    }

    // Stack order: raised windows render last (on top).
    toplevels.sort_by_key(|(fd, sid, _, _)| self.stack_index(*fd, *sid));

    for (fd, sid, dx, dy) in &layers[0] {
      let blits = self
        .clients
        .get(fd)
        .map(|c| Self::collect_surface_tree_blits(c, *sid, *dx, *dy, 0))
        .unwrap_or_default();
      Self::blit_collected(&mut self.fb, &blits);
    }
    for (fd, sid, dx, dy) in &layers[1] {
      let blits = self
        .clients
        .get(fd)
        .map(|c| Self::collect_surface_tree_blits(c, *sid, *dx, *dy, 0))
        .unwrap_or_default();
      Self::blit_collected(&mut self.fb, &blits);
    }
    for (fd, sid, dx, dy) in &toplevels {
      let blits = self
        .clients
        .get(fd)
        .map(|c| Self::collect_surface_tree_blits(c, *sid, *dx, *dy, 0))
        .unwrap_or_default();
      Self::blit_collected(&mut self.fb, &blits);
      // Server-side chrome only when the client explicitly requested SSD (mode 2).
      let ssd = Self::uses_ssd(self.toplevel_decoration_mode(*fd, *sid));
      let maxed_or_full = self.clients.get(fd).and_then(|c| {
        let xdg = match c.objects.get(sid)? {
          Object {
            role: Role::Surface(s),
            ..
          } => s.xdg_surface_id?,
          _ => return None,
        };
        c.objects.values().find_map(|o| match &o.role {
          Role::XdgToplevel {
            xdg_surface_id,
            maximized,
            fullscreen,
            ..
          } if *xdg_surface_id == xdg => Some(*maximized || *fullscreen),
          _ => None,
        })
      }).unwrap_or(false);
      if ssd {
        // Frame around window geometry (excl. any client shadow padding).
        let (fx, fy, fw, fh) = self.surface_geometry(*fd, *sid).unwrap_or_else(|| {
          let (tw, th) = self
            .clients
            .get(fd)
            .and_then(|c| Self::surface_tree_size(c, *sid))
            .unwrap_or((640, 480));
          (*dx, *dy, tw, th)
        });
        self.draw_ssd_titlebar(fx, fy, fw);
        if !maxed_or_full {
          self.draw_window_frame(fx, fy, fw, fh, true);
        }
      }
    }
    for (fd, sid, dx, dy) in &layers[2] {
      let blits = self
        .clients
        .get(fd)
        .map(|c| Self::collect_surface_tree_blits(c, *sid, *dx, *dy, 0))
        .unwrap_or_default();
      Self::blit_collected(&mut self.fb, &blits);
    }
    for (fd, sid, dx, dy) in &popups {
      let blits = self
        .clients
        .get(fd)
        .map(|c| Self::collect_surface_tree_blits(c, *sid, *dx, *dy, 0))
        .unwrap_or_default();
      Self::blit_collected(&mut self.fb, &blits);
    }
    for (fd, sid, dx, dy) in &layers[3] {
      let blits = self
        .clients
        .get(fd)
        .map(|c| Self::collect_surface_tree_blits(c, *sid, *dx, *dy, 0))
        .unwrap_or_default();
      Self::blit_collected(&mut self.fb, &blits);
    }

    // Software cursor — topmost.  Prefer the client's wl_pointer.set_cursor
    // surface; otherwise draw a built-in arrow so the pointer is never blank
    // on a KMS compositor with no hardware cursor plane of its own.
    {
      let px = (self.ptr_x * w as f32) as i32;
      let py = (self.ptr_y * h as f32) as i32;
      // The compositor owns cursor feedback over its resize rim.  During a
      // grab the pointer may be anywhere, but it must keep the cursor shape
      // selected when the grab began.
      let resize_edges = match &self.wm_grab {
        WmGrab::Resize { edges, .. } => Some(*edges),
        WmGrab::None => self.hit_resize_edge(px, py).map(|(_, _, edges)| edges),
        WmGrab::Move { .. } => None,
      };
      let client_cursor = if self.cursor_surface_id != 0 && self.cursor_client_fd >= 0 {
        self.clients.get(&self.cursor_client_fd).and_then(|client| {
          match client.objects.get(&self.cursor_surface_id) {
            Some(Object { role: Role::Surface(s), .. }) => s.current_buffer.clone(),
            _ => None,
          }
        })
      } else {
        None
      };
      let drew_client = if resize_edges.is_some() {
        false
      } else if let Some(buf) = client_cursor {
        if buf.width > 0 && buf.height > 0 && shm_buffer_has_visible_pixel(&buf) {
          self.fb.blit_shm(&buf, px - self.cursor_hot_x, py - self.cursor_hot_y);
          true
        } else {
          false
        }
      } else {
        false
      };
      // GTK can hand us a cursor surface whose buffer is still empty /
      // fully transparent (theme load race).  Fall back so the pointer
      // never vanishes until the next set_cursor (often triggered by typing).
      if let Some(edges) = resize_edges {
        crate::cursor_aero::blit_resize_cursor(&mut self.fb, px, py, edges);
      } else if !drew_client {
        self.fb.blit_default_cursor(px, py);
      }
    }

    self.backend.present(&self.fb);

    // Release buffers AFTER presentation so the client (Mesa EGL) doesn't
    // free its backing buffer while eglSwapBuffers is still pending on
    // wl_callback.done.  Sending release here, before frame_done, lets Mesa
    // reuse the buffer as early as possible while still being safe.
    let releases = std::mem::take(&mut self.buffer_release);
    for (fd, bid) in releases {
      if let Some(client) = self.clients.get_mut(&fd) {
        client.send(bid, 0, &[]);
      }
    }

    let ts = now_ms();
    let pending = std::mem::take(&mut self.frame_done);
    for (fd, cb) in pending {
      if let Some(client) = self.clients.get_mut(&fd) {
        client.send(cb, 0, &[Arg::Uint(ts)]);
        client.send(1, 1, &[Arg::Uint(cb)]);
        client.objects.remove(&cb);
      }
    }
  }

  fn process_input_events(&mut self) {
    let Some(rx) = self.input_rx.take() else { return };
    while let Ok(ev) = rx.try_recv() {
      match ev {
        InputEvent::PointerMotion { x, y } => {
          self.ptr_x = x.clamp(0.0, 1.0);
          self.ptr_y = y.clamp(0.0, 1.0);
          self.dirty = true; // software cursor must move with the pointer
          self.inject_ptr_motion(self.ptr_x, self.ptr_y);
        }
        InputEvent::PointerRelative { dx, dy } => {
          let (w, h) = self.backend.size();
          self.ptr_x = (self.ptr_x + dx / w.max(1) as f32).clamp(0.0, 1.0);
          self.ptr_y = (self.ptr_y + dy / h.max(1) as f32).clamp(0.0, 1.0);
          self.dirty = true;
          self.inject_ptr_motion(self.ptr_x, self.ptr_y);
        }
        InputEvent::PointerButton { button, pressed } => {
          self.inject_ptr_button(button, pressed);
          // After a press, drain client sockets once so xdg_toplevel.move /
          // .resize can land before a queued release clears last_button_pressed.
          if pressed {
            self.poll_clients_nonblocking();
          }
        }
        InputEvent::PointerAxis { axis, value } => self.inject_ptr_axis(axis, value),
        InputEvent::Key { keycode, pressed } => self.inject_key(keycode, pressed),
        InputEvent::Reset => self.reset_input_state(),
        InputEvent::VtSwitch(vt) => self.backend.switch_vt(vt),
      }
    }
    self.input_rx = Some(rx);
  }

  /// Non-blocking pass over every client socket so interactive grabs
  /// (move/resize) requested in response to a button press are applied
  /// before we process a release that is already sitting in the input queue.
  fn poll_clients_nonblocking(&mut self) {
    let fds: Vec<RawFd> = self.clients.keys().copied().collect();
    for fd in fds {
      // Skip if the client disappeared mid-loop.
      if !self.clients.contains_key(&fd) {
        continue;
      }
      // Peek with a 0-timeout poll so we never block the input path.
      let mut pfd = libc::pollfd {
        fd,
        events: libc::POLLIN,
        revents: 0,
      };
      let pr = unsafe { libc::poll(&mut pfd, 1, 0) };
      if pr > 0 && (pfd.revents & libc::POLLIN) != 0 {
        self.recv_client(fd);
      }
    }
  }

  fn reset_input_state(&mut self) {
    let keys = std::mem::take(&mut self.pressed_keys);
    for key in keys {
      self.inject_key(key, false);
    }
    self.kbd_mods = 0;
    self.ptr_entered = false;
    self.kbd_entered = false;
  }

  fn process_signals(&mut self) -> bool {
    loop {
      let mut info: libc::signalfd_siginfo = unsafe { std::mem::zeroed() };
      let n = unsafe { libc::read(self.signal_fd, &mut info as *mut libc::signalfd_siginfo as *mut libc::c_void, std::mem::size_of::<libc::signalfd_siginfo>()) };
      if n < 0 {
        break;
      }
      match info.ssi_signo as libc::c_int {
        libc::SIGUSR1 => self.backend.deactivate(),
        libc::SIGUSR2 => {
          self.backend.activate();
          self.dirty = true;
        }
        libc::SIGINT | libc::SIGTERM | libc::SIGHUP | libc::SIGQUIT => return false,
        _ => {}
      }
    }
    true
  }

  fn find_input_target(&self) -> Option<(RawFd, u32, u32, u32, i32, i32, i32, i32)> {
    let (bw, bh) = self.backend.size();
    let px = (self.ptr_x * bw as f32) as i32;
    let py = (self.ptr_y * bh as f32) as i32;

    // Collect candidates per client
    // (layer_priority, fd, surface_id, ptr_id, kbd_id, surf_w, surf_h, ox, oy)
    // layer_priority: OVERLAY=60, IM/xdg popup=50, TOP=30,
    // focused_window=20, other_window=10, BOTTOM=5, BG=0.
    // Keep this in the same order as composite_and_present(), so an invisible
    // pointer-focus layer can never disagree with what is visibly on top.
    let mut candidates: Vec<(i32, RawFd, u32, u32, u32, i32, i32, i32, i32)> = Vec::new();

    for (&fd, client) in &self.clients {
      let mut ptr_id = 0u32;
      let mut kbd_id = 0u32;
      for (&oid, obj) in &client.objects {
        match &obj.role {
          Role::Pointer => ptr_id = oid,
          Role::Keyboard => kbd_id = oid,
          _ => {}
        }
      }

      // Build map: surface_id → (layer, anchor, size_w, size_h, mt, mr, mb, ml, keyboard)
      let ls_map: HashMap<u32, (u32, u32, u32, u32, i32, i32, i32, i32, u32)> = client.objects.iter().filter_map(|(_, obj)| {
        if let Role::LayerSurface { surface_id, layer, anchor, size_w, size_h, margin_top, margin_right, margin_bottom, margin_left, keyboard, .. } = &obj.role {
          Some((*surface_id, (*layer, *anchor, *size_w, *size_h, *margin_top, *margin_right, *margin_bottom, *margin_left, *keyboard)))
        } else { None }
      }).collect();

      for (&surf_id, obj) in &client.objects {
        let Role::Surface(s) = &obj.role else { continue };
        if !s.accepts_input { continue; }
        if let Some((layer, anchor, size_w, size_h, mt, mr, mb, ml, _kbd)) = ls_map.get(&surf_id).copied() {
          if s.mapped && s.current_buffer.is_some() {
            // BACKGROUND is wallpaper: even if a buggy client forgets to clear
            // its input region, never let it beat real windows or chrome.
            if layer == 0 {
              continue;
            }
            let (ox, oy, cw, ch) = layer_surface_rect(bw, bh, anchor, size_w, size_h, mt, mr, mb, ml);
            if px >= ox && py >= oy && px < ox + cw as i32 && py < oy + ch as i32 {
              let prio = match layer { 3 => 60, 2 => 30, 1 => 5, _ => 0 };
              candidates.push((prio, fd, surf_id, ptr_id, kbd_id, cw as i32, ch as i32, ox, oy));
            }
          }
        } else if s.input_method_popup {
          if let Some(buf) = &s.current_buffer {
            if self.active_text_input.is_some()
              && px >= s.x
              && py >= s.y
              && px < s.x + buf.width
              && py < s.y + buf.height
            {
              candidates.push((50, fd, surf_id, ptr_id, kbd_id, buf.width, buf.height, s.x, s.y));
            }
          }
        } else if s.popup {
          if let Some(buf) = &s.current_buffer {
            if px >= s.x && py >= s.y && px < s.x + buf.width && py < s.y + buf.height {
              candidates.push((45, fd, surf_id, ptr_id, kbd_id, buf.width, buf.height, s.x, s.y));
            }
          }
        } else if s.xdg_surface_id.is_some()
            && !self.surface_is_minimized(client, surf_id)
            // Firefox/WebRender often leaves the xdg_toplevel parent
            // buffer-less and commits pixels only to desynchronised
            // subsurfaces.  The parent still owns wl_pointer/wl_keyboard,
            // so hit-test the visible surface tree but deliver input to this
            // parent surface.
            && Self::surface_tree_has_content(client, surf_id)
          {
            let ox = s.x;
            let oy = s.y;
            // MUST clip to the window rect.  Without this, the topmost
            // xdg_toplevel steals pointer focus for the entire output —
            // cursor vanishes everywhere except layer-shell chrome (dock),
            // and title-bar hit-testing / move requests go to the wrong place.
            let (w, h) = Self::surface_tree_size(client, surf_id)
              // `surface_tree_has_content` above guarantees a non-empty tree.
              .unwrap_or((0, 0));
            if px >= ox && py >= oy && px < ox + w && py < oy + h {
              // Base prio 10 for all windows; stack order breaks ties below.
              candidates.push((10, fd, surf_id, ptr_id, kbd_id, w, h, ox, oy));
            }
        }
      }
    }

    // Return highest-priority candidate; among equal prio prefer raised windows.
    candidates.sort_by(|a, b| {
      b.0.cmp(&a.0).then_with(|| self.stack_index(b.1, b.2).cmp(&self.stack_index(a.1, a.2)))
    });
    candidates.into_iter().next().map(|(_, fd, sid, ptr_id, kbd_id, sw, sh, ox, oy)| (fd, sid, ptr_id, kbd_id, sw, sh, ox, oy))
  }

  fn to_surface_fixed(&self, nx: f32, ny: f32, origin_x: i32, origin_y: i32) -> (i32, i32) {
    let (bw, bh) = self.backend.size();
    let sx = (nx * bw as f32) as i32 - origin_x;
    let sy = (ny * bh as f32) as i32 - origin_y;
    (sx * 256, sy * 256)
  }

  fn inject_ptr_motion(&mut self, nx: f32, ny: f32) {
    if !matches!(self.wm_grab, WmGrab::None) {
      self.update_wm_grab();
      return;
    }
    let (px, py) = self.screen_ptr();
    // Keep compositor cursor over SSD chrome / resize frame (outside client buffer).
    if self.hit_ssd(px, py).is_some() || self.hit_resize_edge(px, py).is_some() {
      let had_client_cursor = self.cursor_surface_id != 0 || self.ptr_entered;
      self.send_pointer_leave();
      if self.cursor_client_fd >= 0 || self.cursor_surface_id != 0 {
        self.cursor_client_fd = -1;
        self.cursor_surface_id = 0;
        self.pending_cursor = None;
        self.dirty = true; // redraw once to drop client glyph
      } else if had_client_cursor {
        self.dirty = true;
      }
      return;
    }
    let Some((fd, surf_id, ptr_id, _, _sw, _sh, ox, oy)) = self.find_input_target() else {
      // Pointer left every interactive surface — notify the previous client.
      self.send_pointer_leave();
      return;
    };
    if ptr_id == 0 {
      // Surface exists but the client has not bound wl_pointer yet —
      // leave the previous focus so we don't keep a stale cursor role,
      // and let the compositor default arrow show.
      self.send_pointer_leave();
      return;
    }
    let (fx, fy) = self.to_surface_fixed(nx, ny, ox, oy);
    let ts = now_ms();
    let entered = self.ptr_entered && self.ptr_client_fd == fd && self.ptr_surface_id == surf_id;
    if !entered {
      // Leave the previous surface (same or other client) before entering.
      self.send_pointer_leave();
      let serial = self.next_serial();
      if let Some(client) = self.clients.get_mut(&fd) {
        client.send(ptr_id, 0, &[Arg::Uint(serial), Arg::Object(surf_id), Arg::Fixed(fx), Arg::Fixed(fy)]);
        client.send(ptr_id, 5, &[]);
        client.conn.flush();
      }
      self.ptr_entered = true;
      self.ptr_client_fd = fd;
      self.ptr_surface_id = surf_id;
      // New surface owns the cursor role; clear a stale glyph from the
      // previous client so we show the default arrow until set_cursor.
      if self.cursor_client_fd != fd {
        self.cursor_client_fd = -1;
        self.cursor_surface_id = 0;
        self.pending_cursor = None;
      }
      self.dirty = true;
    }
    if let Some(client) = self.clients.get_mut(&fd) {
      client.send(ptr_id, 2, &[Arg::Uint(ts), Arg::Fixed(fx), Arg::Fixed(fy)]);
      client.send(ptr_id, 5, &[]);
      client.conn.flush();
    }
    // Do NOT move keyboard / text-input focus on motion.  Pointer-follows
    // focus was deactivating whiz-im-wayland whenever the cursor crossed the
    // dock or menubar, which is why Japanese input died after a hover.
  }

  fn send_pointer_leave(&mut self) {
    if !self.ptr_entered || self.ptr_client_fd < 0 || self.ptr_surface_id == 0 {
      self.ptr_entered = false;
      return;
    }
    let fd = self.ptr_client_fd;
    let surf = self.ptr_surface_id;
    let ptr_id = self.clients.get(&fd).and_then(|c| {
      c.objects.iter().find_map(|(&id, o)| matches!(o.role, Role::Pointer).then_some(id))
    }).unwrap_or(0);
    if ptr_id != 0 {
      let serial = self.next_serial();
      if let Some(client) = self.clients.get_mut(&fd) {
        // wl_pointer.leave = opcode 1
        client.send(ptr_id, 1, &[Arg::Uint(serial), Arg::Object(surf)]);
        client.send(ptr_id, 5, &[]);
        client.conn.flush();
      }
    }
    // Drop the leaving client's cursor role so we never keep drawing a
    // stale glyph over the next surface (or an empty desktop).
    if self.cursor_client_fd == fd {
      self.cursor_client_fd = -1;
      self.cursor_surface_id = 0;
      self.dirty = true;
    }
    self.ptr_entered = false;
  }

  fn inject_ptr_button(&mut self, button: u32, pressed: bool) {
    // Left button release ends an interactive move/resize grab.
    if !pressed && button == 0x110 && !matches!(self.wm_grab, WmGrab::None) {
      self.last_button_pressed = false;
      self.end_wm_grab();
      return;
    }
    // Server-side decoration chrome (above client surface).
    if button == 0x110 && pressed {
      let (px, py) = self.screen_ptr();
      if let Some((action, fd, sid)) = self.hit_ssd(px, py) {
        let serial = self.next_serial();
        self.last_button_serial = serial;
        self.last_button_pressed = true;
        match action {
          "close" => self.close_surface(sid),
          "min" => self.minimize_surface(sid),
          "max" => self.toggle_maximize_surface(sid),
          "move" => {
            // Double-click titlebar → maximize/restore (classic WM).
            let now = now_ms();
            let dbl = matches!(
              self.ssd_last_click,
              Some((t, prev)) if prev == sid && now.wrapping_sub(t) < 400
            );
            self.ssd_last_click = Some((now, sid));
            if dbl {
              self.ssd_last_click = None;
              self.raise_surface(fd, sid);
              self.focused_client_fd = fd;
              self.focused_surface_id = sid;
              self.toggle_maximize_surface(sid);
              self.dirty = true;
              self.shell_state_dirty = true;
              return;
            }
            let (orig_x, orig_y) = match self.clients.get(&fd).and_then(|c| c.objects.get(&sid)) {
              Some(Object {
                role: Role::Surface(s),
                ..
              }) => (s.x, s.y),
              _ => return,
            };
            self.raise_surface(fd, sid);
            self.focused_client_fd = fd;
            self.focused_surface_id = sid;
            self.wm_grab = WmGrab::Move {
              fd,
              surface_id: sid,
              grab_px: px,
              grab_py: py,
              orig_x,
              orig_y,
            };
            self.dirty = true;
          }
          _ => {}
        }
        return;
      }
      // Compositor-side resize border (outside the client buffer only).
      // Firefox SSD / clients that never call xdg_toplevel.resize from CSD edges.
      if let Some((fd, sid, edges)) = self.hit_resize_edge(px, py) {
        let serial = self.next_serial();
        self.last_button_serial = serial;
        self.last_button_pressed = true;
        // Position = buffer origin; size = window geometry (configure units).
        let Some((orig_x, orig_y, _, _)) = self.surface_buffer_rect(fd, sid) else {
          return;
        };
        let Some((_, _, orig_w, orig_h)) = self.surface_geometry(fd, sid) else {
          return;
        };
        self.raise_surface(fd, sid);
        self.focused_client_fd = fd;
        self.focused_surface_id = sid;
        self.wm_grab = WmGrab::Resize {
          fd,
          surface_id: sid,
          edges,
          grab_px: px,
          grab_py: py,
          orig_x,
          orig_y,
          orig_w,
          orig_h,
        };
        self.cursor_client_fd = -1;
        self.cursor_surface_id = 0;
        self.pending_cursor = None;
        // Do NOT send a configure until the pointer actually moves — a bare
        // click on the frame was flooding GTK/Firefox with RESIZING configures.
        self.dirty = true;
        self.shell_state_dirty = true;
        return;
      }
    }
    let Some((fd, surf_id, ptr_id, _, _sw, _sh, ox, oy)) = self.find_input_target() else { return };
    if ptr_id == 0 {
      return;
    }
    if !self.ptr_entered || self.ptr_client_fd != fd || self.ptr_surface_id != surf_id {
      // Enter at the real pointer location, not the screen center, so the
      // button lands where the user actually clicked.
      self.send_pointer_leave();
      let (fx, fy) = self.to_surface_fixed(self.ptr_x, self.ptr_y, ox, oy);
      let serial = self.next_serial();
      {
        let client = self.clients.get_mut(&fd).unwrap();
        client.send(ptr_id, 0, &[Arg::Uint(serial), Arg::Object(surf_id), Arg::Fixed(fx), Arg::Fixed(fy)]);
        client.send(ptr_id, 5, &[]);
        client.conn.flush();
      }
      self.ptr_entered = true;
      self.ptr_client_fd = fd;
      self.ptr_surface_id = surf_id;
      if self.cursor_client_fd != fd {
        self.cursor_client_fd = -1;
        self.cursor_surface_id = 0;
        self.pending_cursor = None;
      }
    }
    let serial = self.next_serial();
    if pressed {
      self.last_button_pressed = true;
      self.last_button_serial = serial;
      // Click-to-focus.  Object ids are per-client — always check the surface
      // on the fd returned by find_input_target, never a global id lookup.
      let is_toplevel = self
        .clients
        .get(&fd)
        .map(|c| Self::surface_is_xdg_toplevel_in(c, surf_id))
        .unwrap_or(false);
      if is_toplevel {
        let prev_fd = self.focused_client_fd;
        let prev_sid = self.focused_surface_id;
        let focus_changed = prev_fd != fd || prev_sid != surf_id;
        if focus_changed {
          self.raise_surface(fd, surf_id);
          self.focused_client_fd = fd;
          self.focused_surface_id = surf_id;
          self.shell_state_dirty = true;
          self.dirty = true;
          // No ACTIVATED configure here — see activate_surface().
          self.ensure_kbd_entered(fd, surf_id);
        }
      } else if self.layer_keyboard_mode(fd, surf_id) != 0 {
        // Launchpad / settings / confirm: layer-shell ON_DEMAND keyboard.
        // Dock and menubar keep keyboard=NONE so they never steal typing.
        self.focused_client_fd = fd;
        self.focused_surface_id = surf_id;
        self.shell_state_dirty = true;
        self.ensure_kbd_entered(fd, surf_id);
      }
      // else: chrome without keyboard — pointer events only; leave kbd focus.
    } else {
      self.last_button_pressed = false;
    }
    let ts = now_ms();
    let state: u32 = if pressed { 1 } else { 0 };
    let client = self.clients.get_mut(&fd).unwrap();
    client.send(ptr_id, 3, &[Arg::Uint(serial), Arg::Uint(ts), Arg::Uint(button), Arg::Uint(state)]);
    client.send(ptr_id, 5, &[]);
    client.conn.flush();
  }

  fn inject_ptr_axis(&mut self, axis: u32, value: f32) {
    let Some((fd, _, ptr_id, _, _, _, _, _)) = self.find_input_target() else { return };
    if ptr_id == 0 {
      return;
    }
    let ts = now_ms();
    let fv = (value * 256.0) as i32;
    let client = self.clients.get_mut(&fd).unwrap();
    client.send(ptr_id, 4, &[Arg::Uint(ts), Arg::Uint(axis), Arg::Fixed(fv)]);
    client.send(ptr_id, 5, &[]);
    client.conn.flush();
  }

  fn inject_key(&mut self, keycode: u32, pressed: bool) {
    if pressed {
      self.pressed_keys.insert(keycode);
    } else {
      self.pressed_keys.remove(&keycode);
    }
    // xkb mod bits for the stock pc+us keymap: Shift=1, Control=4, Mod1/Alt=8, Mod4/Super=64
    let mod_bit: u32 = match keycode {
      42 | 54 => 1,          // ShiftLeft / ShiftRight
      29 | 97 => 4,          // ControlLeft / ControlRight
      56 | 100 => 8,         // AltLeft / AltRight
      125 | 126 => 64,       // SuperLeft / SuperRight (Mod4)
      _ => 0,
    };
    if mod_bit != 0 {
      if pressed {
        self.kbd_mods |= mod_bit;
      } else {
        self.kbd_mods &= !mod_bit;
      }
    }

    // KEY_ESC is 1 on libinput's evdev keycode scale.  A resize is a
    // compositor-owned modal operation, so do this before forwarding to an
    // input-method grab or the focused client.
    if pressed && keycode == 1 && matches!(self.wm_grab, WmGrab::Resize { .. }) {
      self.cancel_wm_grab();
      return;
    }

    // Session Zap — handled here so it wins over text fields and IM grabs.
    // KEY_BACKSPACE = 14 on the Linux evdev keycode scale used by libinput.
    if pressed && keycode == 14 && (self.kbd_mods & (4 | 8)) == (4 | 8) {
      self.session_quit = true;
      return;
    }
    // Alt+F4 — close the focused toplevel only (never Zap the session).
    if pressed && keycode == 62 && (self.kbd_mods & 8) != 0 {
      let focus = self.focused_surface_id;
      let focus_fd = self.focused_client_fd;
      if focus != 0
        && focus_fd >= 0
        && self
          .clients
          .get(&focus_fd)
          .map(|c| Self::surface_is_xdg_toplevel_in(c, focus))
          .unwrap_or(false)
      {
        self.close_surface(focus);
      }
      return;
    }
    // Super+D — show desktop / restore (minimize all floating toplevels).
    if pressed && keycode == 32 && (self.kbd_mods & 64) != 0 && (self.kbd_mods & 8) == 0 {
      self.toggle_show_desktop();
      return;
    }
    // Super+Arrow WM shortcuts (ignore when Alt held for Alt+Tab).
    if pressed && (self.kbd_mods & 64) != 0 && (self.kbd_mods & 8) == 0 {
      let focus = self.focused_surface_id;
      if focus != 0 {
        match keycode {
          103 => {
            // Up → maximize toggle
            self.toggle_maximize_surface(focus);
            return;
          }
          108 => {
            // Down → restore if max/tiled else minimize
            let state = self.client_for_xdg_surface(focus).and_then(|(fd, _)| {
              let c = self.clients.get(&fd)?;
              let xdg = match c.objects.get(&focus)? {
                Object {
                  role: Role::Surface(s),
                  ..
                } => s.xdg_surface_id?,
                _ => return None,
              };
              c.objects.values().find_map(|o| match &o.role {
                Role::XdgToplevel {
                  xdg_surface_id,
                  maximized,
                  tiled,
                  ..
                } if *xdg_surface_id == xdg => Some((*maximized, *tiled)),
                _ => None,
              })
            });
            match state {
              Some((true, _)) => self.maximize_surface(focus, false),
              Some((_, t)) if t != 0 => {
                self.clear_tile_flags(
                  self.client_for_xdg_surface(focus).map(|(f, _)| f).unwrap_or(-1),
                  focus,
                );
                if let Some((fd, _)) = self.client_for_xdg_surface(focus) {
                  let size = self.restore_saved_geom(fd, focus);
                  self.send_toplevel_configure_for(fd, focus, size, false);
                }
              }
              _ => self.minimize_surface(focus),
            }
            return;
          }
          105 => {
            // Left → tile left
            self.tile_surface(focus, 1);
            return;
          }
          106 => {
            // Right → tile right
            self.tile_surface(focus, 2);
            return;
          }
          _ => {}
        }
      }
    }
    // Alt+Tab / Super+Tab — cycle among mapped toplevels (Shift reverses).
    if pressed && keycode == 15 {
      let alt = (self.kbd_mods & 8) != 0;
      let super_ = (self.kbd_mods & 64) != 0;
      if alt || super_ {
        let reverse = (self.kbd_mods & 1) != 0;
        self.cycle_focused_window(reverse);
        return;
      }
    }
    // Alt/Super release ends the switcher.
    if !pressed && (keycode == 56 || keycode == 100 || keycode == 125 || keycode == 126) {
      if self.switcher.is_some() && (self.kbd_mods & (8 | 64)) == 0 {
        self.commit_switcher();
        return;
      }
    }

    if let Some((fd, grab_id)) = self.find_input_method_keyboard_grab() {
      let serial = self.next_serial();
      let modifiers_serial = self.next_serial();
      let ts = now_ms();
      let state = if pressed { 1 } else { 0 };
      if let Some(client) = self.clients.get_mut(&fd) {
        client.send(grab_id, 1, &[Arg::Uint(serial), Arg::Uint(ts), Arg::Uint(keycode), Arg::Uint(state)]);
        client.send(grab_id, 2, &[Arg::Uint(modifiers_serial), Arg::Uint(self.kbd_mods), Arg::Uint(0), Arg::Uint(0), Arg::Uint(0)]);
        client.conn.flush();
      }
      return;
    }
    // Click-to-focus: keys go to the activated surface, not wherever the
    // pointer happens to hover (dock/menubar must not steal typing).
    let (fd, surf_id, kbd_id) = if self.focused_client_fd >= 0 && self.focused_surface_id != 0 {
      let fd = self.focused_client_fd;
      let surf_id = self.focused_surface_id;
      let kbd_id = self
        .clients
        .get(&fd)
        .and_then(|c| {
          c.objects
            .iter()
            .find_map(|(&id, o)| matches!(o.role, Role::Keyboard).then_some(id))
        })
        .unwrap_or(0);
      (fd, surf_id, kbd_id)
    } else {
      let Some((fd, surf_id, _, kbd_id, _, _, _, _)) = self.find_input_target() else {
        return;
      };
      (fd, surf_id, kbd_id)
    };
    if kbd_id == 0 {
      return;
    }
    self.ensure_kbd_entered(fd, surf_id);
    let serial = self.next_serial();
    let s2 = self.next_serial();
    let ts = now_ms();
    let state: u32 = if pressed { 1 } else { 0 };
    let mods = self.kbd_mods;
    let client = self.clients.get_mut(&fd).unwrap();
    client.send(kbd_id, 3, &[Arg::Uint(serial), Arg::Uint(ts), Arg::Uint(keycode), Arg::Uint(state)]);
    client.send(kbd_id, 4, &[Arg::Uint(s2), Arg::Uint(mods), Arg::Uint(0), Arg::Uint(0), Arg::Uint(0)]);
    client.conn.flush();
  }

  fn find_input_method_keyboard_grab(&self) -> Option<(RawFd, u32)> {
    if self.active_text_input.is_none() {
      return None;
    }
    self.clients.iter().find_map(|(&fd, client)| client.objects.iter().find_map(|(&id, obj)| matches!(obj.role, Role::InputMethodKeyboardGrab { .. }).then_some((fd, id))))
  }

  fn ensure_kbd_entered(&mut self, fd: RawFd, surf_id: u32) {
    self.update_text_input_focus(fd, surf_id);
    let focus_changed = !(self.kbd_entered && self.kbd_client_fd == fd && self.kbd_surface_id == surf_id);
    if !focus_changed {
      return;
    }
    // Leave the previous keyboard focus so GTK/shell don't both think they own it.
    if self.kbd_entered && self.kbd_client_fd >= 0 &&
       (self.kbd_client_fd != fd || self.kbd_surface_id != surf_id)
    {
      let prev_fd = self.kbd_client_fd;
      let prev_surf = self.kbd_surface_id;
      let kbd_id = self.clients.get(&prev_fd).and_then(|c| {
        c.objects.iter().find_map(|(&id, o)| matches!(o.role, Role::Keyboard).then_some(id))
      }).unwrap_or(0);
      if kbd_id != 0 {
        let serial = self.next_serial();
        if let Some(client) = self.clients.get_mut(&prev_fd) {
          // wl_keyboard.leave = opcode 2
          client.send(kbd_id, 2, &[Arg::Uint(serial), Arg::Object(prev_surf)]);
          client.conn.flush();
        }
      }
      self.kbd_entered = false;
    }
    let kbd_id = match self.clients.get(&fd) {
      Some(c) => c.objects.iter().find(|(_, o)| matches!(o.role, Role::Keyboard)).map(|(&id, _)| id).unwrap_or(0),
      None => return,
    };
    if kbd_id == 0 {
      return;
    }
    let serial = self.next_serial();
    let keys: Vec<u8> = Vec::new();
    let client = self.clients.get_mut(&fd).unwrap();
    client.send(kbd_id, 1, &[Arg::Uint(serial), Arg::Object(surf_id), Arg::Array(keys)]);
    client.conn.flush();
    self.kbd_entered = true;
    self.kbd_client_fd = fd;
    self.kbd_surface_id = surf_id;
    // Re-offer clipboard + PRIMARY on keyboard enter.  GTK caches selection
    // offers per-focus; without this, paste after Alt-Tab / click-to-focus
    // often fails until the user copies again.
    self.emit_selection_to_client_fd(fd);
    self.emit_primary_to_client_fd(fd);
  }

  fn update_text_input_focus(&mut self, fd: RawFd, surf_id: u32) {
    let unchanged = self.active_text_input.map(|(active_fd, _, active_surface)| active_fd == fd && active_surface == surf_id).unwrap_or(false);
    if !unchanged && self.active_text_input.is_some() {
      self.active_text_input = None;
      self.deactivate_input_methods(None);
    }

    let client_fds: Vec<RawFd> = self.clients.keys().copied().collect();
    for client_fd in client_fds {
      let mut leaves = Vec::new();
      let mut enters = Vec::new();
      let mut activate = Vec::new();
      if let Some(client) = self.clients.get_mut(&client_fd) {
        for (&id, obj) in &mut client.objects {
          if let Role::TextInput {
            surface_id,
            enabled,
            ..
          } = &mut obj.role
          {
            let new_surface = (client_fd == fd).then_some(surf_id);
            if *surface_id != new_surface {
              if let Some(old) = *surface_id {
                leaves.push((id, old));
              }
              *surface_id = new_surface;
              if let Some(new_id) = new_surface {
                enters.push((id, new_id));
                if *enabled {
                  activate.push(id);
                }
              }
            }
          }
        }
        for (id, surface) in leaves {
          client.send(id, 1, &[Arg::Object(surface)]);
        }
        for (id, surface) in enters {
          client.send(id, 0, &[Arg::Object(surface)]);
        }
      }
      if client_fd == fd {
        for id in activate {
          if let Some(target) = self.clients.remove(&client_fd) {
            self.active_text_input = Some((client_fd, id, surf_id));
            self.activate_input_methods(&target, id);
            self.clients.insert(client_fd, target);
          }
        }
      }
    }
  }

  fn clear_text_input_focus(&mut self) {
    if self.active_text_input.take().is_some() {
      self.deactivate_input_methods(None);
    }
    for client in self.clients.values_mut() {
      let mut leaves = Vec::new();
      for (&id, obj) in &mut client.objects {
        if let Role::TextInput { surface_id, .. } = &mut obj.role {
          if let Some(old) = surface_id.take() {
            leaves.push((id, old));
          }
        }
      }
      for (id, surface_id) in leaves {
        client.send(id, 1, &[Arg::Object(surface_id)]);
      }
    }
  }
}

impl Drop for Server {
  fn drop(&mut self) {
    self.backend.shutdown();
    unsafe {
      libc::close(self.epoll_fd);
      libc::close(self.signal_fd);
      if self.dmabuf_format_table >= 0 {
        libc::close(self.dmabuf_format_table);
      }
      if self.input_wake_fd >= 0 {
        libc::close(self.input_wake_fd);
      }
    }
  }
}

/// Build an XKB keymap text blob from RMLVO-ish env / overrides.
/// Clients compile the includes via libxkbcommon (same approach as the
/// previous hardcoded `pc+us` keymap, but multilingual).
fn build_xkb_keymap(layout_ov: Option<&str>, variant_ov: Option<&str>, options_ov: Option<&str>) -> Vec<u8> {
  let layout = layout_ov
    .map(|s| s.to_string())
    .or_else(|| std::env::var("XKB_DEFAULT_LAYOUT").ok())
    .filter(|s| !s.is_empty())
    .unwrap_or_else(default_xkb_layout);
  let variant = variant_ov
    .map(|s| s.to_string())
    .or_else(|| std::env::var("XKB_DEFAULT_VARIANT").ok())
    .unwrap_or_default();
  let options = options_ov
    .map(|s| s.to_string())
    .or_else(|| std::env::var("XKB_DEFAULT_OPTIONS").ok())
    .unwrap_or_else(|| {
      if layout.contains(',') {
        "grp:alt_shift_toggle".to_string()
      } else {
        String::new()
      }
    });
  let symbols = compose_xkb_symbols(&layout, &variant, &options);
  let aliases = "evdev+aliases(qwerty)";
  let text = format!(
    "xkb_keymap {{\n\
     \x20\x20xkb_keycodes  \"{aliases}\" {{ include \"{aliases}\" }};\n\
     \x20\x20xkb_types     \"complete\" {{ include \"complete\" }};\n\
     \x20\x20xkb_compat    \"complete\" {{ include \"complete\" }};\n\
     \x20\x20xkb_symbols   \"{symbols}\" {{ include \"{symbols}\" }};\n\
     \x20\x20xkb_geometry  \"pc(pc105)\" {{ include \"pc(pc105)\" }};\n\
     }};\n\0",
    aliases = aliases,
    symbols = symbols,
  );
  eprintln!(
    "[luna-compositor] xkb layout={} variant={} options={} → {}",
    layout, variant, options, symbols
  );
  text.into_bytes()
}

fn default_xkb_layout() -> String {
  let lang = std::env::var("LC_ALL")
    .or_else(|_| std::env::var("LC_CTYPE"))
    .or_else(|_| std::env::var("LANG"))
    .unwrap_or_default()
    .to_lowercase();
  // Multilingual defaults: primary layout from locale + US as group 2.
  if lang.starts_with("ja") {
    "jp,us".into()
  } else if lang.starts_with("ko") {
    "kr,us".into()
  } else if lang.starts_with("zh_cn") || lang.starts_with("zh_sg") {
    "cn,us".into()
  } else if lang.starts_with("zh_tw") || lang.starts_with("zh_hk") {
    "tw,us".into()
  } else if lang.starts_with("de") {
    "de,us".into()
  } else if lang.starts_with("fr") {
    "fr,us".into()
  } else if lang.starts_with("es") {
    "es,us".into()
  } else if lang.starts_with("pt_br") {
    "br,us".into()
  } else if lang.starts_with("ru") {
    "us,ru".into()
  } else if lang.starts_with("ar") {
    "us,ara".into()
  } else {
    "us".into()
  }
}

/// Compose an xkb_symbols include string from comma-separated layouts/variants
/// and colon-style options (`grp:alt_shift_toggle` → `+group(alt_shift_toggle)`).
fn compose_xkb_symbols(layout: &str, variant: &str, options: &str) -> String {
  let layouts: Vec<&str> = layout.split(',').map(|s| s.trim()).filter(|s| !s.is_empty()).collect();
  let variants: Vec<&str> = variant.split(',').map(|s| s.trim()).collect();
  let mut sym = String::from("pc");
  for (i, lay) in layouts.iter().enumerate() {
    let var = variants.get(i).copied().unwrap_or("");
    if i == 0 {
      if var.is_empty() {
        sym.push_str(&format!("+{}", lay));
      } else {
        sym.push_str(&format!("+{}({})", lay, var));
      }
    } else if var.is_empty() {
      sym.push_str(&format!("+{}:{}", lay, i + 1));
    } else {
      sym.push_str(&format!("+{}({}):{}", lay, var, i + 1));
    }
  }
  if layouts.is_empty() {
    sym.push_str("+us");
  }
  sym.push_str("+inet(evdev)");
  for opt in options.split(',') {
    let opt = opt.trim();
    if opt.is_empty() {
      continue;
    }
    if let Some((name, arg)) = opt.split_once(':') {
      // grp:alt_shift_toggle → group(alt_shift_toggle); caps:escape → caps(escape)
      let comp = match name {
        "grp" => "group",
        other => other,
      };
      sym.push_str(&format!("+{}({})", comp, arg));
    } else {
      sym.push_str(&format!("+{}", opt));
    }
  }
  sym
}

/// True if the SHM buffer has at least one roughly-opaque pixel.
/// Used to detect GTK cursor surfaces that are still empty/transparent
/// after set_cursor (theme load race) so we can fall back to the default arrow.
fn shm_buffer_has_visible_pixel(buf: &crate::shm::ShmBuffer) -> bool {
  buf.begin_cpu_read();
  let step_x = (buf.width / 8).max(1);
  let step_y = (buf.height / 8).max(1);
  let mut found = false;
  'outer: for y in (0..buf.height).step_by(step_y as usize) {
    for x in (0..buf.width).step_by(step_x as usize) {
      if let Some(px) = buf.pixel(x, y) {
        if (px >> 24) & 0xff > 8 {
          found = true;
          break 'outer;
        }
      }
    }
  }
  buf.end_cpu_read();
  found
}

// Compute screen-space rect for a layer shell surface.
// Anchor bits: TOP=1, BOTTOM=2, LEFT=4, RIGHT=8
fn layer_surface_rect(bw: u32, bh: u32, anchor: u32, size_w: u32, size_h: u32, mt: i32, mr: i32, mb: i32, ml: i32) -> (i32, i32, u32, u32) {
  const A_TOP: u32 = 1; const A_BOT: u32 = 2; const A_LEFT: u32 = 4; const A_RIGHT: u32 = 8;
  let h_stretch = (anchor & A_TOP != 0) && (anchor & A_BOT != 0);
  let v_stretch = (anchor & A_LEFT != 0) && (anchor & A_RIGHT != 0);
  let w = if v_stretch { bw.saturating_sub((ml + mr).max(0) as u32) } else if size_w > 0 { size_w } else { bw };
  let h = if h_stretch { bh.saturating_sub((mt + mb).max(0) as u32) } else if size_h > 0 { size_h } else { bh };
  let x = if (anchor & A_LEFT != 0) && (anchor & A_RIGHT == 0) { ml }
           else if (anchor & A_RIGHT != 0) && (anchor & A_LEFT == 0) { bw as i32 - w as i32 - mr }
           else { ml + (bw as i32 - ml - mr - w as i32) / 2 };
  let y = if (anchor & A_TOP != 0) && (anchor & A_BOT == 0) { mt }
           else if (anchor & A_BOT != 0) && (anchor & A_TOP == 0) { bh as i32 - h as i32 - mb }
           else { mt + (bh as i32 - mt - mb - h as i32) / 2 };
  (x, y, w, h)
}

/// Compute an xdg_popup position (parent-surface-local top-left) honouring the
/// positioner's anchor, gravity, offset and constraint_adjustment.
///
/// `parent_ox`/`parent_oy` are the parent surface's on-screen top-left, used
/// together with `output_w`/`output_h` to keep the popup on screen via
/// flip/slide when the client permits it. Returns `(rel_x, rel_y)` relative to
/// the parent surface, which is exactly what `xdg_popup.configure` expects.
#[allow(clippy::too_many_arguments)]
fn popup_rel_position(
  anchor_x: i32, anchor_y: i32, anchor_w: i32, anchor_h: i32,
  offset_x: i32, offset_y: i32,
  pw: i32, ph: i32,
  anchor: u32, gravity: u32, constraint: u32,
  parent_ox: i32, parent_oy: i32,
  output_w: i32, output_h: i32,
) -> (i32, i32) {
  // xdg_positioner.anchor / .gravity enums decomposed into axis components:
  // -1 = left/top edge, 0 = center, +1 = right/bottom edge.
  //   none=0 top=1 bottom=2 left=3 right=4
  //   top_left=5 bottom_left=6 top_right=7 bottom_right=8
  fn h_of(e: u32) -> i32 {
    match e { 3 | 5 | 6 => -1, 4 | 7 | 8 => 1, _ => 0 }
  }
  fn v_of(e: u32) -> i32 {
    match e { 1 | 5 | 7 => -1, 2 | 6 | 8 => 1, _ => 0 }
  }

  // Place the popup's top-left from the chosen anchor/gravity components.
  let place = |ah: i32, av: i32, gh: i32, gv: i32| -> (i32, i32) {
    // Anchor point on the anchor rect (parent-local).
    let ax = anchor_x + match ah { -1 => 0, 1 => anchor_w, _ => anchor_w / 2 };
    let ay = anchor_y + match av { -1 => 0, 1 => anchor_h, _ => anchor_h / 2 };
    // Gravity decides which way the popup extends from the anchor point.
    let x = ax + match gh { -1 => -pw, 1 => 0, _ => -pw / 2 } + offset_x;
    let y = ay + match gv { -1 => -ph, 1 => 0, _ => -ph / 2 } + offset_y;
    (x, y)
  };

  let ah = h_of(anchor);
  let av = v_of(anchor);
  let gh = h_of(gravity);
  let gv = v_of(gravity);

  let (mut rx, mut ry) = place(ah, av, gh, gv);

  const SLIDE_X: u32 = 1;
  const SLIDE_Y: u32 = 2;
  const FLIP_X: u32 = 4;
  const FLIP_Y: u32 = 8;

  // Flip to the opposite side when the popup would overflow and the client
  // allows flipping — this is what makes submenus/combos land correctly.
  if constraint & FLIP_X != 0 {
    let abs = parent_ox + rx;
    if abs < 0 || abs + pw > output_w {
      let (fx, _) = place(-ah, av, -gh, gv);
      let fabs = parent_ox + fx;
      if fabs >= 0 && fabs + pw <= output_w {
        rx = fx;
      }
    }
  }
  if constraint & FLIP_Y != 0 {
    let abs = parent_oy + ry;
    if abs < 0 || abs + ph > output_h {
      let (_, fy) = place(ah, -av, gh, -gv);
      let fabs = parent_oy + fy;
      if fabs >= 0 && fabs + ph <= output_h {
        ry = fy;
      }
    }
  }

  // Slide back onto the output when still constrained and sliding is permitted.
  if constraint & SLIDE_X != 0 {
    if parent_ox + rx + pw > output_w { rx = output_w - pw - parent_ox; }
    if parent_ox + rx < 0 { rx = -parent_ox; }
  }
  if constraint & SLIDE_Y != 0 {
    if parent_oy + ry + ph > output_h { ry = output_h - ph - parent_oy; }
    if parent_oy + ry < 0 { ry = -parent_oy; }
  }

  (rx, ry)
}

fn role_for(iface: &str) -> Role {
  match iface {
    "wl_compositor" => Role::Compositor,
    "wl_subcompositor" => Role::Subcompositor,
    "wl_shm" => Role::Shm,
    "wl_seat" => Role::Seat,
    "wl_output" => Role::Output,
    "wl_data_device_manager" => Role::DataDeviceManager,
    "zwp_primary_selection_device_manager_v1" => Role::PrimarySelectionDeviceManager,
    "zxdg_decoration_manager_v1" => Role::DecorationManager,
    "zwp_text_input_manager_v3" => Role::TextInputManager,
    "zwp_input_method_manager_v2" => Role::InputMethodManager,
    "zwp_virtual_keyboard_manager_v1" => Role::VirtualKeyboardManager,
    "xdg_wm_base" => Role::WmBase,
    "zwp_linux_dmabuf_v1" => Role::Dmabuf,
    "zwlr_layer_shell_v1" => Role::LayerShell,
    _ => Role::Display,
  }
}

fn now_ms() -> u32 { SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_millis() as u32).unwrap_or(0) }

/// format_table layout: { u32 format; u32 pad; u64 modifier }[]
fn create_format_table() -> (RawFd, usize) {
  let entries: [(u32, u64); 2] = [(DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR), (DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR)];
  let mut data = Vec::with_capacity(entries.len() * 16);
  for (fmt, modi) in entries {
    data.extend_from_slice(&fmt.to_ne_bytes());
    data.extend_from_slice(&0u32.to_ne_bytes());
    data.extend_from_slice(&modi.to_ne_bytes());
  }

  let name = CString::new("vespera-dmabuf").unwrap();
  let fd = unsafe { libc::memfd_create(name.as_ptr(), libc::MFD_CLOEXEC | libc::MFD_ALLOW_SEALING) };
  if fd < 0 {
    return (-1, 0);
  }
  let mut off = 0usize;
  while off < data.len() {
    let n = unsafe { libc::write(fd, data[off..].as_ptr() as *const libc::c_void, data.len() - off) };
    if n <= 0 {
      break;
    }
    off += n as usize;
  }
  // Seal memfd read-only for client mmap.
  unsafe { libc::fcntl(fd, libc::F_ADD_SEALS, libc::F_SEAL_SHRINK | libc::F_SEAL_GROW | libc::F_SEAL_WRITE | libc::F_SEAL_SEAL) };
  (fd, data.len())
}

/// Pick the `dev_t` advertised as `zwp_linux_dmabuf_v1.main_device`.
///
/// Dmabuf is **off by default**.  This compositor CPU-mmaps client buffers and
/// only accepts LINEAR AR24/XR24; Mesa OpenGL clients (luna-shell) allocate
/// tiled GPU buffers, and advertising dmabuf then sends them down a path that
/// ends in `dri2_query_image(NULL)` — a segfault inside libgallium on the first
/// `eglSwapBuffers()`.  `wl_shm` is the supported path for GL clients.
///
/// Order of preference when dmabuf is enabled:
///  1. `LUNA_DISABLE_DMABUF=1` — hard opt-out (wins over ENABLE).
///  2. Otherwise require `LUNA_ENABLE_DMABUF=1` (GTK linear allocators).
///  3. `LUNA_DRM_DEVICE=/dev/dri/renderD129` — explicit override.
///  4. The render node of the card the backend actually drives.
///  5. The first *render* node that can be opened.
///
/// Only render nodes are ever reported.  The previous version fell back to
/// `/dev/dri/card0`, i.e. a primary node on a fixed, possibly wrong GPU; Mesa
/// cannot render on a primary node it is not DRM master of, with the same
/// NULL-image segfault as above.
fn detect_drm_device(backend_dev: Option<u64>) -> Option<u64> {
  if env_flag("LUNA_DISABLE_DMABUF") {
    eprintln!("[luna-compositor] LUNA_DISABLE_DMABUF set; dmabuf disabled");
    return None;
  }
  if !env_flag("LUNA_ENABLE_DMABUF") {
    eprintln!(
      "[luna-compositor] dmabuf disabled (default); set LUNA_ENABLE_DMABUF=1 to enable"
    );
    return None;
  }

  if let Ok(path) = std::env::var("LUNA_DRM_DEVICE") {
    match probe_render_node(&path) {
      Some(dev) => {
        eprintln!("[luna-compositor] dmabuf main_device = {} (LUNA_DRM_DEVICE)", path);
        return Some(dev);
      }
      None => eprintln!("[luna-compositor] LUNA_DRM_DEVICE={} unusable; ignoring", path),
    }
  }

  if backend_dev.is_some() {
    return backend_dev;
  }

  for n in 128..192 {
    let path = format!("/dev/dri/renderD{}", n);
    if let Some(dev) = probe_render_node(&path) {
      eprintln!("[luna-compositor] dmabuf main_device = {}", path);
      return Some(dev);
    }
  }
  None
}

fn env_flag(name: &str) -> bool {
  match std::env::var(name) {
    Ok(v) => !v.is_empty() && v != "0" && !v.eq_ignore_ascii_case("false"),
    Err(_) => false,
  }
}

fn create_signal_fd() -> std::io::Result<RawFd> {
  unsafe {
    let mut mask: libc::sigset_t = std::mem::zeroed();
    libc::sigemptyset(&mut mask);
    for signal in [libc::SIGINT, libc::SIGTERM, libc::SIGHUP, libc::SIGQUIT, libc::SIGUSR1, libc::SIGUSR2] {
      libc::sigaddset(&mut mask, signal);
    }
    if libc::pthread_sigmask(libc::SIG_BLOCK, &mask, std::ptr::null_mut()) != 0 {
      return Err(std::io::Error::last_os_error());
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
  fn format_table_can_be_transferred_repeatedly() {
    let (fd, size) = create_format_table();
    assert!(fd >= 0);
    assert_eq!(size, 32);

    let first = unsafe { libc::fcntl(fd, libc::F_DUPFD_CLOEXEC, 0) };
    let second = unsafe { libc::fcntl(fd, libc::F_DUPFD_CLOEXEC, 0) };
    assert!(first >= 0 && second >= 0);
    assert_ne!(first, second);

    unsafe {
      libc::close(first);
      assert!(libc::fcntl(fd, libc::F_GETFD) >= 0);
      libc::close(second);
      assert!(libc::fcntl(fd, libc::F_GETFD) >= 0);
      libc::close(fd);
    }
  }

  // xdg_positioner enum helpers used below.
  const A_BOTTOM_LEFT: u32 = 6;
  const G_BOTTOM_RIGHT: u32 = 8;
  const A_TOP_RIGHT: u32 = 7;
  const G_BOTTOM_LEFT: u32 = 6;
  const A_TOP_LEFT: u32 = 5;
  const G_TOP_LEFT: u32 = 5;

  #[test]
  fn popup_menu_bar_dropdown_drops_below_anchor() {
    // Anchor rect = a menu-bar item at (10,0) size 40x20.
    // GTK menu: anchor=bottom_left, gravity=bottom_right → drop straight down.
    let (rx, ry) = popup_rel_position(
      10, 0, 40, 20, /*offset*/ 0, 0, /*popup*/ 120, 200,
      A_BOTTOM_LEFT, G_BOTTOM_RIGHT, /*constraint*/ 0,
      /*parent origin*/ 0, 0, /*output*/ 1920, 1080,
    );
    assert_eq!((rx, ry), (10, 20));
  }

  #[test]
  fn popup_submenu_flies_out_to_the_right() {
    // Submenu: anchor=top_right, gravity=bottom_right → appears to the right,
    // aligned with the top of the parent item.
    let (rx, ry) = popup_rel_position(
      0, 0, 150, 24, 0, 0, 150, 300,
      A_TOP_RIGHT, G_BOTTOM_RIGHT, 0,
      0, 0, 1920, 1080,
    );
    assert_eq!((rx, ry), (150, 0));
  }

  #[test]
  fn popup_flip_y_when_it_would_overflow_bottom() {
    // Popup would drop below the output edge; flip_y should place it above the
    // anchor instead. Parent bottom edge near the screen bottom.
    let (_rx, ry) = popup_rel_position(
      0, 0, 100, 20, 0, 0, 100, 300,
      A_BOTTOM_LEFT, G_BOTTOM_RIGHT, /*flip_y*/ 8,
      0, /*parent_oy*/ 1000, 1920, 1080,
    );
    // Down would be y=20 → abs 1020 (+300 overflows 1080). Flipped: anchor top
    // (y=0) with upward gravity → rel_y = -popup_h = -300.
    assert_eq!(ry, -300);
  }

  #[test]
  fn popup_offset_is_applied() {
    let (rx, ry) = popup_rel_position(
      0, 0, 40, 20, /*offset*/ 5, 7, 100, 100,
      A_TOP_LEFT, G_TOP_LEFT, 0,
      0, 0, 1920, 1080,
    );
    // anchor top-left (0,0), gravity top-left → top-left = anchor - popup size.
    assert_eq!((rx, ry), (-100 + 5, -100 + 7));
  }
}
