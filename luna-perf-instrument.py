#!/usr/bin/env python3
from pathlib import Path
import sys

ROOT = Path.cwd()

def replace_once(path: Path, old: str, new: str):
    text = path.read_text()
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected exactly one match, found {count}")
    path.write_text(text.replace(old, new, 1))
    print(f"patched {path}")

shell = ROOT / "ui/luna-shell.c"
server = ROOT / "wayland-server-rs/src/server.rs"
dri = ROOT / "wayland-server-rs/src/render/dri.rs"
for p in (shell, server, dri):
    if not p.exists():
        raise SystemExit(f"run this from the luna-shell repository root; missing {p}")

# ---- luna-shell ---------------------------------------------------------
old = '''static double plat_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    static struct timespec t0;
    static int started = 0;
    if (!started) { t0 = ts; started = 1; }
    return (double)(ts.tv_sec - t0.tv_sec) + (double)(ts.tv_nsec - t0.tv_nsec) / 1e9;
}
'''
new = old + r'''
/* Lightweight hitch profiler. Enable with LUNA_PERF=1; threshold in ms via
 * LUNA_PERF_MS (default 4). Only slow sections are printed. */
static int shell_perf_enabled(void) {
    static int initialized = 0, enabled = 0;
    if (!initialized) {
        const char* v = getenv("LUNA_PERF");
        enabled = v && *v && strcmp(v, "0") && strcasecmp(v, "false");
        initialized = 1;
    }
    return enabled;
}

static double shell_perf_threshold_ms(void) {
    static int initialized = 0;
    static double threshold = 4.0;
    if (!initialized) {
        const char* v = getenv("LUNA_PERF_MS");
        if (v && *v) {
            char* end = NULL;
            double parsed = strtod(v, &end);
            if (end != v && parsed >= 0.0) threshold = parsed;
        }
        initialized = 1;
    }
    return threshold;
}

static void shell_perf_log(const char* label, double started) {
    if (!shell_perf_enabled()) return;
    double ms = (plat_time() - started) * 1000.0;
    if (ms >= shell_perf_threshold_ms()) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        double stamp = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
        fprintf(stderr, "[luna-perf/shell] t=%.3f %-28s %8.3f ms\n",
                stamp, label, ms);
    }
}
'''
replace_once(shell, old, new)

replace_once(shell,
'''        if (!ui_dragging) poll_shell_state();
''',
'''        if (!ui_dragging) {
            double perf_t = plat_time();
            poll_shell_state();
            shell_perf_log("poll_shell_state", perf_t);
        }
''')

replace_once(shell,
'''        if (!interaction_busy) {
            update_async_status();
            wifi_tick();
            weather_tick();
            update_launchpad_filter();
            wm_settings_retry_tick();
            session_restore_tick();
        }
''',
'''        if (!interaction_busy) {
            double perf_t = plat_time();
            update_async_status();
            shell_perf_log("update_async_status", perf_t);
            perf_t = plat_time();
            wifi_tick();
            shell_perf_log("wifi_tick", perf_t);
            perf_t = plat_time();
            weather_tick();
            shell_perf_log("weather_tick", perf_t);
            perf_t = plat_time();
            update_launchpad_filter();
            shell_perf_log("launchpad_filter", perf_t);
            perf_t = plat_time();
            wm_settings_retry_tick();
            session_restore_tick();
            shell_perf_log("session_maintenance", perf_t);
        }
''')

replace_once(shell,
'''            unsigned settle_mask = 0;
            int settling = luna_update_settling_mask(g_now, dt, surf_roots,
                                                      LUNA_SURF_COUNT, &settle_mask);
            if (!interaction_busy && g_now - g_last_bg_anim_check >= 1.0) {
                g_last_bg_anim_check = g_now;
                g_bg_animated = luna_css_anim_running_under(g_surfs[LUNA_SURF_BG].root_idx);
            }
            wl_surfs_update();
''',
'''            unsigned settle_mask = 0;
            double perf_t = plat_time();
            int settling = luna_update_settling_mask(g_now, dt, surf_roots,
                                                      LUNA_SURF_COUNT, &settle_mask);
            shell_perf_log("update_settling_mask", perf_t);
            if (!interaction_busy && g_now - g_last_bg_anim_check >= 1.0) {
                g_last_bg_anim_check = g_now;
                perf_t = plat_time();
                g_bg_animated = luna_css_anim_running_under(g_surfs[LUNA_SURF_BG].root_idx);
                shell_perf_log("css_anim_scan", perf_t);
            }
            perf_t = plat_time();
            wl_surfs_update();
            shell_perf_log("wl_surfs_update", perf_t);
''')

replace_once(shell,
'''            for (int i = 0; i < LUNA_SURF_COUNT; i++) {
                wl_surf_render(&g_surfs[i], i);
            }
''',
'''            perf_t = plat_time();
            for (int i = 0; i < LUNA_SURF_COUNT; i++) {
                double surf_t = plat_time();
                wl_surf_render(&g_surfs[i], i);
                if (shell_perf_enabled()) {
                    double surf_ms = (plat_time() - surf_t) * 1000.0;
                    if (surf_ms >= shell_perf_threshold_ms())
                        struct timespec perf_ts;
                        clock_gettime(CLOCK_REALTIME, &perf_ts);
                        double perf_stamp = (double)perf_ts.tv_sec +
                            (double)perf_ts.tv_nsec / 1e9;
                        fprintf(stderr, "[luna-perf/shell] t=%.3f surface[%02d] %-16s %8.3f ms\n",
                                perf_stamp, i,
                                g_surfs[i].name ? g_surfs[i].name : "?", surf_ms);
                }
            }
            shell_perf_log("wl_surf_render(all)", perf_t);
''')

replace_once(shell,
'''        } else {
            int settling = luna_update_settling(g_now, dt);
            if (!interaction_busy && g_now - g_last_bg_anim_check >= 1.0) {
                g_last_bg_anim_check = g_now;
                g_bg_animated = luna_css_anim_running_under(-1);
            }
''',
'''        } else {
            double perf_t = plat_time();
            int settling = luna_update_settling(g_now, dt);
            shell_perf_log("update_settling", perf_t);
            if (!interaction_busy && g_now - g_last_bg_anim_check >= 1.0) {
                g_last_bg_anim_check = g_now;
                perf_t = plat_time();
                g_bg_animated = luna_css_anim_running_under(-1);
                shell_perf_log("css_anim_scan", perf_t);
            }
''')

replace_once(shell,
'''                luna_render(fbw, fbh);
                /* The default framebuffer contents are undefined after EGL swap
''',
'''                perf_t = plat_time();
                luna_render(fbw, fbh);
                shell_perf_log("luna_render", perf_t);
                /* The default framebuffer contents are undefined after EGL swap
''')

replace_once(shell,
'''                g_backend->swap_buffers();
                g_frame_dirty = 0;
''',
'''                perf_t = plat_time();
                g_backend->swap_buffers();
                shell_perf_log("swap_buffers", perf_t);
                g_frame_dirty = 0;
''')

# ---- compositor ---------------------------------------------------------
replace_once(server,
'''use std::time::{SystemTime, UNIX_EPOCH};
''',
'''use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};
''')

replace_once(server,
'''  /// Scratch list for the ordered nonblocking client pass after input edges.
  /// Keeping its capacity avoids allocator churn during click/drag bursts.
  poll_client_fds: Vec<RawFd>,
}
''',
'''  /// Scratch list for the ordered nonblocking client pass after input edges.
  /// Keeping its capacity avoids allocator churn during click/drag bursts.
  poll_client_fds: Vec<RawFd>,
  /// Lightweight hitch profiler, enabled by LUNA_PERF=1.
  perf_enabled: bool,
  perf_threshold: Duration,
}
''')

replace_once(server,
'''    let main_device = detect_drm_device(backend.drm_render_device()).unwrap_or(0);

    let mut server = Server {
''',
'''    let main_device = detect_drm_device(backend.drm_render_device()).unwrap_or(0);
    let perf_enabled = std::env::var("LUNA_PERF")
      .map(|v| !v.is_empty() && v != "0" && !v.eq_ignore_ascii_case("false"))
      .unwrap_or(false);
    let perf_ms = std::env::var("LUNA_PERF_MS")
      .ok().and_then(|v| v.parse::<f64>().ok()).filter(|v| *v >= 0.0).unwrap_or(4.0);

    let mut server = Server {
''')

replace_once(server,
'''      compose_gpu_surfaces: Vec::new(),
      poll_client_fds: Vec::new(),
    };
''',
'''      compose_gpu_surfaces: Vec::new(),
      poll_client_fds: Vec::new(),
      perf_enabled,
      perf_threshold: Duration::from_secs_f64(perf_ms / 1000.0),
    };
''')

replace_once(server,
'''        if self.dirty {
          self.composite_and_present();
          self.dirty = false;
          self.cursor_dirty = false;
''',
'''        if self.dirty {
          let perf_t = Instant::now();
          self.composite_and_present();
          let elapsed = perf_t.elapsed();
          if self.perf_enabled && elapsed >= self.perf_threshold {
            let stamp = SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_secs_f64();
            eprintln!("[luna-perf/compositor] t={:.3} composite_and_present {:8.3} ms clients={} windows={}",
              stamp, elapsed.as_secs_f64() * 1000.0, self.clients.len(), self.window_stack.len());
          }
          self.dirty = false;
          self.cursor_dirty = false;
''')

replace_once(server,
'''      if self.shell_state_dirty {
        self.export_shell_state(false);
      }

      for c in self.clients.values_mut() {
        c.conn.flush();
      }
''',
'''      if self.shell_state_dirty {
        let perf_t = Instant::now();
        self.export_shell_state(false);
        let elapsed = perf_t.elapsed();
        if self.perf_enabled && elapsed >= self.perf_threshold {
          let stamp = SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_secs_f64();
          eprintln!("[luna-perf/compositor] t={:.3} export_shell_state    {:8.3} ms",
            stamp, elapsed.as_secs_f64() * 1000.0);
        }
      }

      let perf_t = Instant::now();
      for c in self.clients.values_mut() {
        c.conn.flush();
      }
      let elapsed = perf_t.elapsed();
      if self.perf_enabled && elapsed >= self.perf_threshold {
        let stamp = SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_secs_f64();
        eprintln!("[luna-perf/compositor] t={:.3} client_flush          {:8.3} ms",
          stamp, elapsed.as_secs_f64() * 1000.0);
      }
''')

# ---- DRI ----------------------------------------------------------------
replace_once(dri,
'''use std::sync::mpsc;
''',
'''use std::sync::{mpsc, OnceLock};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};
''')

# Insert profiler helpers before Backend impl.
marker = '''impl Backend for DriBackend {
'''
helpers = r'''fn dri_perf_threshold() -> Option<Duration> {
  static PERF: OnceLock<Option<Duration>> = OnceLock::new();
  *PERF.get_or_init(|| {
    let enabled = std::env::var("LUNA_PERF")
      .map(|v| !v.is_empty() && v != "0" && !v.eq_ignore_ascii_case("false"))
      .unwrap_or(false);
    if !enabled { return None; }
    let ms = std::env::var("LUNA_PERF_MS")
      .ok().and_then(|v| v.parse::<f64>().ok()).filter(|v| *v >= 0.0).unwrap_or(4.0);
    Some(Duration::from_secs_f64(ms / 1000.0))
  })
}

fn dri_perf_log(label: &str, started: Instant) {
  let Some(threshold) = dri_perf_threshold() else { return; };
  let elapsed = started.elapsed();
  if elapsed >= threshold {
    let stamp = SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_secs_f64();
    eprintln!("[luna-perf/dri] t={:.3} {:24} {:8.3} ms",
      stamp, label, elapsed.as_secs_f64() * 1000.0);
  }
}

'''
replace_once(dri, marker, helpers + marker)

replace_once(dri,
'''    let back = 1 - self.front;
    if !self.copy_damaged(fb, back, damage) {
''',
'''    let present_t = Instant::now();
    let back = 1 - self.front;
    let copy_t = Instant::now();
    if !self.copy_damaged(fb, back, damage) {
      dri_perf_log("copy_damaged(noop)", copy_t);
      dri_perf_log("present_damage(noop)", present_t);
''')

replace_once(dri,
'''      return;
    }
    if unsafe { self.flip_to(back) } {
''',
'''      return;
    }
    dri_perf_log("copy_damaged", copy_t);
    let flip_t = Instant::now();
    if unsafe { self.flip_to(back) } {
''')

replace_once(dri,
'''      if !self.flip_pending {
        self.release_retired();
      }
    }
  }

  fn present_dmabuf''',
'''      if !self.flip_pending {
        self.release_retired();
      }
    }
    dri_perf_log("flip_to", flip_t);
    dri_perf_log("present_damage", present_t);
  }

  fn present_dmabuf''')

replace_once(dri,
'''  unsafe fn set_crtc_fb(&mut self, fb_id: u32) -> bool {
    let cid = self.connector_id;
''',
'''  unsafe fn set_crtc_fb(&mut self, fb_id: u32) -> bool {
    let perf_t = Instant::now();
    let cid = self.connector_id;
''')

replace_once(dri,
'''    ioctl(self.fd, iowr::<ModeCrtc>(0xA2), &mut crtc) == 0
  }
''',
'''    let ok = ioctl(self.fd, iowr::<ModeCrtc>(0xA2), &mut crtc) == 0;
    dri_perf_log("DRM_MODE_SETCRTC", perf_t);
    ok
  }
''')

print("\nDone. Rebuild and run with:")
print("  make build-desktop-system")
print("  LUNA_PERF=1 LUNA_PERF_MS=2 ./luna-session 2> /tmp/luna-perf.log")
print("Then reproduce the hitch for 15-30 seconds and inspect:")
print("  grep 'luna-perf' /tmp/luna-perf.log")
