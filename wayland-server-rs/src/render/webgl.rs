/*
 * Copyright © 2026 Yuichiro Nakada / Project Vespera
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

use super::{Backend, Framebuffer};

pub struct WebGlBackend {
  width: u32,
  height: u32,
  pub rgba: Vec<u8>,
  pub generation: u64,
}

impl WebGlBackend {
  pub fn new(width: u32, height: u32) -> Self {
    WebGlBackend {
      width,
      height,
      rgba: vec![0u8; (width * height * 4) as usize],
      generation: 0,
    }
  }

  pub fn pixels_ptr(&self) -> *const u8 { self.rgba.as_ptr() }
  pub fn pixels_len(&self) -> usize { self.rgba.len() }
}

impl Backend for WebGlBackend {
  fn size(&self) -> (u32, u32) { (self.width, self.height) }

  fn present(&mut self, fb: &Framebuffer) {
    let n = (self.width * self.height).min(fb.width * fb.height) as usize;
    for i in 0..n {
      let px = fb.pixels[i];
      let o = i * 4;
      self.rgba[o] = ((px >> 16) & 0xff) as u8;
      self.rgba[o + 1] = ((px >> 8) & 0xff) as u8;
      self.rgba[o + 2] = (px & 0xff) as u8;
      self.rgba[o + 3] = ((px >> 24) & 0xff) as u8;
    }
    self.generation += 1;
  }
}
