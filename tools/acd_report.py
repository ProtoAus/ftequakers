#!/usr/bin/env python3
"""
acd_report.py - audit the .acd convex-decomposition sidecars and the IQMs that lack them.

Read-only. Tells you WHICH props are over-decomposed, so you can decide which ones only ever needed
a single hull (boxes, barrels, bushes, crates...). Two costs scale with piece count:
  - LOAD:  the engine runs Mod_BuildConvHull once PER PIECE (a v1 sidecar caches only the partition).
  - TRACE: PM_HullTrace / World_HullTrace walk each piece, and each piece's planes, per trace.
So a crate split into 12 pieces pays ~12x at load and up to 12x per trace, for a shape a single hull
describes exactly.

  python acd_report.py                 # default nettest models root
  python acd_report.py <root> [root..]
  python acd_report.py --csv           # machine-readable

Fix a bad one with:   python acd_bake.py <model.iqm> --single
"""
import sys, os, glob, struct

DEFAULT_ROOT = r"C:\FTEQuake\nettest\models"
# Piece counts above this on a visually-simple prop are the interesting ones. Not a hard rule --
# a genuinely concave prop (a chair, an arch, a pipe) legitimately needs several.
BUSY = 6


def read_acd(path):
    """-> (version, [piece_vertcount|piece_planecount...]) or None."""
    with open(path, 'rb') as f:
        d = f.read()
    if len(d) < 12 or d[:4] != b'FCAD':
        return None
    ver, npieces = struct.unpack_from('<ii', d, 4)
    p, sizes = 12, []
    try:
        for _ in range(npieces):
            if ver == 1:                       # partition: numverts + xyz
                nv, = struct.unpack_from('<i', d, p); p += 4 + nv * 12
                sizes.append(nv)
            elif ver == 2:                     # prebuilt: planes + tris + bounds
                npl, = struct.unpack_from('<i', d, p); p += 4 + npl * 16
                nt, = struct.unpack_from('<i', d, p); p += 4 + nt * 36 + 24
                sizes.append(npl)
            else:
                return (ver, [])
    except struct.error:
        return (ver, [])
    return (ver, sizes)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    as_csv = "--csv" in sys.argv
    roots = args or [DEFAULT_ROOT]

    rows = []
    for r in roots:
        for iqm in glob.glob(os.path.join(r, "**", "*.iqm"), recursive=True):
            acd = os.path.splitext(iqm)[0] + ".acd"
            rel = iqm.replace("\\", "/").split("/models/")[-1]
            if os.path.exists(acd):
                got = read_acd(acd)
                if not got:
                    rows.append((rel, -1, 0, "BAD FILE"))
                    continue
                ver, sizes = got
                rows.append((rel, len(sizes), sum(sizes), "v%d" % ver))
            else:
                # No sidecar: the engine decomposes at LOAD every time (sv_prop_decomp 1/2), unless
                # the path is covered by sv_prop_hull_exclude (players/gibs) in which case it builds
                # no hull at all and this line is expected.
                rows.append((rel, 0, 0, "no sidecar (runtime ACD)"))

    rows.sort(key=lambda r: (-r[1], -r[2]))

    if as_csv:
        print("model,pieces,total,kind")
        for rel, n, tot, kind in rows:
            print("%s,%d,%d,%s" % (rel, n, tot, kind))
        return

    baked = [r for r in rows if r[1] > 0]
    busy = [r for r in baked if r[1] >= BUSY]
    print("%-58s %6s %8s  %s" % ("MODEL", "PIECES", "TOTAL", "KIND"))
    print("-" * 96)
    for rel, n, tot, kind in rows[:40]:
        flag = "  <-- candidate for --single" if n >= BUSY else ""
        print("%-58s %6d %8d  %s%s" % (rel[:58], n, tot, kind, flag))
    print("-" * 96)
    print("%d models with a sidecar, %d without." % (len(baked), len(rows) - len(baked)))
    if baked:
        print("pieces: max %d, mean %.1f" % (max(r[1] for r in baked),
                                             sum(r[1] for r in baked) / len(baked)))
    print("%d model(s) at >=%d pieces - each costs that many hull builds at load AND that many"
          % (len(busy), BUSY))
    print("   piece-walks per trace. If the prop is really just a box/barrel/bush, force one hull:")
    print("   python acd_bake.py <model.iqm> --single")


if __name__ == "__main__":
    main()
