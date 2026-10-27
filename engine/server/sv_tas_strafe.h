/* P1074: observed-state air-acceleration objective, not collision/route search. */
#ifndef SV_TAS_STRAFE_H
#define SV_TAS_STRAFE_H
#include <math.h>

static int SV_TASStrafeFinite(float value)
{
	union { float f; unsigned int u; } bits;
	bits.f = value;
	return (bits.u & 0x7f800000u) != 0x7f800000u;
}

/* Require matching positive server/mover caps and an unchanged head (including
 * vertical friction regime). Divergent custom caps use the preset. Whole AIR
 * acceleration plus float margin must fit before claiming an unclipped gain. */
static int SV_TASStrafeUnclipped(float vx, float vy, float vz, float accel,
	float limit, float serverlimit)
{
	double bound;
	if (!SV_TASStrafeFinite(vx) || !SV_TASStrafeFinite(vy) || !SV_TASStrafeFinite(vz) ||
		!SV_TASStrafeFinite(accel) || !SV_TASStrafeFinite(limit) ||
		!SV_TASStrafeFinite(serverlimit) || accel < 0) return 0;
	if (limit <= 0) limit = 3500; /* PMSrc_CheckVelocity default */
	if (serverlimit <= 0 || serverlimit != limit || fabs((double)vz) > limit) return 0;
	bound = (double)accel * 1.0001 + 0.01;
	return fabs((double)vx) + bound < limit && fabs((double)vy) + bound < limit;
}

/* Input is already an admitted synthetic TAS command. Both sides use Source's
 * uncapped wishspeed acceleration against a capped along-wish budget. */
static double SV_TASStrafeGain(double vx, double vy, double accel,
	double cap, int side, unsigned short yaw)
{
	double r = (double)yaw * (6.2831853071795864769 / 65536.0);
	double x = side * sin(r), y = -side * cos(r);
	double dot = vx*x + vy*y, add = cap - dot;
	/* Quantization must not reverse the selected turn (allow float trig noise). */
	if (side * (vx*y-vy*x) > sqrt(vx*vx+vy*vy)*0.000001) return -1e300;
	if (add <= 0) return 0;
	if (add > accel) add = accel;
	return 2*add*dot + add*add;
}

/* Nearest command-yaw neighbors bracket the continuous objective's maximum.
 * side=-1 means A/left; +1 means D/right. Flat input pitch does not affect yaw. */
static int SV_TASStrafeYaw(float vx, float vy, float wishspeed, float airaccel,
	float tick, float friction, float aircap, int side, unsigned short *yaw)
{
	double speed, cap, accel, dot, theta, target, best, gain;
	unsigned short candidate, chosen;
	int center, i;
	if (!SV_TASStrafeFinite(vx) || !SV_TASStrafeFinite(vy) || !SV_TASStrafeFinite(wishspeed) ||
		!SV_TASStrafeFinite(airaccel) || !SV_TASStrafeFinite(tick) || !SV_TASStrafeFinite(friction) ||
		!SV_TASStrafeFinite(aircap) || wishspeed <= 0 || airaccel <= 0 || tick <= 0 ||
		friction <= 0 || aircap <= 0 || (side != -1 && side != 1)) return 0;
	speed = sqrt((double)vx*vx + (double)vy*vy);
	if (speed < 0.0001) return 0; /* no velocity bearing; keep the explicit preset */
	cap = wishspeed < aircap ? wishspeed : aircap;
	accel = (double)airaccel * wishspeed * tick * friction;
	dot = cap - accel;
	if (dot < 0) dot = 0;
	if (dot > speed) dot = speed;
	theta = acos(dot/speed);
	/* Left wish=velocity+theta, but A is +90deg from the camera; mirror for D. */
	target = atan2(vy, vx) + side * (1.5707963267948966192 - theta);
	center = (int)floor(target * (65536.0 / 6.2831853071795864769) + 0.5);
	chosen = (unsigned short)center;
	best = SV_TASStrafeGain(vx, vy, accel, cap, side, chosen);
	for (i = -2; i <= 2; ++i)
	{
		candidate = (unsigned short)(center+i);
		gain = SV_TASStrafeGain(vx, vy, accel, cap, side, candidate);
		if (gain > best)
		{
			best = gain;
			chosen = candidate;
		}
	}
	*yaw = chosen;
	return 1;
}
#endif
