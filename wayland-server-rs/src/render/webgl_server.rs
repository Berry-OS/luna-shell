/*
 * Copyright © 2026 Yuichiro Nakada / Project Vespera
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

use super::{Backend, Framebuffer, InputEvent};
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::os::unix::io::RawFd;
use std::sync::mpsc;
use std::sync::{Arc, Mutex};
use std::thread;
use tungstenite::{accept, Message};

struct BroadcastState {
  latest: Arc<Vec<u8>>,
  senders: Vec<mpsc::SyncSender<Arc<Vec<u8>>>>,
  input_tx: mpsc::SyncSender<InputEvent>,
  wakeup_fd: RawFd, // eventfd wakes server epoll
}

pub struct WebGlServerBackend {
  width: u32,
  height: u32,
  state: Arc<Mutex<BroadcastState>>,
  input_rx: Option<mpsc::Receiver<InputEvent>>,
  eventfd: RawFd,
}

impl WebGlServerBackend {
  pub fn new(width: u32, height: u32, port: u16) -> Self {
    let efd = unsafe { libc::eventfd(0, libc::EFD_NONBLOCK | libc::EFD_CLOEXEC) };

    let (input_tx, input_rx) = mpsc::sync_channel::<InputEvent>(256);

    let state = Arc::new(Mutex::new(BroadcastState {
      latest: Arc::new(vec![0u8; (width * height * 4) as usize]),
      senders: Vec::new(),
      input_tx,
      wakeup_fd: efd,
    }));

    let st = Arc::clone(&state);
    thread::spawn(move || run_server(st, port, width, height));

    eprintln!("[vespera/webgl] open http://localhost:{}/ in a browser", port);

    WebGlServerBackend {
      width,
      height,
      state,
      input_rx: Some(input_rx),
      eventfd: efd,
    }
  }
}

impl Backend for WebGlServerBackend {
  fn size(&self) -> (u32, u32) { (self.width, self.height) }

  fn present(&mut self, fb: &Framebuffer) {
    let n = (self.width * self.height).min(fb.width * fb.height) as usize;
    let mut rgba = vec![0u8; n * 4];
    for (i, &px) in fb.pixels[..n].iter().enumerate() {
      rgba[i * 4] = (px >> 16) as u8;
      rgba[i * 4 + 1] = (px >> 8) as u8;
      rgba[i * 4 + 2] = px as u8;
      rgba[i * 4 + 3] = (px >> 24) as u8;
    }

    let frame = Arc::new(rgba);
    let mut st = self.state.lock().unwrap();
    st.latest = Arc::clone(&frame);
    st.senders.retain(|tx| match tx.try_send(Arc::clone(&frame)) {
      Ok(_) => true,
      Err(mpsc::TrySendError::Full(_)) => true,
      Err(mpsc::TrySendError::Disconnected(_)) => false,
    });
  }

  fn take_input_channel(&mut self) -> Option<(mpsc::Receiver<InputEvent>, RawFd)> { self.input_rx.take().map(|rx| (rx, self.eventfd)) }
}

fn run_server(state: Arc<Mutex<BroadcastState>>, port: u16, width: u32, height: u32) {
  let listener = match TcpListener::bind(format!("0.0.0.0:{}", port)) {
    Ok(l) => l,
    Err(e) => {
      eprintln!("[vespera/webgl] failed to bind port {}: {}", port, e);
      return;
    }
  };
  for stream in listener.incoming().flatten() {
    let st = Arc::clone(&state);
    thread::spawn(move || handle_conn(stream, st, width, height));
  }
}

fn handle_conn(stream: TcpStream, state: Arc<Mutex<BroadcastState>>, width: u32, height: u32) {
  let mut peek = [0u8; 4096];
  let n = stream.peek(&mut peek).unwrap_or(0);
  let header = std::str::from_utf8(&peek[..n]).unwrap_or("").to_ascii_lowercase();

  if header.contains("upgrade: websocket") {
    handle_ws(stream, state);
  } else {
    serve_html(stream, width, height);
  }
}

fn serve_html(mut stream: TcpStream, width: u32, height: u32) {
  let mut buf = [0u8; 4096];
  let _ = stream.read(&mut buf);
  let html = make_html(width, height);
  let _ = write!(stream, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}", html.len(), html);
}

fn handle_ws(stream: TcpStream, state: Arc<Mutex<BroadcastState>>) {
  let mut ws = match accept(stream) {
    Ok(w) => w,
    Err(e) => {
      eprintln!("[vespera/webgl] WebSocket handshake failed: {}", e);
      return;
    }
  };

  let (frame_tx, frame_rx) = mpsc::sync_channel::<Arc<Vec<u8>>>(2);
  let (input_tx, wakeup_fd) = {
    let mut st = state.lock().unwrap();
    let initial = Arc::clone(&st.latest);
    st.senders.push(frame_tx);
    let itx = st.input_tx.clone();
    let wfd = st.wakeup_fd;
    drop(st);
    // Send first frame blocking before switching modes.
    if ws.send(Message::Binary((*initial).clone())).is_err() {
      return;
    }
    (itx, wfd)
  };

  // Blocking writes; read timeout avoids old nonblocking+yield loop (~150ms/frame).
  ws.get_mut().set_read_timeout(Some(std::time::Duration::from_millis(1))).ok();

  loop {
    let frame = frame_rx.recv_timeout(std::time::Duration::from_millis(8)).ok();
    if let Some(mut f) = frame {
      // Drop stale queued frames; send only the latest.
      while let Ok(newer) = frame_rx.try_recv() {
        f = newer;
      }
      match ws.send(Message::Binary((*f).clone())) {
        Ok(_) => {}
        Err(_) => return,
      }
      match ws.flush() {
        Ok(_) => {}
        Err(_) => return,
      }
    }

    loop {
      match ws.read() {
        Ok(Message::Text(s)) => {
          if let Some(ev) = parse_input(&s) {
            let _ = input_tx.try_send(ev);
            if wakeup_fd >= 0 {
              let one: u64 = 1;
              unsafe { libc::write(wakeup_fd, &one as *const u64 as *const libc::c_void, 8) };
            }
          }
        }
        Ok(Message::Close(_)) => return,
        Err(tungstenite::Error::ConnectionClosed) => return,
        Err(tungstenite::Error::Io(e)) if e.kind() == std::io::ErrorKind::WouldBlock || e.kind() == std::io::ErrorKind::TimedOut => break,
        Err(_) => return,
        Ok(_) => {}
      }
    }
  }
}

/// Browser input wire format: "m x y", "b btn pressed", "a axis value", "k code pressed"
fn parse_input(s: &str) -> Option<InputEvent> {
  let mut parts = s.split_ascii_whitespace();
  match parts.next()? {
    "m" => {
      let x: f32 = parts.next()?.parse().ok()?;
      let y: f32 = parts.next()?.parse().ok()?;
      Some(InputEvent::PointerMotion { x, y })
    }
    "b" => {
      let button: u32 = parts.next()?.parse().ok()?;
      let pressed = parts.next()?.parse::<u8>().ok()? != 0;
      Some(InputEvent::PointerButton { button, pressed })
    }
    "a" => {
      let axis: u32 = parts.next()?.parse().ok()?;
      let value: f32 = parts.next()?.parse().ok()?;
      Some(InputEvent::PointerAxis { axis, value })
    }
    "k" => {
      let keycode: u32 = parts.next()?.parse().ok()?;
      let pressed = parts.next()?.parse::<u8>().ok()? != 0;
      Some(InputEvent::Key { keycode, pressed })
    }
    _ => None,
  }
}

fn make_html(width: u32, height: u32) -> String {
  format!(
    r#"<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>Vespera</title>
<style>
body{{margin:0;background:#101014;display:flex;justify-content:center;align-items:center;height:100vh;overflow:hidden}}
canvas{{display:block;image-rendering:pixelated;cursor:default;outline:none}}
#status{{position:fixed;bottom:4px;right:8px;color:#666;font:11px monospace}}
</style>
</head>
<body>
<canvas id="c" tabindex="0"></canvas>
<div id="status">connecting…</div>
<script>
const W={w},H={h};
const canvas=document.getElementById('c');
canvas.width=W;canvas.height=H;
const status=document.getElementById('status');
function resize(){{
  const s=Math.min(window.innerWidth/W,window.innerHeight/H);
  canvas.style.width=(W*s)+'px';canvas.style.height=(H*s)+'px';
}}
window.addEventListener('resize',resize);resize();
canvas.focus();

const gl=canvas.getContext('webgl');
if(!gl){{document.body.textContent='WebGL not supported';throw 0;}}

const VS=`attribute vec2 a;varying vec2 v;
void main(){{v=vec2(a.x*.5+.5,.5-a.y*.5);gl_Position=vec4(a,0,1);}}`;
const FS=`precision mediump float;varying vec2 v;uniform sampler2D t;
void main(){{gl_FragColor=texture2D(t,v);}}`;
function mkShader(type,src){{
  const s=gl.createShader(type);gl.shaderSource(s,src);gl.compileShader(s);return s;
}}
const prog=gl.createProgram();
gl.attachShader(prog,mkShader(gl.VERTEX_SHADER,VS));
gl.attachShader(prog,mkShader(gl.FRAGMENT_SHADER,FS));
gl.linkProgram(prog);gl.useProgram(prog);

const vbuf=gl.createBuffer();
gl.bindBuffer(gl.ARRAY_BUFFER,vbuf);
gl.bufferData(gl.ARRAY_BUFFER,new Float32Array([-1,-1,1,-1,-1,1,1,1]),gl.STATIC_DRAW);
const al=gl.getAttribLocation(prog,'a');
gl.enableVertexAttribArray(al);
gl.vertexAttribPointer(al,2,gl.FLOAT,false,0,0);

const tex=gl.createTexture();
gl.bindTexture(gl.TEXTURE_2D,tex);
gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.NEAREST);
gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.NEAREST);
gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_S,gl.CLAMP_TO_EDGE);
gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_T,gl.CLAMP_TO_EDGE);
gl.pixelStorei(gl.UNPACK_ALIGNMENT,1);
gl.viewport(0,0,W,H);

function draw(rgba){{
  gl.bindTexture(gl.TEXTURE_2D,tex);
  gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,W,H,0,gl.RGBA,gl.UNSIGNED_BYTE,rgba);
  gl.drawArrays(gl.TRIANGLE_STRIP,0,4);
}}

// evdev scancodes: KeyboardEvent.code → Linux evdev
const KEY_MAP={{
  'Escape':1,'Digit1':2,'Digit2':3,'Digit3':4,'Digit4':5,'Digit5':6,
  'Digit6':7,'Digit7':8,'Digit8':9,'Digit9':10,'Digit0':11,'Minus':12,
  'Equal':13,'Backspace':14,'Tab':15,'KeyQ':16,'KeyW':17,'KeyE':18,
  'KeyR':19,'KeyT':20,'KeyY':21,'KeyU':22,'KeyI':23,'KeyO':24,'KeyP':25,
  'BracketLeft':26,'BracketRight':27,'Enter':28,'ControlLeft':29,
  'KeyA':30,'KeyS':31,'KeyD':32,'KeyF':33,'KeyG':34,'KeyH':35,'KeyJ':36,
  'KeyK':37,'KeyL':38,'Semicolon':39,'Quote':40,'Backquote':41,
  'ShiftLeft':42,'Backslash':43,'KeyZ':44,'KeyX':45,'KeyC':46,'KeyV':47,
  'KeyB':48,'KeyN':49,'KeyM':50,'Comma':51,'Period':52,'Slash':53,
  'ShiftRight':54,'NumpadMultiply':55,'AltLeft':56,'Space':57,
  'CapsLock':58,'F1':59,'F2':60,'F3':61,'F4':62,'F5':63,'F6':64,
  'F7':65,'F8':66,'F9':67,'F10':68,'NumLock':69,'ScrollLock':70,
  'Numpad7':71,'Numpad8':72,'Numpad9':73,'NumpadSubtract':74,
  'Numpad4':75,'Numpad5':76,'Numpad6':77,'NumpadAdd':78,
  'Numpad1':79,'Numpad2':80,'Numpad3':81,'Numpad0':82,'NumpadDecimal':83,
  'IntlBackslash':86,'F11':87,'F12':88,'NumpadEnter':96,
  'ControlRight':97,'NumpadDivide':98,'PrintScreen':99,'AltRight':100,
  'Home':102,'ArrowUp':103,'PageUp':104,'ArrowLeft':105,'ArrowRight':106,
  'End':107,'ArrowDown':108,'PageDown':109,'Insert':110,'Delete':111,
  'MetaLeft':125,'MetaRight':126,'ContextMenu':127,
}};
// browser button → Linux BTN_*
const BTN_MAP=[272,274,273]; // left, middle, right

let ws=null;
let wsReady=false;
function send(s){{if(wsReady&&ws)ws.send(s);}}

function getPos(e){{
  const r=canvas.getBoundingClientRect();
  const x=((e.clientX-r.left)/r.width).toFixed(4);
  const y=((e.clientY-r.top)/r.height).toFixed(4);
  return [x,y];
}}

canvas.addEventListener('mousemove',e=>{{
  const [x,y]=getPos(e);send('m '+x+' '+y);
}});
canvas.addEventListener('mousedown',e=>{{
  canvas.focus();
  const btn=BTN_MAP[e.button]??272;
  send('b '+btn+' 1');e.preventDefault();
}});
canvas.addEventListener('mouseup',e=>{{
  const btn=BTN_MAP[e.button]??272;
  send('b '+btn+' 0');
}});
canvas.addEventListener('wheel',e=>{{
  const v=(e.deltaY*0.1).toFixed(3);
  send('a 0 '+v);e.preventDefault();
}},{{passive:false}});
canvas.addEventListener('contextmenu',e=>e.preventDefault());
canvas.addEventListener('keydown',e=>{{
  const c=KEY_MAP[e.code];
  if(c)send('k '+c+' 1');
  if(!e.ctrlKey&&!e.metaKey)e.preventDefault();
}});
canvas.addEventListener('keyup',e=>{{
  const c=KEY_MAP[e.code];
  if(c)send('k '+c+' 0');
}});

const proto=location.protocol==='https:'?'wss':'ws';
let reconnectDelay=500,frames=0,last=performance.now();

function connect(){{
  ws=new WebSocket(proto+'://'+location.host+'/ws');
  ws.binaryType='arraybuffer';
  ws.onopen=()=>{{
    wsReady=true;
    document.title='Vespera';
    status.textContent='connected – click canvas to focus';
    reconnectDelay=500;
  }};
  ws.onmessage=e=>{{
    draw(new Uint8Array(e.data));
    frames++;
    const now=performance.now();
    if(now-last>=1000){{
      status.textContent=frames+'fps '+W+'x'+H;
      frames=0;last=now;
    }}
  }};
  ws.onclose=()=>{{
    wsReady=false;
    document.title='Vespera (disconnected)';
    status.textContent='reconnecting in '+(reconnectDelay/1000).toFixed(1)+'s…';
    setTimeout(connect,reconnectDelay);
    reconnectDelay=Math.min(reconnectDelay*2,16000);
  }};
  ws.onerror=()=>{{status.textContent='error'}};
}}
connect();
</script>
</body>
</html>"#,
    w = width,
    h = height
  )
}
