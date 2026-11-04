/* P1308: directional gain guide. Inputs stay independent of assisted turning.
 * This is a local next-step objective, not collision or route lookahead. */
#ifndef SV_TAS_CONTROL_H
#define SV_TAS_CONTROL_H
#include <math.h>
#include "sv_tas_strafe.h"

typedef struct
{
	float vx, vy, forward, side, maxspeed, accel, dt, surface;
	float aircap, friction, stopspeed, amount, duckspeed;
	int ground, ducked;
} sv_tas_guide_t;

/* Cache the actual body word, not integer degrees from a naked macro cast.
 * Signed rounding precedes the defined unsigned wrap, including negative yaw. */
static unsigned short SV_TASPlanAngleWord(float degrees)
{
	double word;
	if (!SV_TASStrafeFinite(degrees)) return 0;
	word = fmod((double)degrees,360.0)*(65536.0/360.0);
	return (unsigned short)(int)floor(word+.5);
}

static double SV_TASGuideScore(const sv_tas_guide_t *g, unsigned short yaw)
{
	float r = (float)(yaw*(6.2831853071795864769/65536.0));
	float x = g->forward*cosf(r)+g->side*sinf(r);
	float y = g->forward*sinf(r)-g->side*cosf(r);
	float norm = sqrtf(x*x+y*y), wish = norm, vx = g->vx, vy = g->vy;
	float speed, control, after, cap, add, impulse;
	x /= norm; y /= norm;
	if (wish > g->maxspeed) wish = g->maxspeed;
	if (g->ground && g->ducked) wish *= g->duckspeed > 0 ? g->duckspeed : .34f;
	if (g->ground)
	{
		speed = sqrtf(vx*vx+vy*vy);
		control = speed < g->stopspeed ? g->stopspeed : speed;
		after = speed-control*(g->friction*g->surface)*g->dt;
		if (after < 0) after = 0;
		if (speed >= .1f && after != speed) { vx *= after/speed; vy *= after/speed; }
	}
	cap = g->ground ? wish : (wish < g->aircap ? wish : g->aircap);
	add = cap-(vx*x+vy*y);
	if (add > 0)
	{
		impulse = g->accel*g->dt*wish*g->surface;
		if (impulse > add) impulse = add;
		vx += impulse*x; vy += impulse*y;
	}
	return (double)vx*vx+(double)vy*vy;
}

static int SV_TASGuideYaw(const sv_tas_guide_t *g, unsigned short *yaw)
{
	double speed, wish, impulse, cap, dot, theta, bearing, offset, angle;
	float finite[13];
	unsigned int i;
	int step;
	if (!g || !yaw || (g->ground != 0 && g->ground != 1) ||
		(g->ducked != 0 && g->ducked != 1)) return 0;
	finite[0]=g->vx; finite[1]=g->vy; finite[2]=g->forward; finite[3]=g->side;
	finite[4]=g->maxspeed; finite[5]=g->accel; finite[6]=g->dt;
	finite[7]=g->surface; finite[8]=g->aircap; finite[9]=g->friction;
	finite[10]=g->stopspeed; finite[11]=g->amount; finite[12]=g->duckspeed;
	for (i=0;i<13;++i) if (!SV_TASStrafeFinite(finite[i])) return 0;
	if (!g->side || g->maxspeed <= 0 || g->accel <= 0 ||
		g->dt <= 0 || g->dt > .02f || g->surface <= 0 || g->aircap <= 0 ||
		g->friction < 0 || g->stopspeed < 0 || g->amount < 0 || g->amount > 4 ||
		(g->ground && g->ducked && g->duckspeed > 1))
		return 0;
	speed = sqrt((double)g->vx*g->vx + (double)g->vy*g->vy);
	if (speed < .0001) return 0; /* no bearing: retain the actual view */
	wish = sqrt((double)g->forward*g->forward + (double)g->side*g->side);
	if (wish > g->maxspeed) wish = g->maxspeed;
	if (g->ground && g->ducked) wish *= g->duckspeed > 0 ? g->duckspeed : .34f;
	if (g->ground)
	{
		double control = speed < g->stopspeed ? g->stopspeed : speed;
		speed -= control * g->friction * g->surface * g->dt;
		if (speed <= .0001) return 0;
	}
	impulse = (double)g->accel * wish * g->dt * g->surface;
	cap = g->ground ? wish : (wish < g->aircap ? wish : g->aircap);
	/* Quantized production gain reference; amount scales the yaw delta below. */
	dot = cap - impulse;
	if (dot < 0) dot = 0;
	if (dot > speed) dot = speed;
	if (dot < -speed) dot = -speed;
	theta = acos(dot/speed);
	bearing = atan2(g->vy,g->vx);
	offset = atan2(-g->side,g->forward);
	angle = (bearing + (g->side < 0 ? theta : -theta) - offset) *
		(65536.0 / 6.2831853071795864769);
	*yaw = (unsigned short)((int)floor(angle+.5) & 65535);
	{
		unsigned short center = *yaw;
		double best = SV_TASGuideScore(g,center);
		/* The cap is a cusp: nearest-angle rounding need not be the best
		 * quantized command. Score its neighboring actual float bases. */
		for (step=-8;step<=8;++step)
		{
			unsigned short candidate = (unsigned short)((center+step)&65535);
			double score = SV_TASGuideScore(g,candidate);
			if (score > best) { best = score; *yaw = candidate; }
		}
	}
	return 1;
}

/* Bounded shortest-angle change, never multiply absolute compass bearings. */
static unsigned short SV_TASGuideSlew(unsigned short from, unsigned short target,
	float dt, float amount)
{
	int change = (int)((target-from+32768u)&65535u)-32768;
	double scaled, limit;
	if (!SV_TASStrafeFinite(dt) || !SV_TASStrafeFinite(amount) ||
		dt <= 0 || dt > .02f || amount <= 0 || amount > 4) return from;
	scaled = change * (double)amount;
	limit = 360.0 * dt * amount * (65536.0/360.0);
	if (scaled > limit) scaled = limit;
	if (scaled < -limit) scaled = -limit;
	change = scaled >= 0 ? (int)floor(scaled+.5) : -(int)floor(-scaled+.5);
	return (unsigned short)((from+change)&65535);
}
#endif
