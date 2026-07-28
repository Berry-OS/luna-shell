/*
 * Copyright © 2026 Yuichiro Nakada / Project Vespera
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */


use std::os::raw::c_char;
use crate::types::wl_message;

#[repr(C)]
pub struct WlInterface {
    pub name: *const c_char,
    pub version: i32,
    pub method_count: i32,
    pub methods: *const wl_message,
    pub event_count: i32,
    pub events: *const wl_message,
}

unsafe impl Send for WlInterface {}
unsafe impl Sync for WlInterface {}

// Static interface pointer arrays for C `types` fields.
#[repr(transparent)]
pub struct Spa<const N: usize>(pub [*const WlInterface; N]);
unsafe impl<const N: usize> Sync for Spa<N> {}
unsafe impl<const N: usize> Send for Spa<N> {}

impl<const N: usize> Spa<N> {
    pub const fn ptr(&self) -> *const *const WlInterface {
        self.0.as_ptr()
    }
}

macro_rules! c_str {
    ($s:expr) => { concat!($s, "\0").as_ptr() as *const c_char }
}

pub static TYPES_NULL: Spa<1> = Spa([std::ptr::null()]);


static WL_CALLBACK_EVENTS: [wl_message; 1] = [
    wl_message { name: c_str!("done"), signature: c_str!("u"), types: std::ptr::null() },
];

#[no_mangle]
pub static wl_callback_interface: WlInterface = WlInterface {
    name: c_str!("wl_callback"),
    version: 1,
    method_count: 0,
    methods: std::ptr::null(),
    event_count: 1,
    events: WL_CALLBACK_EVENTS.as_ptr(),
};


static WL_REGISTRY_REQUESTS: [wl_message; 1] = [
    wl_message { name: c_str!("bind"), signature: c_str!("usun"), types: std::ptr::null() },
];
static WL_REGISTRY_EVENTS: [wl_message; 2] = [
    wl_message { name: c_str!("global"),        signature: c_str!("usu"), types: std::ptr::null() },
    wl_message { name: c_str!("global_remove"), signature: c_str!("u"),   types: std::ptr::null() },
];

#[no_mangle]
pub static wl_registry_interface: WlInterface = WlInterface {
    name: c_str!("wl_registry"),
    version: 1,
    method_count: 1,
    methods: WL_REGISTRY_REQUESTS.as_ptr(),
    event_count: 2,
    events: WL_REGISTRY_EVENTS.as_ptr(),
};


pub static WL_DISPLAY_SYNC_TYPES: Spa<1> = Spa([&wl_callback_interface as *const _]);
pub static WL_DISPLAY_GET_REGISTRY_TYPES: Spa<1> = Spa([&wl_registry_interface as *const _]);

static WL_DISPLAY_REQUESTS: [wl_message; 2] = [
    wl_message { name: c_str!("sync"),         signature: c_str!("n"), types: WL_DISPLAY_SYNC_TYPES.ptr() },
    wl_message { name: c_str!("get_registry"), signature: c_str!("n"), types: WL_DISPLAY_GET_REGISTRY_TYPES.ptr() },
];
static WL_DISPLAY_EVENTS: [wl_message; 2] = [
    wl_message { name: c_str!("error"),     signature: c_str!("ous"), types: std::ptr::null() },
    wl_message { name: c_str!("delete_id"), signature: c_str!("u"),   types: std::ptr::null() },
];

#[no_mangle]
pub static wl_display_interface: WlInterface = WlInterface {
    name: c_str!("wl_display"),
    version: 1,
    method_count: 2,
    methods: WL_DISPLAY_REQUESTS.as_ptr(),
    event_count: 2,
    events: WL_DISPLAY_EVENTS.as_ptr(),
};


static WL_SURFACE_REQUESTS: [wl_message; 10] = [
    wl_message { name: c_str!("destroy"),              signature: c_str!(""),       types: std::ptr::null() },
    wl_message { name: c_str!("attach"),               signature: c_str!("?oii"),   types: std::ptr::null() },
    wl_message { name: c_str!("damage"),               signature: c_str!("iiii"),   types: std::ptr::null() },
    wl_message { name: c_str!("frame"),                signature: c_str!("n"),      types: WL_DISPLAY_SYNC_TYPES.ptr() },
    wl_message { name: c_str!("set_opaque_region"),    signature: c_str!("?o"),     types: std::ptr::null() },
    wl_message { name: c_str!("set_input_region"),     signature: c_str!("?o"),     types: std::ptr::null() },
    wl_message { name: c_str!("commit"),               signature: c_str!(""),       types: std::ptr::null() },
    wl_message { name: c_str!("set_buffer_transform"), signature: c_str!("2i"),     types: std::ptr::null() },
    wl_message { name: c_str!("set_buffer_scale"),     signature: c_str!("3i"),     types: std::ptr::null() },
    wl_message { name: c_str!("damage_buffer"),        signature: c_str!("4iiii"),  types: std::ptr::null() },
];
static WL_SURFACE_EVENTS: [wl_message; 2] = [
    wl_message { name: c_str!("enter"), signature: c_str!("o"), types: std::ptr::null() },
    wl_message { name: c_str!("leave"), signature: c_str!("o"), types: std::ptr::null() },
];

#[no_mangle]
pub static wl_surface_interface: WlInterface = WlInterface {
    name: c_str!("wl_surface"),
    version: 6,
    method_count: 10,
    methods: WL_SURFACE_REQUESTS.as_ptr(),
    event_count: 2,
    events: WL_SURFACE_EVENTS.as_ptr(),
};


static WL_REGION_REQUESTS: [wl_message; 3] = [
    wl_message { name: c_str!("destroy"),  signature: c_str!(""),     types: std::ptr::null() },
    wl_message { name: c_str!("add"),      signature: c_str!("iiii"), types: std::ptr::null() },
    wl_message { name: c_str!("subtract"), signature: c_str!("iiii"), types: std::ptr::null() },
];

#[no_mangle]
pub static wl_region_interface: WlInterface = WlInterface {
    name: c_str!("wl_region"),
    version: 1,
    method_count: 3,
    methods: WL_REGION_REQUESTS.as_ptr(),
    event_count: 0,
    events: std::ptr::null(),
};


static WL_COMPOSITOR_CREATE_SURFACE_TYPES: Spa<1> = Spa([&wl_surface_interface as *const _]);
static WL_COMPOSITOR_CREATE_REGION_TYPES:  Spa<1> = Spa([&wl_region_interface  as *const _]);

static WL_COMPOSITOR_REQUESTS: [wl_message; 2] = [
    wl_message { name: c_str!("create_surface"), signature: c_str!("n"), types: WL_COMPOSITOR_CREATE_SURFACE_TYPES.ptr() },
    wl_message { name: c_str!("create_region"),  signature: c_str!("n"), types: WL_COMPOSITOR_CREATE_REGION_TYPES.ptr() },
];

#[no_mangle]
pub static wl_compositor_interface: WlInterface = WlInterface {
    name: c_str!("wl_compositor"),
    version: 6,
    method_count: 2,
    methods: WL_COMPOSITOR_REQUESTS.as_ptr(),
    event_count: 0,
    events: std::ptr::null(),
};


static WL_BUFFER_REQUESTS: [wl_message; 1] = [
    wl_message { name: c_str!("destroy"), signature: c_str!(""), types: std::ptr::null() },
];
static WL_BUFFER_EVENTS: [wl_message; 1] = [
    wl_message { name: c_str!("release"), signature: c_str!(""), types: std::ptr::null() },
];

#[no_mangle]
pub static wl_buffer_interface: WlInterface = WlInterface {
    name: c_str!("wl_buffer"),
    version: 1,
    method_count: 1,
    methods: WL_BUFFER_REQUESTS.as_ptr(),
    event_count: 1,
    events: WL_BUFFER_EVENTS.as_ptr(),
};

static WL_SHM_POOL_CREATE_BUFFER_TYPES: Spa<1> = Spa([&wl_buffer_interface as *const _]);
static WL_SHM_POOL_REQUESTS: [wl_message; 3] = [
    wl_message { name: c_str!("create_buffer"), signature: c_str!("niiiiu"), types: WL_SHM_POOL_CREATE_BUFFER_TYPES.ptr() },
    wl_message { name: c_str!("destroy"),       signature: c_str!(""),       types: std::ptr::null() },
    wl_message { name: c_str!("resize"),        signature: c_str!("i"),      types: std::ptr::null() },
];

#[no_mangle]
pub static wl_shm_pool_interface: WlInterface = WlInterface {
    name: c_str!("wl_shm_pool"),
    version: 1,
    method_count: 3,
    methods: WL_SHM_POOL_REQUESTS.as_ptr(),
    event_count: 0,
    events: std::ptr::null(),
};

static WL_SHM_CREATE_POOL_TYPES: Spa<1> = Spa([&wl_shm_pool_interface as *const _]);
static WL_SHM_REQUESTS: [wl_message; 1] = [
    wl_message { name: c_str!("create_pool"), signature: c_str!("nhi"), types: WL_SHM_CREATE_POOL_TYPES.ptr() },
];
static WL_SHM_EVENTS: [wl_message; 1] = [
    wl_message { name: c_str!("format"), signature: c_str!("u"), types: std::ptr::null() },
];

#[no_mangle]
pub static wl_shm_interface: WlInterface = WlInterface {
    name: c_str!("wl_shm"),
    version: 1,
    method_count: 1,
    methods: WL_SHM_REQUESTS.as_ptr(),
    event_count: 1,
    events: WL_SHM_EVENTS.as_ptr(),
};


static WL_KEYBOARD_EVENTS: [wl_message; 6] = [
    wl_message { name: c_str!("keymap"),      signature: c_str!("uhu"),   types: std::ptr::null() },
    wl_message { name: c_str!("enter"),       signature: c_str!("uoa"),   types: std::ptr::null() },
    wl_message { name: c_str!("leave"),       signature: c_str!("uo"),    types: std::ptr::null() },
    wl_message { name: c_str!("key"),         signature: c_str!("uuuu"),  types: std::ptr::null() },
    wl_message { name: c_str!("modifiers"),   signature: c_str!("uuuuu"), types: std::ptr::null() },
    wl_message { name: c_str!("repeat_info"), signature: c_str!("4ii"),   types: std::ptr::null() },
];
static WL_KEYBOARD_REQUESTS: [wl_message; 1] = [
    wl_message { name: c_str!("release"), signature: c_str!("3"), types: std::ptr::null() },
];

static WL_POINTER_EVENTS: [wl_message; 11] = [
    wl_message { name: c_str!("enter"),               signature: c_str!("uoff"),    types: std::ptr::null() },
    wl_message { name: c_str!("leave"),               signature: c_str!("uo"),      types: std::ptr::null() },
    wl_message { name: c_str!("motion"),              signature: c_str!("uff"),     types: std::ptr::null() },
    wl_message { name: c_str!("button"),              signature: c_str!("uuuu"),    types: std::ptr::null() },
    wl_message { name: c_str!("axis"),                signature: c_str!("uuf"),     types: std::ptr::null() },
    wl_message { name: c_str!("frame"),               signature: c_str!("5"),       types: std::ptr::null() },
    wl_message { name: c_str!("axis_source"),         signature: c_str!("5u"),      types: std::ptr::null() },
    wl_message { name: c_str!("axis_stop"),           signature: c_str!("5uu"),     types: std::ptr::null() },
    wl_message { name: c_str!("axis_discrete"),       signature: c_str!("5ui"),     types: std::ptr::null() },
    wl_message { name: c_str!("axis_value120"),       signature: c_str!("8ui"),     types: std::ptr::null() },
    wl_message { name: c_str!("axis_relative_direction"), signature: c_str!("9uu"), types: std::ptr::null() },
];
static WL_POINTER_REQUESTS: [wl_message; 2] = [
    wl_message { name: c_str!("set_cursor"),  signature: c_str!("u?oii"), types: std::ptr::null() },
    wl_message { name: c_str!("release"),     signature: c_str!("3"),     types: std::ptr::null() },
];

#[no_mangle]
pub static wl_pointer_interface: WlInterface = WlInterface {
    name: c_str!("wl_pointer"),
    version: 9,
    method_count: 2,
    methods: WL_POINTER_REQUESTS.as_ptr(),
    event_count: 11,
    events: WL_POINTER_EVENTS.as_ptr(),
};
#[no_mangle]
pub static wl_keyboard_interface: WlInterface = WlInterface {
    name: c_str!("wl_keyboard"),
    version: 9,
    method_count: 1,
    methods: WL_KEYBOARD_REQUESTS.as_ptr(),
    event_count: 6,
    events: WL_KEYBOARD_EVENTS.as_ptr(),
};
#[no_mangle]
pub static wl_touch_interface:    WlInterface = WlInterface { name: c_str!("wl_touch"),    version: 9, method_count: 0, methods: std::ptr::null(), event_count: 0, events: std::ptr::null() };

static WL_SEAT_GET_POINTER_TYPES:  Spa<1> = Spa([&wl_pointer_interface  as *const _]);
static WL_SEAT_GET_KEYBOARD_TYPES: Spa<1> = Spa([&wl_keyboard_interface as *const _]);
static WL_SEAT_GET_TOUCH_TYPES:    Spa<1> = Spa([&wl_touch_interface    as *const _]);

static WL_SEAT_REQUESTS: [wl_message; 4] = [
    wl_message { name: c_str!("get_pointer"),  signature: c_str!("n"),  types: WL_SEAT_GET_POINTER_TYPES.ptr() },
    wl_message { name: c_str!("get_keyboard"), signature: c_str!("n"),  types: WL_SEAT_GET_KEYBOARD_TYPES.ptr() },
    wl_message { name: c_str!("get_touch"),    signature: c_str!("n"),  types: WL_SEAT_GET_TOUCH_TYPES.ptr() },
    wl_message { name: c_str!("release"),      signature: c_str!("5"),  types: std::ptr::null() },
];
static WL_SEAT_EVENTS: [wl_message; 2] = [
    wl_message { name: c_str!("capabilities"), signature: c_str!("u"),  types: std::ptr::null() },
    wl_message { name: c_str!("name"),         signature: c_str!("2s"), types: std::ptr::null() },
];

#[no_mangle]
pub static wl_seat_interface: WlInterface = WlInterface {
    name: c_str!("wl_seat"),
    version: 9,
    method_count: 4,
    methods: WL_SEAT_REQUESTS.as_ptr(),
    event_count: 2,
    events: WL_SEAT_EVENTS.as_ptr(),
};


static WL_OUTPUT_REQUESTS: [wl_message; 1] = [
    wl_message { name: c_str!("release"), signature: c_str!("3"), types: std::ptr::null() },
];
static WL_OUTPUT_EVENTS: [wl_message; 5] = [
    wl_message { name: c_str!("geometry"), signature: c_str!("iiiiissi"), types: std::ptr::null() },
    wl_message { name: c_str!("mode"),     signature: c_str!("uiii"),     types: std::ptr::null() },
    wl_message { name: c_str!("done"),     signature: c_str!("2"),        types: std::ptr::null() },
    wl_message { name: c_str!("scale"),    signature: c_str!("2i"),       types: std::ptr::null() },
    wl_message { name: c_str!("name"),     signature: c_str!("4s"),       types: std::ptr::null() },
];

#[no_mangle]
pub static wl_output_interface: WlInterface = WlInterface {
    name: c_str!("wl_output"),
    version: 4,
    method_count: 1,
    methods: WL_OUTPUT_REQUESTS.as_ptr(),
    event_count: 5,
    events: WL_OUTPUT_EVENTS.as_ptr(),
};


#[no_mangle]
pub static wl_subsurface_interface: WlInterface = WlInterface {
    name: c_str!("wl_subsurface"), version: 1,
    method_count: 0, methods: std::ptr::null(), event_count: 0, events: std::ptr::null(),
};

static WL_SUBCOMPOSITOR_GET_SUBSURFACE_TYPES: Spa<2> = Spa([&wl_subsurface_interface as *const _, std::ptr::null()]);
static WL_SUBCOMPOSITOR_REQUESTS: [wl_message; 2] = [
    wl_message { name: c_str!("destroy"),        signature: c_str!(""),    types: std::ptr::null() },
    wl_message { name: c_str!("get_subsurface"), signature: c_str!("noo"), types: WL_SUBCOMPOSITOR_GET_SUBSURFACE_TYPES.ptr() },
];

#[no_mangle]
pub static wl_subcompositor_interface: WlInterface = WlInterface {
    name: c_str!("wl_subcompositor"),
    version: 1,
    method_count: 2,
    methods: WL_SUBCOMPOSITOR_REQUESTS.as_ptr(),
    event_count: 0,
    events: std::ptr::null(),
};

static XDG_POSITIONER_REQUESTS: [wl_message; 10] = [
    wl_message { name: c_str!("destroy"),                  signature: c_str!(""),         types: std::ptr::null() },
    wl_message { name: c_str!("set_size"),                 signature: c_str!("ii"),        types: std::ptr::null() },
    wl_message { name: c_str!("set_anchor_rect"),          signature: c_str!("iiii"),      types: std::ptr::null() },
    wl_message { name: c_str!("set_anchor"),               signature: c_str!("u"),         types: std::ptr::null() },
    wl_message { name: c_str!("set_gravity"),              signature: c_str!("u"),         types: std::ptr::null() },
    wl_message { name: c_str!("set_constraint_adjustment"), signature: c_str!("u"),        types: std::ptr::null() },
    wl_message { name: c_str!("set_offset"),               signature: c_str!("ii"),        types: std::ptr::null() },
    wl_message { name: c_str!("set_reactive"),             signature: c_str!("3"),         types: std::ptr::null() },
    wl_message { name: c_str!("set_parent_size"),          signature: c_str!("3ii"),       types: std::ptr::null() },
    wl_message { name: c_str!("set_parent_configure"),     signature: c_str!("3u"),        types: std::ptr::null() },
];
#[no_mangle]
pub static xdg_positioner_interface: WlInterface = WlInterface {
    name: c_str!("xdg_positioner"), version: 6,
    method_count: 10, methods: XDG_POSITIONER_REQUESTS.as_ptr(),
    event_count: 0, events: std::ptr::null(),
};

static XDG_POPUP_REQUESTS: [wl_message; 3] = [
    wl_message { name: c_str!("destroy"),     signature: c_str!(""),    types: std::ptr::null() },
    wl_message { name: c_str!("grab"),        signature: c_str!("ou"),  types: std::ptr::null() },
    wl_message { name: c_str!("reposition"),  signature: c_str!("3ou"), types: std::ptr::null() },
];
static XDG_POPUP_EVENTS: [wl_message; 3] = [
    wl_message { name: c_str!("configure"),     signature: c_str!("iiii"), types: std::ptr::null() },
    wl_message { name: c_str!("popup_done"),    signature: c_str!(""),     types: std::ptr::null() },
    wl_message { name: c_str!("repositioned"), signature: c_str!("3u"),   types: std::ptr::null() },
];
#[no_mangle]
pub static xdg_popup_interface: WlInterface = WlInterface {
    name: c_str!("xdg_popup"), version: 6,
    method_count: 3, methods: XDG_POPUP_REQUESTS.as_ptr(),
    event_count: 3, events: XDG_POPUP_EVENTS.as_ptr(),
};

static XDG_TOPLEVEL_REQUESTS: [wl_message; 14] = [
    wl_message { name: c_str!("destroy"),           signature: c_str!(""),         types: std::ptr::null() },
    wl_message { name: c_str!("set_parent"),        signature: c_str!("?o"),       types: std::ptr::null() },
    wl_message { name: c_str!("set_title"),         signature: c_str!("s"),        types: std::ptr::null() },
    wl_message { name: c_str!("set_app_id"),        signature: c_str!("s"),        types: std::ptr::null() },
    wl_message { name: c_str!("show_window_menu"),  signature: c_str!("ouu"),      types: std::ptr::null() },
    wl_message { name: c_str!("move"),              signature: c_str!("ou"),       types: std::ptr::null() },
    wl_message { name: c_str!("resize"),            signature: c_str!("ouu"),      types: std::ptr::null() },
    wl_message { name: c_str!("set_max_size"),      signature: c_str!("ii"),       types: std::ptr::null() },
    wl_message { name: c_str!("set_min_size"),      signature: c_str!("ii"),       types: std::ptr::null() },
    wl_message { name: c_str!("set_maximized"),     signature: c_str!(""),         types: std::ptr::null() },
    wl_message { name: c_str!("unset_maximized"),   signature: c_str!(""),         types: std::ptr::null() },
    wl_message { name: c_str!("set_fullscreen"),    signature: c_str!("?o"),       types: std::ptr::null() },
    wl_message { name: c_str!("unset_fullscreen"),  signature: c_str!(""),         types: std::ptr::null() },
    wl_message { name: c_str!("set_minimized"),     signature: c_str!(""),         types: std::ptr::null() },
];
static XDG_TOPLEVEL_EVENTS: [wl_message; 4] = [
    wl_message { name: c_str!("configure"),        signature: c_str!("iia"),  types: std::ptr::null() },
    wl_message { name: c_str!("close"),            signature: c_str!(""),     types: std::ptr::null() },
    wl_message { name: c_str!("configure_bounds"), signature: c_str!("4ii"),  types: std::ptr::null() },
    wl_message { name: c_str!("wm_capabilities"),  signature: c_str!("5a"),   types: std::ptr::null() },
];
#[no_mangle]
pub static xdg_toplevel_interface: WlInterface = WlInterface {
    name: c_str!("xdg_toplevel"), version: 6,
    method_count: 14, methods: XDG_TOPLEVEL_REQUESTS.as_ptr(),
    event_count: 4, events: XDG_TOPLEVEL_EVENTS.as_ptr(),
};

static XDG_TOPLEVEL_TYPES: Spa<1> = Spa([&xdg_toplevel_interface as *const _]);
static XDG_POPUP_TYPES:    Spa<2> = Spa([&xdg_popup_interface as *const _, &xdg_positioner_interface as *const _]);
static XDG_SURFACE_REQUESTS: [wl_message; 5] = [
    wl_message { name: c_str!("destroy"),             signature: c_str!(""),   types: std::ptr::null() },
    wl_message { name: c_str!("get_toplevel"),        signature: c_str!("n"),  types: XDG_TOPLEVEL_TYPES.ptr() },
    wl_message { name: c_str!("get_popup"),           signature: c_str!("n?oo"), types: XDG_POPUP_TYPES.ptr() },
    wl_message { name: c_str!("set_window_geometry"), signature: c_str!("iiii"), types: std::ptr::null() },
    wl_message { name: c_str!("ack_configure"),       signature: c_str!("u"),  types: std::ptr::null() },
];
static XDG_SURFACE_EVENTS: [wl_message; 1] = [
    wl_message { name: c_str!("configure"), signature: c_str!("u"), types: std::ptr::null() },
];
#[no_mangle]
pub static xdg_surface_interface: WlInterface = WlInterface {
    name: c_str!("xdg_surface"), version: 6,
    method_count: 5, methods: XDG_SURFACE_REQUESTS.as_ptr(),
    event_count: 1, events: XDG_SURFACE_EVENTS.as_ptr(),
};

static XDG_WM_BASE_CREATE_POSITIONER_TYPES: Spa<1> = Spa([&xdg_positioner_interface as *const _]);
static XDG_WM_BASE_GET_XDG_SURFACE_TYPES:   Spa<2> = Spa([&xdg_surface_interface as *const _, std::ptr::null()]);
static XDG_WM_BASE_REQUESTS: [wl_message; 4] = [
    wl_message { name: c_str!("destroy"),           signature: c_str!(""),   types: std::ptr::null() },
    wl_message { name: c_str!("create_positioner"), signature: c_str!("n"),  types: XDG_WM_BASE_CREATE_POSITIONER_TYPES.ptr() },
    wl_message { name: c_str!("get_xdg_surface"),   signature: c_str!("no"), types: XDG_WM_BASE_GET_XDG_SURFACE_TYPES.ptr() },
    wl_message { name: c_str!("pong"),              signature: c_str!("u"),  types: std::ptr::null() },
];
static XDG_WM_BASE_EVENTS: [wl_message; 1] = [
    wl_message { name: c_str!("ping"), signature: c_str!("u"), types: std::ptr::null() },
];

#[no_mangle]
pub static xdg_wm_base_interface: WlInterface = WlInterface {
    name: c_str!("xdg_wm_base"),
    version: 6,
    method_count: 4,
    methods: XDG_WM_BASE_REQUESTS.as_ptr(),
    event_count: 1,
    events: XDG_WM_BASE_EVENTS.as_ptr(),
};


#[no_mangle]
pub static wp_viewport_interface:             WlInterface = WlInterface { name: c_str!("wp_viewport"),             version: 1, method_count: 0, methods: std::ptr::null(), event_count: 0, events: std::ptr::null() };
#[no_mangle]
pub static wp_viewporter_interface:           WlInterface = WlInterface { name: c_str!("wp_viewporter"),           version: 1, method_count: 0, methods: std::ptr::null(), event_count: 0, events: std::ptr::null() };
#[no_mangle]
pub static zwp_linux_dmabuf_v1_interface:     WlInterface = WlInterface { name: c_str!("zwp_linux_dmabuf_v1"),     version: 4, method_count: 0, methods: std::ptr::null(), event_count: 0, events: std::ptr::null() };

/* ── wl_data_device family (clipboard). Empty stubs here made GTK Ctrl+C/V
 * and luna-clipboard silently no-op under the vendored libwayland-client. ── */

static WL_DATA_OFFER_REQUESTS: [wl_message; 5] = [
    wl_message { name: c_str!("accept"),      signature: c_str!("u?s"),  types: std::ptr::null() },
    wl_message { name: c_str!("receive"),     signature: c_str!("sh"),   types: std::ptr::null() },
    wl_message { name: c_str!("destroy"),     signature: c_str!(""),     types: std::ptr::null() },
    wl_message { name: c_str!("finish"),      signature: c_str!("3"),    types: std::ptr::null() },
    wl_message { name: c_str!("set_actions"), signature: c_str!("3uu"),  types: std::ptr::null() },
];
static WL_DATA_OFFER_EVENTS: [wl_message; 3] = [
    wl_message { name: c_str!("offer"),          signature: c_str!("s"),  types: std::ptr::null() },
    wl_message { name: c_str!("source_actions"), signature: c_str!("3u"), types: std::ptr::null() },
    wl_message { name: c_str!("action"),         signature: c_str!("3u"), types: std::ptr::null() },
];

#[no_mangle]
pub static wl_data_offer_interface: WlInterface = WlInterface {
    name: c_str!("wl_data_offer"),
    version: 3,
    method_count: 5,
    methods: WL_DATA_OFFER_REQUESTS.as_ptr(),
    event_count: 3,
    events: WL_DATA_OFFER_EVENTS.as_ptr(),
};

static WL_DATA_SOURCE_REQUESTS: [wl_message; 3] = [
    wl_message { name: c_str!("offer"),       signature: c_str!("s"),  types: std::ptr::null() },
    wl_message { name: c_str!("destroy"),     signature: c_str!(""),   types: std::ptr::null() },
    wl_message { name: c_str!("set_actions"), signature: c_str!("3u"), types: std::ptr::null() },
];
static WL_DATA_SOURCE_EVENTS: [wl_message; 6] = [
    wl_message { name: c_str!("target"),             signature: c_str!("?s"), types: std::ptr::null() },
    wl_message { name: c_str!("send"),               signature: c_str!("sh"), types: std::ptr::null() },
    wl_message { name: c_str!("cancelled"),          signature: c_str!(""),   types: std::ptr::null() },
    wl_message { name: c_str!("dnd_drop_performed"), signature: c_str!("3"),  types: std::ptr::null() },
    wl_message { name: c_str!("dnd_finished"),       signature: c_str!("3"),  types: std::ptr::null() },
    wl_message { name: c_str!("action"),             signature: c_str!("3u"), types: std::ptr::null() },
];

#[no_mangle]
pub static wl_data_source_interface: WlInterface = WlInterface {
    name: c_str!("wl_data_source"),
    version: 3,
    method_count: 3,
    methods: WL_DATA_SOURCE_REQUESTS.as_ptr(),
    event_count: 6,
    events: WL_DATA_SOURCE_EVENTS.as_ptr(),
};

static WL_DATA_DEVICE_DATA_OFFER_TYPES: Spa<1> = Spa([&wl_data_offer_interface as *const _]);
static WL_DATA_DEVICE_EVENTS: [wl_message; 6] = [
    wl_message { name: c_str!("data_offer"), signature: c_str!("n"),      types: WL_DATA_DEVICE_DATA_OFFER_TYPES.ptr() },
    wl_message { name: c_str!("enter"),      signature: c_str!("uoff?o"), types: std::ptr::null() },
    wl_message { name: c_str!("leave"),      signature: c_str!(""),       types: std::ptr::null() },
    wl_message { name: c_str!("motion"),     signature: c_str!("uff"),    types: std::ptr::null() },
    wl_message { name: c_str!("drop"),       signature: c_str!(""),       types: std::ptr::null() },
    wl_message { name: c_str!("selection"),  signature: c_str!("?o"),     types: std::ptr::null() },
];
static WL_DATA_DEVICE_REQUESTS: [wl_message; 3] = [
    wl_message { name: c_str!("start_drag"),    signature: c_str!("?oo?ou"), types: std::ptr::null() },
    wl_message { name: c_str!("set_selection"), signature: c_str!("?ou"),    types: std::ptr::null() },
    wl_message { name: c_str!("release"),       signature: c_str!("2"),      types: std::ptr::null() },
];

#[no_mangle]
pub static wl_data_device_interface: WlInterface = WlInterface {
    name: c_str!("wl_data_device"),
    version: 3,
    method_count: 3,
    methods: WL_DATA_DEVICE_REQUESTS.as_ptr(),
    event_count: 6,
    events: WL_DATA_DEVICE_EVENTS.as_ptr(),
};

static WL_DDM_CREATE_SOURCE_TYPES: Spa<1> = Spa([&wl_data_source_interface as *const _]);
static WL_DDM_GET_DEVICE_TYPES: Spa<2> = Spa([
    &wl_data_device_interface as *const _,
    &wl_seat_interface as *const _,
]);
static WL_DATA_DEVICE_MANAGER_REQUESTS: [wl_message; 2] = [
    wl_message { name: c_str!("create_data_source"), signature: c_str!("n"),  types: WL_DDM_CREATE_SOURCE_TYPES.ptr() },
    wl_message { name: c_str!("get_data_device"),    signature: c_str!("no"), types: WL_DDM_GET_DEVICE_TYPES.ptr() },
];

#[no_mangle]
pub static wl_data_device_manager_interface: WlInterface = WlInterface {
    name: c_str!("wl_data_device_manager"),
    version: 3,
    method_count: 2,
    methods: WL_DATA_DEVICE_MANAGER_REQUESTS.as_ptr(),
    event_count: 0,
    events: std::ptr::null(),
};

/* ── zwp_primary_selection (middle-click paste). Without types[] on
 * data_offer, GTK receives the raw server id 0xff000000 and segfaults. ── */

static ZWP_PRIMARY_OFFER_REQUESTS: [wl_message; 2] = [
    wl_message { name: c_str!("receive"), signature: c_str!("sh"), types: std::ptr::null() },
    wl_message { name: c_str!("destroy"), signature: c_str!(""),   types: std::ptr::null() },
];
static ZWP_PRIMARY_OFFER_EVENTS: [wl_message; 1] = [
    wl_message { name: c_str!("offer"), signature: c_str!("s"), types: std::ptr::null() },
];

#[no_mangle]
pub static zwp_primary_selection_offer_v1_interface: WlInterface = WlInterface {
    name: c_str!("zwp_primary_selection_offer_v1"),
    version: 1,
    method_count: 2,
    methods: ZWP_PRIMARY_OFFER_REQUESTS.as_ptr(),
    event_count: 1,
    events: ZWP_PRIMARY_OFFER_EVENTS.as_ptr(),
};

static ZWP_PRIMARY_SOURCE_REQUESTS: [wl_message; 2] = [
    wl_message { name: c_str!("offer"),   signature: c_str!("s"), types: std::ptr::null() },
    wl_message { name: c_str!("destroy"), signature: c_str!(""),  types: std::ptr::null() },
];
static ZWP_PRIMARY_SOURCE_EVENTS: [wl_message; 2] = [
    wl_message { name: c_str!("send"),      signature: c_str!("sh"), types: std::ptr::null() },
    wl_message { name: c_str!("cancelled"), signature: c_str!(""),   types: std::ptr::null() },
];

#[no_mangle]
pub static zwp_primary_selection_source_v1_interface: WlInterface = WlInterface {
    name: c_str!("zwp_primary_selection_source_v1"),
    version: 1,
    method_count: 2,
    methods: ZWP_PRIMARY_SOURCE_REQUESTS.as_ptr(),
    event_count: 2,
    events: ZWP_PRIMARY_SOURCE_EVENTS.as_ptr(),
};

static ZWP_PRIMARY_DEVICE_DATA_OFFER_TYPES: Spa<1> =
    Spa([&zwp_primary_selection_offer_v1_interface as *const _]);
static ZWP_PRIMARY_DEVICE_EVENTS: [wl_message; 2] = [
    wl_message {
        name: c_str!("data_offer"),
        signature: c_str!("n"),
        types: ZWP_PRIMARY_DEVICE_DATA_OFFER_TYPES.ptr(),
    },
    wl_message { name: c_str!("selection"), signature: c_str!("?o"), types: std::ptr::null() },
];
static ZWP_PRIMARY_DEVICE_REQUESTS: [wl_message; 2] = [
    wl_message { name: c_str!("set_selection"), signature: c_str!("?ou"), types: std::ptr::null() },
    wl_message { name: c_str!("destroy"),       signature: c_str!(""),    types: std::ptr::null() },
];

#[no_mangle]
pub static zwp_primary_selection_device_v1_interface: WlInterface = WlInterface {
    name: c_str!("zwp_primary_selection_device_v1"),
    version: 1,
    method_count: 2,
    methods: ZWP_PRIMARY_DEVICE_REQUESTS.as_ptr(),
    event_count: 2,
    events: ZWP_PRIMARY_DEVICE_EVENTS.as_ptr(),
};

static ZWP_PRIMARY_MGR_CREATE_TYPES: Spa<1> =
    Spa([&zwp_primary_selection_source_v1_interface as *const _]);
static ZWP_PRIMARY_MGR_GET_TYPES: Spa<2> = Spa([
    &zwp_primary_selection_device_v1_interface as *const _,
    &wl_seat_interface as *const _,
]);
static ZWP_PRIMARY_MGR_REQUESTS: [wl_message; 3] = [
    wl_message {
        name: c_str!("create_source"),
        signature: c_str!("n"),
        types: ZWP_PRIMARY_MGR_CREATE_TYPES.ptr(),
    },
    wl_message {
        name: c_str!("get_device"),
        signature: c_str!("no"),
        types: ZWP_PRIMARY_MGR_GET_TYPES.ptr(),
    },
    wl_message { name: c_str!("destroy"), signature: c_str!(""), types: std::ptr::null() },
];

#[no_mangle]
pub static zwp_primary_selection_device_manager_v1_interface: WlInterface = WlInterface {
    name: c_str!("zwp_primary_selection_device_manager_v1"),
    version: 1,
    method_count: 3,
    methods: ZWP_PRIMARY_MGR_REQUESTS.as_ptr(),
    event_count: 0,
    events: std::ptr::null(),
};

// wl_shell stub: deprecated but referenced by libgstgl
#[no_mangle]
pub static wl_shell_interface:                WlInterface = WlInterface { name: c_str!("wl_shell"),               version: 1, method_count: 0, methods: std::ptr::null(), event_count: 0, events: std::ptr::null() };
#[no_mangle]
pub static wl_shell_surface_interface:        WlInterface = WlInterface { name: c_str!("wl_shell_surface"),       version: 1, method_count: 0, methods: std::ptr::null(), event_count: 0, events: std::ptr::null() };
