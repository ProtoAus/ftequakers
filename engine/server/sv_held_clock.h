/* Initial-only held replay clock. All values are native float32 state. */
#ifndef SV_HELD_CLOCK_H
#define SV_HELD_CLOCK_H

typedef struct
{
	float starttick, startsec, startwall, tickcarry;
	float lastmt, lastsec;
	float freeze, frzon, frzat, frzmt, frzcarry, frzticks, frzsec;
	float state, ticks, elapsed;
	float ckwall, ckwt, ckgaps, ckncr, ckcb, ckcr, ckd, cktr, ckpk, ckup, cku, ckskip;
	float resumesec, lastorg[3], packet, counted, flags, frame;
} held_clock_t;

typedef struct
{
	float excuse, slow, hyst, allow, freecredits, gap;
	unsigned int frozenflag, slowflag;
	float runningstate;
} held_clock_context_t;

typedef struct
{
	held_clock_t clock;
	held_clock_context_t context;
	int initialized;
	int released;
	int failed;
	unsigned int post_phase;
	/* Derived viewing lane; NOT serialized or imported from clock observations. */
	int view_policy, view_owner;
	float view_seconds, view_wall, view_rate;
} held_clock_replay_t;

int SV_HeldClockBegin(held_clock_replay_t *replay, const held_clock_t *initial,
	const held_clock_context_t *context);
int SV_HeldClockFrame(held_clock_replay_t *replay, float time, float frametime);
int SV_HeldClockRequest(held_clock_replay_t *replay, int freeze);
float SV_HeldClockViewSeconds(const held_clock_replay_t *replay);
float SV_HeldClockSeconds(const held_clock_replay_t *replay);
int SV_HeldClockViewTicks(const held_clock_replay_t *replay, float *ticks);
/* Caller must have validated accepted-message kind4 EXIT; no observed flag setter. */
int SV_HeldClockNonce(held_clock_replay_t *replay);
int SV_HeldClockAccounting(held_clock_replay_t *replay, float time, float movetick,
	float movetickrate, float movecarry);
int SV_HeldClockPacket(held_clock_replay_t *replay);
int SV_HeldClockRecordFrame(held_clock_replay_t *replay);
int SV_HeldClockLastOrigin(held_clock_replay_t *replay, const float origin[3]);
int SV_HeldClockFinish(held_clock_replay_t *replay);
int SV_HeldClockCompare(const held_clock_replay_t *replay, const held_clock_t *observation);
#endif
