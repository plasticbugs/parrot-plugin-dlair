#!/usr/bin/env python3
"""mkpsgtab.py LADDER OUT.h: the 256 PSG volume triples Movies plays its
sound through (app/engine/src/audio.rs smooth_triples, window 0.15-2.40,
weight 0.5, from the console's measured ladder.json), as the three bytes
the kernel sends a sample: ch2, ch1, ch0 (0xD0|t2, 0xB0|t1, 0x90|t0)."""
import json, sys

WIN, LEVELS, WEIGHT = (0.15, 2.40), 256, 0.5
a = json.load(open(sys.argv[1]))["amp"]


def linspace(lo, hi, n):
    step = (hi - lo) / (n - 1)
    v = [i * step + lo for i in range(n)]
    v[-1] = hi
    return v


mid, span = (WIN[0] + WIN[1]) / 2.0, (WIN[1] - WIN[0]) / 2.0
tg = [mid + u * span for u in linspace(-1.0, 1.0, LEVELS)]
t = [(i >> 8, (i >> 4) & 15, i & 15) for i in range(4096)]
s = [a[0][x[0]] + a[1][x[1]] + a[2][x[2]] for x in t]
step = (WIN[1] - WIN[0]) / (LEVELS - 1)
step2 = step * step
cand = []
for g in tg:
    c = [i for i in range(len(s)) if abs(s[i] - g) <= 0.6 * step]
    if not c:
        b = 0
        for i in range(1, len(s)):
            if abs(s[i] - g) < abs(s[b] - g):
                b = i
        c = [b]
    cand.append(c)


def glitch(p, q):
    d0 = a[0][q[0]] - a[0][p[0]]
    d1 = a[1][q[1]] - a[1][p[1]]
    return (d1 + d0) * (d1 + d0) + d0 * d0


def lvl(i, k):
    e = (s[cand[i][k]] - tg[i]) / step
    return WEIGHT * (e * e)


cost = [lvl(0, k) for k in range(len(cand[0]))]
back = [[]]
for i in range(1, LEVELS):
    nc, bk = [], []
    for kj, j in enumerate(cand[i]):
        best, bv = 0, float("inf")
        for ki, pi in enumerate(cand[i - 1]):
            tot = cost[ki] + glitch(t[pi], t[j]) / step2
            if tot < bv:
                bv, best = tot, ki
        bk.append(best)
        nc.append(bv + lvl(i, kj))
    cost = nc
    back.append(bk)
k = min(range(len(cost)), key=lambda i: (cost[i], i))
path = [0] * LEVELS
path[-1] = k
for i in range(LEVELS - 1, 0, -1):
    k = back[i][k]
    path[i - 1] = k
tri = [t[cand[i][path[i]]] for i in range(LEVELS)]
with open(sys.argv[2], "w") as f:
    f.write("// made by tools/mkpsgtab.py from ladder.json: level index -> the kernel's three PSG bytes\n")
    f.write("#pragma once\n#include <stdint.h>\nstatic const uint8_t dl_psg[256][3] = {\n")
    for i, (t0, t1, t2) in enumerate(tri):
        f.write("    {0x%02X, 0x%02X, 0x%02X},%s\n" % (0xD0 | t2, 0xB0 | t1, 0x90 | t0, "" if i % 1 else ""))
    f.write("};\n")
