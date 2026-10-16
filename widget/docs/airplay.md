# airplay.h — AirPlay receiver

Single-header C library (Linux / macOS / Windows, no dependencies).

```c
#define AIRPLAY_IMPLEMENTATION      /* once */
#include "airplay.h"                /* link: -pthread (Linux/macOS), ws2_32 (Windows), -lm if AIRPLAY_PLAYFAIR */

airplay_config cfg = { .name = "My Receiver", .fairplay = &my_ops /* or define AIRPLAY_PLAYFAIR */ };
airplay_callbacks cb = { .on_video = my_video, .on_audio = my_audio, .on_client = my_client };
airplay_t* a = airplay_create(&cfg, &cb);
airplay_start(a);                   /* listens on :7000 and advertises over mDNS */
...
airplay_destroy(a);
```

Callbacks run on worker threads.  Video arrives as Annex-B H.264 (SPS/PPS in-band),
audio as decrypted ALAC / AAC-ELD frames — decoding is up to the sink.

## What is implemented
mDNS responder, RTSP/HTTP server, `/info`, legacy `pair-setup` / `pair-verify`
(ed25519 + x25519), `fp-setup`, `SETUP`, `RECORD`, `SET_PARAMETER` (volume),
`TEARDOWN`, mirroring stream (AES-CTR, AVCC → Annex-B), audio RTP (AES-CBC).

### Audio and A/V sync
* **ALAC** is decoded by the library (`ap_alac_*`, bit-exact against ffmpeg's lossless output) and
  delivered as PCM through `on_pcm`.
* **AAC-LC / AAC-ELD** stay raw: `on_audio_ex` gets the frame, `airplay_loas_wrap()` turns it into
  LATM/LOAS for `ffmpeg/ffplay -f latm` (AAC-ELD needs ffmpeg ≥ 4).
* The control port's sync packets map RTP time to the sender's clock; AAC-ELD's threefold redundancy and
  start-of-stream marker packets are removed; reordered packets are put back in order.
* With `latency_ms > 0` a scheduler holds every audio and video event until
  `sender_time + offset + latency` (one shared clock-offset estimate), so both leave together,
  in timestamp order, from one thread.  `av_offset_ms` shifts video against audio to compensate for sinks
  with different delays.  `latency_ms = 0` delivers on arrival.

## What is not
Audio resend requests, our own NTP timing exchange (the sender's clock is used as is), AirPlay 2 URL / HLS
playback, PIN pairing, IPv6, metadata / cover art.  Not yet tried against real Apple senders; AAC-ELD
decoding is unverified (no ELD encoder here to make test data).

## FairPlay (GPL-3.0) — a separate program
Apple senders require FairPlay.  The code that does it (UxPlay's playfair) is GPL-3.0, so it runs as its own
program and `airplay.h` only talks to it over a pipe — nothing GPL is linked or loaded into your program:

```sh
cc -O2 -o airplay-fairplayd airplay_fairplayd.c -lm        # GPL-3.0
```
```c
airplay_fairplay_ops ops; char err[200];
if (airplay_fairplay_spawn(&ops, NULL /* or a path */, err, sizeof err) == 0) cfg.fairplay = &ops;
```
`airplay_fairplay_spawn` looks at `$LUNA_AIRPLAY_FAIRPLAYD`, next to the program, `./`, `~/.local/lib`,
`/usr/local/lib/luna`, `/usr/lib/luna`, `/usr/libexec/luna`, `$PATH`.  The helper exits when your program does.
Whether two separate programs talking over a pipe stay separate for licensing purposes is a legal question
(this is not legal advice); the process boundary is the clearest arrangement the GPL FAQ describes.
Other options: provide `airplay_fairplay_ops` yourself, or `#define AIRPLAY_PLAYFAIR` to link
`airplay_playfair.h` statically (the program is then GPL-3.0).  Without any of them `fp-setup` answers 501.
Regenerate `airplay_playfair.h` with `tools/gen_airplay_playfair.py /path/to/UxPlay > airplay_playfair.h`.

FairPlay is Apple's DRM; check the law that applies to you before distributing the helper.

## Tests
```
cc -O2 test/airplay_crypto_test.c -o t && ./t                 # RFC / FIPS / NIST vectors
cc -O1 -pthread test/airplay_test.c -o t -ldl && ./t           # scripted sender over loopback
cc -O1 -pthread test/airplay_sync_test.c -o t -ldl && ./t      # A/V scheduler, dedupe, ALAC in the stream
python3 test/gen_alac_vectors.py /tmp/alacv && cc -O1 test/airplay_alac_test.c -o t && ./t /tmp/alacv   # needs ffmpeg
cc -O1 test/airplay_loas_test.c -o t && ./t /tmp/loas         # needs ffmpeg
cc -O1 -pthread -DAIRPLAY_PLAYFAIR test/airplay_playfair_test.c -o t -lm && ./t     # static GPL variant
cc -O1 -pthread test/airplay_helper_test.c -o t -ldl && LUNA_AIRPLAY_FAIRPLAYD=./airplay-fairplayd ./t
```
