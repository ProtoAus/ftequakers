"""Compile actual blend collector/guards with engine stubs, not a GL pixel test.

Run from any cwd: python tools/test_gl_blend_faces.py [--cc gcc] [--output DIR].
Requires a GCC-compatible C99 compiler. Generated controls never edit engine source.
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
parser.add_argument('--output', type=Path, help='retain generated C, binaries and control logs here')
args = parser.parse_args()
scratch = tempfile.TemporaryDirectory(prefix='gl-blend-faces-') if args.output is None else None
root = args.output.resolve() if args.output is not None else Path(scratch.name)
root.mkdir(parents=True, exist_ok=True)
engine = Path(__file__).resolve().parents[1] / 'engine'
# Deliberately aborting mutant binaries must not leave platform core dumps.
if os.name == 'posix':
    import resource
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
s = (engine / 'gl/gl_backend.c').read_text()
actual = s[s.index('typedef struct\n{\n\tbatch_t *batch;\n\tmesh_t *mesh;'):s.index('\nvoid GLBE_SubmitMeshes (')]
dispatch = s[s.index('\nvoid GLBE_SubmitMeshes ('):s.index('\n#if (defined(GLQUAKE) || defined(VKQUAKE)) && defined(MULTITHREAD)', s.index('\nvoid GLBE_SubmitMeshes ('))]
# Exercise the real pre-build BEM gates, but stub GL submission itself.
submit = s[s.index('static void GLBE_SubmitMeshesSortRange(batch_t *sortlist, batch_t *end)'):s.index('typedef struct\n{\n\tbatch_t *batch;')]
prebuild = submit[submit.index('\t\tif (batch->meshes =='):submit.index('\n\t\t//buildmeshes')]
shadergates = submit[submit.index('\t\tif ((bs->flags & SHADER_NODRAW)'):submit.index('\n\t\tif (bs->flags & SHADER_SKY)')]
rotate = (engine / 'gl/gl_rmain.c').read_text()
a = rotate.index('\n\telse\n\t{\n\t\tm[0] = e->axis[0][0];', rotate.index('void R_RotateForEntity'))
b = rotate.index('\n\tif (e->scale', a)
normal_matrix = rotate[a:b].split('{', 1)[1].rsplit('}', 1)[0]
normal_matrix += rotate[b:rotate.index('\n\tMatrix4_Multiply(r_refdef.m_view', b)]
pre = r'''
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
typedef int qboolean;
#define false 0
#define true 1
#define SHADER_HASREFLECT 1
#define SHADER_HASREFRACT 2
#define SHADER_HASRIPPLEMAP 4
#define SHADER_HASPORTAL 8
#define SHADER_SKY 16
#define BEF_FORCEADDITIVE 1
#define BEF_NODLIGHT 2
#define BEF_NOSHADOWS 4
#define SHADER_NODRAW 32
#define SHADER_NODLIGHT 64
#define SHADER_NOSHADOWS 128
#define BEM_STANDARD 0
#define BEM_LIGHT 1
#define BEM_STENCIL 2
#define BEM_DEPTHONLY 3
struct batch_s;
static struct { int mode; struct batch_s *mbatches[4]; } shaderstate;
#define SHADER_SORT_PORTAL 1
#define SHADER_SORT_BLEND 2
static struct { int ival; } r_blendsort={1},r_portalrecursion={0},r_portalonly={0};
#define RF_WEAPONMODEL 1
#define RF_DEPTHHACK 2
#define RF_XFLIP 4
#define nanmask 0x7f800000
__ACTUAL_IS_NAN__
typedef struct { int numvertexes,numindexes; float (*xyz_array)[3]; int id; unsigned *indexes; float (*xyz2_array)[3]; int numbones; } mesh_t;
// GPU handles are simulated; only the collector/submission guards run here.
typedef struct { unsigned vertcount, vbo_handle, ebo_handle; } test_vbo_t;
#define SHADER_PASS_MAX 16
#define SBITS_TESSELLATION (1u<<20)
typedef struct shader_s { unsigned flags; struct shader_s *remapto; int numpasses,numdeforms; void *prog; struct { unsigned shaderbits; void *prog; } passes[SHADER_PASS_MAX]; } shader_t;
typedef struct { float mins[3],maxs[3],clampscale; const char *name; int type; } model_t;
typedef struct { unsigned flags; float axis[3][3],origin[3]; model_t *model; float scale; unsigned drawflags; } entity_t;
#define VectorScale(a,s,b) do { (b)[0]=(a)[0]*(s);(b)[1]=(a)[1]*(s);(b)[2]=(a)[2]*(s); } while(0)
#define VectorMA(a,s,v,b) do { (b)[0]=(a)[0]+(s)*(v)[0];(b)[1]=(a)[1]+(s)*(v)[1];(b)[2]=(a)[2]+(s)*(v)[2]; } while(0)
#define SCALE_TYPE_MASK 3
#define SCALE_TYPE_UNIFORM 0
#define SCALE_TYPE_XYONLY 1
#define SCALE_TYPE_ZONLY 2
#define SCALE_ORIGIN_MASK 12
#define SCALE_ORIGIN_CENTER 0
#define SCALE_ORIGIN_BOTTOM 4
#define SCALE_ORIGIN_TOP 8
#define mod_alias 1
static struct { int ival; } ruleset_allow_larger_models={1};
#define Con_DPrintf(...) ((void)0)
typedef struct batch_s { struct batch_s *next; mesh_t **mesh; unsigned firstmesh,meshes,maxmeshes,flags; shader_t *shader; entity_t *ent; void (*buildmeshes)(struct batch_s*); int id; void *vbo, *fog; union { struct { unsigned surface; } poly; } user; } batch_t;
typedef struct { int firstvert, numvert; } scenetris_t;
static scenetris_t *cl_stris;
static float (*cl_strisvertv)[3];
static unsigned cl_numstris, cl_numstrisvert;
static entity_t r_worldentity;
static void R_DB_Poly(batch_t *b);
static float r_origin[3],vpn[3]={0,0,1},vright[3]={1,0,0},vup[3]={0,1,0};
static struct { int useperspective; float m_projection_std[16]; int recurse; } r_refdef={1,{1,0,0,0,0,1,0,0,0,0,-1,-1,0,0,-1,0},0};
#define BZ_Malloc malloc
#define BZ_Free free
static int draws[128],ndraw, builds, ranges[128], emitted[128], nemit, recordmeshes;
static void GLBE_SubmitMeshesPortals(batch_t **world,batch_t *dynamic) { (void)world;(void)dynamic; }
static void GLBE_SubmitMeshesSortRange(batch_t *batch,batch_t *end) {
 for(;batch&&batch!=end;batch=batch->next) {
 __ACTUAL_PREBUILD__
 if(batch->buildmeshes) batch->buildmeshes(batch);
 shader_t *bs=batch->shader;
 __ACTUAL_SHADERGATES__
 if(batch->meshes>batch->firstmesh) {
  if(recordmeshes) for(unsigned i=batch->firstmesh;i<batch->meshes;i++) {assert(nemit<128);emitted[nemit++]=batch->mesh[i]->id;}
  ranges[ndraw]=batch->meshes-batch->firstmesh; draws[ndraw++]=batch->mesh[batch->firstmesh]->id;
 }
 }
}
static void GLBE_SubmitMeshesSortList(batch_t *batch) { GLBE_SubmitMeshesSortRange(batch,NULL); }
static void R_RotateForEntity(float *m,float *mv,const entity_t *e,model_t *mod) {
'''
post = r'''
(void)mv;
}
'''
tests = r'''
static batch_t *callback_original;
static shader_t *callback_shader;
static void update_shader(batch_t *b) { assert(b==callback_original);builds++;b->shader=callback_shader; }
static void procedural(batch_t *b) {
 static mesh_t shared; static mesh_t *ptr;
 // The caller has not built/retained any alias descriptor during gather.
 assert(b->mesh==NULL); builds++; shared.id=b->id; ptr=&shared;
 b->mesh=&ptr;b->firstmesh=0;b->meshes=1;
}
static mesh_t poly_shared;
static void R_DB_Poly(batch_t *b) {
 static mesh_t *ptr=&poly_shared;
 assert(b->mesh==NULL); builds++;
 poly_shared.id=b->id; b->mesh=&ptr;b->firstmesh=0;b->meshes=1;
}
static void capture_recursive(batch_t *b) {
 shader_t sh={0};sh.remapto=&sh;
 entity_t e={.axis={{1,0,0},{0,1,0},{0,0,1}}};
 batch_t nested={.meshes=1,.shader=&sh,.ent=&e,.buildmeshes=procedural,.id=88};
 GLBE_SubmitBlendedFaces(NULL,&nested);
 procedural(b); // Same rebuild-after-recursion discipline as real captures.
}
int main(void) {
 float xyz[4][3]={{-2,0,10},{2,0,10},{2,2,10},{-2,2,10}};
 mesh_t far={.numvertexes=4,.numindexes=6,.xyz_array=xyz,.id=1}, middle={.numvertexes=4,.numindexes=6,.xyz_array=xyz,.id=2}, near={.numvertexes=1,.numindexes=6,.id=3},ignored={.numvertexes=4,.numindexes=6,.xyz_array=xyz,.id=77};
 entity_t e={.axis={{1,0,0},{0,1,0},{0,0,1}}},brush=e;
 float m[16],mv[16]; R_RotateForEntity(m,mv,&e,0);
 assert(GLBE_BlendFaceDepth(&far,m)==10);
 // XY depth discriminates yaw sign, unlike a Z-only rotated fixture.
 brush.axis[0][0]=0;brush.axis[0][1]=1;brush.axis[1][0]=-1;brush.axis[1][1]=0;
 vpn[0]=1;vpn[2]=0;
 R_RotateForEntity(m,mv,&brush,0); assert(GLBE_BlendFaceDepth(&far,m)==-1);
 brush.scale=3;R_RotateForEntity(m,mv,&brush,0); assert(GLBE_BlendFaceDepth(&far,m)==-3);
 brush.origin[0]=8;R_RotateForEntity(m,mv,&brush,0); assert(GLBE_BlendFaceDepth(&far,m)==5);
#ifdef HEXEN2
 brush.drawflags=SCALE_TYPE_ZONLY;R_RotateForEntity(m,mv,&brush,0);
 assert(GLBE_BlendFaceDepth(&far,m)==7);brush.drawflags=0;
#endif
 vpn[0]=0;vpn[2]=1;brush=e;brush.origin[2]=20;
 R_RotateForEntity(m,mv,&brush,0);assert(GLBE_BlendFaceDepth(&far,m)==30);
 xyz[0][0]=INFINITY;assert(GLBE_BlendFaceDepth(&far,m)==0);xyz[0][0]=-2;
 xyz[0][0]=NAN;assert(GLBE_BlendFaceDepth(&far,m)==0);xyz[0][0]=-2;
 m[14]=3e38f;m[10]=3e38f;assert(GLBE_BlendFaceDepth(&far,m)>3e39);
 mesh_t zero={0};assert(GLBE_BlendFaceDepth(&zero,m)==0);
 glblendface_t ties[3]={{.depth=5,.sequence=2},{.depth=5,.sequence=0},{.depth=5,.sequence=1}};
 qsort(ties,3,sizeof(*ties),GLBE_BlendFaceCompare);assert(ties[0].sequence==0&&ties[2].sequence==2);
 shader_t sh={0};sh.remapto=&sh;
 mesh_t *wm[3]={&ignored,&far,&near}, *bm[1]={&middle};
 float nxyz[1][3]={{0,0,0}};near.xyz_array=nxyz;
 brush.origin[2]=-5;
 batch_t world={.mesh=wm,.firstmesh=1,.meshes=3,.maxmeshes=3,.shader=&sh,.ent=&e},
 dynamic={.mesh=bm,.meshes=1,.maxmeshes=1,.shader=&sh,.ent=&brush};
 GLBE_SubmitBlendedFaces(&world,&dynamic);
 assert(ndraw==3&&draws[0]==1&&draws[1]==2&&draws[2]==3);
 assert(world.firstmesh==1&&world.meshes==3&&world.mesh==wm&&wm[0]==&ignored);
 ndraw=0;vpn[2]=-1;GLBE_SubmitBlendedFaces(&world,&dynamic);
 assert(ndraw==3&&draws[0]==3&&draws[1]==2&&draws[2]==1);vpn[2]=1;
 // Mixed far-model / plain-near / near-model; both alias commands use one shared meshbuf.
 model_t mdl={.mins={0,0,-2},.maxs={0,0,2},.name="test",.clampscale=1};
 entity_t ef=e,en=e;ef.model=en.model=&mdl;ef.origin[2]=20;en.origin[2]=-10;
 batch_t modelnear={.meshes=1,.shader=&sh,.ent=&en,.buildmeshes=procedural,.id=5};
 batch_t modelfar={.next=&modelnear,.meshes=1,.shader=&sh,.ent=&ef,.buildmeshes=procedural,.id=4};
 batch_t plain={.mesh=&wm[2],.meshes=1,.shader=&sh,.ent=&e};
 ndraw=builds=0;GLBE_SubmitBlendedFaces(&plain,&modelfar);
 assert(ndraw==3&&draws[0]==4&&draws[1]==3&&draws[2]==5&&builds==2);
 assert(modelfar.mesh==NULL&&modelnear.mesh==NULL&&modelfar.next==&modelnear);
 ndraw=builds=0;vpn[2]=-1;GLBE_SubmitBlendedFaces(&plain,&modelfar);
 assert(draws[0]==5&&draws[1]==3&&draws[2]==4&&builds==2);vpn[2]=1;
 // Real scenetris convention: world owner/bbox is unrelated to sprite vertices.
 model_t worldmdl={.name="world",.mins={0,0,100},.maxs={0,0,100}};
 r_worldentity=e;r_worldentity.model=&worldmdl;
 scenetris_t tris[2]={{0,2},{2,2}};
 float verts[4][3]={{-1,0,10},{1,0,10},{-1,0,30},{1,0,30}};
 cl_stris=tris;cl_strisvertv=verts;cl_numstris=2;cl_numstrisvert=4;
 batch_t spritefar={.meshes=1,.shader=&sh,.ent=&r_worldentity,.buildmeshes=R_DB_Poly,.id=12,.user.poly.surface=1};
 batch_t spritenear={.next=&spritefar,.meshes=1,.shader=&sh,.ent=&r_worldentity,.buildmeshes=R_DB_Poly,.id=11};
 float paneverts[1][3]={{0,0,20}};mesh_t pane={.numvertexes=1,.numindexes=6,.xyz_array=paneverts,.id=13};mesh_t *paneptr=&pane;
 batch_t panecommand={.mesh=&paneptr,.meshes=1,.shader=&sh,.ent=&r_worldentity};
 ndraw=builds=0;GLBE_SubmitBlendedFaces(&panecommand,&spritenear);
 assert(ndraw==3&&draws[0]==12&&draws[1]==13&&draws[2]==11&&builds==2);
 assert(spritenear.mesh==NULL&&spritefar.mesh==NULL&&poly_shared.id==11);
 ndraw=builds=0;vpn[2]=-1;GLBE_SubmitBlendedFaces(&panecommand,&spritenear);
 assert(ndraw==3&&draws[0]==11&&draws[1]==13&&draws[2]==12&&builds==2&&poly_shared.id==12);vpn[2]=1;
 // Stable CPU command center outranks its world bbox even when forced coarse.
 panecommand.flags=BEF_FORCEADDITIVE;
 R_RotateForEntity(m,mv,&r_worldentity,&worldmdl);
 assert(GLBE_BlendBatchDepth(&panecommand,m)==20);panecommand.flags=0;
 r_origin[2]=5;assert(GLBE_BlendBatchDepth(&spritenear,m)==5);r_origin[2]=0;
 m[14]=7;assert(GLBE_BlendBatchDepth(&spritenear,m)==17);
 m[10]=3e38f;m[14]=3e38f;verts[0][2]=verts[1][2]=3e38f;
 assert(GLBE_BlendBatchDepth(&spritenear,m)>1e76);verts[0][2]=verts[1][2]=10;
 R_RotateForEntity(m,mv,&r_worldentity,&worldmdl);
 // Invalid keys canonicalize to zero but must not drop the replay command.
 spritenear.next=NULL;
 for(int invalid=0;invalid<12;invalid++) {
  tris[0].firstvert=0;tris[0].numvert=2;cl_stris=tris;cl_strisvertv=verts;
  cl_numstris=2;cl_numstrisvert=4;spritenear.user.poly.surface=0;
  verts[0][0]=-1;m[14]=0;
  switch(invalid) {
   case 0:spritenear.user.poly.surface=2;break;
   case 1:spritenear.user.poly.surface=UINT32_MAX;break;
   case 2:cl_stris=NULL;break;
   case 3:cl_strisvertv=NULL;break;
   case 4:tris[0].firstvert=-1;break;
   case 5:tris[0].numvert=0;break;
   case 6:tris[0].numvert=-1;break;
   case 7:tris[0].firstvert=5;break;
   case 8:tris[0].firstvert=3;tris[0].numvert=INT32_MAX;break;
   case 9:verts[0][0]=INFINITY;break;
   case 10:verts[0][0]=NAN;break;
   case 11:cl_numstrisvert=0;break;
  }
  builds=0;assert(GLBE_BlendBatchDepth(&spritenear,m)==0&&builds==0);
  ndraw=0;GLBE_SubmitBlendedFaces(NULL,&spritenear);
  assert(ndraw==1&&draws[0]==11&&builds==1&&spritenear.mesh==NULL);
 }
 cl_stris=tris;cl_strisvertv=verts;cl_numstrisvert=4;tris[0].firstvert=0;tris[0].numvert=2;
 verts[0][0]=-1;R_RotateForEntity(m,mv,&r_worldentity,&worldmdl);m[14]=INFINITY;
 assert(GLBE_BlendBatchDepth(&spritenear,m)==0);
 // Foreground draws follow ordinary panes despite a nominally far depth key.
 ef.flags=RF_DEPTHHACK;ndraw=0;GLBE_SubmitBlendedFaces(&plain,&modelfar);
 assert(draws[0]==3&&draws[1]==5&&draws[2]==4);ef.flags=0;
 // Positive-count efficient scene cache: fixed VBO/EBO with no CPU positions.
 mesh_t cached={.numvertexes=32,.numindexes=6,.id=6};mesh_t *cm=&cached;
 test_vbo_t fixedvbo={32,1,2};
 batch_t cache={.mesh=&cm,.meshes=1,.maxmeshes=1,.shader=&sh,.ent=&ef,.vbo=&fixedvbo};
 assert(fixedvbo.vertcount==(unsigned)cached.numvertexes&&fixedvbo.vbo_handle&&fixedvbo.ebo_handle);
 assert(!GLBE_BlendFaceEligible(&cache));ndraw=0;
 GLBE_SubmitBlendedFaces(&plain,&cache);
 assert(ndraw==2&&draws[0]==6&&draws[1]==3&&cache.mesh==&cm&&cache.vbo==&fixedvbo);
 // Nonfinite bounding boxes/matrices canonicalize coarse keys too.
 R_RotateForEntity(m,mv,&ef,&mdl);mdl.mins[0]=NAN;
 assert(GLBE_BlendBatchDepth(&cache,m)==0);mdl.mins[0]=0;
 m[14]=INFINITY;assert(GLBE_BlendBatchDepth(&cache,m)==0);
 // Equal-depth coarse/perface commands keep world then dynamic sequence.
 ef.origin[2]=0;ndraw=0;GLBE_SubmitBlendedFaces(&plain,&cache);
 assert(draws[0]==3&&draws[1]==6);
 sh.flags=SHADER_HASREFRACT;assert(GLBE_BlendBatchImmediate(&plain));sh.flags=0;
 shader_t remap={.flags=SHADER_HASPORTAL};sh.remapto=&remap;assert(GLBE_BlendBatchImmediate(&plain));sh.remapto=&sh;
 dynamic.flags=BEF_FORCEADDITIVE;assert(!GLBE_BlendFaceEligible(&dynamic));assert(!GLBE_BlendBatchImmediate(&dynamic));
 // Immediate capture command precedes gather but does not consume alias records.
 shader_t cap={.flags=SHADER_HASREFRACT};cap.remapto=&cap;
 batch_t capture={.next=&modelfar,.meshes=1,.shader=&cap,.ent=&e,.buildmeshes=capture_recursive,.id=99};
 ndraw=builds=0;GLBE_SubmitBlendedFaces(&plain,&capture);
 assert(draws[0]==88&&draws[1]==99&&builds==4&&ndraw==5);
 assert(plain.mesh==&wm[2]&&modelfar.mesh==NULL&&modelnear.mesh==NULL);
 // Empty current range never submits a coarse cache command.
 cache.firstmesh=cache.meshes;ndraw=0;GLBE_SubmitBlendedFaces(NULL,&cache);
 assert(ndraw==0);cache.firstmesh=0;
 // Actual submission BEM/nodraw guards must still operate on replayed views.
 cache.flags=BEF_NODLIGHT;shaderstate.mode=BEM_LIGHT;ndraw=0;
 GLBE_SubmitBlendedFaces(NULL,&cache);assert(ndraw==0);
 cache.flags=BEF_NOSHADOWS;shaderstate.mode=BEM_DEPTHONLY;
 GLBE_SubmitBlendedFaces(NULL,&cache);assert(ndraw==0);
 cache.flags=0;sh.flags=SHADER_NODRAW;shaderstate.mode=BEM_STANDARD;
 GLBE_SubmitBlendedFaces(NULL,&cache);assert(ndraw==0);sh.flags=0;
 ndraw=0;GLBE_SubmitBlendedFaces(NULL,&cache);assert(ndraw==1&&draws[0]==6);
 // Actual submission must call the plane stage, not just compile helper tests.
 // CPU centroid ranks the wide slope first; strict overlap requires tiny pane first.
 float px[2][4][3]={{{-.1f,-.1f,10},{.1f,-.1f,10},{.1f,.1f,10},{-.1f,.1f,10}},
  {{0,-100,9},{22,-100,31},{22,100,31},{0,100,9}}};
 unsigned pi[6]={0,1,2,0,2,3};mesh_t pm[2];mesh_t *pml[2];
 for(int i=0;i<2;i++){pm[i]=(mesh_t){.numvertexes=4,.numindexes=6,.xyz_array=px[i],.indexes=pi,.id=250+i};pml[i]=&pm[i];}
 batch_t pb={.ent=&r_worldentity,.shader=&sh,.mesh=pml,.meshes=2};
 shaderstate.mode=BEM_STANDARD;vpn[0]=vpn[1]=0;vpn[2]=1;
 memset(r_origin,0,sizeof(r_origin));sh.numdeforms=0;sh.prog=NULL;sh.numpasses=0;
 ndraw=0;GLBE_SubmitBlendedFaces(&pb,NULL);assert(ndraw==2&&draws[0]==250&&draws[1]==251);
 sh.prog=&pm[0];ndraw=0;GLBE_SubmitBlendedFaces(&pb,NULL);assert(ndraw==2&&draws[0]==251&&draws[1]==250);sh.prog=NULL;
 r_refdef.useperspective=0;ndraw=0;GLBE_SubmitBlendedFaces(&pb,NULL);assert(ndraw==2&&draws[0]==251&&draws[1]==250);r_refdef.useperspective=1;
 // Review regression: a fixed dynamic slot is not a pairwise order guarantee.
 // B centroid20, D15, A10: baseline B,D,A. On ray(.005,0,1),
 // D15 is behind A10 behind B9.045: moving A across D breaks D-before-A.
 float dx[4][3]={{-.2f,-.2f,15},{.2f,-.2f,15},{.2f,.2f,15},{-.2f,.2f,15}};
 mesh_t dm={.numvertexes=4,.numindexes=6,.xyz_array=dx,.indexes=pi,.id=252};mesh_t *dml[1]={&dm};
 entity_t de=r_worldentity;batch_t db={.ent=&de,.shader=&sh,.mesh=dml,.meshes=1};
 ndraw=0;GLBE_SubmitBlendedFaces(&pb,&db);assert(ndraw==3&&draws[0]==251&&draws[1]==252&&draws[2]==250);
 // Three admitted panes: an unproven A/D intersection must keep baseline
 // A-before-D. D-before-B and B-before-A then form a conservative cycle.
 dx[0][0]=dx[3][0]=0;dx[1][0]=dx[2][0]=12;dm.id=253;db.ent=&r_worldentity;
 ndraw=0;GLBE_SubmitBlendedFaces(&pb,&db);assert(ndraw==3&&draws[0]==251&&draws[1]==253&&draws[2]==250);
 // Exercise actual default dispatch, not merely an unused plane helper.
 sh.numpasses=1;mesh_t *reverse[2]={&pm[1],&pm[0]};pb.mesh=reverse;
 batch_t *wb[4]={0};wb[SHADER_SORT_BLEND]=&pb;db.ent=&de;dm.id=252;
 shaderstate.mbatches[SHADER_SORT_BLEND]=&db;
 r_blendsort.ival=1;ndraw=0;GLBE_SubmitMeshes(wb,SHADER_SORT_BLEND,SHADER_SORT_BLEND);
 assert(ndraw==3&&draws[0]==250&&draws[1]==251&&draws[2]==252&&ranges[0]==1&&ranges[1]==1);
 assert(pb.mesh==reverse&&reverse[0]==&pm[1]&&pb.meshes==2);
 r_blendsort.ival=0;ndraw=0;GLBE_SubmitMeshes(wb,SHADER_SORT_BLEND,SHADER_SORT_BLEND);
 assert(ndraw==2&&draws[0]==251&&ranges[0]==2&&draws[1]==252);
 r_blendsort.ival=2;ndraw=0;GLBE_SubmitMeshes(wb,SHADER_SORT_BLEND,SHADER_SORT_BLEND);assert(ndraw==3);
 shaderstate.mbatches[SHADER_SORT_BLEND]=NULL;r_blendsort.ival=1;
 // Default unchanged, unsupported and recursive runs retain full batches.
 for(int arm=0;arm<7;arm++) {
  switch(arm){case 0:sh.prog=&pm[0];break;case 1:sh.numpasses=2;break;case 2:r_refdef.recurse=1;break;
   case 3:r_refdef.useperspective=0;break;case 4:sh.numdeforms=1;break;case 5:pm[0].xyz_array=NULL;break;case 6:pb.fog=&sh;break;}
  ndraw=0;GLBE_SubmitMeshes(wb,SHADER_SORT_BLEND,SHADER_SORT_BLEND);
  assert(ndraw==1&&draws[0]==251&&ranges[0]==2);
  sh.prog=NULL;sh.numpasses=1;r_refdef.recurse=0;r_refdef.useperspective=1;sh.numdeforms=0;pm[0].xyz_array=px[0];pb.fog=NULL;
 }
 pb.mesh=pml;ndraw=0;GLBE_SubmitBlendedWorld(&pb);assert(ndraw==1&&ranges[0]==2); // already correct
 // Capture stays between adjacent ordinary runs, never hoisted or crossed.
 shader_t barrier=sh;barrier.flags=SHADER_HASREFRACT;barrier.remapto=&barrier;
 batch_t last=pb;last.mesh=&pml[0];last.meshes=1;
 batch_t barrierbatch=last;barrierbatch.shader=&barrier;barrierbatch.mesh=dml;barrierbatch.next=&last;
 batch_t first=last;first.mesh=&pml[1];first.next=&barrierbatch;
 ndraw=0;GLBE_SubmitBlendedWorld(&first);assert(ndraw==3&&draws[0]==251&&draws[1]==252&&draws[2]==250);
 barrierbatch.shader=&sh;barrierbatch.buildmeshes=update_shader;callback_original=&barrierbatch;callback_shader=&barrier;
 ndraw=builds=0;GLBE_SubmitBlendedWorld(&first);
 assert(ndraw==3&&builds==1&&barrierbatch.shader==&barrier&&barrierbatch.next==&last&&draws[1]==252);
 // Oversize eligible run must not be partially sorted or partially drawn.
 mesh_t *oversize[129];for(int i=0;i<129;i++)oversize[i]=&pm[i%2];
 pb.mesh=oversize;pb.meshes=129;ndraw=0;GLBE_SubmitBlendedWorld(&pb);assert(ndraw==1&&ranges[0]==129);pb.meshes=2;
 // Recorded Kitsune panes, six yaw viewpoints, every material-batch order.
 mesh1235.id=1235;mesh1236.id=1236;mesh1228.id=1228;(void)cyclexyz;
 // Recorded grid_red rear quad shares the actual foreground material batch.
 float xyz4029[4][3]={{-14976,-14832,792},{-15744,-14832,792},{-15872,-15088,792},{-14848,-15088,792}};
 mesh_t rear={.numvertexes=4,.numindexes=6,.xyz_array=xyz4029,.indexes=idx1236,.id=4029};
 mesh_t *frozen[3]={&mesh1235,&mesh1236,&mesh1228};
 const int perm[6][3]={{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};
 float yaws[6]={125.6f,104.3f,104.2f,103.2f,103.1f,95};
 r_origin[0]=-14986.237f;r_origin[1]=-14484.737f;r_origin[2]=836.36249f;
 for(int y=0;y<6;y++) {
  double pitch=19.3*3.141592653589793/180,yaw=yaws[y]*3.141592653589793/180;
  vpn[0]=cos(pitch)*cos(yaw);vpn[1]=cos(pitch)*sin(yaw);vpn[2]=-sin(pitch);
  vright[0]=sin(yaw);vright[1]=-cos(yaw);vright[2]=0;
  vup[0]=sin(pitch)*cos(yaw);vup[1]=sin(pitch)*sin(yaw);vup[2]=cos(pitch);
  for(int p=0;p<6;p++) {
   batch_t fb[3];mesh_t *fm[3];
   for(int i=0;i<3;i++){fm[i]=frozen[perm[p][i]];fb[i]=last;fb[i].mesh=&fm[i];fb[i].next=i<2?&fb[i+1]:NULL;}
   wb[SHADER_SORT_BLEND]=fb;ndraw=0;GLBE_SubmitMeshes(wb,SHADER_SORT_BLEND,SHADER_SORT_BLEND);
   assert(ndraw==3&&draws[0]==1235); // wrong legacy material rank must ACT and fail
   // Same batch, not three one-face batches: rear geometry must not veto
   // foreground admission. Unsupported slots themselves never move.
   for(int slot=0;slot<=3;slot+=3) {
    mesh_t *mixed[4];for(int i=0,j=0;i<4;i++)mixed[i]=i==slot?&rear:frozen[perm[p][j++]];
    batch_t mb=last;mb.mesh=mixed;mb.meshes=4;mb.next=NULL;wb[SHADER_SORT_BLEND]=&mb;
    recordmeshes=1;ndraw=nemit=0;r_blendsort.ival=1;
    GLBE_SubmitMeshes(wb,SHADER_SORT_BLEND,SHADER_SORT_BLEND);
    assert(nemit==4&&emitted[slot]==4029&&emitted[slot?0:1]==1235);
    assert(mb.mesh==mixed&&mb.meshes==4&&mixed[slot]==&rear);
    ndraw=nemit=0;r_blendsort.ival=0;
    GLBE_SubmitMeshes(wb,SHADER_SORT_BLEND,SHADER_SORT_BLEND);
    assert(ndraw==1&&ranges[0]==4&&nemit==4);
    for(int i=0;i<4;i++)assert(emitted[i]==mixed[i]->id);
    recordmeshes=0;r_blendsort.ival=1;
   }
   // An unsupported INTERVENING slot is still a pairwise safety barrier.
   mesh_t *blocked[4]={frozen[perm[p][0]],&rear,frozen[perm[p][1]],frozen[perm[p][2]]};
   batch_t bb=last;bb.mesh=blocked;bb.meshes=4;bb.next=NULL;wb[SHADER_SORT_BLEND]=&bb;
   ndraw=0;GLBE_SubmitMeshes(wb,SHADER_SORT_BLEND,SHADER_SORT_BLEND);
   if(perm[p][0]!=0)assert(ndraw==1&&ranges[0]==4&&draws[0]==blocked[0]->id);
  }
 }
 puts("PASS actual default0/1/2 dispatch:36 frozen-yaw/material permutations;72 same-material mixed foreground/rear batches; stationary unsupported slots and mode0 full-range order; world-only correction; whole-batch/program/multipass/cache/recursive/budget fallbacks; capture barrier; existing mode2 lifetime/range/remap/BEM/plane regressions");
}
'''

c = root / 'gl_blend_faces_test.c'
nan_macro = next(line for line in (engine/'common/mathlib.h').read_text().splitlines() if line.startswith('#define') and 'IS_NAN(x)' in line)
c.write_text(pre.replace('__ACTUAL_IS_NAN__', nan_macro).replace('__ACTUAL_PREBUILD__', prebuild).replace('__ACTUAL_SHADERGATES__', shadergates) + normal_matrix + post + actual + dispatch + (engine.parent/'tools/blend_plane_fixtures.inc').read_text() + tests)
suffix = '.exe' if os.name == 'nt' else ''
exe = root / ('gl_blend_faces_test' + suffix)
flags = ['-std=c99', '-Wall', '-Wextra', '-Werror', '-O2', '-fno-strict-aliasing']
compiler = shlex.split(args.cc) + flags
for variant in ([], ['-DHEXEN2'], ['-ffast-math'], ['-DHEXEN2', '-ffast-math']):
    subprocess.run(compiler + variant + [str(c), '-o', str(exe), '-lm'], check=True, cwd=root)
    subprocess.run([str(exe)], check=True, cwd=root)
# Deliberately restore each rejected behavior in the extracted source, never the engine.
# The assertions must fail, not merely report successful compilation.
base = c.read_text()
controls = {
    'legacy_default': base.replace('GLBE_SubmitBlendedWorld(worldbatches[i]);', 'GLBE_SubmitMeshesSortList(worldbatches[i]);'),
    'whole_batch_geometry': base.replace(
        'return GLBE_BlendPlaneShaderEligible(batch->shader) && GLBE_BlendPlaneShaderEligible(batch->shader->remapto);',
        'glblendface_t face={0}; glblendplane_t plane; face.batch=batch; '
        'for(unsigned m=batch->firstmesh;m<batch->meshes;m++){face.mesh=batch->mesh[m]; '
        'if(!GLBE_BlendPlaneGeometry(&face,&plane))return false;} return true;'),
    'lost_fog': base.replace('|| batch->fog ||', '||'),
    'lost_callback_update': base.replace('GLBE_SubmitMeshesSortRange(batch, batch->next);', 'view=*batch;view.next=NULL;GLBE_SubmitMeshesSortList(&view);'),
    'world_bbox': base.replace('if (batch->buildmeshes == R_DB_Poly)', 'if (0)'),
    'special_first': base.replace('return !sh || !sh->remapto || !batch->ent ||',
                                  'return batch->buildmeshes || !sh || !sh->remapto || !batch->ent ||'),
    'lost_cache': base.replace('!batch->mesh[m]->xyz_array)', '0)').replace(
        'assert(!GLBE_BlendFaceEligible(&cache));', ''),
}
for name, control in controls.items():
    assert control != base
    cc = root / f'gl_blend_faces_{name}_control.c'
    ce = root / (f'gl_blend_faces_{name}_control' + suffix)
    cc.write_text(control)
    subprocess.run(compiler + [str(cc), '-o', str(ce), '-ffast-math', '-lm'], check=True, cwd=root)
    failed = subprocess.run([str(ce)], capture_output=True, text=True, cwd=root)
    (root / f'{name}-control.log').write_text(failed.stdout + failed.stderr)
    assert failed.returncode != 0 and 'assert' in failed.stderr.lower(), failed
    print(f'PASS falsifier rejects {name}: exit {failed.returncode}')
# Source control seams, not pixel claims.
surf = (engine/'client/r_surf.c').read_text()
assert 'r_blendsort.ival == 2 && qrenderer == QR_OPENGL' in surf
assert 'b->shader->sort == SHADER_SORT_BLEND' in surf
assert 'i == SHADER_SORT_BLEND && r_blendsort.ival == 2' in s
assert 'i == SHADER_SORT_BLEND && r_blendsort.ival == 1' in s
assert 'CVARFD ("r_blendsort", "1"' in (engine/'client/renderer.c').read_text()
print('PASS: default1 plane correction ACTED; mode0/2, dynamic boundary and brush no-flatten gates retained')
