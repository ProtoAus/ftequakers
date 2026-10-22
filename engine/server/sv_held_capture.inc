/* Namespace-bound source collector. The builtin/producer gates stay OFF until
 * original-byte validation and checked publication are complete. No authority
 * comes from a client flag, an imported ledger, or this buffer's sealed bit. */
static int Held_CaptureFinite(float value)
{
	uint32_t bits;
	memcpy(&bits,&value,sizeof(bits));
	return (bits&0x7f800000u) != 0x7f800000u;
}
static int Held_CaptureTime(held_session_t *s, float *time, float *frametime)
{
	eval_t *value;
	etype_t type;
	value = PR_FindGlobal(s->vm,"time",PR_ANY,&type);
	if (!value || (type&0xfff) != ev_float || !Held_CaptureFinite(value->_float)) return 0;
	*time = value->_float;
	value = PR_FindGlobal(s->vm,"frametime",PR_ANY,&type);
	if (!value || (type&0xfff) != ev_float || !Held_CaptureFinite(value->_float) ||
		value->_float < 0 || value->_float > 1) return 0;
	*frametime = value->_float;
	return 1;
}
static int Held_CaptureContextEqual(const held_record_t *a, const held_record_t *b)
{
	unsigned int i;
	if (a->family != HELD_RECORD_HCTX || b->family != HELD_RECORD_HCTX) return 0;
	/* Native builders zero their records; prefix is ledger-owned, not context. */
	for (i = 4; i < held_schemas[HELD_RECORD_HCTX].count; ++i)
		if (!a->assigned[i] || !b->assigned[i] || memcmp(&a->values[i],&b->values[i],sizeof(a->values[i])))
			return 0;
	return 1;
}
static int Held_CaptureOwner(held_session_t *s)
{
	if (!s || s->failed) return 0;
	if (!SV_HeldSessionCheck(s->client,s->generation) || !s->capture.started ||
		s->capture.progs_epoch != SV_HeldProgsEpoch())
	{
		if (!s->failed) Held_SessionFail(s,2);
		return 0;
	}
	return 1;
}
static int Held_CaptureContext(held_session_t *s, const movevars_t *source)
{
	held_record_t now;
	int equal = 0;
	if (!Held_CaptureOwner(s)) return 0;
	if (s->capture.terminal)
	{
		/* The terminal pose already acted at its exact QC boundary. Subsequent
		 * native scope exits must still bind the same loaded rules/artifacts. */
		if (SV_HeldContextBuildNative(source,&now) &&
			SV_HeldRecordI(&now,"ctx_geomtype",(int32_t)s->entity->xv->geomtype))
			equal = Held_CaptureContextEqual(&s->capture.context,&now);
	}
	else if (SV_HeldContextCapture(s->client,source,&now) && SV_HeldAirCell(s->client))
		equal = Held_CaptureContextEqual(&s->capture.context,&now);
	Held_SessionWipe(&now,sizeof(now));
	if (!equal) Held_SessionFail(s,1);
	return equal;
}
static int Held_CaptureAppend(held_session_t *s, held_record_t *r, uint64_t message, uint64_t execution)
{
	int appended;
	if (!Held_CaptureOwner(s)) {Held_SessionWipe(r,sizeof(*r)); return 0;}
	appended = SV_HeldLedgerAppend(&s->ledger,r,message,execution);
	Held_SessionWipe(r,sizeof(*r));
	if (!appended) Held_SessionFail(s,2);
	return appended;
}
static int Held_CaptureScope(held_session_t *s, unsigned int kind, unsigned int phase, float time, float frametime)
{
	held_record_t r;
	uint64_t message = s->client->held_coverage.message_open ? s->client->held_coverage.messages:0;
	SV_HeldRecordBegin(&r,HELD_RECORD_HSCOPE);
	SV_HeldRecordU(&r,"kind",kind); SV_HeldRecordU(&r,"phase",phase);
	SV_HeldRecordF(&r,"time",time); SV_HeldRecordF(&r,"frametime",frametime);
	return Held_CaptureAppend(s,&r,message,0);
}
static int Held_CaptureCommandBegin(held_session_t *s, unsigned int kind)
{
	if (!s->capture.command_open || (kind != 3 && kind != 4)) return 0;
	if (s->capture.command_written) return s->capture.command_kind == kind;
	if (!Held_CaptureScope(s,kind,1,s->capture.command_time,s->capture.command_frametime)) return 0;
	s->capture.command_kind = kind; s->capture.command_written = 1;
	return 1;
}
static void Held_CaptureTrySeal(held_session_t *s)
{
	held_record_t r;
	held_coverage_t *c = &s->client->held_coverage;
	if (!s->capture.close_requested || s->capture.sealed || s->failed || !SV_HeldCoverageClosed(c)) return;
	if (!Held_CaptureOwner(s) || !s->capture.terminal || s->capture.pending_move ||
		s->capture.command_open || !SV_HeldFileClosed(s->source))
	{
		if (!s->failed) Held_SessionFail(s,2);
		return;
	}
	SV_HeldRecordBegin(&r,HELD_RECORD_HSEAL);
#define HC_SEAL(n,v) SV_HeldRecordL(&r,n,v)
	HC_SEAL("first_execution",1); HC_SEAL("last_execution",c->executions); HC_SEAL("executions",c->executions);
	HC_SEAL("pre_enters",c->pre_enters); HC_SEAL("pre_exits",c->pre_exits); HC_SEAL("movers",c->movers);
	HC_SEAL("completions",c->completions); HC_SEAL("post_enters",c->post_enters); HC_SEAL("post_exits",c->post_exits);
	HC_SEAL("messages",c->messages); HC_SEAL("commands",c->commands); HC_SEAL("scene_fences",c->scene_fences);
	HC_SEAL("control_events",c->controls); HC_SEAL("effect_dispatches",c->effects);
#undef HC_SEAL
	SV_HeldRecordU(&r,"qualification",1);
	if (Held_CaptureAppend(s,&r,0,0))
	{
		s->capture.census = *c; /* Exact independently closed native window. */
		s->capture.sealed = 1; c->active = 0;
	}
	/* STILL NO readiness/publication. Closed original-byte validator and a
	 * native checked non-served publisher must independently consume this. */
}
static void Held_CaptureBeforeBoundary(client_t *client, held_boundary_t boundary)
{
	held_session_t *s = Held_SessionFind(client);
	held_record_t context;
	if (!s || s->failed || s->capture.sealed || s->capture.started) return;
	if (!SV_HeldSessionCheck(client,s->generation)) return;
	if (!s->capture.started)
	{
		if (boundary != HELD_MESSAGE_ENTER)
		{
			/* A binding born inside PreThink or before another same-message
			 * execution cannot move the horizon past that unobserved physics. */
			if (boundary == HELD_PIECE_BEGIN || boundary == HELD_PRE_ENTER ||
				boundary == HELD_MOVER_ENTER || boundary == HELD_EFFECT) Held_SessionFail(s,1);
			return;
		}
		if (!SV_HeldAirCell(client) || !SV_HeldMoveVarsCapture(client,&s->capture.source_vars) ||
			!SV_HeldContextCapture(client,&s->capture.source_vars,&s->capture.context) ||
			!SV_HeldLedgerBegin(&s->ledger,s->generation) || !SV_HeldCoverageBegin(&client->held_coverage))
			Held_SessionFail(s,1);
		else
		{
			s->capture.started = 1; s->capture.progs_epoch = SV_HeldProgsEpoch(); s->capture.started_time = sv.time;
			context = s->capture.context;
			Held_CaptureAppend(s,&context,0,0);
		}
		return;
	}
	/* All active-record checks happen AFTER the independent native edge. */
}
static void Held_CaptureAfterBoundary(client_t *client, held_boundary_t boundary)
{
	held_session_t *s = Held_SessionFind(client);
	held_coverage_t *c = &client->held_coverage;
	held_record_t r;
	held_clock_t clock;
	held_clock_context_t clock_context;
	movevars_t initial_vars;
	float time, frametime, owned, frozen;
	unsigned int kind;
	uint64_t message;
	if (!s || s->failed || !s->capture.started || s->capture.sealed) return;
	if (c->failed) {Held_SessionFail(s,c->failed); return;}
	if (!Held_CaptureContext(s,boundary == HELD_MOVER_ENTER ? &movevars:&s->capture.source_vars)) return;
	if (!Held_CaptureOwner(s) || !Held_CaptureTime(s,&time,&frametime))
	{
		if (!s->failed) Held_SessionFail(s,1);
		return;
	}
	if (sv.time < s->capture.started_time || sv.time-s->capture.started_time > 60 ||
		(s->capture.guard && (sv.time < s->capture.protected_time || sv.time-s->capture.protected_time > 10)))
		{Held_SessionFail(s,1); return;}
	message = c->message_open ? c->messages:0;
	switch (boundary)
	{
	case HELD_MESSAGE_ENTER:
		Held_CaptureScope(s,1,1,time,frametime);
		break;
	case HELD_PIECE_BEGIN:
		if (!s->capture.initial_written)
		{
			/* Unique actual first execution, not message arrival or a later
			 * successful retry. Prefix commands/frames already have coverage. */
			memset(&clock,0,sizeof(clock)); memset(&clock_context,0,sizeof(clock_context));
			memset(&initial_vars,0,sizeof(initial_vars));
			kind = c->executions == 1 && !s->selected &&
				SV_HeldInitialCapture(client,&r,&clock,&clock_context,&initial_vars) &&
				Held_CaptureContext(s,&initial_vars) && Held_CaptureAppend(s,&r,c->messages,c->executions);
			Held_SessionWipe(&r,sizeof(r)); Held_SessionWipe(&clock,sizeof(clock));
			Held_SessionWipe(&clock_context,sizeof(clock_context)); Held_SessionWipe(&initial_vars,sizeof(initial_vars));
			if (!kind) {if (!s->failed) Held_SessionFail(s,1); return;}
			s->capture.initial_written = 1;
		}
		break;
	case HELD_MESSAGE_EXIT:
		/* The independent native edge just closed it; keep its actual message
		 * identity for these final two records rather than a future packet. */
		SV_HeldRecordBegin(&r,HELD_RECORD_HPEND); SV_HeldRecordL(&r,"last_execution",c->executions);
		if (!Held_CaptureAppend(s,&r,c->messages,0)) return;
		SV_HeldRecordBegin(&r,HELD_RECORD_HSCOPE); SV_HeldRecordU(&r,"kind",1); SV_HeldRecordU(&r,"phase",2);
		SV_HeldRecordF(&r,"time",time); SV_HeldRecordF(&r,"frametime",frametime);
		if (!Held_CaptureAppend(s,&r,c->messages,0)) return;
		Held_CaptureTrySeal(s);
		break;
	case HELD_FRAME_ENTER: case HELD_FRAME_EXIT:
		if (!Held_CaptureScope(s,2,boundary == HELD_FRAME_ENTER ? 1:2,time,frametime)) return;
		if (boundary == HELD_FRAME_EXIT) Held_CaptureTrySeal(s);
		break;
	case HELD_COMMAND_ENTER:
		if (s->capture.command_open) {Held_SessionFail(s,2); return;}
		s->capture.command_open = 1; s->capture.command_time = time; s->capture.command_frametime = frametime;
		s->capture.command_controls = c->controls;
		break;
	case HELD_COMMAND_EXIT:
		if (!s->capture.command_open || !s->capture.command_written ||
			(s->capture.command_kind == 3 && c->controls != s->capture.command_controls+1))
			{Held_SessionFail(s,1); return;}
		kind = s->capture.command_kind;
		if (!Held_CaptureScope(s,kind,2,time,frametime)) return;
		s->capture.command_open = s->capture.command_written = s->capture.command_kind = 0;
		break;
	case HELD_PRE_ENTER: case HELD_PRE_EXIT:
		SV_HeldRecordBegin(&r,HELD_RECORD_HPRE); SV_HeldRecordU(&r,"phase",boundary == HELD_PRE_ENTER ? 1:2);
		SV_HeldRecordF(&r,"time",time); SV_HeldRecordF(&r,"frametime",frametime);
		Held_CaptureAppend(s,&r,message,c->executions);
		break;
	case HELD_MOVER_ENTER:
		if (s->capture.pending_move || !s->capture.initial_written || s->capture.terminal ||
			!Held_SessionScalar(s->entity,"rec_wt_hold",&owned) || (owned != 0 && owned != 1) ||
			!Held_SessionScalar(s->entity,"run_t_frzon",&frozen) || (frozen != 0 && frozen != 1) ||
			pmove.pm_type != (owned ? PM_NONE:PM_NORMAL) || !Held_CaptureFinite(pmove.cmd.msec) ||
			pmove.cmd.msec < 0 || pmove.cmd.msec > 50 || sv.gamespeed != 1)
			{Held_SessionFail(s,1); return;}
		kind = owned ? 3:(frozen ? 2:1);
		SV_HeldRecordBegin(&r,HELD_RECORD_HMOVE);
		SV_HeldRecordU(&r,"context",1); SV_HeldRecordU(&r,"pmtype",pmove.pm_type);
		SV_HeldRecordU(&r,"ownerkind",kind); SV_HeldRecordU(&r,"ownerindex",owned ? 0:s->capture.aliases);
		SV_HeldRecordF(&r,"effective_msec",pmove.cmd.msec); SV_HeldRecordF(&r,"gamespeed",1);
		if (!Held_CaptureAppend(s,&r,message,c->executions)) return;
		if (!owned) ++s->capture.aliases;
		if (owned || s->capture.guard) s->capture.protected_piece[c->executions-1] = 1;
		if (s->capture.guard && s->capture.thawed && kind == 1) s->capture.guard_close = 1;
		s->capture.pending_move = 1;
		break;
	case HELD_POST_ENTER: case HELD_POST_EXIT:
		if (s->capture.pending_move) {Held_SessionFail(s,2); return;}
		SV_HeldRecordBegin(&r,HELD_RECORD_HPOST); SV_HeldRecordU(&r,"phase",boundary == HELD_POST_ENTER ? 1:2);
		SV_HeldRecordF(&r,"time",time); SV_HeldRecordF(&r,"frametime",frametime);
		Held_CaptureAppend(s,&r,message,0);
		break;
	default:
		break; /* Piece begin/completion counters are independent native facts. */
	}
}
void SV_HeldSourceBoundary(client_t *client, held_boundary_t boundary)
{
	held_session_t *s;
	eval_t *mode;
	const char *generation;
	float owned, selected;
	if (!client) return;
	if (boundary == HELD_PRE_ENTER && svprogfuncs && client->edict)
	{
		s = Held_SessionFind(client);
		generation = Held_SessionString(client->edict,"rec_hm_generation");
		if (generation && *generation && Held_SessionScalar(client->edict,"rec_wt_hold",&owned) && owned == 1 &&
			Held_SessionScalar(client->edict,"rec_hm_mode",&selected) && selected == 0)
		{
			if (s && !s->selected && Held_SessionOwnerIdentity(s)) SV_HeldSessionSelect(client,s->generation);
			else
			{
				/* Lost/mismatched native authority cannot grant any publication.
				 * It can ONLY reduce collection, before the trusted QC formatter. */
				mode = svprogfuncs->GetEdictFieldValue(svprogfuncs,client->edict,"rec_hm_mode",ev_float,NULL);
				if (mode) mode->_float = 12;
				if (s) Held_SessionFail(s,2);
			}
		}
	}
	Held_CaptureBeforeBoundary(client,boundary);
	/* ALWAYS the original independent census. Never infer it from the ledger. */
	SV_HeldCoverageBoundary(&client->held_coverage,boundary);
	Held_CaptureAfterBoundary(client,boundary);
}
void SV_HeldSourceCommand(client_t *client, const char *command, qboolean entering)
{
	held_session_t *s = Held_SessionFind(client);
	float flags;
	if (!s || s->failed || !s->capture.started || s->capture.sealed || !command ||
		!s->capture.command_open) return;
	if (strncmp(command,"rec_nack ",9) || strlen(command+9) != 32 || strcmp(command+9,s->nonce)) return;
	/* Match actual accepted client text to the namespace's existing nonce,
	 * never a QC caller-supplied safe token. No text/nonce copy or hash. */
	if (!Held_CaptureOwner(s) || !SV_HeldFileOwnsStream(Held_SessionStream(s),s->source)) return;
	if (entering) return; /* Preserve actual command enter/exit around execution. */
	if (!Held_SessionScalar(s->entity,"run_t_flags",&flags) || flags < 0 || flags > 1048575 ||
		(unsigned int)flags != flags || !((unsigned int)flags&131072u) || !Held_CaptureCommandBegin(s,4))
		Held_SessionFail(s,1);
}
void SV_HeldSessionMoved(client_t *client)
{
	held_session_t *s = Held_SessionFind(client);
	held_coverage_t *c;
	held_record_t r;
	float rate;
	if (!s || s->failed || !s->capture.started || s->capture.sealed) return;
	c = &client->held_coverage; rate = s->capture.source_vars.ticrate;
	if (!Held_CaptureContext(s,&movevars) || !s->capture.pending_move || c->completions != c->executions ||
		client->movetickcount > 16777216 || pmove.ticksrun < 0 || pmove.ticksrun > 65536 ||
		pmove.numtouch || pmove.portalcrossings || pmove.onground || pmove.waterlevel || pmove.onladder ||
		!Held_CaptureFinite(pmove.tickused) || pmove.tickused != rate ||
		!Held_CaptureFinite(client->pmsrc.msec_carry) || client->pmsrc.msec_carry < 0 ||
		client->pmsrc.msec_carry >= rate)
	{
		if (!s->failed) Held_SessionFail(s,2);
		return;
	}
	SV_HeldRecordBegin(&r,HELD_RECORD_HDONE);
	SV_HeldRecordL(&r,"raw_moveticks",client->movetickcount); SV_HeldRecordU(&r,"ticksrun",pmove.ticksrun);
	SV_HeldRecordF(&r,"carry",client->pmsrc.msec_carry);
	if (!Held_CaptureAppend(s,&r,c->messages,c->executions)) return;
	s->capture.pending_move = 0;
	if (s->capture.guard_close) s->capture.guard = s->capture.guard_close = 0;
	if (!s->selected) s->initialized = 1; /* A real preceding ordinary step ACTED. */
}
int SV_HeldSessionEvent(client_t *client, const char *generation, const char *site)
{
	held_session_t *s = Held_SessionFind(client);
	held_coverage_t *c;
	held_record_t r;
	held_control_t control;
	float value;
	unsigned int boundary = 0, maint = 0;
	int result = 0;
	if (!s || !generation || !site || strcmp(generation,s->generation) || !Held_CaptureOwner(s)) return 0;
	c = &client->held_coverage;
	if (!strcmp(site,"terminal"))
	{
		/* Existing QC event is AFTER final taints/tag, BEFORE RecClose clears
		 * RECORDING. Never substitute a later close/lastorg/flags snapshot. */
		if (!s->selected || !s->initialized || s->capture.terminal || s->capture.pending_move ||
			!c->post_open || !c->message_open || !SV_HeldObservationCapture(client,4,&r) ||
			!Held_CaptureAppend(s,&r,c->messages,0) || !SV_HeldClockBoundaryCapture(client,HELD_RECORD_HTERMINAL,&r) ||
			!Held_CaptureAppend(s,&r,c->messages,0)) goto unsupported;
		s->capture.terminal = 1;
		return 1;
	}
	if (!s->selected || !s->initialized || !Held_CaptureContext(s,&s->capture.source_vars)) goto unsupported;
	if (!strcmp(site,"entry"))
	{
		if (!Held_CaptureCommandBegin(s,3) || !SV_HeldObservationCapture(client,1,&r) ||
			!Held_CaptureAppend(s,&r,c->messages,0)) goto unsupported;
		control = HELD_PIN_ENTRY;
		if (!Held_SessionScalar(s->entity,"rec_wt_holdmove",&value) || value != MOVETYPE_WALK) goto unsupported;
		SV_HeldRecordBegin(&r,HELD_RECORD_HENTRY); SV_HeldRecordI(&r,"saved_movetype",(int32_t)value);
	}
	else if (!strcmp(site,"release"))
	{
		if (!Held_CaptureCommandBegin(s,3)) goto unsupported;
		control = HELD_PIN_RELEASE; boundary = 2;
		if (s->entity->v->movetype != MOVETYPE_WALK) goto unsupported;
		SV_HeldRecordBegin(&r,HELD_RECORD_HRELEASE); SV_HeldRecordI(&r,"restored_movetype",MOVETYPE_WALK);
	}
	else if (!strcmp(site,"maint-pre") || !strcmp(site,"maint-post") || !strcmp(site,"maint-tail"))
	{
		maint = !strcmp(site,"maint-pre") ? 1:(!strcmp(site,"maint-post") ? 2:3);
		control = maint == 1 ? HELD_MAINT_PRE:(maint == 2 ? HELD_MAINT_POST:HELD_MAINT_TAIL);
		SV_HeldRecordBegin(&r,HELD_RECORD_HMAINT); SV_HeldRecordU(&r,"site",maint);
	}
	else if (!strcmp(site,"freeze") || !strcmp(site,"thaw"))
	{
		control = !strcmp(site,"freeze") ? HELD_FREEZE:HELD_THAW;
		if (!SV_HeldClockBoundaryCapture(client,control == HELD_FREEZE ? HELD_RECORD_HFREEZE:HELD_RECORD_HTHAW,&r)) goto unsupported;
		if (control == HELD_THAW) boundary = 3;
	}
	else goto unsupported;
	/* This source-native control edge precedes writing its metadata. */
	if (!SV_HeldCoverageControl(c,control) || !Held_CaptureAppend(s,&r,c->messages,maint == 1 ? c->executions:0)) goto unsupported;
	if (boundary && (!SV_HeldObservationCapture(client,boundary,&r) || !Held_CaptureAppend(s,&r,c->messages,0))) goto unsupported;
	if (control == HELD_PIN_ENTRY)
	{
		if (!c->executions || c->executions > HELD_MAX_PIECES) goto unsupported;
		s->capture.protected_piece[c->executions-1] = 1; /* Last actual ordinary ingress. */
		s->capture.guard = 1; s->capture.protected_time = sv.time;
	}
	if (control == HELD_THAW) s->capture.thawed = 1;
	result = 1;
	Held_SessionWipe(&r,sizeof(r));
	return result;
unsupported:
	Held_SessionWipe(&r,sizeof(r));
	if (!s->failed) Held_SessionFail(s,1);
	return 0;
}
