#!/usr/bin/env python3
"""Exercise cl_pred.c's actual return-fence helpers in an isolated C harness.

This is not a full client build or a dedicated-socket prediction playtest.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


STUBS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#define MAX_SPLITS 2
#define UPDATE_BACKUP 64
#define UPDATE_MASK 63
#define ca_active 4
#define CP_QUAKEWORLD 1
#define PEXT2_VRINPUTS 1
#define PEXT2_REPLACEMENTDELTAS 2
#define CAM_FREECAM 0
#define IM_NONE 0
#define MLS_LOADED 1
#define PHYSMODE_SOURCE 1
#define PM_NORMAL 0
#define STAT_SOURCE_RETURN_REQUEST 202
#define STAT_SOURCE_RETURN_SEED 203
#define true 1
#define false 0
typedef int qboolean;
typedef float vec3_t[3];
#define VectorCopy(a,b) memcpy((b),(a),sizeof(vec3_t))
typedef struct {float stamina, msec_carry; int landticks, oldbuttons; char extra[80];} pmsourcestate_t;
typedef struct {pmsourcestate_t pmsrc; int jump_held, jump_msec, messagenum, pm_type, onground;
 float waterjumptime; vec3_t gravitydir, velocity, origin;} player_state_t;
typedef struct {unsigned int sequence; float msec; unsigned long long buttons, impulse;
 int forwardmove, sidemove, upmove;} usercmd_t;
typedef struct {int number; player_state_t net;} entity_state_t;
typedef struct {int num_entities; entity_state_t entities[2]; double servertime;} packet_entities_t;
typedef struct {int frameid, invalid, ackframe; player_state_t playerstate[2]; packet_entities_t packet_entities;} inframe_t;
typedef struct {int cmd_sequence; usercmd_t cmd[MAX_SPLITS];} outframe_t;
typedef struct {int playernum, viewentity, spectator, cam_state, cam_spec_track; float statsf[256];
 struct {pmsourcestate_t pmsrc; int sequence, onground; vec3_t gravitydir, velocity, origin;} prop;
 struct {int owner, userid, request, completed, seedframe, minack;} source_return;} playerview_t;
static struct {int loadstate;} world;
static struct {int splitclients, allocated_client_slots, intermissionmode, paused;
 int validsequence, ackedmovesequence, movesequence; typeof(world) *worldmodel;
 struct {int userid;} players[2]; playerview_t playerview[MAX_SPLITS];
 inframe_t inframes[UPDATE_BACKUP]; outframe_t outframes[UPDATE_BACKUP];} cl;
static struct {int state, protocol, demoplayback, fteprotocolextensions2;
 struct {int incoming_sequence;} netchan;} cls;
static struct {int physicsmode;} movevars;
static int flushes, diagnostics;
static void CSQC_PredictBaseVelResetSeat(int seat) {if (seat == 0) flushes++;}
static void Con_DPrintf(const char *format, ...) {(void)format; diagnostics++;}
// Stand-in for network entity conversion; preserve its actual retained jumpheld contract.
static void CL_EntStateToPlayerState(player_state_t *out, entity_state_t *entity)
{int held=out->jump_held; *out=entity->net; out->jump_held=held;}
#define CHECK(x) do {if (!(x)) {fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);}} while(0)
'''

HARNESS = r'''
static void setup(void)
{
 memset(&cl,0,sizeof(cl)); memset(&cls,0,sizeof(cls)); flushes=diagnostics=0;
 cls.state=ca_active; cls.protocol=CP_QUAKEWORLD; cls.netchan.incoming_sequence=20;
 cl.validsequence=20; cl.ackedmovesequence=102; cl.movesequence=104;
 cl.splitclients=1; cl.allocated_client_slots=2; world.loadstate=MLS_LOADED; cl.worldmodel=&world;
 movevars.physicsmode=PHYSMODE_SOURCE;
 cl.playerview[0].viewentity=1; cl.playerview[0].cam_spec_track=-1;
 cl.players[0].userid=42; cl.playerview[0].statsf[202]=7; cl.playerview[0].statsf[203]=100;
 memset(&cl.playerview[0].prop.pmsrc,0x3f,sizeof(pmsourcestate_t));
 for(int i=0;i<UPDATE_BACKUP;i++)
  for(int p=0;p<2;p++) {memset(&cl.inframes[i].playerstate[p].pmsrc,0x3f,sizeof(pmsourcestate_t));
   cl.inframes[i].playerstate[p].jump_held=1; cl.inframes[i].playerstate[p].jump_msec=8;
   cl.inframes[i].playerstate[p].waterjumptime=9;}
 cl.inframes[20].frameid=20; cl.inframes[20].ackframe=102;
 cl.inframes[20].playerstate[0].messagenum=20;
 for(int i=100;i<=102;i++) {cl.outframes[i&UPDATE_MASK].cmd_sequence=i;
  cl.outframes[i&UPDATE_MASK].cmd[0].sequence=i;}
 CHECK(CL_SourceReturnOwner(0));
}
static void register_request(void) {CHECK(!CL_SourcePracticePredictReset(0,7,0));}
static void refused_unchanged(void)
{
 unsigned char before[sizeof(cl)]; memcpy(before,&cl,sizeof(cl));
 CHECK(!CL_SourcePracticePredictReset(0,7,100)); CHECK(!memcmp(before,&cl,sizeof(cl)));
 CHECK(flushes==0 && diagnostics==0);
}
int main(void)
{
 pmsourcestate_t zero={0};
 setup(); refused_unchanged(); // no registration
 register_request(); CHECK(flushes==0); // positive request alone must not act
 CHECK(CL_SourcePracticePredictReset(0,7,100));
 CHECK(flushes==1 && diagnostics==1); CHECK(cl.playerview[0].source_return.minack==102);
 CHECK(cl.playerview[0].prop.sequence==102);
 CHECK(!memcmp(&zero,&cl.playerview[0].prop.pmsrc,sizeof(zero)));
 for(int i=0;i<UPDATE_BACKUP;i++) {
  CHECK(!memcmp(&zero,&cl.inframes[i].playerstate[0].pmsrc,sizeof(zero)));
  CHECK(cl.inframes[i].playerstate[0].jump_held==0 && cl.inframes[i].playerstate[0].jump_msec==0);
  CHECK(cl.inframes[i].playerstate[0].waterjumptime==0);
  CHECK(cl.inframes[i].playerstate[1].jump_held==1);
  CHECK(memcmp(&zero,&cl.inframes[i].playerstate[1].pmsrc,sizeof(zero)));
 }
 CHECK(cl.outframes[100&UPDATE_MASK].cmd_sequence==100 && cl.movesequence==104);
 cl.playerview[0].prop.pmsrc.stamina=99; cl.playerview[0].statsf[202]=-7;
 CHECK(CL_SourcePracticePredictReset(0,7,100)); CHECK(cl.playerview[0].prop.pmsrc.stamina==99);
 CHECK(flushes==1 && diagnostics==1);
 // Persistent seed selection fence: both command identity and ack lower bound.
 CHECK(CL_SourceReturnFrameAllowed(&cl.playerview[0],20));
 cl.inframes[19].frameid=19; cl.inframes[19].ackframe=101;
 CHECK(!CL_SourceReturnFrameAllowed(&cl.playerview[0],19));
 cl.inframes[20].invalid=1; CHECK(!CL_SourceReturnFrameAllowed(&cl.playerview[0],20));
 // Actual absolute-frame helper with the former no-prediction CALLER arguments:
 // post-wrap slots are refused even though both real endpoint frames are valid.
 setup(); register_request(); CHECK(CL_SourcePracticePredictReset(0,7,100));
 cl.validsequence=cls.netchan.incoming_sequence=999;
 cl.inframes[998&UPDATE_MASK].frameid=998;cl.inframes[998&UPDATE_MASK].ackframe=102;
 cl.inframes[999&UPDATE_MASK].frameid=999;cl.inframes[999&UPDATE_MASK].ackframe=102;
 cl.inframes[998&UPDATE_MASK].packet_entities.servertime=9.98;
 cl.inframes[999&UPDATE_MASK].packet_entities.servertime=9.99;
 CHECK(CL_SourceReturnFrameAllowed(&cl.playerview[0],998)&&CL_SourceReturnFrameAllowed(&cl.playerview[0],999));
 CHECK(!CL_SourceReturnFrameAllowed(&cl.playerview[0],998&UPDATE_MASK)); // concrete old-caller counterexample
 int from=CL_SourceReturnEndpointFrame(998&UPDATE_MASK,true);
 int to=CL_SourceReturnEndpointFrame(999&UPDATE_MASK,true);
 CHECK(from==998&&to==999&&from!=to);
 CHECK(CL_SourceReturnFrameAllowed(&cl.playerview[0],from)&&CL_SourceReturnFrameAllowed(&cl.playerview[0],to));
 CHECK(cl.inframes[from&UPDATE_MASK].packet_entities.servertime<cl.inframes[to&UPDATE_MASK].packet_entities.servertime);
 CHECK(CL_SourceReturnEndpointFrame(998,false)==998);
 setup(); register_request(); cl.playerview[0].statsf[202]=0; refused_unchanged();
 setup(); register_request(); cl.playerview[0].statsf[202]=-7; refused_unchanged();
 setup(); register_request(); cl.playerview[0].statsf[203]=99; refused_unchanged();
 setup(); register_request(); cl.inframes[20].invalid=1; refused_unchanged();
 setup(); register_request(); cl.inframes[20].frameid=19; refused_unchanged();
 setup(); register_request(); cl.inframes[20].ackframe=101; refused_unchanged();
 setup(); register_request(); cl.inframes[20].playerstate[0].messagenum=19; refused_unchanged();
 setup(); register_request(); cl.inframes[20].playerstate[0].pm_type=1; refused_unchanged();
 setup(); register_request(); cl.outframes[101&UPDATE_MASK].cmd_sequence=99; refused_unchanged();
 setup(); register_request(); cl.outframes[101&UPDATE_MASK].cmd[0].sequence=99; refused_unchanged();
 setup(); register_request(); cl.outframes[100&UPDATE_MASK].cmd[0].msec=1; refused_unchanged();
 setup(); register_request(); cl.outframes[101&UPDATE_MASK].cmd[0].buttons=1; refused_unchanged();
 setup(); register_request(); cl.outframes[102&UPDATE_MASK].cmd[0].impulse=1; refused_unchanged();
 setup(); register_request(); cl.outframes[101&UPDATE_MASK].cmd[0].forwardmove=1; refused_unchanged();
 setup(); register_request(); cl.outframes[101&UPDATE_MASK].cmd[0].sidemove=1; refused_unchanged();
 setup(); register_request(); cl.outframes[101&UPDATE_MASK].cmd[0].upmove=1; refused_unchanged();
 setup(); register_request(); cl.ackedmovesequence=cl.inframes[20].ackframe=164; refused_unchanged();
 setup(); register_request(); cl.playerview[0].source_return.minack=100; refused_unchanged();
 setup(); CHECK(!CL_SourcePracticePredictReset(0,NAN,0)); CHECK(!CL_SourcePracticePredictReset(0,7,NAN));
 setup(); CHECK(!CL_SourcePracticePredictReset(0,7.5f,0)); CHECK(!CL_SourcePracticePredictReset(0,16000001,0));
 setup(); cl.splitclients=2; CHECK(!CL_SourcePracticePredictReset(0,7,0));
 setup(); cls.demoplayback=1; CHECK(!CL_SourcePracticePredictReset(0,7,0));
 setup(); cl.playerview[0].spectator=1; CHECK(!CL_SourcePracticePredictReset(0,7,0));
 setup(); cl.playerview[0].viewentity=2; CHECK(!CL_SourcePracticePredictReset(0,7,0));
 setup(); register_request(); cl.players[0].userid++; CHECK(!CL_SourcePracticePredictReset(0,7,100));
 setup(); register_request(); cls.fteprotocolextensions2=PEXT2_REPLACEMENTDELTAS;
 refused_unchanged(); // own replacement-delta entity is mandatory
 cl.inframes[20].packet_entities.num_entities=1;
 cl.inframes[20].packet_entities.entities[0].number=1;
 CHECK(CL_SourcePracticePredictReset(0,7,100));
 CHECK(!cl.inframes[20].playerstate[0].jump_held);
 puts("PASS sourcepractice prediction reset: registration, neutral proof, refusal atomicity, epoch fence, idempotence, ownership, replacement entity");
 return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', required=True)
    parser.add_argument('--work-dir', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = (root / 'engine/client/cl_pred.c').read_text()
    signatures = ['static qboolean CL_SourceReturnOwner(',
                  'static int CL_SourceReturnEndpointFrame(',
                  'static qboolean CL_SourceReturnFrameAllowed(',
                  'static void CL_SourceReturnClearCarry(',
                  'qboolean CL_SourcePracticePredictReset(']
    # The lower-bound selection and phase guard must remain wired into the real paths.
    prediction = function(source, 'void CL_PredictMovePNum (')
    assert 'out < pv->source_return.minack' in prediction
    assert 'from.frame = CL_SourceReturnEndpointFrame(from.frame, nopred)' in prediction
    assert 'to.frame = CL_SourceReturnEndpointFrame(to.frame, nopred)' in prediction
    assert prediction.index('from.frame = CL_SourceReturnEndpointFrame(from.frame, nopred)') < prediction.index('!CL_SourceReturnFrameAllowed(pv, from.frame)')
    assert '!CL_SourceReturnFrameAllowed(pv, from.frame)' in prediction
    assert prediction.index('!CL_SourceReturnFrameAllowed(pv, from.frame)') < prediction.index('if (pv->nolocalplayer)')
    assert 'sequence <= cl.playerview[seat].source_return.minack' in function(source, 'static void CL_PredictSourcePhase(')
    with tempfile.TemporaryDirectory(prefix='source-return-test-', dir=args.work_dir) as tmp:
        c = Path(tmp) / 'test.c'
        exe = Path(tmp) / 'test.exe'
        c.write_text(STUBS + '\n'.join(function(source, sig) for sig in signatures) + HARNESS)
        subprocess.run([args.cc, '-std=gnu99', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
