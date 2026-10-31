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
def main():
 p=argparse.ArgumentParser();p.add_argument('--cc',default='gcc');p.add_argument('--work-dir',type=pathlib.Path);p.add_argument('--source-file',type=pathlib.Path);a=p.parse_args()
 source=(a.source_file or ROOT/'engine/server/sv_user.c').read_text()
 sig='static void SV_SeedZeroSourceGround('
 helper=function(source,sig) if sig in source else ''
 caller=re.search(r'if \(movevars.physicsmode == PHYSMODE_SOURCE && !held && pmove.pm_type == PM_NORMAL &&\s*pmove.cmd.msec == 0 && pmove.onground\)\s*SV_SeedZeroSourceGround\(NUM_FOR_EDICT\(svprogfuncs,\s*PROG_TO_EDICT\(svprogfuncs, sv_player->v->groundentity\)\)\);',source)
 caller=caller.group(0) if caller else ''
 if helper:
  assert caller,'new helper must act on supported live caller'
  assert source.index('AddAllLinksToPmove (&sv.world, (wedict_t*)sv_player);')<source.index(caller)<source.index('SV_HeldSourceBoundary(host_client, HELD_MOVER_ENTER);',source.index(caller))
 assign=source.index('sv_player->v->groundentity = EDICT_TO_PROG(svprogfuncs, EDICT_NUM_PB(svprogfuncs, pmove.physents[pmove.groundent].info));')
 start=source.rfind('if (pmove.onground)',0,assign);end=source.index('sv_player->v->flags = (int)sv_player->v->flags & ~FL_ONGROUND;',assign)+len('sv_player->v->flags = (int)sv_player->v->flags & ~FL_ONGROUND;')
 c=STUB.replace('__HELPER__',helper).replace('__CALLER__',caller).replace('__WRITEBACK__',source[start:end])
 with tempfile.TemporaryDirectory(prefix='native-ground-',dir=a.work_dir) as temp:
  d=pathlib.Path(temp);s=d/'check.c';exe=d/('check.exe' if os.name=='nt' else 'check');s.write_text(c)
  subprocess.run([a.cc,'-std=gnu99','-Wall','-Wextra','-Werror',str(s),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
if __name__=='__main__':main()
