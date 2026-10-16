# airdrop.h — AirDrop-compatible file transfer

Single-header C library (Linux / macOS / Windows).  OpenSSL is loaded at run time
(`libssl`/`libcrypto` 1.1 or 3.x — nothing to link; link `-pthread`, `-ldl` on old glibc,
`ws2_32 iphlpapi` on Windows).

```c
#define AIRDROP_IMPLEMENTATION      /* once */
#include "airdrop.h"

airdrop_config cfg = { .name = "My Desktop", .interface_name = "awdl0", .download_dir = "/home/me/Downloads" };
airdrop_callbacks cb = { .on_ask = my_prompt, .on_peer = my_peer, .on_received = my_file };
airdrop_t* a = airdrop_create(&cfg, &cb);
airdrop_start(a);                               /* listens (HTTPS) and advertises via mDNS */
...
airdrop_send(a, "fe80::1%awdl0", port, files, nfiles, err, sizeof err);   /* blocking */
airdrop_destroy(a);
```

## Protocol
mDNS `_airdrop._tcp` (TXT `flags=136`), HTTPS `POST /Discover`, `/Ask`, `/Upload`
(chunked, `application/x-cpio`, gzip'd odc/newc cpio).  The receiver answers in
"Everyone" mode only; there are no Apple ID validation records.  A self-signed RSA-2048
certificate is generated in memory on start-up.

## Reaching Apple devices
Real AirDrop runs over AWDL, which is not an ordinary network interface.

* **Linux**: run [OWL](https://github.com/seemoo-lab/owl) (needs a supported Wi-Fi chip) and use
  `interface_name = "awdl0"`.
* **macOS**: AirDrop is built in; this library is not needed.
* **Windows / plain LAN**: with `interface_name = NULL` two instances of this library — or
  OpenDrop — find each other, but an iPhone will not see you.

## What is tested
`test/airdrop_test.c`: inflate against the system `gzip` (stored / fixed / dynamic blocks, bad CRC),
cpio parsing (odc, newc from the system `cpio`, odd-sized feeds, path traversal), and a full
sender → receiver transfer over loopback TLS (IPv6 and IPv4, duplicates, decline).
`test/airdrop_interop/run.sh`: transfers in both directions against OpenDrop over `::1`
(byte-identical).  mDNS was checked with python-zeroconf in both directions.
**Not tested against an iPhone / Mac** — that needs AWDL.

## Safety
Archive paths are reduced to safe relative components (`..`, absolute paths, drive letters and
control characters are refused), existing files are never overwritten (`name (1).ext`), uploads over
`max_bytes` (default 16 GiB) are cut off, and nothing is written before `on_ask` returns 1.
