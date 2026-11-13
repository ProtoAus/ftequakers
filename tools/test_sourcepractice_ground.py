#!/usr/bin/env python3
"""Actual native zero-Source grounding seed/caller/writeback in a controlled C harness.
World/mover/next-PreThink carrier are bounded stubs, NOT a live multiplayer proof.
--source-file permits acting the original committed no-seed counterexample.
"""
import argparse,os,pathlib,re,subprocess,tempfile
from test_sourcepractice_predictreset import function
ROOT=pathlib.Path(__file__).resolve().parents[1]
STUB=r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define true 1
#define false 0
#define FL_ONGROUND 1
#define PM_NORMAL 0
#define PHYSMODE_SOURCE 1
#define PHYSMODE_AG 2
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
typedef struct {unsigned int info;} physent_t;
struct {int onground,groundent,numphysent,pm_type;struct{int msec;}cmd;physent_t physents[3];}pmove;
struct{int physicsmode;}movevars;
int held=0,conveyor_arms=0;
struct{float flags;unsigned int groundentity;}v;
struct{typeof(v)*v;}player={&v},*sv_player=&player;
#define svprogfuncs 0
#define PROG_TO_EDICT(vm,e) (e)
#define NUM_FOR_EDICT(vm,e) (e)
#define EDICT_NUM_PB(vm,e) (e)
#define EDICT_TO_PROG(vm,e) (e)
__HELPER__
static void native_zero_command(void){
 __CALLER__
 /* Real zero-duration/zero-carry Source contract: zero ticks, no new contact. */
 __WRITEBACK__
 if(v.groundentity==7)++conveyor_arms; /* bounded consumer: unrelated conveyor */
}
static void setup(unsigned int desired){
 memset(&pmove,0,sizeof(pmove));pmove.onground=1;pmove.groundent=1;pmove.numphysent=3;
 pmove.physents[0].info=0;pmove.physents[1].info=7;pmove.physents[2].info=42;
 v.flags=FL_ONGROUND;v.groundentity=desired;movevars.physicsmode=PHYSMODE_SOURCE;held=conveyor_arms=0;
}
int main(void){
 setup(0);native_zero_command();CHECK(v.groundentity==0&&(int)v.flags&FL_ONGROUND);CHECK(!conveyor_arms);
 setup(42);native_zero_command();CHECK(v.groundentity==42&&pmove.groundent==2);CHECK(!conveyor_arms);
 setup(0);pmove.groundent=123456;native_zero_command();CHECK(v.groundentity==0&&pmove.groundent==0);CHECK(!conveyor_arms);
 setup(99);native_zero_command();CHECK(!pmove.onground&&!((int)v.flags&FL_ONGROUND)&&pmove.groundent==-1);CHECK(!conveyor_arms);
 setup(42);held=1;native_zero_command();CHECK(pmove.groundent==1); /* held path not changed */
 setup(42);pmove.cmd.msec=8;native_zero_command();CHECK(pmove.groundent==1); /* ordinary tick path not changed */
 setup(42);pmove.pm_type=2;native_zero_command();CHECK(pmove.groundent==1);
 setup(42);movevars.physicsmode=0;native_zero_command();CHECK(pmove.groundent==1);
 puts("PASS native zero-command ground seed/caller/writeback: world, moving support, stale prior-client index, missing support, unrelated conveyor refusal, scoped non-fence paths");
 return 0;
}
'''
AG=r'''
#define main retained_world_checks
#include "__FIXTURE__"
#undef main
__CARRY__
static struct {float flags,waterlevel,watertype;unsigned int groundentity;} body;
static struct {typeof(body)*v;} actor={&body},*test_player=&actor;
static int held;
#define test_vm 0
#undef PROG_TO_EDICT
#undef NUM_FOR_EDICT
#undef EDICT_NUM_PB
#undef EDICT_TO_PROG
#define PROG_TO_EDICT(vm,e) (e)
#define NUM_FOR_EDICT(vm,e) (e)
#define EDICT_NUM_PB(vm,e) (e)
#define EDICT_TO_PROG(vm,e) (e)
__HELPER__
static void server_call(void) {
 __CALLER__
 PMAg_PlayerMove(1);
 __WATERBACK__
 __WRITEBACK__
}
int main(void) {
 int rate,duration,stance,wet,support,prior;
 for(rate=0;rate<2;rate++) for(duration=0;duration<2;duration++)
 for(stance=0;stance<3;stance++) for(wet=0;wet<2;wet++)
 for(support=0;support<3;support++) for(prior=0;prior<3;prior++) {
  pmsourcestate_t b,after;
  vec3_t origin,velocity;
  float carry;int oldtouch;
  Reset();waterz=2000;forcevolume=true;
  movevars.ticrate=rate?0.004f:0.01f;movevars.agprofile=rate?250:100;
  pmove.origin[2]=1000;pmove.cmd.msec=rate?4:10;
  PMAg_PlayerMove(1);CHECK(!pmove.onground&&pmove.groundent==-1&&pmove.waterlevel==3);
  /* Only the real carried state and the fields assigned by the server change
   * actors. Prior global output poisoning is intentional; no pmove snapshot. */
  memset(&b,0,sizeof(b));b.surfacefriction=0.7f;b.ducked=stance==1;b.ducking=stance==2;
  b.ducktime=200;b.agwaterjump=true;b.waterjumpremaining=1.5f;
  b.waterjumpdir[0]=50;b.waterjumpdir[1]=-5;b.oldbuttons=BUTTON_DUCK;
  b.msec_carry=duration?movevars.ticrate*0.25f:0;
  PMSrc_LoadState(&b);
  VectorSet(pmove.origin,wet?10:110,7,36);VectorSet(pmove.velocity,31,2,0);
  VectorCopy(pmove.origin,origin);VectorCopy(pmove.velocity,velocity);
  pmove.pm_type=PM_NORMAL;pmove.onground=true;pmove.cmd.msec=duration;
  pmove.cmd.buttons=BUTTON_JUMP;held=prior==2;
  pmove.groundent=prior==0?-1:prior==1?123:1;
  pmove.numphysent=3;pmove.physents[0].info=0;
  pmove.physents[1].info=7;pmove.physents[2].info=42;
  if(duration){pmove.physents[1].info=42;pmove.physents[2].info=7;}
  body.flags=FL_ONGROUND;body.groundentity=support==0?0:support==1?42:99;
  waterz=50;oldtouch=touches;carry=b.msec_carry+duration*0.001f;
  server_call();PMSrc_SaveState(&after);
  CHECK(pmove.ticksrun==0&&touches==oldtouch);
  CHECK(!memcmp(origin,pmove.origin,sizeof(origin))&&!memcmp(velocity,pmove.velocity,sizeof(velocity)));
  CHECK(after.ducktime==b.ducktime&&after.waterjumpremaining==b.waterjumpremaining&&after.agwaterjump);
  CHECK(after.oldbuttons==b.oldbuttons&&after.waterjumpdir[0]==50&&after.waterjumpdir[1]==-5);
  NEAR(after.msec_carry,carry,0.0000001f);
  CHECK(body.waterlevel==(wet?(stance==0?2:3):0));
  CHECK(body.watertype==(wet?Q1CONTENTS_WATER:Q1CONTENTS_EMPTY));
  if(support<2){CHECK(pmove.groundent>=0&&pmove.groundent<pmove.numphysent);CHECK((int)body.flags&FL_ONGROUND);CHECK(body.groundentity==(support==0?0:42));}
  else CHECK(!pmove.onground&&!((int)body.flags&FL_ONGROUND)&&pmove.groundent==-1);
 }
 puts("PASS AG production-shaped carry/server caller/writeback: 216 zero/subtick actor cases; support reordered/missing/prior-airborne, wet/dry posture matrix, no simulation/timers/contact/tick credit");
 return 0;
}
'''
def main():
 p=argparse.ArgumentParser();p.add_argument('--cc',default='gcc');p.add_argument('--work-dir',type=pathlib.Path);p.add_argument('--source-file',type=pathlib.Path);p.add_argument('--ag-isolation',action='store_true');p.add_argument('--world-source',type=pathlib.Path);a=p.parse_args()
 source=(a.source_file or ROOT/'engine/server/sv_user.c').read_text()
 sig='static void SV_SeedPlayerGround(' if 'static void SV_SeedPlayerGround(' in source else 'static void SV_SeedZeroSourceGround('
 helper=function(source,sig) if sig in source else ''
 start=source.index('AddAllLinksToPmove (&sv.world, (wedict_t*)sv_player);')+len('AddAllLinksToPmove (&sv.world, (wedict_t*)sv_player);')
 end=source.index('if ((int)sv_player->xv->pmove_flags & PMF_LADDER)',start)
 caller=source[start:end].strip() if helper else ''
 if helper:
  assert caller,'new helper must act on supported live caller'
  assert source.index('AddAllLinksToPmove (&sv.world, (wedict_t*)sv_player);')<source.index(caller)<source.index('SV_HeldSourceBoundary(host_client, HELD_MOVER_ENTER);',source.index(caller))
 assign=source.index('sv_player->v->groundentity = EDICT_TO_PROG(svprogfuncs, EDICT_NUM_PB(svprogfuncs, pmove.physents[pmove.groundent].info));')
 start=source.rfind('if (pmove.onground)',0,assign);end=source.index('sv_player->v->flags = (int)sv_player->v->flags & ~FL_ONGROUND;',assign)+len('sv_player->v->flags = (int)sv_player->v->flags & ~FL_ONGROUND;')
 c=STUB.replace('__HELPER__',helper).replace('__CALLER__',caller).replace('__WRITEBACK__',source[start:end])
 with tempfile.TemporaryDirectory(prefix='native-ground-',dir=a.work_dir) as temp:
  d=pathlib.Path(temp);s=d/'check.c';exe=d/('check.exe' if os.name=='nt' else 'check');s.write_text(c)
  subprocess.run([a.cc,'-std=gnu99','-Wall','-Wextra','-Werror',str(s),'-o',str(exe)],check=True,timeout=30);subprocess.run([str(exe)],check=True,timeout=10)
  if a.ag_isolation:
   carry=(ROOT/'engine/common/pm_source.c').read_text()
   water_start=source.index('sv_player->v->waterlevel = pmove.waterlevel;')
   water_end=source.index('if (pmove.jump_held',water_start)
   bind=lambda text:text.replace('sv_player','test_player').replace('svprogfuncs','test_vm')
   ag=AG.replace('__FIXTURE__',(ROOT/'tools/test_pm_ag_world.c').as_posix()).replace('__CARRY__',function(carry,'void PMSrc_SaveState (')+'\n'+function(carry,'void PMSrc_LoadState (')).replace('__HELPER__',helper).replace('__CALLER__',bind(caller)).replace('__WRITEBACK__',bind(source[start:end])).replace('__WATERBACK__',bind(source[water_start:water_end]))
   s.write_text(ag)
   argv=[a.cc,'-std=gnu99','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-ffp-contract=off','-I'+str(ROOT/'engine/client'),'-I'+str(ROOT/'engine/common'),str(s),str(ROOT/'engine/common/pm_ag.c'),str(a.world_source or ROOT/'engine/common/pm_ag_world.c'),'-lm','-o',str(exe)]
   print('AG isolation compile:',argv,flush=True)
   subprocess.run(argv,check=True,timeout=30);subprocess.run([str(exe)],check=True,timeout=10)
if __name__=='__main__':main()
