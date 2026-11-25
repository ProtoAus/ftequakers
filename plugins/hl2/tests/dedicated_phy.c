// Isolated production-loader/parser test; no engine runtime or asset install.
#include "../mod_hl2.c"
#include "../mod_phy.c"
#include <assert.h>
#include <stdio.h>

cvar_t *hl2_propcollision, *hl2_phywinding, *hl2_rigidprops, *hl2_skinfallback;
plugcmdfuncs_t *cmdfuncs;
void Q_strlcpy(char *d,const char *s,int n) { snprintf(d,n,"%s",s); }
void Q_strlcat(char *d,const char *s,int n) { int l=strlen(d); if(l<n) Q_strlcpy(d+l,s,n-l); }
qboolean Q_snprintfz(char *d,size_t n,const char *f,...) { va_list a;va_start(a,f);int r=vsnprintf(d,n,f,a);va_end(a);return r<0 || (size_t)r>=n; }
void Con_Printf(const char *fmt,...) { (void)fmt; }
plugcorefuncs_t core;
plugcorefuncs_t *plugfuncs = &core;
static unsigned char fixture[240];
static size_t fixturelen;
static int reads, renderreads, piececalls;
static galiasinfo_t *traced;
static void *galloc(struct zonegroup_s *g, size_t n) { (void)g; return calloc(1,n); }
void Con_DPrintf(const char *fmt, ...) { (void)fmt; }
void VBSP_MDLVPhysAlt(model_t *m) {
    vecV_t *v; index_t *idx; size_t *ledges,n; qboolean wrapped;
    assert(Mod_PHY_ConvexPieces(m,filefuncs,modfuncs,&v,&idx,&ledges,&n,1,&wrapped));
    assert(n==1 && ledges[0]==0 && ledges[1]==3 && !wrapped);
    free(v); free(idx); free(ledges); piececalls++;
}
static void strip(const char *s, char *out, int n) { Q_strlcpy(out,s,n); *strrchr(out,'.')=0; }
static void *load(const char *s, size_t *n) {
    if (!strstr(s,".phy")) { if (strstr(s,".vtx") || strstr(s,".vvd")) renderreads++; *n=0; return NULL; }
    reads++; *n=fixturelen;
    if (!fixturelen) return NULL;
    void *p=malloc(*n); memcpy(p,fixture,*n); return p;
}
static void bih(model_t *m, galiasinfo_t *s) { (void)m; traced=s; }
static void matrix(const float *p, const float *q, const float *s, float *m) {
    (void)q; (void)s; memset(m,0,12*sizeof(float)); m[0]=m[5]=m[10]=1; m[3]=p[0]; m[7]=p[1]; m[11]=p[2];
}
static void u32(size_t o, unsigned v) { memcpy(fixture+o,&v,4); }
static void u16(size_t o, unsigned short v) { memcpy(fixture+o,&v,2); }
static void point(size_t o,float x,float y,float z) { memcpy(fixture+o,&x,4);memcpy(fixture+o+4,&y,4);memcpy(fixture+o+8,&z,4); }
static void makephy(void) {
    memset(fixture,0,sizeof(fixture)); fixturelen=sizeof(fixture);
    u32(0,16);u32(8,1);u32(16,220);memcpy(fixture+20,"VPHY",4);u16(24,256);
    u32(80,48);memcpy(fixture+92,"IVPS",4); // surface at 48, root at 96
    u32(100,32); // leaf points to ledge at 128
    u32(128,48);u16(140,1); // one triangle; points at 176
    u32(148,0);u32(152,1);u32(156,2);
    point(176,0,0,0);point(192,.0254f,0,0);point(208,0,0,.0254f);
}
int main(void) {
    static plugfsfuncs_t fs; static plugmodfuncs_t mf; static cvar_t fast,mode;
    struct { hl2mdlheader_t h; hl2mdlbone_t b; char name[8]; hl2mdlanim_t anim; } mdl={0};
    model_t a={0},b={0};
    core.Malloc=malloc;core.Realloc=realloc;core.Free=free;core.GMalloc=galloc;
    fs.LoadFile=load;mf.StripExtension=strip;mf.BIH_BuildAlias=bih;mf.GenMatrixPosQuat4Scale=matrix;
    filefuncs=&fs;modfuncs=&mf;hl2_dedicatedphy=&fast;hl2_propcollision=&mode;mode.ival=1;hl2_dedicated=true;
    mdl.h.num_bones=1;mdl.h.ofs_bones=offsetof(typeof(mdl),b);mdl.h.flags=HL2MDL_STATIC_PROP;
    mdl.b.parent=-1;mdl.b.name_ofs=sizeof(mdl.b);strcpy(mdl.name,"root");
    for(int k=0;k<3;k++){mdl.h.mins[k]=-2;mdl.h.maxs[k]=2;} mdl.b.pos[0]=.5f;
    makephy(); strcpy(a.name,"models/test.mdl");b=a;
    fast.ival=0;assert(Mod_LoadHL2Model(&a,&mdl,sizeof(mdl)));assert(renderreads==5 && piececalls==1);galiasinfo_t *control=traced;
    renderreads=0; fast.ival=1;assert(Mod_LoadHL2Model(&b,&mdl,sizeof(mdl)));assert(renderreads==0 && piececalls==2);
    assert(control->numverts==3 && traced->numindexes==3 && traced->contents==FTECONTENTS_BODY);
    assert(!memcmp(control->ofs_skel_xyz,traced->ofs_skel_xyz,3*sizeof(vecV_t)));
    assert(!memcmp(control->ofs_indexes,traced->ofs_indexes,3*sizeof(index_t)));
    assert(traced->ofs_indexes[0]==0 && traced->ofs_indexes[1]==2 && traced->ofs_indexes[2]==1);
    assert(fabs(traced->ofs_skel_xyz[1][0]-1)<1e-6 && fabs(traced->ofs_skel_xyz[2][1]-1)<1e-6);
    assert(!memcmp(a.mins,b.mins,sizeof(a.mins)) && !memcmp(a.maxs,b.maxs,sizeof(a.maxs)) && a.radius==b.radius);
    assert(!memcmp(((galiasinfo_t*)a.meshinfo)->baseframeofs,((galiasinfo_t*)b.meshinfo)->baseframeofs,12*sizeof(float)));
    puts("PASS static: render reads 5 -> 0; exact collision vertices/indices/contents, bounds/radius, bone matrices; convex pieces called twice");
    for(int arm=0;arm<11;arm++) {
        model_t m={0};strcpy(m.name,"models/test.mdl");makephy();mdl.h.num_sequences=0;mdl.h.num_anims=0;mode.ival=1;hl2_dedicated=true;
        if(arm==0)fixturelen=0;
        if(arm==1)fixturelen=23;
        if(arm==2)u16(24,257);
        if(arm==3){mdl.h.num_anims=1;mdl.h.ofs_anims=offsetof(typeof(mdl),anim);}
        if(arm==4)hl2_dedicated=false;
        if(arm==5)u32(8,2);
        if(arm==6)u32(176,0x7fc00000);
        if(arm==7)mdl.h.num_sequences=1;
        if(arm>=8)mode.ival=arm-8==0?0:arm-7;
        renderreads=0;assert(Mod_LoadHL2Model(&m,&mdl,sizeof(mdl)));assert(renderreads==5);
        galiasinfo_t *candidate=traced;
        model_t legacy={0};strcpy(legacy.name,m.name);fast.ival=0;
        assert(Mod_LoadHL2Model(&legacy,&mdl,sizeof(mdl))); fast.ival=1;
        assert(candidate->numindexes==traced->numindexes && candidate->contents==traced->contents);
        if(candidate->numindexes) {
            assert(candidate->numverts==traced->numverts);
            assert(!memcmp(candidate->ofs_indexes,traced->ofs_indexes,candidate->numindexes*sizeof(index_t)));
            assert(!memcmp(candidate->ofs_skel_xyz,traced->ofs_skel_xyz,candidate->numverts*sizeof(vecV_t)));
        }
        printf("PASS fallback arm %d: legacy render reads 5; exact collision output matches off\n",arm);
    }
    return 0;
}
