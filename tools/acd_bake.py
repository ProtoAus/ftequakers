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

  v1 (legacy, still read):
    char[4] "FCAD" ; int32 version=1 ; int32 numpieces ;
    per piece: int32 numverts ; float32 xyz[numverts*3]   (MODEL space, same as the IQM verts)

  v2 (default, engine Patch 102) - fully BUILT hulls:
    char[4] "FCAD" ; int32 version=2 ; int32 numpieces ;
    per piece: int32 numplanes ; float32 planes[numplanes*4]  (xyz = outward unit normal, w = dist)
               int32 numtris   ; float32 tris[numtris*3*3]    (numtris triangles x 3 verts x xyz)
               float32 mins[3] ; float32 maxs[3]

Why v2: v1 cached only the PARTITION, so the engine still ran Mod_BuildConvHull for every piece on
every load - a 26-piece van meant 26 QuickHull builds on the loader worker the map load waits on,
from a file that was meant to BE the cache. v2 stores what those builds produce, so loading is a
read + memcpy. It is also worth baking v2 for CONVEX (1-piece) props, which v1 skipped on the
grounds that a 1-piece v1 file saved nothing - a 1-piece v2 file still skips that hull build.

Plane convention (mirrors convhull_t / Mod_BuildConvHull): outward unit normal, dist = dot(n, p),
so a point is OUTSIDE iff dot(p, n) - dist > 0. Normals are sign-corrected against the piece
centroid here rather than trusted from the mesh winding.
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
    """v1 (legacy): pieces = list of vertex lists [[x,y,z],...]; the engine rebuilds each hull."""
    with open(path, 'wb') as o:
        o.write(b'FCAD')
        o.write(struct.pack('<i', 1))            # version
        o.write(struct.pack('<i', len(pieces)))  # numpieces
        for pv in pieces:
            o.write(struct.pack('<i', len(pv)))
            for v in pv:
                o.write(struct.pack('<3f', float(v[0]), float(v[1]), float(v[2])))


PLANE_EPS = 1e-4        # coplanar-merge tolerance (normal dot + dist)
DEGEN_AREA_EPS = 1e-10  # drop slivers whose cross product is ~0
# Must match the `cap` the engine passes to Mod_BuildConvHull for decomposition pieces
# (common/com_mesh.c). World_HullTrace / PM_HullTrace are O(numplanes), so shipping a hull with more
# planes than the runtime would have built trades load time for FRAME time - the opposite of the
# point. A tessellated CoACD piece (e.g. a barrel's curved shell) easily exceeds this.
PLANE_CAP = 256


def build_hull_planes(verts, faces, cap=PLANE_CAP):
    """*** DO NOT USE AS-IS. THIS DOES NOT REPRODUCE Mod_BuildConvHull. ***

    Its output was baked over the whole model set once and REVERTED. Three divergences, all of which
    silently degraded collision (measured on the real bake):
      1. MERGE TOLERANCE. The engine merges faces within ~2 degrees (dot > 0.99939, com_mesh.c:2987).
         This merges only near-identical planes (PLANE_EPS), so 697 of 7819 pieces jammed at the 256
         cap and 1979 exceeded 128 planes - van_2 came out 3339 planes over 24 pieces vs 111 for the
         engine's single hull of the same model. Traces are O(planes) per piece, so this traded load
         time for frame time; and every capped piece hit the conservative-outward merge below, which
         INFLATED the hull -> invisible collision.
      2. NO Mod_AddHullBevels (Patch 63) - the engine calls it from Mod_BuildConvHull unconditionally.
         Without it the swept player box catches on prop edges. Bevels need the piece's VERT set, so a
         correct offline format is probably v3 = v1's verts PLUS prebuilt planes.
      3. NO Patch-60 conservative-outward push - CoACD face dists are not guaranteed to enclose the
         source mesh, so hulls can clip INTO the visible model.

    The real lesson: any offline baker must re-implement the engine's hull pipeline and will drift
    from it silently. Prefer having the ENGINE write the cache next to Mod_BuildConvHull, where it is
    byte-identical by construction. See the note in Mod_LoadACDSidecar (common/com_mesh.c).
    """
    """Convex piece (verts + triangle faces) -> (planes, tris, mins, maxs) in convhull_t form.

    CoACD already emits each piece as a CONVEX mesh, so its own faces ARE the hull surface: we do
    not need to re-run a hull algorithm (and must not, or the collision geometry would drift from
    the asset). We only need to derive the plane set the engine traces against.

    Normals are oriented by the CENTROID rather than by winding: convex-decomposer output winding is
    not something to bet collision correctness on, and for a convex piece the centroid is strictly
    inside, so `dot(n, centroid) - dist > 0` unambiguously means the normal points the wrong way.
    """
    import math
    tris_out = []
    planes = []

    cx = sum(v[0] for v in verts) / len(verts)
    cy = sum(v[1] for v in verts) / len(verts)
    cz = sum(v[2] for v in verts) / len(verts)

    for f in faces:
        a, b, c = verts[f[0]], verts[f[1]], verts[f[2]]
        ux, uy, uz = b[0]-a[0], b[1]-a[1], b[2]-a[2]
        vx, vy, vz = c[0]-a[0], c[1]-a[1], c[2]-a[2]
        nx, ny, nz = uy*vz - uz*vy, uz*vx - ux*vz, ux*vy - uy*vx
        ln = math.sqrt(nx*nx + ny*ny + nz*nz)
        if ln <= DEGEN_AREA_EPS:
            continue                      # degenerate sliver: no plane, and no use as a viz tri
        nx, ny, nz = nx/ln, ny/ln, nz/ln
        d = nx*a[0] + ny*a[1] + nz*a[2]
        if (nx*cx + ny*cy + nz*cz) - d > 0:   # centroid outside => inward normal => flip
            nx, ny, nz, d = -nx, -ny, -nz, -d

        tris_out.append((a, b, c))

        for p in planes:                  # merge coplanar faces onto one plane
            if (abs(p[0]-nx) < PLANE_EPS and abs(p[1]-ny) < PLANE_EPS and
                abs(p[2]-nz) < PLANE_EPS and abs(p[3]-d) < PLANE_EPS):
                break
        else:
            if len(planes) >= cap:
                # Over cap: mirror Mod_BuildConvHull's Patch 57 behaviour EXACTLY - merge this face
                # into the most-parallel existing plane and keep the LOOSER (larger) .w, i.e. push
                # that plane conservatively outward. The result stays a real, valid, convex hull at
                # <= cap planes that never clips INTO the model. Do not instead drop the face: that
                # would open the volume and let traces through.
                best, bestdot = 0, -2.0
                for i, p in enumerate(planes):
                    dot = p[0]*nx + p[1]*ny + p[2]*nz
                    if dot > bestdot:
                        bestdot, best = dot, i
                if d > planes[best][3]:
                    planes[best][3] = d
            else:
                planes.append([nx, ny, nz, d])

    mins = [min(v[i] for v in verts) for i in range(3)]
    maxs = [max(v[i] for v in verts) for i in range(3)]
    return planes, tris_out, mins, maxs


def write_acd_v2(path, hulls):
    """v2: hulls = list of (planes, tris, mins, maxs) from build_hull_planes; engine reads verbatim."""
    with open(path, 'wb') as o:
        o.write(b'FCAD')
        o.write(struct.pack('<i', 2))
        o.write(struct.pack('<i', len(hulls)))
        for planes, tris, mins, maxs in hulls:
            o.write(struct.pack('<i', len(planes)))
            for p in planes:
                o.write(struct.pack('<4f', float(p[0]), float(p[1]), float(p[2]), float(p[3])))
            o.write(struct.pack('<i', len(tris)))
            for (a, b, c) in tris:
                for v in (a, b, c):
                    o.write(struct.pack('<3f', float(v[0]), float(v[1]), float(v[2])))
            o.write(struct.pack('<3f', float(mins[0]), float(mins[1]), float(mins[2])))
            o.write(struct.pack('<3f', float(maxs[0]), float(maxs[1]), float(maxs[2])))


def main():
    ap = argparse.ArgumentParser(description="Bake a CoACD convex decomposition into a .acd sidecar for FTEQW.")
    ap.add_argument("iqm", help="input .iqm model")
    ap.add_argument("-o", "--out", help="output .acd (default: <model>.acd next to the iqm)")
    ap.add_argument("-t", "--threshold", type=float, default=0.05,
                    help="CoACD concavity threshold 0.01..1 (smaller = more pieces, default 0.05)")
    ap.add_argument("-m", "--max-hulls", type=int, default=48,
                    help="cap on convex pieces (<= %d, the engine limit; default 48)" % ENGINE_PIECE_CAP)
    ap.add_argument("--single", action="store_true",
                    help="Force ONE convex hull: write a 1-piece sidecar containing the model's own "
                         "verts. No CoACD run (instant). Use for props that are convex enough that a "
                         "single hull IS the shape - boxes, barrels, crates, bushes. Saves the whole "
                         "load-time decomposition AND makes every trace cheaper.")
    ap.add_argument("--v2-unsafe", action="store_true",
                    help="write the v2 (prebuilt-hull) format. BROKEN - see build_hull_planes: the "
                         "hulls diverge from Mod_BuildConvHull (no bevels, no Patch-60 push, wrong "
                         "merge tolerance -> inflated hulls / invisible collision). Default is v1.")
    args = ap.parse_args()

    out = args.out or (os.path.splitext(args.iqm)[0] + ".acd")

    if args.single:
        # ONE hull, no decomposition. A 1-piece sidecar hands the engine the model's whole vert set,
        # so Mod_LoadACDSidecar runs Mod_BuildConvHull exactly ONCE and numhulls=1 -- the engine's own
        # hull builder still does the work (bevels, Patch-60 push, 2-degree merge, 256 cap all intact),
        # so this cannot drift from the runtime the way the v2 baker did.
        # Wins vs letting the runtime ACD decompose it: no concavity recursion at load (that is the
        # expensive part - up to 64 hull builds), and one piece to walk per trace instead of N.
        # Only correct where a single convex hull IS the collision shape you want: a box, barrel,
        # crate or bush. On a genuinely concave prop (arch, pipe, chair) this fills in the hollow.
        # No CoACD, so it is instant.
        verts, _tris = parse_iqm(args.iqm)
        if len(verts) < 4:
            sys.exit("ERROR: %s has %d verts - need >=4 for a hull" % (args.iqm, len(verts)))
        write_acd(out, [verts])
        print("wrote %s: v1, 1 piece (%d verts) - forces a SINGLE convex hull, no load-time decomposition"
              % (out, len(verts)))
        print("in-game:  sv_prop_collision 3 ; sv_prop_decomp 2 ; reload   (r_showhull 1 to check it)")
        return

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

    # parts = list of (vertices, faces); drop degenerate pieces (<4 verts can't bound a volume).
    usable = [p for p in parts if len(p[0]) >= 4]
    if not usable:
        sys.exit("ERROR: CoACD produced no usable pieces")
    if len(usable) > ENGINE_PIECE_CAP:
        print("WARNING: %d pieces > engine cap %d; truncating (raise -m / -t)." % (len(usable), ENGINE_PIECE_CAP))
        usable = usable[:ENGINE_PIECE_CAP]

    if not args.v2_unsafe:
        pieces = [p[0] for p in usable]
        write_acd(out, pieces)
        print("wrote %s: v1, %d convex pieces (%d verts total) - engine builds each hull at load"
              % (out, len(pieces), sum(len(p) for p in pieces)))
    else:
        print("WARNING: --v2-unsafe produces hulls that DIVERGE from Mod_BuildConvHull "
              "(no bevels, no Patch-60 push, wrong merge tolerance). See build_hull_planes.")
        hulls = []
        for (pv, pf) in usable:
            planes, htris, mins, maxs = build_hull_planes(pv, pf)
            # The engine rejects a piece with <4 planes (Mod_LoadACDSidecar): fewer than a
            # tetrahedron cannot bound a closed volume, and one bad piece fails the whole sidecar
            # back to the runtime ACD. Drop it here instead.
            if len(planes) < 4:
                print("  skipping a piece with %d planes (degenerate)" % len(planes))
                continue
            hulls.append((planes, htris, mins, maxs))
        if not hulls:
            sys.exit("ERROR: no piece produced a valid hull (>=4 planes)")
        write_acd_v2(out, hulls)
        print("wrote %s: v2, %d convex pieces (%d planes, %d tris total) - prebuilt, no load-time hull build"
              % (out, len(hulls), sum(len(h[0]) for h in hulls), sum(len(h[1]) for h in hulls)))
    print("in-game:  sv_prop_collision 3 ; sv_prop_decomp 2 ; reload")


if __name__ == "__main__":
    main()
