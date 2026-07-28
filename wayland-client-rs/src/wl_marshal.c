/*
 * Copyright © 2026 Yuichiro Nakada / Project Vespera
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* Variadic marshal helpers — Rust c_variadic is unstable. */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

typedef int32_t wl_fixed_t;

struct wl_array {
    size_t  size;
    size_t  alloc;
    void   *data;
};

union wl_argument {
    int32_t         i;
    uint32_t        u;
    wl_fixed_t      f;
    const char     *s;
    void           *o;
    uint32_t        n;
    struct wl_array *a;
    int32_t         h;
};

struct wl_message {
    const char  *name;
    const char  *signature;
    const void **types;
};

struct wl_interface {
    const char               *name;
    int                       version;
    int                       method_count;
    const struct wl_message  *methods;
    int                       event_count;
    const struct wl_message  *events;
};

extern void *wl_proxy_marshal_array_flags(
    void *proxy,
    uint32_t opcode,
    const struct wl_interface *interface,
    uint32_t version,
    uint32_t flags,
    union wl_argument *args);

static int fill_args_from_va(const char *sig, va_list va, union wl_argument *args, int max)
{
    int argc = 0;
    if (!sig) return 0;
    for (const char *p = sig; *p && argc < max; ++p) {
        switch (*p) {
        case '?':
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            break;
        case 'i':
            args[argc++].i = va_arg(va, int32_t);
            break;
        case 'u':
            args[argc++].u = va_arg(va, uint32_t);
            break;
        case 'f':
            args[argc++].f = (wl_fixed_t)va_arg(va, int);
            break;
        case 's':
            args[argc++].s = va_arg(va, const char *);
            break;
        case 'o':
            args[argc++].o = va_arg(va, void *);
            break;
        case 'n':
            (void)va_arg(va, void *);
            args[argc++].n = 0;
            break;
        case 'a':
            args[argc++].a = va_arg(va, struct wl_array *);
            break;
        case 'h':
            args[argc++].h = va_arg(va, int);
            break;
        default:
            break;
        }
    }
    return argc;
}

static const char *proxy_method_sig(void *proxy, uint32_t opcode)
{
    if (!proxy) return NULL;
    const struct wl_interface *proxy_iface =
        *(const struct wl_interface **)proxy;
    if (proxy_iface && (int)opcode < proxy_iface->method_count &&
            proxy_iface->methods)
        return proxy_iface->methods[opcode].signature;
    return NULL;
}

void *wl_proxy_marshal_flags(
    void                     *proxy,
    uint32_t                  opcode,
    const struct wl_interface *interface,
    uint32_t                  version,
    uint32_t                  flags,
    ...)
{
    if (!proxy) return NULL;
    union wl_argument args[24];
    va_list va;
    va_start(va, flags);
    int argc = fill_args_from_va(proxy_method_sig(proxy, opcode), va, args, 24);
    va_end(va);
    return wl_proxy_marshal_array_flags(
        proxy, opcode, interface, version, flags,
        argc > 0 ? args : NULL);
}

void wl_proxy_marshal(void *proxy, uint32_t opcode, ...)
{
    if (!proxy) return;
    union wl_argument args[24];
    va_list va;
    va_start(va, opcode);
    int argc = fill_args_from_va(proxy_method_sig(proxy, opcode), va, args, 24);
    va_end(va);
    (void)wl_proxy_marshal_array_flags(
        proxy, opcode, NULL, 0, 0, argc > 0 ? args : NULL);
}

void *wl_proxy_marshal_constructor(
    void *proxy, uint32_t opcode,
    const struct wl_interface *interface, ...)
{
    if (!proxy) return NULL;
    union wl_argument args[24];
    va_list va;
    va_start(va, interface);
    int argc = fill_args_from_va(proxy_method_sig(proxy, opcode), va, args, 24);
    va_end(va);
    return wl_proxy_marshal_array_flags(
        proxy, opcode, interface, 0, 0, argc > 0 ? args : NULL);
}

void *wl_proxy_marshal_constructor_versioned(
    void *proxy, uint32_t opcode,
    const struct wl_interface *interface, uint32_t version, ...)
{
    if (!proxy) return NULL;
    union wl_argument args[24];
    va_list va;
    va_start(va, version);
    int argc = fill_args_from_va(proxy_method_sig(proxy, opcode), va, args, 24);
    va_end(va);
    return wl_proxy_marshal_array_flags(
        proxy, opcode, interface, version, 0, argc > 0 ? args : NULL);
}
