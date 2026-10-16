#!/bin/sh
# Interoperability check against OpenDrop (https://github.com/seemoo-lab/opendrop, GPL-3.0),
# used here only as a reference peer over loopback ::1 — no AWDL needed.
# Needs: python3, cc, gzip, cpio, network access for pip.  Everything lives in a temp dir.
set -e
here=$(cd "$(dirname "$0")" && pwd)
w=$(mktemp -d)
python3 -m venv "$w/v" >/dev/null
"$w/v/bin/pip" install -q opendrop "setuptools<81" cryptography >/dev/null 2>&1
export ODHOME="$w/od"; mkdir -p "$ODHOME/keys" "$w/in" "$w/out_mine" "$w/out_od"
"$w/v/bin/python" - <<'PY'
import datetime, os
from cryptography import x509
from cryptography.x509.oid import NameOID
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
k = rsa.generate_private_key(public_exponent=65537, key_size=2048)
n = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "OpenDropTest")])
c = x509.CertificateBuilder().subject_name(n).issuer_name(n).public_key(k.public_key()).serial_number(1).not_valid_before(datetime.datetime(2024,1,1)).not_valid_after(datetime.datetime(2034,1,1)).sign(k, hashes.SHA256())
d = os.environ['ODHOME'] + '/keys/'
open(d + 'key.pem', 'wb').write(k.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.TraditionalOpenSSL, serialization.NoEncryption()))
open(d + 'certificate.pem', 'wb').write(c.public_bytes(serialization.Encoding.PEM))
PY
cc -O1 -pthread -o "$w/mine" "$here/interop_mine.c" -ldl
head -c 150000 /dev/urandom > "$w/in/payload.jpg"
# OpenDrop sender -> this library's receiver
"$w/mine" recv 8 "$w/out_mine" > "$w/mine.log" 2>&1 & sleep 3
"$w/v/bin/python" "$here/od_to_mine.py" 18880 "$w/in/payload.jpg" 2>&1 | grep -v -i "pkg_resources\|resource_filename" || true; wait
cmp "$w/in/payload.jpg" "$w/out_mine/payload.jpg" && echo "OpenDrop -> airdrop.h : identical"
# this library's sender -> OpenDrop receiver
"$w/v/bin/python" "$here/mine_to_od.py" 18990 "$w/out_od" 8 >/dev/null 2>&1 & sleep 3
AD_PORT=18881 "$w/mine" send 18990 "$w/in/payload.jpg"; wait
cmp "$w/in/payload.jpg" "$w/out_od/payload.jpg" && echo "airdrop.h -> OpenDrop : identical"
cat "$w/mine.log"; rm -rf "$w"
