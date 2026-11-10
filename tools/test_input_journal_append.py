#!/usr/bin/env python3
"""Actual journal/drain/paste transactions with explicit hosts and real OS mutexes.

Synthetic constants only. No game, capture, clipboard OS API or player data.
Producer-entry/header-search checkpoints stop the unsafe control before mutation;
Identity uses its complete actual body; backend diagnostics, Cvar/game callbacks
and pending-paste creation remain explicit hosts, not engine runtime acceptance.
Mutex-call counts are not latency measurements or general native thread safety.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess

from test_input_journal_end import HEADER, close_support
from test_input_journal_lifecycle import PRE, HOST


def func(text, signature):
    """Keep original function bytes; mask strings/comments only to balance braces."""
    masked = re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*.*?\*/|//[^\n]*',
                    lambda m: ' ' * len(m.group()), text, flags=re.S)
    start = text.index(signature)
    begin = masked.index('{', start)
    depth = 0
    for i in range(begin, len(masked)):
        if masked[i] == '{':
            depth += 1
        elif masked[i] == '}':
            depth -= 1
            if not depth:
                return text[start:i + 1] + '\n'
    raise ValueError('Unclosed actual function: ' + signature)


EXTRA = r'''
#include <math.h>
#define CSQC_DAT 1
#define TEXTEDITOR 1
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define K_MAX 256
#define K_MOUSE1 200
#define K_TOUCH 201
#define K_TOUCHLONG 202
#define K_TOUCHTAP 203
#define K_TOUCHSLIDE 204
#define kdm_game 0
#define bound(a,b,c) ((b)<(a)?(a):((b)>(c)?(c):(b)))
typedef float vec2_t[2];
static unsigned keydown[K_MAX];
static bool in_real_keydown, in_real_held[K_MAX];
static float in_jrn_lastabs[8][2], in_jrn_vlast[2];
static float mousecursor_x, mousecursor_y;
static struct { int width,height,pixelwidth,pixelheight; } vid={800,600,800,600};
static struct { bool enabled; } vrui;
static cvar_t in_rawmice,in_rawkbds,in_jrn484_frames,in_jrn484_ghosts;
static cvar_t in_jrn484_violations,in_jrn484_badframes,in_jrn484_skipped;
static cvar_t m_longpressthreshold={.value=1},m_slidethreshold={.value=8};
static cvar_t m_forcewheel,m_forcewheel_threshold,r_xflip;
static bool watch_status;
static int status_mode,status_cb_count,status_reads;
static int main_locks,main_unlocks,main_owned,worker_owned,recursive,foreign_hint;
static int callback_mode,callback_count,paste_count,paste_frees,inherited,editormodal;
static int joy_calls,accel_calls,gyro_calls;
static bool validate,source_stop;
static jmp_buf worker_abort;
static pthread_t return_control;
static int return_worker_started;
static void *worker(void *ctx);
static void *controller(void *ctx);
static void wait_for(int *a,int *b);
static void check(bool ok,const char *label) {
 checks++; if(!ok){failures++;fprintf(stderr,"FAIL %s\n",label);}
}
static void callback_action(void);
static void callback_checkpoint(void) {
 if (pthread_equal(pthread_self(),main_tid))
  check(!main_owned || inherited,"callback outside newly-owned journal group");
 callback_count++;
}
static bool Key_Dest_Has_Higher(int d) {(void)d;return false;}
static bool Key_MouseShouldBeFree(void) {return true;}
static void INS_Commands(void) {callback_checkpoint();}
static void status_callback(cvar_t *c);
static void Cvar_ForceSetValue(cvar_t *c,float value) {
 bool watched=watch_status;
 callback_checkpoint();c->ival=(int)value;c->value=value;
 if(c==&in_jrn484_frames || c==&in_jrn484_ghosts || c==&in_jrn484_violations
    || c==&in_jrn484_badframes || c==&in_jrn484_skipped) {
  /*Model core update-before-callback; callback setup is not a status read.*/
  watch_status=false;status_callback(c);watch_status=watched;
 }
}
static void Key_Event(unsigned d,int key,int unicode,bool down) {
 (void)unicode; callback_checkpoint();
 if(key>0 && key<K_MAX && d<32) {
  if(down)keydown[key]|=1u<<d;else keydown[key]&=~(1u<<d);
 }
 if(down)callback_action();
}
static bool CSQC_JoystickAxis(int a,float v,unsigned d) {
 (void)a;(void)v;(void)d;joy_calls++;callback_checkpoint();return false;
}
static void CSQC_Accelerometer(float x,float y,float z) {
 (void)x;(void)y;(void)z;accel_calls++;callback_checkpoint();
}
static void CSQC_Gyroscope(float x,float y,float z) {
 (void)x;(void)y;(void)z;gyro_calls++;callback_checkpoint();
}
static struct menu_s { bool (*joyaxis)(struct menu_s *,unsigned,int,float); } *topmenu;
static void Z_Free(void *p);
static void producer_entry(void) {
 if(!validate || !runningindepphys)return;
 if(pthread_equal(pthread_self(),main_tid)) {
  check(main_owned,"main producer owns existing exclusion");
  if(main_owned)return;
  pthread_mutex_lock(&state);
  if(schedule && worker_busy) {
   unsafe=1;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&state);
   source_stop=true;longjmp(main_abort,1);
  }
  pthread_mutex_unlock(&state);
 }
}
static char *checked_strstr(const char *s,const char *needle) {
 if(!strcmp(needle,"\nsynth 0\n"))producer_entry();
 return strstr(s,needle);
}
#if HAS_HINT
static bool host_mainactive;
#if HAS_CACHE
static bool host_mainsynth;
#endif
static bool *hint_slot(bool poisoned) {
 if(!pthread_equal(pthread_self(),main_tid))foreign_hint++;
#if HAS_CACHE
 if(poisoned)return &host_mainsynth;
#else
 (void)poisoned;
#endif
 return &host_mainactive;
}
#define in_jrn_mainactive (*hint_slot(false))
#if HAS_CACHE
#define in_jrn_mainsynth (*hint_slot(true))
#endif
#endif
static unsigned int *status_slot(int slot) {
#if HAS_HINT
 if(watch_status && runningindepphys && pthread_equal(pthread_self(),main_tid) && host_mainactive) {
  status_reads++;producer_entry();
 }
#endif
 switch(slot) {
 case 0:return &in_jrn_vcount;
 case 1:return in_jrn_ghosts;
 case 2:return in_jrn_violations;
 case 3:return &in_jrn_vbad;
 case 4:return &in_jrn_vskip;
 default:abort();
 }
}
#define in_jrn_vcount (*status_slot(0))
#define in_jrn_ghosts (status_slot(1))
#define in_jrn_violations (status_slot(2))
#define in_jrn_vbad (*status_slot(3))
#define in_jrn_vskip (*status_slot(4))
'''

SYNC = r'''
static void Sys_LockMutex(void *p) {
 bool main=pthread_equal(pthread_self(),main_tid);
 pthread_mutex_lock(&state);
 lock_calls++;
 if((main && main_owned)||(!main && worker_owned)) {
  recursive++;pthread_mutex_unlock(&state);
  if(main)longjmp(main_abort,1);else longjmp(worker_abort,1);
 }
 if(main) {
  main_locks++;
  if(schedule && main_ready){attempted=1;pthread_cond_broadcast(&changed);}
 }
 pthread_mutex_unlock(&state);
 if(pthread_mutex_lock(p))abort();
 if(main)main_owned=1;else worker_owned=1;
}
static void Sys_UnlockMutex(void *p) {
 bool main=pthread_equal(pthread_self(),main_tid);
 pthread_mutex_lock(&state);unlock_calls++;
 if(main){main_unlocks++;main_owned=0;}else worker_owned=0;
 pthread_mutex_unlock(&state);
 if(pthread_mutex_unlock(p))abort();
}
'''

TEST = r'''
static void Z_Free(void *p) {
 size_t i;const unsigned char *s=p;
 for(i=0;i<sizeof(pasteanswer_t);i++)if(s[i])break;
 check(i==sizeof(pasteanswer_t),"actual paste answer zeroed before free");
 check(cl_paste_ready==NULL,"actual paste pending consumed before free");
 paste_frees++;free(p);
}
static void callback_action(void) {
 int mode=callback_mode;
 if(!mode)return;
 callback_mode=0;
 if(mode==1) {
  IN_Journal_Note("callback");
  CL_AllowIndependantSendCmd(false);inherited=1;
 }else if(mode==2) {
  IN_Journal_Drop();IN_JournalBegin_f();
  IN_Journal_Note("replacement");
 }else if(mode==3) {
  IN_JournalBegin_f();IN_Journal_Note("opened");
 }else if(mode==4) {
  check(!main_owned,"paste released owner before admitting another worker");
  if(main_owned)return; /*Safe stop instead of waiting on our own bad lock.*/
  schedule=1;
  if(pthread_create(&worker_tid,NULL,worker,NULL))abort();
  pthread_mutex_lock(&state);wait_for(&paused,NULL);main_ready=1;pthread_mutex_unlock(&state);
  if(pthread_create(&return_control,NULL,controller,NULL))abort();
  return_worker_started=1;
 }
}
static void give(const unsigned int *text,int chars,unsigned int vm) {
 callback_checkpoint();paste_count++;
 check(chars==1 && text[0]=='P' && vm==17,"actual copied paste survives erased/freed queue");
 check(paste_frees==paste_count,"paste free precedes recipient");
 callback_action();
}
static void queue_paste(void) {
 pasteanswer_t *p=calloc(1,sizeof(*p));if(!p)abort();
 p->give=give;p->text[0]='P';p->chars=1;p->vm=17;cl_paste_ready=p;
}
static void wait_for(int *a,int *b) {
 struct timespec until;clock_gettime(CLOCK_REALTIME,&until);until.tv_sec+=5;
 while(!*a && (!b || !*b))
  if(pthread_cond_timedwait(&changed,&state,&until)==ETIMEDOUT)abort();
}
static void *worker(void *ctx) {
 float angles[3]={10,20,0};(void)ctx;
 Sys_LockMutex(indeplock);
 pthread_mutex_lock(&state);worker_busy=1;pthread_mutex_unlock(&state);
 IN_Journal_View(angles);
 pthread_mutex_lock(&state);worker_busy=0;done=1;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&state);
 Sys_UnlockMutex(indeplock);return NULL;
}
static void *controller(void *ctx) {
 (void)ctx;pthread_mutex_lock(&state);wait_for(&attempted,&unsafe);
 release_worker=1;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&state);return NULL;
}
static void cleanup(void) {
 validate=false;schedule=0;
 if(main_owned)Sys_UnlockMutex(indeplock);
 runningindepphys=false;allowindepphys=true;inherited=0;
 IN_JournalEnd_f();IN_Journal_Drop();
 if(cl_paste_ready){free(cl_paste_ready);cl_paste_ready=NULL;}
}
static cvar_t *mirror(int i) {
 static cvar_t *vars[]={&in_jrn484_frames,&in_jrn484_ghosts,&in_jrn484_violations,
  &in_jrn484_badframes,&in_jrn484_skipped};
 return vars[i];
}
static void clear_mirrors(void) {
 int i;for(i=0;i<5;i++){mirror(i)->ival=0;mirror(i)->value=0;}
}
static void sync_mirrors(void) {
 unsigned int values[]={in_jrn_vcount,in_jrn_ghosts[0]+in_jrn_ghosts[1],
  in_jrn_violations[0]+in_jrn_violations[1],in_jrn_vbad,in_jrn_vskip};
 int i;for(i=0;i<5;i++) {
  mirror(i)->ival=in_jrn_ran?(int)values[i]:-1;
  mirror(i)->value=in_jrn_ran?(float)values[i]:-1;
 }
}
static void seed_status(unsigned int base) {
 bool was=CL_AllowIndependantSendCmd(false);
 in_jrn_vcount=base;in_jrn_ghosts[0]=base+1;in_jrn_ghosts[1]=base+2;
 in_jrn_violations[0]=base+3;in_jrn_violations[1]=base+4;
 in_jrn_vbad=base+5;in_jrn_vskip=base+6;
 CL_AllowIndependantSendCmd(was);
}
static void status_callback(cvar_t *c) {
 int mode=status_mode;status_cb_count++;
 if(c!=&in_jrn484_frames || !mode)return;
 status_mode=0;
 if(mode==2)IN_Journal_Drop();else IN_JournalBegin_f();
 seed_status((unsigned int)mode*100);
 if(mode==3){CL_AllowIndependantSendCmd(false);inherited=1;}
}
static void watched_commands(void) {
 watch_status=true;IN_Commands();watch_status=false;
}
static void mirrors_are(int f,int g,int v,int b,int s,const char *label) {
 check(mirror(0)->ival==f && mirror(1)->ival==g && mirror(2)->ival==v
  && mirror(3)->ival==b && mirror(4)->ival==s,label);
}
static void prepare(bool active) {
 watch_status=false;status_mode=status_cb_count=status_reads=0;
 cleanup();
 events_used=events_avail=0;memset(eventlist,0,sizeof(eventlist));
 memset(ptr,0,sizeof(ptr));memset(joy,0,sizeof(joy));memset(keydown,0,sizeof(keydown));
 callback_mode=callback_count=paste_count=paste_frees=0;editormodal=0;
 joy_calls=accel_calls=gyro_calls=0;return_worker_started=0;
 argc_value=1;arg_name="";write_ok=true;
 if(active)IN_JournalBegin_f();
 cl.minpitch=-90;cl.maxpitch=90;m_filter.value=m_accel.value=1;r_xflip.ival=0;
 /*Pure-group budgets start with unchanged mirrors; changed-setter arms are explicit.*/
 sync_mirrors();
 main_locks=main_unlocks=0;recursive=foreign_hint=0;
 paused=release_worker=worker_busy=main_ready=attempted=unsafe=done=0;source_stop=false;
 runningindepphys=true;allowindepphys=true;validate=true;
}
static void relative(int n) {
 int i;events_avail=n;
 for(i=0;i<n;i++) {
  eventlist[i].type=IEV_MOUSEDELTA;eventlist[i].time=2;
  eventlist[i].mouse.x=1;eventlist[i].mouse.y=2;
 }
}
static char *body(void) {return copy_bytes(in_jrn_buf,in_jrn_len);}
static int rows(const char *s,const char *prefix) {
 int n=0;size_t len=strlen(prefix);const char *p=s;
 do {if(!strncmp(p,prefix,len))n++;p=strchr(p,'\n');if(p)p++;}while(p && *p);
 return n;
}
static void ordinary(void) {
 int l,u;char *s,*pad;size_t n,oldcap;
 prepare(false);relative(32);IN_Commands();
 check(main_locks==0 && main_unlocks==0,"inactive capture adds zero journal mutex attempts");
 check(events_used==32 && ptr[0].delta[0]==32,"inactive input still dispatched");
 prepare(true);relative(700);oldcap=in_jrn_cap;n=in_jrn_cap-in_jrn_len-8;
 pad=malloc(n+1);if(!pad)abort();memset(pad,'p',n);pad[n-1]='\n';pad[n]=0;
 CL_AllowIndependantSendCmd(false);IN_Journal_Raw(pad);CL_AllowIndependantSendCmd(true);
 free(pad);main_locks=main_unlocks=0;IN_Commands();s=body();
 check(main_locks==1 && main_unlocks==1,"relative group shares one real lock across empty paste calls");
 check(rows(s,"f ")==1 && rows(s,"m ")==700,"all actual frame/event rows complete");
 check(in_jrn_events==700 && in_jrn_frames==1,"actual compound counters complete");
 check(ptr[0].delta[0]==700 && ptr[0].rawpend[1]==1400,"actual dispatch accumulators complete");
 check(in_jrn_cap>oldcap,"actual append growth acted");free(s);
 prepare(true);relative(8);CL_AllowIndependantSendCmd(false);l=main_locks;u=main_unlocks;inherited=1;
 IN_Commands();check(main_locks==l && main_unlocks==u,"borrowed false neither relocks nor unlocks");
 check(!allowindepphys && main_owned,"borrowed owner stays false and held");
 prepare(true);relative(0);IN_Commands();
 check(main_locks==(HAS_STATUS?1:0),"active empty mirror group follows bounded source budget");
 prepare(true);relative(4);queue_paste();editormodal=1;IN_Commands();
 check(cl_paste_ready!=NULL && paste_count==0,"actual editor early return preserves pending answer");
 check(main_locks==1 && main_unlocks==1,"editor/empty flush preserves relative group");
 editormodal=0;CL_Paste_Flush();check(paste_count==1,"plain flush interface still delivers");
 prepare(true);relative(3);eventlist[1].type=IEV_KEYDOWN;eventlist[1].keyboard.scancode=42;
 callback_mode=1;IN_Commands();s=body();
 check(!allowindepphys && main_owned,"callback false is not overwritten by stale restoration");
 check(strstr(s,"+ 0 0 42 0\n# 0 callback\nm 0")!=NULL,"key annotation retains actual stream position");free(s);
 prepare(true);relative(3);eventlist[1].type=IEV_KEYDOWN;eventlist[1].keyboard.scancode=42;
 callback_mode=2;IN_Commands();s=body();
 check(rows(s,"m ")==1 && rows(s,"f ")==0,"callback replacement has only later event, no prepass");
 check(strstr(s,"# 0 replacement\nm 0")!=NULL,"replacement annotation precedes later event");free(s);
 prepare(false);relative(3);queue_paste();callback_mode=3;IN_Commands();s=body();
 check(rows(s,"f ")==0 && rows(s,"m ")==3,"initial paste Begin admits later events with fresh hint");
 check(strstr(s,"# 0 opened\nm 0")!=NULL,"initial paste chronology preserved");free(s);
 prepare(true);relative(3);queue_paste();callback_mode=1;IN_Commands();
 check(!allowindepphys && main_owned,"paste callback owns its deliberately false result");
 check(paste_count==1,"actual queued paste acted");
 prepare(true);relative(5);eventlist[0].type=IEV_MOUSEABS;eventlist[0].mouse.x=10;
 eventlist[1].type=IEV_JOYAXIS;eventlist[1].joy.axis=0;eventlist[1].joy.value=1;
 eventlist[2].type=IEV_ACCELEROMETER;eventlist[3].type=IEV_GYROSCOPE;
 IN_Commands();check(events_used==5 && joy_calls==1 && accel_calls==1 && gyro_calls==1
  && joy[0].axis[0]==1 && mousecursor_x==11,"nonrelative actual dispatch branches acted");
 check(allowindepphys && !main_owned,"nonrelative branches restore only own permission");
 prepare(true);relative(2);runningindepphys=false;IN_Commands();
 check(main_locks==0 && main_unlocks==0,"disabled-worker selected body needs no OS mutex");
}
static void competing(int op) {
 pthread_t control;char *s;int before;
 prepare(true);relative(2);in_jrn_vdx=in_jrn_vrawx=3;in_jrn_vdy=in_jrn_vrawy=4;
 schedule=1;
 if(pthread_create(&worker_tid,NULL,worker,NULL))abort();
 pthread_mutex_lock(&state);wait_for(&paused,NULL);main_ready=1;pthread_mutex_unlock(&state);
 if(pthread_create(&control,NULL,controller,NULL))abort();
 before=failures;
 if(!setjmp(main_abort)) {
  if(op==0)IN_Commands();else if(op==1)IN_Journal_Note("main");else IN_Journal_MarkSynth("fixture");
 }
 pthread_join(worker_tid,NULL);pthread_join(control,NULL);schedule=0;
 check(!source_stop && !unsafe,"main transaction excludes actually admitted worker");
 check(attempted && done,"actual mutex attempt and worker completion both acted");
 check(!recursive,"no recursive OS lock");
 check(main_locks==1 && main_unlocks==1,"one owned transaction; nested Note never relocks");s=body();
 check(rows(s,"v ")==1,"complete actual worker View row");
 check(in_jrn_vdx==0 && in_jrn_vrawy==0,"actual worker accumulator reset complete");
 if(op==0)check(rows(s,"f ")==1 && rows(s,"m ")==2,"complete subsequent actual drain rows");
 if(op==1)check(strstr(s,"\n# 0 main\n")!=NULL,"complete subsequent actual Note");
 if(op==2)check(strstr(s,"\nsynth 1\n") && strstr(s,"# 0 SYNTH source fixture\n"),"complete guarded synth header and nested Note");
 printf("append competing %d: %s\n",op,failures==before?"pass":"falsified");free(s);
}
static void fresh_after_paste(void) {
 char *s,*f,*v,*m;
 prepare(true);relative(3);queue_paste();callback_mode=4;
 in_jrn_vdx=in_jrn_vrawx=3;in_jrn_vdy=in_jrn_vrawy=4;
 if(!setjmp(main_abort))IN_Commands();
 if(return_worker_started){pthread_join(worker_tid,NULL);pthread_join(return_control,NULL);}
 schedule=0;
 check(return_worker_started && attempted && done && !unsafe && !source_stop,
       "fresh worker exclusion after actual paste callback");
 check(main_locks==2 && main_unlocks==2,"actual paste splits two owned relative groups");
 s=body();f=strstr(s,"\nf ");v=strstr(s,"\nv ");m=strstr(s,"\nm ");
 check(f && v && m && f<v && v<m && rows(s,"m ")==3,"actual frame paste worker event chronology");free(s);
}
static void status_publication(void) {
 pthread_t control;char *s;
 prepare(true);relative(0);clear_mirrors();
 m_filter.value=m_accel.value=0;in_jrn_vhavelast=true;
 in_jrn_vlast[PITCH]=in_jrn_vlast[YAW]=0; /*Different from worker's angles: no quiet return.*/
 schedule=1;
 if(pthread_create(&worker_tid,NULL,worker,NULL))abort();
 pthread_mutex_lock(&state);wait_for(&paused,NULL);main_ready=1;pthread_mutex_unlock(&state);
 if(pthread_create(&control,NULL,controller,NULL))abort();
 if(!setjmp(main_abort))watched_commands();
 watch_status=false;
 pthread_join(worker_tid,NULL);pthread_join(control,NULL);schedule=0;
 check((attempted || unsafe) && done && in_jrn_vcount==1 && in_jrn_ghosts[0]==1
  && in_jrn_vbad==1,"actual admitted View identity writer acted, not a no-op adapter");
 check(!unsafe && !source_stop,"status read waits for actual admitted counter writer");
 check(status_reads==7,"all seven unsigned source loads protected once before setter release");
 mirrors_are(1,1,0,1,0,"published complete actual worker identity values");
 check(main_locks==4 && main_unlocks==4,"three changed mirrors split bounded empty publication groups");
 s=body();check(rows(s,"v ")==1 && rows(s,"f ")==0,"actual worker row complete without inventing idle frame");free(s);

 prepare(true);relative(0);clear_mirrors();m_filter.value=m_accel.value=0;
 in_jrn_vhavelast=true;in_jrn_vdx=in_jrn_vrawx=1;
 if(pthread_create(&worker_tid,NULL,worker,NULL))abort();
 pthread_join(worker_tid,NULL);
 m_filter.value=1;in_jrn_vdx=in_jrn_vrawx=1;
 if(pthread_create(&worker_tid,NULL,worker,NULL))abort();
 pthread_join(worker_tid,NULL);
 watched_commands();
 mirrors_are(1,0,1,1,1,"actual violation and skipped writer branches published");
 check(in_jrn_violations[0]==1 && in_jrn_vskip==1 && done,"actual full identity violation/abstention acted");

 prepare(true);relative(0);watched_commands();
 check(status_reads==7 && status_cb_count==0,"unchanged mirrors read once without setters");
 check(main_locks==(HAS_STATUS?1:0) && main_unlocks==main_locks,"one unchanged active empty mirror group");
 s=body();check(rows(s,"f ")==0 && rows(s,"m ")==0,"idle publication adds no journal row");free(s);

 prepare(true);seed_status(7);IN_Journal_Drop();clear_mirrors();main_locks=main_unlocks=0;
 watched_commands();mirrors_are(7,17,21,12,13,"inactive retained measurements still publish after Ready false");
 check(main_locks==0 && main_unlocks==0,"inactive retained publication acquires zero journal mutexes");
 prepare(true);seed_status(7);clear_mirrors();main_locks=main_unlocks=0;runningindepphys=false;
 watched_commands();mirrors_are(7,17,21,12,13,"disabled worker still publishes measurements");
 check(main_locks==0 && main_unlocks==0,"disabled worker needs no real status mutex");

 prepare(false);in_jrn_ran=false;clear_mirrors();watched_commands();
 mirrors_are(-1,-1,-1,-1,-1,"never measured remains unknown, not measured zero");
 check(status_reads==0 && main_locks==0,"unknown branch reads no shared counters or mutex");
 prepare(false);in_jrn_ran=false;clear_mirrors();status_mode=4;watched_commands();
 mirrors_are(-1,-1,-1,-1,-1,"unknown branch chronology retained when its first setter begins capture");
 check(in_jrn_ran && in_jrn_vcount==400,"unknown setter callback actually opened and seeded new capture");

 prepare(true);seed_status(10);clear_mirrors();status_mode=1;watched_commands();
 mirrors_are(10,203,207,105,106,"callback replacement rechecks next source values, not old tuple");
 check(status_cb_count==5 && in_jrn_vcount==100 && allowindepphys && !main_owned,"actual setter host acted outside owned token");
 prepare(true);seed_status(10);clear_mirrors();status_mode=2;watched_commands();
 mirrors_are(10,403,407,205,206,"callback Drop still publishes fresh retained values");
 check(!in_jrn_buf && allowindepphys && !main_owned,"callback Drop stays inactive without stale token restore");
 prepare(true);seed_status(10);clear_mirrors();status_mode=3;watched_commands();
 mirrors_are(10,603,607,305,306,"callback replacement values stay fresh under borrowed ownership");
 check(!allowindepphys && main_owned && !recursive,"setter's deliberately false ownership survives final release");
}
static void *poison_worker(void *ctx) {
 (void)ctx;Sys_LockMutex(indeplock);IN_Journal_MarkSynth("worker");Sys_UnlockMutex(indeplock);return NULL;
}
static void synth_hot(void) {
 int i;char *s;
 prepare(true);
 for(i=0;i<1000;i++)IN_Journal_MarkSynth("plugin");
 check(main_locks==1 && main_unlocks==1,"1000 main synthetic API marks own once, not per event");
 s=body();check(strstr(s,"\nsynth 1\n") && strstr(s,"SYNTH source plugin\n"),"cold main synthetic mark actually poisons");free(s);
 IN_Journal_Drop();IN_JournalBegin_f();main_locks=main_unlocks=0;
 for(i=0;i<1000;i++)IN_Journal_MarkSynth("plugin");
 check(main_locks==1 && main_unlocks==1 && in_jrn_synth,"actual Drop/Begin resets current main poisoned hint");
 prepare(true);
 if(pthread_create(&worker_tid,NULL,poison_worker,NULL))abort();
 pthread_join(worker_tid,NULL);
 check(in_jrn_synth && !foreign_hint,"admitted worker poisons without touching main-only hints");
 for(i=0;i<1000;i++)IN_Journal_MarkSynth("plugin");
 check(main_locks==1 && main_unlocks==1,"main observes worker poison once then uses its own cache");
 s=body();check(strstr(s,"SYNTH source worker\n") && !strstr(s,"SYNTH source plugin\n"),"observing poison preserves original marker provenance");free(s);
 prepare(false);for(i=0;i<1000;i++)IN_Journal_MarkSynth("plugin");
 check(main_locks==0 && main_unlocks==0,"inactive synthetic API adds zero acquisitions");
}
static void *editor_worker(void *ctx) {
 (void)ctx;Sys_LockMutex(indeplock);
 if(!setjmp(worker_abort))IN_Commands_Editor();
 Sys_UnlockMutex(indeplock);return NULL;
}
static void offmain(void) {
 prepare(true);relative(3);
 if(pthread_create(&worker_tid,NULL,editor_worker,NULL))abort();
 pthread_join(worker_tid,NULL);
 check(events_used==3,"actual shared editor drain consumed worker-side events");
 check(!recursive && !foreign_hint,"off-main route neither reads main hint nor recursively takes permission");
 check(main_locks==0 && allowindepphys,"off-main route never changes main permission");
}
int main(void) {
 main_tid=pthread_self();ordinary();competing(0);competing(1);competing(2);fresh_after_paste();status_publication();synth_hot();offmain();
 cleanup();free(freed_snapshot);free(snapshot);free(written);
 printf("journal ordinary append: %d checks, %d failures\n",checks,failures);
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
    ap.add_argument('--source-ref', help='Inspected immutable acting-control source')
    a = ap.parse_args()
    a.work_root.mkdir(parents=True, exist_ok=False)
    (a.work_root / 'OWNER.md').write_text('Synthetic actual-C journal/drain/paste OS-thread fixture. Parent c66e. No game, input/clipboard capture or player data. Retain source/compile/result evidence.\n')
    root = Path(__file__).resolve().parents[1]
    ref = subprocess.check_output(['git', '-C', str(root), 'rev-parse', '--verify', a.source_ref + '^{commit}'], text=True).strip() if a.source_ref else None
    text, ci, paste = [blob(root, ref, n) for n in ['engine/client/in_generic.c', 'engine/client/cl_input.c', 'engine/client/pr_clcmd.c']]
    has_hint = 'static qboolean\t\tin_jrn_mainactive;' in text
    h = HEADER[:HEADER.index('static void IN_Journal_Drop(void)')]
    h = h.replace('static bool Sys_IsMainThread(void) { return true; }', 'static bool Sys_IsMainThread(void) { return pthread_equal(pthread_self(),main_tid); }')
    h = h.replace('void (*process)(void *, const void *, size_t);', 'void (*process)(void *, const void *, size_t); size_t contextsize; void (*init)(void *);')
    h = h.replace('hash_sha2_256 = {hash_process};', 'hash_sha2_256 = {hash_process,1,hash_init};')
    host = HOST.replace('worker_tid, main_tid', 'worker_tid')
    lock = func(host, 'static void Sys_LockMutex(')
    unlock = func(host, 'static void Sys_UnlockMutex(')
    host = host.replace(lock, '').replace(unlock, '')
    host = host.replace('int serverinfo;', 'int serverinfo,movesequence;float minpitch,maxpitch;')
    host = host.replace('in_jrn_haveabs[2][4]', 'in_jrn_haveabs[8]')
    host = host.replace('static int in_jrn_dropreported,', 'static unsigned in_jrn_dropreported;\nstatic int')
    host = host.replace('lock_calls, unlock_calls, operation', 'lock_calls, unlock_calls')
    host = host.replace('if (p == in_jrn_buf)', 'if (p && p == in_jrn_buf)')
    helper = ci[ci.index('qboolean CL_AllowIndependantSendCmd('):ci.index('int CL_IndepPhysicsThread(')]
    raw = text[text.index('static void IN_Journal_Raw('):text.index('void IN_Journal_Note(')]
    raw = raw.replace('static void IN_Journal_Line(double when, const char *kind, const char *tail)\n{', 'static void IN_Journal_Line(double when, const char *kind, const char *tail)\n{\n producer_entry();')
    guard = text[text.index('typedef struct in_jrn_guard_s'):text.index('static void IN_Journal_Raw(')] if has_hint else ''
    note = func(text, 'void IN_Journal_Note(')
    synth = func(text, 'void IN_Journal_MarkSynth(')
    event = func(text, 'static void IN_Journal_Event(')
    frame = func(text, 'static void IN_Journal_Frame(').replace('{', '{\n producer_entry();', 1)
    identity = func(text, 'static void IN_Journal_CheckIdentity(')
    view = func(text, 'void IN_Journal_View(').replace('{', '{\n thread_before_realloc();', 1)
    publish = func(text, 'static void IN_Journal_PublishCounter(') if 'static void IN_Journal_PublishCounter(' in text else ''
    drop = text[text.index('void IN_Journal_Drop(void)'):text.index('static void IN_Journal_Event(')]
    start = text.index('static void IN_JournalBegin_f(void)\n{')
    lifecycle = text[start:text.index('static void IN_JournalNote_f(', start)]
    assert lifecycle.startswith('static void IN_JournalBegin_f') and 'static void IN_JournalEnd_f' in lifecycle
    ring = text[text.index('#define EVENTQUEUELENGTH'):text.index('/*FTESurf Patch 202: the ring has always')]
    devices = text[text.index('#define MAXPOINTERS 8'):text.index('void IN_Shutdown(void)')]
    pt = paste[paste.index('#define CL_PASTE_MAX'):paste.index('static unsigned int cl_paste_asked;')]
    pname = 'void CL_Paste_FlushGuarded(' if 'void CL_Paste_FlushGuarded(' in paste else 'void CL_Paste_Flush(void)'
    flush = paste[paste.index(pname):paste.index('/* Existing render-tail fallback')]
    drain = text[text.index('static void IN_Commands_Internal('):text.index('/*FTESurf Patch 376: per seat')]
    start, end = drain.index('\t\tswitch(ev->type)'), drain.index('\t\tevents_used++;')
    legacy = drain[start:end]
    frozen = blob(root, 'bb3e7cbac8cbfc1c585c891ab743a8b951d3f7dc', 'engine/client/in_generic.c')
    frozen = frozen[frozen.index('static void IN_Commands_Internal('):]
    assert legacy == frozen[frozen.index('\t\tswitch(ev->type)'):frozen.index('\t\tevents_used++;')]
    # Frozen dispatch has three existing extra signedness warnings. Keep them
    # visible ONLY there; all changed journal/guard/host code remains strict.
    drain = drain[:start] + '\n#pragma GCC diagnostic push\n#pragma GCC diagnostic warning "-Wsign-compare"\n' + legacy + '\n#pragma GCC diagnostic pop\n' + drain[end:]
    code = PRE + '\nstatic pthread_t main_tid;\n' + h + host + f'\n#define HAS_HINT {int(has_hint)}\n#define HAS_CACHE {int("in_jrn_mainsynth" in text)}\n#define HAS_STATUS {int(bool(publish))}\n' + EXTRA + SYNC + helper + close_support(text)
    code += ring + devices + guard + raw + note + drop + event + frame + identity + view + lifecycle
    code += '\n#define strstr checked_strstr\n' + synth + '\n#undef strstr\n' + pt + flush + publish + drain + TEST
    c = a.work_root / 'journal-append.c'; exe = a.work_root / 'journal-append.exe'
    c.write_text(code)
    env = dict(os.environ)
    if Path(a.cc).is_absolute():
        env['PATH'] = str(Path(a.cc).parent) + os.pathsep + env.get('PATH', '')
    subprocess.run([a.cc, '-std=c99', '-Wall', '-Wextra', '-Werror', '-pthread', str(c), '-o', str(exe)], env=env, check=True)
    result = subprocess.run([str(exe)], env=env, capture_output=True, text=True, timeout=30)
    (a.work_root / 'result.txt').write_text(result.stdout + result.stderr)
    print(result.stdout, end='')
    if result.returncode:
        print(result.stderr, end='')
    raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()
