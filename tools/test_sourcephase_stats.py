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


DELIVERY_HEADER = r"""
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX_CL_STATS 256
#define STAT_FS_SOURCEPHASE 179
#define MAX_SPLITS 4
#define UPDATE_MASK 3
#define UPDATE_BACKUP 4
#define PEXT_CSQC 1
#define PEXT2_REPLACEMENTDELTAS 1
#define PEXT2_VRINPUTS 2
#define PEXT_HEXEN2 1
#define PHYSMODE_SOURCE 1
#define GT_PROGS 1
#define GT_Q1QVM 2
#define SCP_NETQUAKE 1
#define SCP_DARKPLACES6 6
#define SCP_DARKPLACES7 7
#define MAX_QW_STATS 32
#define MAX_OVERALLMSGLEN 8192
#define MAX_NQDATAGRAM 1024
#define NA_LOOPBACK 1
#define SV_REPLACEMENT_ENTITY_ADMISSION @ADMISSION@
#define true 1
#define false 0
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
#define countof(a) (sizeof(a)/sizeof((a)[0]))
#define Z_Free free
#define Z_StrDup strdup
#define Q_memcpy memcpy
#define VectorCopy(a,b) memcpy(b,a,sizeof(b))
typedef unsigned char qbyte;
typedef int qboolean;
typedef struct {int ival;} cvar_t;
static cvar_t dpcompat_stats={0},pm_physicsmode={1};
static struct {int info,gametype;} svs={0,GT_PROGS};
static struct {struct {void *worldmodel;} world;} sv;
static struct {float gravity;} movevars={800};
static double realtime;
static void Con_Printf(const char *fmt,...) {(void)fmt;}
#define Sys_Printf Con_Printf
static void Sys_Error(const char *fmt,...) {(void)fmt;abort();}
static const char *InfoBuf_ValueForKey(const void *p,const char *s) {(void)p;(void)s;return "1";}
enum {svcfte_choosesplitclient=0xe0,svcfte_updatestatstring,svcfte_updatestatfloat,svcdp_updatestatbyte,svcqw_updatestatbyte,svcnq_updatestatlong,svcqw_updatestatlong,svc_entgravity,svcfte_setanglebase,svcfte_voicechat};
typedef struct {qbyte *data;int cursize,maxsize,allowoverflow,overflowed,prim;} sizebuf_t;
struct resendinfo_s {unsigned bits,flags;int entnum;};
typedef struct {int sequence,numresend;unsigned numresendstats;unsigned short resendstats[32];struct resendinfo_s *resend;int baseanglelocked[MAX_SPLITS];short baseangles[MAX_SPLITS][3];int packetsizeout,ping_time;double senttime;} client_frame_t;
typedef struct {int incoming_sequence,outgoing_sequence;struct {int type;} remote_address;} netchan_t;
typedef struct {struct {int goalentity;} *v;} edict_t;
typedef struct client_s {unsigned fteprotocolextensions,fteprotocolextensions2;int protocol,pextknown,spec_track;const char *name;edict_t *edict;struct client_s *controlled,*controller;struct {client_frame_t *frames;} frameunion;netchan_t netchan;sizebuf_t datagram;int statsi[256];float statsf[256];const char *statss[256];unsigned pendingstats[16],pendingdeltabits[64],pendingcsqcbits[64];int lastsequence_acknowledged,delta_sequence,baseanglelock,seat;short baseangles[3];int damage,calc_calls,data_calls;int desired;const char *phase;} client_t;
#define ISQWCLIENT(c) ((c)->protocol==0)
#define ISNQCLIENT(c) ((c)->protocol==1)
#define ISQ2CLIENT(c) 0
static int wire_size,force_overflow,entity_admitted;
static int body_limit;
static qbyte wire[8192];
static int Netchan_GetMaxUnreliable(netchan_t *n) {(void)n;return 1200;}
static int SV_RateForClient(client_t *c) {(void)c;return 0;}
static int Netchan_CanReliable(netchan_t *n,int r) {(void)n;(void)r;return 1;}
static unsigned Netchan_Transmit(netchan_t *n,int len,qbyte *p,int r) {(void)r;wire_size=len;memcpy(wire,p,len);n->outgoing_sequence=n->incoming_sequence;return len;}
static int SV_CanTrack(client_t *c,int e) {(void)c;(void)e;return 1;}
void SZ_Clear(sizebuf_t *);
void *SZ_GetSpace(sizebuf_t *,int);
void SZ_Write(sizebuf_t *,const void *,int);
"""

DELIVERY_PRODUCERS = r"""
static void MSG_WriteByte(sizebuf_t *b,int x) {*(qbyte*)SZ_GetSpace(b,1)=x;}
static void MSG_WriteShort(sizebuf_t *b,int x) {qbyte *p=SZ_GetSpace(b,2);p[0]=x;p[1]=x>>8;}
static void MSG_WriteLong(sizebuf_t *b,int x) {qbyte *p=SZ_GetSpace(b,4);for(int i=0;i<4;i++)p[i]=(unsigned)x>>(i*8);}
static void MSG_WriteFloat(sizebuf_t *b,float x) {union{float f;int i;}v;v.f=x;MSG_WriteLong(b,v.i);}
static void MSG_WriteString(sizebuf_t *b,const char *s) {SZ_Write(b,s,strlen(s)+1);}
static float SV_ClientCurGravity(client_t *c) {(void)c;return 800;}
static unsigned SV_CalcClientStats(client_t *c,int *ints,float *floats,const char **strings) {
 memset(ints,0,256*sizeof(*ints));memset(floats,0,256*sizeof(*floats));memset(strings,0,256*sizeof(*strings));
 c->calc_calls++;ints[0]=c->desired;strings[179]=c->phase;return 256;
}
static void SV_WriteClientdataToMessage(client_t *c,sizebuf_t *msg) {
 c->data_calls++;assert(c->calc_calls>0);if(c->controlled)assert(c->controlled->calc_calls>0);
 if(c->damage) {MSG_WriteByte(msg,0xa0);MSG_WriteByte(msg,1);MSG_WriteByte(msg,1);for(int i=0;i<3;i++)MSG_WriteShort(msg,1);c->damage=0;}
}
static void SV_WriteEntitiesToClient(client_t *client,sizebuf_t *msg,int ignore) {
 (void)ignore;body_limit=msg->maxsize;int i;client_frame_t *frame=&client->frameunion.frames[client->netchan.incoming_sequence&UPDATE_MASK];
 @VR@
 @PREDICATE@ {entity_admitted++;MSG_WriteByte(msg,0xb0);MSG_WriteFloat(msg,0);MSG_WriteShort(msg,0);}
 frame->sequence=client->netchan.incoming_sequence;
 if(force_overflow) {frame->resend[frame->numresend++]=(struct resendinfo_s){4,8,52};client->pendingdeltabits[52]=0;client->pendingcsqcbits[52]=0;qbyte bytes[1200]={0};SZ_Write(msg,bytes,1200);}
}
static void voice(sizebuf_t *buf,int len) {
 struct {int datalen,sender,gen,seq;qbyte data[100];} value={0},*ring=&value;ring->datalen=len;
 @VOICE@
}
"""

DELIVERY_TESTS = r"""
static int checks,failures;
static void check(int ok,const char *name) {checks++;failures+=!ok;printf("%s %s\n",ok?"PASS":"FAIL",name);}
static client_t c,seat;static client_frame_t frames[4];static struct resendinfo_s records[4][8];static qbyte dg[1200];static edict_t ed;
static void init(void) {
 for(int i=0;i<256;i++){free((void*)c.statss[i]);free((void*)seat.statss[i]);}
 memset(&c,0,sizeof(c));memset(&seat,0,sizeof(seat));memset(frames,0,sizeof(frames));memset(records,0,sizeof(records));
 for(int i=0;i<4;i++){frames[i].resend=records[i];frames[i].sequence=-1;}
 c.frameunion.frames=frames;c.fteprotocolextensions2=1;c.fteprotocolextensions=1;c.edict=&ed;c.name="fixture";c.netchan.incoming_sequence=1;c.lastsequence_acknowledged=0;
 c.datagram=(sizebuf_t){dg,0,1200,1,0,0};c.desired=7;c.phase="1:100:0:0.014";c.damage=1;sv.world.worldmodel=&c;
 force_overflow=0;entity_admitted=0;wire_size=0;
}
static int pending(client_t *cl,int stat) {return !!(cl->pendingstats[stat>>5]&(1u<<(stat&31)));}
int main(void) {
 init();c.datagram.cursize=1168;SV_SendClientDatagram(&c);SV_AckEntityFrame(&c,1);SV_AckEntityFrame(&c,2);SV_ReplaceEntityFrame(&c,6);
 printf("WITNESS optional pressure wire=%d numeric_pending=%d phase_pending=%d clientdata_calls=%d entity_admitted=%d\n",wire_size,pending(&c,0),pending(&c,435),c.data_calls,entity_admitted);
 check(wire_size==40 && wire[0]==svcfte_updatestatstring && wire[16]==svcqw_updatestatbyte && wire[19]==svc_entgravity && wire[24]==0xa0 && wire[33]==0xb0 && c.data_calls==1 && entity_admitted==1,"32-byte optional pressure reclaims essential capacity and serializes clientdata once");
 check(wire_size>0 || pending(&c,0),"empty discarded packet ACK cannot silently retire unchanged numeric stat");
 init();force_overflow=1;SV_SendClientDatagram(&c);SV_AckEntityFrame(&c,1);SV_AckEntityFrame(&c,2);
 check(wire_size==0 && pending(&c,0) && pending(&c,435),"local discard requeues current numeric and phase before ACK");
 check(c.pendingdeltabits[52]==4&&c.pendingcsqcbits[52]==8&&frames[1].numresend==0,"local discard restores serialized entity and CSQC records");
 force_overflow=0;c.netchan.incoming_sequence=3;SV_SendClientDatagram(&c);SV_AckEntityFrame(&c,3);
 check(wire_size>=19&&wire[0]==svcfte_updatestatstring&&wire[16]==svcqw_updatestatbyte&&!pending(&c,0)&&!pending(&c,435),"unchanged discarded values are actually written on next attempt then ACKed");
 init();c.controlled=&seat;seat.fteprotocolextensions2=1;seat.fteprotocolextensions=1;seat.edict=&ed;seat.seat=1;seat.desired=9;seat.phase="1:100:0:0.014";
 SV_SendClientDatagram(&c);SV_CSQC_DroppedPacket(&c,1);int count=frames[1].numresendstats;SV_CSQC_DroppedPacket(&c,1);
 check(pending(&c,0)&&pending(&seat,0)&&pending(&seat,435)&&count==0&&frames[1].numresendstats==0,"production drop restores both seats and consumes records idempotently");
 frames[2].sequence=2;frames[2].numresend=1;records[2][0]=(struct resendinfo_s){4,8,52};SV_CSQC_DroppedPacket(&c,2);SV_CSQC_DroppedPacket(&c,2);
 check(c.pendingdeltabits[52]==4&&c.pendingcsqcbits[52]==8&&frames[2].numresend==0,"entity 52 and CSQC flags restored idempotently");
 c.netchan.incoming_sequence=3;c.desired=12;c.phase="1:101:0:0.012";seat.phase="1:101:0:0.012";SV_SendClientDatagram(&c);SV_AckEntityFrame(&c,3);
 check(wire_size>0&&c.statsi[0]==12&&!pending(&c,0)&&!pending(&seat,435)&&strstr(c.statss[179],"101"),"requeue emits current newer values then actual ACK leaves them delivered");
 init();frames[1].sequence=0;frames[1].numresendstats=1;frames[1].resendstats[0]=5;frames[1].numresend=1;records[1][0]=(struct resendinfo_s){16,32,51};c.lastsequence_acknowledged=-1;SV_SendClientDatagram(&c);
 check(frames[1].sequence==1&&c.pendingdeltabits[51]==16&&c.pendingcsqcbits[51]==32,"ring slot retires stale records before current sequence assignment");
 init();c.datagram.cursize=1134;SV_SendClientDatagram(&c);
 check(body_limit==66&&entity_admitted==1&&pending(&c,0),"exact mandatory plus shared entity admission defers stats and retains optional ledger");
 init();c.fteprotocolextensions2|=PEXT2_VRINPUTS;c.baseangles[0]=1;c.baseangles[1]=2;c.baseangles[2]=3;c.datagram.cursize=1126;SV_SendClientDatagram(&c);
 check(body_limit==74&&entity_admitted==1&&pending(&c,0),"VR prefix measured reserve preserves exact entity admission boundary");
 init();c.protocol=1;c.pextknown=1;c.datagram.cursize=1168;SV_SendClientDatagram(&c);
 check(c.data_calls==1&&body_limit==32,"NQ keeps original unreserved flow, excluded from QW repair");
 init();c.fteprotocolextensions2=0;c.datagram.cursize=1168;SV_SendClientDatagram(&c);
 check(c.data_calls==1&&body_limit==32,"legacy QW keeps original unreserved flow");
 qbyte bytes[128];sizebuf_t b={bytes,0,15,1,0,0};voice(&b,10);
 check(b.cursize==0&&!b.overflowed,"voice payload plus five boundary defers rather than overflowing");
 b.maxsize=16;b.cursize=0;b.overflowed=0;voice(&b,10);check(b.cursize==16&&!b.overflowed,"voice exact six-byte header boundary writes");
 printf("DELIVERY SUMMARY checks=%d failures=%d\n",checks,failures);return failures;
}
"""

def delivery(a, text, compiler, env):
    """Act the production datagram, loss/ACK and capacity paths with CPU-only producers."""
    source_root = a.source.parent
    ents = (source_root / 'sv_ents.c').read_text()
    common = (source_root.parent / 'common/common.c').read_text() if (source_root.parent / 'common/common.c').exists() else (source_root / 'common.c').read_text()
    server = (source_root / 'server.h').read_text()
    user = (source_root / 'sv_user.c').read_text()
    fixed = 'void SV_RequeueFrame(' in ents
    fragments = []
    for anchor in ['static qboolean SV_WritePendingStringStat(', 'static void SV_DiffReplacementStats(', 'static void SV_WritePendingStats(', 'static unsigned int SV_PrepareReplacementStats(', 'static void SV_WriteMandatoryClientdata(']:
        if anchor in text:
            fragments.append(block(text, text.index(anchor)).replace('extern cvar_t pm_physicsmode;', ''))
    if fixed:
        fragments.insert(0, block(ents, ents.index('int SV_ReplacementEntityReserve(')))
        fragments.insert(0, block(ents, ents.index('void SV_RequeueFrame(')))
    for anchor in ['void SV_CSQC_DroppedPacket(', 'void SV_AckEntityFrame(', 'void SV_ReplaceEntityFrame(']:
        fragments.append(block(ents, ents.index(anchor)))
    # Baseline scheduling is still the real replacement branch, not a model.
    start = text.index('void SV_UpdateClientStats (')
    branch = block(text, text.index('if (client->fteprotocolextensions2 & PEXT2_REPLACEMENTDELTAS)', start))
    update = 'static void SV_UpdateClientStats(client_t *client,int pnum,sizebuf_t *msg,client_frame_t *frame) { int statsi[256];float statsf[256];const char *statss[256];unsigned i,m=SV_CalcClientStats(client,statsi,statsf,statss); (void)i;' + branch + '}'
    fragments.append(update)
    fragments.append(block(text, text.index('qboolean SV_SendClientDatagram (')))
    admission = '52'
    import re
    match = re.search(r'#define SV_REPLACEMENT_ENTITY_ADMISSION (\d+)', server)
    if match:
        admission = match[1]
    # Extract the actual VR prefix and admission predicate from the entity writer.
    vrstart = ents.index('if (client->fteprotocolextensions2 & PEXT2_VRINPUTS)', ents.index('sequence = client->netchan.outgoing_unreliable'))
    vr = block(ents, vrstart)
    predstart = ents.index('if (msg->cursize + ', vrstart + len(vr))
    predicate = ents[predstart:ents.index(chr(10), predstart)]
    voice_start = user.index('if (buf->maxsize - buf->cursize < ring->datalen+')
    voice_end = user.index(chr(10) + chr(9)*2 + '}', voice_start)
    voice = user[voice_start:voice_end].replace('break;', 'return;')
    header = DELIVERY_HEADER.replace('@ADMISSION@', admission)
    buffer = block(common, common.index('void SZ_Clear (')) + block(common, common.index('void *SZ_GetSpace (')) + block(common, common.index('void SZ_Write ('))
    producers = DELIVERY_PRODUCERS.replace('@VR@', vr).replace('@PREDICATE@', predicate).replace('@VOICE@', voice)
    code = header + buffer + producers + chr(10).join(fragments) + DELIVERY_TESTS
    c = a.output / 'delivery.c'
    c.write_bytes(code.encode('utf8'))
    exe = a.output / ('delivery.exe' if os.name == 'nt' else 'delivery')
    build = subprocess.run([compiler, '-std=gnu11', '-Wall', '-Werror', '-o', str(exe), str(c)], env=env, capture_output=True, text=True)
    (a.output / 'delivery.build.stdout').write_text(build.stdout)
    (a.output / 'delivery.build.stderr').write_text(build.stderr)
    if build.returncode:
        raise RuntimeError('delivery fixture compile failed; retained diagnostics')
    run = subprocess.run([str(exe.resolve())], env=env, capture_output=True, text=True)
    (a.output / 'delivery.run.stdout').write_text(run.stdout)
    (a.output / 'delivery.run.stderr').write_text(run.stderr)
    (a.output / 'delivery.receipt.json').write_text(json.dumps(dict(source_hashes={str(p.name): hashlib.sha256(p.read_bytes()).hexdigest() for p in [a.source, source_root/'sv_ents.c', source_root/'server.h', source_root/'sv_user.c', Path(__file__)]}, common_sha256=hashlib.sha256(common.encode()).hexdigest(), extracted_fixture_sha256=hashlib.sha256(code.encode()).hexdigest(), compiler=compiler, compile_exit=build.returncode, run_exit=run.returncode, cpu_only=True), indent=2))
    print(run.stdout, end='')
    expected = set() if fixed else {
        '32-byte optional pressure reclaims essential capacity and serializes clientdata once',
        'empty discarded packet ACK cannot silently retire unchanged numeric stat',
        'local discard requeues current numeric and phase before ACK',
        'local discard restores serialized entity and CSQC records',
        'unchanged discarded values are actually written on next attempt then ACKed',
        'ring slot retires stale records before current sequence assignment',
        'exact mandatory plus shared entity admission defers stats and retains optional ledger',
        'VR prefix measured reserve preserves exact entity admission boundary',
        'voice payload plus five boundary defers rather than overflowing',
    }
    failed = {line[5:] for line in run.stdout.splitlines() if line.startswith('FAIL ')}
    return 0 if failed == expected and run.returncode == len(expected) and 'DELIVERY SUMMARY checks=15' in run.stdout else 1


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
    if 'static void SV_DiffReplacementStats(' in text:
        branch = '{ SV_DiffReplacementStats(client, m, statsi, statsf, statss); SV_WritePendingStats(client, pnum, msg, frame, m); }'
    helper_anchor = 'static qboolean SV_WritePendingStringStat('
    helper = block(text, text.index(helper_anchor)) if helper_anchor in text else ''
    if 'static void SV_DiffReplacementStats(' in text:
        helper += block(text, text.index('static void SV_DiffReplacementStats('))
        helper += block(text, text.index('static void SV_WritePendingStats(')).replace('extern cvar_t pm_physicsmode;', '')
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
typedef struct {unsigned char data[8192];size_t cursize,maxsize;int allowoverflow,overflowed;} sizebuf_t;
typedef struct {unsigned numresendstats;unsigned short resendstats[32];} client_frame_t;
typedef struct {unsigned fteprotocolextensions2;int statsi[256];float statsf[256];const char *statss[256];unsigned pendingstats[16];} client_t;
static void SZ_Clear(sizebuf_t *b) {b->cursize=0;b->overflowed=0;}
static void *SZ_GetSpace(sizebuf_t *b,int length) {
 if(b->cursize+length>b->maxsize) {SZ_Clear(b);b->overflowed=1;}
 void *data=b->data+b->cursize;b->cursize+=length;return data;
}
static void MSG_WriteByte(sizebuf_t *b,int x) {*(unsigned char*)SZ_GetSpace(b,1)=(unsigned char)x;}
static void MSG_WriteLong(sizebuf_t *b,int x) {for(int i=0;i<4;i++)MSG_WriteByte(b,(unsigned)x>>(i*8));}
static void MSG_WriteFloat(sizebuf_t *b,float x) {union{float f;int i;}v;v.f=x;MSG_WriteLong(b,v.i);}
static void MSG_WriteString(sizebuf_t *b,const char *s) {do {MSG_WriteByte(b,*s);}while(*s++);}
'''
    wrapper = r'''
static void schedule(client_t *client,int pnum,sizebuf_t *msg,client_frame_t *frame,unsigned m,int *statsi,float *statsf,const char **statss) {
 unsigned i; (void)i;
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
    delivery_result = delivery(a, text, compiler, env)
    return 0 if delivery_result == 0 and run.returncode == a.expect_failures and 'SUMMARY checks=' in run.stdout else 1


if __name__ == '__main__':
    raise SystemExit(main())
