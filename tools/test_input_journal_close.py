#!/usr/bin/env python3
"""Actual close/digest/lifecycle + real worker and callback/abort adapters.

No game, physical input, consent, backend VFS or loader actors. Actual engine
SHA256 and CalcHash are compiled. Injected returning callbacks are adversarial
host schedules, not a claim that production filesystem work executes commands.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess

from test_input_journal_end import HEADER
from test_input_journal_lifecycle import HOST, PRE

CRYPTO_TYPES = r'''
#ifndef JOURNAL_HOST_QUAKEDEF
#define JOURNAL_HOST_QUAKEDEF
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef unsigned char byte, qbyte;
typedef uint32_t quint32_t;
typedef uint64_t quint64_t;
#define fte_inlinestatic static inline
typedef struct {
    unsigned int digestsize, contextsize;
    void (*init)(void *);
    void (*process)(void *, const void *, size_t);
    void (*terminate)(unsigned char *, void *);
} hashfunc_t;
#endif
'''
EXTRA_PRE = r'''
#include <stdbool.h>
#include <stdint.h>
#include <malloc.h>
#define NORETURN __attribute__((noreturn))
#define VARGS
#define CON_ERROR ""
#define LOGI(...) ((void)0)
#define LOGE(...) ((void)0)
static void *host_realloc(void *, size_t);
static double host_clock(void);
static bool host_is_main(void);
#include "quakedef.h"
extern hashfunc_t hash_sha2_256;
'''
EXTRA_HOST = r'''
static bool COM_WriteFile(const char *, int, const void *, size_t);
static void IN_JournalBegin_f(void);
static void IN_JournalEnd_f(void);
void IN_Journal_Drop(void);
NORETURN void VARGS Host_EndGame(const char *, ...);
void VARGS SV_Error(const char *, ...);
NORETURN void Sys_Error(const char *, ...);
void Sys_Quit(void);
static struct { void *p; bool live, quarantine; } allocations[64];
static int borrowed_frees, double_frees, callbacks, io_depth, callback_mode;
static const void *borrowed;
static bool aborting, expect_outer, nested_ok, nested_kept;
static qbyte nested_digest[32];
static size_t nested_len;
static jmp_buf host_abort;
static int abort_kind;
static void check(bool ok, const char *label) {
    checks++; if (!ok) { failures++; fprintf(stderr,"FAIL %s\n",label); }
}
static int live_count(void) {
    int n=0; for (int i=0;i<64;i++) n+=allocations[i].live; return n;
}
static int allocation(void *p) {
    for (int i=0;i<64;i++) if (allocations[i].p==p && allocations[i].live) return i;
    return -1;
}
static void *BZ_Malloc(size_t n) {
    void *p=malloc(n); if (!p) abort();
    for (int i=0;i<64;i++) if (!allocations[i].live && !allocations[i].quarantine) {
        allocations[i].p=p; allocations[i].live=true; return p;
    }
    abort();
}
static void *host_realloc(void *p, size_t n) {
    int i=allocation(p); if (i<0) abort();
    void *q=realloc(p,n); if (!q) abort(); allocations[i].p=q; return q;
}
static void BZ_Free(void *p) {
    if (!p) return;
    int i=allocation(p);
    if (i<0) { double_frees++; return; }
    allocations[i].live=false;
    if (io_depth && p==borrowed && !aborting) {
        borrowed_frees++; allocations[i].quarantine=true;
        return; /*Logical free observed; quarantine avoids deliberately reading freed memory.*/
    }
    free(p); allocations[i].p=NULL; drops++;
}
static bool host_is_main(void) {
    return !schedule || (main_ready && pthread_equal(pthread_self(),main_tid));
}
static double host_clock(void) {
    pthread_mutex_lock(&state);
    if (schedule && main_ready && worker_busy && in_jrn_buf && pthread_equal(pthread_self(),main_tid)) {
        unsafe=1; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&state);
        longjmp(main_abort,1); /*Baseline stops before competing journal mutation.*/
    }
    pthread_mutex_unlock(&state);
    return 2.0;
}
static void clean_quarantine(void) {
    for (int i=0;i<64;i++) if (allocations[i].quarantine) {
        free(allocations[i].p); allocations[i].p=NULL; allocations[i].quarantine=false;
    }
}
static void COM_AssertMainThread(const char *s) { (void)s; if (!host_is_main()) abort(); }
#define localtext(s) (s)
#define Con_TPrintf Con_Printf
static void SCR_EndLoadingPlaque(void) {}
static void CL_Disconnect(const char *s) { (void)s; IN_Journal_Drop(); }
static void CL_ConnectAbort(void *p) { (void)p; }
static void SV_UnspawnServer(void) {}
static cvar_t cl_shownet, pr_ssqc_coreonerror;
static void Cvar_Set(cvar_t *v, const char *s) { v->value=(float)atof(s); }
static bool host_initialized=true, sys_nounload;
static struct { bool activeapp; } vid;
static struct { int state; } cls;
static bool isDedicated;
static struct { int state,allocated_client_slots,spawned_client_slots,spawned_observer_slots; } sv;
static struct { struct { bool spawned; } clients[1]; } svs;
static struct { void (*save_ents)(void *,char *,size_t *,size_t,int); } *svprogfuncs;
static const char *va(const char *fmt,...) {
    static char s[1024]; va_list args; va_start(args,fmt); vsnprintf(s,sizeof(s),fmt,args); va_end(args); return s;
}
static void SV_EndRedirect(void) {}
static void SV_FinalMessage(const char *s) { (void)s; }
static void SV_Shutdown(void) {}
static void Host_Shutdown(void) { IN_Journal_Drop(); }
static void showMessageAndQuit(const char *s) { (void)s; }
static void COM_WorkerAbort(const char *s) { (void)s; }
'''
CALLBACKS = r'''
static void abort_now(int kind) {
    aborting=true; abort_kind=kind;
    if (kind==1) Host_EndGame("synthetic close abort");
    if (kind==2) { cls.state=0; isDedicated=false; SV_Error("synthetic server abort"); }
    if (kind==3) Sys_Quit();
    if (kind==4) Sys_Error("synthetic android abort");
    abort();
}
static bool COM_WriteFile(const char *name,int root,const void *p,size_t n) {
    qbyte d[32]; size_t len; bool kept;
    int mode=callback_mode; callback_mode=0;
    (void)name; (void)root;
    writes++;
    check(allowindepphys==!expect_outer,"write restores caller permission before callbacks");
    if (!expect_outer) {
        int e=pthread_mutex_trylock(&physics);
        check(e==0,"no newly held worker mutex during file adapter");
        if (!e) pthread_mutex_unlock(&physics);
    }
    check(!IN_Journal_LastDigest(d,&len,&kept),"pending close digest is unavailable");
    free(written); written=copy_bytes(p,n); written_len=n;
    const void *outer_borrow=borrowed; borrowed=p; io_depth++;
    if (mode) callbacks++;
    if (mode==1) IN_Journal_Drop();
    if (mode==2 || mode==3 || mode==4) {
        IN_JournalBegin_f();
        if (mode==3 || mode==4) {
            const char *old=arg_name; int argc_old=argc_value;
            arg_name="data/nested.hid"; argc_value=(mode==3)?1:2;
            IN_JournalEnd_f();
            nested_ok=IN_Journal_LastDigest(nested_digest,&nested_len,&nested_kept);
            arg_name=old; argc_value=argc_old;
        }
    }
    if (mode==5) { int argc_old=argc_value; argc_value=1; IN_JournalEnd_f(); argc_value=argc_old; }
    if (mode>=10 && mode<=13) abort_now(mode-9);
    if (mode==20) {
        IN_JournalBegin_f(); callback_mode=10;
        arg_name="data/nested-abort.hid"; argc_value=2;
        IN_JournalEnd_f();
        abort();
    }
    io_depth--; borrowed=outer_borrow;
    return write_ok;
}
'''
TEST = r'''
static void fresh(void) {
    schedule=0; aborting=false; io_depth=callbacks=borrowed_frees=double_frees=0;
    borrowed=NULL; callback_mode=0; expect_outer=nested_ok=false;
    runningindepphys=allowindepphys=true;
    argc_value=2; arg_name="data/outer.hid"; write_ok=true;
    IN_JournalBegin_f();
}
static void callback_case(int mode,bool ok,bool outer) {
    qbyte d[32], expected[32]; size_t len=0; bool kept=false;
    bool saved=true;
    fresh(); callback_mode=mode; write_ok=ok; expect_outer=outer;
    if (outer) saved=CL_AllowIndependantSendCmd(false);
    IN_JournalEnd_f();
    check(!borrowed_frees && !double_frees,"callbacks cannot free the borrowed closed allocation");
    check(callbacks==(mode!=0),"selected returning callback acted");
    if (mode==0) {
        check(IN_Journal_LastDigest(d,&len,&kept) && len==written_len && kept==ok,"completed bytes/result published");
        CalcHash(&hash_sha2_256,expected,sizeof(expected),(qbyte*)written,written_len);
        check(!memcmp(d,expected,32),"actual digest matches exact submitted bytes");
    } else if (mode==2) {
        check(in_jrn_buf && in_jrn_len && in_jrn_hctx,"new Begin journal survives old completion");
        check(!IN_Journal_LastDigest(d,&len,&kept),"old failure cannot digest or publish new open journal");
    } else if (mode==3 || mode==4) {
        check(IN_Journal_LastDigest(d,&len,&kept) && nested_ok && len==nested_len && kept==nested_kept
              && !memcmp(d,nested_digest,32) && kept==(mode==4 && ok),"nested completed digest survives older completion");
    } else check(!IN_Journal_LastDigest(d,&len,&kept),"drop/no-open revokes pending publication");
    check(allowindepphys==!outer,"End preserves previous permission");
    if (outer) CL_AllowIndependantSendCmd(saved);
    IN_Journal_Drop(); clean_quarantine();
    check(live_count()==0,"all normal close allocations released");
}
static void abort_case(int mode) {
    qbyte d[32]; size_t len; bool kept;
    fresh(); callback_mode=mode;
    if (!setjmp(host_abort)) { IN_JournalEnd_f(); check(false,"abort returned normally"); }
    check(abort_kind==(mode==20?1:mode-9),"actual selected host jump acted");
    check(live_count()==0 && !double_frees,"all detached allocations released before actual longjmp");
    check(!IN_Journal_LastDigest(d,&len,&kept),"aborted pending close unavailable");
    check(allowindepphys,"End restored its worker permission before nonlocal callback");
    /*The test adapter stack was abandoned as intended; reset only host bookkeeping.*/
    io_depth=0; borrowed=NULL; aborting=false; clean_quarantine();
}
static void wait_flag(int *flag) {
    struct timespec until; clock_gettime(CLOCK_REALTIME,&until); until.tv_sec+=5;
    while (!*flag) if(pthread_cond_timedwait(&changed,&state,&until)==ETIMEDOUT) abort();
}
static void *worker(void *p) {
    (void)p; pthread_mutex_lock(&state); worker_tid=pthread_self(); pthread_mutex_unlock(&state);
    Sys_LockMutex(indeplock);
    pthread_mutex_lock(&state); worker_busy=1; pthread_mutex_unlock(&state);
    IN_Journal_Raw("# worker-row 01234567890123456789012345678901234567890123456789\n");
    pthread_mutex_lock(&state); worker_busy=0; pthread_mutex_unlock(&state);
    Sys_UnlockMutex(indeplock); return NULL;
}
static void *closer(void *p) {
    (void)p;
    pthread_mutex_lock(&state); main_tid=pthread_self(); main_ready=1; pthread_mutex_unlock(&state);
    if (!setjmp(main_abort)) IN_JournalEnd_f();
    pthread_mutex_lock(&state); done=1; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&state);
    return NULL;
}
static void admission_case(void) {
    pthread_t w,m; struct timespec until;
    fresh();
    /*Fill via actual Raw before worker admission, leaving a grow boundary.*/
    size_t n=65520-in_jrn_len; char *padding=malloc(n+1); memset(padding,'x',n); padding[n]=0;
    IN_Journal_Raw(padding); free(padding);
    schedule=1; paused=release_worker=worker_busy=main_ready=attempted=unsafe=done=0;
    if (pthread_create(&w,NULL,worker,NULL)) abort();
    pthread_mutex_lock(&state); wait_flag(&paused); pthread_mutex_unlock(&state);
    if (pthread_create(&m,NULL,closer,NULL)) abort();
    pthread_mutex_lock(&state); clock_gettime(CLOCK_REALTIME,&until); until.tv_sec+=5;
    while (!attempted && !unsafe && !done)
        if(pthread_cond_timedwait(&changed,&state,&until)==ETIMEDOUT) abort();
    check(!unsafe && attempted && !done,"End waits before reading/finalizing admitted worker state");
    release_worker=1; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&state);
    if (pthread_join(w,NULL) || pthread_join(m,NULL)) abort();
    schedule=0;
    if (!unsafe) check(written && strstr(written,"# worker-row ") && strstr(written,"\nend "),"released worker row and terminal both submitted");
    check(done && allowindepphys,"worker and close finish with permission restored");
    IN_Journal_Drop(); check(live_count()==0,"admission-case storage released");
}
int main(void) {
    qbyte d[32];
    static const qbyte abc[32]={0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    CalcHash(&hash_sha2_256,d,sizeof(d),(qbyte*)"abc",3); check(!memcmp(d,abc,32),"actual SHA256 known vector");
    admission_case();
    callback_case(0,true,false); callback_case(0,false,false);
    callback_case(0,true,true);
    for (int mode=1;mode<=5;mode++) callback_case(mode,false,false);
    callback_case(2,true,false); callback_case(4,true,false);
    for (int mode=10;mode<=13;mode++) abort_case(mode);
    abort_case(20);
    (void)freed_snapshot; (void)operation;
    free(snapshot); free(written);
    printf("journal close ownership: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
'''


def blob(root, ref, name):
    if ref:
        return subprocess.check_output(['git', '-C', str(root), 'show', ref + ':' + name], text=True, encoding='utf-8')
    return (root / name).read_text()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--work-root', required=True, type=Path)
    ap.add_argument('--cc', default='gcc')
    ap.add_argument('--source-ref', help='Inspected immutable commit for acting prechange control')
    a = ap.parse_args()
    a.work_root.mkdir(parents=True, exist_ok=False)
    (a.work_root / 'OWNER.md').write_text('Owned actual-C synthetic close/callback/abort/mutex fixture. No game/input actors or player data.\n')
    root = Path(__file__).resolve().parents[1]
    ref = None
    if a.source_ref:
        ref = subprocess.check_output(['git', '-C', str(root), 'rev-parse', '--verify', a.source_ref + '^{commit}'], text=True).strip()
    text = blob(root, ref, 'engine/client/in_generic.c')
    ci = blob(root, ref, 'engine/client/cl_input.c')
    cm = blob(root, ref, 'engine/client/cl_main.c')
    sv = blob(root, ref, 'engine/server/sv_main.c')
    droid = blob(root, ref, 'engine/client/sys_droid.c')
    sha1 = blob(root, ref, 'engine/common/sha1.c')
    sha2 = blob(root, ref, 'engine/common/sha2.c')
    (a.work_root / 'quakedef.h').write_text(CRYPTO_TYPES)
    (a.work_root / 'sha2.c').write_text(sha2)
    calc = sha1[sha1.index('size_t CalcHash('):sha1.index('/* hmac-sha1.c')]
    h = HEADER[:HEADER.index('static void IN_Journal_Digest(')]
    h = h[:h.index('static void hash_process(')] + h[h.index('static int Cmd_Argc('):]
    for old,new in [('in_jrn_hashed, in_jrn_digestlen','in_jrn_hashed'),('in_jrn_rehash, in_jrn_digestok','in_jrn_rehash'),('in_jrn_base, in_jrn_digestms','in_jrn_base'),('digests, writes','writes'),('snapshot_len, written_len','written_len'),('last_kept, write_ok','write_ok')]:
        assert old in h
        h = h.replace(old,new)
    if 'in_jrn_mainactive' in text:
        h += '\nstatic qboolean in_jrn_mainactive;\n'
    if 'in_jrn_mainsynth' in text:
        h += '\nstatic qboolean in_jrn_mainsynth;\n'
    h = h.replace('return 2.0;', 'return host_clock();').replace('return true; }\nstatic void *BZ_Realloc', 'return host_is_main(); }\nstatic void *BZ_Realloc')
    h = h.replace('void *q = realloc(p, n + 1);', 'thread_before_realloc();\n    void *q = host_realloc(p, n + 1);')
    pre = PRE.replace('static void hash_init(void *p);\n','')
    host = HOST[:HOST.index('static void BZ_Free(')] + 'static void BZ_Free(void *);\nstatic void *BZ_Malloc(size_t);\n' + HOST[HOST.index('typedef struct { float value;'):]
    helper = ci[ci.index('qboolean CL_AllowIndependantSendCmd('):ci.index('int CL_IndepPhysicsThread(')]
    raw = text[text.index('static void IN_Journal_Raw('):text.index('void IN_Journal_Note(')]
    digest_start = re.search(r'^static qbyte\s+in_jrn_digest\[32\]',text,re.M).start()
    digest = text[digest_start:text.index('void IN_Journal_Drop(void)')]
    drop = text[text.index('void IN_Journal_Drop(void)'):text.index('static void IN_Journal_Event(')]
    start = text.index('static void IN_JournalBegin_f(void)\n{')
    lifecycle = text[start:text.index('static void IN_JournalNote_f(',start)]
    endgame = cm[cm.index('NORETURN void VARGS Host_EndGame ('):cm.index('void VARGS Host_Error (')]
    servererror = sv[sv.index('void VARGS SV_Error ('):sv.index('\n#ifdef SERVERONLY',sv.index('void VARGS SV_Error ('))]
    android = droid[droid.index('void Sys_Quit(void)'):droid.index('void Sys_Printf (')]
    c = a.work_root / 'journal-close-ownership.c'
    exe = a.work_root / 'journal-close-ownership.exe'
    c.write_text(EXTRA_PRE + pre + calc + h + host + EXTRA_HOST + helper + raw + digest + drop + lifecycle + endgame + servererror + android + CALLBACKS + TEST)
    env = dict(os.environ)
    if Path(a.cc).is_absolute():
        env['PATH'] = str(Path(a.cc).parent) + os.pathsep + env.get('PATH','')
    crypto = a.work_root / 'sha2.o'
    # Frozen, untouched crypto has existing -Wextra signedness diagnostics. Keep
    # them visible, separate from strict journal/host warnings; do not edit it.
    subprocess.run([a.cc,'-std=c99','-Wall','-Wextra','-DSHA2=256','-c',str(a.work_root/'sha2.c'),'-o',str(crypto)],check=True,env=env)
    subprocess.run([a.cc,'-std=c99','-Wall','-Wextra','-Werror','-pthread',str(c),str(crypto),'-o',str(exe)],check=True,env=env)
    result = subprocess.run([str(exe)],text=True,capture_output=True,env=env,timeout=30)
    (a.work_root / 'result.txt').write_text(result.stdout + result.stderr)
    print(result.stdout,end='')
    if result.returncode:
        print(result.stderr,end='')
    raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()
