/*
 * Copyright © 2026 Yuichiro Nakada / Project Vespera
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */


#![allow(non_snake_case, clippy::missing_safety_doc)]

use std::ffi::CStr;
use std::os::raw::{c_char, c_int, c_void};
use crate::display::Display;
use crate::interfaces::WlInterface;
use crate::proxy::{
    Proxy, WL_PROXY_FLAG_DESTROYED, WL_PROXY_FLAG_WRAPPER,
};
use crate::types::{wl_argument, wl_array, wl_dispatcher_func_t, wl_event_queue, wl_list};
use crate::wire;

// Fallback only when proxy->display is NULL (broken callers). Never override a
// non-null display pointer: Mesa opens a second wl_display during EGL probe,
// and forcing GLOBAL then mutates the wrong HashMap / UAF after disconnect.
static GLOBAL_DISPLAY: std::sync::atomic::AtomicPtr<Display> =
    std::sync::atomic::AtomicPtr::new(std::ptr::null_mut());

/// Live wl_display pointers.  After wl_display_disconnect frees a Display, Mesa
/// may still call wl_proxy_destroy on already-freed proxies; validating against
/// this set prevents HashMap ops on dangling Display* (the old GPF in
/// hashbrown::HashMap::remove during dri2_teardown_wayland).
struct LiveDisplay(*mut Display);
unsafe impl Send for LiveDisplay {}
unsafe impl Sync for LiveDisplay {}

static LIVE_DISPLAYS: std::sync::Mutex<Vec<LiveDisplay>> =
    std::sync::Mutex::new(Vec::new());

fn live_display_register(d: *mut Display) {
    if d.is_null() {
        return;
    }
    if let Ok(mut live) = LIVE_DISPLAYS.lock() {
        if !live.iter().any(|p| p.0 == d) {
            live.push(LiveDisplay(d));
        }
    }
}

fn live_display_unregister(d: *mut Display) {
    if d.is_null() {
        return;
    }
    if let Ok(mut live) = LIVE_DISPLAYS.lock() {
        live.retain(|p| p.0 != d);
    }
}

fn display_is_live(d: *mut Display) -> bool {
    if d.is_null() {
        return false;
    }
    LIVE_DISPLAYS
        .lock()
        .map(|live| live.iter().any(|p| p.0 == d))
        .unwrap_or(false)
}

#[inline(always)]
unsafe fn disp(d: *mut Display) -> &'static mut Display {
    &mut *d
}

/// Prefer the proxy's own display when it is still live; GLOBAL only as a
/// last resort and only if that display is still registered.
#[inline(always)]
unsafe fn resolve_display(proxy: *const Proxy) -> *mut Display {
    if proxy.is_null() {
        return std::ptr::null_mut();
    }
    let d = (*proxy).display;
    if display_is_live(d) {
        return d;
    }
    let gd = GLOBAL_DISPLAY.load(std::sync::atomic::Ordering::Relaxed);
    if display_is_live(gd) {
        return gd;
    }
    std::ptr::null_mut()
}

unsafe fn proxy_unref(proxy: *mut Proxy) {
    if proxy.is_null() {
        return;
    }
    (*proxy).refcount -= 1;
    if (*proxy).refcount <= 0 {
        let _ = Box::from_raw(proxy);
    }
}

/// libwayland-compatible destroy: mark DESTROYED, drop map entry, unref.
/// Never frees the wl_display object (id 1) — that is wl_display_disconnect.
unsafe fn proxy_destroy_locked(proxy: *mut Proxy) {
    if proxy.is_null() {
        return;
    }
    if (*proxy).flags & WL_PROXY_FLAG_WRAPPER != 0 {
        // Real libwayland aborts; treat as no-op to avoid killing the session.
        return;
    }
    if (*proxy).flags & WL_PROXY_FLAG_DESTROYED != 0 {
        return;
    }
    (*proxy).flags |= WL_PROXY_FLAG_DESTROYED;

    let id = (*proxy).id;
    let display = resolve_display(proxy);
    let is_display_obj = id == 1 || (!display.is_null() && proxy as *mut Display == display);

    if !display.is_null() && !is_display_obj {
        if let Some(&mapped) = (*display).objects.get(&id) {
            if mapped == proxy {
                (*display).objects.remove(&id);
            }
        }
    }

    (*proxy).queue = std::ptr::null_mut();
    let link = &mut (*proxy).queue_link as *mut wl_list;
    if !(*link).prev.is_null() && !(*link).next.is_null() {
        wl_list_remove(link);
        wl_list::init(link);
    }

    if is_display_obj {
        // Display lifetime is owned by wl_display_disconnect.
        return;
    }
    proxy_unref(proxy);
}


#[no_mangle]
pub unsafe extern "C" fn wl_display_connect(name: *const c_char) -> *mut Display {
    let name_str: Option<&str> = if name.is_null() {
        None
    } else {
        CStr::from_ptr(name).to_str().ok()
    };
    eprintln!("[wl-client] wl_display_connect({:?})", name_str);
    match Display::connect(name_str) {
        Ok(d) => {
            let proxy_id = (*d).proxy.id;
            let proxy_off = (&(*d).proxy as *const _ as usize) - (d as usize);
            eprintln!("[wl-client] wl_display_connect → {:p} proxy_offset={} proxy.id={}", d, proxy_off, proxy_id);
            live_display_register(d);
            GLOBAL_DISPLAY.store(d, std::sync::atomic::Ordering::Relaxed);
            d
        }
        Err(e) => {
            eprintln!("[wl-client] wl_display_connect FAILED: {}", e);
            std::ptr::null_mut()
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn wl_display_connect_to_fd(fd: c_int) -> *mut Display {
    use crate::socket::WaylandSocket;
    use std::collections::HashMap;

    let display = Box::new(Display {
        proxy: Proxy::new(1, &crate::interfaces::wl_display_interface, 1, std::ptr::null_mut()),
        socket: WaylandSocket {
            fd,
            send_buf: Vec::new(),
            send_fds: Vec::new(),
            recv_buf: Vec::new(),
            recv_fds: std::collections::VecDeque::new(),
        },
        objects: HashMap::new(),
        next_id: 2,
        event_queue: Vec::new(),
        error: 0,
        error_msg: None,
        mutex: std::sync::Mutex::new(()),
    });
    let raw = Box::into_raw(display);
    (*raw).proxy.display = raw;
    Proxy::init_queue_link(&mut (*raw).proxy);
    (*raw).objects.insert(1, raw as *mut Proxy);
    live_display_register(raw);
    GLOBAL_DISPLAY.store(raw, std::sync::atomic::Ordering::Relaxed);
    raw
}

#[no_mangle]
pub unsafe extern "C" fn wl_display_disconnect(display: *mut Display) {
    if display.is_null() {
        return;
    }
    // Unregister before freeing so concurrent/stray wl_proxy_destroy cannot
    // touch this Display's HashMap after teardown (Mesa EGL probe path).
    live_display_unregister(display);
    let _ = GLOBAL_DISPLAY.compare_exchange(
        display,
        std::ptr::null_mut(),
        std::sync::atomic::Ordering::SeqCst,
        std::sync::atomic::Ordering::Relaxed,
    );
    // Lock via raw pointer so we can still mutably access Display fields.
    let mutex = &(*display).mutex as *const std::sync::Mutex<()>;
    let _guard = (*mutex).lock().unwrap_or_else(|e| e.into_inner());
    let d = &mut *display;
    let ids: Vec<u32> = d.objects.keys().copied().collect();
    for id in ids {
        if id == 1 {
            continue;
        }
        if let Some(p) = d.objects.remove(&id) {
            if p.is_null() {
                continue;
            }
            // Detach before free so a late destroy sees a null display and
            // bails via resolve_display / DESTROYED instead of UAF.
            (*p).display = std::ptr::null_mut();
            if ((*p).flags & WL_PROXY_FLAG_DESTROYED) == 0 {
                (*p).flags |= WL_PROXY_FLAG_DESTROYED;
                let _ = Box::from_raw(p);
            }
        }
    }
    d.proxy.display = std::ptr::null_mut();
    drop(_guard);
    let _ = Box::from_raw(display);
}

#[no_mangle]
pub unsafe extern "C" fn wl_display_get_fd(display: *mut Display) -> c_int {
    disp(display).socket.fd
}

#[no_mangle]
pub unsafe extern "C" fn wl_display_get_error(display: *mut Display) -> c_int {
    disp(display).error
}

#[no_mangle]
pub unsafe extern "C" fn wl_display_get_protocol_error(
    display: *mut Display,
    interface: *mut *const WlInterface,
    id: *mut u32,
) -> u32 {
    let d = disp(display);
    if !interface.is_null() { *interface = std::ptr::null(); }
    if !id.is_null() { *id = 0; }
    d.error as u32
}

#[no_mangle]
pub unsafe extern "C" fn wl_display_flush(display: *mut Display) -> c_int {
    disp(display).flush()
}

#[no_mangle]
pub unsafe extern "C" fn wl_display_roundtrip(display: *mut Display) -> c_int {
    disp(display).roundtrip()
}

#[no_mangle]
pub unsafe extern "C" fn wl_display_roundtrip_queue(
    display: *mut Display,
    _queue: *mut wl_event_queue,
) -> c_int {
    disp(display).roundtrip()
}

#[no_mangle]
pub unsafe extern "C" fn wl_display_dispatch(display: *mut Display) -> c_int {
    let gd = GLOBAL_DISPLAY.load(std::sync::atomic::Ordering::Relaxed);
    let d = if !display.is_null() { disp(display) } else { disp(gd) };
    d.flush();
    if d.socket.recv_blocking().is_err() {
        return -1;
    }
    d.drain_recv_buf();
    0
}

#[no_mangle]
pub unsafe extern "C" fn wl_display_dispatch_pending(display: *mut Display) -> c_int {
    let d = disp(display);
    // Only drain recv_buf; do not recv() again.
    d.drain_recv_buf();
    0
}

#[no_mangle]
pub unsafe extern "C" fn wl_display_dispatch_queue(
    display: *mut Display,
    _queue: *mut wl_event_queue,
) -> c_int {
    wl_display_dispatch(display)
}

#[no_mangle]
pub unsafe extern "C" fn wl_display_dispatch_queue_pending(
    display: *mut Display,
    _queue: *mut wl_event_queue,
) -> c_int {
    wl_display_dispatch_pending(display)
}

#[no_mangle]
pub unsafe extern "C" fn wl_display_dispatch_pending_single(display: *mut Display) -> c_int {
    wl_display_dispatch_pending(display)
}

#[no_mangle]
pub unsafe extern "C" fn wl_display_dispatch_queue_pending_single(
    display: *mut Display,
    queue: *mut wl_event_queue,
) -> c_int {
    wl_display_dispatch_queue_pending(display, queue)
}

#[no_mangle]
pub unsafe extern "C" fn wl_display_dispatch_timeout(
    display: *mut Display,
    _timeout: *const libc::timespec,
) -> c_int {
    wl_display_dispatch(display)
}

#[no_mangle]
pub unsafe extern "C" fn wl_display_dispatch_queue_timeout(
    display: *mut Display,
    queue: *mut wl_event_queue,
    _timeout: *const libc::timespec,
) -> c_int {
    wl_display_dispatch_queue(display, queue)
}

#[no_mangle]
pub unsafe extern "C" fn wl_display_prepare_read(display: *mut Display) -> c_int {
    let _ = display;
    0
}

#[no_mangle]
pub unsafe extern "C" fn wl_display_prepare_read_queue(
    display: *mut Display,
    _queue: *mut wl_event_queue,
) -> c_int {
    wl_display_prepare_read(display)
}

#[no_mangle]
pub unsafe extern "C" fn wl_display_read_events(display: *mut Display) -> c_int {
    disp(display).read_events()
}

#[no_mangle]
pub unsafe extern "C" fn wl_display_cancel_read(_display: *mut Display) {
}

#[no_mangle]
pub unsafe extern "C" fn wl_display_create_queue(display: *mut Display) -> *mut wl_event_queue {
    wl_display_create_queue_with_name(display, std::ptr::null())
}

#[no_mangle]
pub unsafe extern "C" fn wl_display_create_queue_with_name(
    _display: *mut Display,
    _name: *const c_char,
) -> *mut wl_event_queue {
    // Distinct opaque handles; events still dispatch immediately.
    Box::into_raw(Box::new(wl_event_queue { _opaque: 0 }))
}

#[no_mangle]
pub unsafe extern "C" fn wl_event_queue_destroy(queue: *mut wl_event_queue) {
    if !queue.is_null() {
        let _ = Box::from_raw(queue);
    }
}

#[no_mangle]
pub unsafe extern "C" fn wl_event_queue_get_name(_queue: *const wl_event_queue) -> *const c_char {
    std::ptr::null()
}

#[no_mangle]
pub unsafe extern "C" fn wl_display_set_max_buffer_size(_display: *mut Display, _max: usize) {
}


#[no_mangle]
pub unsafe extern "C" fn wl_proxy_create(
    factory: *mut Proxy,
    interface: *const WlInterface,
) -> *mut Proxy {
    if factory.is_null() || interface.is_null() {
        return std::ptr::null_mut();
    }
    let display = resolve_display(factory);
    if display.is_null() {
        return std::ptr::null_mut();
    }
    let _guard = (*display).lock();
    let id = (*display).alloc_id();
    let raw = Proxy::into_heap(Proxy::new(id, interface, (*interface).version as u32, display));
    (*display).register(raw);
    raw
}

#[no_mangle]
pub unsafe extern "C" fn wl_proxy_create_wrapper(proxy: *mut Proxy) -> *mut c_void {
    if proxy.is_null() { return std::ptr::null_mut(); }
    let p = &*proxy;
    let mut wrapper = Box::new(Proxy {
        interface: p.interface,
        implementation: p.implementation,
        id: p.id,
        _id_pad: p._id_pad,
        display: p.display,
        queue: p.queue,
        flags: p.flags | WL_PROXY_FLAG_WRAPPER,
        refcount: 1,
        user_data: p.user_data,
        dispatcher: p.dispatcher,
        version: p.version,
        _version_pad: p._version_pad,
        tag: p.tag,
        queue_link: crate::types::wl_list::new(),
    });
    // Init while still in Box (heap address stable across into_raw).
    Proxy::init_queue_link(&mut *wrapper);
    Box::into_raw(wrapper) as *mut c_void
}

#[no_mangle]
pub unsafe extern "C" fn wl_proxy_wrapper_destroy(proxy: *mut c_void) {
    if proxy.is_null() {
        return;
    }
    let p = proxy as *mut Proxy;
    if ((*p).flags & WL_PROXY_FLAG_WRAPPER) == 0 {
        return;
    }
    let _ = Box::from_raw(p);
}

#[no_mangle]
pub unsafe extern "C" fn wl_proxy_destroy(proxy: *mut Proxy) {
    if proxy.is_null() {
        return;
    }
    // Refuse HashMap ops on a Display that has already been disconnect()'d.
    // Late destroys after Mesa's dri2_teardown are safe no-ops (may leak the
    // proxy box if disconnect already tore the map down — matches libwayland
    // leak-on-disconnect-without-destroy more than a GPF).
    if ((*proxy).flags & WL_PROXY_FLAG_DESTROYED) != 0 {
        return;
    }
    let display = resolve_display(proxy);
    if display.is_null() {
        return;
    }
    let _guard = (*display).lock();
    if ((*proxy).flags & WL_PROXY_FLAG_DESTROYED) != 0 {
        return;
    }
    proxy_destroy_locked(proxy);
}

#[no_mangle]
pub unsafe extern "C" fn wl_proxy_add_listener(
    proxy: *mut Proxy,
    listener: *mut *const c_void,
    data: *mut c_void,
) -> c_int {
    if proxy.is_null() { return -1; }
    if (*proxy).flags & WL_PROXY_FLAG_WRAPPER != 0 { return -1; }
    if !(*proxy).implementation.is_null() || (*proxy).dispatcher.is_some() { return -1; }
    (*proxy).implementation = listener as *const c_void;
    (*proxy).user_data = data;
    (*proxy).dispatcher = None;
    0
}

#[no_mangle]
pub unsafe extern "C" fn wl_proxy_get_listener(proxy: *mut Proxy) -> *const c_void {
    if proxy.is_null() { return std::ptr::null(); }
    (*proxy).implementation
}

#[no_mangle]
pub unsafe extern "C" fn wl_proxy_add_dispatcher(
    proxy: *mut Proxy,
    dispatcher: wl_dispatcher_func_t,
    implementation: *const c_void,
    data: *mut c_void,
) -> c_int {
    if proxy.is_null() { return -1; }
    if (*proxy).flags & WL_PROXY_FLAG_WRAPPER != 0 { return -1; }
    if !(*proxy).implementation.is_null() || (*proxy).dispatcher.is_some() { return -1; }
    (*proxy).implementation = implementation;
    (*proxy).dispatcher = dispatcher;
    (*proxy).user_data = data;
    0
}

#[no_mangle]
pub unsafe extern "C" fn wl_proxy_marshal_array_flags(
    proxy: *mut Proxy,
    opcode: u32,
    interface: *const WlInterface,
    version: u32,
    flags: u32,
    args: *mut wl_argument,
) -> *mut Proxy {
    if proxy.is_null() { return std::ptr::null_mut(); }

    let display = resolve_display(proxy);
    if display.is_null() { return std::ptr::null_mut(); }
    let _guard = (*display).lock();

    if (*proxy).flags & WL_PROXY_FLAG_DESTROYED != 0 {
        return std::ptr::null_mut();
    }

    let proxy_iface = (*proxy).interface;

    fn is_valid_iface(p: *const WlInterface) -> bool {
        !p.is_null() && (p as usize) % 8 == 0
    }
    let method_count: i32 = if is_valid_iface(proxy_iface) { (*proxy_iface).method_count } else { 0 };
    let methods_ok = is_valid_iface(proxy_iface)
        && (opcode as i32) < method_count
        && { let m = (*proxy_iface).methods as usize; m != 0 && m % 8 == 0 };

    let arg_count: usize = if methods_ok {
        let msg = &*(*proxy_iface).methods.add(opcode as usize);
        let sig = CStr::from_ptr(msg.signature).to_bytes();
        sig.iter().filter(|&&c| !matches!(c, b'?' | b'0'..=b'9')).count()
    } else {
        0
    };

    let arg_slice = if arg_count > 0 && !args.is_null() {
        std::slice::from_raw_parts(args, arg_count)
    } else {
        &[]
    };

    let iface_valid = !interface.is_null() && (interface as usize) % 8 == 0;

    let mut new_proxy: *mut Proxy = std::ptr::null_mut();
    if iface_valid {
        let id = (*display).alloc_id();
        let vv = if version > 0 { version } else { (*interface).version as u32 };
        // new_id inherits the factory proxy's queue (Mesa eglSwapBuffers).
        let mut p = Proxy::new(id, interface, vv, display);
        p.queue = (*proxy).queue;
        new_proxy = Proxy::into_heap(p);
        (*display).register(new_proxy);

        if methods_ok {
            let msg = &*(*proxy_iface).methods.add(opcode as usize);
            let sig = CStr::from_ptr(msg.signature).to_bytes();
            let mut ai = 0usize;
            for &ch in sig {
                match ch {
                    b'?' | b'0'..=b'9' => {}
                    b'n' => {
                        if !args.is_null() { (*args.add(ai)).n = id; }
                        ai += 1;
                    }
                    _ => { ai += 1; }
                }
            }
        }
    }

    let safe_iface = if is_valid_iface(proxy_iface) { proxy_iface } else { std::ptr::null() };
    let (payload, fds) = wire::encode_args(safe_iface, opcode, arg_slice);
    if !fds.is_empty() {
        eprintln!("[wl-client] marshal fds: proxy_id={} op={} fds={:?}", (*proxy).id, opcode, fds);
    }
    let msg = wire::build_message((*proxy).id, opcode as u16, &payload);
    (*display).socket.queue(&msg, &fds);

    if flags & 1 != 0 {
        // WL_MARSHAL_FLAG_DESTROY — same as libwayland (under display lock).
        proxy_destroy_locked(proxy);
    }

    new_proxy
}


#[no_mangle]
pub unsafe extern "C" fn wl_proxy_set_user_data(proxy: *mut Proxy, data: *mut c_void) {
    if !proxy.is_null() { (*proxy).user_data = data; }
}

#[no_mangle]
pub unsafe extern "C" fn wl_proxy_get_user_data(proxy: *mut Proxy) -> *mut c_void {
    if proxy.is_null() { std::ptr::null_mut() } else { (*proxy).user_data }
}

#[no_mangle]
pub unsafe extern "C" fn wl_proxy_get_version(proxy: *mut Proxy) -> u32 {
    if proxy.is_null() { 0 } else { (*proxy).version }
}

#[no_mangle]
pub unsafe extern "C" fn wl_proxy_get_id(proxy: *mut Proxy) -> u32 {
    if proxy.is_null() { 0 } else { (*proxy).id }
}

#[no_mangle]
pub unsafe extern "C" fn wl_proxy_get_interface(proxy: *mut Proxy) -> *const WlInterface {
    if proxy.is_null() { std::ptr::null() } else { (*proxy).interface }
}

#[no_mangle]
pub unsafe extern "C" fn wl_proxy_get_class(proxy: *mut Proxy) -> *const c_char {
    if proxy.is_null() { return std::ptr::null(); }
    let iface = (*proxy).interface;
    if iface.is_null() { std::ptr::null() } else { (*iface).name }
}

#[no_mangle]
pub unsafe extern "C" fn wl_proxy_set_queue(
    proxy: *mut Proxy,
    queue: *mut wl_event_queue,
) {
    if !proxy.is_null() { (*proxy).queue = queue; }
}

#[no_mangle]
pub unsafe extern "C" fn wl_proxy_get_queue(proxy: *const Proxy) -> *mut wl_event_queue {
    if proxy.is_null() { std::ptr::null_mut() } else { (*proxy).queue }
}

#[no_mangle]
pub unsafe extern "C" fn wl_proxy_get_display(proxy: *mut Proxy) -> *mut Display {
    resolve_display(proxy)
}

#[no_mangle]
pub unsafe extern "C" fn wl_proxy_marshal_array(
    proxy: *mut Proxy,
    opcode: u32,
    args: *mut wl_argument,
) {
    let _ = wl_proxy_marshal_array_flags(proxy, opcode, std::ptr::null(), 0, 0, args);
}

#[no_mangle]
pub unsafe extern "C" fn wl_proxy_marshal_array_constructor(
    proxy: *mut Proxy,
    opcode: u32,
    args: *mut wl_argument,
    interface: *const WlInterface,
) -> *mut Proxy {
    wl_proxy_marshal_array_flags(proxy, opcode, interface, 0, 0, args)
}

#[no_mangle]
pub unsafe extern "C" fn wl_proxy_marshal_array_constructor_versioned(
    proxy: *mut Proxy,
    opcode: u32,
    args: *mut wl_argument,
    interface: *const WlInterface,
    version: u32,
) -> *mut Proxy {
    wl_proxy_marshal_array_flags(proxy, opcode, interface, version, 0, args)
}

#[no_mangle]
pub unsafe extern "C" fn wl_proxy_get_tag(proxy: *mut Proxy) -> *const *const c_char {
    if proxy.is_null() { std::ptr::null() } else { (*proxy).tag }
}

#[no_mangle]
pub unsafe extern "C" fn wl_proxy_set_tag(
    proxy: *mut Proxy,
    tag: *const *const c_char,
) {
    if !proxy.is_null() { (*proxy).tag = tag; }
}


#[no_mangle]
pub unsafe extern "C" fn wl_list_init(list: *mut wl_list) {
    if list.is_null() { return; }
    (*list).prev = list;
    (*list).next = list;
}

#[no_mangle]
pub unsafe extern "C" fn wl_list_insert(list: *mut wl_list, elm: *mut wl_list) {
    (*elm).prev = list;
    (*elm).next = (*list).next;
    (*(*list).next).prev = elm;
    (*list).next = elm;
}

#[no_mangle]
pub unsafe extern "C" fn wl_list_remove(elm: *mut wl_list) {
    if elm.is_null() {
        return;
    }
    // Uninitialized / already-detached nodes have null links.
    if (*elm).prev.is_null() || (*elm).next.is_null() {
        wl_list::init(elm);
        return;
    }
    (*(*elm).prev).next = (*elm).next;
    (*(*elm).next).prev = (*elm).prev;
    wl_list::init(elm);
}

#[no_mangle]
pub unsafe extern "C" fn wl_list_length(list: *const wl_list) -> c_int {
    let mut count = 0i32;
    let mut e = (*list).next;
    while e != list as *mut _ {
        count += 1;
        e = (*e).next;
    }
    count
}

#[no_mangle]
pub unsafe extern "C" fn wl_list_empty(list: *const wl_list) -> c_int {
    ((*list).next == list as *mut _) as c_int
}

#[no_mangle]
pub unsafe extern "C" fn wl_list_insert_list(list: *mut wl_list, other: *mut wl_list) {
    if wl_list_empty(other) != 0 { return; }
    (*(*other).prev).next = (*list).next;
    (*(*list).next).prev = (*other).prev;
    (*list).next = (*other).next;
    (*(*other).next).prev = list;
}


#[no_mangle]
pub unsafe extern "C" fn wl_array_init(array: *mut wl_array) {
    (*array).size  = 0;
    (*array).alloc = 0;
    (*array).data  = std::ptr::null_mut();
}

#[no_mangle]
pub unsafe extern "C" fn wl_array_release(array: *mut wl_array) {
    if !(*array).data.is_null() {
        libc::free((*array).data);
        (*array).data = std::ptr::null_mut();
    }
    (*array).size  = 0;
    (*array).alloc = 0;
}

#[no_mangle]
pub unsafe extern "C" fn wl_array_add(array: *mut wl_array, size: usize) -> *mut c_void {
    let need = (*array).size + size;
    if need > (*array).alloc {
        let new_alloc = (need * 2).max(64);
        let new_data = libc::realloc((*array).data, new_alloc);
        if new_data.is_null() { return std::ptr::null_mut(); }
        (*array).alloc = new_alloc;
        (*array).data = new_data;
    }
    let ptr = ((*array).data as *mut u8).add((*array).size) as *mut c_void;
    (*array).size += size;
    ptr
}

#[no_mangle]
pub unsafe extern "C" fn wl_array_copy(array: *mut wl_array, source: *const wl_array) -> c_int {
    let src = &*source;
    let dst_ptr = wl_array_add(array, src.size);
    if dst_ptr.is_null() { return -1; }
    std::ptr::copy_nonoverlapping(src.data as *const u8, dst_ptr as *mut u8, src.size);
    0
}


type WlLogHandlerFn = unsafe extern "C" fn(*const c_char, ...);

static mut LOG_HANDLER: Option<WlLogHandlerFn> = None;

#[no_mangle]
pub unsafe extern "C" fn wl_log_set_handler_client(handler: WlLogHandlerFn) {
    LOG_HANDLER = Some(handler);
}


#[no_mangle]
pub extern "C" fn wl_fixed_to_double(f: i32) -> f64 {
    crate::types::wl_fixed_to_double(f)
}

#[no_mangle]
pub extern "C" fn wl_fixed_from_double(d: f64) -> i32 {
    crate::types::wl_fixed_from_double(d)
}

#[no_mangle]
pub extern "C" fn wl_fixed_from_int(i: i32) -> i32 {
    i * 256
}
