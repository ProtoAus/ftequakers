#!/usr/bin/env python3
"""Compile literal prediction-diagnostic helpers and dump writer, then read its bytes.

Bounded saved-state/format test, not native lifecycle/network or historical cause.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess


def braces(text, start):
    p = text.index('{', start)
    depth = 1
    q = p + 1
    while depth:
        depth += (text[q] == '{') - (text[q] == '}')
        q += 1
    return text[start:q]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1] / 'engine/client/cl_pred.c')
    ap.add_argument('--reader', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--cc', default='gcc')
    a = ap.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    raw = a.source.read_bytes()
    text = raw.decode('utf8').replace('\r\n', '\n')
    header = a.source.with_name('client.h').read_text(encoding='utf8').replace('\r\n', '\n')
    prop = braces(header, header.index('struct playerpredprop_s')) + ' prop;'
    end = text.index('} pring_t;') + len('} pring_t;')
    ring = text[text.rfind('typedef struct', 0, end):end]
    se = text.index('} pred_diag_seed_t;') + len('} pred_diag_seed_t;')
    seedtype = text[text.rfind('typedef struct', 0, se):se]
    names = ['CL_PredictionDiagAnchor', 'CL_PredictionDiagKeep', 'CL_PredictionDiagCopy', 'CL_CacheSourcePhase', 'CL_PredictSourcePhase']
    parts = {name: braces(text, text.index(('void ' if name == 'CL_CacheSourcePhase' else 'static qboolean ' if name == 'CL_PredictSourcePhase' else 'static void ') + name + '(')) for name in names}
    counter = braces(text, text.index('\tif (cl_pred_localchain)\n\t{\n\t\tcl_pred_diag_parts++;'))
    reset_anchor = '\tif (cl_pred_localchain)\n\t\tcl_pred_diag_parts = cl_pred_diag_ticks = 0;'
    assert text.count(reset_anchor) == 1
    advance = '\t\t\tdiag.parts += cl_pred_diag_parts;\n\t\t\tdiag.ticks += cl_pred_diag_ticks;'
    assert text.count(advance) == 1
    dump = text.index('static void CL_PredErrDump_f(')
    hs = text.index('\tVFS_PRINTF(f, "FTESURF-PREDRING', dump)
    header_writer = text[hs:text.index(');', hs)+2]
    rs = text.index('\t\tVFS_PRINTF(f, "%.4f %i ', hs)
    row_writer = text[rs:text.index(');', rs)+2]
    prefix = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define IS_NAN(x) isnan(x)
#define MAX_SPLITS 4
#define UPDATE_MASK 63
#define STAT_FS_SOURCEPHASE 179
#define PHYSMODE_SOURCE 1
#define true 1
#define false 0
#define SHORT2ANGLE(x) ((x)*(360.0/65536.0))
#define VFS_PRINTF fprintf
typedef int qboolean;
typedef unsigned char qbyte;
typedef float vec3_t[3];
typedef struct {float msec_carry;} pmsourcestate_t;
'''
    structs = '\ntypedef struct {\n' + prop + r'''
 int playernum;struct{int minack;}source_return;const char *statsstr[256];
} playerview_t;
typedef struct {pmsourcestate_t pmsrc;} player_state_t;
static struct {playerview_t playerview[4];int serverinfo;struct {unsigned sequence,owner;float carry;int valid;} sourcephase[4][64];} cl;
static struct {float ticrate;int physicsmode;} movevars={.ticrate=.015f,.physicsmode=1};
static int advertised=1;
static const char *InfoBuf_ValueForKey(const void *p,const char *key) {(void)p;(void)key;return advertised?"1":"";}
static int cl_pred_localchain,cl_pred_diag_parts,cl_pred_diag_ticks;
static struct{int ticksrun;}pmove;
static const char *version_string(void) {return "fixture";}
'''
    wrappers = '\nstatic void advance_chain(pred_diag_seed_t *out) {pred_diag_seed_t diag=*out;\n' + advance + '\n*out=diag;}\nstatic void reset_parts(void) {\n' + reset_anchor + '\n}\nstatic void count_part(void) {\n' + counter + '\n}\nstatic void write_header(FILE *f) {int n=2,seat=0;double realtime=2;\n' + header_writer + '\n}\nstatic void write_row(FILE *f,const pring_t *r) {\n' + row_writer + '\n}\n'
    tests = r'''
static int checks=0,failed=0;
static void check(int pass,const char *name){checks++;failed+=!pass;printf("%s %s\n",pass?"PASS":"FAIL",name);}
int main(int argc,char **argv) {
 if(argc!=2)return 99;
 playerview_t *pv=&cl.playerview[0];pring_t r={0};player_state_t state={0};pv->playernum=0;
 pv->statsstr[179]="1:44:0:0.014";
 check(CL_PredictSourcePhase(0,44,&state)&&fabsf(state.pmsrc.msec_carry-.014f)<1e-8f,"actual cache/lookup returns exact owner/sequence phase hit");
 state.pmsrc.msec_carry=.002f;
 check(!CL_PredictSourcePhase(0,45,&state)&&state.pmsrc.msec_carry==.002f,"missing sequence preserves supplied seed and reports miss");
 pv->source_return.minack=44;state.pmsrc.msec_carry=0;
 check(!CL_PredictSourcePhase(0,44,&state)&&state.pmsrc.msec_carry==0,"neutral prepared seed ignores stale phase tag");pv->source_return.minack=0;
 pv->statsstr[179]="2:46:0:0.013";
 check(!CL_PredictSourcePhase(0,46,&state),"wrong owner cannot supply phase");
 advertised=0;check(!CL_PredictSourcePhase(0,44,&state),"unadvertised phase is unknown");advertised=1;
 movevars.physicsmode=0;check(!CL_PredictSourcePhase(0,44,&state),"non Source phase is unknown");movevars.physicsmode=1;
 cl_pred_localchain=1;pmove.ticksrun=1;count_part();count_part();
 check(cl_pred_diag_parts==2&&cl_pred_diag_ticks==2,"literal counter records two actual native parts/ticks");
 cl_pred_localchain=0;pmove.ticksrun=19;count_part();
 check(cl_pred_diag_parts==2&&cl_pred_diag_ticks==2,"other-player chain cannot contaminate local counters");
 reset_parts();check(cl_pred_diag_parts==2&&cl_pred_diag_ticks==2,"other-player wrapper does not reset active local counters");
 cl_pred_localchain=1;reset_parts();check(!cl_pred_diag_parts&&!cl_pred_diag_ticks,"next local command resets part and tick counts");
 pred_diag_seed_t anchor={0};CL_PredictionDiagAnchor(&anchor,99,.014f,true);
 cl_pred_diag_parts=cl_pred_diag_ticks=2;advance_chain(&anchor);
 CL_PredictionDiagAnchor(&anchor,100,0,false);cl_pred_diag_parts=cl_pred_diag_ticks=1;advance_chain(&anchor);
 check(anchor.sequence==99&&anchor.carry==.014f&&anchor.phasehit==1&&anchor.parts==3&&anchor.ticks==3,"first replay anchor survives later commands; counts cover whole saved chain");
 CL_PredictionDiagKeep(pv,101,0,anchor.sequence,anchor.carry,anchor.phasehit,anchor.parts,anchor.ticks);
 // Event-time capture must survive later cache/state changes before dump/copy.
 cl.sourcephase[0][99&63].valid=0;cl.sourcephase[0][99&63].carry=0;state.pmsrc.msec_carry=0;
 CL_PredictionDiagCopy(&r,pv,101,true);
 check(r.cmdvalid&&r.predvalid&&r.predpm==0&&r.seedseq==99&&fabsf(r.seedcarry-.014f)<1e-8f&&r.phasehit==1&&r.parts==3&&r.ticks==3,"saved prediction provenance is not sampled from later state");
 FILE *f=fopen(argv[1],"wb");if(!f)return 98;
 write_header(f);r.t=1;r.seq=101;r.msec=15;r.sorg[0]=9;r.pmtype=2;write_row(f,&r);
 r.predvalid=r.cmdvalid=1;r.predpm=r.seedseq=r.phasehit=r.parts=r.ticks=23;r.seedcarry=.014f;
 CL_PredictionDiagCopy(&r,pv,102,false);
 check(!r.cmdvalid&&!r.predvalid&&!r.predpm&&!r.seedseq&&!r.seedcarry&&!r.phasehit&&!r.parts&&!r.ticks,"mismatched prediction sequence clears every provenance field");
 r.seq=102;r.t=1.015;write_row(f,&r);fclose(f);
 CL_PredictionDiagKeep(pv,103,0,102,NAN,false,1,1);CL_PredictionDiagCopy(&r,pv,103,true);
 check(r.cmdvalid&&!r.predvalid&&!r.seedcarry&&!r.parts&&!r.ticks,"invalid seed capture is explicitly unknown, not fabricated");
 memset(&pv->prop,0,sizeof(pv->prop));pv->prop.sequence=101;CL_PredictionDiagCopy(&r,pv,101,true);
 check(!r.predvalid,"neutral/reset prop has no fabricated prediction provenance");
 printf("SUMMARY checks=%d failures=%d\n",checks,failed);return failed;
}
'''
    code = prefix + structs + ring + '\n' + seedtype + '\n' + '\n'.join(parts.values()) + wrappers + tests
    c = a.output / 'provenance.c'
    c.write_text(code, encoding='utf8', newline='\n')
    compiler = shutil.which(a.cc)
    if compiler is None:
        raise FileNotFoundError(a.cc)
    compiler = str(Path(compiler).resolve())
    env = os.environ.copy()
    env['PATH'] = str(Path(compiler).parent) + os.pathsep + env.get('PATH', '')
    exe = a.output / ('provenance.exe' if os.name == 'nt' else 'provenance')
    build = subprocess.run([compiler, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-o', str(exe), str(c), '-lm'], env=env, capture_output=True, text=True)
    for name, value in [('build.stdout', build.stdout), ('build.stderr', build.stderr), ('build.exit', str(build.returncode)+'\n')]:
        (a.output / name).write_text(value, encoding='utf8')
    if build.returncode:
        raise RuntimeError('fixture compilation failed; inspect retained diagnostics')
    capture = a.output / 'dump.txt'
    run = subprocess.run([str(exe.resolve()), str(capture.resolve())], env=env, capture_output=True, text=True)
    (a.output / 'run.stdout').write_text(run.stdout, encoding='utf8')
    (a.output / 'run.stderr').write_text(run.stderr, encoding='utf8')
    if run.returncode:
        raise RuntimeError('provenance acting control failed')
    spec = importlib.util.spec_from_file_location('native_provenance_reader', a.reader)
    reader = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reader)
    head, _, rows, _ = reader.load(capture)
    assert len(rows) == 2 and len(reader.RING_V2_COLS) == 33
    assert any(s == '# command_identity ack-seat-tag-v1' for s in head)
    assert rows[0]['cmdvalid'] == rows[0]['predvalid'] == 1 and rows[0]['seedseq'] == 99
    assert abs(rows[0]['seedcarry'] - .014) < 1e-8 and rows[0]['parts'] == rows[0]['ticks'] == 3
    assert rows[1]['cmdvalid'] == rows[1]['predvalid'] == rows[1]['seedcarry'] == rows[1]['parts'] == rows[1]['ticks'] == 0
    receipt = dict(source_sha256=hashlib.sha256(raw).hexdigest(),
                   helper_sha256={k: hashlib.sha256(v.encode()).hexdigest() for k,v in parts.items()},
                   reader_sha256=hashlib.sha256(a.reader.read_bytes()).hexdigest(),
                   compile_exit=build.returncode, run_exit=run.returncode, actual_writer_rows=2, actual_writer_columns=33,
                   not_full_native_lifecycle_or_historical_cause=True)
    (a.output / 'receipt.json').write_text(json.dumps(receipt, indent=2)+'\n', encoding='utf8')
    print(run.stdout, end='')
    print('PASS actual native v2 writer -> current reader, two positive/unknown rows')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
