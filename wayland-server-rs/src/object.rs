/*
 * Copyright © 2026 Yuichiro Nakada / Project Vespera
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

use crate::protocol::Interface;
use crate::shm::{ShmBuffer, ShmPool};
use std::os::unix::io::RawFd;
use std::rc::Rc;

pub struct DmabufPlane {
  pub fd: RawFd,
  pub plane_idx: u32,
  pub offset: u32,
  pub stride: u32,
  pub modifier: u64,
}

#[derive(Default)]
pub struct DmabufParams {
  pub planes: Vec<DmabufPlane>,
  /// create / create_immed may run only once
  pub used: bool,
}

impl Drop for DmabufParams {
  fn drop(&mut self) {
    // Close unconsumed fds.
    for p in &self.planes {
      unsafe { libc::close(p.fd) };
    }
  }
}

pub struct Surface {
  pub pending_buffer: Option<u32>,
  pub pending_attach: bool,
  pub current_buffer: Option<ShmBuffer>,
  /// Object id of `current_buffer`.  The compositor must retain this wl_buffer
  /// until it has stopped reading the shared storage.
  pub current_buffer_id: Option<u32>,
  pub x: i32,
  pub y: i32,
  pub frame_callbacks: Vec<u32>,
  pub xdg_surface_id: Option<u32>,
  pub mapped: bool,
  /// We have sent wl_surface.enter for the compositor's single output.
  pub output_entered: bool,
  pub popup: bool,
  pub input_method_popup: bool,
  // Layer shell
  pub layer_surface_id: Option<u32>,
  /// Surface-local input region from `wl_surface.set_input_region`.
  /// `None` = entire surface (default / NULL region).  `Some(rects)` limits
  /// hits to the union of those rectangles; an empty vec accepts no input.
  /// luna-shell modeless dialogs rely on this so their full-screen overlay
  /// stays click-through outside the dialog chrome.
  pub input_region: Option<Vec<(i32, i32, i32, i32)>>,
  /// xdg_surface.set_window_geometry — content box relative to buffer origin.
  /// None → whole buffer. GTK CSD uses this to exclude drop-shadow padding.
  pub window_geom: Option<(i32, i32, i32, i32)>,
  /// When this surface is a wl_subsurface child, parent wl_surface id.
  /// Firefox / WebRender put content on subsurfaces of an empty toplevel.
  pub subsurface_parent: Option<u32>,
  /// Damage posted since the last commit, in surface-local coordinates, as a
  /// bounding box.  `wl_surface.damage` and `damage_buffer` both land here; we
  /// do not track scale/transform, so the two are equivalent for us.
  pub pending_damage: crate::render::Rect,
  /// Damage carried by the most recent commit, consumed by the next composite.
  /// This is what lets the compositor repaint a blinking terminal cursor
  /// instead of the entire desktop underneath it.
  pub damage: crate::render::Rect,
}

pub enum Role {
  Display,
  Registry,
  Callback,
  Compositor,
  Subcompositor,
  Subsurface {
    /// Child wl_surface id.
    surface_id: u32,
    /// Parent wl_surface id.
    parent_id: u32,
    /// Position relative to parent buffer origin.
    x: i32,
    y: i32,
    /// Sibling stack rank; negative values are below the parent surface.
    z: i32,
    /// true = commit synchronized with parent (we still present immediately).
    sync: bool,
  },
  Shm,
  Output,
  Seat,
  Pointer,
  Keyboard,
  TextInputManager,
  TextInput {
    seat_id: u32,
    surface_id: Option<u32>,
    enabled: bool,
    pending_enabled: Option<bool>,
    surrounding_text: String,
    cursor: i32,
    anchor: i32,
    text_change_cause: u32,
    content_hint: u32,
    content_purpose: u32,
    cursor_rect: (i32, i32, i32, i32),
    commit_serial: u32,
  },
  InputMethodManager,
  InputMethod {
    seat_id: u32,
    pending_commit: Option<String>,
    pending_preedit: Option<(String, i32, i32)>,
    pending_delete: Option<(u32, u32)>,
  },
  InputPopupSurface {
    surface_id: u32,
    input_method_id: u32,
  },
  InputMethodKeyboardGrab {
    input_method_id: u32,
  },
  VirtualKeyboardManager,
  VirtualKeyboard {
    seat_id: u32,
    keymap_set: bool,
  },
  DataDeviceManager,
  DataDevice {
    seat_id: u32,
  },
  DataSource {
    mime_types: Vec<String>,
  },
  DataOffer {
    /// Source lives on `source_fd` as object `source_id` (0 = none).
    source_fd: RawFd,
    source_id: u32,
    mime_types: Vec<String>,
  },
  PrimarySelectionDeviceManager,
  PrimarySelectionDevice {
    seat_id: u32,
  },
  PrimarySelectionSource {
    mime_types: Vec<String>,
  },
  PrimarySelectionOffer {
    source_fd: RawFd,
    source_id: u32,
    mime_types: Vec<String>,
  },
  WmBase,
  Positioner {
    size_w: i32,
    size_h: i32,
    anchor_x: i32,
    anchor_y: i32,
    anchor_w: i32,
    anchor_h: i32,
    offset_x: i32,
    offset_y: i32,
    /// xdg_positioner.anchor enum (none=0, top=1, bottom=2, left=3, right=4,
    /// top_left=5, bottom_left=6, top_right=7, bottom_right=8)
    anchor: u32,
    /// xdg_positioner.gravity enum (same numbering as anchor)
    gravity: u32,
    /// xdg_positioner.constraint_adjustment bitmask
    /// (slide_x=1, slide_y=2, flip_x=4, flip_y=8, resize_x=16, resize_y=32)
    constraint_adjustment: u32,
  },
  Region { rects: Vec<(i32, i32, i32, i32)> },
  LayerShell,
  LayerSurface {
    surface_id: u32,
    layer: u32,           // 0=BACKGROUND, 1=BOTTOM, 2=TOP, 3=OVERLAY
    anchor: u32,          // bitmask: TOP=1, BOTTOM=2, LEFT=4, RIGHT=8
    exclusive_zone: i32,
    size_w: u32,
    size_h: u32,
    margin_top: i32,
    margin_right: i32,
    margin_bottom: i32,
    margin_left: i32,
    keyboard: u32,
    configure_serial: u32,
    configured: bool,
  },
  Dmabuf,
  DmabufParams(DmabufParams),
  DmabufFeedback,
  ShmPool {
    pool: Option<Rc<ShmPool>>,
  },
  Buffer(ShmBuffer),
  Surface(Surface),
  XdgSurface {
    surface_id: u32,
    configured: bool,
  },
  XdgToplevel {
    xdg_surface_id: u32,
    title: String,
    app_id: String,
    minimized: bool,
    maximized: bool,
    fullscreen: bool,
    /// Parent wl_surface id (transient dialogs), if any.
    parent_surface_id: Option<u32>,
    /// Geometry before maximize / fullscreen / tile: (x, y, w, h).
    saved_geom: Option<(i32, i32, i32, i32)>,
    /// 0=none, 1=left half, 2=right half.
    tiled: u32,
    /// zxdg_toplevel_decoration mode: 0=unset, 1=client, 2=server.
    decoration_mode: u32,
    /// Client-requested size clamp (0 = unset).
    min_w: i32,
    min_h: i32,
    max_w: i32,
    max_h: i32,
  },
  XdgPopup {
    xdg_surface_id: u32,
  },
  DecorationManager,
  ToplevelDecoration {
    toplevel_id: u32,
    mode: u32, // 0=unset, 1=client, 2=server
  },
}

pub struct Object {
  pub interface: &'static Interface,
  pub version: u32,
  pub role: Role,
}

impl Object {
  pub fn new(interface: &'static Interface, version: u32, role: Role) -> Self {
    Object {
      interface,
      version,
      role,
    }
  }
}

impl Default for Surface {
  fn default() -> Self {
    Surface {
      pending_buffer: None,
      pending_attach: false,
      current_buffer: None,
      current_buffer_id: None,
      x: 0,
      y: 0,
      frame_callbacks: Vec::new(),
      xdg_surface_id: None,
      mapped: false,
      output_entered: false,
      popup: false,
      input_method_popup: false,
      layer_surface_id: None,
      input_region: None,
      window_geom: None,
      subsurface_parent: None,
      pending_damage: crate::render::Rect::EMPTY,
      damage: crate::render::Rect::EMPTY,
    }
  }
}
