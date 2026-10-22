#include <string.h>
#include "sv_held_coverage.h"

static int Held_CoverageFail(held_coverage_t *c, unsigned int failure)
{
	c->failed = failure;
	return 0;
}

int SV_HeldCoverageBegin(held_coverage_t *c)
{
	if (c->active || c->failed)
		return Held_CoverageFail(c, 2);
	memset(c, 0, sizeof(*c));
	c->active = 1;
	return 1;
}

int SV_HeldCoverageBoundary(held_coverage_t *c, held_boundary_t boundary)
{
	if (!c->active)
		return 1; /* No collection before an admitted bind. */
	if (c->failed)
		return 0;
	if (++c->events > 16384)
		return Held_CoverageFail(c, 2);
	switch (boundary)
	{
	case HELD_MESSAGE_ENTER:
		++c->messages;
		if (c->messages > 4096 || c->message_open || c->command_open || c->frame_open || c->stage || c->post_open)
			return Held_CoverageFail(c, 2);
		c->message_open = 1;
		return 1;
	case HELD_MESSAGE_EXIT:
		if (!c->message_open || c->command_open || c->frame_open || c->stage || c->post_open)
			return Held_CoverageFail(c, 2);
		c->message_open = 0;
		return 1;
	case HELD_PIECE_BEGIN:
		++c->executions;
		if (c->executions > 4096 || !c->message_open || c->command_open || c->frame_open || c->stage || c->post_open)
			return Held_CoverageFail(c, 2);
		c->stage = 1;
		return 1;
	case HELD_PRE_ENTER:
		++c->pre_enters;
		++c->scene_fences;
		if (c->stage != 1)
			return Held_CoverageFail(c, 2);
		c->stage = 2;
		return 1;
	case HELD_PRE_EXIT:
		++c->pre_exits;
		++c->scene_fences;
		if (c->stage != 2)
			return Held_CoverageFail(c, 2);
		c->stage = 3;
		return 1;
	case HELD_MOVER_ENTER:
		++c->movers;
		if (c->stage != 3)
			return Held_CoverageFail(c, 2);
		c->stage = 4;
		return 1;
	case HELD_MOVER_EXIT:
		++c->completions;
		if (c->stage != 4)
			return Held_CoverageFail(c, 2);
		c->stage = 0;
		return 1;
	case HELD_POST_ENTER:
		++c->post_enters;
		++c->scene_fences;
		if (!c->message_open || c->stage || c->post_open || c->command_open || c->frame_open)
			return Held_CoverageFail(c, 2);
		c->post_open = 1;
		return 1;
	case HELD_POST_EXIT:
		++c->post_exits;
		++c->scene_fences;
		if (!c->post_open || c->stage)
			return Held_CoverageFail(c, 2);
		c->post_open = 0;
		return 1;
	case HELD_COMMAND_ENTER:
		++c->commands;
		++c->scene_fences;
		if (!c->message_open || c->stage || c->post_open || c->command_open || c->frame_open)
			return Held_CoverageFail(c, 1);
		c->command_open = 1;
		return 1;
	case HELD_COMMAND_EXIT:
		++c->scene_fences;
		if (!c->command_open)
			return Held_CoverageFail(c, 2);
		c->command_open = 0;
		return 1;
	case HELD_FRAME_ENTER:
		++c->frames;
		++c->scene_fences;
		if (c->message_open || c->stage || c->post_open || c->command_open || c->frame_open)
			return Held_CoverageFail(c, 2);
		c->frame_open = 1;
		return 1;
	case HELD_FRAME_EXIT:
		++c->scene_fences;
		if (!c->frame_open)
			return Held_CoverageFail(c, 2);
		c->frame_open = 0;
		return 1;
	case HELD_EFFECT:
		++c->effects;
		return Held_CoverageFail(c, 1);
	}
	return Held_CoverageFail(c, 1);
}

int SV_HeldCoverageControl(held_coverage_t *c, held_control_t control)
{
	if (!c->active) return 1;
	if (c->failed) return 0;
	if (++c->events > 16384) return Held_CoverageFail(c, 2);
	++c->controls;
	switch (control)
	{
	case HELD_PIN_ENTRY:
	case HELD_PIN_RELEASE:
		++c->scene_fences;
		if (!c->command_open || c->stage || c->post_open || c->frame_open)
			return Held_CoverageFail(c, 1);
		return 1;
	case HELD_MAINT_PRE:
		++c->scene_fences;
		if (c->stage != 2 || c->post_open || c->command_open || c->frame_open)
			return Held_CoverageFail(c, 1);
		return 1;
	case HELD_MAINT_POST:
	case HELD_MAINT_TAIL:
		++c->scene_fences;
		if (!c->post_open || c->stage || c->command_open || c->frame_open)
			return Held_CoverageFail(c, 1);
		return 1;
	case HELD_FREEZE:
	case HELD_THAW:
		if (!c->post_open || c->stage || c->command_open || c->frame_open)
			return Held_CoverageFail(c, 1);
		return 1;
	}
	return Held_CoverageFail(c, 1);
}

int SV_HeldCoverageClosed(const held_coverage_t *c)
{
	return c->active && !c->failed && !c->message_open && !c->command_open &&
		!c->frame_open && !c->stage && !c->post_open && !c->effects &&
		c->executions && c->messages && c->executions == c->pre_enters &&
		c->executions == c->pre_exits && c->executions == c->movers &&
		c->executions == c->completions && c->post_enters == c->post_exits;
}


unsigned int SV_HeldCoverageTest(unsigned int *checks)
{
	static const held_boundary_t path[] = {
		HELD_FRAME_ENTER, HELD_FRAME_EXIT, HELD_MESSAGE_ENTER,
		HELD_COMMAND_ENTER, HELD_COMMAND_EXIT, HELD_PIECE_BEGIN,
		HELD_PRE_ENTER, HELD_PRE_EXIT, HELD_MOVER_ENTER, HELD_MOVER_EXIT,
		HELD_POST_ENTER, HELD_POST_EXIT, HELD_MESSAGE_EXIT
	};
	held_coverage_t c, zero;
	unsigned int count = 0, failures = 0, i, changed;
	int valid;
#define HC_TEST(x) do { ++count; if (!(x)) ++failures; } while (0)
	memset(&zero, 0, sizeof(zero));
	c = zero;
	for (i = 0; i < sizeof(path)/sizeof(path[0]); ++i)
		HC_TEST(SV_HeldCoverageBoundary(&c, path[i]));
	for (i = HELD_PIN_ENTRY; i <= HELD_THAW; ++i)
		HC_TEST(SV_HeldCoverageControl(&c, (held_control_t)i));
	HC_TEST(!memcmp(&c, &zero, sizeof(c))); /* Unbound remains entirely inert. */
	HC_TEST(SV_HeldCoverageBegin(&c));
	for (i = 0; i < sizeof(path)/sizeof(path[0]); ++i)
		HC_TEST(SV_HeldCoverageBoundary(&c, path[i]));
	HC_TEST(SV_HeldCoverageClosed(&c));
	HC_TEST(c.frames == 1 && c.commands == 1 && c.messages == 1 && c.executions == 1);
	HC_TEST(c.scene_fences == 8 && !c.controls && !c.effects);
	/* Every omitted or duplicated actual dispatch must prevent normal closure. */
	for (changed = 0; changed < sizeof(path)/sizeof(path[0]); ++changed)
	{
		c = zero; SV_HeldCoverageBegin(&c); valid = 1;
		for (i = 0; i < sizeof(path)/sizeof(path[0]); ++i)
			if (i != changed) valid &= SV_HeldCoverageBoundary(&c, path[i]);
		HC_TEST(!valid || !SV_HeldCoverageClosed(&c));
		c = zero; SV_HeldCoverageBegin(&c); valid = 1;
		for (i = 0; i < sizeof(path)/sizeof(path[0]); ++i)
		{
			valid &= SV_HeldCoverageBoundary(&c, path[i]);
			if (i == changed) valid &= SV_HeldCoverageBoundary(&c, path[i]);
		}
		HC_TEST(!valid || !SV_HeldCoverageClosed(&c));
	}
	for (i = HELD_PIN_ENTRY; i <= HELD_THAW; ++i)
	{
		c = zero; SV_HeldCoverageBegin(&c);
		HC_TEST(!SV_HeldCoverageControl(&c, (held_control_t)i));
		c = zero; SV_HeldCoverageBegin(&c);
		SV_HeldCoverageBoundary(&c, HELD_MESSAGE_ENTER);
		if (i <= HELD_PIN_RELEASE)
			SV_HeldCoverageBoundary(&c, HELD_COMMAND_ENTER);
		else if (i == HELD_MAINT_PRE)
		{
			SV_HeldCoverageBoundary(&c, HELD_PIECE_BEGIN);
			SV_HeldCoverageBoundary(&c, HELD_PRE_ENTER);
		}
		else SV_HeldCoverageBoundary(&c, HELD_POST_ENTER);
		changed = (unsigned int)c.scene_fences;
		HC_TEST(SV_HeldCoverageControl(&c, (held_control_t)i));
		HC_TEST(c.controls == 1 && c.scene_fences == changed + (i <= HELD_MAINT_TAIL));
	}
	c = zero; SV_HeldCoverageBegin(&c);
	HC_TEST(!SV_HeldCoverageBoundary(&c, HELD_EFFECT) && c.effects == 1 && !SV_HeldCoverageClosed(&c));
	c = zero; SV_HeldCoverageBegin(&c); c.events = 16384;
	HC_TEST(!SV_HeldCoverageControl(&c, HELD_FREEZE) && c.failed == 2);
	c = zero; SV_HeldCoverageBegin(&c); c.executions = 4096;
	SV_HeldCoverageBoundary(&c, HELD_MESSAGE_ENTER);
	HC_TEST(!SV_HeldCoverageBoundary(&c, HELD_PIECE_BEGIN) && c.failed == 2);
#undef HC_TEST
	*checks = count;
	return failures;
}
