#include <stdio.h>
#include <string.h>
#include "sv_held_coverage.h"

static unsigned int checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

static const held_boundary_t positive[] = {
	HELD_FRAME_ENTER, HELD_FRAME_EXIT,
	HELD_MESSAGE_ENTER, HELD_COMMAND_ENTER, HELD_COMMAND_EXIT,
	HELD_PIECE_BEGIN, HELD_PRE_ENTER, HELD_PRE_EXIT, HELD_MOVER_ENTER, HELD_MOVER_EXIT,
	HELD_PIECE_BEGIN, HELD_PRE_ENTER, HELD_PRE_EXIT, HELD_MOVER_ENTER, HELD_MOVER_EXIT,
	HELD_POST_ENTER, HELD_POST_EXIT,
	HELD_POST_ENTER, HELD_POST_EXIT, HELD_MESSAGE_EXIT,
	HELD_MESSAGE_ENTER, HELD_POST_ENTER, HELD_POST_EXIT, HELD_MESSAGE_EXIT
};

static held_coverage_t Begin(void)
{
	held_coverage_t c;
	memset(&c, 0, sizeof(c));
	CHECK(SV_HeldCoverageBegin(&c));
	return c;
}

int main(void)
{
	held_coverage_t c;
	unsigned int i, omit, operation, message;
	memset(&c, 0, sizeof(c));
	for (i = 0; i < sizeof(positive)/sizeof(positive[0]); ++i)
		CHECK(SV_HeldCoverageBoundary(&c, positive[i]));
	CHECK(!c.events && !c.messages && !c.executions && !SV_HeldCoverageClosed(&c));
	c = Begin();
	for (i = 0; i < sizeof(positive)/sizeof(positive[0]); ++i)
		CHECK(SV_HeldCoverageBoundary(&c, positive[i]));
	CHECK(SV_HeldCoverageClosed(&c));
	CHECK(c.executions == 2 && c.pre_enters == 2 && c.pre_exits == 2);
	CHECK(c.movers == 2 && c.completions == 2 && c.messages == 2);
	CHECK(c.post_enters == 3 && c.post_exits == 3);
	CHECK(c.commands == 1 && c.frames == 1 && c.scene_fences == 14);
	CHECK(!SV_HeldCoverageBegin(&c));

	/* Same four-message shape as the portable witness fixture. Native dispatch
	 * counters must not be inferred from the number of serialized callbacks. */
	c = Begin();
#define B(x) CHECK(SV_HeldCoverageBoundary(&c,x))
#define C(x) CHECK(SV_HeldCoverageControl(&c,x))
	for (message = 0; message < 4; ++message)
	{
		B(HELD_MESSAGE_ENTER);
		if (message == 1 || message == 2)
		{
			B(HELD_COMMAND_ENTER); C(message == 1 ? HELD_PIN_ENTRY:HELD_PIN_RELEASE); B(HELD_COMMAND_EXIT);
		}
		B(HELD_PIECE_BEGIN); B(HELD_PRE_ENTER);
		if (message == 1) C(HELD_MAINT_PRE);
		B(HELD_PRE_EXIT); B(HELD_MOVER_ENTER); B(HELD_MOVER_EXIT); B(HELD_POST_ENTER);
		if (message == 1) {C(HELD_MAINT_POST); C(HELD_FREEZE); C(HELD_MAINT_TAIL);}
		if (message == 2) C(HELD_THAW);
		B(HELD_POST_EXIT); B(HELD_MESSAGE_EXIT);
	}
	CHECK(SV_HeldCoverageClosed(&c));
	CHECK(c.messages == 4 && c.executions == 4 && c.commands == 2);
	CHECK(c.scene_fences == 25 && c.controls == 7);
#undef B
#undef C

	/* Missing paired callbacks must not pass as zero native work. */
	for (omit = 5; omit <= 18; ++omit)
	{
		for (operation = 0; operation != 3; ++operation)
		{
			c = Begin();
			for (i = 0; i < sizeof(positive)/sizeof(positive[0]); ++i)
			{
				if (i == omit && operation == 0)
					continue;
				if (i == omit && operation == 2)
				{
					SV_HeldCoverageBoundary(&c, positive[i + 1]);
					SV_HeldCoverageBoundary(&c, positive[i]);
					++i;
					continue;
				}
				SV_HeldCoverageBoundary(&c, positive[i]);
				if (i == omit && operation == 1)
					SV_HeldCoverageBoundary(&c, positive[i]);
			}
			/* A complete extra PostThink pair is real work, not a malformed callback. */
			if (!SV_HeldCoverageClosed(&c))
				CHECK(c.failed);
			else
				CHECK(c.post_enters != 3 || c.executions != 2);
		}
	}

	/* Final-piece absence is observable independently from the event writer. */
	c = Begin();
	for (i = 0; i < sizeof(positive)/sizeof(positive[0]); ++i)
		SV_HeldCoverageBoundary(&c, positive[i]);
	CHECK(c.executions == 2);
	CHECK(SV_HeldCoverageClosed(&c));
	--c.completions; /* Mutant serialized completion count, not a native callback. */
	CHECK(!SV_HeldCoverageClosed(&c));

	c = Begin();
	CHECK(SV_HeldCoverageBoundary(&c, HELD_MESSAGE_ENTER));
	CHECK(SV_HeldCoverageBoundary(&c, HELD_PIECE_BEGIN));
	CHECK(!SV_HeldCoverageBoundary(&c, HELD_PIECE_BEGIN));
	CHECK(c.executions == 2 && c.failed == 2);
	c = Begin();
	CHECK(!SV_HeldCoverageBoundary(&c, HELD_EFFECT));
	CHECK(c.effects == 1 && c.failed == 1);
	c = Begin();
	c.events = 16384;
	CHECK(!SV_HeldCoverageBoundary(&c, HELD_MESSAGE_ENTER));
	CHECK(c.events == 16385 && c.failed == 2);
	c = Begin();
	c.messages = 4096;
	CHECK(!SV_HeldCoverageBoundary(&c, HELD_MESSAGE_ENTER));
	CHECK(c.messages == 4097 && c.failed == 2);
	printf("held coverage: %u checks, %u failures\n", checks, failures);
	return failures ? 1 : 0;
}
