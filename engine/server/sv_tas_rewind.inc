/* P1061: VM-only TAS movement checkpoints. No input, file or human-proof import.
 * The QC ring owns selection/recording cuts; this bounded native ring preserves
 * Source carry/jump/duck state that ordinary save policy deliberately resets.
 * World, counters, command freshness and assistance are never rolled back. */
#include "sv_tas_input.h"
#include "sv_tas_strafe.h"
#define TAS_REWIND_SLOTS 4000
#define TAS_REWIND_LIMIT 16000000u
#define TAS_REWIND_FLAGS (FL_ONGROUND | FL_WATERJUMP | FL_JUMPRELEASED)

typedef struct
{
	unsigned int token, pmepoch;
	pmsourcestate_t source;
	qboolean jumpheld;
	vec3_t origin, oldorigin, velocity, angles, viewangles, mins, maxs, viewofs, gravitydir, basevelocity;
	float teleporttime, gravity, entgravity, waterlevel, watertype;
	float forceduck, momlimit, momcool, momslide;
	int flags, pmflags;
	sv_tas_input_t input; // immutable availability; not a historical live-counter key
} tas_rewind_state_t;

typedef struct
{
	int userid, spawncount;
	sv_tas_input_t lastinput;
	double lastmove;
	sv_tas_tape_t tape;
	sv_tas_input_t planinput;
	unsigned int planepoch;
	unsigned short planyaw;
	int planmode, planair, planpostpending; // 0 live, 1 manual preset, 2 left, 3 right
	tas_rewind_state_t state[TAS_REWIND_SLOTS];
} tas_rewind_pool_t;

static tas_rewind_pool_t *tas_rewind_pool[MAX_CLIENTS];
static unsigned int tas_rewind_token;

void SV_TASRewindClear(void)
{
	unsigned int i;
	for (i = 0; i < countof(tas_rewind_pool); ++i)
	{
		if (tas_rewind_pool[i]) Z_Free(tas_rewind_pool[i]);
		tas_rewind_pool[i] = NULL;
	}
	tas_rewind_token = 0; // all QC handles expire at this progs/map boundary too
}

static eval_t *SV_TASRewindField(edict_t *e, const char *name, etype_t type)
{
	return svprogfuncs->GetEdictFieldValue(svprogfuncs, e, name, type, NULL);
}

static client_t *SV_TASRewindClient(edict_t *e)
{
	extern cvar_t pm_physicsmode, pm_ticrate;
	eval_t *f;
	int slot;
	if (svs.gametype != GT_PROGS || !svprogfuncs || !e || e != sv_player ||
		sv.state != ss_active || !host_client) return NULL;
	slot = e->entnum - 1;
	if (slot < 0 || slot >= sv.allocated_client_slots || slot >= MAX_CLIENTS ||
		host_client != &svs.clients[slot] || host_client->edict != e ||
		host_client->state != cs_spawned || host_client->userid <= 0 || host_client->spectator ||
		!SV_TASRewindCount(host_client->movetickcount) ||
		host_client->protocol != SCP_QUAKEWORLD ||
		(host_client->fteprotocolextensions2 & PEXT2_VRINPUTS) || sv_nqplayerphysics.ival ||
		PR_FindFunction(svprogfuncs, "SV_RunClientCommand", PR_ANY) ||
		!(e->v->health > 0) ||
		!SV_TASClockSupported(pm_physicsmode.ival == PHYSMODE_SOURCE,
			pm_ticrate.value > 0 ? pm_ticrate.value : SV_TAS_TICK,
			sv.gamespeed, host_client->pmsrc.msec_carry)) return NULL;
	f = SV_TASRewindField(e, "run_tas_mode", ev_float);
	if (!f || f->_float != 1) return NULL;
	f = SV_TASRewindField(e, "run_tas_used", ev_float);
	if (!f || f->_float != 1) return NULL;
	return host_client;
}

static void SV_TASTapeStop(edict_t *e, tas_rewind_pool_t *pool, enum sv_tas_tape_state reason);
static void SV_TASInputCancel(edict_t *e);

static tas_rewind_pool_t *SV_TASPool(client_t *cl, qboolean create)
{
	int index = cl - svs.clients;
	tas_rewind_pool_t *p;
	if (index < 0 || index >= sv.allocated_client_slots || index >= MAX_CLIENTS) return NULL;
	p = tas_rewind_pool[index];
	if (!p && create)
	{
		tas_rewind_pool[index] = p = Z_Malloc(sizeof(*p));
		memset(p, 0, sizeof(*p));
	}
	if (!p) return NULL;
	if (p->userid != cl->userid || p->spawncount != svs.spawncount)
	{
		if (!create) return NULL;
		memset(p, 0, sizeof(*p));
		p->userid = cl->userid; p->spawncount = svs.spawncount;
	}
	return p;
}

float SV_TASRewind(edict_t *e, unsigned int slot, unsigned int token, unsigned int op)
{
	client_t *cl;
	tas_rewind_pool_t *pool;
	tas_rewind_state_t *s;
	eval_t *f;
	int index;
	if (slot >= TAS_REWIND_SLOTS || op > 2) return 0;
	cl = SV_TASRewindClient(e);
	if (!cl) return 0;
	index = e->entnum - 1;
	pool = tas_rewind_pool[index];
	if (op == 0)
	{
		// Carriers are world state. Capture only a normal private walk body.
		if (e->v->movetype != MOVETYPE_WALK ||
			(((int)e->v->flags & FL_ONGROUND) && e->v->groundentity) ||
			tas_rewind_token >= TAS_REWIND_LIMIT) return 0;
		pool = SV_TASPool(cl, true);
		if (!pool) return 0;
		s = &pool->state[slot];
		memset(s, 0, sizeof(*s));
		s->token = ++tas_rewind_token;
		s->pmepoch = cl->pmepoch;
		s->source = cl->pmsrc;
		s->jumpheld = cl->jump_held;
		if (pool->lastinput.valid && pool->lastmove == cl->movetickcount)
			s->input = pool->lastinput;
		VectorCopy(e->v->origin, s->origin); VectorCopy(e->v->oldorigin, s->oldorigin);
		VectorCopy(e->v->velocity, s->velocity); VectorCopy(e->v->angles, s->angles);
		VectorCopy(e->v->v_angle, s->viewangles); VectorCopy(e->v->mins, s->mins);
		VectorCopy(e->v->maxs, s->maxs); VectorCopy(e->v->view_ofs, s->viewofs);
		VectorCopy(e->xv->gravitydir, s->gravitydir);
		s->flags = (int)e->v->flags & TAS_REWIND_FLAGS;
		s->pmflags = (int)e->xv->pmove_flags & (PMF_DUCKED | PMF_LADDER);
		s->teleporttime = max(0, e->v->teleport_time - sv.time);
		s->gravity = e->xv->gravity; s->entgravity = cl->entgravity;
		s->waterlevel = e->v->waterlevel; s->watertype = e->v->watertype;
		f = SV_TASRewindField(e, "run_basevelocity", ev_vector);
		if (f) VectorCopy(f->_vector, s->basevelocity);
#define TAS_CAPTURE_FIELD(name, member) f = SV_TASRewindField(e, name, ev_float); if (f) s->member = f->_float
		TAS_CAPTURE_FIELD("run_forceduck", forceduck);
		TAS_CAPTURE_FIELD("run_momlimit", momlimit);
		TAS_CAPTURE_FIELD("run_momcool", momcool);
		TAS_CAPTURE_FIELD("run_momslide", momslide);
#undef TAS_CAPTURE_FIELD
		return s->token;
	}
	if (!pool || !token || pool->userid != cl->userid || pool->spawncount != svs.spawncount) return 0;
	s = &pool->state[slot];
	if (s->token != token) return 0;
	// Watch holds legitimately change the live pin epoch; do not mistake that
	// for a different owner. Rules/world are current, not rolled back by a go.
	if (op == 1) return 1; // read-only preflight; never changes body or recording
	if (pool->tape.state == SV_TAPE_ACTIVE || pool->tape.state == SV_TAPE_STAGING)
	{
		SV_TASTapeStop(e, pool, SV_TAPE_CANCELLED);
		return 0; // no replay can silently attach to a different placement
	}
	if (e->v->movetype != MOVETYPE_WALK) return 0;
	f = SV_TASRewindField(e, "run_tas_paused", ev_float);
	if (!f || f->_float != 1) return 0;
	SV_TASInputCancel(e); // a placement never revives a latent future-input plan
	cl->pmsrc = s->source; cl->jump_held = s->jumpheld;
	pool->lastinput = s->input; // state-only snapshots explicitly clear availability
	pool->lastmove = cl->movetickcount; // rebind, NEVER rewind the connection counter
	// Publish restored fractional state immediately, never the cumulative tick.
	f = SV_TASRewindField(e, "run_movecarry", ev_float);
	if (f) f->_float = cl->pmsrc.msec_carry;
	VectorCopy(s->origin, e->v->origin); VectorCopy(s->oldorigin, e->v->oldorigin);
	VectorCopy(s->velocity, e->v->velocity); VectorCopy(s->angles, e->v->angles);
	VectorCopy(s->viewangles, e->v->v_angle); VectorCopy(s->mins, e->v->mins);
	VectorCopy(s->maxs, e->v->maxs); VectorCopy(s->viewofs, e->v->view_ofs);
	VectorCopy(s->gravitydir, e->xv->gravitydir);
	VectorSubtract(e->v->maxs, e->v->mins, e->v->size);
	e->v->flags = ((int)e->v->flags & ~TAS_REWIND_FLAGS) | s->flags;
	e->v->groundentity = 0;
	e->xv->pmove_flags = ((int)e->xv->pmove_flags & ~(PMF_DUCKED | PMF_LADDER)) | s->pmflags;
	e->v->teleport_time = s->teleporttime > 0 ? sv.time + s->teleporttime : 0;
	e->xv->gravity = s->gravity; cl->entgravity = s->entgravity;
	e->v->waterlevel = s->waterlevel; e->v->watertype = s->watertype;
	f = SV_TASRewindField(e, "run_basevelocity", ev_vector);
	if (f) VectorCopy(s->basevelocity, f->_vector);
#define TAS_RESTORE_FIELD(name, member) f = SV_TASRewindField(e, name, ev_float); if (f) f->_float = s->member
	TAS_RESTORE_FIELD("run_forceduck", forceduck);
	TAS_RESTORE_FIELD("run_momlimit", momlimit);
	TAS_RESTORE_FIELD("run_momcool", momcool);
	TAS_RESTORE_FIELD("run_momslide", momslide);
#undef TAS_RESTORE_FIELD
	// This restore owns its notification; keep historical body angles off the wire.
	e->v->fixangle = FIXANGLE_NO;
	SV_SendFixAngle(cl, NULL, FIXANGLE_FIXED, true);
	World_LinkEdict(&sv.world, (wedict_t*)e, false);
	return 1;
}

// P1065: movement-only inputs, not raw network/hardware evidence.
static tas_rewind_pool_t *SV_TASInputPool(client_t *cl)
{
	int index;
	tas_rewind_pool_t *p;
	if (!cl || !cl->edict) return NULL;
	index = cl->edict->entnum - 1;
	if (index < 0 || index >= sv.allocated_client_slots || index >= MAX_CLIENTS ||
		cl != &svs.clients[index]) return NULL;
	p = tas_rewind_pool[index];
	if (!p || p->userid != cl->userid || p->spawncount != svs.spawncount) return NULL;
	return p;
}

static float SV_TASInputField(edict_t *e, const char *name)
{
	eval_t *f = SV_TASRewindField(e, name, ev_float);
	return f ? f->_float : 0;
}

static void SV_TASInputSet(edict_t *e, const char *name, float value)
{
	eval_t *f = SV_TASRewindField(e, name, ev_float);
	if (f) f->_float = value;
}

static void SV_TASTapePublish(edict_t *e, const tas_rewind_pool_t *p)
{
	SV_TASInputSet(e, "run_tas_replaystate", p->tape.state);
	SV_TASInputSet(e, "run_tas_replaycount", p->tape.count);
	SV_TASInputSet(e, "run_tas_replayat", p->tape.at);
	SV_TASInputSet(e, "run_tas_planmode", p->planmode);
	SV_TASInputSet(e, "run_tas_planforward", p->planinput.move[0]);
	SV_TASInputSet(e, "run_tas_planside", p->planinput.move[1]);
	SV_TASInputSet(e, "run_tas_planpitch", SHORT2ANGLE(p->planinput.angle[0]) > 180 ? SHORT2ANGLE(p->planinput.angle[0])-360 : SHORT2ANGLE(p->planinput.angle[0]));
	SV_TASInputSet(e, "run_tas_planyaw", SHORT2ANGLE(p->planinput.angle[1]) > 180 ? SHORT2ANGLE(p->planinput.angle[1])-360 : SHORT2ANGLE(p->planinput.angle[1]));
	SV_TASInputSet(e, "run_tas_planbits", p->planinput.buttons);
	SV_TASInputSet(e, "run_tas_planair", p->planair);
}

static void SV_TASTapeStop(edict_t *e, tas_rewind_pool_t *p, enum sv_tas_tape_state reason)
{
	SV_TASTapeEnd(&p->tape, reason);
	p->planmode = p->planair = p->planpostpending = 0;
	p->lastinput.valid = 0;
	SV_TASInputSet(e, "run_tas_pending", 0);
	if (reason == SV_TAPE_FAULT)
	{
		SV_TASInputSet(e, "run_tas_paused", 1);
		SV_TASInputSet(e, "run_tas_fault", 1);
	}
	SV_TASTapePublish(e, p);
}

// Cleanup uses only trusted VM/client identity, never alive/clock capability.
static void SV_TASInputCancel(edict_t *e)
{
	int index;
	client_t *cl;
	tas_rewind_pool_t *p;
	if (!e || svs.gametype != GT_PROGS || !svprogfuncs) return;
	index = e->entnum - 1;
	if (index < 0 || index >= sv.allocated_client_slots || index >= MAX_CLIENTS) return;
	cl = &svs.clients[index];
	if (cl->edict != e) return;
	p = tas_rewind_pool[index];
	if (!p) return;
	if (p->userid != cl->userid || p->spawncount != svs.spawncount)
	{
		memset(p, 0, sizeof(*p));
		p->userid = cl->userid; p->spawncount = svs.spawncount;
	}
	p->lastinput.valid = 0;
	p->planmode = p->planair = p->planpostpending = 0;
	if (p->tape.state == SV_TAPE_ACTIVE || p->tape.state == SV_TAPE_STAGING)
		SV_TASTapeStop(e, p, SV_TAPE_CANCELLED);
	else
		SV_TASTapePublish(e, p);
}

static void SV_TASInputInvalidate(client_t *cl)
{
	tas_rewind_pool_t *p = SV_TASInputPool(cl);
	if (p)
	{
		p->lastinput.valid = 0; // includes ordinary sub-tick state changes
		if (p->planmode)
		{
			p->planmode = p->planair = p->planpostpending = 0;
			SV_TASTapePublish(cl->edict, p);
		}
	}
}

static int SV_TASInputCapture(const usercmd_t *cmd, sv_tas_input_t *out)
{
	vec3_t moves, angles;
	float buttons;
	int i;
	memset(out, 0, sizeof(*out));
	if (cmd->impulse || cmd->weapon || (cmd->buttons & ~(quint64_t)(BUTTON_JUMP | BUTTON_DUCK | BUTTON_SPEED))) return 0;
	// FTE supplies passive cursor traces during ordinary walking. They are NOT
	// movement inputs: omit them, reject action buttons, clear live extras on replay.
	moves[0] = cmd->forwardmove; moves[1] = cmd->sidemove; moves[2] = cmd->upmove;
	for (i = 0; i < 3; ++i) angles[i] = cmd->angles[i];
	buttons = ((cmd->buttons & BUTTON_JUMP) ? 1 : 0) |
		((cmd->buttons & BUTTON_DUCK) ? 2 : 0) | ((cmd->buttons & BUTTON_SPEED) ? 4 : 0);
	return SV_TASInputDecode(moves, angles, buttons, out);
}

void SV_TASInput(edict_t *e, unsigned int slot, unsigned int token, unsigned int field, vec3_t out)
{
	client_t *cl;
	tas_rewind_pool_t *p;
	const sv_tas_input_t *in;
	int i;
	VectorClear(out);
	if (slot >= TAS_REWIND_SLOTS || field > 3 || !token) return;
	cl = SV_TASRewindClient(e);
	if (!cl || !(p = SV_TASInputPool(cl)) || p->state[slot].token != token) return;
	in = &p->state[slot].input;
	if (!in->valid) return;
	if (field == 0) out[0] = 1;
	else if (field == 1) for (i = 0; i < 3; ++i) out[i] = in->move[i];
	else if (field == 2) for (i = 0; i < 3; ++i) out[i] = in->angle[i];
	else out[0] = in->buttons;
}

static int SV_TASTapeContext(edict_t *e, client_t *cl, int count)
{
	float serial = SV_TASInputField(e, "rec_rec_serial");
	return e->v->movetype == MOVETYPE_WALK && SV_TASInputField(e, "run_tas_paused") == 1 &&
		SV_TASInputField(e, "run_tas_pending") == 0 && !SV_TASInputField(e, "run_tas_fault") &&
		!SV_TASInputField(e, "rec_wt_hold") && !SV_TASInputField(e, "rec_sl_hold") &&
		!SV_TASInputField(e, "run_t_viewowner") && !SV_TASInputField(e, "rec_gh_on") &&
		!SV_TASInputField(e, "run_ms_phase") && !SV_TASInputField(e, "run_pmhold") &&
		serial > 0 && serial <= TAS_REWIND_LIMIT && floorf(serial) == serial &&
		tas_rewind_token + count + 8 < TAS_REWIND_LIMIT &&
		SV_TASTapeHeadroom(cl->movetickcount, SV_TASInputField(e, "run_tas_done"), count);
}

float SV_TASTape(edict_t *e, unsigned int op, float a, float b,
	const vec3_t moves, const vec3_t angles, float buttons)
{
	client_t *cl;
	tas_rewind_pool_t *p;
	sv_tas_input_t in;
	unsigned int serial;
	float done, request;
	int i, same;
	if (op == SV_TAPE_CANCEL) { SV_TASInputCancel(e); return 1; }
	if (op == SV_TAPE_PLANCAP) return 1071; // readonly planner capability, not an action ticket
	cl = SV_TASRewindClient(e);
	if (op == SV_TAPE_PLANSET)
	{
		if (!cl || !SV_TASStrafeFinite(a) || a < 0 || a > 3 || a != floorf(a) || b != 0 ||
			SV_TASInputField(e, "run_tas_paused") != 1 || SV_TASInputField(e, "run_tas_pending") != 0 ||
			SV_TASInputField(e, "run_tas_fault") || e->v->movetype != MOVETYPE_WALK ||
			SV_TASInputField(e, "rec_wt_hold") || SV_TASInputField(e, "rec_sl_hold") ||
			SV_TASInputField(e, "run_t_viewowner") || SV_TASInputField(e, "rec_gh_on") ||
			SV_TASInputField(e, "run_ms_phase") || SV_TASInputField(e, "run_pmhold")) return 0;
		for (i = 0; i < 3; ++i)
			if (!SV_TASStrafeFinite(moves[i]) || !SV_TASStrafeFinite(angles[i])) return 0;
		if (!SV_TASStrafeFinite(buttons) || !SV_TASInputDecode(moves, angles, buttons, &in)) return 0;
		p = SV_TASInputPool(cl);
		if (p && (p->tape.state == SV_TAPE_ACTIVE || p->tape.state == SV_TAPE_STAGING)) return 0;
		p = SV_TASPool(cl, true);
		if (!p) return 0;
		// Equality is decided here, never from a client's potentially stale stats.
		same = p->planmode == (int)a && p->planepoch == cl->pmepoch && p->planinput.valid;
		for (i = 0; i < 3; ++i)
			if (p->planinput.move[i] != in.move[i] || p->planinput.angle[i] != in.angle[i]) same = 0;
		if (p->planinput.buttons != in.buttons) same = 0;
		p->planinput = in; p->planmode = (int)a; p->planair = 0;
		p->planepoch = cl->pmepoch;
		if (!same)
		{
			p->planyaw = in.angle[YAW];
			p->planpostpending = 0;
		}
		SV_TASTapePublish(e, p);
		return 1;
	}
	if (!cl || !(p = SV_TASInputPool(cl)))
	{
		SV_TASInputCancel(e); // failed capability must not leave a stage to arm later
		return 0;
	}
	if (op == SV_TAPE_STATUS) return p->tape.state;
	if (op == SV_TAPE_VALIDATE) return SV_TASInputDecode(moves, angles, buttons, &in);
	if (op == SV_TAPE_CHECK || op == SV_TAPE_BEGIN)
	{
		if (!(a >= 1 && a <= SV_TAS_TAPE_MAX && floorf(a) == a) ||
			p->tape.state == SV_TAPE_ACTIVE || !SV_TASTapeContext(e, cl, (int)a)) return 0;
		if (op == SV_TAPE_CHECK) return 1;
		serial = (unsigned int)SV_TASInputField(e, "rec_rec_serial");
		done = SV_TASInputField(e, "run_tas_done"); request = SV_TASInputField(e, "run_tas_req");
		if (!SV_TASTapeBegin(&p->tape, ++tas_rewind_token, serial, cl->movetickcount, done, request, (int)a)) return 0;
		p->planmode = p->planair = p->planpostpending = 0;
		SV_TASTapePublish(e, p);
		return p->tape.token;
	}
	if (!(a >= 1 && a <= TAS_REWIND_LIMIT && floorf(a) == a)) return 0;
	if (op == SV_TAPE_PUT)
	{
		if (!(b >= 0 && b < SV_TAS_TAPE_MAX && floorf(b) == b))
		{
			if (p->tape.state == SV_TAPE_STAGING && p->tape.token == (unsigned int)a)
				SV_TASTapeStop(e, p, SV_TAPE_FAULT);
			return 0;
		}
		SV_TASInputDecode(moves, angles, buttons, &in);
		if (!SV_TASTapePut(&p->tape, (unsigned int)a, (int)b, &in))
		{
			SV_TASTapePublish(e, p);
			return 0;
		}
		return 1;
	}
	if (op == SV_TAPE_ARM)
	{
		if (!SV_TASTapeContext(e, cl, p->tape.count))
		{
			if (p->tape.state == SV_TAPE_STAGING && p->tape.token == (unsigned int)a)
				SV_TASTapeStop(e, p, SV_TAPE_FAULT);
			return 0;
		}
		serial = (unsigned int)SV_TASInputField(e, "rec_rec_serial");
		done = SV_TASInputField(e, "run_tas_done"); request = SV_TASInputField(e, "run_tas_req");
		if (!SV_TASTapeArm(&p->tape, (unsigned int)a, serial, cl->movetickcount, done, request))
		{
			SV_TASTapePublish(e, p);
			return 0;
		}
		SV_TASInputSet(e, "run_tas_pending", p->tape.count);
		SV_TASTapePublish(e, p);
		return 1;
	}
	return 0;
}

// Return true only for an active coherent tape; stale decoded packets simply wait.
static int SV_TASTapeGuard(client_t *cl, edict_t *e, int supported, int bodyfree, int impulse)
{
	tas_rewind_pool_t *p = SV_TASInputPool(cl);
	float serial;
	if (!p) return 0;
	if (p->planmode)
	{
		if (impulse || SV_TASInputField(e, "run_tas_paused") != 1)
		{
			SV_TASInputCancel(e);
			return 0;
		}
		if (!supported || !bodyfree || !SV_TASRewindClient(e) || p->planepoch != cl->pmepoch ||
			e->v->movetype != MOVETYPE_WALK)
		{
			SV_TASTapeStop(e, p, SV_TAPE_FAULT);
			return -1;
		}
	}
	if (p->tape.state != SV_TAPE_ACTIVE) return 0;
	serial = SV_TASInputField(e, "rec_rec_serial");
	if (impulse || SV_TASInputField(e, "run_tas_paused") != 1)
	{
		SV_TASTapeStop(e, p, SV_TAPE_CANCELLED);
		return 0;
	}
	if (!supported || !bodyfree || !(SV_TASInputField(e, "run_tas_used") > 0) ||
		!(serial >= 1 && serial <= TAS_REWIND_LIMIT && floorf(serial) == serial) ||
		!SV_TASTapeCoherent(&p->tape, (unsigned int)serial,
			cl->movetickcount, SV_TASInputField(e, "run_tas_done"),
			SV_TASInputField(e, "run_tas_pending"), SV_TASInputField(e, "run_tas_req")))
	{
		SV_TASTapeStop(e, p, SV_TAPE_FAULT);
		return -1; // block this execution, never fall into ordinary movement
	}
	return 1;
}

static void SV_TASApplyInput(edict_t *e, usercmd_t *cmd, const sv_tas_input_t *in)
{
	int i;
	cmd->forwardmove = in->move[0]; cmd->sidemove = in->move[1]; cmd->upmove = in->move[2];
	cmd->buttons = ((in->buttons & 1) ? BUTTON_JUMP : 0) |
		((in->buttons & 2) ? BUTTON_DUCK : 0) | ((in->buttons & 4) ? BUTTON_SPEED : 0);
	cmd->impulse = cmd->weapon = 0;
	cmd->lightlevel = 0;
	memset(cmd->cursor_screen, 0, sizeof(cmd->cursor_screen));
	VectorClear(cmd->cursor_start); VectorClear(cmd->cursor_impact);
	cmd->cursor_entitynumber = 0;
	memset(cmd->vr, 0, sizeof(cmd->vr));
	for (i = 0; i < 3; ++i)
	{
		cmd->angles[i] = in->angle[i];
		if (!e->v->fixangle) // genuine QC corrections retain ordinary angle ownership
			e->v->v_angle[i] = SHORT2ANGLE(cmd->angles[i]);
	}
}

static int SV_TASTapeOverlay(client_t *cl, edict_t *e, usercmd_t *cmd)
{
	tas_rewind_pool_t *p = SV_TASInputPool(cl);
	if (!p || p->tape.state != SV_TAPE_ACTIVE) return 0;
	SV_TASApplyInput(e, cmd, &p->tape.row[p->tape.at]);
	return 1;
}

static void SV_TASInputComplete(client_t *cl, edict_t *e, const sv_tas_input_t *in, int success)
{
	tas_rewind_pool_t *p = SV_TASInputPool(cl);
	if (!p) return;
	p->lastinput = *in;
	p->lastinput.valid = success && in->valid;
	p->lastmove = cl->movetickcount; // called AFTER the resulting counter is published
	if (p->tape.state == SV_TAPE_ACTIVE)
	{
		if (!SV_TASTapeCommit(&p->tape, success && in->valid, cl->movetickcount,
			SV_TASInputField(e, "run_tas_done")))
			SV_TASTapeStop(e, p, SV_TAPE_FAULT);
		else SV_TASTapePublish(e, p);
	}
}
