#!/usr/bin/env python3
"""Generate airplay_playfair.h from a UxPlay checkout.

    tools/gen_airplay_playfair.py /path/to/UxPlay > airplay_playfair.h

The vendored code (lib/playfair/*.c and lib/fairplay_playfair.c) is GPL-3.0 and
stays GPL-3.0; only the mechanical renaming/static-ification done here is ours.
"""
import re, subprocess, sys, os, tempfile

root = sys.argv[1]
lib = os.path.join(root, "lib")
pf = os.path.join(lib, "playfair")
order = ["omg_hax.c", "hand_garble.c", "modified_md5.c", "sap_hash.c", "playfair.c"]

def read(p):
    return open(p, encoding="utf-8", errors="replace").read()

# global symbols, taken from the compiled objects
syms = set()
with tempfile.TemporaryDirectory() as td:
    for f in order:
        o = os.path.join(td, f + ".o")
        subprocess.check_call(["gcc", "-c", "-O0", "-w", "-o", o, os.path.join(pf, f)])
        out = subprocess.check_output(["nm", o]).decode()
        for line in out.splitlines():
            m = re.match(r"[0-9a-f]+ [TDBRC] (\S+)", line)
            if m:
                syms.add(m.group(1))
rename = {s: "ap_pf_" + s for s in syms}
rename["playfair_decrypt"] = "ap_pf_decrypt"

def conv(src):
    src = re.sub(r'^\s*#\s*define\s+printf[^\n]*\n', '', src, flags=re.M)
    src = re.sub(r'(?<![A-Za-z0-9_])printf\s*\(', 'ap_pf_dbg(', src)
    src = re.sub(r'^\s*#\s*include[^\n]*\n', '', src, flags=re.M)
    src = re.sub(r'^\s*#ifndef\s+\w+_H\s*\n\s*#define\s+\w+_H\s*\n', '', src, flags=re.M)
    src = re.sub(r'^\s*#endif\s*\n?\s*$', lambda m: m.group(0) if False else m.group(0), src, flags=re.M)
    for old, new in sorted(rename.items(), key=lambda kv: -len(kv[0])):
        src = re.sub(r'(?<![A-Za-z0-9_])' + re.escape(old) + r'(?![A-Za-z0-9_])', new, src)
    return src

def staticify(src):
    out = []
    for line in src.split("\n"):
        # top-level definitions only (column 0), skip prototypes handled below
        if re.match(r'^(unsigned|uint\d+_t|int|void|char|const)\b[^;{]*\bap_pf_\w+\s*(\(|\[|=)', line) and not line.startswith("static"):
            line = "static " + line
        out.append(line)
    return "\n".join(out)

parts = []
hdr = read(os.path.join(pf, "omg_hax.h"))
hdr = re.sub(r'^\s*#\s*(ifndef|define|endif)[^\n]*\n?', '', conv(hdr), flags=re.M)
# prototypes from the header become static forward declarations
hdr = "\n".join(("static " + l if re.match(r'^[a-z].*\)\s*;', l) and not l.startswith("static") else l) for l in hdr.split("\n"))
parts.append(hdr)
for f in order:
    parts.append("/* ---- %s ---- */\n" % f + staticify(conv(read(os.path.join(pf, f)))))
# fairplay_playfair.c: rename its public API
fp = read(os.path.join(lib, "fairplay_playfair.c"))
fp = re.sub(r'^\s*#\s*include[^\n]*\n', '', fp, flags=re.M)
fp = fp.replace("playfair_decrypt", "ap_pf_decrypt")
fp = re.sub(r'logger_t\s*\*\s*logger;', '', fp)
fp = fp.replace("fairplay_init(logger_t *logger)", "ap_fairplay_init(void)")
fp = fp.replace("    fp->logger = logger;\n", "")
for n in ("fairplay_setup", "fairplay_handshake", "fairplay_decrypt", "fairplay_destroy"):
    fp = fp.replace(n + "(", "ap_" + n + "(")
fp = fp.replace("fairplay_t *\nfairplay_init", "ap_fairplay_t *\nap_fairplay_init")
fp = fp.replace("struct fairplay_s", "struct ap_fairplay_s")
fp = fp.replace("fairplay_t", "ap_fairplay_t").replace("ap_ap_", "ap_")
fp = re.sub(r'(?<![A-Za-z0-9_])reply_message(?![A-Za-z0-9_])', 'ap_fp_reply_message', fp)
fp = re.sub(r'(?<![A-Za-z0-9_])fp_header(?![A-Za-z0-9_])', 'ap_fp_header', fp)
fp = fp.replace("char ap_fp_reply_message", "static char ap_fp_reply_message").replace("char ap_fp_header", "static char ap_fp_header")
fp = re.sub(r'^(ap_fairplay_t \*|int|void)\n(ap_fairplay_\w+)', r'static \1\n\2', fp, flags=re.M)
fp = fp.replace("static static", "static")
parts.append("/* ---- fairplay_playfair.c ---- */\n" + fp)

body = "\n\n".join(parts)
sys.stdout.write('''/*
 * airplay_playfair.h — FairPlay key handling for the airplay.h receiver.
 *
 * THIS FILE IS GPL-3.0-ONLY, NOT MPL-2.0.
 *
 * It is a mechanical single-header repackaging (symbols renamed with ap_pf_ /
 * ap_fairplay_, everything made static) of the "playfair" implementation and
 * lib/fairplay_playfair.c from the UxPlay project, itself derived from
 * RPiPlay / shairport-sync work:
 *
 *   UxPlay      https://github.com/antimof/UxPlay  (GPL-3.0)
 *   RPiPlay     https://github.com/FD-/RPiPlay     (GPL-3.0)
 *
 * Regenerate with:  tools/gen_airplay_playfair.py /path/to/UxPlay > airplay_playfair.h
 *
 * Opt-in only.  airplay.h includes this file when AIRPLAY_PLAYFAIR is defined
 * before AIRPLAY_IMPLEMENTATION; a program built that way is a combined work
 * under the GPL-3.0.  Without it airplay.h stays MPL-2.0 and takes FairPlay
 * through the airplay_fairplay_ops callback table instead.
 *
 * FairPlay is Apple's DRM.  Check the law that applies to you (DMCA 1201,
 * EUCD, ...) before shipping a binary that contains this.
 */
#ifndef AIRPLAY_PLAYFAIR_H
#define AIRPLAY_PLAYFAIR_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>

#define ap_pf_dbg(...) ((void)0)
typedef struct ap_fairplay_s ap_fairplay_t;

#ifdef AIRPLAY_IMPLEMENTATION
''' + body + '''
#endif /* AIRPLAY_IMPLEMENTATION */
#endif /* AIRPLAY_PLAYFAIR_H */
''')
