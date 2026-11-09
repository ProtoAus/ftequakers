/* FTESurf Patch 1443: tas_rollout -- what n ticks of given inputs would do from the paused TAS
 * body, through SV_RunCmd's own mover setup and one PM_PlayerMove a tick. The edict, the client's
 * movement state, the recorder and QC are never written; pmove, movevars, pmove_mins/maxs and
 * frametime are restored. (SV_SetQWClientMoveVars' noclip-speed latch and the trace/area caches
 * are touched as the next real command would touch them; none changes what a later move does.)
 * Per-command QC -- PlayerPreThink's carriers, limit/slide volumes, gravity; the touch loop --
 * does not run. So each tick names the first entity that QC could act on (a touched solid whose
 * touch would run, a non-world ground entity, any trigger volume, with or without .touch) and the
 * rollout goes on: QC, which knows what each one does, decides where the prediction stops.
 * Starts only where SV_RunCmd's inputs are the plain ones; there the copy-back between commands
 * hands the next command exactly the pmove the last one left, so carrying pmove across ticks is
 * the same. */

#define SV_TAS_ROLL_MAX 2048
#define SV_TAS_ROLL_IN 7	/* fwd side up, pitch yaw roll angle words, buttons (tas_tape's words) */
#define SV_TAS_ROLL_OUT 13	/* origin3 velocity3 onground ducked contact-kind normal3 entity-QC-may-act-on (negated: before the tick) */

/* The first entity per-command QC could act on after this tick (0 none): SV_RunCmd's touch loop
 * (a touched non-world solid whose touch or the player's would run; such a touch also makes the
 * contact kind 3, as SV_RunCmd's observation does), a non-world ground entity (P924 conveyors),
 * then any trigger volume. */
static int SV_TASRolloutActor(edict_t *e, int *kind)
{
	wedict_t *t;
	int j, index, rep = 0;
	for (j = 0; j < pmove.numtouch; ++j)
	{
		index = pmove.touchindex[j];
		if (index < 0 || index >= pmove.numphysent)
		{
			*kind = 3;
			continue;
		}
		if (!pmove.physents[index].info)
			continue;
		*kind = 3;
		if (!rep && !pmove.physents[index].notouch)
		{
			edict_t *te = EDICT_NUM_PB(svprogfuncs, pmove.physents[index].info);
			if (te->v->touch || e->v->touch)
				rep = pmove.physents[index].info;
		}
	}
	if (!rep && pmove.onground && pmove.groundent >= 0 && pmove.groundent < pmove.numphysent &&
		pmove.physents[pmove.groundent].info)
		rep = pmove.physents[pmove.groundent].info;
	if (!rep && (t = World_TriggerAt(&sv.world, (wedict_t*)e, pmove.origin, pmove.player_mins, pmove.player_maxs, true)))
		rep = NUM_FOR_EDICT(svprogfuncs, (edict_t*)t);
	return rep;
}

/* Returns the ticks run (0..n; fewer when a tick the plain case does not cover came), or a
 * negative reason it cannot start: -1 no TAS client/arguments, -2 not a plain Source walker,
 * -3 not paused or held, -4 a QC movement limit or forced duck pending, -5 a gravity direction,
 * -6 water/ladder/duck transition, -7 non-default gravity (bank gravity would observe it),
 * -8 the TAS clock, -9 a carrier (QC's run_basevel, or standing on a non-world entity),
 * -10 a row that does not decode. A trigger volume the body starts in is named, negated, on the first tick. */
int SV_TASRollout(edict_t *e, const float *in, int n, float *out)
{
	static playermove_t saved;
	extern float frametime;
	movevars_t savedmv;
	vec3_t savedmins, savedmaxs, normal, point;
	float savedframetime, distance;
	sv_tas_input_t row;
	client_t *cl;
	eval_t *ev;
	wedict_t *start;
	extern cvar_t pm_physicsmode;
	int i, k, msec, kind, rep, startrep, ran;
	cl = SV_TASRewindClient(e);
	if (!cl || !SV_TASInputPool(cl) || n < 1 || n > SV_TAS_ROLL_MAX || !in || !out)
		return -1;
	if (pm_physicsmode.ival != PHYSMODE_SOURCE || sv.gamespeed != 1 ||
		e->v->movetype != MOVETYPE_WALK || !(e->v->health > 0))
		return -2;
	if (SV_TASInputField(e, "run_tas_paused") != 1 || SV_TASInputField(e, "run_pmhold"))
		return -3;
	if (SV_TASInputField(e, "run_momlimit") || SV_TASInputField(e, "run_momcool") ||
		SV_TASInputField(e, "run_momslide") || SV_TASInputField(e, "run_forceduck"))
		return -4;
	if (e->xv->gravitydir[0] || e->xv->gravitydir[1] || (e->xv->gravitydir[2] && e->xv->gravitydir[2] != -1))
		return -5;
	if (cl->pmsrc.waterjumpremaining || cl->pmsrc.srcladder || cl->pmsrc.ducking || e->v->waterlevel >= 2 ||
		((int)e->xv->pmove_flags & PMF_LADDER))
		return -6;
	// P1129's observation only acts when movevars.entgravity (SV_SetQWClientMoveVars' ratio) is
	// neither 0 (default) nor 1; SV_RunCmd arms it, this does not.
	if (SV_BankGravityEnabled() && movevars.gravity && cl->entgravity &&
		cl->entgravity / movevars.gravity != 1)
		return -7;
	if (!SV_TASClockForClient(cl, NULL))
		return -8;
	// The carrier PreThink cashes out or hands over (SV_BaseVelocityFrame, SV_ConveyorArm), not the
	// engine's read-and-clear run_basevelocity, which is always zero while paused.
	ev = SV_TASRewindField(e, "run_basevel", ev_vector);
	if (ev && !VectorCompare(ev->_vector, vec3_origin))
		return -9;
	if (((int)e->v->flags & FL_ONGROUND) && e->v->groundentity)
		return -9;
	msec = SV_TASTickMsec(SV_TASCurrentTick());
	if (!msec)
		return -8;
	for (k = 0; k < n; ++k)
		if (!SV_TASInputDecode(in + k*SV_TAS_ROLL_IN, in + k*SV_TAS_ROLL_IN + 3, in[k*SV_TAS_ROLL_IN + 6], &row))
			return -10;

	memcpy(&saved, &pmove, sizeof(pmove));
	savedmv = movevars;
	VectorCopy(pmove_mins, savedmins);
	VectorCopy(pmove_maxs, savedmaxs);
	savedframetime = frametime;

	// SV_RunCmd's setup, the plain case
	VectorCopy(e->v->mins, pmove.player_mins);
	VectorCopy(e->v->maxs, pmove.player_maxs);
	if (e->xv->gravitydir[2])
		VectorCopy(e->xv->gravitydir, pmove.gravitydir);
	else
		VectorCopy(sv.world.g.defaultgravitydir, pmove.gravitydir);
	VectorCopy(e->v->origin, pmove.origin);
	VectorCopy(e->v->oldorigin, pmove.safeorigin);
	pmove.safeorigin_known = progstype != PROG_QW;
	VectorCopy(e->v->velocity, pmove.velocity);
	VectorCopy(e->v->v_angle, pmove.angles);
	pmove.pm_type = SV_PMTypeForClient(cl, e);
	pmove.onground = ((int)e->v->flags & FL_ONGROUND) != 0;
	pmove.jump_held = cl->jump_held;
	PMSrc_LoadState(&cl->pmsrc);
	VectorClear(pmove.basevelocity);
	pmove.momlimit = 0;
	pmove.momcool = 0;
	pmove.momslide = 0;
	pmove.waterjumptime = (progstype != PROG_QW) ? e->v->teleport_time - sv.time : e->v->teleport_time;
	pmove.skipent = -1;
	pmove.capsule = (e->xv->geomtype == GEOMTYPE_CAPSULE);
	pmove.onladder = false;
	pmove.world = &sv.world;
	SV_SetQWClientMoveVars(cl);
	// A volume the body already stands in: PreThink finds limit/slide volumes by overlap
	start = World_TriggerAt(&sv.world, (wedict_t*)e, pmove.origin, pmove.player_mins, pmove.player_maxs, true);
	startrep = start ? NUM_FOR_EDICT(svprogfuncs, (edict_t*)start) : 0;

	ran = 0;
	if (pmove.pm_type == PM_NORMAL && !pmove.waterjumptime)
	for (k = 0; k < n; ++k)
	{
		const float *r = in + k*SV_TAS_ROLL_IN;
		float *o = out + k*SV_TAS_ROLL_OUT;
		SV_TASInputDecode(r, r + 3, r[6], &row);
		memset(&pmove.cmd, 0, sizeof(pmove.cmd));
		pmove.cmd.msec = msec;
		pmove.cmd.forwardmove = row.move[0];
		pmove.cmd.sidemove = row.move[1];
		pmove.cmd.upmove = row.move[2];
		pmove.cmd.buttons = ((row.buttons & 1) ? BUTTON_JUMP : 0) |
			((row.buttons & 2) ? BUTTON_DUCK : 0) | ((row.buttons & 4) ? BUTTON_SPEED : 0);
		for (i = 0; i < 3; ++i)
			pmove.cmd.angles[i] = row.angle[i];
		pmove.jump_msec = 0;
		pmove.numphysent = 1;
		pmove.physents[0].model = sv.world.worldmodel;
		for (i = 0; i < 3; ++i)
		{
			pmove_mins[i] = pmove.origin[i] - 256;
			pmove_maxs[i] = pmove.origin[i] + 256;
		}
		AddAllLinksToPmove(&sv.world, (wedict_t*)e);
		PMSrc_TASContactBegin(true);
		PMSrc_BankGravityBegin(false);
		PM_PlayerMove(sv.gamespeed);
		PMSrc_BankGravityEnd();
		kind = PMSrc_TASContactEnd(normal, &distance, point);
		if (pmove.ticksrun != 1 || pmove.portalcrossings || pmove.pm_type != PM_NORMAL ||
			pmove.srcladder || pmove.waterlevel > 1 || pmove.numtouch < 0 || pmove.numtouch > MAX_PHYSENTS)
			break;		// a tick the plain case does not cover: stop before it counts
		rep = SV_TASRolloutActor(e, &kind);
		if (k == 0 && startrep)
			rep = -startrep;	// negative, and first: PreThink acts on it before tick 1's move
		VectorCopy(pmove.origin, o);
		VectorCopy(pmove.velocity, o + 3);
		o[6] = pmove.onground;
		o[7] = pmove.ducked;
		o[8] = kind;
		if (kind == 1)
			VectorCopy(normal, o + 9);
		else
			VectorClear(o + 9);
		o[12] = rep;
		ran = k + 1;
	}

	memcpy(&pmove, &saved, sizeof(pmove));
	movevars = savedmv;
	VectorCopy(savedmins, pmove_mins);
	VectorCopy(savedmaxs, pmove_maxs);
	frametime = savedframetime;
	return ran;
}
