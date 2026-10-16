import sys, os, http.client
_orig=http.client.HTTPSConnection.__init__
def _init(self,*a,key_file=None,cert_file=None,check_hostname=None,**k): _orig(self,*a,**k)
http.client.HTTPSConnection.__init__=_init
from opendrop.config import AirDropConfig
from opendrop.client import AirDropClient
port=int(sys.argv[1]); f=sys.argv[2]
cfg=AirDropConfig(airdrop_dir=os.environ['ODHOME'], interface='lo', computer_name='OpenDropSender')
c=AirDropClient(cfg,('::1%1',port))
print('discover ->', c.send_discover())
print('ask ->', c.send_ask(f))
import io, subprocess, gzip
d=os.path.dirname(f); b=os.path.basename(f)
cpio=subprocess.run(['cpio','-o','-H','odc'],input=('./'+b+'\n').encode(),cwd=d,capture_output=True).stdout
stream=io.BytesIO(gzip.compress(cpio))
ok,_=c.send_POST('/Upload',stream,{'Content-Type':'application/x-cpio'})
print('upload ->', ok)
