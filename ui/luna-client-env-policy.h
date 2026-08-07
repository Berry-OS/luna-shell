#ifndef LUNA_CLIENT_ENV_POLICY_H
#define LUNA_CLIENT_ENV_POLICY_H

/*
 * Child rendering policy for luna-shell.
 *
 * The old session fallback forced three Mesa knobs on every ordinary child:
 *   LIBGL_ALWAYS_SOFTWARE=1
 *   MESA_LOADER_DRIVER_OVERRIDE=llvmpipe
 *   GALLIUM_DRIVER=llvmpipe
 *
 * For the stable wl_shm path we only need LIBGL_ALWAYS_SOFTWARE. Let Mesa
 * choose its normal software loader/Gallium driver instead of overriding both
 * layers independently; this avoids driver-name mismatches and keeps the
 * fallback portable across Mesa builds.
 *
 * Hardware clients remain explicit opt-in because the compositor currently
 * advertises only LINEAR dmabuf. Both LUNA_CLIENT_GPU=1 and
 * LUNA_ENABLE_DMABUF=1 are required before the generic software fallback is
 * suppressed. Browsers are unaffected: their launcher branch explicitly sets
 * its conservative llvmpipe variables with overwrite=1.
 *
 * This header is force-included before luna-shell.c. Keep it freestanding so
 * luna-shell.c can define _GNU_SOURCE before any libc header is included.
 */

extern char *getenv(const char *name);

static inline int luna_client_env_policy_eq_ci(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + ('a' - 'A'));
        if (ca != cb) return 0;
    }
    return *a == 0 && *b == 0;
}

static inline int luna_client_env_policy_true(const char *value) {
    return value && *value &&
           (luna_client_env_policy_eq_ci(value, "1") ||
            luna_client_env_policy_eq_ci(value, "yes") ||
            luna_client_env_policy_eq_ci(value, "true") ||
            luna_client_env_policy_eq_ci(value, "on"));
}

static inline char *luna_client_env_policy_getenv(const char *name) {
    char *value = getenv(name);
    if (value) return value;

    /* Never let the generic child fallback force Mesa's loader/Gallium driver.
     * Returning a synthetic value only affects luna-shell's presence check; it
     * is not inserted into environ and therefore is invisible to the child. */
    if (luna_client_env_policy_eq_ci(name, "MESA_LOADER_DRIVER_OVERRIDE") ||
        luna_client_env_policy_eq_ci(name, "GALLIUM_DRIVER")) {
        return (char *)"0";
    }

    if (luna_client_env_policy_eq_ci(name, "LIBGL_ALWAYS_SOFTWARE") &&
        luna_client_env_policy_true(getenv("LUNA_CLIENT_GPU")) &&
        luna_client_env_policy_true(getenv("LUNA_ENABLE_DMABUF")) &&
        !luna_client_env_policy_true(getenv("LUNA_EGL_SOFTWARE"))) {
        return (char *)"0";
    }

    return (char *)0;
}

#define getenv(name) luna_client_env_policy_getenv(name)

#endif /* LUNA_CLIENT_ENV_POLICY_H */
