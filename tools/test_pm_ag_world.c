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
	int duration[]={10,4};
	Reset(); pmove.velocity[0]=600; pmove.cmd.buttons=BUTTON_JUMP;
	PMAg_PlayerMove(1);
	NEAR(pmove.origin[0],6,0.0001f); NEAR(pmove.origin[2],38.643282f,0.0001f);
	NEAR(pmove.velocity[2],260.328157f,0.0001f); CHECK(!pmove.onground); CHECK(touches>0);
	Reset(); pmove.cmd.forwardmove=320; PMAg_PlayerMove(1);
	NEAR(pmove.velocity[0],32,0.0001f); NEAR(pmove.origin[0],0.32f,0.0001f); CHECK(pmove.onground);
	for (rate=0;rate<2;rate++)
	{
		Reset(); pmove.origin[2]=1000; pmove.cmd.msec=duration[rate]; movevars.ticrate=duration[rate]==10 ? 0.01f : 0.004f; movevars.agprofile=1000/duration[rate]; total=0;
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
	CHECK(!PMAg_ProfileRate("ag-100-v1",1,0.004f)); CHECK(!PMAg_ProfileRate("ag-1000-v1",1,0.001f));
	movevars.agprofile=0; pmove.cmd.forwardmove=320; PMAg_PlayerMove(1); CHECK(pmove.ticksrun==0); CHECK(pmove.origin[0]==0);
	movevars.agprofile=100; movevars.ticrate=0.001f; PMAg_PlayerMove(1); CHECK(pmove.ticksrun==0);
	movevars.ticrate=0.01f; movevars.sourceversion=2; PMAg_PlayerMove(1); CHECK(pmove.ticksrun==0);
	CHECK(PMAg_CommandSlot(0.004f,66,30)==0); CHECK(PMAg_CommandSlot(0.004f,66,32)==32);
	CHECK(PMAg_CommandSlot(0.004f,66,0)==16); CHECK(PMAg_CommandSlot(0.01f,75,0)==10);
	puts("PASS binding/refusal and packet ceiling250/30 refused,250/32=8ticks");
}
int main(void) {Dry();Clock();CollisionAndNoEquipment();Duck();WaterLadder();ReviewedControls();puts("PASS AG world solver fixtures (not BSP/socket/oracle acceptance)");return 0;}
