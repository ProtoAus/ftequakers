#!/usr/bin/env python3
"""Compile the exact raw EOF/purge callbacks with an allocation-counting adapter.

This is an isolated C ownership unit, not a mixer-thread or hardware model.
Control and subject callbacks come verbatim from the inspected repository.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define QDECL
#define false 0
typedef struct { void *buf; } sfxdecode_t;
typedef struct { sfxdecode_t decoder; } sfx_t;
typedef struct { sfxdecode_t decoder; int inuse, id; sfx_t *sfx;
 int numchannels, format, length; void *data; } streaming_t;
static unsigned allocations, frees;
void BZ_Free(void *p) { if (p) { ++frees; free(p); } }
/*CALLBACKS*/
int main(void) {
 streaming_t stream = {0}; sfx_t sfx = {{&stream}};
 unsigned i, released = 0;
 for (i = 0; i < 4096; ++i) {
  /* Same buffer ownership as the native new-cache/reuse path. */
  stream.data = malloc(512); if (!stream.data) return 3;
  ++allocations; stream.inuse = 1; stream.length = 256;
  S_Raw_Ended(&sfx);
  if (!stream.inuse && !stream.data && !stream.length) ++released;
  S_Raw_Ended(&sfx); /* idempotent late callback must not double-free */
  /* Next iteration reuses the ended slot. No intervening native purge. */
 }
 S_Raw_Purge(&sfx); /* shutdown can recover only the final retained buffer */
 printf("{\"allocations\":%u,\"frees\":%u,\"released_cycles\":%u,\"unreclaimed\":%u}\n",
        allocations, frees, released, allocations - frees);
 return allocations != frees || released != 4096;
}
'''


def callbacks(text):
    result = []
    for name in ("Ended", "Purge"):
        m = re.search(r"void QDECL S_Raw_" + name + r"\(sfx_t \*sfx\)\s*\{.*?\n\}", text, re.S)
        if not m:
            raise RuntimeError("exact raw callback not found: " + name)
        result.append(m.group())
    return "\n".join(result)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--cc", default="C:/msys64/ucrt64/bin/gcc.exe")
    p.add_argument("--control-ref", required=True)
    a = p.parse_args()
    repo = Path(__file__).resolve().parent.parent
    a.out.mkdir(parents=True, exist_ok=True)
    rel = "engine/client/snd_dma.c"
    control = subprocess.check_output(["git", "-C", str(repo), "show", a.control_ref + ":" + rel], text=True)
    subject = (repo / rel).read_text()
    report = {}
    cc_env = dict(os.environ, PATH=str(Path(a.cc).parent) + os.pathsep + os.environ.get("PATH", ""))
    for name, text in (("control", control), ("subject", subject)):
        exact = callbacks(text)
        src, exe = a.out / (name + ".c"), a.out / (name + ".exe")
        src.write_text(HARNESS.replace("/*CALLBACKS*/", exact))
        subprocess.run([a.cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-O2", str(src), "-o", str(exe)], check=True, env=cc_env)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        counts = json.loads(r.stdout)
        report[name] = {"returncode": r.returncode, "callback_sha256": hashlib.sha256(exact.encode()).hexdigest(), **counts}
    # The old callbacks ACT, but orphan all except the shutdown-purged last buffer.
    assert report["control"]["returncode"] == 1 and report["control"]["unreclaimed"] == 4095, report
    assert report["control"]["allocations"] == 4096 and report["control"]["released_cycles"] == 0, report
    assert report["subject"]["returncode"] == 0 and report["subject"]["unreclaimed"] == 0, report
    assert report["subject"]["frees"] == report["subject"]["released_cycles"] == 4096, report
    (a.out / "result.json").write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))
    print("PASS exact C callback ownership: acting control leaks; subject balances 4096 drain/reuse cycles")


if __name__ == "__main__":
    main()
