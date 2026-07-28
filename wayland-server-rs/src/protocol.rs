/*
 * Copyright © 2026 Yuichiro Nakada / Project Vespera
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

pub struct Msg {
  pub name: &'static str,
  pub signature: &'static str,
}

macro_rules! m {
  ($name:expr, $sig:expr) => {
    Msg {
      name: $name,
      signature: $sig,
    }
  };
}

pub struct Interface {
  pub name: &'static str,
  pub version: u32,
  pub requests: &'static [Msg],
  pub events: &'static [Msg],
}

impl Interface {
  pub fn request_sig(&self, opcode: u16) -> Option<&'static str> { self.requests.get(opcode as usize).map(|m| m.signature) }
  pub fn request_name(&self, opcode: u16) -> &'static str { self.requests.get(opcode as usize).map(|m| m.name).unwrap_or("?") }
}

pub static WL_DISPLAY: Interface = Interface {
  name: "wl_display",
  version: 1,
  requests: &[m!("sync", "n"), m!("get_registry", "n")],
  events: &[m!("error", "ous"), m!("delete_id", "u")],
};

pub static WL_REGISTRY: Interface = Interface {
  name: "wl_registry",
  version: 1,
  requests: &[m!("bind", "usun")],
  events: &[m!("global", "usu"), m!("global_remove", "u")],
};

pub static WL_CALLBACK: Interface = Interface {
  name: "wl_callback",
  version: 1,
  requests: &[],
  events: &[m!("done", "u")],
};

pub static WL_COMPOSITOR: Interface = Interface {
  name: "wl_compositor",
  version: 6,
  requests: &[m!("create_surface", "n"), m!("create_region", "n")],
  events: &[],
};

pub static WL_SHM_POOL: Interface = Interface {
  name: "wl_shm_pool",
  version: 1,
  requests: &[m!("create_buffer", "niiiiu"), m!("destroy", ""), m!("resize", "i")],
  events: &[],
};

pub static WL_SHM: Interface = Interface {
  name: "wl_shm",
  version: 1,
  requests: &[m!("create_pool", "nhi")],
  events: &[m!("format", "u")],
};

pub static WL_BUFFER: Interface = Interface {
  name: "wl_buffer",
  version: 1,
  requests: &[m!("destroy", "")],
  events: &[m!("release", "")],
};

pub static WL_SURFACE: Interface = Interface {
  name: "wl_surface",
  version: 6,
  requests: &[m!("destroy", ""), m!("attach", "?oii"), m!("damage", "iiii"), m!("frame", "n"), m!("set_opaque_region", "?o"), m!("set_input_region", "?o"), m!("commit", ""), m!("set_buffer_transform", "i"), m!("set_buffer_scale", "i"), m!("damage_buffer", "iiii"), m!("offset", "ii")],
  events: &[m!("enter", "o"), m!("leave", "o")],
};

pub static WL_REGION: Interface = Interface {
  name: "wl_region",
  version: 1,
  requests: &[m!("destroy", ""), m!("add", "iiii"), m!("subtract", "iiii")],
  events: &[],
};

pub static WL_SUBSURFACE: Interface = Interface {
  name: "wl_subsurface",
  version: 1,
  requests: &[m!("destroy", ""), m!("set_position", "ii"), m!("place_above", "o"), m!("place_below", "o"), m!("set_sync", ""), m!("set_desync", "")],
  events: &[],
};

pub static WL_SUBCOMPOSITOR: Interface = Interface {
  name: "wl_subcompositor",
  version: 1,
  requests: &[m!("destroy", ""), m!("get_subsurface", "noo")],
  events: &[],
};

pub static WL_SEAT: Interface = Interface {
  name: "wl_seat",
  version: 7,
  requests: &[m!("get_pointer", "n"), m!("get_keyboard", "n"), m!("get_touch", "n"), m!("release", "")],
  events: &[m!("capabilities", "u"), m!("name", "s")],
};

pub static WL_POINTER: Interface = Interface {
  name: "wl_pointer",
  version: 7,
  requests: &[m!("set_cursor", "u?oii"), m!("release", "")],
  events: &[m!("enter", "uoff"), m!("leave", "uo"), m!("motion", "uff"), m!("button", "uuuu"), m!("axis", "uuf"), m!("frame", ""), m!("axis_source", "u"), m!("axis_stop", "uu"), m!("axis_discrete", "ui")],
};

pub static WL_KEYBOARD: Interface = Interface {
  name: "wl_keyboard",
  version: 7,
  requests: &[m!("release", "")],
  events: &[m!("keymap", "uhu"), m!("enter", "uoa"), m!("leave", "uo"), m!("key", "uuuu"), m!("modifiers", "uuuuu"), m!("repeat_info", "ii")],
};

pub static WL_OUTPUT: Interface = Interface {
  name: "wl_output",
  version: 4,
  requests: &[m!("release", "")],
  events: &[m!("geometry", "iiiiissi"), m!("mode", "uiii"), m!("done", ""), m!("scale", "i"), m!("name", "s"), m!("description", "s")],
};

pub static WL_DATA_DEVICE_MANAGER: Interface = Interface {
  name: "wl_data_device_manager",
  version: 3,
  requests: &[m!("create_data_source", "n"), m!("get_data_device", "no")],
  events: &[],
};

pub static WL_DATA_SOURCE: Interface = Interface {
  name: "wl_data_source",
  version: 3,
  requests: &[m!("offer", "s"), m!("destroy", ""), m!("set_actions", "3u")],
  events: &[
    m!("target", "?s"),
    m!("send", "sh"),
    m!("cancelled", ""),
    m!("dnd_drop_performed", "3"),
    m!("dnd_finished", "3"),
    m!("action", "3u"),
  ],
};

pub static WL_DATA_DEVICE: Interface = Interface {
  name: "wl_data_device",
  version: 3,
  requests: &[m!("start_drag", "?oo?ou"), m!("set_selection", "?ou"), m!("release", "2")],
  events: &[
    m!("data_offer", "n"),
    m!("enter", "uoff?o"),
    m!("leave", ""),
    m!("motion", "uff"),
    m!("drop", ""),
    m!("selection", "?o"),
  ],
};

pub static WL_DATA_OFFER: Interface = Interface {
  name: "wl_data_offer",
  version: 3,
  requests: &[
    m!("accept", "u?s"),
    m!("receive", "sh"),
    m!("destroy", ""),
    m!("finish", "3"),
    m!("set_actions", "3uu"),
  ],
  events: &[m!("offer", "s"), m!("source_actions", "3u"), m!("action", "3u")],
};

/// Middle-click (PRIMARY) paste — GTK4/Qt use this instead of X11 PRIMARY.
pub static ZWP_PRIMARY_SELECTION_DEVICE_MANAGER_V1: Interface = Interface {
  name: "zwp_primary_selection_device_manager_v1",
  version: 1,
  requests: &[
    m!("create_source", "n"),
    m!("get_device", "no"),
    m!("destroy", ""),
  ],
  events: &[],
};

pub static ZWP_PRIMARY_SELECTION_DEVICE_V1: Interface = Interface {
  name: "zwp_primary_selection_device_v1",
  version: 1,
  requests: &[m!("set_selection", "?ou"), m!("destroy", "")],
  events: &[m!("data_offer", "n"), m!("selection", "?o")],
};

pub static ZWP_PRIMARY_SELECTION_SOURCE_V1: Interface = Interface {
  name: "zwp_primary_selection_source_v1",
  version: 1,
  requests: &[m!("offer", "s"), m!("destroy", "")],
  events: &[m!("send", "sh"), m!("cancelled", "")],
};

pub static ZWP_PRIMARY_SELECTION_OFFER_V1: Interface = Interface {
  name: "zwp_primary_selection_offer_v1",
  version: 1,
  requests: &[m!("receive", "sh"), m!("destroy", "")],
  events: &[m!("offer", "s")],
};

/// xdg-decoration: prefer client CSD; optional compositor SSD strip.
pub static ZXDG_DECORATION_MANAGER_V1: Interface = Interface {
  name: "zxdg_decoration_manager_v1",
  version: 1,
  requests: &[m!("destroy", ""), m!("get_toplevel_decoration", "no")],
  events: &[],
};

pub static ZXDG_TOPLEVEL_DECORATION_V1: Interface = Interface {
  name: "zxdg_toplevel_decoration_v1",
  version: 1,
  requests: &[m!("destroy", ""), m!("set_mode", "u"), m!("unset_mode", "")],
  events: &[m!("configure", "u")],
};

pub static ZWP_TEXT_INPUT_MANAGER_V3: Interface = Interface {
  name: "zwp_text_input_manager_v3",
  version: 1,
  requests: &[m!("destroy", ""), m!("get_text_input", "no")],
  events: &[],
};

pub static ZWP_TEXT_INPUT_V3: Interface = Interface {
  name: "zwp_text_input_v3",
  version: 1,
  requests: &[m!("destroy", ""), m!("enable", ""), m!("disable", ""), m!("set_surrounding_text", "sii"), m!("set_text_change_cause", "u"), m!("set_content_type", "uu"), m!("set_cursor_rectangle", "iiii"), m!("commit", "")],
  events: &[m!("enter", "o"), m!("leave", "o"), m!("preedit_string", "?sii"), m!("commit_string", "?s"), m!("delete_surrounding_text", "uu"), m!("done", "u")],
};

pub static ZWP_INPUT_METHOD_MANAGER_V2: Interface = Interface {
  name: "zwp_input_method_manager_v2",
  version: 1,
  requests: &[m!("get_input_method", "on"), m!("destroy", "")],
  events: &[],
};

pub static ZWP_INPUT_METHOD_V2: Interface = Interface {
  name: "zwp_input_method_v2",
  version: 1,
  requests: &[m!("commit_string", "s"), m!("set_preedit_string", "sii"), m!("delete_surrounding_text", "uu"), m!("commit", "u"), m!("get_input_popup_surface", "no"), m!("grab_keyboard", "n"), m!("destroy", "")],
  events: &[m!("activate", ""), m!("deactivate", ""), m!("surrounding_text", "suu"), m!("text_change_cause", "u"), m!("content_type", "uu"), m!("done", ""), m!("unavailable", "")],
};

pub static ZWP_INPUT_POPUP_SURFACE_V2: Interface = Interface {
  name: "zwp_input_popup_surface_v2",
  version: 1,
  requests: &[m!("destroy", "")],
  events: &[m!("text_input_rectangle", "iiii")],
};

pub static ZWP_INPUT_METHOD_KEYBOARD_GRAB_V2: Interface = Interface {
  name: "zwp_input_method_keyboard_grab_v2",
  version: 1,
  requests: &[m!("release", "")],
  events: &[m!("keymap", "uhu"), m!("key", "uuuu"), m!("modifiers", "uuuuu"), m!("repeat_info", "ii")],
};

pub static ZWP_VIRTUAL_KEYBOARD_MANAGER_V1: Interface = Interface {
  name: "zwp_virtual_keyboard_manager_v1",
  version: 1,
  requests: &[m!("create_virtual_keyboard", "on"), m!("destroy", "")],
  events: &[],
};

pub static ZWP_VIRTUAL_KEYBOARD_V1: Interface = Interface {
  name: "zwp_virtual_keyboard_v1",
  version: 1,
  requests: &[m!("keymap", "uhu"), m!("key", "uuu"), m!("modifiers", "uuuu"), m!("destroy", "")],
  events: &[],
};

pub static XDG_WM_BASE: Interface = Interface {
  name: "xdg_wm_base",
  version: 5,
  requests: &[m!("destroy", ""), m!("create_positioner", "n"), m!("get_xdg_surface", "no"), m!("pong", "u")],
  events: &[m!("ping", "u")],
};

pub static XDG_POSITIONER: Interface = Interface {
  name: "xdg_positioner",
  version: 5,
  requests: &[m!("destroy", ""), m!("set_size", "ii"), m!("set_anchor_rect", "iiii"), m!("set_anchor", "u"), m!("set_gravity", "u"), m!("set_constraint_adjustment", "u"), m!("set_offset", "ii"), m!("set_reactive", ""), m!("set_parent_size", "ii"), m!("set_parent_configure", "u")],
  events: &[],
};

pub static XDG_SURFACE: Interface = Interface {
  name: "xdg_surface",
  version: 5,
  requests: &[m!("destroy", ""), m!("get_toplevel", "n"), m!("get_popup", "n?oo"), m!("set_window_geometry", "iiii"), m!("ack_configure", "u")],
  events: &[m!("configure", "u")],
};

pub static XDG_TOPLEVEL: Interface = Interface {
  name: "xdg_toplevel",
  version: 5,
  requests: &[
    m!("destroy", ""),
    m!("set_parent", "?o"),
    m!("set_title", "s"),
    m!("set_app_id", "s"),
    m!("show_window_menu", "ouii"),
    m!("move", "ou"),
    m!("resize", "ouu"),
    m!("set_max_size", "ii"),
    m!("set_min_size", "ii"),
    m!("set_maximized", ""),
    m!("unset_maximized", ""),
    m!("set_fullscreen", "?o"),
    m!("unset_fullscreen", ""),
    m!("set_minimized", ""),
  ],
  events: &[m!("configure", "iia"), m!("close", ""), m!("configure_bounds", "ii"), m!("wm_capabilities", "a")],
};

pub static ZWP_LINUX_DMABUF_V1: Interface = Interface {
  name: "zwp_linux_dmabuf_v1",
  version: 4,
  requests: &[
    m!("destroy", ""),
    m!("create_params", "n"),
    m!("get_default_feedback", "n"),  // since 4
    m!("get_surface_feedback", "no"), // since 4
  ],
  events: &[
    m!("format", "u"),
    m!("modifier", "uuu"), // format, modifier_hi, modifier_lo (since 3)
  ],
};

pub static ZWP_LINUX_BUFFER_PARAMS_V1: Interface = Interface {
  name: "zwp_linux_buffer_params_v1",
  version: 4,
  requests: &[
    m!("destroy", ""),
    m!("add", "huuuuu"),         // fd, plane_idx, offset, stride, mod_hi, mod_lo
    m!("create", "iiuu"),        // width, height, format, flags
    m!("create_immed", "niiuu"), // buffer_id, width, height, format, flags (since 2)
  ],
  events: &[m!("created", "n"), m!("failed", "")],
};

pub static ZWP_LINUX_DMABUF_FEEDBACK_V1: Interface = Interface {
  name: "zwp_linux_dmabuf_feedback_v1",
  version: 4,
  requests: &[m!("destroy", "")],
  events: &[
    m!("done", ""),
    m!("format_table", "hu"), // fd, size
    m!("main_device", "a"),   // dev_t
    m!("tranche_done", ""),
    m!("tranche_target_device", "a"), // dev_t
    m!("tranche_formats", "a"),       // u16 indices into format_table
    m!("tranche_flags", "u"),
  ],
};

pub static XDG_POPUP: Interface = Interface {
  name: "xdg_popup",
  version: 5,
  requests: &[m!("destroy", ""), m!("grab", "ou"), m!("reposition", "ou")],
  events: &[m!("configure", "iiii"), m!("popup_done", ""), m!("repositioned", "u")],
};

// Layer-shell protocol (wlr-layer-shell-unstable-v1)
// Layers: BACKGROUND=0, BOTTOM=1, TOP=2, OVERLAY=3
// Anchor bits: TOP=1, BOTTOM=2, LEFT=4, RIGHT=8
pub static ZWLR_LAYER_SHELL_V1: Interface = Interface {
  name: "zwlr_layer_shell_v1",
  version: 4,
  requests: &[
    m!("get_layer_surface", "no?ous"), // new_id, surface, ?output, layer, namespace
    m!("destroy", ""),
  ],
  events: &[],
};

pub static ZWLR_LAYER_SURFACE_V1: Interface = Interface {
  name: "zwlr_layer_surface_v1",
  version: 4,
  requests: &[
    m!("set_size", "uu"),                    // 0
    m!("set_anchor", "u"),                   // 1
    m!("set_exclusive_zone", "i"),           // 2
    m!("set_margin", "iiii"),                // 3
    m!("set_keyboard_interactivity", "u"),   // 4
    m!("get_popup", "o"),                    // 5
    m!("ack_configure", "u"),                // 6
    m!("destroy", ""),                       // 7
    m!("set_layer", "u"),                    // 8
  ],
  events: &[
    m!("configure", "uuu"), // 0: serial, width, height
    m!("closed", ""),       // 1
  ],
};

pub fn by_name(name: &str) -> Option<&'static Interface> {
  let table: &[&'static Interface] = &[
    &WL_DISPLAY,
    &WL_REGISTRY,
    &WL_CALLBACK,
    &WL_COMPOSITOR,
    &WL_SHM,
    &WL_SHM_POOL,
    &WL_BUFFER,
    &WL_SURFACE,
    &WL_REGION,
    &WL_SUBSURFACE,
    &WL_SUBCOMPOSITOR,
    &WL_SEAT,
    &WL_POINTER,
    &WL_KEYBOARD,
    &WL_OUTPUT,
    &WL_DATA_DEVICE_MANAGER,
    &WL_DATA_SOURCE,
    &WL_DATA_DEVICE,
    &WL_DATA_OFFER,
    &ZWP_PRIMARY_SELECTION_DEVICE_MANAGER_V1,
    &ZWP_PRIMARY_SELECTION_DEVICE_V1,
    &ZWP_PRIMARY_SELECTION_SOURCE_V1,
    &ZWP_PRIMARY_SELECTION_OFFER_V1,
    &ZXDG_DECORATION_MANAGER_V1,
    &ZXDG_TOPLEVEL_DECORATION_V1,
    &ZWP_TEXT_INPUT_MANAGER_V3,
    &ZWP_TEXT_INPUT_V3,
    &ZWP_INPUT_METHOD_MANAGER_V2,
    &ZWP_INPUT_METHOD_V2,
    &ZWP_INPUT_POPUP_SURFACE_V2,
    &ZWP_INPUT_METHOD_KEYBOARD_GRAB_V2,
    &ZWP_VIRTUAL_KEYBOARD_MANAGER_V1,
    &ZWP_VIRTUAL_KEYBOARD_V1,
    &XDG_WM_BASE,
    &XDG_POSITIONER,
    &XDG_SURFACE,
    &XDG_TOPLEVEL,
    &XDG_POPUP,
    &ZWP_LINUX_DMABUF_V1,
    &ZWP_LINUX_BUFFER_PARAMS_V1,
    &ZWP_LINUX_DMABUF_FEEDBACK_V1,
    &ZWLR_LAYER_SHELL_V1,
    &ZWLR_LAYER_SURFACE_V1,
  ];
  table.iter().copied().find(|i| i.name == name)
}
