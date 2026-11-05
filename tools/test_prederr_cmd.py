#!/usr/bin/env python3
"""Compile literal client prediction/report blocks in a minimal command fixture.

This is a metadata regression, not a live-network or full-engine movement test.
All output must be directed to a new, caller-owned directory.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def extract(source, first, last):
    if source.count(first) != 1:
        raise ValueError('production anchor absent or ambiguous: ' + first)
    start = source.index(first)
    return source[start:source.index(last, start)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path,
                        default=Path(__file__).resolve().parents[1] / 'engine/client/cl_pred.c')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--expect-failures', type=int, default=0)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    raw = args.source.read_bytes()
    source = raw.decode('utf8').replace('\r\n', '\n')
    scan = extract(source, '\t\tto.frame = from.frame = 0;', '\n\t}\n\n//\tCon_DPrintf("sim%f')
    fallback = extract(source, '\tif (!from.cmd)\n', '\n\t//if our network protocol')
    remap = extract(source, '\t\tint validsequence = cl.inframes[cl.validsequence&UPDATE_MASK].ackframe;', '\n\t\tpring_firsti =')
    replay = extract(source, '\t\tfor (i=to.frame+1, stopframe=', '\n\n\t\t\tif (from.frame == pv->prop.sequence')
    marker = '\t\t\t\t\t// Diagnostic inputs belong to this acknowledged move and local seat.\n'
    if marker not in source:
        marker = '\t\t\t\t\tif (from.cmd)\n'
    ring = extract(source, marker, '\n\t\t\t\t}\n\t\t\t\tstatic float pt_ncspeed;')
    chunks = dict(scan=scan, fallback=fallback, remap=remap, replay_prefix=replay, ring_command_copy=ring)
    header = r'''
#include <stdio.h>
#include <string.h>
#define UPDATE_BACKUP 64
#define UPDATE_MASK 63
#define MAX_SPLITS 4
#define min(a,b) ((a)<(b)?(a):(b))
#define VectorCopy(a,b) do { (b)[0]=(a)[0]; (b)[1]=(a)[1]; (b)[2]=(a)[2]; } while(0)
#define VectorClear(a) memset((a),0,sizeof(a))
typedef struct { unsigned char msec; short angles[3], forwardmove, sidemove, upmove; unsigned buttons; } usercmd_t;
typedef struct { int cmd_sequence, server_message_num; double senttime; usercmd_t cmd[MAX_SPLITS]; } outframe_t;
typedef struct { int frameid, invalid, ackframe; } inframe_t;
typedef struct { int seq; float msec, ang[3], fm,sm,um; unsigned buttons; } pring_t;
typedef struct { int frame; double time; usercmd_t *cmd; void *state; } endpoint_t;
typedef struct { int playernum; struct {int minack;} source_return; struct {int sequence;} prop; } player_t;
static struct { int validsequence, movesequence; inframe_t inframes[64]; outframe_t outframes[64]; } cl;
static struct { struct { int incoming_sequence; } netchan; } cls;
static player_t player;
static int run(int single, int packetdelta, int seat, int playernum) {
 endpoint_t from,to;
 int i,stopframe; void *framebuf[2]={0}; double simtime=1000; outframe_t *backdate;
 pring_t row={0}, *r=&row; int samples=0; int packet=35, ack=packet+packetdelta;
 player_t *pv=&player;
 memset(&cl,0,sizeof(cl)); memset(&player,0,sizeof(player));
 pv->playernum=playernum; pv->prop.sequence=ack; pv->source_return.minack=single?ack:ack-1;
 cl.validsequence=packet; cls.netchan.incoming_sequence=packet; cl.movesequence=ack+2;
 for(i=0;i<64;i++) { int j; cl.inframes[i].invalid=1; cl.outframes[i].cmd_sequence=-1;
  for(j=0;j<MAX_SPLITS;j++) {
   usercmd_t *c=&cl.outframes[i].cmd[j]; c->msec=1+i+j*50;
   c->angles[0]=10+i+j*100; c->angles[1]=20+i+j*100; c->angles[2]=30+i+j*100;
   c->forwardmove=40+i+j*100; c->sidemove=50+i+j*100; c->upmove=60+i+j*100; c->buttons=70+i+j*100;
  }
 }
 for(i=0;i<2;i++) { inframe_t *in=&cl.inframes[(packet-i)&63];
  in->frameid=packet-i;in->invalid=0;in->ackframe=ack-i;
  cl.outframes[(ack-i)&63].cmd_sequence=ack-i;cl.outframes[(ack-i)&63].senttime=100-i;
 }
 cl.outframes[(ack+1)&63].cmd_sequence=ack+1;cl.outframes[(ack+1)&63].senttime=101;
'''
    trailer = r'''
 row.seq=from.frame;
 samples++;
 break;
 }
 usercmd_t *expected=&cl.outframes[ack&63].cmd[seat];
 int angles_ok=row.ang[0]==expected->angles[0] && row.ang[1]==expected->angles[1] && row.ang[2]==expected->angles[2];
 int good=samples==1 && row.seq==ack && row.msec==expected->msec && row.fm==expected->forwardmove && row.sm==expected->sidemove && row.um==expected->upmove && row.buttons==expected->buttons && angles_ok;
 printf("single=%d packetdelta=%d seat=%d player=%d seq=%d actual_msec=%.0f expected_msec=%u %s\n",single,packetdelta,seat,playernum,row.seq,row.msec,expected->msec,good?"PASS":"FAIL");
 return good;
}
static int stale(void) {
 endpoint_t from={0}; pring_t row, *r=&row; int seat=1;
 memset(&row,0x5a,sizeof(row)); memset(&cl,0,sizeof(cl));
 from.frame=9; from.cmd=&cl.outframes[9].cmd[0]; from.cmd->msec=99;
 usercmd_t *selected=&cl.outframes[9].cmd[seat];
 selected->msec=17; selected->angles[0]=31; selected->angles[1]=43; selected->angles[2]=59;
 selected->forwardmove=71; selected->sidemove=83; selected->upmove=97; selected->buttons=109;
 cl.outframes[9].cmd_sequence=8;
'''
    end = r'''
 int good=row.msec==0 && row.fm==0 && row.sm==0 && row.um==0 && row.buttons==0 && row.ang[0]==0 && row.ang[1]==0 && row.ang[2]==0;
 printf("stale_ack %s\n",good?"PASS":"FAIL");return good;
}
int main(void) { int failures=0, n=0;
 for(int single=0;single<2;single++) for(int delta=0;delta<=7;delta+=7)
 for(int seat=0;seat<2;seat++) for(int player=0;player<3;player++) { n++;failures+=!run(single,delta,seat,player); }
 n++;failures+=!stale();
 printf("SUMMARY cases=%d failures=%d\n",n,failures);return failures;
}
'''
    # The literal prefix contains from=to and the next move's setup. Stop after
    # the first comparison; later predicted-vs-predicted rows are not reports.
    code = header + '\n{\n' + scan + '\n}\n{\n' + fallback + '\n}\n' + remap + '\n' + replay + '\n' + ring + '\n' + trailer + ring + end
    cfile = args.output / 'probe.c'
    cfile.write_text(code, encoding='utf8', newline='\n')
    compiler = shutil.which(args.cc)
    if compiler is None:
        raise FileNotFoundError(args.cc)
    compiler = str(Path(compiler).resolve())
    env = os.environ.copy()
    env['PATH'] = str(Path(compiler).parent) + os.pathsep + env.get('PATH', '')
    exe = args.output / ('probe.exe' if os.name == 'nt' else 'probe')
    build = subprocess.run([compiler, '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-variable', '-o', str(exe), str(cfile)],
                           capture_output=True, text=True, env=env)
    for name, text in [('build.stdout', build.stdout), ('build.stderr', build.stderr),
                       ('build.exit', str(build.returncode)+'\n')]:
        (args.output / name).write_text(text, encoding='utf8')
    if build.returncode:
        raise RuntimeError('probe compilation failed; see retained diagnostics')
    run = subprocess.run([str(exe.resolve())], capture_output=True, text=True, env=env)
    (args.output / 'run.stdout').write_text(run.stdout, encoding='utf8')
    (args.output / 'run.stderr').write_text(run.stderr, encoding='utf8')
    accepted = (run.returncode == args.expect_failures and
                ('SUMMARY cases=25 failures=%d' % args.expect_failures) in run.stdout)
    receipt = dict(source_sha256=hashlib.sha256(raw).hexdigest(),
                   chunks_sha256={k: hashlib.sha256(v.encode()).hexdigest() for k,v in chunks.items()},
                   compiler=compiler, compiler_sha256=hashlib.sha256(Path(compiler).read_bytes()).hexdigest(),
                   compile_exit=build.returncode, run_exit=run.returncode,
                   expected_failures=args.expect_failures, accepted=accepted,
                   standalone_literal_blocks_not_full_native_runtime=True)
    (args.output / 'receipt.json').write_text(json.dumps(receipt, indent=2)+'\n', encoding='utf8')
    print(run.stdout, end='')
    return 0 if accepted else 1


if __name__ == '__main__':
    raise SystemExit(main())
