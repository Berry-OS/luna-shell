/*
 * Copyright © 2026 Yuichiro Nakada / Project Vespera
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

//! Title text for server-side decorations (stb_truetype via `title_text.c`).
//!
//! The same typeface the shell's dialogs use (Inter, with a CJK fallback), so a
//! window titlebar reads like the Settings dialog's.

use std::collections::HashMap;
use std::ffi::CString;
use std::rc::Rc;

unsafe extern "C" {
  fn luna_tt_add(path: *const libc::c_char) -> i32;
  fn luna_tt_count() -> i32;
  fn luna_tt_covers(cp: u32) -> i32;
  fn luna_tt_text(s: *const u8, len: usize, px: i32, bold: i32, w: *mut i32, h: *mut i32, asc: *mut i32) -> *mut u8;
  fn luna_tt_free(p: *mut u8);
}

/// A rendered line: coverage mask plus the ascent (baseline offset).
pub struct TitleMask {
  pub w: i32,
  pub h: i32,
  pub ascent: i32,
  pub alpha: Vec<u8>,
}

#[derive(Default)]
pub struct TitleFonts {
  loaded: bool,
  cjk_tried: bool,
  cache: HashMap<(String, i32), Option<Rc<TitleMask>>>,
}

const LATIN: &[&str] = &[
  "/usr/share/fonts/luna/web/Inter-Regular.ttf",
  "/usr/local/share/fonts/luna/web/Inter-Regular.ttf",
  "/usr/local/share/luna-desktop/skins/fonts/web/Inter-Regular.ttf",
  "/usr/share/fonts/inter/Inter-Regular.ttf",
  "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
  "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
  "skins/fonts/web/Inter-Regular.ttf",
];

const CJK_DIRS: &[&str] = &["/usr/share/fonts", "/usr/local/share/fonts"];
const CJK_NAMES: &[&str] = &["notosanscjk", "notosansjp", "ipaexg", "ipagp", "takao", "vl-gothic", "vlgothic", "droidsansfallback", "wqy", "unifont"];

fn add(path: &str) -> bool {
  CString::new(path).map(|c| unsafe { luna_tt_add(c.as_ptr()) >= 0 }).unwrap_or(false)
}

/// First CJK-capable face under the system font directories (bounded walk).
fn find_cjk() -> Option<String> {
  if let Ok(p) = std::env::var("LUNA_FONT_CJK") {
    if !p.is_empty() {
      return Some(p);
    }
  }
  let mut stack: Vec<(std::path::PathBuf, u8)> = CJK_DIRS.iter().map(|d| (std::path::PathBuf::from(d), 0)).collect();
  while let Some((dir, depth)) = stack.pop() {
    let Ok(rd) = std::fs::read_dir(&dir) else { continue };
    for e in rd.flatten() {
      let path = e.path();
      if path.is_dir() {
        if depth < 4 {
          stack.push((path, depth + 1));
        }
        continue;
      }
      let name = path.file_name().and_then(|n| n.to_str()).unwrap_or("").to_lowercase();
      let ext_ok = name.ends_with(".ttf") || name.ends_with(".otf") || name.ends_with(".ttc");
      if ext_ok && !name.contains("bold") && CJK_NAMES.iter().any(|k| name.contains(k)) {
        return Some(path.to_string_lossy().into_owned());
      }
    }
  }
  None
}

impl TitleFonts {
  fn ensure(&mut self) {
    if self.loaded {
      return;
    }
    self.loaded = true;
    let explicit = std::env::var("LUNA_FONT_REGULAR").ok();
    for p in explicit.iter().map(String::as_str).chain(LATIN.iter().copied()) {
      if add(p) {
        break;
      }
    }
  }

  /// Render `text` at `px`; None when no font is available.  Results are cached.
  pub fn render(&mut self, text: &str, px: i32) -> Option<Rc<TitleMask>> {
    self.ensure();
    let key = (text.to_string(), px);
    if let Some(hit) = self.cache.get(&key) {
      return hit.clone();
    }
    // The CJK face is only loaded the first time a title needs it.
    if !self.cjk_tried && text.chars().any(|c| !c.is_ascii() && unsafe { luna_tt_covers(c as u32) } == 0) {
      self.cjk_tried = true;
      if let Some(p) = find_cjk() {
        add(&p);
      }
    }
    let result = if unsafe { luna_tt_count() } == 0 || text.is_empty() {
      None
    } else {
      let (mut w, mut h, mut asc) = (0, 0, 0);
      let p = unsafe { luna_tt_text(text.as_ptr(), text.len(), px, 1, &mut w, &mut h, &mut asc) };
      if p.is_null() {
        None
      } else {
        let alpha = unsafe { std::slice::from_raw_parts(p, (w * h) as usize).to_vec() };
        unsafe { luna_tt_free(p) };
        Some(Rc::new(TitleMask { w, h, ascent: asc, alpha }))
      }
    };
    if self.cache.len() > 96 {
      self.cache.clear();
    }
    self.cache.insert(key, result.clone());
    result
  }
}
