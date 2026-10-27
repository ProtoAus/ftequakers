/* P1061: VM-only TAS movement checkpoints. No input, file or human-proof import.
 * The QC ring owns selection/recording cuts; this bounded native ring preserves
 * Source carry/jump/duck state that ordinary save policy deliberately resets.
 * World, counters, command freshness and assistance are never rolled back. */
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
} tas_rewind_state_t;

typedef struct
{
	int userid, spawncount;
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
		if (!pool)
		{
			tas_rewind_pool[index] = pool = Z_Malloc(sizeof(*pool));
			memset(pool, 0, sizeof(*pool));
		}
		if (pool->userid != cl->userid || pool->spawncount != svs.spawncount)
		{
			memset(pool, 0, sizeof(*pool));
			pool->userid = cl->userid;
			pool->spawncount = svs.spawncount;
		}
		s = &pool->state[slot];
		memset(s, 0, sizeof(*s));
		s->token = ++tas_rewind_token;
		s->pmepoch = cl->pmepoch;
		s->source = cl->pmsrc;
		s->jumpheld = cl->jump_held;
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
	if (e->v->movetype != MOVETYPE_WALK) return 0;
	f = SV_TASRewindField(e, "run_tas_paused", ev_float);
	if (!f || f->_float != 1) return 0;
	cl->pmsrc = s->source; cl->jump_held = s->jumpheld;
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
	e->v->fixangle = 1;
	World_LinkEdict(&sv.world, (wedict_t*)e, false);
	return 1;
}
