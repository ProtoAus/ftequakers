#ifndef SV_HELD_COVERAGE_H
#define SV_HELD_COVERAGE_H
#include <stdint.h>

typedef enum
{
	HELD_MESSAGE_ENTER, HELD_MESSAGE_EXIT,
	HELD_PIECE_BEGIN, HELD_PRE_ENTER, HELD_PRE_EXIT,
	HELD_MOVER_ENTER, HELD_MOVER_EXIT,
	HELD_POST_ENTER, HELD_POST_EXIT,
	HELD_COMMAND_ENTER, HELD_COMMAND_EXIT,
	HELD_FRAME_ENTER, HELD_FRAME_EXIT,
	HELD_EFFECT
} held_boundary_t;

typedef enum
{
	HELD_PIN_ENTRY, HELD_PIN_RELEASE, HELD_MAINT_PRE, HELD_MAINT_POST,
	HELD_MAINT_TAIL, HELD_FREEZE, HELD_THAW
} held_control_t;

typedef struct
{
	uint64_t executions, pre_enters, pre_exits, movers, completions;
	uint64_t post_enters, post_exits, messages, commands, frames, scene_fences;
	uint64_t controls, effects, events;
	unsigned int active, failed, message_open, command_open, frame_open, stage, post_open;
} held_coverage_t;

int SV_HeldCoverageBegin(held_coverage_t *coverage);
int SV_HeldCoverageBoundary(held_coverage_t *coverage, held_boundary_t boundary);
int SV_HeldCoverageControl(held_coverage_t *coverage, held_control_t control);
int SV_HeldCoverageClosed(const held_coverage_t *coverage);
unsigned int SV_HeldCoverageTest(unsigned int *checks);
#endif
