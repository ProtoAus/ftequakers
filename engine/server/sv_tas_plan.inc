/* P1074: native-owned next-command authoring, only after a fresh TAS grant. */
static int SV_TASPlanActive(client_t *cl)
{
	tas_rewind_pool_t *p = SV_TASInputPool(cl);
	return p && p->planmode != SV_PLAN_LIVE;
}

/* The caller has just prepared THIS client's effective movevars. Choose before
 * QC/journal/physics so all consumers see the same synthetic command. Future
 * QC/geometry/collision changes are not part of this observed-air objective. */
static int SV_TASPlanOverlay(client_t *cl, edict_t *e, usercmd_t *cmd)
{
	tas_rewind_pool_t *p = SV_TASInputPool(cl);
	sv_tas_input_t in;
	float friction, wishspeed, cap, tick, accel;
	unsigned short yaw;
	int side, buttons;
	if (!p || !p->planmode || !p->planinput.valid || p->tape.state == SV_TAPE_ACTIVE) return 0;
	in = p->planinput;
	p->planair = 0;
	if (p->planmode >= SV_PLAN_LEFT)
	{
		in.angle[YAW] = (short)p->planyaw; // ground/launch follows the last authored head
		friction = e->v->velocity[2] > 0 && e->v->velocity[2] <= 140 ? 0.25f : 1.0f;
		/* Use the preset at contacts, water/ladders/slide limits and state changes.
		 * Never advertise an air optimum for a ground or collision operation. */
		if (!e->v->fixangle && !((int)e->v->flags & (FL_ONGROUND | FL_WATERJUMP)) && e->v->waterlevel < 2 &&
			cl->pmsrc.waterjumpremaining == 0 && !cl->pmsrc.srcladder && cl->pmsrc.surfacefriction == friction &&
			!SV_TASInputField(e, "run_momlimit") && !SV_TASInputField(e, "run_momslide") &&
			e->xv->gravitydir[0] == 0 && e->xv->gravitydir[1] == 0 &&
			(e->xv->gravitydir[2] == 0 || e->xv->gravitydir[2] == -1))
		{
			buttons = (in.buttons & 4) ? BUTTON_SPEED : 0;
			wishspeed = PMSrc_MaxSpeedForInputs(PM_NORMAL, buttons, cl->pmsrc.stamina);
			if (wishspeed > 450) wishspeed = 450; // fixed full-key strength, not unbounded input
			cap = movevars.maxairspeed > 0 ? movevars.maxairspeed : 30;
			tick = movevars.ticrate > 0 ? movevars.ticrate : SV_TAS_TICK;
			side = p->planmode == SV_PLAN_LEFT ? -1 : 1;
			accel = movevars.airaccelerate * wishspeed * tick * friction;
			if (SV_TASStrafeUnclipped(e->v->velocity[0], e->v->velocity[1], e->v->velocity[2],
				accel, movevars.maxvelocity, sv_maxvelocity.value) &&
				SV_TASStrafeYaw(e->v->velocity[0], e->v->velocity[1], wishspeed,
				movevars.airaccelerate, tick, friction, cap, side, &yaw))
			{
				in.move[0] = in.move[2] = 0;
				in.move[1] = side * 450;
				in.angle[YAW] = (short)yaw;
				p->planair = 1;
			}
		}
	}
	SV_TASApplyInput(e, cmd, &in);
	SV_TASTapePublish(e, p);
	return 1;
}

static void SV_TASPlanComplete(client_t *cl, edict_t *e)
{
	tas_rewind_pool_t *p = SV_TASInputPool(cl);
	if (p && p->planmode >= SV_PLAN_LEFT && SV_TASStrafeFinite(e->v->v_angle[YAW]))
	{
		p->planyaw = (unsigned short)ANGLE2SHORT(e->v->v_angle[YAW]);
		p->planpostpending = 1;
	}
}

/* PostThink/new-missile callbacks can teleport after the per-command touches.
 * Refresh only an executed assist, never a newly configured unexecuted draft. */
static void SV_TASPlanPostComplete(client_t *cl, edict_t *e)
{
	tas_rewind_pool_t *p = SV_TASInputPool(cl);
	if (p && p->planpostpending)
	{
		SV_TASPlanComplete(cl, e);
		p->planpostpending = 0;
	}
}
