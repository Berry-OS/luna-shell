/*
 * Copyright © 2026 Yuichiro Nakada / Project Vespera
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

//! Initial window placement for clients that cannot position themselves.
//!
//! Wayland deliberately gives clients no way to say "put my window at x,y", so
//! any toolkit's position request (`--window-position`, `-geometry`, …) is
//! silently dropped on this platform.  The compositor owns placement, which means the
//! request has to be honoured *here*, at the moment a toplevel first maps and
//! before anything has been drawn at the default spot.  Three sources are
//! consulted, most specific first:
//!
//! 1. one-shot hints pushed through the shell socket (`place_next APP X Y`),
//! 2. a position flag on the client's own command line (see `parse_cmdline_position`), read
//!    from `/proc/<pid>/cmdline` of the process that owns the connection,
//! 3. persistent rules from `window-rules.conf`.
//!
//! The rules file is one rule per line, `#` starts a comment:
//!
//! ```text
//! # app_id (case-insensitive substring)   x  y
//! firefox                                 120 80
//! google-chrome                           200 120
//! ```

use std::path::PathBuf;
use std::time::SystemTime;

#[derive(Clone, Debug, PartialEq, Eq)]
struct Rule {
  key: String,
  x: i32,
  y: i32,
}

#[derive(Default)]
pub struct Placement {
  rules: Vec<Rule>,
  rules_path: Option<PathBuf>,
  rules_mtime: Option<SystemTime>,
  /// One-shot hints, consumed by the first window that matches.
  hints: Vec<Rule>,
}

/// Parse `X,Y` (also `XxY` / `X:Y` / `X Y`) into a coordinate pair.
fn parse_pair(s: &str) -> Option<(i32, i32)> {
  let mut it = s.split(|c: char| c == ',' || c == 'x' || c == ':' || c.is_whitespace()).filter(|p| !p.is_empty());
  let x = it.next()?.parse::<i32>().ok()?;
  let y = it.next()?.parse::<i32>().ok()?;
  if it.next().is_some() {
    return None;
  }
  Some((x, y))
}

/// Parse an X11-style geometry `[WxH]+X+Y` (offsets may be negative: `-X` /
/// `+-X`) and return the offsets.
fn parse_geometry(s: &str) -> Option<(i32, i32)> {
  let rest = s.find(|c| c == '+' || c == '-').map(|i| &s[i..])?;
  let mut nums = Vec::new();
  let mut cur = String::new();
  for c in rest.chars() {
    if (c == '+' || c == '-') && !(cur.is_empty() || cur == "+" || cur == "-") {
      nums.push(std::mem::take(&mut cur));
    }
    if c == '+' && cur.is_empty() {
      continue;
    }
    cur.push(c);
  }
  nums.push(cur);
  match nums.as_slice() {
    [x, y] => Some((x.trim_start_matches('+').parse().ok()?, y.trim_start_matches('+').parse().ok()?)),
    _ => None,
  }
}

/// Find a requested window position in a process command line.
///
/// Toolkit-neutral: any program that advertises one of the usual spellings
/// (`--window-position=X,Y`, `--window-pos`, `--position`, or X11 `-geometry
/// WxH+X+Y`) is honoured, whether the value is attached with `=` or is the next
/// argument.  Some programs (Chrome) rewrite argv into one space-joined string,
/// so `/proc/<pid>/cmdline` may hold NUL- or space-separated arguments; we split
/// on both.
pub fn parse_cmdline_position(cmdline: &[u8]) -> Option<(i32, i32)> {
  let mut args = cmdline.split(|&b| b == 0 || b == b' ').filter(|a| !a.is_empty());
  while let Some(raw) = args.next() {
    let Ok(arg) = std::str::from_utf8(raw) else { continue };
    if !arg.starts_with('-') {
      continue;
    }
    let arg = arg.trim_start_matches('-');
    let (name, inline) = match arg.split_once('=') {
      Some((n, v)) => (n, Some(v)),
      None => (arg, None),
    };
    let is_geometry = name == "geometry" || name == "geom";
    if !is_geometry && !matches!(name, "window-position" | "window-pos" | "position" | "pos") {
      continue;
    }
    let value = match inline {
      Some(v) => v,
      None => match args.next().and_then(|v| std::str::from_utf8(v).ok()) {
        Some(v) => v,
        None => return None,
      },
    };
    return if is_geometry { parse_geometry(value) } else { parse_pair(value) };
  }
  None
}

fn parse_rules(text: &str) -> Vec<Rule> {
  let mut out = Vec::new();
  for line in text.lines() {
    let line = line.split('#').next().unwrap_or("").trim();
    if line.is_empty() {
      continue;
    }
    let mut it = line.split_whitespace();
    let (Some(key), Some(xs), Some(ys)) = (it.next(), it.next(), it.next()) else { continue };
    let (Ok(x), Ok(y)) = (xs.parse::<i32>(), ys.parse::<i32>()) else { continue };
    out.push(Rule { key: key.to_ascii_lowercase(), x, y });
  }
  out
}

fn matches(key: &str, app_id: &str) -> bool {
  !key.is_empty() && app_id.to_ascii_lowercase().contains(key)
}

impl Placement {
  pub fn new() -> Self {
    let path = std::env::var_os("LUNA_WINDOW_RULES")
      .map(PathBuf::from)
      .or_else(|| {
        let base = std::env::var_os("XDG_CONFIG_HOME")
          .map(PathBuf::from)
          .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".config")))?;
        Some(base.join("luna-shell").join("window-rules.conf"))
      });
    let mut p = Placement { rules_path: path, ..Default::default() };
    p.reload_if_changed();
    p
  }

  /// Re-read the rules file when its mtime moved.  A stat per new window is
  /// negligible, and it lets the user edit the file without restarting.
  fn reload_if_changed(&mut self) {
    let Some(path) = self.rules_path.as_ref() else { return };
    let mtime = std::fs::metadata(path).and_then(|m| m.modified()).ok();
    if mtime == self.rules_mtime {
      return;
    }
    self.rules_mtime = mtime;
    self.rules = match mtime {
      Some(_) => std::fs::read_to_string(path).map(|t| parse_rules(&t)).unwrap_or_default(),
      None => Vec::new(),
    };
  }

  /// Queue a hint for the next window whose app_id contains `app_id`.
  pub fn push_hint(&mut self, app_id: &str, x: i32, y: i32) {
    let key = app_id.to_ascii_lowercase();
    self.hints.retain(|h| h.key != key);
    self.hints.push(Rule { key, x, y });
    if self.hints.len() > 16 {
      self.hints.remove(0);
    }
  }

  /// Requested top-left of the visible window geometry for a newly mapped
  /// toplevel, or `None` to keep the default placement.
  pub fn lookup(&mut self, app_id: &str, pid: i32) -> Option<(i32, i32)> {
    if let Some(i) = self.hints.iter().position(|h| matches(&h.key, app_id)) {
      let h = self.hints.remove(i);
      return Some((h.x, h.y));
    }
    if pid > 0 {
      if let Ok(cmdline) = std::fs::read(format!("/proc/{}/cmdline", pid)) {
        if let Some(pos) = parse_cmdline_position(&cmdline) {
          return Some(pos);
        }
      }
    }
    self.reload_if_changed();
    self.rules.iter().find(|r| matches(&r.key, app_id)).map(|r| (r.x, r.y))
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn cmdline_equals_form() {
    let c = b"/opt/google/chrome/chrome\0--window-position=120,80\0--window-size=800,600\0";
    assert_eq!(parse_cmdline_position(c), Some((120, 80)));
  }

  #[test]
  fn cmdline_separate_and_negative() {
    let c = b"chromium\0--window-position\0-20,40\0";
    assert_eq!(parse_cmdline_position(c), Some((-20, 40)));
  }

  #[test]
  fn cmdline_space_joined_like_chrome() {
    let c = b"/opt/google/chrome/chrome --no-sandbox --window-position=700,300 about:blank";
    assert_eq!(parse_cmdline_position(c), Some((700, 300)));
  }

  #[test]
  fn cmdline_x11_geometry() {
    assert_eq!(parse_cmdline_position(b"xterm\0-geometry\0100x40+30+50\0"), Some((30, 50)));
    assert_eq!(parse_cmdline_position(b"app --geometry=+10-20"), Some((10, -20)));
  }

  #[test]
  fn cmdline_without_flag() {
    assert_eq!(parse_cmdline_position(b"firefox\0--new-window\0"), None);
  }

  #[test]
  fn rules_parse_and_match() {
    let r = parse_rules("# c\nFirefox 10 20\nbogus x y\ngoogle-chrome 5 6 # tail\n");
    assert_eq!(r.len(), 2);
    assert!(matches(&r[0].key, "org.mozilla.firefox"));
    assert_eq!((r[1].x, r[1].y), (5, 6));
  }

  #[test]
  fn hints_are_one_shot() {
    let mut p = Placement::default();
    p.push_hint("Firefox", 7, 9);
    assert_eq!(p.lookup("firefox", 0), Some((7, 9)));
    assert_eq!(p.lookup("firefox", 0), None);
  }
}
