/* FTESurf Patch 1443: tas_rollout -- what n ticks of given inputs would do from the paused TAS
 * body, through SV_RunCmd's own mover setup and one PM_PlayerMove a tick. The edict, the client's
 * movement state and the recorder are never written, nor QC memory beyond the two buffers (P1449:
 * FOLLOW/AIM rows get the words they computed written back); pmove, movevars, pmove_mins/maxs and
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
/* contact-kind: pm_source.c's (0 none, 1 world ramp, 2 nonworld, 3 ambiguous); P1449: 4 = no ramp but a wall or ceiling clipped */
/* P1449: buttons bits a row may carry. FOLLOW: the yaw slot holds a turn command q (sign = strafe side, +
 * left) and this tick's yaw word and side are set from the velocity before it, so that the wish direction
 * dotted with the horizontal velocity is 30 - |q| (Source's AirAccelerate adds 30 - that: 0 nothing, 30
 * the most speed, 60 the hardest turn that keeps speed). PITCHV: the pitch word follows the velocity.
 * Both are written back into the caller's row (the flag cleared), so the tape it then plays is the rows
 * this rollout ran, exactly. A QC-side approximation needed several rollouts per candidate and did not
 * converge on a ramp, where contact makes the path sensitive to every tick. */
#define SV_TAS_ROLL_FOLLOW 256
#define SV_TAS_ROLL_PITCHV 512
/* P1449: with FOLLOW, and once the body has had no contact for 3 ticks (a flight, not a ramp's seam), the
 * command steers toward the aim point tas_rollout was given: its sign is the side the point's bearing lies
 * on, its size 30 (the most speed) on the bearing rising to 60 (the hardest turn that keeps speed) aimgain
 * degrees off. On the bearing the side flips as the heading crosses it -- the strafe zigzag that flies
 * straight while gaining. The ramp ride keeps the row's own command. */
#define SV_TAS_ROLL_AIM 1024
/* P1466: the row's tick reports a wall or ceiling clipped (pm_source.c's pms_tas_wall) as bit 2 of out[7] beside
 * ducked (bit 1), on a ramp or the ground too -- contact kind 4 is only set with no ramp under the body, so a ride
 * rubbing a side wall said nothing (Lex, 10 Nov: the bot rubs walls and loses speed). Opt-in: a caller that does
 * not ask reads out[7] as ducked, as before. Like the bits above it is cleared from each row that runs (the row
 * the loop stops on too; rows past it keep it), so a caller sets it again before rolling the same rows. */
#define SV_TAS_ROLL_WALLBIT 2048
/* P1476: with AIM, the strafe side holds until the bearing is SV_TAS_AIMWIDE_DEG past the other way and the side has been
 * held SV_TAS_AIMWIDE_TICKS: wider weaves instead of a flip each tick the heading crosses the bearing (Lex, 10 Nov: the
 * "hyper wiggle" reads badly; a little accuracy for the look). Held against the error, the command is 30, the most speed. */
#define SV_TAS_ROLL_AIMWIDE 4096
#define SV_TAS_AIMWIDE_DEG 3
#define SV_TAS_AIMWIDE_TICKS 8

static float SV_TASRollWord(double deg)
{
	double w = floor(deg * 65536.0 / 360.0 + 0.5);
	while (w > 32767) w -= 65536;
	while (w < -32768) w += 65536;
	return (float)w;
}

/* The flagged slots of row r from pmove's velocity (this tick's start), written back. */
static void SV_TASRollFollow(float *r, int fl)
{
	double vh = sqrt(pmove.velocity[0] * pmove.velocity[0] + pmove.velocity[1] * pmove.velocity[1]);
	if (fl & SV_TAS_ROLL_FOLLOW)
	{
		double q = r[4], c, ph, side = fabs(r[1]) ? (fabs(r[1]) > 32767 ? 32767 : fabs(r[1])) : 450;	/* -side*sd stays a short */
		int sd = (q > 0 || (q == 0 && r[1] <= 0)) ? 1 : -1;	/* a zero command keeps the row's side */
		c = (30 - fabs(q)) / (vh > 1 ? vh : 1);
		if (c > 1) c = 1;
		if (c < -1) c = -1;
		ph = acos(c) * (180.0 / M_PI);	/* the wish direction's angle from the velocity */
		r[4] = SV_TASRollWord(atan2(pmove.velocity[1], pmove.velocity[0]) * (180.0 / M_PI) + sd * (ph - 90));
		r[1] = -side * sd;		/* Source: negative sidemove is left */
	}
	if (fl & SV_TAS_ROLL_PITCHV)
	{
		double p = atan2(-pmove.velocity[2], vh) * (180.0 / M_PI);
		r[3] = SV_TASRollWord(p > 85 ? 85 : (p < -85 ? -85 : p));
	}
	r[6] -= fl;
}

/* P1449: the AIM command for this tick (see SV_TAS_ROLL_AIM). With the face to board (n), it steers at a
 * lead point set back from the aim along the heading that meets the face parallel -- along the ramp's
 * horizontal run (signed by the heading now), tilted out from the face so the horizontal motion cancels
 * the fall into it (v.n = 0 at this vertical speed) -- so the flight curves in along the ramp, not onto
 * its top or into its side (Lex, 9 Nov). P1466: set back lead x the distance (0.5 before; 0 flies straight
 * at the aim and boards with a snap -- a long launch past geometry wants that, taken wide it was lost: Lex). */
static float SV_TASRollAim(const float *aim, float gain, const float *n, float lead)
{
	double tx = aim[0], ty = aim[1];
	double err, mag;
	if (n && (n[0] || n[1]))
	{
		double nh = sqrt(n[0] * n[0] + n[1] * n[1]), ux = n[0] / nh, uy = n[1] / nh;	/* out from the face */
		double rx = -uy, ry = ux, vh = sqrt(pmove.velocity[0] * pmove.velocity[0] + pmove.velocity[1] * pmove.velocity[1]);
		double s, c, dx, dy, dist = sqrt((aim[0] - pmove.origin[0]) * (aim[0] - pmove.origin[0]) + (aim[1] - pmove.origin[1]) * (aim[1] - pmove.origin[1]));
		if (rx * pmove.velocity[0] + ry * pmove.velocity[1] < 0) { rx = -rx; ry = -ry; }	/* the run's way we are going */
		s = vh > 1 ? -pmove.velocity[2] * n[2] / (vh * nh) : 0;
		if (s > 0.9) s = 0.9;
		if (s < -0.9) s = -0.9;
		c = sqrt(1 - s * s);
		dx = rx * c + ux * s; dy = ry * c + uy * s;
		tx -= dx * lead * dist; ty -= dy * lead * dist;
	}
	err = atan2(ty - pmove.origin[1], tx - pmove.origin[0]) - atan2(pmove.velocity[1], pmove.velocity[0]);
	err *= 180.0 / M_PI;
	while (err > 180) err -= 360;
	while (err < -180) err += 360;
	mag = 30 + 30 * (fabs(err) / gain < 1 ? fabs(err) / gain : 1);
	return (float)(err >= 0 ? mag : -mag);
}

/* A row's buttons without the P1449 bits, or -1 when they are not a valid set. */
static int SV_TASRollFlags(const float *r)
{
	int b;
	if (!(r[6] >= 0 && r[6] <= 7 + SV_TAS_ROLL_FOLLOW + SV_TAS_ROLL_PITCHV + SV_TAS_ROLL_AIM + SV_TAS_ROLL_WALLBIT + SV_TAS_ROLL_AIMWIDE && floor(r[6]) == r[6]))
		return -1;
	b = (int)r[6];
	if ((b & SV_TAS_ROLL_FOLLOW) && !(r[4] >= -180 && r[4] <= 180))
		return -1;
	if ((b & SV_TAS_ROLL_AIM) && !(b & SV_TAS_ROLL_FOLLOW))
		return -1;
	if ((b & SV_TAS_ROLL_AIMWIDE) && !(b & SV_TAS_ROLL_AIM))
		return -1;
	return b & (SV_TAS_ROLL_FOLLOW | SV_TAS_ROLL_PITCHV | SV_TAS_ROLL_AIM | SV_TAS_ROLL_WALLBIT | SV_TAS_ROLL_AIMWIDE);
}

/* Patch 1467: this thread is a tas_rollout_batch worker: per-tick entity and trigger queries use their own visit
 * lists (the shared stamps are the main thread's), and what only the main thread may do aborts the candidate. */
static FTE_TLS qboolean sv_tasroll_worker;
/* a worker's brush trigger needs its model loaded already (loading is the main thread's) */
static qboolean SV_TASModelLoaded (world_t *w, int mi)
{
	model_t *m = ((unsigned int)mi < MAX_PRECACHE_MODELS) ? sv.models[mi] : NULL;
	if (m && m->loadstate == MLS_LOADED)
		return true;
	pm_threadabort = true;
	return false;
}

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
	if (!rep && (t = (sv_tasroll_worker ? World_TriggerAtLocal(&sv.world, (wedict_t*)e, pmove.origin, pmove.player_mins, pmove.player_maxs, true, SV_TASModelLoaded)
		: World_TriggerAt(&sv.world, (wedict_t*)e, pmove.origin, pmove.player_mins, pmove.player_maxs, true))))
		rep = NUM_FOR_EDICT(svprogfuncs, (edict_t*)t);
	return rep;
}

/* Returns the ticks run (0..n; fewer when a tick the plain case does not cover came), or a
 * negative reason it cannot start: -1 no TAS client/arguments, -2 not a plain Source walker,
 * -3 not paused or held, -4 a QC movement limit or forced duck pending, -5 a gravity direction,
 * -6 water/ladder/duck transition, -7 non-default gravity (bank gravity would observe it),
 * -8 the TAS clock, -9 a carrier (QC's run_basevel, or standing on a non-world entity),
 * -10 a row that does not decode. A trigger volume the body starts in is named, negated, on the first tick. */
/* Patch 1467: the checks every rollout makes, 0 or SV_TASRollout's negative reason. */
static int SV_TASRollCheck(edict_t *e, client_t **pcl, int *pmsec)
{
	client_t *cl;
	eval_t *ev;
	extern cvar_t pm_physicsmode;
	int msec;
	cl = SV_TASRewindClient(e);
	if (!cl || !SV_TASInputPool(cl))
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
	*pmsec = msec = SV_TASTickMsec(SV_TASCurrentTick());
	if (!msec)
		return -8;
	*pcl = cl;
	return 0;
}

/* Patch 1467: n rows decode (an AIM row needs an aim point): 0 or -10. */
static int SV_TASRollRows(const float *in, int n, const float *aim)
{
	sv_tas_input_t row;
	int k;
	for (k = 0; k < n; ++k)
	{	/* P1449: a FOLLOW row's yaw slot is a command, checked by SV_TASRollFlags, not as a word */
		const float *r = in + k*SV_TAS_ROLL_IN;
		float ang[3];
		int fl = SV_TASRollFlags(r);
		if (fl < 0 || ((fl & SV_TAS_ROLL_AIM) && !aim))
			return -10;
		ang[0] = r[3]; ang[1] = (fl & SV_TAS_ROLL_FOLLOW) ? 0 : r[4]; ang[2] = r[5];
		if (!SV_TASInputDecode(r, ang, r[6] - fl, &row))
			return -10;
	}
	return 0;
}

/* Patch 1467: SV_RunCmd's setup into this thread's pmove and movevars (main thread: it may queue a reliable
 * message); returns the trigger volume the body starts in. */
static int SV_TASRollSetup(edict_t *e, client_t *cl)
{
	wedict_t *start;
	int startrep;
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
	return startrep;
}

/* Patch 1467: the ticks, from this thread's pmove as SV_TASRollSetup left it. */
static int SV_TASRollRun(edict_t *e, float *in, int n, float *out, const float *aim, float aimgain, const float *aimn, float aimlead,
	int msec, int startrep)
{
	vec3_t normal, point;
	float distance;
	sv_tas_input_t row;
	int i, k, kind, rep, ran, airk, wall, wallbit, aimheld;
	float aimside;
	ran = 0;
	airk = 0;
	aimside = 0; aimheld = 0;
	if (!(aimgain > 0)) aimgain = 10;	/* NaN too */
	if (!(aimlead >= 0 && aimlead <= 1)) aimlead = 0.5;	/* P1466: P1449's set-back; NaN too */
	if (aimlead > 0.9) aimlead = 0.9;	/* at 1 an approach along the face puts the lead point on the body (review) */
	if (pmove.pm_type == PM_NORMAL && !pmove.waterjumptime)
	for (k = 0; k < n; ++k)
	{
		float *r = in + k*SV_TAS_ROLL_IN;
		float *o = out + k*SV_TAS_ROLL_OUT;
		int fl = SV_TASRollFlags(r);
		wallbit = (fl & SV_TAS_ROLL_WALLBIT) != 0;	/* SV_TASRollFollow clears every flag from r[6] */
		if ((fl & SV_TAS_ROLL_AIM) && airk >= 3)
		{
			r[4] = SV_TASRollAim(aim, aimgain, aimn, aimlead);	/* P1449: a flight steers to the aim point */
			if (fl & SV_TAS_ROLL_AIMWIDE)
			{	/* P1476: |q| = 30 + 30 min(1, |err| / gain), so the error is read back from the command */
				float side = r[4] >= 0 ? 1 : -1, err = (fabs(r[4]) - 30) / 30 * aimgain;
				/* the error reads back capped at aimgain: a gain under 2x the hold would never let go (review) */
				if (aimside != 0 && side != aimside && (err < (aimgain * 0.5 < SV_TAS_AIMWIDE_DEG ? aimgain * 0.5 : SV_TAS_AIMWIDE_DEG) || aimheld < SV_TAS_AIMWIDE_TICKS))
					r[4] = aimside * 30;
				else if (side != aimside)
				{
					aimside = side;
					aimheld = 0;
				}
				aimheld++;
			}
		}
		else
		{
			aimside = 0;
			aimheld = 0;
		}
		if (fl > 0)
			SV_TASRollFollow(r, fl & ~SV_TAS_ROLL_AIM);	/* P1449: from the velocity this tick starts with */
		if (fl & SV_TAS_ROLL_AIM)
			r[6] -= SV_TAS_ROLL_AIM;
		if (!SV_TASInputDecode(r, r + 3, r[6], &row))
			break;		/* a written-back word out of range (a NaN velocity): not a zero-input tick */
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
		if (sv_tasroll_worker)
		{	/* Patch 1467: its own box (pmove_mins/maxs are the main thread's), its own visit list (the shared
			 * stamps are too); and a portal's trace runs QC */
			vec3_t bmins, bmaxs;
			for (i = 0; i < 3; ++i)
			{
				bmins[i] = pmove.origin[i] - 256;
				bmaxs[i] = pmove.origin[i] + 256;
			}
			AddAllLinksToPmoveLocal(&sv.world, (wedict_t*)e, bmins, bmaxs);
			if (PM_AnyPortals())
				pm_threadabort = true;
			if (pm_threadabort)
				break;
		}
		else
		{
			for (i = 0; i < 3; ++i)
			{
				pmove_mins[i] = pmove.origin[i] - 256;
				pmove_maxs[i] = pmove.origin[i] + 256;
			}
			AddAllLinksToPmove(&sv.world, (wedict_t*)e);
		}
		PMSrc_TASContactBegin(true);
		PMSrc_BankGravityBegin(false);
		PM_PlayerMove(sv.gamespeed);
		PMSrc_BankGravityEnd();
		wall = PMSrc_TASContactWall();
		kind = PMSrc_TASContactEnd(normal, &distance, point);
		if (pmove.ticksrun != 1 || pmove.portalcrossings || pmove.pm_type != PM_NORMAL ||
			pmove.srcladder || pmove.waterlevel > 1 || pmove.numtouch < 0 || pmove.numtouch > MAX_PHYSENTS)
			break;		// a tick the plain case does not cover: stop before it counts
		rep = SV_TASRolloutActor(e, &kind);
		if (kind == 0 && wall)
			kind = 4;
		if (k == 0 && startrep)
			rep = -startrep;	// negative, and first: PreThink acts on it before tick 1's move
		VectorCopy(pmove.origin, o);
		VectorCopy(pmove.velocity, o + 3);
		o[6] = pmove.onground;
		o[7] = pmove.ducked + ((wallbit && wall) ? 2 : 0);	/* P1466 */
		o[8] = kind;
		if (kind == 1)
			VectorCopy(normal, o + 9);
		else
			VectorClear(o + 9);
		o[12] = rep;
		ran = k + 1;
		airk = ((kind >= 1 && kind <= 3) || pmove.onground) ? 0 : airk + 1;	/* P1449: ticks since the last ramp or ground contact */
	}
	return ran;
}

int SV_TASRollout(edict_t *e, float *in, int n, float *out, const float *aim, float aimgain, const float *aimn, float aimlead)
{
	static playermove_t saved;
	extern FTE_TLS float frametime;
	movevars_t savedmv;
	vec3_t savedmins, savedmaxs;
	float savedframetime;
	client_t *cl;
	int msec, startrep, ran;
	if (n < 1 || n > SV_TAS_ROLL_MAX || !in || !out)
		return -1;
	ran = SV_TASRollCheck(e, &cl, &msec);
	if (ran)
		return ran;
	if (SV_TASRollRows(in, n, aim))
		return -10;

	memcpy(&saved, &pmove, sizeof(pmove));
	savedmv = movevars;
	VectorCopy(pmove_mins, savedmins);
	VectorCopy(pmove_maxs, savedmaxs);
	savedframetime = frametime;

	startrep = SV_TASRollSetup(e, cl);
	ran = SV_TASRollRun(e, in, n, out, aim, aimgain, aimn, aimlead, msec, startrep);

	memcpy(&pmove, &saved, sizeof(pmove));
	movevars = savedmv;
	VectorCopy(savedmins, pmove_mins);
	VectorCopy(savedmaxs, pmove_maxs);
	frametime = savedframetime;
	return ran;
}

/* Patch 1467: tas_rollout_batch -- count candidates of n rows each (rows count*n*7, out count*n*13) from the same
 * paused body, on sv_tas_threads worker threads. params holds 5 floats a candidate: its aim point (x y z) and
 * aimlead, and on return the ticks it ran (SV_TASRollout's value). A worker that meets what only the main thread
 * may do (a model to load, a portal, more nearby edicts than its own visit list holds) leaves its candidate to the
 * main thread, which runs it as tas_rollout does; so does every candidate where threads cannot run (no MULTITHREAD,
 * a map with portals, a Quake 2/3 BSP: its trace keeps statics). Returns how many workers ran, or SV_TASRollout's
 * negative reason. */
#define SV_TAS_BATCH_PARAMS 5
#define SV_TAS_BATCH_MAX 256
static cvar_t sv_tas_threads = CVARD("sv_tas_threads", "6", "Patch 1467: worker threads for tas_rollout_batch (1: none)");
typedef struct
{
	edict_t *e;
	float *in, *out, *params;
	int n, count, nthreads, msec, startrep;
	float aimgain;
	const float *aimn;
	const playermove_t *pm0;
	movevars_t mv0;
} sv_tasroll_batch_t;
typedef struct
{
	sv_tasroll_batch_t *b;
	int t;
} sv_tasroll_thread_t;

static const void *pms_snapshot;	/* the main thread's mover state, copied before the workers start */
static int SV_TASRollWorker(void *arg)
{
	sv_tasroll_thread_t *th = arg;
	sv_tasroll_batch_t *b = th->b;
	/* allowed to fail (BZ_Malloc would Sys_Error from a worker): then its candidates stay the main thread's */
	float *rows = BZF_Malloc(b->n * SV_TAS_ROLL_IN * sizeof(float));
	float *outs = BZF_Malloc(b->n * SV_TAS_ROLL_OUT * sizeof(float));
	/* its own movement state: pmove, movevars, the mover's, the traces' box hulls and clip flags (pm_thread.h) */
	playermove_t *pm = BZF_Malloc(sizeof(playermove_t));
	movevars_t *mv = BZF_Malloc(sizeof(movevars_t));
	void *pms = BZF_Malloc(PMSrc_ThreadStateSize()), *pmt = BZF_Malloc(PM_ThreadStateSize());
	void *wbs = BZF_Malloc(World_ThreadStateSize()), *bs = BZF_Malloc(BIH_ThreadStateSize());
	int k, ran;
	if (!rows || !outs || !pm || !mv || !pms || !pmt || !wbs || !bs)
	{	/* its candidates stay -12: the main thread runs them */
		BZ_Free(rows); BZ_Free(outs); BZ_Free(pm); BZ_Free(mv); BZ_Free(pms); BZ_Free(pmt); BZ_Free(wbs); BZ_Free(bs);
		return 0;
	}
	memcpy(pms, pms_snapshot, PMSrc_ThreadStateSize());	/* the main thread's latches (a one-time warning stays one) */
	memset(pmt, 0, PM_ThreadStateSize());
	memset(wbs, 0, World_ThreadStateSize());
	memset(bs, 0, BIH_ThreadStateSize());
	sv_tasroll_worker = true;
	PM_ThreadBind(pm, mv);
	PMSrc_ThreadState(pms);
	PM_ThreadState(pmt);
	World_ThreadState(wbs);
	BIH_ThreadState(bs);
	bih_probe_plane = -1;
	PM_InitBoxHull();
	World_InitThreadBoxHull();
	for (k = th->t; k < b->count; k += b->nthreads)
	{
		float *p = b->params + k * SV_TAS_BATCH_PARAMS;
		float *in = b->in + (size_t)k * b->n * SV_TAS_ROLL_IN;
		memcpy(&pmove, b->pm0, sizeof(pmove));
		movevars = b->mv0;
		pm_threadabort = false;
		memcpy(rows, in, b->n * SV_TAS_ROLL_IN * sizeof(float));		/* rows and outputs land only if it finishes */
		ran = SV_TASRollRun(b->e, rows, b->n, outs, (b->aimgain > 0) ? p : NULL, b->aimgain, b->aimn, p[3], b->msec, b->startrep);
		if (pm_threadabort)
			continue;		/* p[4] stays -12: the main thread runs it */
		memcpy(in, rows, b->n * SV_TAS_ROLL_IN * sizeof(float));
		memcpy(b->out + (size_t)k * b->n * SV_TAS_ROLL_OUT, outs, (size_t)(ran > 0 ? ran : 0) * SV_TAS_ROLL_OUT * sizeof(float));
		p[4] = ran;
	}
	PM_ThreadBind(NULL, NULL);
	PMSrc_ThreadState(NULL);
	PM_ThreadState(NULL);
	World_ThreadState(NULL);
	BIH_ThreadState(NULL);
	BZ_Free(rows); BZ_Free(outs); BZ_Free(pm); BZ_Free(mv); BZ_Free(pms); BZ_Free(pmt); BZ_Free(wbs); BZ_Free(bs);
	return 0;
}

int SV_TASRolloutBatch(edict_t *e, float *in, int n, float *out, int count, float *params, float aimgain, const float *aimn)
{
	extern FTE_TLS float frametime;
	static playermove_t saved;
	movevars_t savedmv;
	vec3_t savedmins, savedmaxs;
	float savedframetime;
	sv_tasroll_batch_t b;
	sv_tasroll_thread_t th[16];
	void *thread[16];
	playermove_t *pm0;
	client_t *cl;
	int k, t, msec, r, done = 0, nthreads;
	qboolean threads;
	if (n < 1 || n > SV_TAS_ROLL_MAX || count < 1 || count > SV_TAS_BATCH_MAX || !in || !out || !params)
		return -1;
	r = SV_TASRollCheck(e, &cl, &msec);
	if (r)
		return r;
	for (k = 0; k < count; ++k)
		if (SV_TASRollRows(in + (size_t)k * n * SV_TAS_ROLL_IN, n, (aimgain > 0) ? params : NULL))
			return -10;
	pm0 = BZ_Malloc(sizeof(*pm0));
	if (!pm0)
		return -1;
	for (k = 0; k < count; ++k)
		params[k * SV_TAS_BATCH_PARAMS + 4] = -12;

	memcpy(&saved, &pmove, sizeof(pmove));
	savedmv = movevars;
	VectorCopy(pmove_mins, savedmins);
	VectorCopy(pmove_maxs, savedmaxs);
	savedframetime = frametime;

	b.startrep = SV_TASRollSetup(e, cl);
	memcpy(pm0, &pmove, sizeof(pmove));
	b.e = e; b.in = in; b.out = out; b.params = params; b.n = n; b.count = count; b.msec = msec;
	b.aimgain = aimgain; b.aimn = aimn; b.pm0 = pm0; b.mv0 = movevars;
	nthreads = bound(1, sv_tas_threads.ival, (int)countof(thread));
	if (nthreads > count)
		nthreads = count;
	threads = nthreads > 1;
#if !defined(MULTITHREAD) || !defined(FTE_TLS_OK)
	threads = false;		/* no threads, or no per-thread pointers to give them */
#endif
#ifdef USEAREAGRID
	if (sv.world.portallist.l.next && sv.world.portallist.l.next != &sv.world.portallist.l)
		threads = false;		/* a portal's trace runs QC (camera_transform) */
#else
	threads = false;			/* the worker queries are the area grid's */
#endif
	if (sv.world.worldmodel && (sv.world.worldmodel->fromgame == fg_quake2 || sv.world.worldmodel->fromgame == fg_quake3))
		threads = false;		/* CM_NativeTrace keeps its trace in statics */
	if (threads)
	{
		PM_PrepareThreads();
		pms_snapshot = PMSrc_MainState();
		b.nthreads = nthreads;
		for (t = 0; t < nthreads; ++t)
		{
			th[t].b = &b;
			th[t].t = t;
			thread[t] = Sys_CreateThread(va("tasroll_%i", t), SV_TASRollWorker, &th[t], THREADP_NORMAL, 1024 * 1024);
		}
		for (t = 0; t < nthreads; ++t)
			if (thread[t])
				Sys_WaitOnThread(thread[t]);
	}
	for (k = 0; k < count; ++k)
	{
		float *p = params + k * SV_TAS_BATCH_PARAMS;
		if (p[4] != -12)
		{
			++done;
			continue;
		}
		memcpy(&pmove, pm0, sizeof(pmove));	/* a worker left it, or none ran: as tas_rollout runs it */
		movevars = b.mv0;
		pm_threadabort = false;
		p[4] = SV_TASRollRun(e, in + (size_t)k * n * SV_TAS_ROLL_IN, n, out + (size_t)k * n * SV_TAS_ROLL_OUT,
			(aimgain > 0) ? p : NULL, aimgain, aimn, p[3], msec, b.startrep);
	}

	memcpy(&pmove, &saved, sizeof(pmove));
	movevars = savedmv;
	VectorCopy(savedmins, pmove_mins);
	VectorCopy(savedmaxs, pmove_maxs);
	frametime = savedframetime;
	BZ_Free(pm0);
	return done;
}
