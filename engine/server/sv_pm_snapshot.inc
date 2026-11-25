/* P1827. Included by sv_user.c. PMS1 is 58 ordered finite numbers. Saved native
 * state is untrusted input: APPLY is gameplay, never evidence of verification.
 * A verifier must independently reproduce the retained endpoint before using it. */
#include "sv_pm_snapshot.h"
#include <float.h>
extern cvar_t sv_gravity;

unsigned int SV_PMSnapshotPin(const float *pin)
{
 unsigned int h = 2166136261u;
 int i, k;
 for (i = 0; i < SV_PMPIN_COUNT; ++i) {
  unsigned int bits;
  memcpy(&bits, pin+i, sizeof(bits));
  for (k = 0; k < 4; ++k) { h ^= (bits >> (k*8)) & 255; h *= 16777619u; }
 }
 return h;
}

void SV_PMSnapshotVector(const sv_pm_snapshot_t *s, int field, float *out)
{
 int i;
 for (i = 0; i < 3; ++i) out[i] = s->v[field+i];
}

static void SV_PMSnapshotStateValues(sv_pm_snapshot_t *s)
{
 const pmsourcestate_t *p = &s->state;
 double *v = s->v + PMS_STATE;
 v[0]=p->surfacefriction; v[1]=p->ducktime; v[2]=p->ducking; v[3]=p->ducked;
 v[4]=p->msec_carry; v[5]=p->oldbuttons; v[6]=p->stamina; v[7]=p->rampoff;
 v[8]=p->boardcount; v[9]=p->rampcontact; v[10]=p->srcladder;
 v[11]=p->srcladdernormal[0]; v[12]=p->srcladdernormal[1]; v[13]=p->srcladdernormal[2];
 v[14]=p->landticks; v[15]=p->portaloffset[0]; v[16]=p->portaloffset[1]; v[17]=p->portaloffset[2];
 v[18]=p->waterjumpremaining; v[19]=p->waterjumpdir[0]; v[20]=p->waterjumpdir[1]; v[21]=p->agwaterjump;
}

static qboolean SV_PMSnapshotInteger(double value, double lo, double hi)
{
 return value >= lo && value <= hi && floor(value) == value;
}

qboolean SV_PMSnapshotParse(const char *text, sv_pm_snapshot_t *s)
{
 int i;
 char *end;
 const char *p;
 double *v, *st;
 if (!text || strncmp(text,"PMS1 ",5)) return false;
 memset(s,0,sizeof(*s)); p=text+5; v=s->v;
 for (i=0; i<SV_PMSNAPSHOT_VALUES; ++i) {
  if (!*p || *p==' ' || *p=='\t' || *p=='\r' || *p=='\n') return false;
  v[i]=strtod(p,&end);
  if (end==p || !(v[i]>=-FLT_MAX && v[i]<=FLT_MAX)) return false;
  if (i+1<SV_PMSNAPSHOT_VALUES) { if (*end!=' ') return false; p=end+1; }
  else if (*end) return false;
 }
 if (v[PMS_MODEL]!=PHYSMODE_SOURCE && v[PMS_MODEL]!=PHYSMODE_AG) return false;
 if (!SV_PMSnapshotInteger(v[PMS_VERSION],1,65535) ||
     !SV_PMSnapshotInteger(v[PMS_EPOCH],1,4294967295.0) ||
     !SV_PMSnapshotInteger(v[PMS_MT],0,2147483647) ||
     !(v[PMS_TICK]>0 && v[PMS_TICK]<=1) ||
     !SV_PMSnapshotInteger(v[PMS_PIN],0,4294967295.0) ||
     !SV_PMSnapshotInteger(v[PMS_FLAGS],0,SV_PMSNAPSHOT_FLAGS) ||
     ((int)v[PMS_FLAGS] & ~SV_PMSNAPSHOT_FLAGS) ||
     !SV_PMSnapshotInteger(v[PMS_PMFLAGS],0,PMF_DUCKED|PMF_LADDER) ||
     ((int)v[PMS_PMFLAGS] & ~(PMF_DUCKED|PMF_LADDER)) ||
     !SV_PMSnapshotInteger(v[PMS_JUMP],0,1) || !SV_PMSnapshotInteger(v[PMS_GROUND],0,2) ||
     !SV_PMSnapshotInteger(v[PMS_WATERLEVEL],0,3) ||
     !SV_PMSnapshotInteger(v[PMS_WATERTYPE],-2147483648.0,2147483647) ||
     !SV_PMSnapshotInteger((float)v[PMS_WATERTYPE],-2147483648.0,2147483647) ||
     !SV_PMSnapshotInteger(v[PMS_LIMIT],0,511) || !(v[PMS_COOL]>=0 && v[PMS_COOL]<=60) ||
     !SV_PMSnapshotInteger(v[PMS_SLIDE],0,15)) return false;
 for (i=0;i<3;++i) {
  float lower=v[PMS_MINS+i], upper=v[PMS_MAXS+i];
  double size=(double)upper-(double)lower;
  /* CHECK and APPLY consume native floats. Validate size without overflowing
   * an intermediate float (including on builds with FP exceptions enabled). */
  if(!(lower<upper) || !(size>0 && size<=FLT_MAX)) return false;
 }
 if (((int)v[PMS_FLAGS]&FL_ONGROUND) ? v[PMS_GROUND]==0 : v[PMS_GROUND]!=0) return false;
 st=v+PMS_STATE;
 if (!SV_PMSnapshotInteger(st[2],0,1) || !SV_PMSnapshotInteger(st[3],0,1) ||
     !(st[4]>=0 && st[4]<v[PMS_TICK]) ||
     !((float)st[4]>=0 && (float)st[4]<(float)v[PMS_TICK]) ||
     !SV_PMSnapshotInteger(st[5],0,2147483647) ||
     !(st[7]>=0) || /* rampoff is elapsed seconds, NOT an integer counter */
     !SV_PMSnapshotInteger(st[8],-2147483648.0,2147483647) ||
     !SV_PMSnapshotInteger(st[9],-2147483648.0,2147483647) ||
     !SV_PMSnapshotInteger((float)st[8],-2147483648.0,2147483647) ||
     !SV_PMSnapshotInteger((float)st[9],-2147483648.0,2147483647) ||
     !SV_PMSnapshotInteger(st[10],0,1) || !SV_PMSnapshotInteger(st[14],0,PMLIM_LANDMAX) ||
     !(st[18]>=0 && st[18]<=2) || fabs(st[19])>50 || fabs(st[20])>50 ||
     !SV_PMSnapshotInteger(st[21],0,1) || (v[PMS_MODEL]==PHYSMODE_SOURCE && st[21])) return false;
 s->state.surfacefriction=st[0]; s->state.ducktime=st[1]; s->state.ducking=st[2]; s->state.ducked=st[3];
 s->state.msec_carry=st[4]; s->state.oldbuttons=st[5]; s->state.stamina=st[6]; s->state.rampoff=st[7];
 s->state.boardcount=st[8]; s->state.rampcontact=st[9]; s->state.srcladder=st[10];
 for(i=0;i<3;++i) { s->state.srcladdernormal[i]=st[11+i]; s->state.portaloffset[i]=st[15+i]; }
 s->state.landticks=st[14]; s->state.waterjumpremaining=st[18];
 s->state.waterjumpdir[0]=st[19]; s->state.waterjumpdir[1]=st[20]; s->state.agwaterjump=st[21];
 return true;
}

qboolean SV_PMSnapshotText(const sv_pm_snapshot_t *snapshot, char *out, size_t size)
{
 sv_pm_snapshot_t s=*snapshot;
 size_t used=4;
 int i;
 if (size<5) return false;
 memcpy(out,"PMS1",5); SV_PMSnapshotStateValues(&s);
 for(i=0;i<SV_PMSNAPSHOT_VALUES;++i) {
  if (!(s.v[i]>=-FLT_MAX && s.v[i]<=FLT_MAX) ||
      Q_snprintfz(out+used,size-used," %.17g",s.v[i])) { *out=0; return false; }
  used+=strlen(out+used);
 }
 return true;
}

qboolean SV_PMSnapshotNear(double a, double b, double tolerance)
{
 return a>=-FLT_MAX && a<=FLT_MAX && b>=-FLT_MAX && b<=FLT_MAX && fabs(a-b)<=tolerance;
}

qboolean SV_PMSnapshotClock(int base, int counter, int origin, int *ticks)
{
 long long value=(long long)base+(long long)counter-(long long)origin;
 if(base<0 || counter<0 || origin<0 || value<0 || value>2147483647LL) return false;
 *ticks=value; return true;
}

qboolean SV_PMSnapshotHorizon(int mt, float carry, int next, float nextcarry, float rate, float *duration)
{
 double dt;
 float maximum;
 if(mt<0 || next<mt || !(rate>0 && rate<=1) ||
    !(carry>=0 && carry<rate) || !(nextcarry>=0 && nextcarry<rate) ||
    (next==mt && nextcarry<carry)) return false;
 dt=((long long)next-(long long)mt)*(double)rate+(double)nextcarry-(double)carry;
 /* SV_RunCmdPart recursively halves over 50ms. A tickpart may instead finish
  * one carried tick with +0.01ms. These are transport/mover bounds, not a new
  * detector threshold; two float ulps cover horizon reconstruction rounding. */
 maximum=max(0.05f,rate+0.00001f);
 maximum=nextafterf(nextafterf(maximum,FLT_MAX),FLT_MAX);
 if(!(dt>=0 && dt<=maximum)) return false;
 *duration=dt; return true;
}

qboolean SV_PMSnapshotInput(const char *args, float rate)
{
 const char *p=args;
 char *end;
 double value[11];
 int i;
 for(i=0;i<11;++i) {
  if(!*p || *p==' ' || *p=='\t' || *p=='\r' || *p=='\n') return false;
  value[i]=strtod(p,&end);
  if(end==p || !(value[i]>=-FLT_MAX && value[i]<=FLT_MAX)) return false;
  if(i==0 || i==1 || i==9 || i==10) {
   const char *q;
   /* sscanf's %i must see the recorder's decimal integer, not octal/hex,
    * a floating spelling or an overflowing prefix. */
   if(end-p>1 && *p=='0') return false;
   for(q=p;q<end;++q) if(*q<'0' || *q>'9') return false;
  }
  if(i<10) { if(*end!=' ') return false; p=end+1; }
  else if(*end) return false;
 }
 if(!SV_PMSnapshotInteger(value[0],0,2147483647) || !SV_PMSnapshotInteger(value[1],0,2147483647) ||
    !SV_PMSnapshotInteger(value[9],0,2147483647) || !SV_PMSnapshotInteger(value[10],0,2147483647) ||
    !(value[2]>=0 && value[2]<rate)) return false;
 /* Movement is the native int usercmd field (Patch345), NOT a short. Check
  * after the REC float conversion, so INT_MAX rounding up cannot overflow
  * the later float-to-int cast. Angles are decoded SHORT2ANGLE values. */
 for(i=3;i<6;++i) {
  float command=value[i];
  if(!(command>=-2147483648.0f && command<2147483648.0f)) return false;
 }
 /* Signed-short SHORT2ANGLE, then the writer's %.4f: -180..179.9945.
  * This is the decoded protocol domain, not a movement/detection threshold. */
 for(i=6;i<9;++i) if(value[i]<-180.0 || value[i]>179.9945) return false;
 return true;
}

qboolean SV_PMSnapshotStateEqual(const pmsourcestate_t *a, const pmsourcestate_t *b)
{
 sv_pm_snapshot_t sa,sb;
 int i;
 memset(&sa,0,sizeof(sa)); memset(&sb,0,sizeof(sb)); sa.state=*a; sb.state=*b;
 SV_PMSnapshotStateValues(&sa); SV_PMSnapshotStateValues(&sb);
 if(!(a->msec_carry>=0) || !(b->msec_carry>=0)) return false;
 for(i=PMS_STATE;i<SV_PMSNAPSHOT_VALUES;++i) {
  /* The move duration is reconstructed from printed horizons. Its fractional
   * carry can differ by float arithmetic; use the existing session print floor,
   * not a new movement/speed allowance. All tick-integrated state stays exact. */
  if(!SV_PMSnapshotNear(sa.v[i],sb.v[i],i==PMS_STATE+4 ? 0.0001f : 0)) return false;
 }
 return true;
}

qboolean SV_PMSnapshotGravity(float entity, float world, float *cached, float *multiplier)
{
 volatile float g=entity*world;
 if(!(world>=-FLT_MAX && world<=FLT_MAX) || world==0 || !(g>=-FLT_MAX && g<=FLT_MAX)) return false;
 *cached=g;
 *multiplier=g/world;
 return *multiplier>=-FLT_MAX && *multiplier<=FLT_MAX;
}

/* Rebuild the current engine interpretation even on a fresh retry connection.
 * Game-authored map properties are deliberately NOT restored from user files:
 * pins on each side describe them, and their existing provenance gates remain.
 * No client setup/cache/message is touched by CHECK. */
static qboolean SV_PMSnapshotCompatible(const sv_pm_snapshot_t *s)
{
 movevars_t mv=movevars;
 qboolean ok;
 SV_SetSourceMoveVars();
 ok=s->v[PMS_MODEL]==movevars.physicsmode && s->v[PMS_VERSION]==movevars.sourceversion &&
    (float)s->v[PMS_TICK]==movevars.ticrate;
 movevars=mv;
 return ok;
}

qboolean SV_PMSnapshot(edict_t *e, const char *text, int operation, char *out, size_t size)
{
 extern cvar_t pm_physicsmode, pm_ticrate;
 sv_pm_snapshot_t s, parsed;
 client_t *cl;
 eval_t *f;
 int slot,i,k;
 static const char *fields[]={"run_momlimit","run_momcool","run_momslide"};
 *out=0;
 if (!svprogfuncs || svs.gametype!=GT_PROGS || !e || sv.state!=ss_active || operation<0 || operation>2) return false;
 slot=e->entnum-1;
 if(slot<0 || slot>=sv.allocated_client_slots || slot>=MAX_CLIENTS) return false;
 cl=&svs.clients[slot];
 if(cl->edict!=e || cl->state!=cs_spawned || cl->spectator || !(e->v->health>0) ||
    cl->protocol!=SCP_QUAKEWORLD || sv_nqplayerphysics.ival ||
    PR_FindFunction(svprogfuncs,"SV_RunClientCommand",PR_ANY)) return false;
 if(operation==0) {
  if(!cl->pmepoch || e->v->movetype!=MOVETYPE_WALK ||
     !SV_PMSnapshotInteger(cl->movetickcount,0,2147483647)) return false;
  memset(&s,0,sizeof(s));
  s.v[PMS_MODEL]=cl->pmpin[1]; s.v[PMS_VERSION]=SV_PMSrcVer(cl->pmpin);
  s.v[PMS_EPOCH]=cl->pmepoch; s.v[PMS_MT]=cl->movetickcount;
  s.v[PMS_TICK]=pm_ticrate.value; s.v[PMS_PIN]=SV_PMSnapshotPin(cl->pmpin);
  s.v[PMS_FLAGS]=(int)e->v->flags&SV_PMSNAPSHOT_FLAGS;
  s.v[PMS_PMFLAGS]=(int)e->xv->pmove_flags&(PMF_DUCKED|PMF_LADDER);
  s.v[PMS_JUMP]=cl->jump_held;
  s.v[PMS_GROUND]=((int)e->v->flags&FL_ONGROUND) ? (e->v->groundentity ? 2 : 1) : 0;
  for(i=0;i<3;++i) {
   s.v[PMS_ORG+i]=e->v->origin[i]; s.v[PMS_VEL+i]=e->v->velocity[i];
   s.v[PMS_MINS+i]=e->v->mins[i]; s.v[PMS_MAXS+i]=e->v->maxs[i];
   s.v[PMS_VIEW+i]=e->v->view_ofs[i]; s.v[PMS_GRAVDIR+i]=e->xv->gravitydir[i];
  }
  s.v[PMS_GRAVITY]=e->xv->gravity; s.v[PMS_ENTGRAVITY]=cl->entgravity;
  s.v[PMS_WATERLEVEL]=e->v->waterlevel; s.v[PMS_WATERTYPE]=e->v->watertype;
  s.v[PMS_TELEPORT]=e->v->teleport_time; /* QW publishes raw relative native waterjumptime */
  for(k=0;k<3;++k) { f=svprogfuncs->GetEdictFieldValue(svprogfuncs,e,fields[k],ev_float,NULL); s.v[PMS_LIMIT+k]=f?f->_float:0; }
  s.state=cl->pmsrc;
  for(i=0;i<SV_PMPIN_COUNT;++i) if(!(cl->pmpin[i]>=-FLT_MAX && cl->pmpin[i]<=FLT_MAX)) return false;
  return SV_PMSnapshotText(&s,out,size) && SV_PMSnapshotParse(out,&parsed);
 }
 if(!SV_PMSnapshotParse(text,&s) || s.v[PMS_MODEL]!=pm_physicsmode.ival ||
    !SV_PMSnapshotCompatible(&s)) return false;
 if(operation==1) { Q_strncpyz(out,"1",size); return true; }
 if(e->v->movetype!=MOVETYPE_WALK) return false;
 /* All validation precedes the first mutation. Live counter/epoch, map and
  * network freshness are deliberately NOT restored. */
 cl->pmsrc=s.state; cl->jump_held=s.v[PMS_JUMP];
 SV_PMSnapshotVector(&s,PMS_ORG,e->v->origin); VectorCopy(e->v->origin,e->v->oldorigin);
 SV_PMSnapshotVector(&s,PMS_VEL,e->v->velocity); SV_PMSnapshotVector(&s,PMS_MINS,e->v->mins);
 SV_PMSnapshotVector(&s,PMS_MAXS,e->v->maxs); SV_PMSnapshotVector(&s,PMS_VIEW,e->v->view_ofs);
 SV_PMSnapshotVector(&s,PMS_GRAVDIR,e->xv->gravitydir);
 VectorSubtract(e->v->maxs,e->v->mins,e->v->size);
 i=(int)s.v[PMS_FLAGS]; if(s.v[PMS_GROUND]==2) i&=~FL_ONGROUND;
 /* Source/AG movement consumes native jump_held, not legacy QC audio/jump
  * flags. Do not import those unrelated QC flags as snapshot authority. */
 e->v->flags=((int)e->v->flags&~FL_ONGROUND)|(i&FL_ONGROUND); e->v->groundentity=0;
 e->xv->pmove_flags=((int)e->xv->pmove_flags&~(PMF_DUCKED|PMF_LADDER))|(int)s.v[PMS_PMFLAGS];
 /* A property/cache is not independent mover state. Do not author gravity or
  * limits from an untrusted file, or erase their actual source. Refresh the
  * native gravity cache from the LIVE game-authored property through its normal
  * selector. Existing inherited-effect eligibility remains exactly in force. */
 SV_ClientGravityRefresh(cl);
 e->v->waterlevel=s.v[PMS_WATERLEVEL]; e->v->watertype=s.v[PMS_WATERTYPE];
 e->v->teleport_time=s.v[PMS_TELEPORT];
#define PMS_PUBLISH(name,value) do { f=svprogfuncs->GetEdictFieldValue(svprogfuncs,e,name,ev_float,NULL); if(f) f->_float=(value); } while(0)
 PMS_PUBLISH("run_forceduck",0); PMS_PUBLISH("run_movetick",cl->movetickcount);
 PMS_PUBLISH("run_movecarry",cl->pmsrc.msec_carry); PMS_PUBLISH("run_movetickrate",s.v[PMS_TICK]);
 PMS_PUBLISH("run_boardcount",cl->pmsrc.boardcount);
#undef PMS_PUBLISH
 World_LinkEdict(&sv.world,(wedict_t*)e,false);
 Q_strncpyz(out,"1",size); return true;
}

static qboolean SV_PMSnapshotTestScalar(const char *text, int slot, const char *scalar, char *out, size_t size)
{
 const char *q=text+5,*after;
 int i;
 if(strncmp(text,"PMS1 ",5)) return false;
 for(i=0;i<slot;++i) { q=strchr(q,' '); if(!q) return false; ++q; }
 if((size_t)(q-text)>=size) return false;
 after=strchr(q,' '); memcpy(out,text,q-text);
 Q_snprintfz(out+(q-text),size-(q-text),"%s%s",scalar,after?after:"");
 return true;
}

unsigned int SV_PMSnapshotTest(unsigned int *checks)
{
 sv_pm_snapshot_t s,p;
 char text[SV_PMSNAPSHOT_TEXT],bad[SV_PMSNAPSHOT_TEXT+64];
 unsigned int n=0,fail=0;
 int i;
#define PS_CHECK(x) do { ++n; if(!(x)) ++fail; } while(0)
 memset(&s,0,sizeof(s));
 s.v[PMS_MODEL]=PHYSMODE_SOURCE; s.v[PMS_VERSION]=15; s.v[PMS_EPOCH]=1;
 s.v[PMS_MT]=42; s.v[PMS_TICK]=0.0078125; s.v[PMS_PIN]=4294967295.0;
 for(i=0;i<3;++i) { s.v[PMS_MINS+i]=-16; s.v[PMS_MAXS+i]=16; }
 s.state.surfacefriction=1; s.state.landticks=PMLIM_LANDMAX;
 s.state.msec_carry=0.003f; s.state.portaloffset[2]=0.125f; s.state.rampoff=0.015f;
 PS_CHECK(SV_PMSnapshotText(&s,text,sizeof(text)));
 PS_CHECK(SV_PMSnapshotParse(text,&p));
 PS_CHECK(SV_PMSnapshotStateEqual(&s.state,&p.state));
 PS_CHECK(p.v[PMS_PIN]==4294967295.0 && p.v[PMS_MT]==42);
 Q_snprintfz(bad,sizeof(bad),"%s 0",text); PS_CHECK(!SV_PMSnapshotParse(bad,&p));
 Q_snprintfz(bad,sizeof(bad),"%s\n",text); PS_CHECK(!SV_PMSnapshotParse(bad,&p));
 PS_CHECK(!SV_PMSnapshotParse("PMS1 nan",&p));
 for(i=0;i<SV_PMSNAPSHOT_VALUES;++i) {
  const char *q=text+5; int j;
  for(j=0;j<i;++j) { q=strchr(q,' '); if(q) ++q; }
  if(!q) { PS_CHECK(false); break; }
  {
   const char *after=strchr(q,' ');
   memcpy(bad,text,q-text);
   Q_snprintfz(bad+(q-text),sizeof(bad)-(q-text),"nan%s",after?after:"");
  }
  PS_CHECK(!SV_PMSnapshotParse(bad,&p));
 }
 for(i=0;i<3;++i) {
  p=s; p.v[PMS_MINS+i]=1; p.v[PMS_MAXS+i]=1.00000001;
  PS_CHECK(SV_PMSnapshotText(&p,bad,sizeof(bad)) && !SV_PMSnapshotParse(bad,&p)); /* collapsed float hull */
  p=s; p.v[PMS_MINS+i]=-3e38; p.v[PMS_MAXS+i]=3e38;
  PS_CHECK(SV_PMSnapshotText(&p,bad,sizeof(bad)) && !SV_PMSnapshotParse(bad,&p)); /* overflowed native size */
  p=s; p.v[PMS_MINS+i]=1; p.v[PMS_MAXS+i]=nextafterf(1,2);
  PS_CHECK(SV_PMSnapshotText(&p,bad,sizeof(bad)) && SV_PMSnapshotParse(bad,&p)); /* positive native ulp */
 }
 {
  int integerfloats[]={PMS_WATERTYPE,PMS_STATE+8,PMS_STATE+9};
  PS_CHECK(SV_PMSnapshotTestScalar(text,PMS_STATE+4,"0.0078124999",bad,sizeof(bad)) &&
           !SV_PMSnapshotParse(bad,&p)); /* below double tick, equal native tick */
  {
   float belowtick=nextafterf((float)s.v[PMS_TICK],0);
   p=s; p.state.msec_carry=belowtick;
   PS_CHECK(SV_PMSnapshotText(&p,bad,sizeof(bad)) && SV_PMSnapshotParse(bad,&p) &&
            p.state.msec_carry==belowtick);
  }
  for(i=0;i<3;++i)
   PS_CHECK(SV_PMSnapshotTestScalar(text,integerfloats[i],"2147483647",bad,sizeof(bad)) &&
            !SV_PMSnapshotParse(bad,&p)); /* native float must remain in int domain */
 }
 p=s; p.state.portaloffset[2]+=1; PS_CHECK(!SV_PMSnapshotStateEqual(&s.state,&p.state));
 p=s; p.state.stamina+=1; PS_CHECK(!SV_PMSnapshotStateEqual(&s.state,&p.state));
 {
  float cached,ratio; volatile float raw=0.33333334f, world=800;
  volatile float expected=raw*world;
  PS_CHECK(SV_PMSnapshotGravity(raw,world,&cached,&ratio) && cached==expected && ratio==expected/world);
  PS_CHECK(SV_PMSnapshotGravity(-1,800,&cached,&ratio) && cached==-800 && ratio==-1);
  PS_CHECK(!SV_PMSnapshotGravity(1,0,&cached,&ratio)); /* no finite native pin */
 }
 {
  double badvalues[]={NAN,INFINITY,-INFINITY};
  float duration;
  int tick,j;
  for(i=0;i<3;++i) {
   PS_CHECK(!SV_PMSnapshotNear(badvalues[i],0,0.0001));
   PS_CHECK(!SV_PMSnapshotNear(0,badvalues[i],0.0001));
   PS_CHECK(!SV_PMSnapshotNear(badvalues[i],badvalues[i],0.0001));
   {
    float *fields[]={&p.state.surfacefriction,&p.state.ducktime,&p.state.msec_carry,
     &p.state.stamina,&p.state.rampoff,&p.state.boardcount,&p.state.rampcontact,
     &p.state.srcladdernormal[0],&p.state.srcladdernormal[1],&p.state.srcladdernormal[2],
     &p.state.portaloffset[0],&p.state.portaloffset[1],&p.state.portaloffset[2],
     &p.state.waterjumpremaining,&p.state.waterjumpdir[0],&p.state.waterjumpdir[1]};
    for(j=0;j<sizeof(fields)/sizeof(fields[0]);++j) {
     p=s; *fields[j]=badvalues[i];
     PS_CHECK(!SV_PMSnapshotStateEqual(&s.state,&p.state));
     PS_CHECK(!SV_PMSnapshotStateEqual(&p.state,&s.state));
    }
   }
  }
  PS_CHECK(SV_PMSnapshotHorizon(5,0,5,0,0.015f,&duration) && duration==0);
  PS_CHECK(SV_PMSnapshotHorizon(5,0.002f,5,0.004f,0.015f,&duration) && duration>0);
  PS_CHECK(SV_PMSnapshotHorizon(5,0,7,0,0.015f,&duration) && duration>0);
  PS_CHECK(!SV_PMSnapshotHorizon(5,0,4,0,0.015f,&duration));
  PS_CHECK(!SV_PMSnapshotHorizon(5,0.004f,5,0.002f,0.015f,&duration));
  PS_CHECK(!SV_PMSnapshotHorizon(5,0,2147483647,0,0.015f,&duration));
  PS_CHECK(!SV_PMSnapshotHorizon(5,NAN,6,0,0.015f,&duration));
  PS_CHECK(SV_PMSnapshotClock(10,25,20,&tick) && tick==15);
  PS_CHECK(!SV_PMSnapshotClock(2147483647,1,0,&tick));
  PS_CHECK(!SV_PMSnapshotClock(0,0,1,&tick));
  PS_CHECK(SV_PMSnapshotClock(2147483647,2147483647,2147483647,&tick) && tick==2147483647);
  PS_CHECK(SV_PMSnapshotInput("1 15 0.003 450 0 0 0 0 0 2 0",0.015f));
  PS_CHECK(!SV_PMSnapshotInput("1 15 nan 450 0 0 0 0 0 2 0",0.015f));
  PS_CHECK(!SV_PMSnapshotInput("1 4294967296 0 450 0 0 0 0 0 2 0",0.015f));
  PS_CHECK(SV_PMSnapshotInput("1 15 0 40000 0 0 0 0 0 2 0",0.015f));
  PS_CHECK(!SV_PMSnapshotInput("1 15 0 2147483648 0 0 0 0 0 2 0",0.015f));
  PS_CHECK(!SV_PMSnapshotInput("1 015 0 450 0 0 0 0 0 2 0",0.015f));
  PS_CHECK(!SV_PMSnapshotInput("1 15e0 0 450 0 0 0 0 0 2 0",0.015f));
  PS_CHECK(!SV_PMSnapshotInput("1 0xf 0 450 0 0 0 0 0 2 0",0.015f));
  PS_CHECK(SV_PMSnapshotInput("1 15 0 450 0 0 -180 0 0 2 0",0.015f));
  PS_CHECK(SV_PMSnapshotInput("1 15 0 450 0 0 179.9945 0 0 2 0",0.015f));
  PS_CHECK(!SV_PMSnapshotInput("1 15 0 450 0 0 180 0 0 2 0",0.015f));
  PS_CHECK(!SV_PMSnapshotInput("1 15 0 450 0 0 360 0 0 2 0",0.015f));
  PS_CHECK(!SV_PMSnapshotInput("1 15 0 450 0 0 -360 0 0 2 0",0.015f));
  PS_CHECK(!SV_PMSnapshotInput("1 15 0 450 0 0 -180.0001 0 0 2 0",0.015f));
  PS_CHECK(!SV_PMSnapshotInput("1 15 0 450 0 0 179.9946 0 0 2 0",0.015f));
 }
#undef PS_CHECK
 if(checks) *checks=n;
 return fail;
}
