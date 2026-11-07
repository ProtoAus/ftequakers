#!/usr/bin/env python3
"""Compile the actual model-lighting caller; falsify every reuse eligibility gate.
Native original-Cube bit comparisons qualify the producer separately. No game,
renderer or performance claim follows from these controlled callback tests.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

PRELUDE = r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
typedef float vec3_t[3];
typedef int qboolean;
typedef struct { int ival; } cvar_t;
typedef struct { int wl_exact, wl_suppress; } vbspinfo_t;
typedef struct model_s model_t;
struct model_s { void *meshinfo; struct { void (*LightPointValues)(model_t*,const vec3_t,vec3_t,vec3_t,vec3_t); } funcs; };
typedef struct { int unused; } refdef_t;
typedef struct { vec3_t origin, light_dir, axis[3], light_avg, light_range, light_cube[6]; int light_known; } entity_t;
static cvar_t *hl2_lt_reusecube;
static int nvalues, ncube, ncombined;
#define DotProduct(a,b) ((a)[0]*(b)[0]+(a)[1]*(b)[1]+(a)[2]*(b)[2])
#define VectorCopy(a,b) memcpy(b,a,sizeof(vec3_t))
static void VectorNormalize(vec3_t a) { float s=sqrtf(DotProduct(a,a)); if(s) for(int i=0;i<3;i++) a[i]/=s; }
static void Values(vec3_t d,vec3_t a,vec3_t v) { for(int i=0;i<3;i++){d[i]=11+i;a[i]=23+i;v[i]=3+i;} }
static void Cube(vec3_t c[6]) { for(int i=0;i<6;i++) for(int j=0;j<3;j++) c[i][j]=1+i*3+j; }
static void VBSP_LightPointValues(model_t*m,const vec3_t p,vec3_t d,vec3_t a,vec3_t v) { nvalues++; Values(d,a,v); }
static void OtherValues(model_t*m,const vec3_t p,vec3_t d,vec3_t a,vec3_t v) { nvalues++; Values(d,a,v); d[0]+=17; }
static void VBSP_LightPointCube(model_t*m,const vec3_t p,vec3_t c[6]) { ncube++; Cube(c); }
static void VBSP_LightPointValuesCube(model_t*m,const vec3_t p,vec3_t d,vec3_t a,vec3_t v,vec3_t c[6]) { ncombined++; Values(d,a,v); Cube(c); }
'''
MAIN = r'''
int main(void) {
 cvar_t off={0},on={1}; int eligible=0;
 for(int mask=0;mask<64;mask++) {
  int native=!!(mask&1),present=!!(mask&2),exact=!!(mask&4),suppressed=!!(mask&8),enabled=!!(mask&16),nullcvar=!!(mask&32);
  vbspinfo_t vb={exact,suppressed}; model_t mod={present?&vb:NULL,{native?VBSP_LightPointValues:OtherValues}};
  entity_t before={0},want,got; before.origin[0]=11; before.origin[1]=22; before.origin[2]=33;
  for(int i=0;i<3;i++)before.axis[i][i]=1;
  want=got=before; hl2_lt_reusecube=&off; HL2_CalcModelLighting(&want,NULL,NULL,&mod);
  nvalues=ncube=ncombined=0; hl2_lt_reusecube=nullcvar?NULL:(enabled?&on:&off);
  HL2_CalcModelLighting(&got,NULL,NULL,&mod);
  int use=native&&present&&exact&&!suppressed&&enabled&&!nullcvar;
  assert(!memcmp(&got,&want,sizeof(got)));
  assert(ncombined==use && nvalues==!use && ncube==(!use&&native));
  eligible+=use;
 }
 assert(eligible==1); puts("64 eligibility/output cases passed; exact reuse and all fallback controls acted");
 return 0;
}
'''

def caller(text):
    start = text.index('qboolean HL2_CalcModelLighting(')
    begin = text.index('{', start)
    depth = 0
    for end in range(begin, len(text)):
        if text[end] == '{':
            depth += 1
        elif text[end] == '}':
            depth -= 1
            if not depth:
                return text[start:end+1]
    raise ValueError('unterminated actual source function')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--cc', default='C:/msys64/ucrt64/bin/gcc.exe')
    a = p.parse_args()
    assert Path(a.cc).is_file(), 'compiler path unavailable; no output created'
    env = dict(os.environ, PATH=str(Path(a.cc).parent)+os.pathsep+os.environ.get('PATH', ''))
    body = caller(a.source.read_text(encoding='utf-8'))
    a.out.mkdir(parents=True, exist_ok=True)
    variants = {'original': body}
    for name, old, new in [
        ('dynamic', 'prv->wl_exact && ', ''),
        ('baked', '!prv->wl_suppress && ', ''),
        ('foreign', 'mod->funcs.LightPointValues == VBSP_LightPointValues && ', ''),
        ('never-act', 'qboolean combined = (', 'qboolean combined = 0 && (')]:
        assert body.count(old) == 1, name
        variants[name] = body.replace(old, new)
    result = {}
    for name, code in variants.items():
        src, exe = a.out/(name+'.c'), a.out/(name+'.exe')
        assert not exe.exists(), 'retain prior compiled controls; do not overwrite or rerun'
        generated = PRELUDE + code + MAIN
        if src.exists():
            assert src.read_text(encoding='utf-8') == generated, 'changed precompile source; preserve refusal'
        else:
            src.write_text(generated, encoding='utf-8')
        build = subprocess.run([a.cc, '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror',
                                '-Wno-unused-parameter', str(src), '-o', str(exe), '-lm'],
                               capture_output=True, text=True, env=env)
        assert build.returncode == 0, build.stderr
        run = subprocess.run([str(exe)], capture_output=True, text=True, env=env)
        result[name] = {'returncode':run.returncode, 'stdout':run.stdout, 'stderr':run.stderr}
        assert (run.returncode == 0) == (name == 'original'), name
    report = {'source_sha256':hashlib.sha256(a.source.read_bytes()).hexdigest(),
              'caller_sha256':hashlib.sha256(body.encode('utf-8')).hexdigest(), 'arms':result}
    (a.out/'results.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
