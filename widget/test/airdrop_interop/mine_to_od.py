import sys, os, logging, threading, time
from opendrop.config import AirDropConfig
from opendrop.server import AirDropServerHandler, HTTPServerV6
port=int(sys.argv[1]); out=sys.argv[2]
os.chdir(out)
cfg=AirDropConfig(airdrop_dir=os.environ['ODHOME'], interface='lo', computer_name='OpenDropReceiver')
AirDropServerHandler.config=cfg
HTTPServerV6.allow_reuse_address=True
h=HTTPServerV6(('::1',port),AirDropServerHandler)
h.socket=cfg.get_ssl_context().wrap_socket(sock=h.socket,server_side=True)
h.timeout=1
t0=time.time()
while time.time()-t0<float(sys.argv[3]): h.handle_request()
