#!/usr/bin/env python3
"""Makes ALAC test vectors with ffmpeg: <dir>/alac.cookie, alac.sizes, alac.bin, alac.pcm.
   Lossless codec => decoding alac.bin must reproduce alac.pcm exactly."""
import subprocess, sys, os, struct, random, math
d = sys.argv[1]
os.makedirs(d, exist_ok=True)
N = 44100 * 2
random.seed(5)
pcm = bytearray()
for i in range(N):
    t = i / 44100.0
    l = 9000 * math.sin(2 * math.pi * 440 * t) + 3000 * math.sin(2 * math.pi * 1250 * t) + random.randint(-300, 300)
    r = 7000 * math.sin(2 * math.pi * 523 * t + 1) + random.randint(-1500, 1500)
    if 20000 < i < 26000: l = r = 0                      # silence: exercises the zero-run path
    if 30000 < i < 30400: l = random.randint(-32000, 32000); r = random.randint(-32000, 32000)   # noise burst
    pcm += struct.pack('<hh', int(max(-32768, min(32767, l))), int(max(-32768, min(32767, r))))
open(d + '/alac.pcm', 'wb').write(pcm)
open(d + '/in.raw', 'wb').write(pcm)
for fl in (352, 4096):
    sub = f'{d}/f{fl}'
    os.makedirs(sub, exist_ok=True)
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 's16le', '-ar', '44100', '-ac', '2', '-i', d + '/in.raw', '-c:a', 'alac', '-frame_size', str(fl), sub + '/a.m4a'], check=True)
    # raw packets: ffprobe gives each packet's byte position and size inside the .m4a
    out = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'a:0', '-show_entries', 'packet=pos,size', '-of', 'csv=p=0', sub + '/a.m4a'], capture_output=True, text=True, check=True).stdout.split()
    m = open(sub + '/a.m4a', 'rb').read()
    sizes, blob = [], bytearray()
    for line in out:
        size, pos = (int(x) for x in line.split(","))
        blob += m[pos:pos + size]; sizes.append(size)
    open(sub + '/alac.bin', 'wb').write(blob)
    open(sub + '/alac.sizes', 'w').write('\n'.join(map(str, sizes)))
    i = m.index(b'alac', m.index(b'alac') + 4)          # the inner "alac" atom = magic cookie
    open(sub + '/alac.cookie', 'wb').write(m[i - 4:i + 32])
