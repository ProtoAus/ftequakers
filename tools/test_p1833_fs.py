#!/usr/bin/env python3
"""Isolated native production-body FS tests + deterministic 184k old-size control.
Run from any cwd; generated C/binaries/logs stay under --output (owned tree).
No engine build, native engine runtime, external assets or dependencies.
"""
import argparse
import hashlib
import re
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]

PRELUDE = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "hash.h"
typedef int qboolean;
typedef unsigned char qbyte;
#define true 1
#define false 0
#define QDECL
#define fte_alignof(t) _Alignof(t)
#define Q_strcasecmp strcasecmp
#define Q_strncasecmp strncasecmp
#define COM_WorkerLock() ((void)0)
#define COM_WorkerUnlock() ((void)0)
#define SPF_ADDON 1
#define SPF_ISDIR 2
#define COM_AssertMainThread(s) ((void)0)
#define Con_DPrintf(...) ((void)0)
#define Con_Printf printf
static void *Z_Malloc(size_t n) { void *p=calloc(1,n); assert(p); return p; }
#define Z_Free free
static double Sys_DoubleTime(void) { struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC,&t)); return t.tv_sec+t.tv_nsec*1e-9; }
static int lock_ok=1;
static int Sys_LockMutex(void *m) { (void)m; return lock_ok; }
static void Sys_UnlockMutex(void *m) { (void)m; }
static void FS_ReloadPackFilesFlags(unsigned int flags) { (void)flags; }
static void *fs_thread_mutex;
static qboolean com_fschanged=true, com_fsneedreload;
static int fs_puremode;
static unsigned int fs_listgen;
static hashtable_t filesystemhash;
typedef struct { void *fhandle; } flocation_t;
typedef struct searchpathfuncs_s searchpathfuncs_t;
struct searchpathfuncs_s {
 void (*BuildHash)(searchpathfuncs_t *, int, void (*)(int,const char *,fsbucket_t *,void *));
 qboolean (*PollChanges)(searchpathfuncs_t *);
 int count;
 const char *name;
 fsbucket_t supplied;
 int present;
 int (*FindFile)(searchpathfuncs_t *,flocation_t *,const char *,void *);
};
typedef struct searchpath_s {
 struct searchpath_s *next, *nextpure;
 searchpathfuncs_t *handle;
 const char *logicalpath;
 unsigned int flags;
} searchpath_t;
static searchpath_t *com_searchpaths, *com_purepaths;
typedef struct { int ival; } cvar_t;
static cvar_t com_fs_cache={1};
cvar_t fs_loadstats={1};
static int fs_finds;
'''
MAIN = r'''
static void build(searchpathfuncs_t *h, int d, void (*add)(int,const char*,fsbucket_t*,void*))
{
 int i; char name[100];
 if(h->name) { add(d,h->name,&h->supplied,h); return; }
 for(i=0;i<h->count;i++) {
  snprintf(name,sizeof(name),"materials/synthetic/%03d/asset_%06d.vtf", i%997,i);
  add(d,name,NULL,h);
 }
}
static qboolean poll(searchpathfuncs_t *h) { (void)h; return true; }
static void reset(void) {
 FS_FlushFSHashReally(false);
 free(filesystemhash.bucket); memset(&filesystemhash,0,sizeof(filesystemhash));
 com_searchpaths=com_purepaths=NULL; fs_puremode=0;
}
static void semantics(void) {
 searchpathfuncs_t low={.BuildHash=build,.PollChanges=poll,.name="Materials/Dup.VTF"};
 searchpathfuncs_t high={.BuildHash=build,.PollChanges=poll,.name="materials/dup.vtf"};
 searchpath_t a={NULL,NULL,&low,"cstrike_placeholder.vpk",SPF_ADDON};
 searchpath_t b={NULL,NULL,&high,"map.pak",0};
 com_searchpaths=&a; a.next=&b;
 COM_FlushFSCache(false,true);
 assert(Hash_GetInsensitive(&filesystemhash,"MATERIALS/DUP.VTF")==&high);
 assert(fs_hash_dups==1);
 //Same depth and deeper insertions must not replace the current winner.
 FS_AddFileHash(2,"materials/dup.vtf",NULL,&low);
 FS_AddFileHashUnsafe(3,"materials/dup.vtf",NULL,&low);
 assert(Hash_GetInsensitive(&filesystemhash,"MATERIALS/DUP.VTF")==&high);
 FS_AddFileHashUnsafe(0,"materials/dup.vtf",NULL,&low);
 assert(Hash_GetInsensitive(&filesystemhash,"MATERIALS/DUP.VTF")==&low);
 //Pure-path priority wins over ordinary mounts; pure-only skips normal paths.
 com_purepaths=&b; b.nextpure=NULL; com_fschanged=true;
 FS_RebuildFSHash(true);
 assert(Hash_GetInsensitive(&filesystemhash,"materials/dup.vtf")==&high);
 fs_puremode=2; com_fschanged=true; FS_RebuildFSHash(true);
 assert(Hash_GetInsensitive(&filesystemhash,"materials/dup.vtf")==&high);
 //Stale callbacks must not read freed package buckets, or insert a new key.
 com_fschanged=true;
 FS_AddFileHash(0,"not/visible",NULL,&low);
 FS_AddFileHashUnsafe(0,"not/visible",NULL,&low);
 assert(!Hash_GetInsensitive(&filesystemhash,"not/visible"));
 com_purepaths=NULL; com_searchpaths=&a; a.next=NULL; fs_puremode=0;
 FS_RebuildFSHash(true);
 assert(Hash_GetInsensitive(&filesystemhash,"materials/dup.vtf")==&low);
 //Unchanged rehash does no work; failed lock leaves invalidation intact.
 { unsigned int n=fs_hash_rebuilds; FS_RebuildFSHash(true); assert(n==fs_hash_rebuilds);
 com_fschanged=true; lock_ok=0; FS_RebuildFSHash(true); assert(com_fschanged && n==fs_hash_rebuilds); lock_ok=1; }
 reset(); puts("semantics: mixed-case, supplied/allocated buckets, duplicate depth, stand-in, pure, stale guard, retirement, lock failure PASS");
}
static int find_file(searchpathfuncs_t *h, flocation_t *loc, const char *name, void *hashed)
{
 (void)hashed;
 if (!h->present || strcasecmp(name,h->name)) return 0;
 loc->fhandle=h;
 return 1;
}
static void assert_single_winner(const char *name, void *winner)
{
 unsigned int i; int entries=0;
 assert(Hash_GetInsensitive(&filesystemhash,name)==winner);
 for (i=0;i<filesystemhash.numbuckets;i++) {
  bucket_t *b;
  for(b=filesystemhash.bucket[i];b;b=b->next) entries++;
 }
 assert(entries==(winner?1:0));
}
static void incremental_mutation(void)
{
 //This spelling's sensitive/insensitive keys coincide at 1024, not 262144.
 const char *name="AAAAAAAaA";
 searchpathfuncs_t high={.BuildHash=build,.name="AAAAAAAaA",.present=1,.FindFile=find_file};
 searchpathfuncs_t low={.BuildHash=build,.name="AAAAAAAaA",.present=1,.FindFile=find_file};
 searchpath_t lower={NULL,NULL,&low,"low",SPF_ISDIR};
 searchpath_t higher={&lower,NULL,&high,"high",SPF_ISDIR};
 com_searchpaths=&higher; FS_RebuildFSHash(true);
 assert_single_winner(name,&high);
 //Simulate successful FS_Remove from the high mount and its real callback.
 high.present=0; FS_FlushFSHashRemoved(name);
 assert_single_winner(name,&low);
 high.present=1; FS_FlushFSHashWritten(name);
 assert_single_winner("aaaaaaaAA",&high);
 high.present=0; FS_FlushFSHashRemoved(name);
 assert_single_winner(name,&low);
 low.present=0; FS_FlushFSHashRemoved(name);
 assert_single_winner(name,NULL);
 low.present=1; FS_FlushFSHashWritten(name);
 assert_single_winner(name,&low);
 high.present=1; FS_FlushFSHashWritten(name);
 assert_single_winner(name,&high);
 //Act the other removal site directly: replacing a deeper allocated bucket
 //with a supplied shallower bucket must unlink, not leave two casefold matches.
 FS_FlushFSHashReally(false); com_fschanged=false;
 FS_AddFileHash(9,name,NULL,&low);
 FS_AddFileHash(1,name,&high.supplied,&high);
 assert_single_winner("aaaaaaaAA",&high);
 //The exact mixed-case callback must preserve the lower mount winner.
 high.present=0; FS_FlushFSHashRemoved(name);
 assert_single_winner(name,&low);
 reset(); puts("incremental mutation: mixed-case duplicate deletion / reinsertion / shallower replacement PASS");
}
int main(void) {
 int i; uint64_t visits=0; char name[100]; double t,insert,lookup;
 searchpathfuncs_t assets={.BuildHash=build,.PollChanges=poll,.count=184000};
 searchpathfuncs_t duplicates={.BuildHash=build,.PollChanges=poll,.count=184000};
 searchpath_t lower={NULL,NULL,&duplicates,"duplicate pack",0};
 searchpath_t path={&lower,NULL,&assets,"synthetic",SPF_ISDIR};
 semantics(); incremental_mutation(); FS_LoadStats_Begin(); com_searchpaths=&path; t=Sys_DoubleTime(); FS_RebuildFSHash(true); insert=Sys_DoubleTime()-t;
 assert(filesystemhash.numbuckets==FS_HASH_BUCKETS && fs_hash_files==184000 && fs_hash_dups==184000);
 t=Sys_DoubleTime();
 for(i=0;i<assets.count;i++) {
  snprintf(name,sizeof(name),"MATERIALS/SYNTHETIC/%03d/ASSET_%06d.VTF", i%997,i);
  assert(Hash_GetInsensitive(&filesystemhash,name)==&assets);
 }
 lookup=Sys_DoubleTime()-t;
 //Deterministic total successful-lookup chain visits, independent of wall timing.
 for(i=0;i<(int)filesystemhash.numbuckets;i++) {
  uint64_t n=0; bucket_t *b;
  for(b=filesystemhash.bucket[i];b;b=b->next) n++;
  visits+=n*(n+1)/2;
 }
 printf("benchmark: files=%d duplicate_entries=184000 buckets=%u bucket_bytes=%zu insert_ms=%.3f mixed_lookup_ms=%.3f chain_visits=%llu\n",assets.count,filesystemhash.numbuckets,Hash_BytesForBuckets(filesystemhash.numbuckets),insert*1000,lookup*1000,(unsigned long long)visits);
 FS_LoadStats_Phase("synthetic index / lookup"); FS_LoadStats_End("synthetic");
 reset(); return 0;
}
'''

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'rig/lobbyfast')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--arm', choices=['subject', 'old-size-control'])
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    fs = (ROOT / 'engine/common/fs.c').read_text()
    header = (ROOT / 'engine/common/fs.h').read_text()
    bucket = header[header.index('typedef struct\n{\n\tbucket_t buck;'):header.index('extern hashtable_t filesystemhash;')]
    bodies = fs[fs.rindex('int fs_hash_dups;'):fs.index('void FS_FlushFSHashFull(void)')]
    flush = fs[fs.index('extern cvar_t fs_loadstats;'):fs.index('/*\nftesurf (P180): what a map load')]
    stats = fs[fs.index('static int\t\tfs_ls_finds;'):fs.index('/*since should start as 0, otherwise this can be used to poll*/')]
    for path in ('engine/common/fs.c', 'engine/common/fs.h', 'engine/qclib/hash.c', 'engine/qclib/hash.h'):
        print(path, hashlib.sha256((ROOT / path).read_bytes()).hexdigest(), flush=True)
    visits = {}
    for label, source in [('subject', bodies), ('old-size-control', bodies.replace('#define FS_HASH_BUCKETS 262144u', '#define FS_HASH_BUCKETS 1024u'))]:
        if args.arm and args.arm != label:
            continue
        cfile = args.output / f'{label}.c'
        binary = args.output / label
        #The control differs ONLY in bucket count; insertion and hash bodies remain production code.
        cfile.write_text(PRELUDE.replace('typedef struct searchpathfuncs_s', bucket+'\ntypedef struct searchpathfuncs_s', 1) + source + flush + stats + MAIN)
        cmd = ['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-unused-variable', '-Wno-sign-compare', '-I', str(ROOT / 'engine/qclib'), str(cfile), str(ROOT / 'engine/qclib/hash.c'), '-o', str(binary)]
        if args.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
        print('+', ' '.join(cmd), flush=True)
        subprocess.run(cmd, check=True)
        print('+', binary, flush=True)
        run = subprocess.run([str(binary.resolve())], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (args.output / f'{label}.log').write_text(run.stdout)
        print(run.stdout, end='', flush=True)
        run.check_returncode()
        assert 'semantics:' in run.stdout and 'PASS' in run.stdout
        assert re.search(r'fs: flush .*\(2 paths\).*\(1 rebuilds,', run.stdout)
        assert re.search(r'fs: phase synthetic index / lookup .*\(1 rebuilds\)', run.stdout)
        assert '184000 files indexed | hash valid' in run.stdout
        visits[label] = int(re.search(r'chain_visits=(\d+)', run.stdout)[1])
    if not args.arm:
        assert visits['subject'] < visits['old-size-control']
        print('control discrimination: larger table has fewer deterministic chain visits PASS', flush=True)

if __name__ == '__main__':
    main()
