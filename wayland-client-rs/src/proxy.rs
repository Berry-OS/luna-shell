/*
 * Copyright © 2026 Yuichiro Nakada / Project Vespera
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */


use std::os::raw::c_void;
use crate::interfaces::WlInterface;
use crate::types::{wl_dispatcher_func_t, wl_event_queue, wl_list};

pub const WL_PROXY_FLAG_ID_DELETED: u32 = 1 << 0;
pub const WL_PROXY_FLAG_DESTROYED:  u32 = 1 << 1;
pub const WL_PROXY_FLAG_WRAPPER:    u32 = 1 << 2;

/// Matches libwayland `struct wl_proxy` (64-bit).
#[repr(C)]
pub struct Proxy {
    pub interface:      *const WlInterface,
    pub implementation: *const c_void,
    pub id:             u32,
    pub _id_pad:        u32,
    pub display:        *mut crate::display::Display,
    pub queue:          *mut wl_event_queue,
    pub flags:          u32,
    pub refcount:       i32,
    pub user_data:      *mut c_void,
    pub dispatcher:     wl_dispatcher_func_t,
    pub version:        u32,
    pub _version_pad:   u32,
    pub tag:            *const *const std::os::raw::c_char,
    pub queue_link:     wl_list,
}

unsafe impl Send for Proxy {}
unsafe impl Sync for Proxy {}

impl Proxy {
    pub fn new(
        id: u32,
        interface: *const WlInterface,
        version: u32,
        display: *mut crate::display::Display,
    ) -> Self {
        // queue_link stays null until `init_queue_link` after the Proxy lives at
        // its final heap address.  Self-init here would bake in a stack pointer
        // that is invalidated when the value is moved into a Box — wl_list_remove
        // on destroy then writes through dangling stack addresses (heap corruption
        // / double-free on wl_data_offer_destroy during clipboard selection).
        Proxy {
            interface,
            implementation: std::ptr::null(),
            id,
            _id_pad: 0,
            display,
            queue: std::ptr::null_mut(),
            flags: 0,
            refcount: 1,
            user_data: std::ptr::null_mut(),
            dispatcher: None,
            version,
            _version_pad: 0,
            tag: std::ptr::null(),
            queue_link: wl_list::new(),
        }
    }

    /// Init `queue_link` to a self-pointing empty list at the proxy's final address.
    pub unsafe fn init_queue_link(proxy: *mut Proxy) {
        if proxy.is_null() {
            return;
        }
        wl_list::init(&mut (*proxy).queue_link);
    }

    /// Box a proxy and seal its queue_link (the only safe construction path).
    pub unsafe fn into_heap(proxy: Proxy) -> *mut Proxy {
        let raw = Box::into_raw(Box::new(proxy));
        Self::init_queue_link(raw);
        raw
    }

    pub fn is_deleted(&self) -> bool {
        self.flags & WL_PROXY_FLAG_ID_DELETED != 0
    }

    pub fn is_destroyed(&self) -> bool {
        self.flags & WL_PROXY_FLAG_DESTROYED != 0
    }
}
