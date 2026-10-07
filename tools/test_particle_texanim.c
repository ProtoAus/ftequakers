/* Compile with: cc -std=c99 -Wall -Wextra -Werror
 * tools/test_particle_texanim.c -lm -o test_particle_texanim
 * Tests the actual selector included by p_script.c, not a second algorithm. */
#include <assert.h>
#include <stdio.h>
#include "../engine/client/p_texanim.h"

int main(void)
{
	particle_texframe_t f[] = {
		{1, 0, 0, .25f, 1},
		{3, .25f, 0, .5f, 1},
		{4, .5f, 0, 1, 1}
	};
	particle_texframe_t full[PARTICLE_TEXFRAME_MAX];
	int mode, i;
	for (i = 0; i < PARTICLE_TEXFRAME_MAX; i++)
	{
		full[i] = f[i%3];
		full[i].end = i+1;
	}
	assert(P_TexAnimFrame(full, PARTICLE_TEXFRAME_MAX, PTEX_LOOP, 241.1, 1, 1) == 241);
	assert(P_TexAnimFrame(full, PARTICLE_TEXFRAME_MAX, PTEX_LIFETIME, .99, 1, 1) == 253);
	assert(P_TexAnimFrame(NULL, 0, PTEX_LOOP, 1, 1, 1) == -1);
	assert(P_TexAnimFrame(f, PARTICLE_TEXFRAME_MAX+1, PTEX_LOOP, 1, 1, 1) == -1);
	assert(P_TexAnimFrame(f, 1, PTEX_LOOP, 100, 1, 1) == 0);
	assert(P_TexAnimFrame(f, 3, PTEX_STATIC, 100, 1, 1) == 0);
	assert(P_TexAnimFrame(f, 3, PTEX_CLAMP, -1, 1, 1) == 0);
	assert(P_TexAnimFrame(f, 3, PTEX_CLAMP, .99, 1, 1) == 0);
	assert(P_TexAnimFrame(f, 3, PTEX_CLAMP, 1, 1, 1) == 1);
	assert(P_TexAnimFrame(f, 3, PTEX_CLAMP, 2.99, 1, 1) == 1);
	assert(P_TexAnimFrame(f, 3, PTEX_CLAMP, 3, 1, 1) == 2);
	assert(P_TexAnimFrame(f, 3, PTEX_CLAMP, 100, 1, 1) == 2);
	assert(P_TexAnimFrame(f, 3, PTEX_LOOP, 4, 1, 1) == 0);
	assert(P_TexAnimFrame(f, 3, PTEX_LOOP, 5, 1, 1) == 1);
	assert(P_TexAnimFrame(f, 3, PTEX_LOOP, 7, 1, 1) == 2);
	assert(P_TexAnimFrame(f, 3, PTEX_LOOP, 8, 1, 1) == 0);
	assert(P_TexAnimFrame(f, 3, PTEX_CLAMP, .5, 1, 2) == 1);
	assert(P_TexAnimFrame(f, 3, PTEX_LIFETIME, 1, 4, 1) == 1);
	assert(P_TexAnimFrame(f, 3, PTEX_LIFETIME, 3, 4, 1) == 2);
	assert(P_TexAnimFrame(f, 3, PTEX_LIFETIME, 4, 4, 1) == 2);
	assert(P_TexAnimFrame(f, 3, PTEX_LIFETIME, .25, 1, 1) == 1);
	assert(P_TexAnimFrame(f, 3, PTEX_LIFETIME, .75, 1, 1) == 2);
	assert(P_TexAnimFrame(f, 3, PTEX_LIFETIME, 1, 0, 1) == 2);
	assert(P_TexAnimFrame(f, 3, PTEX_LIFETIME, .75, 1, .5) == 1);
	assert(P_TexAnimFrame(f, 3, PTEX_LOOP, 1, 1, 0) == 0);
	assert(P_TexAnimFrame(f, 3, PTEX_CLAMP, 1, 1, -1) == -1);
	assert(P_TexAnimFrame(f, 3, PTEX_LOOP, NAN, 1, 1) == -1);
	assert(P_TexAnimFrame(f, 3, PTEX_LOOP, INFINITY, 1, 1) == -1);
	assert(P_TexAnimFrame(f, 3, PTEX_LOOP, 1, 1, INFINITY) == -1);
	assert(P_TexAnimFrame(f, 3, PTEX_LIFETIME, 1, NAN, 1) == 2);
	for (mode = PTEX_STATIC; mode <= PTEX_CLAMP; mode++)
		for (i = -10; i <= 10000; i++)
		{
			int frame = P_TexAnimFrame(f, 3, mode, i/1000.0, 1.2, 1);
			assert(frame >= 0 && frame < 3);
		}
	puts("particle texanim: boundaries, nonuniform durations, randomized lifespan, loop/clamp/rate/static and finite safety PASS");
	return 0;
}
