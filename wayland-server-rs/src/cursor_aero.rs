// Embedded Aero cursors from /usr/share/icons/aero/cursors — regenerate with ui/gen_luna_cur_aero.sh
#[allow(dead_code)]
struct EmbedFrame {
  w: i32,
  h: i32,
  hot_x: i32,
  hot_y: i32,
  delay_ms: i32,
  pixels: &'static [u32],
}

static CUR_DEFAULT_0: [u32; 1024] = include!("cursor_aero_default.inc");

const DEFAULT_0: EmbedFrame = EmbedFrame {
  w: 32,
  h: 32,
  hot_x: 0,
  hot_y: 0,
  delay_ms: 333,
  pixels: &CUR_DEFAULT_0,
};

pub fn blit_default_cursor(fb: &mut crate::render::Framebuffer, x: i32, y: i32) {
  blit_embed_frame(fb, &DEFAULT_0, x, y);
}

pub fn blit_embed_frame(fb: &mut crate::render::Framebuffer, frame: &EmbedFrame, x: i32, y: i32) {
  let w = frame.w as u32;
  let h = frame.h as u32;
  for row in 0..h {
    for col in 0..w {
      let px = frame.pixels[row as usize * w as usize + col as usize];
      let a = (px >> 24) & 0xff;
      if a == 0 {
        continue;
      }
      let dx = x + col as i32 - frame.hot_x;
      let dy = y + row as i32 - frame.hot_y;
      if dx < 0 || dy < 0 || dx >= fb.width as i32 || dy >= fb.height as i32 {
        continue;
      }
      let idx = dy as usize * fb.width as usize + dx as usize;
      fb.pixels[idx] = blend_px(fb.pixels[idx], px);
    }
  }
}

fn blend_px(dst: u32, src: u32) -> u32 {
  let a = (src >> 24) & 0xff;
  if a == 0xff {
    return src;
  }
  if a == 0 {
    return dst;
  }
  let inv = 255 - a;
  let dr = (dst >> 16) & 0xff;
  let dg = (dst >> 8) & 0xff;
  let db = dst & 0xff;
  let sr = (src >> 16) & 0xff;
  let sg = (src >> 8) & 0xff;
  let sb = src & 0xff;
  let r = (sr * a + dr * inv) / 255;
  let g = (sg * a + dg * inv) / 255;
  let b = (sb * a + db * inv) / 255;
  0xff000000 | (r << 16) | (g << 8) | b
}
