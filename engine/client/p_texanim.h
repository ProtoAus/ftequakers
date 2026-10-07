/* Single-channel, discrete particle UV animation. GPL-2.0-or-later. */
#ifndef FTE_P_TEXANIM_H
#define FTE_P_TEXANIM_H

#include <math.h>

#define PARTICLE_TEXFRAME_MAX 256

typedef struct
{
	float end; /* cumulative duration; strictly increasing */
	float s1, t1, s2, t2;
} particle_texframe_t;

typedef enum
{
	PTEX_STATIC, PTEX_LIFETIME, PTEX_LOOP, PTEX_CLAMP
} particle_texanim_t;

/* No allocations, random draws or renderer state changes. Half-open frame
 * intervals select the next frame at an exact boundary. Lifetime mode uses
 * the particle's actual randomized lifespan, not the type's maximum die. */
static int P_TexAnimFrame(const particle_texframe_t *frames, int count,
	particle_texanim_t mode, double age, double lifetime, double rate)
{
	double pos, total;
	int lo = 0, hi = count - 1, mid;
	if (!frames || count < 1 || count > PARTICLE_TEXFRAME_MAX)
		return -1;
	total = frames[hi].end;
	if (!isfinite(age) || !isfinite(total) || total <= 0)
		return -1;
	if (age <= 0 || mode == PTEX_STATIC)
		return 0;
	if (!isfinite(rate) || rate < 0)
		return -1;
	if (mode == PTEX_LIFETIME)
	{
		if (!isfinite(lifetime) || lifetime <= 0)
			return count - 1;
		pos = age / lifetime * total * rate;
	}
	else
		pos = age * rate;
	if (!isfinite(pos))
		return -1;
	if (mode == PTEX_LOOP)
		pos = fmod(pos, total);
	else if (pos >= total)
		return count - 1;
	while (lo < hi)
	{
		mid = lo + (hi - lo) / 2;
		if (pos < frames[mid].end)
			hi = mid;
		else
			lo = mid + 1;
	}
	return lo;
}

#endif
