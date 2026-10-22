#ifndef SV_HELD_REPLAY_H
#define SV_HELD_REPLAY_H
#include "quakedef.h"
#include "sv_held_ledger.h"
#include "sv_held_clock.h"

typedef struct
{
	usercmd_t command;
	uint32_t packet, movetick;
	float carry;
	unsigned int kind, flags;
} held_input_t;
typedef struct
{
	uint32_t packet, row, rejected;
	float ring[2], read[2];
} held_counts_t;
int SV_HeldRelativeCountsParse(const char *line, size_t size, int first, held_counts_t *counts);
int SV_HeldInputParse(const char *line, size_t size, held_input_t *input);
int SV_HeldInputEnd(const char *line, size_t size, held_input_t *endpoint);
/* Initial recorder observation only; caller binds location/alias, never applies it. */
int SV_HeldNeutralRideParse(const char *line, size_t size, uint32_t *packet, uint32_t *movetick);
/* Structural first ordinary key only; full input validation remains mandatory. */
int SV_HeldInitialInputKeyParse(const char *line, size_t size, uint32_t *packet, uint32_t *movetick);
/* HORDINARY only, successor/inend endpoint, never HDONE/HMOVE or quantization. */
int SV_HeldInputMsec(const held_input_t *input, const held_input_t *successor, float tickrate, float *msec);
unsigned int SV_HeldInputTest(unsigned int *checks);

/* One synchronous initial-only physical kernel. Not full profile admission.
 * Callers must separately validate source/artifacts, scene/callbacks, clock/zones.
 * failure1=unsupported context/path,2=derived comparison disagreement. */
typedef struct
{
	playermove_t *saved;
	movevars_t saved_movevars;
	int saved_trace[3]; /* sourceclip, phybrushes, phyfix; never serialized */
	model_t *world;
	uint64_t raw_moveticks;
	unsigned int calls, pending, entered, pinned, released, thawed, guard_done, failure;
	float svmaxvel;
} held_physical_t;
int SV_HeldPhysicalBegin(held_physical_t *replay, const held_record_t *context,
	const held_record_t *initial, model_t *world);
int SV_HeldPhysicalStep(held_physical_t *replay, const held_record_t *move,
	const usercmd_t *ordinary, float ordinary_msec);
int SV_HeldPhysicalDone(held_physical_t *replay, const held_record_t *completion);
int SV_HeldPhysicalControl(held_physical_t *replay, const held_record_t *control);
int SV_HeldPhysicalObserve(held_physical_t *replay, const held_record_t *observation);
void SV_HeldPhysicalEnd(held_physical_t *replay);
unsigned int SV_HeldPhysicalTest(unsigned int *checks, unsigned int *acted);
int SV_HeldReplayClockBegin(held_clock_replay_t *replay, const held_record_t *context, const held_record_t *initial);
int SV_HeldReplayClockCompare(const held_clock_replay_t *replay, const held_record_t *observation);
unsigned int SV_HeldReplayClockTest(unsigned int *checks);

/* Continuous component kernel, NOT full source/scene/profile admission.
 * Caller validates complete witness, original source/artifacts/scene and ordinary
 * payload/observations. Zone callback is the trusted shared pure verifier only:
 * 0=no boundary,1=finish; other actions unsupported by this first profile. */
typedef int (*held_zone_step_t)(void *context, const float lastorg[3], const float origin[3], const float mins[3], const float maxs[3]);
typedef struct
{
	held_physical_t physical;
	held_clock_replay_t clock;
	held_zone_step_t zone_step;
	void *zone_context;
	unsigned int post, advanced, transition, finished, terminal, failure;
} held_timeline_t;
int SV_HeldTimelineBegin(held_timeline_t *timeline, const held_record_t *context,
	const held_record_t *initial, model_t *world, held_zone_step_t zone_step, void *zone_context);
int SV_HeldTimelineEvent(held_timeline_t *timeline, const held_record_t *record,
	const usercmd_t *ordinary, float ordinary_msec);
int SV_HeldTimelineFinish(held_timeline_t *timeline);
void SV_HeldTimelineEnd(held_timeline_t *timeline);

/* Bound shared-QC zone evaluator; synchronous only, never an admission flag. */
typedef struct
{
	void *vm;
	model_t *world;
	unsigned int epoch, active, failed;
	uint64_t progs_epoch;
	func_t step;
	float *globals[9], saved[9];
	unsigned char digest[32];
} held_zone_t;
int SV_HeldZoneFingerprint(unsigned char digest[32]);
int SV_HeldZoneBegin(held_zone_t *zone, const unsigned char digest[32], const float seed[5]);
int SV_HeldZoneStep(void *zone, const float lastorg[3], const float origin[3], const float mins[3], const float maxs[3]);
void SV_HeldZoneEnd(held_zone_t *zone);
unsigned int SV_HeldZoneTest(unsigned int *checks);

/* Closed-byte trajectory component only. Caller-supplied context is independently
 * native-derived, never the file's own HCTX. A match is NOT full profile admission:
 * original source/scene ownership and ALL ordinary observations remain separate.
 * No publisher/readiness caller may treat this result as a complete verdict. */
typedef enum
{
	HELD_VERIFY_UNSUPPORTED = 1,
	HELD_VERIFY_INCOMPLETE = 2,
	HELD_VERIFY_MALFORMED = 3,
	HELD_VERIFY_MISMATCH = 4,
	HELD_VERIFY_MATCH = 5
} held_verify_result_t;
typedef struct
{
	float ticks;
	unsigned int native_moves, failure;
} held_trajectory_t;
int SV_HeldTrajectoryRecording(const char *data, size_t size, const held_record_t *native_context,
	model_t *world, held_trajectory_t *trajectory);
/* Closed original conformance only: authenticated filing/source-hash fences remain
 * the caller's existing responsibility. Never creates a live source/owner grant. */
held_verify_result_t SV_HeldVerifyRecording(const char *data, size_t size,
	model_t *world, held_trajectory_t *trajectory);
#endif
