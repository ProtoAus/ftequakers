#!/usr/bin/env python3
"""Bounded production-body lighting/sample/draw test; no native renderer or GPU claim.
Run: python3 tools/test_model_lighting_point.py (system cc, temporary executables).
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(text, signature):
    start = text.index(signature)
    begin = text.index('{', start)
    depth = 0
    for end in range(begin, len(text)):
        if text[end] == '{':
            depth += 1
        elif text[end] == '}':
            depth -= 1
            if not depth:
                return text[start:end + 1]
    raise AssertionError(signature)


PRELUDE = r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
typedef float vec3_t[3];
typedef float vec4_t[4];
typedef int qboolean;
#define true 1
#define false 0
#define VectorCopy(a,b) memcpy(b,a,sizeof(vec3_t))
#define VectorEquals(a,b) ((a)[0]==(b)[0] && (a)[1]==(b)[1] && (a)[2]==(b)[2])
#define VectorSubtract(a,b,c) do { for(int vi=0;vi<3;vi++) (c)[vi]=(a)[vi]-(b)[vi]; } while(0)
#define VectorScale(a,s,b) do { for(int vi=0;vi<3;vi++) (b)[vi]=(a)[vi]*(s); } while(0)
#define DotProduct(a,b) ((a)[0]*(b)[0]+(a)[1]*(b)[1]+(a)[2]*(b)[2])
#define Length(a) sqrtf(DotProduct(a,a))
#define VectorSet(a,x,y,z) do { (a)[0]=x;(a)[1]=y;(a)[2]=z; } while(0)
#define VectorClear(a) memset(a,0,sizeof(vec3_t))
#define VectorMA(a,s,b,c) do { for(int vi=0;vi<3;vi++) (c)[vi]=(a)[vi]+(s)*(b)[vi]; } while(0)
static void VectorNormalize(vec3_t v) { float l=Length(v); if(l) VectorScale(v,1/l,v); }
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
#define countof(a) (sizeof(a)/sizeof((a)[0]))
enum { MDLF_SOURCEBSP=1,MDLF_SOURCEPITCH=2,MDLF_FLAME=4,MDLF_EZQUAKEFBCHEAT=8,
 RF_WEAPONMODEL=16,RF_FULLBRIGHT=32,RF_WEAPONMODELNOBOB=64,RF_DEPTHHACK=128,Q2RF_GLOW=256,
 MF_ROTATE=512,RDF_NOWORLDMODEL=1,CP_QUAKEWORLD=1,mod_alias=1,
 PTI_E5BGR9=1,PTI_RGBA16F,PTI_RGBA32F,RQUANT_MODELLIGHTSAMPLE };
typedef struct { int ival; float value; } cvar_t;
typedef struct model_s model_t;
struct model_s { int engineflags,type,flags; const char *name; void *proplights;
 void (*LightPointFold)(model_t*,const vec3_t,vec3_t,vec3_t,vec3_t);
 struct { void (*LightPointValues)(model_t*,const vec3_t,vec3_t,vec3_t,vec3_t); } funcs; };
typedef struct { vec3_t origin,angles,axis[3],light_dir,light_avg,light_range,light_cube[6],light_samplepoint;
 int light_known,vertlightverts,flags,keynum,playerindex; model_t *model; void *vertlightcolors,*vertlightbytes; } entity_t;
static struct { model_t *worldmodel; int deathmatch,hexen2pickups; double time,servertime; } cl;
static struct { int protocol; float allow_fbskins; } cls;
static struct { vec3_t vieworg; int flags; } r_refdef;
static vec3_t vpn={1,0,0},vright={0,1,0},vup={0,0,1};
static double realtime;
static unsigned int r_modellight_seq=1;
static cvar_t r_modellight_cache,r_modellight_fallback,r_modellight_moverate,r_viewmodel_lightrate,
 r_propvertexlight,r_fullbright,r_fb_models,ruleset_allow_fbmodels,r_viewmodel_maxlight,
 r_propvertexlight_minlight,r_prop_minlight,r_vertexdlights,r_dynamic,r_fullbrightSkins,gl_overbright_models;
static int r_softwarebanding,r_dlightlightmaps,rtlights_first;
#define RTL_FIRST 1
static struct { char cubemapname[1]; float radius; vec3_t origin,color; } cl_dlights[1];
static cvar_t settling;
static cvar_t *Cvar_FindVar(const char *s) { (void)s; return &settling; }
static vec3_t baked_origin;
static void *PropLight_Find(model_t*w,const char*n,const vec3_t p,const vec3_t a,int*v,vec3_t b)
 { (void)w;(void)n;(void)a;(void)v;(void)b; VectorCopy(p,baked_origin); return NULL; }
#define CON_ERROR ""
static void Con_Printf(const char *fmt,...) { (void)fmt; assert(!"stale sample"); }
static void RQuantAdd(int q,int n) { (void)q;(void)n; }
static vec3_t samples[16]; static int nsamples,black,style;
static void Sample(model_t*m,const vec3_t p,vec3_t s,vec3_t a,vec3_t d) {
 (void)m; assert(nsamples<16); VectorCopy(p,samples[nsamples++]);
 VectorSet(s,black?0:30+style,black?0:40+style,black?0:50+style); VectorSet(a,black?0:3,black?0:4,black?0:5); VectorSet(d,1,2,3);
}
static float firstz;
static void CoordinateSample(model_t*m,const vec3_t p,vec3_t s,vec3_t a,vec3_t d) {
 Sample(m,p,s,a,d);
 VectorClear(s); VectorClear(a);
 if(p[2]!=firstz) s[0]=p[2];
}
static void FoldSample(model_t*m,const vec3_t p,vec3_t s,vec3_t a,vec3_t d) {
 Sample(m,p,s,a,d); s[0]+=70;
}
'''

DRAW_PRELUDE = r'''
#define QCBUILTIN
#define OFS_RETURN 1
#define OFS_PARM0 4
#define OFS_PARM1 7
#define OFS_PARM2 10
#define OFS_PARM3 13
#define OFS_PARM4 16
static float globals[28];
#define G_FLOAT(o) globals[o]
#define G_VECTOR(o) (&globals[o])
typedef struct { int entnum,free; } csqcedict_t;
typedef struct { int unused; } pubprogfuncs_t;
struct globalvars_s { int unused; };
static csqcedict_t edict={1,0};
#define G_EDICT(p,o) (&edict)
#define ED_ISFREE(e) ((e)->free)
static entity_t input,drawn,shadow;
static int submitted,shadows,copyok=1,cl_numvisedicts,cl_maxvisedicts=1024;
static qboolean CopyCSQCEdictToEntity(csqcedict_t *e,entity_t *out) { (void)e; *out=input; return copyok; }
static void csqc_deprecated(const char *s) { (void)s; }
static void CLQ1_AddShadow(entity_t *e) { shadow=*e; shadows++; }
static void V_AddAxisEntity(entity_t *e) { drawn=*e; submitted++; }
'''

MAIN = r'''
static void reset(void) { nsamples=0; input.light_known=0; submitted=shadows=0; }
static void calc(const float *p) { input.light_known=0; if(p) R_CalcModelLightingAtPoint(&input,input.model,p); else R_CalcModelLighting(&input,input.model); }
static void physical(void) {
 assert(VectorEquals(drawn.origin,input.origin)); assert(VectorEquals(shadow.origin,input.origin));
 assert(!memcmp(drawn.axis,input.axis,sizeof(input.axis))); assert(VectorEquals(drawn.angles,input.angles));
 assert(VectorEquals(baked_origin,input.origin));
}
int main(void) {
 model_t world={.engineflags=MDLF_SOURCEBSP,.funcs={Sample}}, model={.type=mod_alias,.name="prop"};
 cl.worldmodel=&world; input.model=&model; input.playerindex=-1; input.keynum=19;
 VectorSet(input.origin,10,20,30); VectorSet(input.angles,11,22,33);
 for(int i=0;i<3;i++) input.axis[i][i]=1;
 r_propvertexlight.ival=1; world.proplights=&world;
 vec3_t identity={10,20,54},other={110,120,154};
 // Exact first point; black fallback is anchored at point-24, not physical origin.
 black=1; r_modellight_fallback.ival=1;
 reset(); calc(NULL); assert(nsamples==4);
 vec3_t defaults[4]; memcpy(defaults,samples,sizeof(defaults));
 reset(); calc(identity); assert(nsamples==4 && !memcmp(defaults,samples,sizeof(defaults)));
 // Fractional origin exposes non-invertible +24/-24 float rounding.
 input.origin[2]=0.1f; identity[2]=input.origin[2]+24;
 assert(identity[2]-24 != input.origin[2]);
 reset(); calc(NULL); memcpy(defaults,samples,sizeof(defaults));
 reset(); calc(identity); assert(nsamples==4 && !memcmp(defaults,samples,sizeof(defaults)));
 input.origin[2]=30; identity[2]=54;
 reset(); calc(other); assert(nsamples==4);
 for(int i=0;i<4;i++) { assert(samples[i][0]==110 && samples[i][1]==120); assert(samples[i][2]==154-24*i); }
 black=0; r_modellight_fallback.ival=0;
 reset(); calc(NULL); entity_t ordinary=input;
 reset(); calc(identity); assert(!memcmp(&ordinary,&input,sizeof(input)));
 // Cache distinguishes mode AND point, even inside a Source moverate hold window.
 r_modellight_cache.ival=1; r_modellight_moverate.value=1; realtime=10;
 reset(); calc(NULL); assert(nsamples==1);
 reset(); calc(NULL); assert(nsamples==0);
 reset(); calc(identity); assert(nsamples==1);
 reset(); calc(identity); assert(nsamples==0);
 reset(); calc(other); assert(nsamples==1);
 reset(); calc(other); assert(nsamples==0);
 r_modellight_seq++; reset(); calc(other); assert(nsamples==1);
 model.engineflags=MDLF_SOURCEPITCH; world.LightPointFold=FoldSample;
 reset(); calc(other); assert(nsamples==1 && fabsf(input.light_avg[0]-100/255.0f)<1e-6f); // fold key/output
 reset(); calc(other); assert(nsamples==0);
 model.engineflags=0; reset(); calc(other); assert(nsamples==1);
 reset(); calc(NULL); assert(nsamples==1);
 settling.ival=1; reset(); calc(other); assert(nsamples==1);
 reset(); calc(other); assert(nsamples==0);
 realtime+=3; reset(); calc(other); assert(nsamples==1);
 settling.ival=0;
 // Existing ordinary moverate hold still acts; alternate never borrows it or blends.
 reset(); calc(NULL); assert(nsamples==1);
 input.origin[0]++; reset(); calc(NULL); assert(nsamples==0);
 reset(); calc(other); assert(nsamples==1 && modellightcache[19].blend==0);
 r_modellight_cache.ival=0; input.origin[0]--;
 // Same explicit point, changed physical X switches fractional identity fallback arithmetic.
 input.origin[2]=0.1f; vec3_t fixed={10,20,input.origin[2]+24}; firstz=fixed[2];
 world.funcs.LightPointValues=CoordinateSample; r_modellight_cache.ival=1; r_modellight_fallback.ival=1;
 r_modellight_seq++; reset(); calc(fixed); assert(nsamples==2 && samples[1][2]==input.origin[2]);
 reset(); calc(fixed); assert(nsamples==0);
 input.origin[0]++; reset(); calc(fixed);
 assert(nsamples==2 && samples[1][2]==fixed[2]-24 && samples[1][2]!=input.origin[2]);
 entity_t movedlit=input; r_modellight_cache.ival=0; reset(); calc(fixed);
 assert(!memcmp(movedlit.light_avg,input.light_avg,sizeof(vec3_t)));
 input.origin[0]--; input.origin[2]=30; world.funcs.LightPointValues=Sample; r_modellight_fallback.ival=0;
 // Dlights attenuate from physical origin, not the distant alternate sample; run every call.
 r_modellight_cache.ival=1;
 r_dlightlightmaps=1; r_dynamic.ival=1; cl_dlights[0].radius=20;
 VectorCopy(input.origin,cl_dlights[0].origin); VectorSet(cl_dlights[0].color,1,1,1);
 reset(); calc(other); assert(fabsf(input.light_avg[0]-50/200.0f)<1e-6f);
 cl_dlights[0].radius=10; reset(); calc(other); assert(nsamples==0); assert(fabsf(input.light_avg[0]-40/200.0f)<1e-6f);
 r_dlightlightmaps=0; r_modellight_cache.ival=0;
 // Fullbright and per-pass known-light guards remain intact.
 input.flags=RF_FULLBRIGHT; reset(); calc(other); assert(nsamples==0 && input.light_avg[0]==1);
 input.flags=0; reset(); calc(other); nsamples=0;
 R_CalcModelLightingAtPoint(&input,input.model,identity); assert(nsamples==0);
 // Draw builtins use production affine math, preserve render/shadow placement and return status.
 cl_dlights[0].radius=0;
 memset(globals,0,sizeof(globals)); globals[OFS_PARM2]=globals[OFS_PARM3+1]=globals[OFS_PARM4+2]=1;
 reset(); PF_R_AddEntityLightingOffset(NULL,NULL); assert(globals[OFS_RETURN]==1 && submitted==1 && shadows==1);
 assert(nsamples==0 && drawn.light_known==MODEL_LIGHT_PENDING_POINT);
 // Scene finalization happens AFTER addentity: style/seq and dlight changes must be visible.
 style=100; r_modellight_seq++; r_dlightlightmaps=1; cl_dlights[0].radius=20;
 R_CalcModelLighting(&drawn,drawn.model); // no memo reset
 assert(drawn.light_known==1 && nsamples==1 && fabsf(drawn.light_avg[0]-150/200.0f)<1e-6f);
 assert(VectorEquals(samples[0],identity)); physical(); entity_t identitydraw=drawn;
 entity_t lateordinary=input; lateordinary.light_known=0;
 R_CalcModelLighting(&lateordinary,lateordinary.model);
 assert(!memcmp(lateordinary.light_avg,drawn.light_avg,sizeof(vec3_t)));
 assert(!memcmp(lateordinary.light_range,drawn.light_range,sizeof(vec3_t)));
 assert(!memcmp(lateordinary.light_dir,drawn.light_dir,sizeof(vec3_t)));
 nsamples=0; R_CalcModelLighting(&drawn,drawn.model); assert(nsamples==0);
 VectorCopy(identity,G_VECTOR(OFS_PARM1)); reset(); PF_R_AddEntityLightingOrigin(NULL,NULL);
 R_CalcModelLighting(&drawn,drawn.model);
 assert(!memcmp(&identitydraw,&drawn,sizeof(drawn))); physical();
 // Nontrivial columns: rotate XY and translate, including the existing +24Z sample.
 VectorSet(G_VECTOR(OFS_PARM1),100,200,300); VectorSet(G_VECTOR(OFS_PARM2),0,1,0);
 VectorSet(G_VECTOR(OFS_PARM3),-1,0,0); VectorSet(G_VECTOR(OFS_PARM4),0,0,2);
 reset(); PF_R_AddEntityLightingOffset(NULL,NULL); assert(submitted==1);
 assert(nsamples==0); R_CalcModelLighting(&drawn,drawn.model);
 assert(samples[0][0]==80 && samples[0][1]==210 && samples[0][2]==408); physical();
 globals[OFS_PARM4]=NAN; reset(); PF_R_AddEntityLightingOffset(NULL,NULL); assert(!submitted && globals[OFS_RETURN]==0);
 globals[OFS_PARM4]=INFINITY; reset(); PF_R_AddEntityLightingOffset(NULL,NULL); assert(!submitted);
 globals[OFS_PARM4]=0;
 globals[OFS_PARM4+2]=3.4e38f; reset(); PF_R_AddEntityLightingOffset(NULL,NULL); assert(!submitted);
 cl_numvisedicts=cl_maxvisedicts; reset(); PF_R_AddEntityLightingOrigin(NULL,NULL); assert(!submitted && !shadows && globals[OFS_RETURN]==0);
 cl_numvisedicts=0;
 input.flags=RF_WEAPONMODEL; reset(); PF_R_AddEntityLightingOrigin(NULL,NULL); assert(!submitted && globals[OFS_RETURN]==0);
 // Defensive internal weapon call ignores supplied point and retains eye sample.
 VectorSet(r_refdef.vieworg,5,6,7); reset(); calc(other); assert(VectorEquals(samples[0],r_refdef.vieworg));
 // Ordinary known states never inspect stale/uninitialized point storage.
 input.flags=0; VectorSet(input.light_samplepoint,NAN,NAN,NAN); reset();
 R_CalcModelLighting(&input,input.model); assert(nsamples==1 && isfinite(samples[0][0]));
 input.light_known=2; nsamples=0; assert(R_CalcModelLighting(&input,input.model)==1 && nsamples==0);
 input.flags=0; edict.free=1; reset(); PF_R_AddEntityLightingOrigin(NULL,NULL); assert(!submitted);
 edict.free=0; copyok=0; reset(); PF_R_AddEntityLightingOrigin(NULL,NULL); assert(!submitted);
 puts("deferred model lighting point: default/identity/fallback, cache, physical/dlight origin, affine draw and rejection passed");
 return 0;
}
'''


def main():
    alias = (ROOT / 'engine/gl/gl_alias.c').read_text()
    draw = (ROOT / 'engine/client/pr_csqc.c').read_text()
    start = alias.index('#define MODELLIGHTCACHE_BUCKETS')
    end_body = function(alias, 'qboolean R_CalcModelLighting(entity_t')
    end = alias.index(end_body) + len(end_body)
    body = alias[start:end]
    for name in ('addentity_lightingorigin', 'addentity_lightingoffset'):
        assert '{"' + name + '"' in draw
        platform = (ROOT / 'engine/server/pr_cmds.c').read_text()
        record = next(line for line in platform.splitlines() if '{"' + name + '"' in line)
        signature = ('float(entity ent, vector samplepoint)' if name.endswith('origin') else
                     'float(entity ent, vector translation, vector axisX, vector axisY, vector axisZ)')
        assert 'PF_Fixme' in record and 'D("' + signature + '"' in record
    header = (ROOT / 'engine/client/render.h').read_text()
    pending = next(line for line in header.splitlines() if line.startswith('#define MODEL_LIGHT_PENDING_POINT '))
    generated = PRELUDE + pending + '\n' + body + DRAW_PRELUDE
    for sig in ('static void PF_R_AddEntityLightingPoint(',
                'static void QCBUILTIN PF_R_AddEntityLightingOrigin(',
                'static void QCBUILTIN PF_R_AddEntityLightingOffset('):
        generated += function(draw, sig) + '\n'
    generated += MAIN
    with tempfile.TemporaryDirectory(prefix='model-light-point-') as tmp:
        source = Path(tmp) / 'test.c'
        source.write_text(generated)
        for opt in ('-O0', '-O2'):
            exe = Path(tmp) / opt[1:]
            subprocess.run(['cc', '-std=c99', opt, '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-parameter', '-Wno-sign-compare', str(source), '-lm', '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
        # Falsify the reviewer regressions, not just the positive fixture.
        for name, old, new in (
            ('eager-lighting', 'ent.light_known = MODEL_LIGHT_PENDING_POINT;',
             'ent.light_known = 0; R_CalcModelLightingAtPoint(&ent, ent.model, point);'),
            ('missing-origin-cache-key', 'VectorEquals(cache->origin, e->origin) &&\n',
             '(samplepoint || VectorEquals(cache->origin, e->origin)) &&\n'),
            ('lost-pending-point', 'e->light_samplepoint);', 'NULL);'),
        ):
            assert generated.count(old) == 1, name
            source.write_text(generated.replace(old, new))
            exe = Path(tmp) / name
            subprocess.run(['cc', '-std=c99', '-O2', str(source), '-lm', '-o', str(exe)], check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            assert result.returncode != 0 and 'Assertion' in result.stderr, (name, result)
            print(name + ': rejected by production-body regression assertions')


if __name__ == '__main__':
    main()
