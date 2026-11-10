#!/usr/bin/env python3
"""Actual Begin/Drop versus an admitted synthetic worker; no input/game actors.

Uses real nonrecursive OS mutexes and actual engine permission/append/lifecycle
functions. The worker ownership interval is an explicit host model of SendCmd,
not the actual worker loop or a claim that normal appends/End are thread-safe.
"""
import argparse
import os
from pathlib import Path
import subprocess

from test_input_journal_end import HEADER, close_support

PRE = r'''
#include <pthread.h>
#include <setjmp.h>
#include <time.h>
#include <errno.h>
static void thread_before_realloc(void);
static void hash_init(void *p);
'''

HOST = r'''
static pthread_mutex_t physics = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t state = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static pthread_t worker_tid, main_tid;
static int schedule, paused, release_worker, worker_busy, main_ready, attempted, unsafe, done;
static int lock_calls, unlock_calls, operation;
static jmp_buf main_abort;
static char *freed_snapshot;
static bool allowindepphys;
static void *indeplock = &physics;
static void Sys_LockMutex(void *p) {
    pthread_mutex_lock(&state);
    lock_calls++;
    if (schedule && main_ready && pthread_equal(pthread_self(), main_tid)) {
        attempted = 1; pthread_cond_broadcast(&changed);
    }
    pthread_mutex_unlock(&state);
    if (pthread_mutex_lock(p)) abort();
}
static void Sys_UnlockMutex(void *p) {
    pthread_mutex_lock(&state); unlock_calls++; pthread_mutex_unlock(&state);
    if (pthread_mutex_unlock(p)) abort();
}
static void thread_before_realloc(void) {
    pthread_mutex_lock(&state);
    if (schedule && pthread_equal(pthread_self(), worker_tid)) {
        paused = 1; pthread_cond_broadcast(&changed);
        while (!release_worker) pthread_cond_wait(&changed, &state);
    }
    pthread_mutex_unlock(&state);
}
static void BZ_Free(void *p) {
    pthread_mutex_lock(&state);
    if (schedule && worker_busy && pthread_equal(pthread_self(), main_tid)) {
        unsafe = 1; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&state);
        longjmp(main_abort, 1); /*Stop the old code BEFORE freeing live storage.*/
    }
    pthread_mutex_unlock(&state);
    if (p == in_jrn_buf) {
        drops++;
        free(freed_snapshot); freed_snapshot = copy_bytes(p, in_jrn_len);
    }
    free(p);
}
static void *BZ_Malloc(size_t n) { void *p = malloc(n); if (!p) abort(); return p; }
static void hash_init(void *p) { (void)p; }
typedef struct { float value; int ival, modifiedcount; const char *name, *string, *defaultstr, *enginevalue; } cvar_t;
#define CVAR(n) static cvar_t n = {.value=1, .ival=1}
CVAR(sensitivity); CVAR(m_yaw); CVAR(m_pitch); CVAR(m_filter); CVAR(m_accel);
CVAR(m_accel_style); CVAR(m_accel_power); CVAR(m_accel_offset); CVAR(m_accel_senscap);
static cvar_t in_journal_maxkb = {.value=256};
static float in_sensitivityscale = 1;
static struct { int serverinfo; } cl;
static const char *InfoBuf_ValueForKey(const void *p, const char *key) { (void)p; (void)key; return "synthetic"; }
static cvar_t *Cvar_FindVar(const char *name) { (void)name; return NULL; }
static const char *COM_QuotedString(const char *s, char *out, size_t n, bool quoted) {
    (void)quoted; snprintf(out,n,"%s",s); return out;
}
static bool in_jrn_synth, in_jrn_haveabs[2][4], in_jrn_ran, in_jrn_vhavelast;
static float in_jrn_vdx, in_jrn_vdy, in_jrn_vkpitch, in_jrn_vkyaw, in_jrn_vrawx, in_jrn_vrawy;
static int in_jrn_vflags;
static unsigned int in_jrn_vcount, in_jrn_vskip, in_jrn_vbad;
static unsigned int in_jrn_violations[2], in_jrn_ghosts[2];
#define PITCH 0
#define YAW 1
static int in_jrn_dropreported, in_jrn_injreported, in_jrn_unenumreported;
static int in_jrn_lgbreported, in_jrn_lgkreported, in_jrn_nhkreported, in_jrn_obsreported;
static int in_jrn_padreported, in_jrn_prreported, in_jrn_prlreported, in_jrn_nolegacyreported;
static int in_raw_legacykey, in_raw_kbdnohandle, in_rawkbd_observechanges;
static int in_raw_padrelease, in_raw_nolegacy_live, in_rawkbd_observe;
static int in_rawmice_live, in_rawkbd_live, in_rawpads_live;
#define MAX_JRN_TRACKEDCVARS 16
static int in_jrn_rcvcount, in_jrn_rcvmod[MAX_JRN_TRACKEDCVARS];
static cvar_t *in_jrn_rcv[MAX_JRN_TRACKEDCVARS];
static const char *in_jrn_inputcvars[] = {"test_input",NULL};
static const char *in_jrn_rendercvars[] = {"test_render",NULL};
'''

TEST = r'''
static void check(bool ok, const char *label) {
    checks++; if (!ok) { failures++; fprintf(stderr,"FAIL %s\n",label); }
}
static void seed(void) {
    in_jrn_cap=65536; in_jrn_max=262144; in_jrn_len=65520;
    in_jrn_buf=malloc(in_jrn_cap+1); if (!in_jrn_buf) abort();
    memset(in_jrn_buf,'x',in_jrn_len); in_jrn_buf[in_jrn_len-1]='\n';
    in_jrn_hctx=NULL; in_jrn_full=false;
}
static void wait_for(int *flag) {
    struct timespec until;
    clock_gettime(CLOCK_REALTIME,&until); until.tv_sec+=5;
    while (!*flag) if (pthread_cond_timedwait(&changed,&state,&until)==ETIMEDOUT) abort();
}
static void *worker(void *p) {
    (void)p;
    pthread_mutex_lock(&state); worker_tid=pthread_self(); pthread_mutex_unlock(&state);
    Sys_LockMutex(indeplock);
    pthread_mutex_lock(&state); worker_busy=1; pthread_mutex_unlock(&state);
    IN_Journal_Raw("# worker-row 01234567890123456789012345678901234567890123456789\n");
    pthread_mutex_lock(&state); worker_busy=0; pthread_mutex_unlock(&state);
    Sys_UnlockMutex(indeplock);
    return NULL;
}
static void *main_actor(void *p) {
    (void)p;
    pthread_mutex_lock(&state); main_tid=pthread_self(); main_ready=1; pthread_mutex_unlock(&state);
    if (!setjmp(main_abort)) {
        if (operation) IN_JournalBegin_f(); else IN_Journal_Drop();
    }
    pthread_mutex_lock(&state); done=1; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&state);
    return NULL;
}
static void competing_case(int begin) {
    pthread_t w,m;
    struct timespec until;
    int was_unsafe;
    operation=begin; schedule=1; paused=release_worker=worker_busy=main_ready=attempted=unsafe=done=0;
    runningindepphys=allowindepphys=true; drops=0; seed();
    if (pthread_create(&w,NULL,worker,NULL)) abort();
    pthread_mutex_lock(&state); wait_for(&paused); pthread_mutex_unlock(&state);
    if (pthread_create(&m,NULL,main_actor,NULL)) abort();
    pthread_mutex_lock(&state);
    clock_gettime(CLOCK_REALTIME,&until); until.tv_sec+=5;
    while (!attempted && !unsafe && !done)
        if (pthread_cond_timedwait(&changed,&state,&until)==ETIMEDOUT) abort();
    was_unsafe=unsafe;
    check(!unsafe,"lifecycle cannot enter free/reset while worker admitted");
    check(attempted && !done,"actual mutex admission blocks before lifecycle completion");
    release_worker=1; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&state);
    if (pthread_join(w,NULL) || pthread_join(m,NULL)) abort();
    check(done && !worker_busy,"released worker and lifecycle both complete");
    check(allowindepphys,"previous worker permission restored");
    if (!was_unsafe) {
        check(drops==1,"old journal storage freed exactly once");
        check(freed_snapshot && strstr(freed_snapshot,"# worker-row "),"completed worker bytes precede free");
        if (begin) {
            char *s=copy_bytes(in_jrn_buf,in_jrn_len);
            check(!strncmp(s,"FTESURF-HID 1\n",14) && strstr(s,"\nbegin\n"),"actual replacement header and begin emitted");
            free(s);
        } else check(!in_jrn_buf && !in_jrn_len && !in_jrn_cap && !in_jrn_hctx,"actual drop resets storage");
    }
    schedule=0; runningindepphys=false; IN_Journal_Drop();
}
int main(void) {
    bool previous;
    int locks,unlocks;
    (void)IN_JournalEnd_f; /*Dependency closure only: not a close-concurrency test.*/
    competing_case(0); competing_case(1);
    main_tid=pthread_self(); runningindepphys=allowindepphys=true;
    previous=CL_AllowIndependantSendCmd(false);
    locks=lock_calls; unlocks=unlock_calls;
    IN_JournalBegin_f();
    check(!allowindepphys && lock_calls==locks && unlock_calls==unlocks,"nested Begin retains outer ownership");
    IN_Journal_Drop();
    check(!allowindepphys && lock_calls==locks && unlock_calls==unlocks,"nested Drop neither reacquires nor releases");
    CL_AllowIndependantSendCmd(previous);
    check(allowindepphys && unlock_calls==unlocks+1,"outer owner alone releases");
    runningindepphys=false; locks=lock_calls; unlocks=unlock_calls;
    IN_JournalBegin_f(); IN_Journal_Drop();
    check(lock_calls==locks && unlock_calls==unlocks,"disabled worker requires no mutex operation");
    free(freed_snapshot); free(snapshot); free(written);
    printf("journal replacement/drop: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--work-root', type=Path, required=True)
    ap.add_argument('--cc', default='gcc')
    ap.add_argument('--source-ref', help='Optional inspected Git commit for acting prechange control')
    a = ap.parse_args()
    a.work_root.mkdir(parents=True, exist_ok=False)
    (a.work_root / 'OWNER.md').write_text('Owned synthetic OS-thread journal lifecycle fixture. No game/input actors or player data.\n')
    root = Path(__file__).resolve().parents[1]
    if a.source_ref:
        ref = subprocess.check_output(['git', '-C', str(root), 'rev-parse', '--verify', a.source_ref + '^{commit}'], text=True).strip()
        def blob(path):
            return subprocess.check_output(['git', '-C', str(root), 'show', ref + ':' + path], text=True, encoding='utf-8')
        text, ci = blob('engine/client/in_generic.c'), blob('engine/client/cl_input.c')
    else:
        text = (root / 'engine/client/in_generic.c').read_text()
        ci = (root / 'engine/client/cl_input.c').read_text()
    helper = ci[ci.index('qboolean CL_AllowIndependantSendCmd('):ci.index('int CL_IndepPhysicsThread(')]
    raw = text[text.index('static void IN_Journal_Raw('):text.index('void IN_Journal_Note(')]
    drop = text[text.index('void IN_Journal_Drop(void)'):text.index('static void IN_Journal_Event(')]
    start = text.index('static void IN_JournalBegin_f(void)\n{')
    lifecycle = text[start:text.index('static void IN_JournalNote_f(', start)]
    h = HEADER[:HEADER.index('static void IN_Journal_Drop(void)')]
    assert h.count('void *q = realloc(p, n + 1);') == 1
    h = h.replace('void *q = realloc(p, n + 1);', 'thread_before_realloc();\n    void *q = realloc(p, n + 1);')
    assert h.count('void (*process)(void *, const void *, size_t);') == 1
    h = h.replace('void (*process)(void *, const void *, size_t);', 'void (*process)(void *, const void *, size_t); size_t contextsize; void (*init)(void *);')
    h = h.replace('hash_sha2_256 = {hash_process};', 'hash_sha2_256 = {hash_process,1,hash_init};')
    if 'in_jrn_mainactive' in text:
        h += '\nstatic qboolean in_jrn_mainactive;\n'
    if 'in_jrn_mainsynth' in text:
        h += '\nstatic qboolean in_jrn_mainsynth;\n'
    c = a.work_root / 'journal-lifecycle.c'
    exe = a.work_root / 'journal-lifecycle.exe'
    c.write_text(PRE + h + HOST + helper + close_support(text) + raw + drop + lifecycle + TEST)
    env = dict(os.environ)
    if Path(a.cc).is_absolute():
        env['PATH'] = str(Path(a.cc).parent) + os.pathsep + env.get('PATH', '')
    subprocess.run([a.cc, '-std=c99', '-Wall', '-Wextra', '-Werror', '-pthread', str(c), '-o', str(exe)], check=True, env=env)
    result = subprocess.run([str(exe)], capture_output=True, text=True, env=env, timeout=30)
    (a.work_root / 'result.txt').write_text(result.stdout + result.stderr)
    print(result.stdout, end='')
    if result.returncode:
        print(result.stderr, end='')
    raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()
