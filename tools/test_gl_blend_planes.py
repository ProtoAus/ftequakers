"""Exercise exact C plane-order helpers, frozen geometry and acting mutants.

Run from any cwd: python tools/test_gl_blend_planes.py [--cc gcc] [--output DIR].
This is not a pixel or performance oracle; see native validation separately.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

if not __debug__:
    raise RuntimeError('Run without python -O: assertions are the test oracle')

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cc', default=os.environ.get('CC', 'gcc'))
parser.add_argument('--output', type=Path, help='retain generated C, binaries and control logs')
args = parser.parse_args()
HERE = Path(__file__).resolve().parent
engine = HERE.parent / 'engine'
s = (engine / 'gl/gl_backend.c').read_text()
tmp = None if args.output else tempfile.TemporaryDirectory(prefix='gl_blend_planes_')
root = args.output.resolve() if args.output else Path(tmp.name)
root.mkdir(parents=True, exist_ok=True)
code=s[s.index('#define GLBLEND_PLANE_FACES'):s.index('static void GLBE_SubmitBlendedFaces')]
# The collector suite covers raster/trace with real batch/view/command stubs;
# this fixture deliberately isolates plane geometry and ordering only.
near=code[code.index('static qboolean GLBE_BlendWorldRasterEmptyNear'):code.index('// -1: a before b;')]
code=code.replace(near,'')
basis=code[code.index('// Plane-side proofs read the eye from r_origin'):code.index('static qboolean GLBE_BlendPlaneGeometry')]
code=code.replace(basis,'')
struct=s[s.index('typedef struct\n{\n\tbatch_t *batch;\n\tmesh_t *mesh;'):s.index('static double GLBE_BlendCenterDepth')]
struct=struct[:struct.index('static qboolean gl_blendtrace_pending;')]
compare=s[s.index('static int GLBE_BlendFaceCompare'):s.index('static qboolean GLBE_BlendBatchImmediate')]
nan = next(l for l in (engine / 'common/mathlib.h').read_text().splitlines() if l.startswith('#define') and 'IS_NAN(x)' in l)
pre=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
typedef int qboolean;
#define true 1
#define false 0
#define nanmask 0x7f800000
#define BZ_Malloc malloc
#define BZ_Free free
typedef struct { int numvertexes,numindexes; float (*xyz_array)[3]; unsigned *indexes; float (*xyz2_array)[3]; int numbones; } mesh_t;
typedef struct { int numdeforms; struct shader_s_unused *unused; void *remapto; } shader_t;
// Match engine shader's self-pointer type; emitted below instead of the dummy.
#define SHADER_PASS_MAX 16
#define SBITS_TESSELLATION (1u<<20)
#define BEM_STANDARD 0
__SHADER__
static struct { int useperspective; float m_projection_std[16], m_view[16]; } r_refdef={1,{1,0,0,0,0,1,0,0,0,0,-1,-1,0,0,-1,0},{0}};
static struct { int mode; } shaderstate={BEM_STANDARD};
static struct { int ival; } r_blendsort={1};
typedef struct { int unused; } entity_t;
typedef struct { entity_t *ent; unsigned flags; void (*buildmeshes)(void); realshader_t *shader; } batch_t;
static entity_t r_worldentity;
static float r_origin[3],vpn[3]={0,0,1},vright[3]={1,0,0},vup[3]={0,1,0};
'''.replace('__SHADER__','typedef struct { unsigned tess,geom; } program_t; typedef struct { unsigned shaderbits; program_t *prog; } shaderpass_t; typedef struct realshader_s { int numdeforms; struct realshader_s *remapto; int numpasses; program_t *prog; shaderpass_t passes[SHADER_PASS_MAX]; } realshader_t; typedef realshader_t shader_t;').replace('typedef struct { int numdeforms; struct shader_s_unused *unused; void *remapto; } shader_t;', '')
# C fragment is inserted after the mesh/shader stubs, not parsed as a standalone header.
fixtures = (HERE / 'blend_plane_fixtures.inc').read_text()
tests=r'''
static unsigned quadidx[6]={0,1,2,0,2,3};
static void procedural(void) { assert(0); }
static void camera(float yaw) {
 double p=19.3*3.141592653589793/180, y=yaw*3.141592653589793/180;
 r_origin[0]=-14986.237f;r_origin[1]=-14484.737f;r_origin[2]=836.36249f;
 vpn[0]=cos(p)*cos(y);vpn[1]=cos(p)*sin(y);vpn[2]=-sin(p);
 vright[0]=sin(y);vright[1]=-cos(y);vright[2]=0;
 vup[0]=sin(p)*cos(y);vup[1]=sin(p)*sin(y);vup[2]=cos(p);
}
static double key(mesh_t *m) {
 double d=0;
 for(int v=0;v<m->numvertexes;v++) for(int j=0;j<3;j++) d+=((double)m->xyz_array[v][j]-r_origin[j])*vpn[j]/m->numvertexes;
 return d;
}
static glblendface_t face(batch_t *b,mesh_t *m,size_t seq) {
 glblendface_t f={.batch=b,.mesh=m,.sequence=seq};f.depth=key(m);return f;
}
static void quad(float xyz[4][3],float x,float z) {
 for(int v=0;v<4;v++) { xyz[v][0]=x+(v==1||v==2?2:-2);xyz[v][1]=v>=2?2:-2;xyz[v][2]=z; }
}
int main(void) {
 realshader_t sh={0};sh.remapto=&sh;
 batch_t b={.ent=&r_worldentity,.shader=&sh};
 mesh_t *frozen[3]={&mesh1235,&mesh1236,&mesh1228};
 float yaws[6]={125.6f,104.3f,104.2f,103.2f,103.1f,95};
 for(int y=0;y<6;y++) {
  camera(yaws[y]);glblendface_t f[3];glblendplane_t p[3];glblendplanestats_t st;
  for(int i=0;i<3;i++) { f[i]=face(&b,frozen[i],i);assert(GLBE_BlendPlaneGeometry(&f[i],&p[i])); }
  //1235 straddles1236's plane; the reverse plane certifies their order.
  assert(GLBE_BlendPlaneSide(&p[0],&p[1])==0);
  assert(GLBE_BlendPlaneSide(&p[1],&p[0])==1);
  assert(GLBE_BlendPlaneSide(&p[0],&p[2])==-1);
  qsort(f,3,sizeof(*f),GLBE_BlendFaceCompare);
  if(y==2||y==4||y==5) assert(f[0].mesh!=&mesh1235); // baseline ACTED wrong rank.
  GLBE_BlendPlaneSort(f,3,&st);assert(st.status==1&&st.edges>=2&&f[0].mesh==&mesh1235);
 }
 memset(r_origin,0,sizeof(r_origin));vpn[0]=vpn[1]=0;vpn[2]=1;vright[0]=1;vright[1]=vright[2]=0;vup[0]=vup[2]=0;vup[1]=1;
 float ax[4][3],bx[4][3];quad(ax,0,10);quad(bx,0,20);
 mesh_t a={.numvertexes=4,.numindexes=6,.xyz_array=ax,.indexes=quadidx},z=a;z.xyz_array=bx;
 glblendface_t f[3]={face(&b,&a,0),face(&b,&z,1),{.batch=&b,.depth=15,.sequence=99}};
 glblendplanestats_t st;GLBE_BlendPlaneSort(f,3,&st);
 assert(st.status==1&&st.edges==1&&f[0].mesh==&z&&f[1].mesh==&a&&f[2].sequence==99);
 // An unsupported slot is a barrier, not just a fixed index.
 // Keep the complete baseline atomically if a permutation crosses it.
 f[0]=face(&b,&a,0);f[1]=(glblendface_t){.batch=&b,.sequence=99};f[2]=face(&b,&z,1);
 glblendface_t barrier_saved[3];memcpy(barrier_saved,f,sizeof(f));
 GLBE_BlendPlaneSort(f,3,&st);assert(st.status==-3&&st.moved==0&&!memcmp(f,barrier_saved,sizeof(f)));
 quad(bx,100,20);f[0]=face(&b,&a,0);f[1]=face(&b,&z,1);
 GLBE_BlendPlaneSort(f,2,&st);assert(st.edges==0&&st.moved==0&&f[0].mesh==&a);
 quad(bx,0,10);GLBE_BlendPlaneSort(f,2,&st);assert(st.moved==0&&f[0].mesh==&a);
 quad(bx,0,20);glblendplane_t p,q;assert(GLBE_BlendPlaneGeometry(&f[0],&p));assert(GLBE_BlendPlaneGeometry(&f[1],&q));
 assert(GLBE_BlendPlaneSide(&p,&q)==1&&GLBE_BlendPlaneSide(&q,&p)==-1);
 // Intersecting vertical pane straddles the other plane: no strict proof.
 bx[0][2]=bx[1][2]=5;bx[2][2]=bx[3][2]=15;bx[0][1]=bx[1][1]=bx[2][1]=bx[3][1]=1;
 assert(GLBE_BlendPlaneGeometry(&f[1],&q));assert(GLBE_BlendPlaneSide(&q,&p)==0&&GLBE_BlendPlaneSide(&p,&q)==0);
 // Review P1 orthographic counterexample: perspective proof reverses the correct orthographic order.
 float ox[2][4][3];mesh_t om[2];glblendface_t of[2],original[2];
 for(int i=0;i<2;i++) {
  for(int v=0;v<4;v++) { ox[i][v][0]=(v==1||v==2)?11:9;ox[i][v][1]=v>=2?1:-1;ox[i][v][2]=2*ox[i][v][0]-9-i; }
  om[i]=(mesh_t){.numvertexes=4,.numindexes=6,.xyz_array=ox[i],.indexes=quadidx};of[i]=face(&b,&om[i],i);
 }
 qsort(of,2,sizeof(*of),GLBE_BlendFaceCompare);assert(of[0].mesh==&om[0]);memcpy(original,of,sizeof(of));
 r_refdef.useperspective=0;GLBE_BlendPlaneSort(of,2,&st);assert(st.status==0&&!memcmp(of,original,sizeof(of)));
 r_refdef.useperspective=1;GLBE_BlendPlaneSort(of,2,&st);assert(st.edges==1&&st.moved==2&&of[0].mesh==&om[1]);
 // Review P1 tessellated counterexample has an ACTED wrong CPU-plane order before the material gate.
 float tx[2][4][3];mesh_t tm[2];glblendface_t tf[2],tsaved[2];
 quad(tx[0],0,10);for(int v=0;v<4;v++) {tx[0][v][0]*=.05f;tx[0][v][1]*=.05f;tx[1][v][0]=(v==1||v==2)?22:0;tx[1][v][1]=v>=2?100:-100;tx[1][v][2]=9+tx[1][v][0];}
 batch_t tb=b;realshader_t tsh={0},remap={0};tsh.remapto=&tsh;remap.remapto=&remap;tb.shader=&tsh;
 for(int i=0;i<2;i++){tm[i]=(mesh_t){.numvertexes=4,.numindexes=6,.xyz_array=tx[i],.indexes=quadidx};tf[i]=face(i?&tb:&b,&tm[i],i);}
 qsort(tf,2,sizeof(*tf),GLBE_BlendFaceCompare);assert(tf[0].mesh==&tm[1]);memcpy(tsaved,tf,sizeof(tf));
 GLBE_BlendPlaneSort(tf,2,&st);assert(st.edges==1&&st.moved==2&&tf[0].mesh==&tm[0]);
 // Source-derived review repro: dynamic D is correctly before A in B,D,A.
 // Ray(.005,0,1) hits D15, A10, B9/(1-.005); never move A across D.
 float dynxyz[4][3];quad(dynxyz,0,15);for(int v=0;v<4;v++){dynxyz[v][0]*=.1f;dynxyz[v][1]*=.1f;}
 mesh_t dynmesh={.numvertexes=4,.numindexes=6,.xyz_array=dynxyz,.indexes=quadidx};
 entity_t dynent;batch_t dynbatch=b;dynbatch.ent=&dynent;
 glblendface_t overlap[3]={tsaved[0],face(&dynbatch,&dynmesh,99),tsaved[1]},overlap_saved[3];
 assert(15>10&&10>9/(1-.005));memcpy(overlap_saved,overlap,sizeof(overlap));
 GLBE_BlendPlaneSort(overlap,3,&st);assert(st.status==-3&&st.edges==1&&st.moved==0&&!memcmp(overlap,overlap_saved,sizeof(overlap)));
 // All-world D spans x0..12, so A/D has no proof. Ray(.5,0,1)
 // hits A18 then D15 and misses B; never reverse baseline A-before-D.
 dynxyz[0][0]=dynxyz[3][0]=0;dynxyz[1][0]=dynxyz[2][0]=12;
 dynbatch.ent=&r_worldentity;overlap[1]=face(&dynbatch,&dynmesh,99);memcpy(overlap_saved,overlap,sizeof(overlap));
 assert(9/(1-.5)>15&&10*.5>.1f);
 GLBE_BlendPlaneSort(overlap,3,&st);assert(st.status==-2&&st.edges==3&&st.moved==0&&!memcmp(overlap,overlap_saved,sizeof(overlap)));
 program_t tessprog={.tess=1},geomprog={.geom=1},vertexprog={0};
 // At the overlap x=0, pane0 z=10 is behind slope z=9: CPU proof
 // moves pane0 first above. A vertex-only program on tb adding 20 to z
 // makes the slope z=29 the far one: original centroid order is correct.
 assert(10 > 9 && 10 < 9+20);
 for(int arm=0;arm<14;arm++) {
  memset(&tsh,0,sizeof(tsh));memset(&remap,0,sizeof(remap));tsh.remapto=&tsh;remap.remapto=&remap;
  realshader_t *material=arm>=7?&remap:&tsh;if(arm>=7)tsh.remapto=&remap;
  switch(arm%7){case 0:material->prog=&tessprog;break;case 1:material->numpasses=1;material->passes[0].prog=&tessprog;break;case 2:material->numpasses=1;material->passes[0].shaderbits=SBITS_TESSELLATION;break;case 3:material->prog=&geomprog;break;case 4:material->numpasses=1;material->passes[0].prog=&geomprog;break;case 5:material->prog=&vertexprog;break;case 6:material->numpasses=1;material->passes[0].prog=&vertexprog;break;}
  memcpy(tf,tsaved,sizeof(tf));GLBE_BlendPlaneSort(tf,2,&st);assert(st.admitted==1&&st.moved==0&&!memcmp(tf,tsaved,sizeof(tf)));
 }
 memcpy(of,original,sizeof(of));shaderstate.mode=1;GLBE_BlendPlaneSort(of,2,&st);assert(st.status==0&&!memcmp(of,original,sizeof(of)));shaderstate.mode=BEM_STANDARD;
 // External matrices can retain useperspective but describe another eye.
 float perspective[16];memcpy(perspective,r_refdef.m_projection_std,sizeof(perspective));
 for(int arm=0;arm<12;arm++) {
  memcpy(r_refdef.m_projection_std,perspective,sizeof(perspective));
  switch(arm){case 0:r_refdef.m_projection_std[15]=1;break;case 1:r_refdef.m_projection_std[11]=0;break;
   case 2:r_refdef.m_projection_std[3]=1;break;case 3:r_refdef.m_projection_std[7]=1;break;
   case 4:r_refdef.m_projection_std[12]=2;break;case 5:r_refdef.m_projection_std[13]=2;break;
   case 6:r_refdef.m_projection_std[0]=0;break;case 7:r_refdef.m_projection_std[5]=0;break;
   case 8:r_refdef.m_projection_std[8]=NAN;break;case 9:r_refdef.m_projection_std[14]=INFINITY;break;
   case 10:r_refdef.m_projection_std[1]=1;break;case 11:r_refdef.m_projection_std[4]=1;break;}
  memcpy(of,original,sizeof(of));GLBE_BlendPlaneSort(of,2,&st);assert(st.status==0&&!memcmp(of,original,sizeof(of)));
 }
 memcpy(r_refdef.m_projection_std,perspective,sizeof(perspective));
 r_refdef.m_projection_std[0]=-1;r_refdef.m_projection_std[8]=.2f;r_refdef.m_projection_std[9]=-.1f;
 r_refdef.m_projection_std[2]=.3f;r_refdef.m_projection_std[6]=.4f; // oblique z row
 memcpy(of,original,sizeof(of));GLBE_BlendPlaneSort(of,2,&st);assert(st.status==1&&st.edges==1&&of[0].mesh==&om[1]);
 memcpy(r_refdef.m_projection_std,perspective,sizeof(perspective));
 // Invalid/uncertain/unsupported geometry abstains without calling builders.
 glblendface_t bad=face(&b,&a,0);batch_t other=b;entity_t dynamic;
 for(int arm=0;arm<16;arm++) {
  quad(ax,0,10);a.numvertexes=4;a.numindexes=6;a.indexes=quadidx;a.xyz2_array=NULL;a.numbones=0;quadidx[0]=0;
  bad.batch=&b;bad.foreground=0;b.flags=0;b.buildmeshes=NULL;sh.numdeforms=0;
  switch(arm) {
   case 0:ax[0][0]=NAN;break;case 1:ax[0][0]=INFINITY;break;case 2:ax[0][2]=10.5f;break;
   case 3:quad(ax,0,-10);break;case 4:quad(ax,0,.1f);break;
   case 5:memset(ax,0,sizeof(ax));break;case 6:quadidx[0]=99;break;
   case 7:a.numindexes=5;break;case 8:a.numvertexes=65;break;case 9:a.numindexes=195;break;
   case 10:bad.foreground=1;break;case 11:b.flags=1;break;case 12:b.buildmeshes=procedural;break;
   case 13:sh.numdeforms=1;break;case 14:a.xyz2_array=ax;break;
   case 15:other.ent=&dynamic;bad.batch=&other;break;
  }
  assert(!GLBE_BlendPlaneGeometry(&bad,&p));
 }
 b.buildmeshes=NULL;sh.numdeforms=0;quadidx[0]=0;
 // Face cap abort is atomic; no partial reordering.
 float manyxyz[129][4][3];mesh_t many[129];glblendface_t mf[129],before[129];
 for(int i=0;i<129;i++) { quad(manyxyz[i],0,10+i);many[i]=z;many[i].xyz_array=manyxyz[i];mf[i]=face(&b,&many[i],i); }
 memcpy(before,mf,sizeof(mf));GLBE_BlendPlaneSort(mf,129,&st);
 assert(st.status==-1&&st.moved==0&&!memcmp(mf,before,sizeof(mf)));
 // Pair-test budget abort with 128 x eight-vertex duplicate quads.
 float eight[128][8][3];
 for(int i=0;i<128;i++) { quad(eight[i],0,10+i);memcpy(eight[i]+4,eight[i],4*3*sizeof(float));many[i].xyz_array=eight[i];many[i].numvertexes=8;mf[i]=face(&b,&many[i],i); }
 memcpy(before,mf,sizeof(mf));GLBE_BlendPlaneSort(mf,128,&st);
 assert(st.status==-1&&st.work<=GLBLEND_PLANE_WORK&&st.moved==0&&!memcmp(mf,before,128*sizeof(*mf)));
 // Total-vertex budget is independent of both face and pair budgets.
 float fullverts[33][64][3];
 for(int i=0;i<33;i++) { for(int k=0;k<16;k++) quad(fullverts[i]+k*4,0,10+i);many[i].xyz_array=fullverts[i];many[i].numvertexes=64;mf[i]=face(&b,&many[i],i); }
 memcpy(before,mf,sizeof(mf));GLBE_BlendPlaneSort(mf,33,&st);
 assert(st.status==-1&&st.moved==0&&!memcmp(mf,before,33*sizeof(*mf)));
 // Legacy coarse triangle graph-cycle: preserve mode1's atomic fallback.
 // This is NOT a visible-cycle oracle; mode2's three-strip proof is in faces.
 unsigned triidx[3]={0,1,2};mesh_t cyclic[3];glblendface_t cf[3],saved[3];
 for(int i=0;i<3;i++) { cyclic[i]=(mesh_t){.numvertexes=3,.numindexes=3,.xyz_array=cyclexyz[i],.indexes=triidx};cf[i]=face(&b,&cyclic[i],i); }
 memcpy(saved,cf,sizeof(cf));GLBE_BlendPlaneSort(cf,3,&st);
 assert(st.status==-2&&st.edges==3&&st.moved==0&&!memcmp(cf,saved,sizeof(cf)));
 puts("PASS actual plane code: six frozen-yaw wrong-rank controls/correction, contiguous static slots, ACTED dynamic barrier and three-world intersection fallback, disjoint/coplanar/intersection/invalid/deform/dynamic gates, atomic face/vertex/work budgets and legacy coarse triangle cycle, ACTED ortho/material-program counterexamples, raw/remap/pass/PN/geom/external-projection/nonstandard gates");
}
'''
base=pre+nan+'\n'+struct+compare+code+fixtures+tests
flags=['-std=c99','-Wall','-Wextra','-Werror','-O2','-fno-strict-aliasing']
mutants = {
 'actual': base,
 'reverse_side': base.replace('return b->eye > 0 ? 1 : -1','return b->eye > 0 ? -1 : 1'),
 'disable_sort': base.replace('faces[planes[i].slot] = ordered[i];','faces[planes[i].slot] = faces[planes[i].slot];'),
 'lost_ineligible_barrier': base.replace('if (planes[to].slot - planes[from].slot != to - from)', 'if (0)'),
 'lost_unknown_pair': base.replace('if (!ab) ab = -1;', 'if (!ab) continue;'),
 'lost_ortho_guard': base.replace('!r_refdef.useperspective || shaderstate.mode != BEM_STANDARD','shaderstate.mode != BEM_STANDARD'),
 'lost_program_guard': base.replace('shader->numdeforms || shader->prog ||', 'shader->numdeforms ||').replace('|| shader->passes[p].prog)', ')'),
 'lost_vertex_guard': base.replace('shader->numdeforms || shader->prog ||', 'shader->numdeforms || (shader->prog && (shader->prog->tess || shader->prog->geom)) ||').replace('|| shader->passes[p].prog)', '|| (shader->passes[p].prog && (shader->passes[p].prog->tess || shader->passes[p].prog->geom)))'),
 'lost_projection_guard': base.replace('return p[0] != 0 && p[5] != 0 && p[1] == 0 && p[4] == 0 &&\n\t\tp[3] == 0 && p[7] == 0 && p[11] == -1 && p[15] == 0 &&\n\t\tp[12] == 0 && p[13] == 0;', 'return true;'),
}
assert 'GLBE_BlendPlaneSort(faces, n, &stats);' in s
for name,mutant in mutants.items():
 assert mutant!=base or name=='actual'
 source=root/f'plane-{name}.c';source.write_text(mutant)
 for variant in ([],['-ffast-math']):
  suffix='fast' if variant else 'normal';exe=root/(f'plane-{name}-{suffix}'+('.exe' if os.name=='nt' else ''))
  built=subprocess.run([*shlex.split(args.cc),*flags,*variant,str(source),'-o',str(exe),'-lm'],capture_output=True,text=True,cwd=root)
  (root/f'plane-{name}-{suffix}-build.txt').write_text(built.stdout+built.stderr)
  assert built.returncode==0,built.stderr
  ran=subprocess.run([str(exe)],capture_output=True,text=True,cwd=root)
  (root/f'plane-{name}-{suffix}-run.txt').write_text(ran.stdout+ran.stderr)
  assert (ran.returncode==0) if name=='actual' else (ran.returncode!=0 and 'assert' in ran.stderr.lower()),ran
  print(name,suffix,'exit',ran.returncode,ran.stdout.strip())
print('PASS: material programs and external projections preserve baseline; no artifact inputs')
