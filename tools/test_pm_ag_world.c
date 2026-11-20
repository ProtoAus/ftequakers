/* Actual AG solver, in-memory collision fixture (not installed AG runtime parity).
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "quakedef.h"
#include "pm_ag.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while (0)
#define NEAR(a,b,e) CHECK(fabsf((a)-(b)) < (e))
playermove_t pmove;
movevars_t movevars;
FTE_TLS playermove_t *pm_ctx = &pmove;
FTE_TLS movevars_t *mv_ctx = &movevars;
static float floorz, ceilingz, waterz, wallx, stepz, floorend;
static qboolean forcevolume;
static int extraboxcalls;
static unsigned int liquid;
static qboolean ladder;
static int touches;

void QDECL CrossProduct(const vec3_t a, const vec3_t b, vec3_t out)
{
	out[0]=a[1]*b[2]-a[2]*b[1]; out[1]=a[2]*b[0]-a[0]*b[2]; out[2]=a[0]*b[1]-a[1]*b[0];
}
float QDECL VectorNormalize(vec3_t vector)
{
	float length = sqrtf(DotProduct(vector,vector));
	if (length) VectorScale(vector,1/length,vector);
	return length;
}
void QDECL AngleVectors(const vec3_t angles, vec3_t forward, vec3_t right, vec3_t up)
{
	float yaw=angles[1]*(float)(M_PI/180), pitch=angles[0]*(float)(M_PI/180);
	if (forward) VectorSet(forward,cosf(pitch)*cosf(yaw),cosf(pitch)*sinf(yaw),-sinf(pitch));
	if (right) VectorSet(right,sinf(yaw),-cosf(yaw),0);
	if (up) VectorSet(up,sinf(pitch)*cosf(yaw),sinf(pitch)*sinf(yaw),cosf(pitch));
}
void PM_AddTouchedEnt(int ent) { if (ent>=0) touches++; }
int PM_PointContents(vec3_t point)
{
	unsigned int result=0;
	if (point[2]<floorz || point[2]>ceilingz) result|=FTECONTENTS_SOLID;
	if (point[2]<waterz && (!forcevolume || point[0]<100)) result|=liquid;
	if (ladder && point[0]>-20 && point[0]<20) result|=FTECONTENTS_LADDER;
	return result;
}
int PM_LadderEntPlane(vec3_t origin, vec3_t normal) { (void)origin; (void)normal; return 0; } // Patch 1655: no brush ladder here
int PM_ExtraBoxContents(vec3_t point)
{
	(void)point; extraboxcalls++;
	return forcevolume && pmove.origin[0]-16<100 && pmove.origin[2]-36<waterz ? liquid : 0;
}
trace_t PM_PlayerTrace(vec3_t start,vec3_t end,unsigned int mask)
{
	trace_t trace;
	float fraction;
	int axis;
	memset(&trace,0,sizeof(trace)); trace.fraction=1; trace.entnum=0; VectorCopy(end,trace.endpos);
	if ((start[0]+pmove.player_mins[0]<=floorend && start[2]+pmove.player_mins[2]<floorz-0.001f) || start[2]+pmove.player_maxs[2]>ceilingz+0.001f ||
	    (start[0]+pmove.player_maxs[0]>wallx+0.001f && start[2]+pmove.player_mins[2]<stepz-0.001f))
	{
		trace.startsolid=trace.allsolid=true; trace.fraction=0; VectorCopy(start,trace.endpos); return trace;
	}
	if (end[0]+pmove.player_mins[0]<=floorend && end[2]+pmove.player_mins[2]<floorz && end[2]<start[2])
	{
		fraction=(start[2]+pmove.player_mins[2]-floorz)/(start[2]-end[2]);
		trace.fraction=fraction; trace.plane.normal[2]=1;
	}
	if (end[2]+pmove.player_maxs[2]>ceilingz && end[2]>start[2])
	{
		fraction=(ceilingz-start[2]-pmove.player_maxs[2])/(end[2]-start[2]);
		if (fraction<trace.fraction) {trace.fraction=fraction;VectorSet(trace.plane.normal,0,0,-1);}
	}
	if (start[0]+pmove.player_maxs[0]<=wallx && end[0]+pmove.player_maxs[0]>wallx && start[2]+pmove.player_mins[2]<stepz)
	{
		fraction=(wallx-start[0]-pmove.player_maxs[0])/(end[0]-start[0]);
		if (fraction<trace.fraction) {trace.fraction=fraction;VectorSet(trace.plane.normal,-1,0,0);}
	}
	if (end[0]+pmove.player_maxs[0]>wallx && end[2]+pmove.player_mins[2]<stepz && end[2]<start[2])
	{
		fraction=(start[2]+pmove.player_mins[2]-stepz)/(start[2]-end[2]);
		if (fraction>=0 && fraction<trace.fraction) {trace.fraction=fraction;VectorSet(trace.plane.normal,0,0,1);}
	}
	if (ladder && (mask&FTECONTENTS_LADDER) && end[0]>start[0])
	{
		fraction=(10-start[0])/(end[0]-start[0]);
		if (fraction>=0 && fraction<trace.fraction) {trace.fraction=fraction;VectorSet(trace.plane.normal,-1,0,0);}
	}
	for (axis=0;axis<3;axis++) trace.endpos[axis]=start[axis]+trace.fraction*(end[axis]-start[axis]);
	return trace;
}
static void Reset(void)
{
	memset(&pmove,0,sizeof(pmove)); memset(&movevars,0,sizeof(movevars));
	wallx=10000; stepz=0; floorend=10000; forcevolume=false; extraboxcalls=0;
	floorz=0; ceilingz=10000; waterz=-10000; liquid=FTECONTENTS_WATER; ladder=false; touches=0;
	pmove.pm_type=PM_NORMAL; pmove.viewheight=28; pmove.origin[2]=36;
	movevars.ticrate=0.01f; movevars.physicsmode=PHYSMODE_AG; movevars.sourceversion=PMAG_VERSION; movevars.agprofile=100;
	movevars.gravity=800; movevars.entgravity=1; movevars.friction=4; movevars.stopspeed=100;
	movevars.edgefriction=2; movevars.accelerate=movevars.airaccelerate=10;
	movevars.maxspeed=320; movevars.stepheight=18; movevars.maxvelocity=2000;
	pmove.cmd.msec=10;
}
static void Dry(void)
{
	int rate,i,total;
	int duration[]={10,4,1};
	Reset(); pmove.velocity[0]=600; pmove.cmd.buttons=BUTTON_JUMP;
	PMAg_PlayerMove(1);
	NEAR(pmove.origin[0],6,0.0001f); NEAR(pmove.origin[2],38.643282f,0.0001f);
	NEAR(pmove.velocity[2],260.328157f,0.0001f); CHECK(!pmove.onground); CHECK(touches>0);
	Reset(); pmove.cmd.forwardmove=320; PMAg_PlayerMove(1);
	NEAR(pmove.velocity[0],32,0.0001f); NEAR(pmove.origin[0],0.32f,0.0001f); CHECK(pmove.onground);
	for (rate=0;rate<3;rate++)
	{
		Reset(); pmove.origin[2]=1000; pmove.cmd.msec=duration[rate]; movevars.ticrate=duration[rate]==10 ? 0.01f : duration[rate]==4 ? 0.004f : 0.001f; movevars.agprofile=1000/duration[rate]; total=0;
		for(i=0;i<1000/duration[rate];i++) {PMAg_PlayerMove(1);total+=pmove.ticksrun;}
		CHECK(total==1000/duration[rate]); NEAR(pmove.tickused,duration[rate]*0.001f,0.000001f); NEAR(pmove.origin[2],600,0.02f);
		printf("PASS actual solver %d Hz free-fall/clock: z %.6f, native ms %d\n",1000/duration[rate],pmove.origin[2],total);
	}
}
static void CollisionAndNoEquipment(void)
{
	Reset(); wallx=18; stepz=16; pmove.velocity[0]=320; pmove.cmd.forwardmove=320;
	PMAg_PlayerMove(1); NEAR(pmove.origin[0],3.2f,0.001f); NEAR(pmove.origin[2],52,0.001f);
	CHECK(pmove.onground);
	Reset(); wallx=18; stepz=32; pmove.velocity[0]=320; pmove.cmd.forwardmove=320;
	PMAg_PlayerMove(1); CHECK(pmove.origin[0]<=2.001f); NEAR(pmove.origin[2],36,0.001f);
	Reset(); pmove.ducked=true; pmove.origin[2]=18; pmove.velocity[0]=300;
	pmove.cmd.forwardmove=320; pmove.cmd.buttons=BUTTON_JUMP|BUTTON_DUCK;
	PMAg_PlayerMove(1); CHECK(pmove.velocity[0]<=320); NEAR(pmove.velocity[2],260.328157f,0.001f);
	puts("PASS actual solver 16u step, blocked 32u wall and crouched fast ordinary jump (no longjump equipment)");
}
static void Clock(void)
{
	int rate;
	float tick[] = {0.01f, 0.004f};
	for (rate=0;rate<2;rate++)
	{
		Reset(); movevars.ticrate=tick[rate]; movevars.agprofile=rate ? 250 : 100; pmove.origin[2]=1000;
		pmove.cmd.msec=255; PMAg_PlayerMove(1);
		CHECK(pmove.ticksrun==8); NEAR(pmove.msec_carry,0,0.000001f);
		NEAR(pmove.origin[2],1000-400*64*tick[rate]*tick[rate],0.002f);
		pmove.cmd.msec=0; PMAg_PlayerMove(1); CHECK(pmove.ticksrun==0);
	}
	Reset(); pmove.cmd.msec=7; PMAg_PlayerMove(1);
	CHECK(pmove.ticksrun==0); NEAR(pmove.msec_carry,0.007f,0.000001f);
	pmove.cmd.msec=3; PMAg_PlayerMove(1);
	CHECK(pmove.ticksrun==1); NEAR(pmove.msec_carry,0,0.000001f);
	CHECK(pmove.cmd.msec==3);
	puts("PASS actual fixed-step carry, zero-credit and eight-executed-tick work/clock cap at qualified 100/250 Hz");
}
static void Duck(void)
{
	int i;
	Reset(); pmove.cmd.buttons=BUTTON_DUCK;
	PMAg_PlayerMove(1); CHECK(pmove.ducking && !pmove.ducked);
	for(i=0;i<20;i++) PMAg_PlayerMove(1);
	NEAR(pmove.viewheight,11,0.0001f); NEAR(pmove.origin[2],36,0.001f);
	for(i=0;i<20;i++) PMAg_PlayerMove(1);
	CHECK(pmove.ducked && !pmove.ducking); NEAR(pmove.origin[2],18,0.001f); NEAR(pmove.viewheight,12,0.001f);
	ceilingz=60; pmove.cmd.buttons=0; PMAg_PlayerMove(1);
	CHECK(pmove.ducked); NEAR(pmove.origin[2],18,0.001f);
	ceilingz=10000; PMAg_PlayerMove(1); CHECK(!pmove.ducked); NEAR(pmove.origin[2],36,0.001f);
	Reset(); pmove.origin[2]=200; pmove.cmd.buttons=BUTTON_DUCK; PMAg_PlayerMove(1);
	CHECK(pmove.ducked); NEAR(pmove.origin[2],199.96f,0.001f); /* no air origin shift */
	Reset(); pmove.cmd.buttons=BUTTON_DUCK; PMAg_PlayerMove(1); for(i=0;i<20;i++) PMAg_PlayerMove(1);
	pmove.cmd.buttons=0; PMAg_PlayerMove(1); CHECK(!pmove.ducked && !pmove.ducking); CHECK(pmove.origin[2]>53);
	Reset(); pmove.origin[2]=200; pmove.cmd.buttons=BUTTON_DUCK; pmove.cmd.forwardmove=320; PMAg_PlayerMove(1);
	NEAR(pmove.velocity[0],30,0.001f); pmove.velocity[0]=0; PMAg_PlayerMove(1);
	NEAR(pmove.velocity[0],10.656f,0.001f);
	puts("PASS actual solver ground/air crouch, 400ms eye/hull, blocked unduck, partial release and next-command input crop");
}
static void WaterLadder(void)
{
	float full,small;
	Reset(); pmove.origin[2]=100; waterz=1000; PMAg_PlayerMove(1);
	CHECK(pmove.waterlevel==3); NEAR(pmove.velocity[2],-4.8f,0.001f);
	Reset(); pmove.origin[2]=100; waterz=1000; pmove.cmd.forwardmove=320; PMAg_PlayerMove(1);
	NEAR(pmove.velocity[0],25.6f,0.001f);
	Reset(); pmove.origin[2]=100; waterz=1000; pmove.cmd.buttons=BUTTON_JUMP; PMAg_PlayerMove(1);
	CHECK(pmove.velocity[2]>90 && pmove.velocity[2]<100);
	Reset(); ladder=true; pmove.cmd.forwardmove=320; PMAg_PlayerMove(1); full=pmove.velocity[2]; CHECK(full>199);
	Reset(); ladder=true; pmove.cmd.forwardmove=1; PMAg_PlayerMove(1); small=pmove.velocity[2]; NEAR(full,small,0.001f);
	pmove.cmd.forwardmove=0; PMAg_PlayerMove(1); NEAR(pmove.velocity[2],0,0.001f);
	pmove.cmd.forwardmove=-1; PMAg_PlayerMove(1); CHECK(pmove.velocity[2]<-199);
	pmove.cmd.sidemove=1; PMAg_PlayerMove(1); CHECK(pmove.velocity[1]<-199);
	pmove.cmd.forwardmove=0; pmove.cmd.sidemove=-1; PMAg_PlayerMove(1); CHECK(pmove.velocity[1]>199);
	pmove.cmd.buttons=BUTTON_JUMP; PMAg_PlayerMove(1); CHECK(!pmove.srcladder); CHECK(pmove.velocity[0]<-269);
	Reset(); pmove.cmd.forwardmove=1; PMAg_PlayerMove(1); CHECK(!pmove.srcladder); CHECK(fabsf(pmove.velocity[2])<1);
	puts("PASS actual solver water idle/swim/jump; analog-sign ladder tiny/full/neutral/negative/both/side/jump and nonladder");
}
static void ReviewedControls(void)
{
	playermove_t crouch;
	int i;
	for (i=0;i<2;i++)
	{
		Reset(); pmove.ducked=true; pmove.origin[2]=18; pmove.cmd.buttons=BUTTON_DUCK;
		pmove.cmd.msec=i; pmove.viewheight=12; crouch=pmove;
		Reset(); PMAg_PlayerMove(1); NEAR(pmove.viewheight,28,0.00001f);
		pmove=crouch; pmove.viewheight=28; PMAg_PlayerMove(1);
		NEAR(pmove.viewheight,12,0.00001f); NEAR(pmove.player_maxs[2],18,0.00001f);
		Reset(); pmove.ducking=true; pmove.ducktime=800; pmove.cmd.buttons=BUTTON_DUCK;
		pmove.cmd.msec=i; pmove.viewheight=28; PMAg_PlayerMove(1);
		NEAR(pmove.viewheight,11,0.00001f); NEAR(pmove.player_maxs[2],36,0.00001f);
	}
	Reset(); pmove.ducked=true; pmove.origin[2]=18; pmove.viewheight=28; pmove.cmd.buttons=BUTTON_DUCK;
	PMAg_PlayerMove(1); NEAR(pmove.viewheight,12,0.00001f);
	puts("PASS interleaved completed/partial posture at zero/subtick/full tick");
	Reset(); ladder=true; pmove.cmd.forwardmove=320; pmove.angles[0]=45; PMAg_PlayerMove(1);
	NEAR(pmove.velocity[2],0,0.0001f);
	Reset(); ladder=true; pmove.cmd.forwardmove=320; pmove.angles[0]=-45; PMAg_PlayerMove(1);
	NEAR(pmove.velocity[2],282.842712f,0.001f);
	puts("PASS pitched ladder full tangent +45=0, -45=282.8427");
	Reset(); forcevolume=true; waterz=70; pmove.origin[2]=60; PMAg_PlayerMove(1);
	CHECK(pmove.waterlevel==2); CHECK(extraboxcalls==1); // ladder broadphase only
	Reset(); forcevolume=true; waterz=70; pmove.origin[0]=110; pmove.origin[2]=60; PMAg_PlayerMove(1);
	CHECK(pmove.waterlevel==0); CHECK(extraboxcalls==1);
	puts("PASS force-water point samples: dry head level2, overlapping dry point level0");
	Reset(); floorend=8; pmove.velocity[0]=200; PMAg_PlayerMove(1); NEAR(pmove.velocity[0],192,0.001f);
	Reset(); floorend=-2; pmove.velocity[0]=200; PMAg_PlayerMove(1); NEAR(pmove.velocity[0],184,0.001f);
	puts("PASS adjudicated current-hull edge probe near192/far184; point mutant near184");
	Reset(); CHECK(PMAg_ContextValid(&movevars));
	CHECK(PMAg_ProfileRate("ag-100-v1",1,0.01f)==100); CHECK(PMAg_ProfileRate("ag-250-v1",1,0.004f)==250);
	CHECK(!PMAg_ProfileRate("ag-100-v1",2,0.01f)); CHECK(!PMAg_ProfileRate("",1,0.01f));
	CHECK(!PMAg_ProfileRate("ag-100-v1",1,0.004f)); CHECK(PMAg_ProfileRate("ag-1000-v1",1,0.001f)==1000);
	CHECK(PMAg_ProfileRate("cs16-v1",1,0.01f)==100); CHECK(!PMAg_ProfileRate("hl2-pre-v1",1,0.015f));
	CHECK(!PMAg_ProfileRate("ag-1000-v1",1,0.01f)); CHECK(!PM_ProfileBySelector("ag-500"));
	CHECK(PM_ProfileBySelector("cs16")->kind==PMPROFILE_CS16); CHECK(PM_ProfileBySelector("hl2-post")->kind==PMPROFILE_HL2POST);
	movevars.profilekind=PMPROFILE_HL2PRE; CHECK(!PMAg_ContextValid(&movevars)); movevars.profilekind=PMPROFILE_CS16; CHECK(PMAg_ContextValid(&movevars));
	movevars.profilekind=0;
	movevars.agprofile=0; pmove.cmd.forwardmove=320; PMAg_PlayerMove(1); CHECK(pmove.ticksrun==0); CHECK(pmove.origin[0]==0);
	movevars.agprofile=100; movevars.ticrate=0.001f; PMAg_PlayerMove(1); CHECK(pmove.ticksrun==0);
	movevars.ticrate=0.01f; movevars.sourceversion=2; PMAg_PlayerMove(1); CHECK(pmove.ticksrun==0);
	CHECK(PMAg_CommandSlot(0.004f,66,30)==0); CHECK(PMAg_CommandSlot(0.004f,66,32)==32);
	CHECK(PMAg_CommandSlot(0.004f,66,0)==16); CHECK(PMAg_CommandSlot(0.01f,75,0)==10);
	CHECK(PMAg_CommandSlot(0.001f,100,0)==10); CHECK(PMAg_CommandSlot(0.001f,100,77)==13); CHECK(PMAg_CommandSlot(0.001f,100,30)==0);
	puts("PASS binding/refusal and packet ceiling250/30 refused,250/32=8ticks, 1000/77=13 ticks, 1000/30 refused");
	Reset(); movevars.ticrate=0.001f; movevars.agprofile=1000; pmove.origin[2]=1000;
	pmove.cmd.msec=255; PMAg_PlayerMove(1);
	CHECK(pmove.ticksrun==32); NEAR(pmove.msec_carry,0,0.000001f);
	NEAR(pmove.origin[2],1000-400*1024*0.001f*0.001f,0.002f);
	puts("PASS 1000 Hz stalled command credits the 32-tick budget");
}
// CS 1.6: fuser2 jump/land stamina, PreventMegaBunnyJumping and the 17-unit eye.
static void CS16(void)
{
	Reset(); movevars.profilekind=PMPROFILE_CS16; movevars.maxspeed=250; movevars.accelerate=5; movevars.stopspeed=75;
	pmove.cmd.buttons=BUTTON_JUMP; PMAg_PlayerMove(1);
	NEAR(pmove.velocity[2],260.328157f,0.0001f); NEAR(pmove.stamina,1315.789429f,0.001f); CHECK(fabsf(pmove.viewheight-17)<0.001f);
	Reset(); movevars.profilekind=PMPROFILE_CS16; movevars.maxspeed=250; movevars.accelerate=5; movevars.stopspeed=75;
	pmove.stamina=510; pmove.cmd.buttons=BUTTON_JUMP; PMAg_PlayerMove(1);
	NEAR(pmove.velocity[2],268.3281573f*0.905f-8,0.001f);
	puts("PASS CS16 jump: fresh 260.33, after 500 ms of fuser2 0.905 x impulse, recharge 1315.79");
	Reset(); movevars.profilekind=PMPROFILE_CS16; movevars.maxspeed=250; movevars.accelerate=5; movevars.stopspeed=75;
	pmove.stamina=510; pmove.velocity[0]=200; pmove.onground=true; PMAg_PlayerMove(1);
	NEAR(pmove.velocity[0],192*0.905f,0.001f);
	Reset(); pmove.velocity[0]=200; pmove.stamina=510; PMAg_PlayerMove(1); NEAR(pmove.velocity[0],192,0.001f);
	puts("PASS CS16 landing: friction 192 then x0.905 inside fuser2; plain AG ignores the field");
	Reset(); movevars.profilekind=PMPROFILE_CS16; movevars.maxspeed=250; movevars.accelerate=5; movevars.stopspeed=75;
	pmove.velocity[0]=400; pmove.cmd.buttons=BUTTON_JUMP; PMAg_PlayerMove(1);
	NEAR(pmove.velocity[0],400*(300/sqrtf(400*400+16))*0.8f,0.01f);
	Reset(); pmove.velocity[0]=400; pmove.cmd.buttons=BUTTON_JUMP; PMAg_PlayerMove(1); NEAR(pmove.velocity[0],400,0.001f);
	puts("PASS CS16 bhop cap: 400 u/s jump drops to 0.8 x 300; plain AG keeps 400");
	Reset(); movevars.profilekind=PMPROFILE_CS16; movevars.maxspeed=250; movevars.accelerate=5; movevars.stopspeed=75;
	pmove.cmd.forwardmove=320; pmove.cmd.buttons=BUTTON_DUCK; PMAg_PlayerMove(1);
	NEAR(pmove.velocity[0],5*250*0.333f*0.01f,0.001f); // clamp to maxspeed, then crop (PM_CheckParamters)
	Reset(); movevars.maxspeed=250; movevars.accelerate=5; movevars.stopspeed=75;
	pmove.cmd.forwardmove=320; pmove.cmd.buttons=BUTTON_DUCK; PMAg_PlayerMove(1);
	NEAR(pmove.velocity[0],12.5f,0.001f);
	puts("PASS CS16 duck crop from the first ducking tick, after the maxspeed clamp (4.16 u/s); plain AG crops only once ducked (12.5)");
	Reset(); pmove.ducked=true; pmove.origin[2]=18; pmove.cmd.forwardmove=450; pmove.cmd.buttons=BUTTON_DUCK; PMAg_PlayerMove(1);
	NEAR(pmove.velocity[0],10*320*0.333f*0.01f,0.001f);
	puts("PASS AG ducked 450 forward: clamp to 320 then crop, wish 106.6 (not 149.9)");
	Reset(); movevars.maxspeed=320; pmove.origin[2]=1000; pmove.cmd.sidemove=450; pmove.cmd.upmove=200; pmove.cmd.buttons=BUTTON_JUMP; PMAg_PlayerMove(1);
	NEAR(fabsf(pmove.velocity[1]),30,0.001f); // air: client upmove 200 with jump held must not shrink the side wish (P1542 behaviour)
	Reset(); pmove.cmd.sidemove=450; pmove.cmd.upmove=200; PMAg_PlayerMove(1); NEAR(fabsf(pmove.velocity[1]),32,0.001f);
	puts("PASS uncropped moves ignore the clamp: held-jump upmove 200 leaves the side wish at 320");
}
// Patch 1591: under pm_autobunny a held jump leaves on the first grounded tick; off, one press is one jump.
static int HeldHops(int commands, int *ground, float *apex)
{
	int i, hops=0, run=0; float vz;
	*ground=0; pmove.cmd.buttons=BUTTON_JUMP;
	for (i=0;i<commands;i++)
	{
		vz=pmove.velocity[2]; PMAg_PlayerMove(1);
		if (vz<=0 && pmove.velocity[2]>0) hops++;
		if (apex && hops && hops<=2 && pmove.origin[2]-36>apex[hops-1]) apex[hops-1]=pmove.origin[2]-36;
		run=pmove.onground ? run+1 : 0; if (run>*ground) *ground=run;
	}
	return hops;
}
static void Autohop(void)
{
	int ground,hops; float apex[2]={0,0};
	Reset(); hops=HeldHops(200,&ground,NULL); CHECK(hops==1); CHECK(ground>100);
	printf("PASS autobunny off: 2 s held = %d jump, then %d grounded ticks\n",hops,ground);
	Reset(); movevars.autobunny=true; hops=HeldHops(200,&ground,NULL); CHECK(hops>=3); CHECK(ground==1);
	printf("PASS autobunny 100 Hz: 2 s held = %d jumps, longest ground %d tick\n",hops,ground);
	Reset(); movevars.autobunny=true; movevars.ticrate=0.001f; movevars.agprofile=1000; pmove.cmd.msec=1;
	hops=HeldHops(2000,&ground,NULL); CHECK(hops>=3); CHECK(ground==1);
	printf("PASS autobunny 1000 Hz: 2 s held = %d jumps, longest ground %d tick\n",hops,ground);
	Reset(); movevars.autobunny=true; movevars.profilekind=PMPROFILE_CS16; movevars.maxspeed=250; movevars.accelerate=5; movevars.stopspeed=75;
	pmove.velocity[0]=400; hops=HeldHops(200,&ground,apex); CHECK(hops>=2); CHECK(ground==1);
	NEAR(apex[0],45,0.5f); CHECK(apex[1]>20 && apex[1]<apex[0]*0.9f); NEAR(pmove.velocity[0],240,1);
	printf("PASS CS16 autohop: %d jumps, apex %.2f then %.2f (fuser2), speed %.2f after the 400 cap\n",hops,apex[0],apex[1],pmove.velocity[0]);
}
/* Patch 1665: the server's pm_viewheight is the eye (17 on a Counter-Strike map under ag-250); 0 and Source's 64 are not
   AG eyes and leave the profile's. The duck still ends at 12 and its blend starts from the published eye. */
static void ServerEye(void)
{
	int i;
	Reset(); movevars.viewheight=17; PMAg_PlayerMove(1); NEAR(pmove.viewheight,17,0.0001f);
	pmove.cmd.buttons=BUTTON_DUCK; PMAg_PlayerMove(1); CHECK(pmove.ducking);
	for(i=0;i<20;i++) PMAg_PlayerMove(1);
	NEAR(pmove.viewheight,5.5f,0.0001f);
	for(i=0;i<20;i++) PMAg_PlayerMove(1);
	CHECK(pmove.ducked); NEAR(pmove.viewheight,12,0.001f);
	ceilingz=10000; pmove.cmd.buttons=0; PMAg_PlayerMove(1); CHECK(!pmove.ducked); NEAR(pmove.viewheight,17,0.0001f);
	/* the water test keeps the profile's eye: a surface between the shown 17 and the own 28 is waist deep (2), not 3,
	   standing and at half duck (shown 5.5, own 11) */
	Reset(); movevars.viewheight=17; waterz=60; PMAg_PlayerMove(1); NEAR(pmove.viewheight,17,0.0001f); CHECK(pmove.waterlevel==2);
	waterz=50; PMAg_PlayerMove(1); CHECK(pmove.waterlevel==2);
	waterz=66; PMAg_PlayerMove(1); CHECK(pmove.waterlevel==3);
	Reset(); movevars.viewheight=17; waterz=45; pmove.cmd.buttons=BUTTON_DUCK; PMAg_PlayerMove(1);
	for(i=0;i<20;i++) PMAg_PlayerMove(1);
	NEAR(pmove.viewheight,5.5f,0.0001f); CHECK(pmove.ducking && pmove.waterlevel==2);
	Reset(); movevars.viewheight=64; PMAg_PlayerMove(1); NEAR(pmove.viewheight,28,0.0001f);
	Reset(); movevars.viewheight=64; movevars.profilekind=PMPROFILE_CS16; PMAg_PlayerMove(1); NEAR(pmove.viewheight,17,0.0001f);
	Reset(); movevars.viewheight=28; movevars.profilekind=PMPROFILE_CS16; PMAg_PlayerMove(1); NEAR(pmove.viewheight,28,0.0001f);
}
/* Patch 1687: noclip (PM_SPECTATOR) flies like Source's. No input holds still (the swim wish sank 60 u/s), the speed is
   pm_noclipspeed x sv_maxspeed (the swim wish capped it at sv_maxspeed: 250 under cs16), +speed doubles it, and a held
   jump does not climb while +moveup does. */
static void Noclip(void)
{
	int i;
	Reset(); pmove.pm_type=PM_SPECTATOR; pmove.origin[2]=500; movevars.noclipspeed=4;
	for(i=0;i<100;i++) PMAg_PlayerMove(1);
	NEAR(pmove.origin[2],500,0.0001f); NEAR(pmove.origin[0],0,0.0001f);
	Reset(); pmove.pm_type=PM_SPECTATOR; pmove.origin[2]=500; movevars.noclipspeed=4; movevars.maxspeed=250;
	movevars.profilekind=PMPROFILE_CS16; pmove.cmd.forwardmove=400; pmove.cmd.buttons=BUTTON_DUCK;
	for(i=0;i<100;i++) PMAg_PlayerMove(1);
	NEAR(pmove.origin[0],1000,0.01f); NEAR(pmove.origin[2],500,0.0001f);
	Reset(); pmove.pm_type=PM_SPECTATOR; pmove.origin[2]=500; movevars.noclipspeed=4; pmove.cmd.forwardmove=400;
	pmove.cmd.buttons=BUTTON_SPEED; for(i=0;i<50;i++) PMAg_PlayerMove(1);
	NEAR(pmove.origin[0],1280,0.01f); NEAR(pmove.velocity[0],2560,0.01f);	/* not the 2000 maxvelocity clamp */
	Reset(); pmove.pm_type=PM_SPECTATOR; pmove.origin[2]=500; movevars.noclipspeed=4; pmove.cmd.upmove=200;
	pmove.cmd.buttons=BUTTON_JUMP; for(i=0;i<50;i++) PMAg_PlayerMove(1);
	NEAR(pmove.origin[2],500,0.0001f);
	pmove.cmd.buttons=0; for(i=0;i<50;i++) PMAg_PlayerMove(1);
	NEAR(pmove.origin[2],1140,0.01f);
	printf("PASS noclip: still holds z, cs16 1000 u/s (ducked too), +speed 2560, held jump stays, +moveup climbs\n");
}
int main(void) {Dry();Clock();CollisionAndNoEquipment();CS16();Autohop();Duck();ServerEye();Noclip();WaterLadder();ReviewedControls();puts("PASS AG world solver fixtures (not BSP/socket/oracle acceptance)");return 0;}
