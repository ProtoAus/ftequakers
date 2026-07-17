#!/usr/bin/env python3
"""
acd_simplify.py - batch-force ONE convex hull on props that are already close to convex.

WHY (this is a gameplay fix, not just a speed one):
  A decomposed prop is N convex pieces butted together, and every internal boundary is a SEAM the
  swept player box can catch on. A single hull has ZERO internal seams. So for "fast, works, players
  can run and jump on it, throw hundreds of them around", one hull is not a compromise - it is the
  better collision. It is also cheaper three times over:
    LOAD  - the engine runs Mod_BuildConvHull once per piece; 1 piece = 1 build, and no concavity
            recursion at all (that recursion is the expensive part - up to 64 hull builds).
    TRACE - PM_HullTrace / World_HullTrace walk every piece, and every plane in it, per trace.
    SIM   - the physics backend builds its shape from the hull; fewer pieces = a cheaper body.

HOW IT DECIDES (the safety question):
  The only real danger of one hull is FILLING IN A HOLE that players are meant to pass through - an
  arch, a doorway, a pipe you walk inside. So we measure exactly that:

      fill ratio = volume(convex hull) / volume(mesh)

  1.0 = the mesh IS its hull (a crate, a brick, a barrel) - one hull changes literally nothing.
  1.2 = the hull adds 20% - a chair, a rock: filling the small gaps is invisible in play.
  4.0 = the hull adds 300% - an arch or a tree: one hull would be a big invisible block. NOT touched.

  Props at or below --max-fill are rewritten to a 1-piece sidecar. Everything else is left alone and
  reported, so you can eyeball it with r_showhull 1 and force it by hand if you want:
      python acd_bake.py <model.iqm> --single

  Writing a 1-piece sidecar keeps the ENGINE's own hull builder in charge (bevels/Patch-60 push/
  2-degree merge/256 cap all intact), so unlike the reverted v2 baker this cannot drift from the
  runtime. Instant - no CoACD.

USAGE:
  python acd_simplify.py                      # DRY RUN over the default prop roots - reports only
  python acd_simplify.py --write              # actually write the 1-piece sidecars
  python acd_simplify.py --max-fill 1.4 --write
  python acd_simplify.py <root> [root..] --write

Sidecars are only ever ADDED/REPLACED; back them up first if you want a trivial undo:
  (the previous set is already at /tmp/acd_backup from the v2 revert)
"""
import sys, os, glob
import importlib.util as u

_here = os.path.dirname(os.path.abspath(__file__))
_spec = u.spec_from_file_location("acd_bake", os.path.join(_here, "acd_bake.py"))
ab = u.module_from_spec(_spec); _spec.loader.exec_module(ab)

import numpy as np
from scipy.spatial import ConvexHull

# Same roots as acd_bake_all.py: COLLIDABLE props only. models/player and models/gibs are excluded by
# the engine outright (sv_prop_hull_exclude) and must never get a sidecar.
DEFAULT_ROOTS = [r"C:\FTEQuake\nettest\models\%s" % f
                 for f in ("bunkers", "mega", "modular", "nature", "tech", "van", "props", "outrun")]
MAX_FILL = 1.25     # hull may add up to 25% volume and still be considered "basically this shape"
MIN_VERTS = 4


def read_acd_pieces(path):
    """Read a v1 .acd -> list of piece vertex arrays. None if absent/not v1."""
    import struct
    if not os.path.exists(path):
        return None
    with open(path, 'rb') as f:
        d = f.read()
    if len(d) < 12 or d[:4] != b'FCAD':
        return None
    ver, npieces = struct.unpack_from('<ii', d, 4)
    if ver != 1:
        return None
    p, out = 12, []
    try:
        for _ in range(npieces):
            nv, = struct.unpack_from('<i', d, p); p += 4
            out.append(np.frombuffer(d, dtype='<f4', count=nv * 3, offset=p).reshape(nv, 3).astype(np.float64))
            p += nv * 12
    except Exception:
        return None
    return out


def hull_volume(pts):
    try:
        return float(ConvexHull(np.asarray(pts, dtype=np.float64)).volume)
    except Exception:
        return 0.0   # flat/degenerate piece contributes nothing


def fill_ratio(path):
    """-> (ratio, detail). ratio = volume(single hull) / volume(the decomposition).

    Measured against the PIECES from the model's existing .acd, NOT against the raw mesh.
    The obvious metric - hull volume / signed-tetrahedron mesh volume - is WRONG here: that formula
    assumes a closed, consistently-wound manifold, and these props frequently are not (open bottoms,
    flipped faces, loose shells). It produced ratios BELOW 1.0, which is geometrically impossible for
    a convex hull, i.e. the mesh volume was garbage - it would have auto-simplified props based on a
    meaningless number.

    CoACD's pieces are convex by construction, so each has a well-defined volume no matter how ratty
    the source mesh is. Their sum approximates the true solid, so:
        ratio ~= 1.0  the pieces tile the hull  -> the prop IS convex; one hull is exact (crate, brick)
        ratio ~= 1.3  one hull adds 30%         -> small gaps filled; invisible in play
        ratio >> 2    one hull adds a lot       -> a real hole (arch, doorway, tree) - LEAVE IT
    Pieces can overlap slightly, which biases the sum UP and the ratio DOWN, so this errs toward
    reporting a prop as convex-ish. The 1.25 default keeps that margin small.
    """
    acd = os.path.splitext(path)[0] + ".acd"
    pieces = read_acd_pieces(acd)
    if pieces is None:
        return (None, "no v1 sidecar")
    if len(pieces) <= 1:
        return (None, "already 1 piece")
    allpts = np.concatenate(pieces, axis=0)
    if len(allpts) < MIN_VERTS:
        return (None, "too few verts")
    whole = hull_volume(allpts)
    partsum = sum(hull_volume(p) for p in pieces)
    if partsum <= 1e-9 or whole <= 1e-9:
        return (None, "degenerate volume")
    return (whole / partsum, "%d pieces" % len(pieces))


def main():
    argv = sys.argv[1:]
    write = "--write" in argv
    maxfill = MAX_FILL
    if "--max-fill" in argv:
        maxfill = float(argv[argv.index("--max-fill") + 1])
    roots = [a for a in argv if not a.startswith("--")
             and not a.replace(".", "", 1).isdigit()] or DEFAULT_ROOTS

    files = sorted(set(f for r in roots for f in glob.glob(os.path.join(r, "**", "*.iqm"), recursive=True)))
    print("%s %d models (max-fill %.2f)...\n" % ("SIMPLIFYING" if write else "DRY RUN over", len(files), maxfill))

    done, skipped, failed = [], [], []
    for f in files:
        rel = f.replace("\\", "/").split("/models/")[-1]
        try:
            ratio, detail = fill_ratio(f)
        except Exception as e:
            failed.append((rel, str(e)[:60])); continue
        if ratio is None:
            failed.append((rel, detail)); continue
        if ratio <= maxfill:
            if write:
                verts, _ = ab.parse_iqm(f)
                ab.write_acd(os.path.splitext(f)[0] + ".acd", [verts])
            done.append((rel, ratio, detail))
        else:
            skipped.append((rel, ratio, detail))

    done.sort(key=lambda r: r[1]); skipped.sort(key=lambda r: r[1])

    print("=== %s: %d models -> ONE hull (no seams, 1 build at load, 1 piece per trace) ===" %
          ("WROTE" if write else "WOULD WRITE", len(done)))
    for rel, r, det in done[:15]:
        print("   %-52s fill %.2f  was %s" % (rel[:52], r, det))
    if len(done) > 15:
        print("   ... and %d more" % (len(done) - 15))

    print("\n=== LEFT ALONE: %d models too hollow for one hull (would fill a real gap) ===" % len(skipped))
    for rel, r, det in skipped[:12]:
        print("   %-52s fill %.2f  %s" % (rel[:52], r, det))
    if len(skipped) > 12:
        print("   ... and %d more (closest to the line first - the ones worth eyeballing)" % (len(skipped) - 12))

    if failed:
        why = {}
        for _rel, w in failed:
            why[w] = why.get(w, 0) + 1
        print("\n=== NOT MEASURED: %d  %s ===" % (len(failed), why))
        print("   ('already 1 piece' / 'no v1 sidecar' are fine - nothing to simplify.)")

    if not write:
        print("\nDRY RUN - nothing written. Re-run with --write to apply.")
    else:
        print("\nin-game:  sv_prop_decomp 2 ; reload    (r_showhull 1 to eyeball the result)")


if __name__ == "__main__":
    main()
