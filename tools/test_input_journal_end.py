#!/usr/bin/env python3
"""Compile actual journal append/close functions; no game, input capture or player data.

Host calls (input flush, device enumeration, clock, digest and filesystem) are
explicit stubs. This checks terminal byte completeness/lifecycle, not capture,
cryptography, reader admissibility or threaded-physics synchronization.
"""
import argparse
import os
from pathlib import Path
import subprocess

HEADER = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
typedef bool qboolean;
#define MAX_QPATH 64
#define FS_GAMEONLY 0
#define CON_WARNING ""
#define IN_JRN_HASHSTEP 16384
#define Q_snprintfz snprintf
static char *in_jrn_buf;
static size_t in_jrn_len, in_jrn_cap, in_jrn_max, in_jrn_hashed, in_jrn_digestlen;
static bool in_jrn_full, in_jrn_rehash, in_jrn_digestok, runningindepphys;
static void *in_jrn_hctx;
static double in_jrn_last, in_jrn_base, in_jrn_digestms;
static unsigned in_jrn_events, in_jrn_frames, in_jrn_dropped, in_jrn_dropbase, in_jrn_hidden;
static int in_raw_injected, in_jrn_injbase, in_raw_unenum, in_jrn_unenumbase;
static int in_raw_legacybtn, in_jrn_lgbbase, in_raw_touchpad, in_jrn_padbase;
static int in_raw_padreports, in_jrn_prbase;
static char *snapshot, *written;
static size_t snapshot_len, written_len;
static int digests, writes, drops, failures, checks, grows, argc_value;
static bool last_kept, write_ok;
static const char *arg_name;
static double Sys_DoubleTime(void) { return 2.0; }
static bool Sys_IsMainThread(void) { return true; }
static void *BZ_Realloc(void *p, size_t n) {
    void *q = realloc(p, n + 1);
    if (!q) abort();
    grows++;
    return q;
}
static void hash_process(void *ctx, const void *p, size_t n) {
    (void)ctx; (void)p; (void)n;
}
static const struct { void (*process)(void *, const void *, size_t); }
    hash_sha2_256 = {hash_process};
static int Cmd_Argc(void) { return argc_value; }
static const char *Cmd_Argv(int n) { (void)n; return arg_name; }
static void Con_Printf(const char *fmt, ...) { (void)fmt; }
#define Con_DPrintf Con_Printf
#define IN_Journal_Rejected(x) ((void)(x))
#define IN_Journal_Pad(x) ((void)(x))
#define IN_Journal_Legacy(x) ((void)(x))
#define IN_Journal_RenderCvars(x) ((void)(x))
#define IN_Journal_Bypassed(x) ((void)(x))
#define IN_Journal_Keys() ((void)0)
static void IN_Journal_DeviceLine(void) {}
static void INS_EnumerateDevices(void *ctx, void (*callback)(void)) {
    (void)ctx; (void)callback;
}
static bool QC_FixFileName(const char *s, const char **name, const char **fallback) {
    *name = s; *fallback = NULL; return true;
}
static char *copy_bytes(const char *p, size_t n) {
    char *s = malloc(n + 1);
    if (!s) abort();
    memcpy(s, p, n); s[n] = 0; return s;
}
static void IN_Journal_Digest(bool kept) {
    free(snapshot); snapshot = copy_bytes(in_jrn_buf, in_jrn_len);
    snapshot_len = in_jrn_digestlen = in_jrn_len;
    in_jrn_digestok = true; last_kept = kept; digests++;
}
static bool COM_WriteFile(const char *s, int root, const void *p, size_t n) {
    (void)s; (void)root;
    free(written); written = copy_bytes(p, n); written_len = n;
    writes++; return write_ok;
}
static void IN_Journal_Drop(void) {
    free(in_jrn_buf); in_jrn_buf = NULL; in_jrn_len = in_jrn_cap = 0; drops++;
}
'''

TEST = r'''
static void check(bool ok, const char *label) {
    checks++;
    if (!ok) { failures++; fprintf(stderr, "FAIL %s\n", label); }
}
static int occurrences(const char *s, const char *needle) {
    int n = 0; const char *p = s;
    while ((p = strstr(p, needle))) { n++; p += strlen(needle); }
    return n;
}
static void seed(size_t cap, size_t max, size_t len, int mode) {
    free(snapshot); free(written); snapshot = written = NULL;
    snapshot_len = written_len = 0;
    in_jrn_buf = malloc(cap + 1);
    if (!in_jrn_buf || len > cap || len < 1) abort();
    memset(in_jrn_buf, 'x', len); in_jrn_buf[len - 1] = '\n';
    in_jrn_len = len; in_jrn_cap = cap; in_jrn_max = max;
    in_jrn_last = in_jrn_base = 1.0;
    in_jrn_full = in_jrn_digestok = false;
    digests = writes = drops = grows = 0; write_ok = mode != 3;
    argc_value = mode == 1 ? 1 : 2;
    arg_name = mode == 2 ? "data/not-a-journal.txt" : "data/test.hid";
}
static void finish(size_t prefix, int mode, int expected_truncated) {
    size_t i;
    IN_JournalEnd_f();
    check(snapshot && snapshot_len > prefix, "close actually reached digest with appended bytes");
    if (!snapshot) return;
    for (i = 0; i + 1 < prefix; i++) if (snapshot[i] != 'x') break;
    check(i + 1 == prefix && snapshot[prefix - 1] == '\n', "body bytes preserved");
    check(occurrences(snapshot, "\nend ") == 1, "one real terminal end record");
    check(snapshot[snapshot_len - 1] == '\n', "terminal newline");
    check(occurrences(snapshot, "\ntruncated ") == expected_truncated, "only genuine truncation marked");
    check(drops == 1 && !in_jrn_buf, "buffer dropped exactly once");
    check(digests == ((mode == 3 && !JOURNAL_CLOSE_DETACHED) ? 2 : 1), "expected digest lifecycle");
    check(writes == (mode == 0 || mode == 3), "expected write lifecycle");
    check(last_kept == (mode == 0), "kept/discard/refusal/write-failure state");
    if (writes) check(written_len == snapshot_len && !memcmp(written, snapshot, snapshot_len),
                      "write covers exact complete digested bytes");
}
int main(void) {
    const size_t spare[] = {1, 16, 32, 64, 128, 256, 512};
    const size_t caps[] = {65536, 131072};
    size_t i, j, prefix;
    int mode;
    for (i = 0; i < sizeof(caps)/sizeof(caps[0]); i++) {
        for (j = 0; j < sizeof(spare)/sizeof(spare[0]); j++) {
            for (mode = 0; mode < 4; mode++) {
                prefix = caps[i] - spare[j];
                seed(caps[i], 262144, prefix, mode);
                finish(prefix, mode, 0);
            }
        }
    }
    /*At the configured limit, genuine loss stays announced once, then end fits
      in the existing reserved allocation. Include the minimum supported cap.*/
    for (i = 4096; i <= 65536; i *= 16) {
        for (mode = 0; mode < 4; mode++) {
            prefix = i - 1;
            seed(i + 1024, i, prefix, mode);
            IN_Journal_Line(1.5, "#", "body that really exceeds the configured cap");
            check(in_jrn_full, "body limit acts before close");
            finish(prefix, mode, 1);
        }
    }
    /*A complete body that exactly fills an allocation still grows at close.*/
    seed(65536, 262144, 65536, 0); finish(65536, 0, 0);
    check(grows > 0, "ordinary terminal actually uses production growth path");
    in_jrn_digestok = true;
    IN_JournalEnd_f();
    check(!in_jrn_digestok && drops == 1, "no-open close invalidates stale digest");
    free(snapshot); free(written);
    printf("journal close: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
'''


def close_support(text, host_stubs=False):
    """Extract actual pending-close state/helpers; hosts provide the dependencies."""
    marker = 'typedef struct in_jrn_close_s'
    if marker not in text:
        return '#define JOURNAL_CLOSE_DETACHED 0\n'
    extra = '''
#define JOURNAL_CLOSE_DETACHED 1
typedef unsigned char qbyte;
static qbyte in_jrn_digest[32];
#define in_jrn_digestkept last_kept
'''
    if host_stubs:
        extra += '''
static qboolean CL_AllowIndependantSendCmd(qboolean allow) { (void)allow; return false; }
static void BZ_Free(void *p) { free(p); }
'''
    return extra + text[text.index(marker):text.index('void IN_Journal_Drop(void)')]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--work-root', type=Path, required=True)
    ap.add_argument('--cc', default='gcc')
    a = ap.parse_args()
    a.work_root.mkdir(parents=True, exist_ok=False)
    (a.work_root / 'OWNER.md').write_text(
        'Owned synthetic journal-close compile fixture; no game actors or player data.\n')
    source = Path(__file__).resolve().parents[1] / 'engine/client/in_generic.c'
    text = source.read_text()
    raw = text[text.index('static void IN_Journal_Raw('):text.index('void IN_Journal_Note(')]
    # Skip the forward declaration: require the function definition itself.
    start = text.index('static void IN_JournalEnd_f(void)\n{')
    end = text.index('static void IN_JournalNote_f(', start)
    code = HEADER + close_support(text, host_stubs=True) + raw + text[start:end] + TEST
    c = a.work_root / 'journal-close.c'
    binary = a.work_root / 'journal-close.exe'
    c.write_text(code)
    env = dict(os.environ)
    if Path(a.cc).is_absolute():
        env['PATH'] = str(Path(a.cc).parent) + os.pathsep + env.get('PATH', '')
    subprocess.run([a.cc, '-std=c99', '-Wall', '-Wextra', '-Werror', str(c),
                    '-o', str(binary)], check=True, env=env)
    result = subprocess.run([str(binary)], text=True, capture_output=True, env=env)
    (a.work_root / 'result.txt').write_text(result.stdout + result.stderr)
    print(result.stdout, end='')
    if result.returncode:
        print(result.stderr, end='')
    raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()
