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
cull = s[s.index('void GL_CullFace(unsigned int sflags)'):s.index('static void R_FetchPlayerColour')]
select_start = s.index('\nvoid GLBE_SelectEntity(entity_t *ent)')
selectentity = s[select_start:s.index('\n#ifndef GLSLONLY', select_start)]
# Preserve the existing signed -1 sentinel/unsigned-fl comparison verbatim;
# its untouched warning is scoped out only around this extracted function.
selectentity = '#pragma GCC diagnostic push\n#pragma GCC diagnostic ignored "-Wsign-compare"\n' + selectentity + '\n#pragma GCC diagnostic pop\n'
select_seam = '\tif (shaderstate.curentity != batch->ent)\n\t\tGLBE_SelectEntity(batch->ent);'
assert s.count(select_seam) == 1
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
__ACTUAL_FLAGS__
#define SHADER_CULL_FLIP (SHADER_CULL_FRONT|SHADER_CULL_BACK)
#define BEM_STANDARD 0
#define BEM_LIGHT 1
#define BEM_STENCIL 2
#define BEM_DEPTHONLY 3
struct batch_s;
typedef struct entity_s entity_t;
static struct { int mode; struct batch_s *mbatches[4]; unsigned flags,curcull; entity_t *curentity; int usingweaponviewmatrix,lastuniform; float modelmatrix[16],modelviewmatrix[16],modelmatrixinv[16],projectionmatrix[16]; } shaderstate={.usingweaponviewmatrix=-1};
#define GL_CULL_FACE 0x0b44
#define GL_FRONT 0x0404
#define GL_BACK 0x0405
static int test_cull_enabled,test_cull_face;
static void qglEnable(int cap) { assert(cap==GL_CULL_FACE);test_cull_enabled=1; }
static void qglDisable(int cap) { assert(cap==GL_CULL_FACE);test_cull_enabled=0; }
static void qglCullFace(int face) { test_cull_face=face; }
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
struct entity_s { unsigned flags; float axis[3][3],origin[3]; model_t *model; float scale; unsigned drawflags; };
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
static struct { int useperspective; float m_projection_std[16],m_projection_view[16]; int recurse; unsigned flipcull; float m_view[16]; } r_refdef={.useperspective=1,.m_projection_std={1,0,0,0,0,1,0,0,0,0,-1,-1,0,0,-1,0},.m_projection_view={1,0,0,0,0,1,0,0,0,0,-1,-1,0,0,-1,0},.m_view={1,0,0,0,0,1,0,0,0,0,-1,0,0,0,0,1}};
// Actual entity-state transition; unrelated matrix inversion/GL upload is stubbed.
static entity_t *currententity;
static void (*qglLoadMatrixf)(const float *matrix);
#define GL_PROJECTION 0x1701
#define GL_MODELVIEW 0x1700
static void qglMatrixMode(int mode) { (void)mode; }
static void Matrix4_Invert(const float *matrix,float *inverse) { memcpy(inverse,matrix,16*sizeof(float)); }
static void GLBE_SelectEntity(entity_t *ent);
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
  shaderstate.flags=batch->flags;
 __ACTUAL_SELECTENTITY_SEAM__
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
 {
  // Mode2's initial key must not depend on a collinear BSP edge split.
  // Decline plane refinement deliberately, so this acts the actual fallback.
  entity_t saved_world=r_worldentity;
  r_worldentity.axis[0][0]=r_worldentity.axis[1][1]=r_worldentity.axis[2][2]=1;
  r_worldentity.scale=1;
  float quad[4][3]={{0,0,4},{8,0,12},{8,8,12},{0,8,4}};
  float split[5][3]={{0,0,4},{1,0,5},{8,0,12},{8,8,12},{0,8,4}};
  float mark[4][3]={{0,0,7.7f},{8,0,7.7f},{8,8,7.7f},{0,8,7.7f}};
  unsigned qi[]={0,1,2,0,2,3},si[]={0,1,2,0,2,3,0,3,4};
  mesh_t face={.numvertexes=4,.numindexes=6,.xyz_array=quad,.indexes=qi,.id=736};
  mesh_t marker={.numvertexes=4,.numindexes=6,.xyz_array=mark,.indexes=qi,.id=737};
  mesh_t *pair[]={&face,&marker};
  shader_t shade={0};shade.remapto=&shade;shade.prog=&shade;
  batch_t batch={.meshes=2,.mesh=pair,.shader=&shade,.ent=&r_worldentity};
  ndraw=0;GLBE_SubmitBlendedFaces(&batch,NULL);
  assert(ndraw==2&&draws[0]==736&&draws[1]==737);
  face.numvertexes=5;face.numindexes=9;face.xyz_array=split;face.indexes=si;
  assert(GLBE_BlendFaceDepth(&face,m)<GLBE_BlendFaceDepth(&marker,m));
  ndraw=0;GLBE_SubmitBlendedFaces(&batch,NULL);
  assert(ndraw==2&&draws[0]==736&&draws[1]==737);
  // Actual surf_kitsune world faces2047/2070, relative to the reported eye.
  float ramp_far[4][3]={{-364.052700000f,1347.077195313f,-173.635203906f},{-28.052700000f,1312.176804688f,-572.124217578f},{-28.052700000f,292.077195313f,-482.876170703f},{-364.052700000f,326.976609375f,-84.387187549f}};
  unsigned ramp_far_idx[]={0,1,2,0,2,3};
  float ramp_near[5][3]={{-12.052700000f,292.077195313f,-482.876170703f},{-12.052700000f,1312.176804688f,-572.122203418f},{-103.961879688f,1321.655320313f,-463.914805957f},{-364.052700000f,1348.476609375f,-157.710216113f},{-364.052700000f,328.377000000f,-68.462199756f}};
  unsigned ramp_near_idx[]={0,1,2,0,2,3,0,3,4};
  face.numvertexes=4;face.numindexes=6;face.xyz_array=ramp_far;face.indexes=ramp_far_idx;
  marker.numvertexes=5;marker.numindexes=9;marker.xyz_array=ramp_near;marker.indexes=ramp_near_idx;
  vpn[0]=-0.532216565f;vpn[1]=0.810234614f;vpn[2]=-0.245490118f;
  assert(GLBE_BlendFaceDepth(&face,m)<GLBE_BlendFaceDepth(&marker,m));
  ndraw=0;GLBE_SubmitBlendedFaces(&batch,NULL);
  assert(ndraw==2&&draws[0]==736&&draws[1]==737);
  vpn[0]=0;vpn[1]=0;vpn[2]=1;ndraw=nemit=0;
  shaderstate.curentity=NULL;currententity=NULL;shaderstate.lastuniform=0;
  r_worldentity=saved_world;
 }
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
 // Full material stream, including1194: the historical three-pane golden
 // reversed1194/1235. Check native eye and rounded body+standing offset.
 sh.flags=SHADER_CULL_FRONT;sh.remapto=&sh;sh.numpasses=1;
 const float eyes[2][3]={{-14986.237f,-14484.737f,836.36249f},{-14986.3f,-14484.8f,836.3f}};
 for(int eye=0;eye<2;eye++)for(int y=0;y<6;y++) {
  memcpy(r_origin,eyes[eye],sizeof(r_origin));
  double pitch=19.3*3.141592653589793/180,yaw=yaws[y]*3.141592653589793/180;
  vpn[0]=cos(pitch)*cos(yaw);vpn[1]=cos(pitch)*sin(yaw);vpn[2]=-sin(pitch);
  vright[0]=sin(yaw);vright[1]=-cos(yaw);vright[2]=0;
  vup[0]=sin(pitch)*cos(yaw);vup[1]=sin(pitch)*sin(yaw);vup[2]=cos(pitch);
  for(int j=0;j<3;j++){r_refdef.m_view[j*4]=vright[j];r_refdef.m_view[j*4+1]=vup[j];r_refdef.m_view[j*4+2]=-vpn[j];}
  r_refdef.m_view[12]=r_refdef.m_view[13]=r_refdef.m_view[14]=0;
  for(int j=0;j<3;j++){r_refdef.m_view[12]-=vright[j]*r_origin[j];r_refdef.m_view[13]-=vup[j]*r_origin[j];r_refdef.m_view[14]+=vpn[j]*r_origin[j];}
  mesh_t *input[28];for(int i=0;i<28;i++)input[i]=hs_all[27-i];
  batch_t all={.ent=&r_worldentity,.shader=&sh,.mesh=input,.meshes=28,.maxmeshes=28};
  wb[SHADER_SORT_BLEND]=&all;recordmeshes=1;ndraw=nemit=0;r_blendsort.ival=1;
  GLBE_SubmitMeshes(wb,SHADER_SORT_BLEND,SHADER_SORT_BLEND);
  int a=-1,b=-1,c=-1,d=-1;
  for(int i=0;i<nemit;i++){if(emitted[i]==1235)a=i;if(emitted[i]==1236)b=i;if(emitted[i]==1228)c=i;if(emitted[i]==1194)d=i;}
  assert(a>=0&&d>=0&&b>=0&&c>=0); // every contributing pane must act
  assert(a<d&&d<b&&b<c); // independently proved1235,1194,1236,1228
  assert(all.mesh==input&&all.meshes==28&&all.maxmeshes==28);
  for(int i=0;i<28;i++)assert(input[i]==hs_all[27-i]);
  ndraw=nemit=0;r_blendsort.ival=0;GLBE_SubmitMeshes(wb,SHADER_SORT_BLEND,SHADER_SORT_BLEND);
  assert(ndraw==1&&nemit==28&&ranges[0]==28);
  for(int i=0;i<28;i++)assert(emitted[i]==input[i]->id);
  // No-cull is a potentially visible stream: the old conservative cycle stays.
  sh.flags=0;ndraw=nemit=0;r_blendsort.ival=1;GLBE_SubmitMeshes(wb,SHADER_SORT_BLEND,SHADER_SORT_BLEND);
  assert(ndraw==1&&nemit==28&&ranges[0]==28);
  for(int i=0;i<28;i++)assert(emitted[i]==input[i]->id);
  sh.flags=SHADER_CULL_FRONT;recordmeshes=0;
 }
 // Cross-check effective cull semantics with the actual GL_CullFace routine.
 float cx[4][3]={{-1,-1,10},{1,-1,10},{1,1,10},{-1,1,10}};
 unsigned ci[6]={0,1,2,0,2,3};mesh_t culled={.numvertexes=4,.numindexes=6,.xyz_array=cx,.indexes=ci,.id=600};
 mesh_t *cp=&culled;shader_t csh={.numpasses=1},mapped=csh;csh.remapto=&mapped;mapped.remapto=&mapped;
 batch_t cb={.ent=&r_worldentity,.shader=&csh,.mesh=&cp,.meshes=1};
 memset(r_refdef.m_view,0,sizeof(r_refdef.m_view));r_refdef.m_view[0]=r_refdef.m_view[5]=r_refdef.m_view[15]=1;r_refdef.m_view[10]=-1;
 for(int winding=0;winding<2;winding++)for(int mirror=0;mirror<2;mirror++)for(int flip=0;flip<2;flip++)for(int two=0;two<2;two++)for(int kind=0;kind<4;kind++) {
  ci[1]=winding?2:1;ci[2]=winding?1:2;ci[4]=winding?3:2;ci[5]=winding?2:3;
  r_refdef.m_projection_std[0]=mirror?-1:1;r_refdef.flipcull=flip?SHADER_CULL_FLIP:0;
  mapped.flags=kind==0?0:kind==1?SHADER_CULL_FRONT:kind==2?SHADER_CULL_BACK:SHADER_CULL_FLIP;
  csh.flags=SHADER_CULL_FLIP^mapped.flags;cb.flags=two?BEF_FORCETWOSIDED:0;
  shaderstate.flags=cb.flags;shaderstate.curcull=~0u;GL_CullFace(mapped.flags);
  int ccw=!(winding^mirror),empty=test_cull_enabled&&(test_cull_face==(ccw?GL_FRONT:GL_BACK));
  assert(GLBE_BlendWorldRasterEmpty(&cb,&culled)==empty);
 }
 r_refdef.m_projection_std[0]=1;r_refdef.flipcull=0;cb.flags=0;mapped.flags=SHADER_CULL_FRONT;
 ci[1]=1;ci[2]=2;ci[4]=2;ci[5]=3;
 assert(GLBE_BlendWorldRasterEmpty(&cb,&culled));
 // A later visible triangle cannot be discarded using the representative normal.
 ci[4]=3;ci[5]=2;assert(!GLBE_BlendWorldRasterEmpty(&cb,&culled));ci[4]=2;ci[5]=3;
 for(int arm=0;arm<17;arm++) {
  mesh_t saved=culled;float coordinate=cx[0][0],depth=cx[0][2],projx=r_refdef.m_projection_std[0],projy=r_refdef.m_projection_std[5];
  switch(arm) {
   case 0:ci[5]=4;break;case 1:culled.indexes=NULL;break;case 2:culled.xyz_array=NULL;break;
   case 3:culled.numindexes=5;break;case 4:culled.numvertexes=65;break;case 5:culled.numindexes=195;break;
   case 6:culled.xyz2_array=cx;break;case 7:culled.numbones=1;break;case 8:cx[0][0]=NAN;break;
   case 9:cx[0][0]=INFINITY;break;case 10:cx[0][2]=-1;break;case 11:r_refdef.m_view[12]=NAN;break;
   case 12:cx[0][2]=.01f;break;
   case 13:r_refdef.m_projection_std[0]=NAN;break;case 14:r_refdef.m_projection_std[5]=NAN;break;
   case 15:r_refdef.m_projection_std[0]=INFINITY;break;case 16:r_refdef.m_projection_std[5]=INFINITY;break;
  }
  assert(!GLBE_BlendWorldRasterEmpty(&cb,&culled));culled=saved;ci[5]=3;cx[0][0]=coordinate;cx[0][2]=depth;r_refdef.m_view[12]=0;
  r_refdef.m_projection_std[0]=projx;r_refdef.m_projection_std[5]=projy;
 }
 // Finite nonzero winding below the relative area threshold is uncertain.
 float mx[3][3]={{0,0,10},{10,10,10},{20,20.000002f,10}};
 unsigned mi[3]={0,1,2};mesh_t marginal={.numvertexes=3,.numindexes=3,.xyz_array=mx,.indexes=mi,.id=601};
 assert(!GLBE_BlendWorldRasterEmpty(&cb,&marginal));
 mx[2][1]=30;assert(GLBE_BlendWorldRasterEmpty(&cb,&marginal));
 // Fully culled commands still consume the original raw-face inspection budget.
 mesh_t *emptyrun[129];for(int i=0;i<129;i++)emptyrun[i]=&culled;
 cb.flags=0;cb.mesh=emptyrun;cb.meshes=cb.maxmeshes=129;
 wb[SHADER_SORT_BLEND]=&cb;ndraw=0;r_blendsort.ival=1;GLBE_SubmitMeshes(wb,SHADER_SORT_BLEND,SHADER_SORT_BLEND);
 assert(ndraw==1&&ranges[0]==129);
 // The budget must also veto a correction after a long inert prefix.
 memset(r_origin,0,sizeof(r_origin));vpn[0]=vpn[1]=0;vpn[2]=1;vright[0]=1;vright[1]=vright[2]=0;vup[0]=vup[2]=0;vup[1]=1;
 pb.mesh=reverse;pb.meshes=2;pb.next=NULL;sh.flags=0;
 cb.meshes=cb.maxmeshes=127;cb.next=&pb;
 ndraw=0;GLBE_SubmitMeshes(wb,SHADER_SORT_BLEND,SHADER_SORT_BLEND);
 assert(ndraw==2&&ranges[0]==127&&ranges[1]==2);
 // An inert intervening slot is removable; a potentially visible one is a barrier.
 mesh_t *crossing[3]={&pm[1],&culled,&pm[0]};pb.mesh=crossing;pb.meshes=3;sh.flags=0;
 shader_t inert=sh;inert.flags=SHADER_CULL_FRONT;inert.remapto=&inert;
 batch_t end=pb;end.mesh=&crossing[2];end.meshes=1;end.next=NULL;
 batch_t mid=end;mid.shader=&inert;mid.mesh=&crossing[1];mid.next=&end;
 batch_t begin=end;begin.mesh=&crossing[0];begin.next=&mid;wb[SHADER_SORT_BLEND]=&begin;
 ndraw=nemit=0;recordmeshes=1;GLBE_SubmitMeshes(wb,SHADER_SORT_BLEND,SHADER_SORT_BLEND);
 assert(nemit==2&&emitted[0]==250&&emitted[1]==251);
 inert.flags=0;ndraw=nemit=0;GLBE_SubmitMeshes(wb,SHADER_SORT_BLEND,SHADER_SORT_BLEND);
 assert(nemit==3&&emitted[0]==251&&emitted[1]==600&&emitted[2]==250);
 // Preceding dynamic entity's RF_XFLIP must not leak into the world proof.
 mesh_t prior=culled;prior.id=700;mesh_t *pp=&prior;
 entity_t preceding=e;
 batch_t before_world={.ent=&preceding,.shader=&sh,.mesh=&pp,.meshes=1};
 shaderstate.mbatches[0]=&before_world;
 for(int view=0;view<2;view++)for(int xflip=0;xflip<2;xflip++)for(int empty=0;empty<2;empty++) {
  unsigned viewflip=view?SHADER_CULL_FLIP:0;
  r_refdef.flipcull=viewflip;shaderstate.usingweaponviewmatrix=-1;shaderstate.curentity=NULL;
  preceding.flags=xflip?RF_XFLIP:0;
  inert.flags=(empty?SHADER_CULL_FRONT:SHADER_CULL_BACK)^viewflip;
  ndraw=nemit=0;GLBE_SubmitMeshes(wb,0,SHADER_SORT_BLEND);
  if(empty)assert(nemit==3&&emitted[0]==700&&emitted[1]==250&&emitted[2]==251);
  else assert(nemit==4&&emitted[0]==700&&emitted[1]==251&&emitted[2]==600&&emitted[3]==250);
  assert(r_refdef.flipcull==viewflip&&shaderstate.curentity==&r_worldentity);
 }
 shaderstate.mbatches[0]=NULL;r_refdef.flipcull=0;shaderstate.usingweaponviewmatrix=-1;shaderstate.curentity=NULL;
 puts("PASS actual default0/1/2 dispatch:8 prior-entity/view-flip contribution contexts;directed marginal-area abstention;128 effective-cull contexts;17 uncertain/invalid geometry/projection controls;inert/visible crossing barriers;raw culled-face budget;12 complete28-face scenes/four-pane rank at two eyes; 36 frozen-yaw/material permutations;72 same-material mixed foreground/rear batches; stationary unsupported slots and mode0 full-range order; world-only correction; whole-batch/program/multipass/cache/recursive/budget fallbacks; capture barrier; existing mode2 lifetime/range/remap/BEM/plane regressions");
}
'''

c = root / 'gl_blend_faces_test.c'
nan_macro = next(line for line in (engine/'common/mathlib.h').read_text().splitlines() if line.startswith('#define') and 'IS_NAN(x)' in line)
shader_header = (engine/'gl/shader.h').read_text()
import re
names = ['SHADER_HASREFLECT', 'SHADER_HASREFRACT', 'SHADER_HASRIPPLEMAP', 'SHADER_HASPORTAL', 'SHADER_SKY', 'SHADER_NODRAW', 'SHADER_NODLIGHT', 'SHADER_NOSHADOWS', 'SHADER_CULL_FRONT', 'SHADER_CULL_BACK']
flag_lines = []
for name in names:
    value = re.search(r'\b' + name + r'\s*=\s*(1 << \d+)', shader_header).group(1)
    flag_lines.append(f'#define {name} ({value})')
flags = '\n'.join(flag_lines)
for name in ['BEF_FORCEADDITIVE', 'BEF_NODLIGHT', 'BEF_NOSHADOWS', 'BEF_FORCETWOSIDED']:
    flags += '\n' + next(line.split('//')[0] for line in shader_header.splitlines() if line.startswith('#define ' + name + '\t'))
c.write_text(pre.replace('__ACTUAL_FLAGS__', flags).replace('__ACTUAL_IS_NAN__', nan_macro).replace('__ACTUAL_PREBUILD__', prebuild).replace('__ACTUAL_SHADERGATES__', shadergates).replace('__ACTUAL_SELECTENTITY_SEAM__', select_seam) + normal_matrix + post + selectentity + cull + actual + dispatch + (engine.parent/'tools/blend_plane_fixtures.inc').read_text() + (engine.parent/'tools/blend_world_fixtures.inc').read_text() + tests)
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
raster = actual[actual.index('static qboolean GLBE_BlendWorldRasterEmpty'):actual.index('static void GLBE_SubmitBlendedWorld')]
controls = {
    'lost_area_center': base.replace('faces[n].depth = GLBE_BlendAreaDepth(mesh, matrix);',
        'faces[n].depth = GLBE_BlendAreaDepth(mesh, matrix); faces[n].depth = GLBE_BlendFaceDepth(mesh, matrix);'),
    # Test-only labels deliberately invert the fourth contributor while
    # preserving the old1235-before1236-before1228 assertion.
    'lost_full_scene_order': base.replace('if (!overflow) GLBE_BlendPlaneSort(faces, count, &stats);', '''if (!overflow) GLBE_BlendPlaneSort(faces, count, &stats);
        { int a=-1,b=-1; for(size_t k=0;k<count;k++) if(faces[k].mesh) {
            if(faces[k].mesh->id==1194) a=(int)k;
            if(faces[k].mesh->id==1235) b=(int)k;
          }
          if(a>=0 && b>=0 && a>b) { glblendface_t swap=faces[a]; faces[a]=faces[b]; faces[b]=swap; }
        }'''),
    'stale_entity_cull': base.replace('\t\t\tif (shaderstate.curentity != batch->ent)\n\t\t\t\tGLBE_SelectEntity(batch->ent);\n', ''),
    'lost_marginal_area': base.replace('if (fabs(area) <= 1e-6*scale + 1e-12) return false;', '(void)scale; /* mutant: no marginal-area abstention */'),
    'lost_projection_guard': base.replace('if (IS_NAN(proj[0]) || IS_NAN(proj[5])) return false;', ''),
    'lost_cull_filter': base.replace('if (GLBE_BlendWorldRasterEmpty(batch, batch->mesh[m])) continue;', 'if (0) continue;'),
    'original_not_remapped_cull': base.replace('cull = batch->shader->remapto->flags &', 'cull = batch->shader->flags &'),
    'lost_flipcull': base.replace('cull ^= r_refdef.flipcull;', 'cull ^= 0;'),
    'lost_twosided': base.replace('(batch->flags & BEF_FORCETWOSIDED) || !mesh', '0 || !mesh'),
    'first_triangle_only': base.replace(raster, raster.replace('t < mesh->numindexes', 't < 3')),
    'filtered_not_raw_budget': base.replace('overflow || inspected == GLBLEND_PLANE_FACES', 'overflow || count == GLBLEND_PLANE_FACES'),
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
    predicate = {
        'lost_area_center': 'ndraw==2&&draws[0]==736&&draws[1]==737',
        'stale_entity_cull': 'nemit==4&&emitted[0]==700&&emitted[1]==251&&emitted[2]==600&&emitted[3]==250',
        'lost_marginal_area': '!GLBE_BlendWorldRasterEmpty(&cb,&marginal)',
        'lost_full_scene_order': 'a<d&&d<b&&b<c',
        'lost_projection_guard': '!GLBE_BlendWorldRasterEmpty(&cb,&culled)',
        'lost_cull_filter': 'a<d&&d<b&&b<c',
        'original_not_remapped_cull': 'GLBE_BlendWorldRasterEmpty(&cb,&culled)==empty',
        'lost_flipcull': 'GLBE_BlendWorldRasterEmpty(&cb,&culled)==empty',
        'lost_twosided': 'GLBE_BlendWorldRasterEmpty(&cb,&culled)==empty',
        'first_triangle_only': 'a>=0&&d>=0&&b>=0&&c>=0',
        'filtered_not_raw_budget': 'ndraw==2&&ranges[0]==127&&ranges[1]==2',
    }.get(name)
    if predicate:
        assert predicate in failed.stderr, failed
    print(f'PASS falsifier rejects {name}: exit {failed.returncode}')
# Source control seams, not pixel claims.
surf = (engine/'client/r_surf.c').read_text()
assert 'r_blendsort.ival == 2 && qrenderer == QR_OPENGL' in surf
assert 'b->shader->sort == SHADER_SORT_BLEND' in surf
assert 'i == SHADER_SORT_BLEND && r_blendsort.ival == 2' in s
assert 'i == SHADER_SORT_BLEND && r_blendsort.ival == 1' in s
assert 'CVARFD ("r_blendsort", "1"' in (engine/'client/renderer.c').read_text()
print('PASS: default1 plane correction ACTED; mode0/2, dynamic boundary and brush no-flatten gates retained')
