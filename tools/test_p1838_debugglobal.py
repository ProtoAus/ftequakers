#!/usr/bin/env python3
"""Act production FTE32/default global/local debug lookup with a failing old-return control."""
import argparse
import hashlib
import json
import os
import resource
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PRE = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { unsigned type, ofs, s_name; } ddef32_t;
typedef struct { unsigned short type, ofs; unsigned s_name; } ddef16_t;
typedef union { float _float; int _int; float _vector[3]; } eval_t;
typedef struct { int parm_start, locals, numparms; } function_t;
typedef struct { int structtype; float *globals; } progstate_t;
typedef struct { struct { char *stringtable; } funcs; progstate_t *state; } progfuncs_t;
enum {PST_DEFAULT, PST_KKQWSV, PST_QTEST, PST_FTE32};
#define current_progstate (progfuncs->state)
static struct { function_t *pr_xfunction; } prinst;
static void die(const char *s) { (void)s; abort(); }
static struct { void (*Sys_Error)(const char *); } callbacks = {die}, *externs = &callbacks;
static char strings[] = "\0value\0localvalue\0";
static ddef32_t defs32[] = {{0x8002,4,1},{2,8,7}};
static ddef16_t defs16[] = {{0x8002,4,1},{2,8,7}};
static ddef32_t *ED_FindGlobal32(progfuncs_t *p, const char *name) {
    for(unsigned i=0;i<2;i++) if(!strcmp(p->funcs.stringtable+defs32[i].s_name,name)) return &defs32[i];
    return NULL;
}
static ddef16_t *ED_FindGlobal16(progfuncs_t *p, const char *name) {
    for(unsigned i=0;i<2;i++) if(!strcmp(p->funcs.stringtable+defs16[i].s_name,name)) return &defs16[i];
    return NULL;
}
static ddef32_t *ED_GlobalAtOfs32(progfuncs_t *p, int ofs) {
    (void)p; for(unsigned i=0;i<2;i++) if(defs32[i].ofs==(unsigned)ofs) return &defs32[i]; return NULL;
}
static ddef16_t *ED_GlobalAtOfs16(progfuncs_t *p, int ofs) {
    (void)p; for(unsigned i=0;i<2;i++) if(defs16[i].ofs==(unsigned)ofs) return &defs16[i]; return NULL;
}
'''
POST = r'''
int main(void) {
    float globals[32]={0}; globals[4]=41.5f; globals[8]=12.25f;
    progstate_t state={PST_FTE32,globals};
    progfuncs_t p={{strings},&state}; eval_t *v=NULL;
    /* First FTE32 global lookup must not return the untouched zero/static 16-bit def. */
    ddef32_t *d=ED_FindLocalOrGlobal(&p,"value",&v);
    assert(d==&defs32[0]); assert(d->type==0x8002 && d->ofs==4); assert(v->_float==41.5f);
    state.structtype=PST_QTEST; d=ED_FindLocalOrGlobal(&p,"value",&v);
    assert(d==&defs32[0] && v->_float==41.5f);
    function_t frame={8,1,1}; prinst.pr_xfunction=&frame;
    for(int kind=PST_DEFAULT;kind<=PST_FTE32;kind++) {
        state.structtype=kind;
        d=ED_FindLocalOrGlobal(&p,"localvalue",&v);
        assert(d && d->type==2 && d->ofs==8 && v->_float==12.25f);
        d=ED_FindLocalOrGlobal(&p,"value",&v);
        assert(d && d->type==0x8002 && d->ofs==4 && v->_float==41.5f);
        assert(!ED_FindLocalOrGlobal(&p,"missing",&v));
    }
    prinst.pr_xfunction=NULL;
    state.structtype=PST_FTE32;
    d=ED_FindLocalOrGlobal(&p,"value",&v);
    assert(d==&defs32[0] && d->type==0x8002 && v->_float==41.5f);
    p.state=NULL; assert(!ED_FindLocalOrGlobal(&p,"value",&v));
    puts("PASS actual debug lookup: FTE32/QTEST global identity/type/value; four-format local/global; missing/no-state");
    return 0;
}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path)
    args = ap.parse_args()
    (ROOT/'rig').mkdir(exist_ok=True)
    out = args.out.resolve() if args.out else Path(tempfile.mkdtemp(prefix='p1838-debugglobal-', dir=ROOT/'rig'))
    if not out.is_relative_to(ROOT/'rig'):
        ap.error('output must remain in this owned worktree/rig')
    if args.out:
        out.mkdir(parents=True, exist_ok=False)
    source = ROOT/'engine/qclib/pr_exec.c'
    text = source.read_text()
    start = text.index('ddef32_t *ED_FindLocalOrGlobal(')
    end = text.index('\nstatic char *TrimString(', start)
    body = text[start:end]
    marker = '*val = (eval_t *)&cp->globals[def32->ofs];\n\treturn def32;'
    old = body.replace(marker, '*val = (eval_t *)&cp->globals[def32->ofs];\n\treturn &def;')
    report = {'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(), 'steps':[], 'status':'RUNNING'}
    def save():
        (out/'receipt.json').write_text(json.dumps(report,indent=2)+'\n')
    try:
        for label, code in (('subject',body),('old-return-control',old)):
            c=out/(label+'.c');exe=out/label;c.write_text(PRE+code+POST)
            cmd=[os.environ.get('CC','cc'),'-std=gnu11','-O2','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)]
            p=subprocess.run(cmd,capture_output=True,text=True)
            (out/(label+'-compile.log')).write_text(p.stdout+p.stderr)
            report['steps'].append({'label':label,'compile':cmd,'compile_exit':p.returncode});save()
            if p.returncode:raise AssertionError(label+' compile failed')
            p=subprocess.run([str(exe)],capture_output=True,text=True,
                             preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
            (out/(label+'-run.log')).write_text(p.stdout+p.stderr)
            report['steps'][-1].update(run_exit=p.returncode,output=p.stdout+p.stderr);save()
            if label=='subject' and p.returncode:raise AssertionError('production global lookup failed')
            if label!='subject' and p.returncode==0:raise AssertionError('old-return control did not discriminate')
        report['status']='PASS-production-and-failing-old-return-control'
        print(report['status'],out)
    except BaseException as exc:
        report['status']='FAIL';report['error']=str(exc);raise
    finally:
        save()


if __name__=='__main__':
    main()
