#!/usr/bin/env python3
"""Unit-test the native enumeration adapter with a recording platform stub."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

SOURCE = Path(__file__).resolve().parents[1] / "engine/common/fs.c"

SHIM = r"""
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef long qofs_t;
typedef struct searchpathfuncs_s searchpathfuncs_t;
#define MAX_QPATH 64

static const char *FS_GetCleanPath(const char *s, bool quiet, char *out, int len)
{
    (void)quiet;
    if (!strcmp(s, "rejected")) return NULL;
    snprintf(out, len, "%s", s);
    return out;
}

struct state {
    char seen[80];
    int invalid;
    bool stop;
};

static int callback(const char *s, qofs_t len, time_t modified, void *parm, searchpathfuncs_t *spath)
{
    struct state *st = parm;
    (void)len; (void)modified; (void)spath;
    size_t used = strlen(st->seen);
    snprintf(st->seen + used, sizeof(st->seen) - used, "%s;", s);
    return !st->stop;
}

static int Sys_EnumerateFiles(const char *root, const char *match,
        int (*visit)(const char *, qofs_t, time_t, void *, searchpathfuncs_t *),
        void *parm, searchpathfuncs_t *spath)
{
    (void)root;
    if (!match || !*match)
    {
        ((struct state *)parm)->invalid++;
        return true; // just record an invalid handoff; never run a platform filesystem call
    }
    return visit(match, 0, 0, parm, spath);
}
"""

TEST = r"""
static int check(const char *name, char *patterns, bool stop, const char *expected)
{
    struct state st = {.stop = stop};
    int completed = FS_EnumerateFilesEachSys("unused", patterns, callback, &st, NULL);
    int ok = !st.invalid && completed == !stop && !strcmp(st.seen, expected);
    printf("%s: %s (invalid handoffs: %d; visited: %s)\n", name, ok ? "PASS" : "FAIL", st.invalid, st.seen);
    return !ok;
}

int main(void)
{
    char valid[] = "a*:b*";
    char mixed[] = "a*::rejected:b*:";
    char stopped[] = "a*:b*";
    char invalid[] = "rejected:";
    int failed = check("valid patterns", valid, false, "a*;b*;");
    failed |= check("skip rejected and empty matches", mixed, false, "a*;b*;");
    failed |= check("callback stop", stopped, true, "a*;");
    failed |= check("only empty or rejected", invalid, false, "");
    return failed;
}
"""


def main():
    text = (Path(sys.argv[1]) if len(sys.argv) == 2 else SOURCE).read_text(encoding="utf-8")
    start = text.index("static int FS_EnumerateFilesEachSys (")
    body = text[start:text.index("\n}", start) + 2]
    with tempfile.TemporaryDirectory(prefix="fs-enumeration-") as tmp:
        c = Path(tmp) / "test.c"
        binary = Path(tmp) / "test"
        c.write_text("\n".join([SHIM, body, TEST]), encoding="utf-8")
        cc = shlex.split(os.environ.get("CC", "cc"))
        subprocess.run([*cc, "-std=c99", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(binary)], check=True)
        return subprocess.run([binary], check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
