#if defined(__GNUC__)
#pragma GCC optimize ("no-fast-math", "fp-contract=off")
#endif
#include "quakedef.h"
#include "com_bih.h"
#include "pr_common.h"
#include "sv_held_initial.h"
#include "sv_held_replay.h"

extern cvar_t sv_gravity;

static int Held_Finite(float value)
{
	uint32_t bits;
	memcpy(&bits, &value, sizeof(bits));
	return (bits & 0x7f800000u) != 0x7f800000u;
}

static int Held_FieldFloat(edict_t *ent, const char *name, float *out)
{
	eval_t *value;
	if (!svprogfuncs) return 0;
	value = svprogfuncs->GetEdictFieldValue(svprogfuncs, ent, name, ev_float, NULL);
	if (!value || !Held_Finite(value->_float)) return 0;
	*out = value->_float;
	return 1;
}

static int Held_FieldZero(edict_t *ent, const char *name, etype_t type)
{
	eval_t *value = svprogfuncs->GetEdictFieldValue(svprogfuncs, ent, name, type, NULL);
	if (!value) return 0;
	if (type == ev_entity) return value->edict == 0;
	if (type == ev_vector)
		return Held_Finite(value->_vector[0]) && Held_Finite(value->_vector[1]) &&
			Held_Finite(value->_vector[2]) && !value->_vector[0] && !value->_vector[1] && !value->_vector[2];
	return Held_Finite(value->_float) && value->_float == 0;
}

static int Held_EntityCensus(client_t *client)
{
	int i, axis;
	vec3_t mins, maxs;
	if (sv.world.num_edicts > 65536 || sv.world.num_edicts < 1) return 0;
	for (axis = 0; axis != 3; ++axis)
	{
		mins[axis] = client->edict->v->origin[axis] - 272;
		maxs[axis] = client->edict->v->origin[axis] + 272;
	}
	for (i = 1; i < sv.world.num_edicts; ++i)
	{
		edict_t *e = EDICT_NUM_PB(svprogfuncs, i);
		int overlap = 1;
		if (e == client->edict || e->ereftype != ER_ENTITY) continue;
		/* Remote static brush triggers are FTE solidity22, not Quake1. */
		if (!Held_Finite(e->v->nextthink) || e->v->nextthink > 0 ||
			!Held_Finite(e->v->solid) || e->v->solid < 0 ||
			(e->v->solid > SOLID_BSP && e->v->solid != SOLID_BSPTRIGGER) ||
			e->v->solid != (int)e->v->solid || e->v->movetype != MOVETYPE_NONE)
			return 0;
		/* Inert render/metadata helpers are not physical colliders. */
		if (e->v->solid == SOLID_NOT && !e->v->touch && !e->v->think && !e->xv->contentstransition)
			continue;
		for (axis = 0; axis != 3; ++axis)
		{
			if (!Held_Finite(e->v->absmin[axis]) || !Held_Finite(e->v->absmax[axis]) ||
				e->v->absmin[axis] > e->v->absmax[axis]) return 0;
			if (e->v->absmin[axis] > maxs[axis] || e->v->absmax[axis] < mins[axis]) overlap = 0;
		}
		if (overlap) return 0;
	}
	return 1;
}

int SV_HeldAirCell(client_t *client)
{
	vec3_t mins, maxs;
	int i;
	if (!client || !client->edict || !sv.world.worldmodel) return 0;
	for (i = 0; i != 3; ++i)
	{
		if (!Held_Finite(client->edict->v->origin[i])) return 0;
		mins[i] = client->edict->v->origin[i] - (i == 2 ? 3 : 273);
		maxs[i] = client->edict->v->origin[i] + (i == 2 ? 129 : 273);
	}
#if defined(Q2BSPS) || defined(Q3BSPS)
	return BIH_HeldAirBoxEmpty(sv.world.worldmodel, mins, maxs) && Held_EntityCensus(client);
#else
	return 0;
#endif
}

static int Held_PhysicalFenceState(client_t *client, int watch_owned, float state, float mode)
{
	static const char *zero_float[] = {
		"run_pmhold", "run_forceduck", "run_basevel_armed", "run_pushed", "run_pushlift",
		"run_jumpcmd", "button2", "button5", "vbsp_lastuse",
		"run_bhop_launch_zone", "run_t_stopat",
		"rw_pin", "rec_sl_hold", "rec_gh_on", "run_ms_phase", "run_ms_sessions",
		"run_sp_phase", "run_sp_trig", "run_mono_phase", "run_mono_deadline",
		"vbsp_bhop_cnt", "vbsp_bhop_since", "run_t_groundsec", "run_rc_ticks",
		"rec_mc_sup", "run_boardcount", "run_rampcontact", "run_t_pwhy",
		"run_momlimit", "run_momcool", "run_momslide"
	};
	static const char *zero_vector[] = {"run_basevel", "run_basevelocity"};
	static const char *zero_entity[] = {"run_mono_pad", "vbsp_bhop_on", "vbsp_bhop_cntblk"};
	edict_t *e;
	float value;
	unsigned int i;
	if (!client || !svprogfuncs || svs.gametype != GT_PROGS || progstype != PROG_QW ||
		Cvar_VariableValue("rec_held_metadata") != 1 ||
		client->state != cs_spawned || client->protocol != SCP_QUAKEWORLD || client->spectator ||
		client->controller || client->controlled ||
		sv.spawned_client_slots != 1 || sv.gamespeed != 1 || !sv.world.g.defaultgravitydir || !sv.world.worldmodel ||
		sv.world.worldmodel->loadstate != MLS_LOADED ||
		!(sv.world.worldmodel->engineflags & MDLF_SOURCEBSP))
		return 0;
#ifdef SERVER_DEMO_PLAYBACK
	if (sv.demostatevalid) return 0;
#endif
	e = client->edict;
	if (!e || e->ereftype != ER_ENTITY || !Held_Finite(e->v->health) || e->v->health <= 0 ||
		e->v->deadflag || e->v->fixangle || e->v->solid != SOLID_SLIDEBOX ||
		e->v->movetype != (watch_owned ? MOVETYPE_NONE : MOVETYPE_WALK) ||
		!Held_Finite(e->v->flags) || e->v->flags < 0 || e->v->flags > 16777215 ||
		((int)e->v->flags & FL_ONGROUND) || e->v->groundentity || e->v->think ||
		e->v->nextthink || e->v->teleport_time || e->v->waterlevel || e->v->watertype != Q1CONTENTS_EMPTY ||
		(e->xv->geomtype != 0 && e->xv->geomtype != GEOMTYPE_BOX) ||
		!Held_Finite(e->xv->pmove_flags) || e->xv->pmove_flags < 0 || e->xv->pmove_flags > 16777215 ||
		((int)e->xv->pmove_flags & (PMF_LADDER|PMF_DUCKED)))
		return 0;
	for (i = 0; i != 3; ++i)
	{
		if (!Held_Finite(e->v->origin[i]) || !Held_Finite(e->v->oldorigin[i]) ||
			!Held_Finite(e->v->velocity[i]) || !Held_Finite(e->xv->gravitydir[i]) ||
			!Held_Finite(client->pmsrc.srcladdernormal[i]) || !Held_Finite(client->pmsrc.portaloffset[i]) ||
			client->pmsrc.srcladdernormal[i] || client->pmsrc.portaloffset[i])
			return 0;
		if (e->v->mins[i] != (i == 2 ? 0 : -16) || e->v->maxs[i] != (i == 2 ? 62 : 16) ||
			sv.world.g.defaultgravitydir[i] != (i == 2 ? -1 : 0)) return 0;
		if (e->xv->gravitydir[i] != 0 && !(i == 2 && e->xv->gravitydir[i] == -1)) return 0;
		if (watch_owned && e->v->velocity[i]) return 0;
	}
	if (e->v->velocity[2] > 0 || client->jump_held || client->pmsrc.surfacefriction != 1 ||
		client->pmsrc.ducking || client->pmsrc.ducked || client->pmsrc.ducktime ||
		client->pmsrc.srcladder || client->pmsrc.boardcount || client->pmsrc.rampcontact ||
		(client->pmsrc.oldbuttons & (BUTTON_JUMP|BUTTON_DUCK)) ||
		client->momlimit || client->momcool || client->momslide ||
		client->pmsrc.landticks < 0 || client->pmsrc.landticks > PMLIM_LANDMAX ||
		client->pmsrc.waterjumpremaining != 0 || client->pmsrc.waterjumpdir[0] != 0 ||
		client->pmsrc.waterjumpdir[1] != 0)
		return 0;
	for (i = 0; i < countof(zero_float); ++i)
		if (!Held_FieldZero(e, zero_float[i], ev_float)) return 0;
	for (i = 0; i < countof(zero_vector); ++i)
		if (!Held_FieldZero(e, zero_vector[i], ev_vector)) return 0;
	for (i = 0; i < countof(zero_entity); ++i)
		if (!Held_FieldZero(e, zero_entity[i], ev_entity)) return 0;
	if (!Held_FieldFloat(e,"rec_hm_cap",&value) || value != 1 ||
		!Held_FieldFloat(e,"rec_hm_allow",&value) || value != 1 ||
		!Held_FieldFloat(e,"run_t_terms",&value) || value != 8 ||
		!Held_FieldFloat(e,"rec_hm_mode",&value) || value != mode ||
		!Held_FieldFloat(e,"rec_wt_hold",&value) || value != watch_owned ||
		!Held_FieldFloat(e,"run_inserver",&value) || value != 1 ||
		!Held_FieldFloat(e,"run_t_state",&value) || value != state ||
		!Held_FieldFloat(e,"run_speedmod",&value) || (value != 0 && value != 1) ||
		!Held_FieldFloat(e,"run_t_lastvz",&value) || value > 0) return 0;
	return Held_EntityCensus(client) && (!watch_owned || SV_HeldAirCell(client));
}

int SV_HeldPhysicalFence(client_t *client, int watch_owned)
{
	return Held_PhysicalFenceState(client,watch_owned,2,12);
}

static int Held_ClockContextCapture(held_clock_context_t *context)
{
	static const char *view_fields[] = {"run_t_viewsec","run_t_viewwall","run_t_viewowner"};
	eval_t *value;
	etype_t type;
	fdef_t *fields;
	unsigned int count, i, j, found = 0;
	/* Actorless interface check only: never use a world/default edict as clock state. */
	if (!svprogfuncs || !svprogfuncs->FieldInfo) return 0;
	fields = svprogfuncs->FieldInfo(svprogfuncs,&count);
	if (!fields || !count || count > 65536) return 0;
	for (i = 0; i < count; ++i)
		if (fields[i].name)
			for (j = 0; j < countof(view_fields); ++j)
				if (!strcmp(fields[i].name,view_fields[j]))
				{
					if ((fields[i].type&0xfff) != ev_float) return 0;
					found |= 1u << j;
				}
	if (found != 7) return 0;
	memset(context,0,sizeof(*context));
	value = PR_FindGlobal(svprogfuncs,"sv_ck_excuse",PR_ANY,&type);
	if (!value || (type&0xfff) != ev_float || !Held_Finite(value->_float)) return 0;
	context->excuse = value->_float;
	value = PR_FindGlobal(svprogfuncs,"run_t_slowclk",PR_ANY,&type);
	if (!value || (type&0xfff) != ev_float || !Held_Finite(value->_float)) return 0;
	context->slow = value->_float;
	context->hyst = .2f; context->allow = .5f; context->freecredits = 4; context->gap = .15f;
	context->frozenflag = 16; context->slowflag = 524288; context->runningstate = 2;
	return 1;
}

int SV_HeldContextBuildNative(const movevars_t *source, held_record_t *r)
{
	static const unsigned char generation[16] = {0};
	static const char *artifacts[] = {"world","plugin","engine","progs","zone"};
	unsigned char digest[5][32];
	float pins[SV_PMPIN_COUNT], scalar;
	movevars_t saved;
	held_clock_context_t clock;
	char name[80];
	cvar_t *trace;
	unsigned int i;
	int result = 0;
	if (!r) return 0;
	memset(r,0,sizeof(*r));
	if (!source || PMSRC_VERSION != HELD_SOURCE_VERSION || SV_PMPIN_COUNT != HELD_CONTEXT_PINS+3 ||
		source->physicsmode != PHYSMODE_SOURCE || source->sourceversion != HELD_SOURCE_VERSION) goto cleanup;
	if (!Mod_HeldWorldArtifacts(sv.world.worldmodel,digest[0],digest[1])) goto cleanup;
#ifdef PLUGINS
	if (!Plug_ArtifactFingerprint(NULL,NULL,digest[2])) goto cleanup;
#else
	goto cleanup;
#endif
	if (!SV_HeldProgsFingerprint(digest[3]) || !SV_HeldZoneFingerprint(digest[4])) goto cleanup;
	saved = movevars; movevars = *source;
	SV_PMPinFill(pins);
	movevars = saved;
	/* No momentum effects in this projection; never export extra context fields. */
	if (pins[HELD_CONTEXT_PINS] || pins[HELD_CONTEXT_PINS+1] || pins[HELD_CONTEXT_PINS+2] ||
		pmove.waterjumpremaining != 0 || pmove.waterjumpdir[0] != 0 || pmove.waterjumpdir[1] != 0) goto cleanup;
	/* Live source path uses box extents; SV_HullNumForPlayer is MVD spectator-only.
	 * Do not copy that branch's encoded view offset (32) into this normal context. */
	pins[2] = PM_NORMAL; pins[4] = pins[5] = -16; pins[6] = pins[7] = 16; pins[8] = 0;
	if (!SV_HeldRecordBegin(r,HELD_RECORD_HCTX) || !SV_HeldRecordBytes(r,"generation",generation,16) ||
		!SV_HeldRecordL(r,"ordinal",1) || !SV_HeldRecordL(r,"message",0) || !SV_HeldRecordL(r,"execution",0) ||
		!SV_HeldRecordU(r,"context",1) || !SV_HeldRecordU(r,"rules",1)) goto cleanup;
	for (i = 0; i < countof(artifacts); ++i)
		if (!SV_HeldRecordBytes(r,artifacts[i],digest[i],32)) goto cleanup;
	for (i = 0; i < HELD_CONTEXT_PINS; ++i)
	{
		scalar = pins[i]; if (!Held_Finite(scalar)) goto cleanup;
		Q_snprintfz(name,sizeof(name),"ctx_%s",sv_pmpin_names[i]);
		if (SV_HeldRecordF(r,name,scalar)) continue;
		if (scalar == floorf(scalar) && scalar >= -2147483648.0f && scalar < 2147483648.0f)
			if (SV_HeldRecordI(r,name,(int32_t)scalar)) continue;
		if (scalar == floorf(scalar) && scalar >= 0 && scalar < 4294967296.0f)
			if (SV_HeldRecordU(r,name,(uint32_t)scalar)) continue;
		goto cleanup;
	}
	if (!SV_HeldRecordU(r,"ctx_flags",source->flags) || !SV_HeldRecordU(r,"ctx_coordtype",source->coordtype) ||
		!SV_HeldRecordI(r,"ctx_hullnum",0) || !SV_HeldRecordI(r,"ctx_geomtype",0) ||
		!SV_HeldRecordU(r,"ctx_numphysent",1) || !SV_HeldRecordF(r,"ctx_hullmins_z",0) || !SV_HeldRecordF(r,"ctx_hullmaxs_z",62) ||
		!SV_HeldRecordF(r,"ctx_gravitydir_x",0) || !SV_HeldRecordF(r,"ctx_gravitydir_y",0) || !SV_HeldRecordF(r,"ctx_gravitydir_z",-1)) goto cleanup;
	trace = Cvar_FindVar("pm_trisoup_bevels"); if (!trace || !SV_HeldRecordI(r,"ctx_trisoup_i",trace->ival)) goto cleanup;
	trace = Cvar_FindVar("pm_rotatedboxhulls"); if (!trace || !SV_HeldRecordI(r,"ctx_rotboxes_i",trace->ival)) goto cleanup;
	trace = Cvar_FindVar("pm_portalcsg_scanall"); if (!trace || !SV_HeldRecordI(r,"ctx_portalcsg_i",trace->ival)) goto cleanup;
	if (!Held_ClockContextCapture(&clock) || !SV_HeldRecordF(r,"ctx_excuse",clock.excuse) || !SV_HeldRecordF(r,"ctx_slow",clock.slow) ||
		!SV_HeldRecordF(r,"ctx_hyst",clock.hyst) || !SV_HeldRecordF(r,"ctx_allow",clock.allow) ||
		!SV_HeldRecordF(r,"ctx_freecredits",clock.freecredits) || !SV_HeldRecordF(r,"ctx_gap",clock.gap)) goto cleanup;
	if (held_schemas[HELD_RECORD_HCTX].count > HELD_MAX_FIELDS) goto cleanup;
	for (i = 0; i < held_schemas[HELD_RECORD_HCTX].count; ++i) if (!r->assigned[i]) goto cleanup;
	/* Prefix generation/ordinal will be replaced by the owning native ledger. */
	result = 1;
cleanup:
	memset(digest,0,sizeof(digest)); memset(pins,0,sizeof(pins)); memset(&saved,0,sizeof(saved)); memset(name,0,sizeof(name)); memset(&clock,0,sizeof(clock));
	if (!result) memset(r,0,sizeof(*r));
	return result;
}

int SV_HeldContextCapture(client_t *client, const movevars_t *source, held_record_t *record)
{
	float mode, owned;
	if (!record) return 0;
	if (!client || !client->edict || !Held_FieldFloat(client->edict,"rec_hm_mode",&mode) ||
		(mode != 0 && mode != 12) || !Held_FieldFloat(client->edict,"rec_wt_hold",&owned) ||
		(owned != 0 && owned != 1) || (mode == 0 && owned) || !Held_PhysicalFenceState(client,(int)owned,2,mode))
		{memset(record,0,sizeof(*record)); return 0;}
	if (!SV_HeldContextBuildNative(source,record)) return 0;
	if (!SV_HeldRecordI(record,"ctx_geomtype",(int32_t)client->edict->xv->geomtype))
		{memset(record,0,sizeof(*record)); return 0;}
	return 1;
}

static int Held_RecordVector(held_record_t *record, const char *prefix, const float vector[3])
{
	char name[80];
	int i;
	for (i = 0; i != 3; ++i)
	{
		Q_snprintfz(name, sizeof(name), "%s_%c", prefix, 'x' + i);
		if (!SV_HeldRecordF(record, name, vector[i])) return 0;
	}
	return 1;
}

static const struct { const char *name, *field; size_t offset; } held_clock_sources[] = {
#define HC(n, f) {"clock_" #n, f, offsetof(held_clock_t, n)}
	HC(starttick,"run_t_starttick"), HC(startsec,"run_t_startsec"), HC(startwall,"run_t_startwall"),
	HC(tickcarry,"run_t_tickcarry"), HC(lastmt,"run_t_lastmt"), HC(lastsec,"run_t_lastsec"),
	HC(freeze,"run_t_freeze"), HC(frzon,"run_t_frzon"), HC(frzat,"run_t_frzat"), HC(frzmt,"run_t_frzmt"),
	HC(frzcarry,"run_t_frzcarry"), HC(frzticks,"run_t_frzticks"), HC(frzsec,"run_t_frzsec"),
	HC(state,"run_t_state"), HC(ticks,"run_t_ticks"), HC(ckwall,"run_t_ckwall"), HC(ckwt,"run_t_ckwt"),
	HC(ckgaps,"run_t_ckgaps"), HC(ckncr,"run_t_ckncr"), HC(ckcb,"run_t_ckcb"), HC(ckcr,"run_t_ckcr"),
	HC(ckd,"run_t_ckd"), HC(cktr,"run_t_cktr"), HC(ckpk,"run_t_ckpk"), HC(ckup,"run_t_ckup"),
	HC(cku,"run_t_cku"), HC(ckskip,"run_t_ckskip"), HC(resumesec,"run_t_resumesec"),
	HC(packet,"rec_rec_pk"), HC(counted,"run_t_clock"), HC(flags,"run_t_flags")
#undef HC
};
static int Held_ClockCaptureActual(client_t *client, float rate, held_clock_t *clock)
{
	eval_t *value;
	etype_t type;
	unsigned int i;
	float scalar;
	if (!clock) return 0;
	memset(clock,0,sizeof(*clock));
	if (!client || !client->edict || !Held_Finite(rate) || rate < .001f || rate > .05f ||
		client->movetickcount > 16777216 || !Held_Finite(client->pmsrc.msec_carry) ||
		client->pmsrc.msec_carry < 0 || client->pmsrc.msec_carry >= rate) goto bad;
	for (i = 0; i < countof(held_clock_sources); ++i)
	{
		if (!Held_FieldFloat(client->edict,held_clock_sources[i].field,&scalar)) goto bad;
		memcpy((char*)clock+held_clock_sources[i].offset,&scalar,sizeof(scalar));
	}
	value = svprogfuncs->GetEdictFieldValue(svprogfuncs,client->edict,"run_t_lastorg",ev_vector,NULL);
	if (!value) goto bad;
	for (i = 0; i < 3; ++i)
	{
		if (!Held_Finite(value->_vector[i])) goto bad;
		clock->lastorg[i] = value->_vector[i];
	}
	value = PR_FindGlobal(svprogfuncs,"sv_ck_frame",PR_ANY,&type);
	if (!value || (type&0xfff) != ev_float || !Held_Finite(value->_float)) goto bad;
	clock->frame = value->_float;
	clock->elapsed = (float)client->movetickcount * rate;
	clock->elapsed = clock->elapsed + client->pmsrc.msec_carry;
	clock->elapsed = clock->elapsed - clock->startsec;
	clock->elapsed = clock->elapsed - clock->frzsec;
	if (clock->frzon) clock->elapsed = clock->frzat;
	if (!Held_Finite(clock->elapsed)) goto bad;
	return 1;
bad:
	memset(clock,0,sizeof(*clock));
	return 0;
}

int SV_HeldMoveVarsCapture(client_t *client, movevars_t *context)
{
	movevars_t saved;
	if (!context) return 0;
	memset(context,0,sizeof(*context));
	if (!client) return 0;
	if (!Held_Finite(sv_gravity.value) || sv_gravity.value <= 0) return 0;
	/* Raw QW gravity0 is Source's effective default1; keep its exact value. */
	if (!Held_Finite(client->entgravity) || client->entgravity < 0) return 0;
	if (!Held_Finite(client->maxspeed) || client->maxspeed <= 0) return 0;
	/* Derive at THIS boundary; never copy a later prepared pmove backward. */
	saved = movevars;
	memset(&movevars,0,sizeof(movevars));
	SV_SetMoveVars();
	movevars.gravity = sv_gravity.value;
	SV_SetQWClientMoveVars(client);
	*context = movevars;
	movevars = saved;
	return 1; /* Packing only; caller still owns full actor/scene admission. */
}

int SV_HeldInitialCapture(client_t *client, held_record_t *r, held_clock_t *clock,
	held_clock_context_t *clock_context, movevars_t *context)
{
	static const float gravitydir[3] = {0, 0, -1};
	edict_t *e;
	unsigned int i, assigned = 0;
	float rate, scalar;
	if (!Held_PhysicalFenceState(client,0,2,0) || !SV_HeldMoveVarsCapture(client,context)) return 0;
	e = client->edict;
	if (PMSRC_VERSION != HELD_SOURCE_VERSION || context->physicsmode != PHYSMODE_SOURCE ||
		context->sourceversion != HELD_SOURCE_VERSION ||
		context->ticrate < .001f || context->ticrate > .05f || context->standheight != 62 ||
		context->duckheight != 45 || context->viewscale != .5f ||
		(context->entgravity != 0 && context->entgravity != 1) ||
		!Held_Finite(context->gravity) || context->gravity <= 0 ||
		!Held_Finite(context->maxspeed) || context->maxspeed <= 0 ||
		!Held_FieldFloat(e, "run_movetickrate", &rate) || rate != context->ticrate ||
		client->movetickcount > 16777216 ||
		!Held_FieldFloat(e, "run_movetick", &scalar) || scalar != (float)client->movetickcount ||
		!Held_Finite(client->pmsrc.msec_carry) ||
		client->pmsrc.msec_carry < 0 || client->pmsrc.msec_carry >= rate) return 0;
	/* A fresh recording's inactive viewing lane is an actual-source zero guard,
	 * not a sampled/default origin. viewbase is inactive/irrelevant for replay. */
	if (!Held_FieldZero(e,"run_t_viewsec",ev_float) ||
		!Held_FieldZero(e,"run_t_viewwall",ev_float) ||
		!Held_FieldZero(e,"run_t_viewowner",ev_float)) return 0;
	if (!Held_ClockCaptureActual(client,rate,clock)) return 0;
	if (clock->counted != 1 || clock->state != 2 || clock->starttick < 0 || clock->freeze ||
		clock->frzon || clock->frzticks || clock->frzsec || clock->tickcarry || clock->resumesec) return 0;
	if (!Held_ClockContextCapture(clock_context)) return 0;
	if (!SV_HeldRecordBegin(r, HELD_RECORD_HINIT) || !SV_HeldRecordU(r,"context",1)) return 0;
#define HF(n,v) do { if (!SV_HeldRecordF(r,n,v)) return 0; } while (0)
#define HU(n,v) do { if (!SV_HeldRecordU(r,n,v)) return 0; } while (0)
#define HI(n,v) do { if (!SV_HeldRecordI(r,n,v)) return 0; } while (0)
	if (!Held_RecordVector(r,"origin",e->v->origin) || !Held_RecordVector(r,"velocity",e->v->velocity) ||
		!Held_RecordVector(r,"safeorigin",e->v->oldorigin) || !Held_RecordVector(r,"hullmins",e->v->mins) ||
		!Held_RecordVector(r,"hullmaxs",e->v->maxs) || !Held_RecordVector(r,"gravitydir",gravitydir)) return 0;
	HU("safeorigin_known",0); HU("ground",0); HI("groundent",0); HU("pmtype",PM_NORMAL);
	HU("jump_held",client->jump_held); HI("jump_msec",0); HF("waterjumptime",e->v->teleport_time);
	HF("basevelocity_x",0); HF("basevelocity_y",0); HF("basevelocity_z",0); HU("onladder",0); HU("capsule",0);
	HF("surfacefriction",client->pmsrc.surfacefriction); HF("ducktime",client->pmsrc.ducktime);
	HU("ducking",client->pmsrc.ducking); HU("ducked",client->pmsrc.ducked); HF("msec_carry",client->pmsrc.msec_carry);
	HI("oldbuttons",client->pmsrc.oldbuttons); HF("stamina",client->pmsrc.stamina); HF("rampoff",client->pmsrc.rampoff);
	HF("boardcount",client->pmsrc.boardcount); HF("rampcontact",client->pmsrc.rampcontact); HU("srcladder",client->pmsrc.srcladder);
	if (!Held_RecordVector(r,"srcladdernormal",client->pmsrc.srcladdernormal) ||
		!Held_RecordVector(r,"portaloffset",client->pmsrc.portaloffset)) return 0;
	HF("viewheight",e->v->view_ofs[2]); HU("waterlevel",0);
	/* Q1 EMPTY=-1 is a tagged content code, not the native bitmask 0xffffffff. */
	HI("watertype",FTECONTENTS_EMPTY); HF("tickused",rate); HU("portalcrossings",0); HU("numtouch",0);
	if (!SV_HeldRecordL(r,"raw_moveticks",client->movetickcount)) return 0;
	for (i = 0; i < countof(held_clock_sources); ++i)
	{
		memcpy(&scalar, (const char*)clock + held_clock_sources[i].offset, sizeof(scalar));
		HF(held_clock_sources[i].name,scalar);
	}
	HF("clock_elapsed",clock->elapsed); HF("clock_frame",clock->frame);
	if (!Held_RecordVector(r,"clock_lastorg",clock->lastorg)) return 0;
	for (i = 0; i < held_schemas[HELD_RECORD_HINIT].count; ++i) assigned += r->assigned[i] != 0;
#undef HF
#undef HU
#undef HI
	return assigned == 91; /* The four prefix fields are owned by the native ledger. */
}

int SV_HeldClockBoundaryCapture(client_t *client, held_record_family_t family, held_record_t *r)
{
	held_clock_t clock = {0};
	unsigned int i, assigned = 0;
	float rate, scalar;
	int result = 0, terminal = family == HELD_RECORD_HTERMINAL;
	if (!r) return 0;
	memset(r,0,sizeof(*r));
	if (family != HELD_RECORD_HFREEZE && family != HELD_RECORD_HTHAW && !terminal) goto cleanup;
	if (!Held_PhysicalFenceState(client,family == HELD_RECORD_HFREEZE,terminal ? 3:2,12) ||
		!Held_FieldFloat(client->edict,"run_movetickrate",&rate) || !Held_ClockCaptureActual(client,rate,&clock) ||
		clock.counted != 1 || !SV_HeldRecordBegin(r,family)) goto cleanup;
	if (family == HELD_RECORD_HFREEZE)
	{
		if (clock.freeze != 1 || clock.frzon != 1) goto cleanup;
	}
	else if (clock.freeze || clock.frzon || clock.frzat) goto cleanup;
	if (terminal && !SV_HeldRecordU(r,"closekind",1)) goto cleanup;
	for (i = 0; i < countof(held_clock_sources); ++i)
	{
		memcpy(&scalar,(const char*)&clock+held_clock_sources[i].offset,sizeof(scalar));
		if (!SV_HeldRecordF(r,held_clock_sources[i].name,scalar)) goto cleanup;
	}
	if (!SV_HeldRecordF(r,"clock_elapsed",clock.elapsed) || !SV_HeldRecordF(r,"clock_frame",clock.frame) ||
		!Held_RecordVector(r,"clock_lastorg",clock.lastorg)) goto cleanup;
	for (i = 0; i < held_schemas[family].count; ++i) assigned += r->assigned[i] != 0;
	result = assigned+4 == held_schemas[family].count;
cleanup:
	memset(&clock,0,sizeof(clock));
	if (!result) memset(r,0,sizeof(*r));
	return result;
}

int SV_HeldObservationCapture(client_t *client, unsigned int boundary, held_record_t *r)
{
	edict_t *e;
	unsigned int i, assigned = 0;
	float rate;
	int result = 0;
	if (!r) return 0;
	memset(r,0,sizeof(*r));
	if (boundary < 1 || boundary > 4 || !Held_PhysicalFenceState(client,0,boundary == 4 ? 3:2,12) ||
		!SV_HeldAirCell(client) || client->movetickcount > 16777216 ||
		!Held_FieldFloat(client->edict,"run_movetickrate",&rate) || rate < .001f || rate > .05f ||
		!Held_Finite(client->pmsrc.msec_carry) || client->pmsrc.msec_carry < 0 || client->pmsrc.msec_carry >= rate ||
		!SV_HeldRecordBegin(r,HELD_RECORD_HOBS)) goto cleanup;
	e = client->edict;
#define HO_F(n,v) do {if (!Held_Finite(v) || !SV_HeldRecordF(r,n,v)) goto cleanup;} while (0)
#define HO_U(n,v) do {if (!SV_HeldRecordU(r,n,v)) goto cleanup;} while (0)
	HO_U("boundary",boundary); HO_U("ground",0);
	if (!Held_RecordVector(r,"origin",e->v->origin) || !Held_RecordVector(r,"velocity",e->v->velocity) ||
		!Held_RecordVector(r,"hullmins",e->v->mins) || !Held_RecordVector(r,"hullmaxs",e->v->maxs) ||
		!SV_HeldRecordL(r,"raw_moveticks",client->movetickcount)) goto cleanup;
	HO_F("tickused",rate); HO_F("msec_carry",client->pmsrc.msec_carry);
	HO_F("surfacefriction",client->pmsrc.surfacefriction); HO_F("ducktime",client->pmsrc.ducktime);
	HO_U("ducking",client->pmsrc.ducking); HO_U("ducked",client->pmsrc.ducked);
	HO_F("stamina",client->pmsrc.stamina); HO_F("rampoff",client->pmsrc.rampoff);
	HO_F("boardcount",client->pmsrc.boardcount); HO_F("rampcontact",client->pmsrc.rampcontact);
	HO_U("srcladder",client->pmsrc.srcladder);
	if (!Held_RecordVector(r,"srcladdernormal",client->pmsrc.srcladdernormal) ||
		!Held_RecordVector(r,"portaloffset",client->pmsrc.portaloffset)) goto cleanup;
	for (i = 0; i < held_schemas[HELD_RECORD_HOBS].count; ++i) assigned += r->assigned[i] != 0;
	result = assigned+4 == held_schemas[HELD_RECORD_HOBS].count;
#undef HO_F
#undef HO_U
cleanup:
	if (!result) memset(r,0,sizeof(*r));
	return result;
}
