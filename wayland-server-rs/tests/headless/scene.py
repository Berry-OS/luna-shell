#!/usr/bin/env python3
"""Headless compositor scene: an xdg_toplevel with a text-input, plus (unless
HEADLESS_NO_IM=1) a Python input-method client that maps an input popup.

Raw Wayland wire protocol only (no libraries), so it runs anywhere python3
does.  Driven by run.sh; see tests/README.md ("Headless GPU composition")."""
import socket,struct,os,sys,array,mmap,time
RT=os.environ['XDG_RUNTIME_DIR']
SOCK=os.environ.get('WAYLAND_DISPLAY','test-1')
DONE=os.environ.get('HEADLESS_DONE','/tmp/luna-headless.done')
class C:
    def __init__(s):
        s.s=socket.socket(socket.AF_UNIX); s.s.connect(RT+'/'+SOCK); s.s.setblocking(False)
        s.buf=b''; s.next=2; s.fds=[]; s.obj={}   # id->iface
        s.ev=[]
    def nid(s,iface):
        i=s.next; s.next+=1; s.obj[i]=iface; return i
    def send(s,o,op,payload=b'',fds=()):
        m=struct.pack('<IHH',o,op,8+len(payload))+payload
        if fds:
            s.s.sendmsg([m],[(socket.SOL_SOCKET,socket.SCM_RIGHTS,array.array('i',fds))])
        else: s.s.send(m)
    def pump(s,t=0.2):
        end=time.time()+t
        while time.time()<end:
            try:
                d,anc,_,_=s.s.recvmsg(65536,4096)
                if not d: break
                s.buf+=d
            except BlockingIOError: time.sleep(0.01)
        while len(s.buf)>=8:
            o,op,sz=struct.unpack_from('<IHH',s.buf)
            if len(s.buf)<sz: break
            s.ev.append((o,op,s.buf[8:sz])); s.buf=s.buf[sz:]
    def take(s,pred=lambda e:True):
        r=[e for e in s.ev if pred(e)]; s.ev=[e for e in s.ev if not pred(e)]; return r
def pstr(x):
    b=x.encode()+b'\0'; return struct.pack('<I',len(b))+b+b'\0'*((-len(b))%4)
def rstr(p,off):
    l=struct.unpack_from('<I',p,off)[0]; return p[off+4:off+4+l-1].decode(), off+4+((l+3)//4*4)
def connect():
    c=C(); c.obj[1]='wl_display'
    reg=c.nid('wl_registry'); c.send(1,1,struct.pack('<I',reg)); 
    sy=c.nid('wl_callback'); c.send(1,0,struct.pack('<I',sy))
    c.globs={}; 
    for _ in range(20):
        c.pump(0.1)
        for o,op,p in c.take(lambda e:e[0]==reg and e[1]==0):
            n=struct.unpack_from('<I',p)[0]; name,off=rstr(p,4); v=struct.unpack_from('<I',p,off)[0]; c.globs[name]=(n,v)
        if any(e[0]==sy for e in c.ev): break
    c.reg=reg
    return c
def bind(c,iface,ver=None):
    n,v=c.globs[iface]; v=min(v,ver or v); i=c.nid(iface)
    c.send(c.reg,0,struct.pack('<I',n)+pstr(iface)+struct.pack('<II',v,i)); return i
def shm_buf(c,shm,w,h,color):
    pad=int(os.environ.get('HEADLESS_STRIDE_PAD','0'))   # bytes of row padding, like Mesa's software buffers
    stride=w*4+pad
    size=stride*h; fd=os.memfd_create('b'); os.ftruncate(fd,size)
    m=mmap.mmap(fd,size)
    for y in range(h): m.seek(y*stride); m.write(struct.pack('<I',color)*w)
    m.close()
    pool=c.nid('wl_shm_pool'); c.send(shm,0,struct.pack('<II',pool,size),fds=[fd])
    buf=c.nid('wl_buffer'); c.send(pool,0,struct.pack('<IiiiII',buf,0,w,h,stride,0)); return buf
def surface_with(c,comp,shm,w,h,color):
    s=c.nid('wl_surface'); c.send(comp,0,struct.pack('<I',s)); return s

# --- client A: app window + text input
A=connect()
comp=bind(A,'wl_compositor',4); shm=bind(A,'wl_shm',1); wm=bind(A,'xdg_wm_base',1); seat=bind(A,'wl_seat',5)
tim=bind(A,'zwp_text_input_manager_v3',1)
kb=A.nid('wl_keyboard'); A.send(seat,1,struct.pack('<I',kb))
ti=A.nid('zwp_text_input_v3'); A.send(tim,1,struct.pack('<II',ti,seat))
surf=A.nid('wl_surface'); A.send(comp,0,struct.pack('<I',surf))
xs=A.nid('xdg_surface'); A.send(wm,2,struct.pack('<II',xs,surf))
tl=A.nid('xdg_toplevel'); A.send(xs,1,struct.pack('<I',tl))
A.send(surf,6)  # commit (initial)
A.pump(0.5)
cfg=A.take(lambda e:e[0]==xs and e[1]==0)
if cfg: A.send(xs,4,struct.pack('<I',struct.unpack_from('<I',cfg[-1][2])[0]))
b=shm_buf(A,shm,500,360,0xff2030c0)   # blue window
A.send(surf,1,struct.pack('<Iii',b,0,0)); A.send(surf,2,struct.pack('<iiii',0,0,500,360)); A.send(surf,6)
A.pump(0.8)
ents=A.take(lambda e:e[0]==ti and e[1]==0)
print('A: text_input.enter events:',len(ents))
A.send(ti,1)                                  # enable
A.send(ti,6,struct.pack('<iiii',100,100,2,18)) # cursor rect
A.send(ti,7)                                  # commit
A.pump(0.3)

# --- client B: input method + popup
if os.environ.get('HEADLESS_NO_IM'):
    open(DONE,'w').write('ok'); time.sleep(int(os.environ.get('HEADLESS_HOLD','30'))); sys.exit(0)
B=connect()
bcomp=bind(B,'wl_compositor',4); bshm=bind(B,'wl_shm',1); bseat=bind(B,'wl_seat',5)
imm=bind(B,'zwp_input_method_manager_v2',1)
im=B.nid('zwp_input_method_v2'); B.send(imm,0,struct.pack('<II',bseat,im))
B.pump(0.5)
act=B.take(lambda e:e[0]==im and e[1]==0)
print('B: input_method.activate events:',len(act))
ps=B.nid('wl_surface'); B.send(bcomp,0,struct.pack('<I',ps))
pop=B.nid('zwp_input_popup_surface_v2'); B.send(im,4,struct.pack('<II',pop,ps))
pb=shm_buf(B,bshm,320,90,0xffe02020)         # red candidate window
B.send(ps,1,struct.pack('<Iii',pb,0,0)); B.send(ps,2,struct.pack('<iiii',0,0,320,90)); B.send(ps,6)
B.pump(0.8)
print('B: popup events:',[(e[1]) for e in B.ev if e[0]==pop])
time.sleep(2)
open(DONE,'w').write('ok')
time.sleep(int(os.environ.get('HEADLESS_HOLD','30')))
