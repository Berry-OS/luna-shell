#!/usr/bin/env python3
import re, sys, statistics
from collections import defaultdict

if len(sys.argv) != 2:
    print(f"usage: {sys.argv[0]} /tmp/luna-perf.log", file=sys.stderr)
    raise SystemExit(2)

rx = re.compile(r'^\[luna-perf/(shell|compositor|dri)\]\s+t=([0-9.]+)\s+(.*?)\s+([0-9.]+)\s+ms(?:\s|$)')
rows = defaultdict(list)
with open(sys.argv[1], errors='replace') as f:
    for line in f:
        m = rx.search(line.strip())
        if not m: continue
        source, ts, label, ms = m.groups()
        # Strip compositor's trailing metadata from the label area if present.
        label = re.sub(r'\s+clients=\d+.*$', '', label).strip()
        rows[(source, label)].append((float(ts), float(ms)))

if not rows:
    print("No luna-perf records found.")
    raise SystemExit(1)

print(f"{'source':10} {'section':30} {'n':>5} {'avg':>8} {'p95':>8} {'max':>8} {'interval':>10}")
print('-' * 92)
summary = []
for (source, label), vals in rows.items():
    times = [t for t, _ in vals]
    ms = [v for _, v in vals]
    ordered = sorted(ms)
    p95 = ordered[min(len(ordered)-1, int(round((len(ordered)-1)*0.95)))]
    intervals = [b-a for a,b in zip(times, times[1:]) if b>a]
    interval = statistics.median(intervals) if intervals else float('nan')
    summary.append((max(ms), source, label, len(ms), statistics.mean(ms), p95, interval))

for mx, source, label, n, avg, p95, interval in sorted(summary, reverse=True):
    iv = f"{interval:8.3f}s" if interval == interval else "       -  "
    print(f"{source:10} {label[:30]:30} {n:5d} {avg:8.3f} {p95:8.3f} {mx:8.3f} {iv:>10}")

print('\nLikely periodic triggers:')
found = False
for mx, source, label, n, avg, p95, interval in sorted(summary, reverse=True):
    if n < 2 or interval != interval: continue
    candidates = [(1.0, '1 s animation/state scan'), (2.0, '2 s system monitor'),
                  (5.0, '5 s battery/network'), (30.0, '30 s disk/statvfs')]
    for target, desc in candidates:
        tol = max(0.12, target * 0.12)
        if abs(interval-target) <= tol:
            print(f"  {source}/{label}: median {interval:.3f}s -> {desc}; max {mx:.3f} ms")
            found = True
            break
if not found:
    print('  No clear 1/2/5/30-second cadence among slow records.')

# Correlate shell and compositor/DRI events within 20 ms.
shell_events = [(t, label, ms) for (src,label), vals in rows.items() if src=='shell' for t,ms in vals]
other_events = [(t, src, label, ms) for (src,label), vals in rows.items() if src!='shell' for t,ms in vals]
correlated=[]
for t, label, ms in shell_events:
    near = sorted((abs(t-u), src, lab, oms) for u,src,lab,oms in other_events if abs(t-u) <= .020)
    if near:
        d,src,lab,oms=near[0]
        correlated.append((ms+oms,t,label,ms,src,lab,oms,d*1000))
if correlated:
    print('\nCross-process correlations (within 20 ms, largest first):')
    for _,t,label,ms,src,lab,oms,dms in sorted(correlated, reverse=True)[:12]:
        print(f"  t={t:.3f}: shell/{label} {ms:.3f} ms -> {src}/{lab} {oms:.3f} ms (Δ{dms:.1f} ms)")
