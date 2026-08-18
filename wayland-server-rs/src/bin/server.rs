/*
 * Copyright © 2026 Yuichiro Nakada / Project Vespera
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

use wayland_server::render::software::SoftwareBackend;
use wayland_server::render::Backend;

fn main() {
  let mut socket = "wayland-1".to_string();
  let mut backend_kind = "software".to_string();
  let mut screenshot: Option<String> = None;
  let mut fbdev: Option<String> = None;
  let mut width = 1280u32;
  let mut height = 720u32;
  let mut port = 8081u16;
  let mut tty: Option<String> = None;
  let mut with_input = true;

  let mut args = std::env::args().skip(1);
  while let Some(a) = args.next() {
    match a.as_str() {
      "--socket" => socket = args.next().unwrap_or(socket),
      "--backend" => backend_kind = args.next().unwrap_or(backend_kind),
      "--screenshot" => screenshot = args.next(),
      "--fbdev" => fbdev = args.next(),
      "--tty" => tty = args.next(),
      "--no-input" => with_input = false,
      "--port" => {
        if let Some(p) = args.next() {
          port = p.parse().unwrap_or(port);
        }
      }
      "--size" => {
        if let Some(s) = args.next() {
          if let Some((w, h)) = s.split_once('x') {
            width = w.parse().unwrap_or(width);
            height = h.parse().unwrap_or(height);
          }
        }
      }
      "-h" | "--help" => {
        eprintln!(
          "luna-compositor [--socket NAME] [--backend software|dri|webgl] \
                     [--screenshot PATH] [--fbdev DEV] [--size WxH] [--port PORT] \
                     [--tty /dev/ttyN] [--no-input]"
        );
        return;
      }
      _ => eprintln!("[luna-compositor] unknown argument: {}", a),
    }
  }
  #[cfg(not(all(target_os = "linux", feature = "dri")))]
  let _ = (&tty, with_input);

  let backend: Box<dyn Backend> = match backend_kind.as_str() {
    #[cfg(all(target_os = "linux", feature = "dri"))]
    "dri" => match wayland_server::render::dri::DriBackend::open_any(tty.as_deref(), with_input) {
      Some(b) => {
        eprintln!("[luna-compositor] using DRI (DRM/KMS) backend");
        Box::new(b)
      }
      None => {
        eprintln!("[luna-compositor] DRI init failed, falling back to software{}", if fbdev.is_some() { " + fbdev" } else { " (no --fbdev: nothing will appear on screen)" });
        Box::new(make_software(width, height, screenshot.clone(), fbdev.clone()))
      }
    },
    #[cfg(not(all(target_os = "linux", feature = "dri")))]
    "dri" => {
      eprintln!(
        "[luna-compositor] dri backend requires rebuild with feature=\"dri\"; \
                 using software"
      );
      Box::new(make_software(width, height, screenshot.clone(), fbdev.clone()))
    }
    #[cfg(feature = "webgl")]
    "webgl" => {
      eprintln!("[luna-compositor] using WebGL browser streaming backend");
      Box::new(wayland_server::render::webgl_server::WebGlServerBackend::new(width, height, port))
    }
    #[cfg(not(feature = "webgl"))]
    "webgl" => {
      eprintln!(
        "[luna-compositor] webgl backend requires rebuild with feature=\"webgl\" \
                 (cargo build --features webgl); using software"
      );
      Box::new(make_software(width, height, screenshot.clone(), fbdev.clone()))
    }
    _ => {
      if fbdev.is_none() && screenshot.is_none() {
        eprintln!(
          "[luna-compositor] software backend with no --fbdev/--screenshot: \
                     frames are composed but not shown on a display"
        );
      }
      Box::new(make_software(width, height, screenshot.clone(), fbdev.clone()))
    }
  };

  if let Err(e) = wayland_server::run(&socket, backend) {
    eprintln!("[luna-compositor] failed to start: {}", e);
    std::process::exit(1);
  }
}

fn make_software(w: u32, h: u32, screenshot: Option<String>, fbdev: Option<String>) -> SoftwareBackend {
  let mut b = SoftwareBackend::new(w, h);
  if let Some(p) = screenshot {
    b = b.with_screenshot(p);
  }
  if let Some(dev) = fbdev {
    b = b.with_fbdev(&dev);
  }
  b
}
