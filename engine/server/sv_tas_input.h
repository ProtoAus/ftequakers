/* P1065: compact movement inputs and a terminal, bounded replay queue.
 * No transport, cursor, weapon, impulse or hardware-evidence import. */
#ifndef SV_TAS_INPUT_H
#define SV_TAS_INPUT_H

#include <math.h>
#include <string.h>

#define SV_TAS_TAPE_MAX 128
#define SV_TAS_TAPE_LIMIT 16000000

enum sv_tas_tape_state {
	SV_TAPE_EMPTY, SV_TAPE_STAGING, SV_TAPE_ACTIVE, SV_TAPE_DONE,
	SV_TAPE_CANCELLED, SV_TAPE_FAULT
};
enum sv_tas_tape_op {
	SV_TAPE_CANCEL, SV_TAPE_VALIDATE, SV_TAPE_CHECK, SV_TAPE_BEGIN,
	SV_TAPE_PUT, SV_TAPE_ARM, SV_TAPE_STATUS
};

typedef struct {
	int move[3];
	short angle[3];
	unsigned char buttons, valid; /* compact jump1 / duck2 / walk4 */
} sv_tas_input_t;

typedef struct {
	unsigned int token, serial;
	double movebase;
	float donebase, request;
	int count, at;
	enum sv_tas_tape_state state;
	unsigned char filled[SV_TAS_TAPE_MAX];
	sv_tas_input_t row[SV_TAS_TAPE_MAX];
} sv_tas_tape_t;

static int SV_TASInputDecode(const float *move, const float *angle, float buttons, sv_tas_input_t *out)
{
	int i;
	memset(out, 0, sizeof(*out));
	if (!(buttons >= 0 && buttons <= 7 && floor(buttons) == buttons)) return 0;
	for (i = 0; i < 3; ++i)
	{
		if (!(move[i] >= -32768 && move[i] <= 32767 && floor(move[i]) == move[i]) ||
			!(angle[i] >= -32768 && angle[i] <= 32767 && floor(angle[i]) == angle[i])) return 0;
	}
	for (i = 0; i < 3; ++i)
	{
		out->move[i] = (int)move[i];
		out->angle[i] = (short)angle[i];
	}
	out->buttons = (unsigned char)buttons;
	out->valid = 1;
	return 1;
}

static int SV_TASTapeHeadroom(double moves, float done, int count)
{
	return count >= 1 && count <= SV_TAS_TAPE_MAX &&
		moves >= 0 && floor(moves) == moves && moves + count < SV_TAS_TAPE_LIMIT &&
		done >= 0 && floor(done) == done && done + count <= SV_TAS_TAPE_LIMIT;
}

static void SV_TASTapeEnd(sv_tas_tape_t *t, enum sv_tas_tape_state reason)
{
	memset(t->row, 0, sizeof(t->row));
	memset(t->filled, 0, sizeof(t->filled));
	t->state = reason; /* retain token/progress, never revive an old handle */
}

static int SV_TASTapeBegin(sv_tas_tape_t *t, unsigned int token, unsigned int serial,
	double moves, float done, float request, int count)
{
	if (t->state == SV_TAPE_ACTIVE || !token || token <= t->token || token > SV_TAS_TAPE_LIMIT ||
		!serial || serial > SV_TAS_TAPE_LIMIT ||
		!(request >= 1 && request <= SV_TAS_TAPE_LIMIT && floor(request) == request) ||
		!SV_TASTapeHeadroom(moves, done, count)) return 0;
	memset(t, 0, sizeof(*t));
	t->token = token; t->serial = serial;
	t->movebase = moves; t->donebase = done; t->request = request;
	t->count = count; t->state = SV_TAPE_STAGING;
	return 1;
}

static int SV_TASTapePut(sv_tas_tape_t *t, unsigned int token, int index, const sv_tas_input_t *in)
{
	if (t->state != SV_TAPE_STAGING || t->token != token) return 0;
	if (index < 0 || index >= t->count || !in->valid)
	{
		SV_TASTapeEnd(t, SV_TAPE_FAULT);
		return 0;
	}
	t->row[index] = *in; t->filled[index] = 1;
	return 1;
}

static int SV_TASTapeArm(sv_tas_tape_t *t, unsigned int token, unsigned int serial,
	double moves, float done, float request)
{
	int i;
	if (t->state != SV_TAPE_STAGING || t->token != token) return 0;
	if (t->serial != serial || t->movebase != moves || t->donebase != done || t->request != request)
	{
		SV_TASTapeEnd(t, SV_TAPE_FAULT);
		return 0;
	}
	for (i = 0; i < t->count; ++i)
		if (!t->filled[i])
		{
			SV_TASTapeEnd(t, SV_TAPE_FAULT);
			return 0;
		}
	t->state = SV_TAPE_ACTIVE;
	return 1;
}

static int SV_TASTapeCoherent(const sv_tas_tape_t *t, unsigned int serial,
	double moves, float done, float pending, float request)
{
	return t->state == SV_TAPE_ACTIVE && t->at >= 0 && t->at < t->count &&
		t->serial == serial && t->request == request &&
		t->movebase + t->at == moves && t->donebase + t->at == done &&
		pending == t->count - t->at;
}

static int SV_TASTapeCommit(sv_tas_tape_t *t, int success, double moves, float done)
{
	if (t->state != SV_TAPE_ACTIVE) return 0;
	if (!success || t->at >= t->count ||
		moves != t->movebase + t->at + 1 || done != t->donebase + t->at + 1)
	{
		SV_TASTapeEnd(t, SV_TAPE_FAULT);
		return 0;
	}
	++t->at;
	if (t->at == t->count) SV_TASTapeEnd(t, SV_TAPE_DONE);
	return 1;
}

#endif
