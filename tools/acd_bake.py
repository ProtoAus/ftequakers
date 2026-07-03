#!/usr/bin/env python3
"""
acd_bake.py - bake an offline convex decomposition of an IQM model into a .acd sidecar
that the FTEQW engine loads for `sv_prop_collision 3` + `sv_prop_decomp 2` (Patch 65 Phase B).

The engine only consumes the PARTITION (each convex piece's vertices) and builds the actual
collision hull itself (Mod_BuildConvHull: bevels + conservative push-out), so an offline bake
and the runtime ACD differ ONLY in the partition -> a fair A/B comparison. CoACD usually gives
a tighter/cleaner split than the runtime recursive concavity-split, at the cost of an asset step.

Usage:
    pip install coacd numpy
    python acd_bake.py models/props/pipe.iqm
    python acd_bake.py models/props/pipe.iqm -o models/props/pipe.acd -t 0.05 -m 48
Then in-game: sv_prop_decomp 2 ; reload  (the engine reads <model>.acd next to the model).

.acd format (little-endian, matches Mod_LoadACDSidecar in common/com_mesh.c):
    char[4] "FCAD" ; int32 version=1 ; int32 numpieces ;
    per piece: int32 numverts ; float32 xyz[numverts*3]   (MODEL space, same as the IQM verts)
"""
import argparse, struct, sys, os

ENGINE_PIECE_CAP = 128   # ACD_ARRAY in com_mesh.c - the engine rejects sidecars with more pieces


def parse_iqm(path):
    """Minimal IQM reader -> (verts[N][3] float, tris[M][3] int). Model-space positions."""
    d = open(path, 'rb').read()
    if d[:16] != b'INTERQUAKEMODEL\x00':
        raise ValueError("%s: not an IQM (bad magic)" % path)
    # header uints starting at offset 28 (num_text): see iqm.h
    f = struct.unpack_from('<24I', d, 28)
    num_va, num_vertexes, ofs_va = f[4], f[5], f[6]
    num_tris, ofs_tris = f[7], f[8]
    if not num_vertexes or not num_tris:
        raise ValueError("%s: no geometry (verts=%d tris=%d)" % (path, num_vertexes, num_tris))
    pos_off = None
    for i in range(num_va):
        vtype, vflags, vfmt, vsize, voff = struct.unpack_from('<5I', d, ofs_va + i * 20)
        if vtype == 0 and vfmt == 7 and vsize == 3:   # IQM_POSITION, FLOAT, xyz
            pos_off = voff
            break
    if pos_off is None:
        raise ValueError("%s: no float3 POSITION vertex array" % path)
    verts = [struct.unpack_from('<3f', d, pos_off + v * 12) for v in range(num_vertexes)]
    tris = [struct.unpack_from('<3I', d, ofs_tris + t * 12) for t in range(num_tris)]
    return verts, tris


def write_acd(path, pieces):
    """pieces = list of vertex lists [[x,y,z],...]; write the FCAD sidecar."""
    with open(path, 'wb') as o:
        o.write(b'FCAD')
        o.write(struct.pack('<i', 1))            # version
        o.write(struct.pack('<i', len(pieces)))  # numpieces
        for pv in pieces:
            o.write(struct.pack('<i', len(pv)))
            for v in pv:
                o.write(struct.pack('<3f', float(v[0]), float(v[1]), float(v[2])))


def main():
    ap = argparse.ArgumentParser(description="Bake a CoACD convex decomposition into a .acd sidecar for FTEQW.")
    ap.add_argument("iqm", help="input .iqm model")
    ap.add_argument("-o", "--out", help="output .acd (default: <model>.acd next to the iqm)")
    ap.add_argument("-t", "--threshold", type=float, default=0.05,
                    help="CoACD concavity threshold 0.01..1 (smaller = more pieces, default 0.05)")
    ap.add_argument("-m", "--max-hulls", type=int, default=48,
                    help="cap on convex pieces (<= %d, the engine limit; default 48)" % ENGINE_PIECE_CAP)
    args = ap.parse_args()

    try:
        import numpy as np
        import coacd
    except ImportError:
        sys.exit("ERROR: needs CoACD + numpy.  Install with:  pip install coacd numpy")

    verts, tris = parse_iqm(args.iqm)
    print("%s: %d verts, %d tris -> running CoACD (threshold=%.3f, max_hulls=%d)..."
          % (os.path.basename(args.iqm), len(verts), len(tris), args.threshold, args.max_hulls))

    mesh = coacd.Mesh(np.asarray(verts, dtype=np.float64), np.asarray(tris, dtype=np.int32))
    cap = max(1, min(args.max_hulls, ENGINE_PIECE_CAP))
    parts = coacd.run_coacd(mesh, threshold=args.threshold, max_convex_hull=cap)

    # parts = list of (vertices, faces); keep each piece's vertices. Drop degenerate (<4 verts).
    pieces = [p[0] for p in parts if len(p[0]) >= 4]
    if not pieces:
        sys.exit("ERROR: CoACD produced no usable pieces")
    if len(pieces) > ENGINE_PIECE_CAP:
        print("WARNING: %d pieces > engine cap %d; truncating (raise -m / -t)." % (len(pieces), ENGINE_PIECE_CAP))
        pieces = pieces[:ENGINE_PIECE_CAP]

    out = args.out or (os.path.splitext(args.iqm)[0] + ".acd")
    write_acd(out, pieces)
    print("wrote %s: %d convex pieces (%d verts total)"
          % (out, len(pieces), sum(len(p) for p in pieces)))
    print("in-game:  sv_prop_collision 3 ; sv_prop_decomp 2 ; reload")


if __name__ == "__main__":
    main()
