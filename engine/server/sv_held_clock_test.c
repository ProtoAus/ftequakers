#include <math.h>
#include <stdio.h>
#include <string.h>
#include "sv_held_clock.h"

static unsigned int checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

static held_clock_replay_t Begin(void)
{
	held_clock_replay_t r;
	held_clock_t initial;
	held_clock_context_t context = { .15f, 1, .2f, .5f, 4, .15f, 16, 524288, 2 };
	memset(&r, 0, sizeof(r));
	memset(&initial, 0, sizeof(initial));
	initial.counted = 1;
	initial.state = 2;
	initial.ckcb = 4;
	CHECK(SV_HeldClockBegin(&r, &initial, &context));
	return r;
}

static int FullPost(held_clock_replay_t *r, float time, float ticks, float rate,
	float carry, const float origin[3])
{
	return SV_HeldClockAccounting(r, time, ticks, rate, carry) &&
		SV_HeldClockPacket(r) && SV_HeldClockRecordFrame(r) &&
		SV_HeldClockLastOrigin(r, origin);
}

int main(void)
{
	held_clock_replay_t r, other;
	held_clock_t observed;
	float origin[3] = { 1, 2, 3 };
	unsigned int i;
	float expected;

	/* Real elapsed deltas, including zero-tick carry, freeze and accounting thaw. */
	r = Begin();
	CHECK(SV_HeldClockFrame(&r, .125f, 0));
	CHECK(FullPost(&r, .125f, 1, .015f, .0025f, origin));
	CHECK(r.clock.ticks == 1 && r.clock.elapsed == 1 * .015f + .0025f);
	CHECK(SV_HeldClockRequest(&r, 1));
	CHECK(FullPost(&r, .25f, 2, .015f, .005f, origin));
	expected = 2 * .015f + .005f;
	CHECK(r.clock.frzon == 1 && r.clock.frzat == expected && r.clock.frzticks == 0);
	CHECK(FullPost(&r, .375f, 2, .015f, .0125f, origin));
	CHECK(r.clock.frzticks == 0 && r.clock.elapsed == expected);
	CHECK(FullPost(&r, .5f, 3, .015f, 0, origin));
	CHECK(r.clock.frzticks == 1 && r.clock.elapsed == expected);
	CHECK(SV_HeldClockRequest(&r, 0));
	CHECK(FullPost(&r, .625f, 4, .015f, .001f, origin));
	CHECK(r.clock.frzticks == 2 && !r.clock.frzon && !r.clock.frzat);
	expected = (4 * .015f + .001f) - r.clock.frzsec;
	CHECK(r.clock.elapsed == expected && r.clock.ticks == 2);
	CHECK(r.clock.packet == 5 && r.clock.flags == 1 && r.clock.lastorg[2] == 3);
	observed = r.clock;
	CHECK(SV_HeldClockCompare(&r, &observed));
	/* Every observation scalar is compared, never installed to repair the model. */
	for (i = 0; i < sizeof(observed)/sizeof(float); ++i)
	{
		float words[36];
		memcpy(words, &r.clock, sizeof(words));
		words[i] = nextafterf(words[i], INFINITY);
		memcpy(&observed, words, sizeof(observed));
		CHECK(!SV_HeldClockCompare(&r, &observed));
	}
	CHECK(SV_HeldClockAccounting(&r, .75f, 5, .015f, 0));
	CHECK(r.clock.packet == 5 && r.clock.ticks == 2 && r.clock.lastorg[2] == 3);
	CHECK(SV_HeldClockPacket(&r));
	CHECK(r.clock.packet == 6 && r.clock.ticks == 2);
	CHECK(SV_HeldClockFinish(&r));
	CHECK(r.clock.ticks == 3 && r.clock.lastorg[2] == 3);
	origin[2] = 4;
	CHECK(SV_HeldClockLastOrigin(&r, origin));
	CHECK(r.clock.lastorg[2] == 4);
	CHECK(r.clock.state == 3);
	CHECK(!SV_HeldClockRequest(&r, 1));
	CHECK(r.failed);

	/* A second initial horizon is fatal, even if its values agree. */
	r = Begin();
	CHECK(!SV_HeldClockBegin(&r, &r.clock, &r.context));
	CHECK(r.failed);

	/* Pinning is not a completed release; no invented close-thaw. */
	r = Begin();
	CHECK(SV_HeldClockRequest(&r, 1));
	CHECK(FullPost(&r, .01f, 1, .01f, 0, origin));
	CHECK(SV_HeldClockAccounting(&r, .02f, 2, .01f, 0));
	CHECK(SV_HeldClockPacket(&r));
	CHECK(!SV_HeldClockFinish(&r));
	CHECK(r.clock.frzon == 1);
	r = Begin();
	CHECK(!SV_HeldClockRequest(&r, 0));
	r = Begin();
	CHECK(SV_HeldClockRequest(&r, 1));
	CHECK(!SV_HeldClockRequest(&r, 1));

	/* Monitor stall credit actually acts; a server stall is independently excluded. */
	r = Begin();
	CHECK(SV_HeldClockFrame(&r, .4f, 0));
	CHECK(FullPost(&r, .4f, 1, .01f, 0, origin));
	CHECK(r.clock.ckgaps == 1 && r.clock.ckncr == 1);
	CHECK(r.clock.ckcr > .38f && r.clock.ckcr < .4f && r.clock.ckcb == 3);
	CHECK(r.clock.ckd == 0);
	other = Begin();
	CHECK(FullPost(&other, .4f, 1, .01f, 0, origin));
	CHECK(other.clock.ckgaps == 0 && other.clock.ckncr == 0);
	CHECK(other.clock.ckd == -.01f);

	/* No credits: repeated lost game time must produce the slow-clock flag. */
	r = Begin();
	r.clock.ckcb = 0;
	for (i = 1; i <= 8; ++i)
	{
		CHECK(SV_HeldClockFrame(&r, i * .4f, 0));
		CHECK(FullPost(&r, i * .4f, (float)i, .01f, 0, origin));
	}
	CHECK(r.clock.ckd > 3 && r.clock.ckup == 1);
	CHECK(SV_HeldClockRequest(&r, 1));
	CHECK(FullPost(&r, 3.21f, 9, .01f, 0, origin));
	CHECK(SV_HeldClockRequest(&r, 0));
	CHECK(FullPost(&r, 3.22f, 10, .01f, 0, origin));
	CHECK(SV_HeldClockAccounting(&r, 3.23f, 11, .01f, 0));
	CHECK(SV_HeldClockPacket(&r));
	CHECK(SV_HeldClockFinish(&r));
	CHECK(r.clock.flags == 524289);

	/* Accounting snapshots precede packet++, RecFrame ticks and lastorg. */
	r = Begin();
	CHECK(FullPost(&r, .1f, 1, .01f, 0, origin));
	CHECK(SV_HeldClockRequest(&r, 1));
	observed = r.clock;
	CHECK(SV_HeldClockAccounting(&r, .2f, 2, .01f, 0));
	CHECK(r.clock.packet == observed.packet && r.clock.ticks == observed.ticks);
	CHECK(!memcmp(r.clock.lastorg, observed.lastorg, sizeof(observed.lastorg)));
	observed = r.clock;
	CHECK(SV_HeldClockCompare(&r, &observed));
	observed.packet += 1;
	CHECK(!SV_HeldClockCompare(&r, &observed));
	CHECK(SV_HeldClockPacket(&r));
	CHECK(r.clock.packet == observed.packet && r.clock.ticks == 1);
	CHECK(SV_HeldClockRecordFrame(&r));
	CHECK(r.clock.ticks == 2);
	CHECK(SV_HeldClockLastOrigin(&r, origin));
	CHECK(!SV_HeldClockRecordFrame(&r));
	r = Begin();
	CHECK(!SV_HeldClockPacket(&r));
	r = Begin();
	CHECK(SV_HeldClockAccounting(&r, .1f, 1, .01f, 0));
	CHECK(!SV_HeldClockRecordFrame(&r));
	r = Begin();
	CHECK(SV_HeldClockAccounting(&r, .1f, 1, .01f, 0));
	CHECK(!SV_HeldClockAccounting(&r, .1f, 1, .01f, 0));

	/* Counter/carry/NaN inputs are refused, not clamped into a comparable clock. */
	r = Begin();
	CHECK(!FullPost(&r, .1f, 1.5f, .01f, 0, origin));
	r = Begin();
	CHECK(!FullPost(&r, .1f, 1, .01f, .01f, origin));
	r = Begin();
	CHECK(!FullPost(&r, NAN, 1, .01f, 0, origin));
	r = Begin();
	CHECK(FullPost(&r, .1f, 3, .01f, 0, origin));
	CHECK(!FullPost(&r, .2f, 2, .01f, 0, origin));
	printf("held clock: %u checks, %u failures\n", checks, failures);
	return failures ? 1 : 0;
}
