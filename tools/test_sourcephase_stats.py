#!/usr/bin/env python3
"""Compile actual replacement-stat scheduling in a bounded wire/bookkeeping fixture.

This proves scheduling availability/limits, NOT a live save or historical cause.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def block(text, start):
    first = text.index('{', start)
    depth = 1
    end = first + 1
    while depth:
        if text[end] == '{':
            depth += 1
        elif text[end] == '}':
            depth -= 1
        end += 1
    return text[start:end]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1] / 'engine/server/sv_send.c')
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--cc', default='gcc')
    ap.add_argument('--expect-failures', type=int, default=0)
    a = ap.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    raw = a.source.read_bytes()
    text = raw.decode('utf8').replace('\r\n', '\n')
    function = text.index('void SV_UpdateClientStats (')
    branch = block(text, text.index('if (client->fteprotocolextensions2 & PEXT2_REPLACEMENTDELTAS)', function))
    helper_anchor = 'static qboolean SV_WritePendingStringStat('
    helper = block(text, text.index(helper_anchor)) if helper_anchor in text else ''
    header = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX_CL_STATS 256
#define STAT_FS_SOURCEPHASE 179
#define PHYSMODE_SOURCE 1
#define PEXT2_REPLACEMENTDELTAS 1
#define true 1
#define false 0
#define ISNQCLIENT(c) 0
typedef int qboolean;
typedef struct {int ival;} cvar_t;
static cvar_t dpcompat_stats={0},pm_physicsmode={1};
static struct {int info;} svs;
static int advertise=1;
static const char *InfoBuf_ValueForKey(const void *p,const char *key) {(void)p;(void)key;return advertise?"1":"";}
#define Z_Free free
#define Z_StrDup strdup
enum { svcfte_choosesplitclient=0xe0,svcfte_updatestatstring,svcfte_updatestatfloat,svcdp_updatestatbyte,svcqw_updatestatbyte,svcnq_updatestatlong,svcqw_updatestatlong };
typedef struct {unsigned char data[8192];size_t cursize,maxsize;} sizebuf_t;
typedef struct {unsigned numresendstats;unsigned short resendstats[32];} client_frame_t;
typedef struct {unsigned fteprotocolextensions2;int statsi[256];float statsf[256];const char *statss[256];unsigned pendingstats[16];} client_t;
static void MSG_WriteByte(sizebuf_t *b,int x) {assert(b->cursize<b->maxsize);b->data[b->cursize++]=(unsigned char)x;}
static void MSG_WriteLong(sizebuf_t *b,int x) {for(int i=0;i<4;i++)MSG_WriteByte(b,(unsigned)x>>(i*8));}
static void MSG_WriteFloat(sizebuf_t *b,float x) {union{float f;int i;}v;v.f=x;MSG_WriteLong(b,v.i);}
static void MSG_WriteString(sizebuf_t *b,const char *s) {do {MSG_WriteByte(b,*s);}while(*s++);}
'''
    wrapper = r'''
static void schedule(client_t *client,int pnum,sizebuf_t *msg,client_frame_t *frame,unsigned m,int *statsi,float *statsf,const char **statss) {
 unsigned i;
'''
    fixture = r'''
}
typedef struct {int phase_count,phase_seat,num_count,float_count,long_count,long_value;float float_value;char phase[128];} decoded_t;
static decoded_t decode(const sizebuf_t *b) {
 decoded_t d={0};size_t p=0;
 while(p<b->cursize) {
  int seat=0,op=b->data[p++];
  if(op==svcfte_choosesplitclient) {assert(p+2<=b->cursize);seat=b->data[p++];op=b->data[p++];}
  assert(p<b->cursize);int stat=b->data[p++];
  if(op==svcfte_updatestatstring) {
   size_t first=p;while(p<b->cursize&&b->data[p])p++;assert(p<b->cursize);
   if(stat==STAT_FS_SOURCEPHASE) {d.phase_count++;d.phase_seat=seat;snprintf(d.phase,sizeof(d.phase),"%s",(const char*)b->data+first);}p++;
  } else {
   d.num_count++;size_t bytes=(op==svcdp_updatestatbyte||op==svcqw_updatestatbyte)?1:4;
   assert(op>=svcfte_updatestatfloat&&op<=svcqw_updatestatlong);assert(p+bytes<=b->cursize);
   if(bytes==4) {unsigned bits=0;for(int i=0;i<4;i++)bits|=(unsigned)b->data[p+i]<<(i*8);
    if(op==svcfte_updatestatfloat) {union{unsigned u;float f;}v;v.u=bits;d.float_count++;d.float_value=v.f;}
    else {d.long_count++;d.long_value=(int)bits;}
   }p+=bytes;
  }
 }
 assert(p==b->cursize);return d;
}
static int checks=0,failures=0;
static void check(int good,const char *name) {checks++;if(!good)failures++;printf("%s %s\n",good?"PASS":"FAIL",name);}
static void one(const char *name,int numerics,int phase,int slots,size_t max,int pnum,int mode,int feature,int expect_phase) {
 client_t c={0};client_frame_t f={0};sizebuf_t b={0};int ints[256]={0};float floats[256]={0};const char *strings[256]={0};
 c.fteprotocolextensions2=PEXT2_REPLACEMENTDELTAS;f.numresendstats=slots;b.maxsize=max;
 pm_physicsmode.ival=mode;advertise=feature;for(int i=0;i<numerics;i++)ints[i]=100+i;
 assert(!strcmp(InfoBuf_ValueForKey(&svs.info,"fs_sourcephase"),feature?"1":""));
 int mixed=!strcmp(name,"numeric float/long paths with no phase");
 if(mixed) {floats[7]=1.25f;ints[8]=999;}
 if(phase)strings[STAT_FS_SOURCEPHASE]="1:100:0:0.014";
 schedule(&c,pnum,&b,&f,256,ints,floats,strings);decoded_t d=decode(&b);
 check(d.phase_count==expect_phase,name);
 if(mixed)check(d.num_count==2&&d.float_count==1&&d.long_count==1&&d.float_value==1.25f&&d.long_value==999,"float/long wire values and split seat survive");
 check(f.numresendstats<=32&&b.cursize<=b.maxsize,"shared byte/slot limits");
 check(d.num_count+d.phase_count==(int)f.numresendstats-slots,"one resend entry per decoded message");
 if(d.phase_count) {
  int encoded=(STAT_FS_SOURCEPHASE+MAX_CL_STATS)|(pnum<<12),found=0;
  for(unsigned i=slots;i<f.numresendstats;i++)found+=f.resendstats[i]==encoded;
  check(d.phase_count==1&&d.phase_seat==pnum&&!strcmp(d.phase,"1:100:0:0.014")&&found==1,"phase string/pnum/resend identity decoded once");
  check(!(c.pendingstats[(STAT_FS_SOURCEPHASE+MAX_CL_STATS)>>5]&(1u<<(STAT_FS_SOURCEPHASE&31))),"sent phase clears pending only after write");
 } else if(phase)check((c.pendingstats[(STAT_FS_SOURCEPHASE+MAX_CL_STATS)>>5]&(1u<<(STAT_FS_SOURCEPHASE&31)))!=0,"unsent phase remains pending");
 for(int i=0;i<256;i++)free((void*)c.statss[i]);
}
static void resend_newer(void) {
 client_t c={0};client_frame_t f={0};sizebuf_t b={0};int ints[256]={0};float floats[256]={0};const char *strings[256]={0};
 c.fteprotocolextensions2=1;pm_physicsmode.ival=1;advertise=1;b.maxsize=sizeof(b.data);strings[179]="1:100:0:0.014";
 schedule(&c,1,&b,&f,256,ints,floats,strings);decoded_t first=decode(&b);
 // Model the normal dropped-frame requeue; source scheduling still emits current tagged value.
 c.pendingstats[(179+MAX_CL_STATS)>>5]|=1u<<(179&31);f.numresendstats=0;b.cursize=0;strings[179]="1:101:0:0.012";
 schedule(&c,1,&b,&f,256,ints,floats,strings);decoded_t second=decode(&b);
 check(first.phase_count==1&&second.phase_count==1&&second.phase_seat==1&&!strcmp(second.phase,"1:101:0:0.012"),"requeued phase updates to newer tag, never fabricates history");
 free((void*)c.statss[179]);
}
int main(void) {
 one("pressure32 sends phase before numeric crowding",32,1,0,8192,0,1,1,1);
 one("one remaining slot reserved opportunistically",32,1,31,8192,1,1,1,1);
 one("quiet Source with phase",1,1,0,8192,0,1,1,1);
 one("no phase ordinary numeric control",32,0,0,8192,0,1,1,0);
 one("non Source retains ordinary ordering",32,1,0,8192,0,0,1,0);
 one("unadvertised retains ordinary ordering",32,1,0,8192,0,1,0,0);
 one("full resend frame cannot invent room",32,1,32,8192,1,1,1,0);
 one("insufficient phase bytes keep pending",0,1,0,4+strlen("1:100:0:0.014"),1,1,1,0);
 one("exact split-seat phase boundary fits",0,1,0,5+strlen("1:100:0:0.014"),1,1,1,1);
 one("numeric float/long paths with no phase",0,0,0,8192,1,1,1,0);
 resend_newer();
 printf("SUMMARY checks=%d failures=%d\n",checks,failures);return failures;
}
'''
    code = header + helper + wrapper + branch + fixture
    c = a.output / 'stats.c'
    c.write_text(code, encoding='utf8', newline='\n')
    compiler = shutil.which(a.cc)
    if compiler is None:
        raise FileNotFoundError(a.cc)
    compiler = str(Path(compiler).resolve())
    env = os.environ.copy()
    env['PATH'] = str(Path(compiler).parent) + os.pathsep + env.get('PATH', '')
    exe = a.output / ('stats.exe' if os.name == 'nt' else 'stats')
    build = subprocess.run([compiler, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-o', str(exe), str(c)], env=env, capture_output=True, text=True)
    (a.output / 'build.stdout').write_text(build.stdout, encoding='utf8')
    (a.output / 'build.stderr').write_text(build.stderr, encoding='utf8')
    (a.output / 'build.exit').write_text(str(build.returncode)+'\n')
    if build.returncode:
        raise RuntimeError('fixture compile failed; retained build diagnostics')
    run = subprocess.run([str(exe.resolve())], env=env, capture_output=True, text=True)
    (a.output / 'run.stdout').write_text(run.stdout, encoding='utf8')
    (a.output / 'run.stderr').write_text(run.stderr, encoding='utf8')
    receipt = dict(source_sha256=hashlib.sha256(raw).hexdigest(), helper_sha256=hashlib.sha256(helper.encode()).hexdigest(),
                   scheduling_branch_sha256=hashlib.sha256(branch.encode()).hexdigest(), compiler=compiler,
                   compile_exit=build.returncode, run_exit=run.returncode, expected_failures=a.expect_failures,
                   availability_only_not_live_save_or_historical_repair=True)
    (a.output / 'receipt.json').write_text(json.dumps(receipt, indent=2)+'\n', encoding='utf8')
    print(run.stdout, end='')
    return 0 if run.returncode == a.expect_failures and 'SUMMARY checks=' in run.stdout else 1


if __name__ == '__main__':
    raise SystemExit(main())
