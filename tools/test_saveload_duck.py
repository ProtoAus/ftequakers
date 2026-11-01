#!/usr/bin/env python3
"""Act the real forced-posture block and PMSrc_Duck/FinishDuck in a C harness.

World tracing, hull dimensions, and ground categorization are bounded stubs.
This proves the button-edge counterexample, NOT a full engine/client playtest.
--source-file accepts the original sv_user.c for an expected failing control.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

from test_sourcepractice_predictreset import function

ROOT = Path(__file__).resolve().parents[1]
STUB = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define true 1
#define false 0
__INPUT_BITS__
#define PM_DEAD 3
#define PMSRC_DUCK_TIMER 1000.0f
#define PMSRC_TIME_TO_DUCK 0.4f
#define PMSRC_TIME_TO_UNDUCK 0.2f
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
typedef int qboolean;
typedef union {float _float;} eval_t;
typedef struct {int ducked, ducking, oldbuttons, onground, pm_type; float ducktime, origin[3], player_maxs[3]; struct {int buttons;} cmd;} movestate;
static movestate pmove;
static struct {float viewscale;} movevars;
static int pms_slidetick, evalc_forceduck, field_present=1;
static void *sv_player;
static eval_t value;
static eval_t *field(void *vm, void *player, const char *name, int type, int *cache) {
 (void)vm;(void)player;(void)name;(void)type;(void)cache;return field_present ? &value : NULL;
}
static struct {eval_t *(*GetEdictFieldValue)(void*,void*,const char*,int,int*);} api={field}, *svprogfuncs=&api;
#define ev_float 1
static float PMSrc_HullDelta(void) {return 18;}
static void PMSrc_ApplyHull(void) {pmove.player_maxs[2]=pmove.ducked ? 54 : 72;}
static void PMSrc_CategorizePosition(void) {}
static void PMSrc_HandleDuckingSpeedCrop(void) {}
static qboolean PMSrc_CanUnduck(void) {return true;}
__FUNCTIONS__
static void restore(int state) {
 value._float=(float)state;
 __RESTORE__
 CHECK(value._float==(field_present ? 0 : state));
 PMSrc_ApplyHull();
}
static void predicted_restore_flags(int state, int flags) {
 struct {int flags,forceduck;} in={flags,state}, *input=&in;
 movestate *move=&pmove;
 __PREINPUT__
 PMSrc_ApplyHull();
}
static void predicted_restore(int state) {predicted_restore_flags(state,PMPI_DUCK);}
static void setup(int grounded, int held) {
 memset(&pmove,0,sizeof(pmove));field_present=1;
 pmove.onground=grounded;pmove.origin[2]=100;movevars.viewscale=0.5f;
 pmove.ducked=held;pmove.oldbuttons=BUTTON_JUMP|BUTTON_ATTACK|(held?BUTTON_DUCK:0);
 pmove.cmd.buttons=BUTTON_JUMP|BUTTON_DUCK;
 PMSrc_ApplyHull();
}
static void step(void) {
 if(pmove.ducktime>0)pmove.ducktime-=8;
 PMSrc_Duck();
}
int main(void) {
 float origin[2], height[2];int held,ground,pred;movestate before;
 setup(0,1);before=pmove;field_present=0;restore(1);
 CHECK(!memcmp(&before,&pmove,sizeof(pmove)));field_present=1; /* absent optional field is inert */
 setup(0,1);before=pmove;predicted_restore_flags(1,0);
 CHECK(!memcmp(&before,&pmove,sizeof(pmove))); /* unselected duck payload is inert */
 for(pred=0;pred<2;pred++)
 for(ground=0;ground<2;ground++) {
  for(held=0;held<2;held++) {
   setup(ground,held);if(pred)predicted_restore(1);else restore(1);
   CHECK((pmove.oldbuttons&(BUTTON_JUMP|BUTTON_ATTACK))==(BUTTON_JUMP|BUTTON_ATTACK)); /* preserve unrelated history */
   for(int i=0;i<64;i++)step();
   CHECK(pmove.ducked);
   origin[held]=pmove.origin[2];height[held]=pmove.player_maxs[2];
  }
  CHECK(origin[0]==origin[1]&&height[0]==height[1]);
  CHECK(origin[0]==(ground?100:109)&&height[0]==54);
 }
 setup(0,1);restore(2);step();CHECK(pmove.origin[2]==100&&pmove.ducked); /* no second crouch shift */
 pmove.cmd.buttons=BUTTON_JUMP;step();CHECK(pmove.origin[2]==91&&!pmove.ducked); /* ordinary air unduck */
 puts("PASS production input bits, absent/unselected restore, authoritative + predicted forced-posture and native Duck/FinishDuck: held-before vs pressed-after, ground/air, other latches, crouched restore, release");
 return 0;
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='C:/msys64/ucrt64/bin/gcc.exe')
    parser.add_argument('--source-file', type=Path)
    parser.add_argument('--preinput-source-file', type=Path)
    parser.add_argument('--work-dir', type=Path)
    args = parser.parse_args()
    sv = (args.source_file or ROOT / 'engine/server/sv_user.c').read_text()
    pm = (ROOT / 'engine/common/pm_source.c').read_text()
    marker = 'eval_t *ev = svprogfuncs->GetEdictFieldValue(svprogfuncs, sv_player, "run_forceduck"'
    restore = function(sv, marker)
    functions = '\n'.join(function(pm, sig) for sig in (
        'static float PMSrc_AirDuckShift (void)',
        'static void PMSrc_FinishDuck (void)',
        'static void PMSrc_FinishUnDuck (void)',
        'static void PMSrc_Duck (void)'))
    preinput = (args.preinput_source_file or ROOT / 'engine/common/pmove.c').read_text()
    branch = function(preinput, 'if (input->flags & PMPI_DUCK)')
    header = (ROOT / 'engine/common/pmove.h').read_text()
    bits = []
    for name in ('BUTTON_ATTACK', 'BUTTON_JUMP', 'BUTTON_DUCK', 'PMPI_DUCK'):
        value = re.search(r'^#define\s+' + name + r'\s+([0-9]+)\b', header, re.M)
        if value is None:
            raise RuntimeError('production input constant is not a numeric definition: ' + name)
        bits.append('#define ' + name + ' ' + value.group(1))
    source = STUB.replace('__INPUT_BITS__', '\n'.join(bits)).replace('__FUNCTIONS__', functions).replace('__RESTORE__', restore).replace('__PREINPUT__', branch)
    with tempfile.TemporaryDirectory(prefix='save-duck-', dir=args.work_dir) as folder:
        folder = Path(folder)
        cfile, exe = folder / 'check.c', folder / 'check.exe'
        cfile.write_text(source)
        subprocess.run([args.cc, '-std=c99', '-Wall', '-Wextra', '-Werror', str(cfile), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
