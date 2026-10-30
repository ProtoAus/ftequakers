/* P1129: derived working provenance, never entity/network/save/record fields.
 * Only explicit VM writes author a property. Native cache selection preserves
 * its exact source until selection or an explicit same-value reauthor. */
static func_t bankgravity_callback;
static qboolean bankgravity_callback_known;

static qboolean SV_BankGravityValid(client_t *cl, const sv_bankgravity_source_t *s)
{
	return cl->userid > 0 && s->userid == cl->userid && s->spawncount == svs.spawncount;
}

void SV_BankGravityClear(void)
{
	int i;
	bankgravity_callback = 0;
	bankgravity_callback_known = false;
	for (i = 0; i < sv.allocated_client_slots; ++i)
	{
		memset(&svs.clients[i].gravitybank_property, 0, sizeof(svs.clients[i].gravitybank_property));
		memset(&svs.clients[i].gravitybank_cached, 0, sizeof(svs.clients[i].gravitybank_cached));
	}
}

void SV_BankGravityBind(edict_t *body, int player, int attempt, float bad)
{
	extern cvar_t sv_gravity;
	client_t *cl;
	sv_bankgravity_source_t *s;
	float selected;
	int slot;
	if (svs.gametype != GT_PROGS || !svprogfuncs || !body) return;
	slot = body->entnum - 1;
	if (slot < 0 || slot >= sv.allocated_client_slots) return;
	cl = &svs.clients[slot];
	if (cl->edict != body) return;
	s = &cl->gravitybank_property;
	s->player = player; s->attempt = attempt; s->bad = bad;
	s->userid = cl->userid; s->spawncount = svs.spawncount;
	s->value = body->xv->gravity;
	selected = s->value * sv_gravity.value;
	if (progstype != PROG_QW && !selected) selected = sv_gravity.value;
	// Explicit independent reauthoring supports an already identical cache.
	// A different current property must never relabel the old selected value.
	if (cl->entgravity == selected)
	{
		cl->gravitybank_cached = *s;
		cl->gravitybank_cached.value = cl->entgravity;
	}
}

void SV_BankGravitySelect(client_t *cl)
{
	if (svs.gametype != GT_PROGS || !cl->edict ||
		!SV_BankGravityValid(cl, &cl->gravitybank_property) ||
		cl->gravitybank_property.value != cl->edict->xv->gravity)
	{
		// An unbound native/restore write cannot resurrect an old property tag.
		memset(&cl->gravitybank_property, 0, sizeof(cl->gravitybank_property));
		memset(&cl->gravitybank_cached, 0, sizeof(cl->gravitybank_cached));
		return;
	}
	cl->gravitybank_cached = cl->gravitybank_property;
	cl->gravitybank_cached.value = cl->entgravity;
}

static qboolean SV_BankGravityEnabled(void)
{
	if (svs.gametype != GT_PROGS || !svprogfuncs) return false;
	if (!bankgravity_callback_known)
	{
		bankgravity_callback = PR_FindFunction(svprogfuncs, "SV_BankGravityUse", PR_ANY);
		bankgravity_callback_known = true;
	}
	return bankgravity_callback != 0;
}

static void SV_BankGravityUsed(client_t *cl)
{
	globalvars_t *pr_globals;
	const sv_bankgravity_source_t *s = &cl->gravitybank_cached;
	int saved[15], oldargc, oldself;
	pvec_t oldtime;
	float selected;
	qboolean known;
	if (!SV_BankGravityEnabled() || !cl->edict) return;
	pr_globals = PR_globals(svprogfuncs, PR_CURRENT);
	if (!pr_globals) return;
	selected = cl->entgravity / movevars.gravity;
	known = SV_BankGravityValid(cl, s) && s->value == cl->entgravity &&
		selected == movevars.entgravity;
	// Preserve native caller registers; this is a derived policy callback only.
	memcpy(saved, &G_INT(OFS_RETURN), sizeof(saved));
	oldargc = svprogfuncs->callargc;
	oldself = pr_global_struct->self;
	oldtime = pr_global_struct->time;
	G_INT(OFS_PARM0) = EDICT_TO_PROG(svprogfuncs, cl->edict);
	G_INT(OFS_PARM1) = known ? s->player : 0;
	G_INT(OFS_PARM2) = known ? s->attempt : -1;
	G_FLOAT(OFS_PARM3) = known ? s->bad : 0;
	pr_global_struct->self = EDICT_TO_PROG(svprogfuncs, cl->edict);
	pr_global_struct->time = sv.time;
	svprogfuncs->callargc = 4;
	PR_ExecuteProgram(svprogfuncs, bankgravity_callback);
	svprogfuncs->callargc = oldargc;
	pr_global_struct->self = oldself;
	pr_global_struct->time = oldtime;
	memcpy(&G_INT(OFS_RETURN), saved, sizeof(saved));
}
