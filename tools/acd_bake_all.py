#!/usr/bin/env python3
"""
Batch-bake .acd convex-decomposition sidecars for every IQM under the given roots (CoACD).
Writes <model>.acd next to each model ONLY when CoACD finds real concavity (>1 piece); convex
props are skipped (the engine's runtime ACD handles them identically with no asset). Tuned a bit
coarse (higher threshold + capped hulls) so simple props don't over-split. Deterministic (seed 0).

  python acd_bake_all.py                 # default: nettest models/{bunkers,mega,modular,nature,tech,van}
  python acd_bake_all.py <root> [root..] # custom roots
"""
import sys, os, glob, time
import importlib.util as u

_here = os.path.dirname(os.path.abspath(__file__))
spec = u.spec_from_file_location("acd_bake", os.path.join(_here, "acd_bake.py"))
ab = u.module_from_spec(spec); spec.loader.exec_module(ab)

import numpy as np, coacd
try:
    coacd.set_log_level("error")     # silence CoACD's per-iteration spam
except Exception:
    pass

DEFAULT_ROOTS = [r"C:\FTEQuake\nettest\models\%s" % f
                 for f in ("bunkers", "mega", "modular", "nature", "tech", "van")]
THRESHOLD = 0.08    # CoACD concavity tolerance (higher = fewer/coarser pieces; 0.05 default)
MAXHULLS  = 32      # cap pieces per model (well under the engine's 128)


def bake(path):
    try:
        verts, tris = ab.parse_iqm(path)
    except Exception as e:
        return ("parse-fail", 0, str(e)[:80])
    try:
        mesh = coacd.Mesh(np.asarray(verts, np.float64), np.asarray(tris, np.int32))
        parts = coacd.run_coacd(mesh, threshold=THRESHOLD, max_convex_hull=MAXHULLS,
                                preprocess_resolution=30, resolution=1000,
                                mcts_iterations=40, mcts_nodes=16, mcts_max_depth=2, seed=0)
    except Exception as e:
        return ("coacd-fail", 0, str(e)[:80])
    pieces = [p[0] for p in parts if len(p[0]) >= 4]
    if len(pieces) <= 1:
        return ("convex-skip", len(pieces), "")
    pieces = pieces[:ab.ENGINE_PIECE_CAP]
    out = os.path.splitext(path)[0] + ".acd"
    try:
        ab.write_acd(out, pieces)
    except Exception as e:
        return ("write-fail", len(pieces), str(e)[:80])
    return ("baked", len(pieces), out)


def main():
    roots = sys.argv[1:] or DEFAULT_ROOTS
    files = []
    for r in roots:
        files += glob.glob(os.path.join(r, "**", "*.iqm"), recursive=True)
    files = sorted(set(files))
    print("baking %d models (threshold=%.3f max_hulls=%d)..." % (len(files), THRESHOLD, MAXHULLS), flush=True)
    t0 = time.time()
    stat = {}
    for i, f in enumerate(files):
        t = time.time()
        kind, n, info = bake(f)
        stat[kind] = stat.get(kind, 0) + 1
        rel = f.replace("\\", "/").split("/models/")[-1]
        print("[%d/%d] %-11s %3d pc %6.1fs  %s" % (i + 1, len(files), kind, n, time.time() - t, rel),
              flush=True)
    print("DONE in %.1f min  %s" % ((time.time() - t0) / 60.0, stat), flush=True)


if __name__ == "__main__":
    main()
