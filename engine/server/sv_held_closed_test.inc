/* Synthetic closed-byte control in the existing physical selftest only.
 * Never a source/grant/readiness authority; no persistent test recordings. */
typedef struct
{
	held_ledger_t ledger;
	held_coverage_t coverage;
	held_timeline_t timeline;
	held_zone_t zone;
	held_record_t context, initial, record;
	char bytes[65536], pins[8192], seed_state[8192];
	size_t size;
	unsigned int normal, frozen, samples, pmrows, perows, epoch, recorded_epoch, pcset, mode;
} held_closed_test_t;
static int Held_ClosedText(held_closed_test_t *f, const char *format, ...)
{
	va_list ap;
	size_t count, remaining = sizeof(f->bytes)-f->size;
	if (remaining < 2) return 0;
	va_start(ap,format);
	Q_vsnprintfz(f->bytes+f->size,remaining,format,ap);
	va_end(ap);
	count = strlen(f->bytes+f->size);
	if (count >= remaining-1) return 0;
	f->size += count;
	return 1;
}
static void Held_ClosedClock(held_record_t *r, const held_clock_t *clock)
{
	unsigned int i;
	float value;
	for (i = 0; i < countof(held_clock_fields); ++i)
	{
		memcpy(&value,(const char*)clock+held_clock_fields[i].offset,sizeof(value));
		SV_HeldRecordF(r,held_clock_fields[i].wire,value);
	}
}
static int Held_ClosedEvent(held_closed_test_t *f)
{
	held_record_t *r = &f->record;
	held_coverage_t *c = &f->coverage;
	const held_value_t *value;
	held_boundary_t boundary = HELD_EFFECT;
	held_control_t control = HELD_FREEZE;
	uint64_t message = c->message_open ? c->messages:0, execution = 0;
	unsigned int kind = 0, phase = 0;
	int is_boundary = 0, is_control = 0;
	usercmd_t ordinary = {0};
	float seed[5] = {-1,-1,0,0,0};
	if (r->family == HELD_RECORD_HSCOPE)
	{
		kind = SV_HeldRecordGet(r,"kind",'U')->u; phase = SV_HeldRecordGet(r,"phase",'U')->u;
		if (kind == 1)
		{
			boundary = phase == 1 ? HELD_MESSAGE_ENTER:HELD_MESSAGE_EXIT;
			if (phase == 1) message = c->messages+1;
		}
		else if (kind == 2) boundary = phase == 1 ? HELD_FRAME_ENTER:HELD_FRAME_EXIT;
		else boundary = phase == 1 ? HELD_COMMAND_ENTER:HELD_COMMAND_EXIT;
		is_boundary = 1;
	}
	else if (r->family == HELD_RECORD_HPRE)
	{
		phase = SV_HeldRecordGet(r,"phase",'U')->u;
		if (phase == 1 && !SV_HeldCoverageBoundary(c,HELD_PIECE_BEGIN)) return 0;
		execution = c->executions;
		boundary = phase == 1 ? HELD_PRE_ENTER:HELD_PRE_EXIT; is_boundary = 1;
	}
	else if (r->family == HELD_RECORD_HMOVE || r->family == HELD_RECORD_HDONE)
	{
		execution = c->executions;
		boundary = r->family == HELD_RECORD_HMOVE ? HELD_MOVER_ENTER:HELD_MOVER_EXIT; is_boundary = 1;
	}
	else if (r->family == HELD_RECORD_HPOST)
	{
		phase = SV_HeldRecordGet(r,"phase",'U')->u;
		boundary = phase == 1 ? HELD_POST_ENTER:HELD_POST_EXIT; is_boundary = 1;
	}
	else if (r->family == HELD_RECORD_HENTRY) {control = HELD_PIN_ENTRY; is_control = 1;}
	else if (r->family == HELD_RECORD_HRELEASE) {control = HELD_PIN_RELEASE; is_control = 1;}
	else if (r->family == HELD_RECORD_HFREEZE) {control = HELD_FREEZE; is_control = 1;}
	else if (r->family == HELD_RECORD_HTHAW) {control = HELD_THAW; is_control = 1;}
	else if (r->family == HELD_RECORD_HMAINT)
	{
		kind = SV_HeldRecordGet(r,"site",'U')->u;
		control = kind == 1 ? HELD_MAINT_PRE:(kind == 2 ? HELD_MAINT_POST:HELD_MAINT_TAIL); is_control = 1;
		if (kind == 1) execution = c->executions;
	}
	else if (r->family == HELD_RECORD_HINIT) execution = 1;
	if (is_boundary && !SV_HeldCoverageBoundary(c,boundary)) return 0;
	if (is_control && !SV_HeldCoverageControl(c,control)) return 0;
	if (!SV_HeldLedgerAppend(&f->ledger,r,message,execution)) return 0;
	if (r->family == HELD_RECORD_HINIT)
	{
		value = SV_HeldRecordGet(&f->context,"zone",'D');
		return value && SV_HeldZoneBegin(&f->zone,value->bytes,seed) &&
			SV_HeldTimelineBegin(&f->timeline,&f->context,r,sv.world.worldmodel,SV_HeldZoneStep,&f->zone);
	}
	if (!f->timeline.clock.initialized || r->family == HELD_RECORD_HSEAL) return 1;
	value = SV_HeldRecordGet(r,"ownerkind",'U');
	return SV_HeldTimelineEvent(&f->timeline,r,value && value->u != 3 ? &ordinary:NULL,0);
}
static int Held_ClosedScope(held_closed_test_t *f, unsigned int kind, unsigned int phase, float time, float frametime)
{
	Held_PhysicalTestRecord(&f->record,HELD_RECORD_HSCOPE);
	SV_HeldRecordU(&f->record,"kind",kind); SV_HeldRecordU(&f->record,"phase",phase);
	SV_HeldRecordF(&f->record,"time",time); SV_HeldRecordF(&f->record,"frametime",frametime);
	return Held_ClosedEvent(f);
}
static int Held_ClosedCallback(held_closed_test_t *f, held_record_family_t family, unsigned int phase, float time, float frametime)
{
	Held_PhysicalTestRecord(&f->record,family); SV_HeldRecordU(&f->record,"phase",phase);
	SV_HeldRecordF(&f->record,"time",time); SV_HeldRecordF(&f->record,"frametime",frametime);
	return Held_ClosedEvent(f);
}
static int Held_ClosedState(held_closed_test_t *f)
{
	float pins[SV_PMPIN_COUNT];
	unsigned int crc, aliases = f->normal+f->frozen;
	held_clock_t *clock = &f->timeline.clock.clock;
	if (!aliases || (clock->frzon && clock->freeze)) return 1;
	if (f->epoch != f->recorded_epoch)
	{
		SV_PMPinFill(pins);
		if (!SV_PMPinText(pins,f->pins,sizeof(f->pins)) ||
			!Held_ClosedText(f,"pm %d %u %s\n",(int)clock->packet,aliases-1,f->pins)) return 0;
		f->recorded_epoch = f->epoch; ++f->pmrows;
	}
	if (!f->pcset)
	{
		crc = SV_PhysentDigest();
		if (!Held_ClosedText(f,"pe %d %u %u\n",(int)clock->packet,aliases-1,crc)) return 0;
		f->pcset = 1; ++f->perows;
	}
	return 1;
}
static int Held_ClosedSample(held_closed_test_t *f)
{
	if (!Held_ClosedText(f,"%.4f %.2f %.2f %.2f %.2f %.2f %.2f 0.0 0.0 0 0 0 0 0 0.0000 0.0000 0.0000\n",
		SV_HeldClockSeconds(&f->timeline.clock),pmove.origin[0],pmove.origin[1],pmove.origin[2],
		pmove.velocity[0],pmove.velocity[1],pmove.velocity[2])) return 0;
	++f->samples;
	return 1;
}
static int Held_ClosedView(const held_ledger_t *ledger, const void **data, size_t *size)
{
	if (!ledger->initialized || !ledger->sealed || ledger->failed || !ledger->detail ||
		!ledger->size || ledger->size > HELD_MAX_DETAIL) return 0;
	*data = ledger->detail; *size = ledger->size;
	return 1;
}
static int Held_ClosedRehash(held_closed_test_t *f, size_t ordinary, held_record_family_t family, const char *field,
	const held_record_t *native)
{
	held_ledger_t forged = {0};
	held_record_t r;
	held_trajectory_t result;
	held_witness_t witness;
	const held_value_t *value;
	const char *cursor, *end, *newline;
	const void *view;
	size_t size, length, parsed_ordinary = 0;
	unsigned int mutated = 0;
	int refused = 0;
	if (!Held_ClosedView(&f->ledger,&view,&size) || !SV_HeldLedgerBegin(&forged,"abababababababababababababababab")) goto cleanup;
	cursor = view; end = cursor+size;
	while (cursor < end)
	{
		newline = memchr(cursor,'\n',end-cursor); if (!newline) goto cleanup;
		length = newline+1-cursor;
		if (!SV_HeldRecordParse(cursor,length,&r)) goto cleanup;
		if (!mutated && r.family == family)
		{
			value = SV_HeldRecordGet(&r,field,'F'); if (!value) goto cleanup;
			if (!SV_HeldRecordF(&r,field,nextafterf(value->f,INFINITY))) goto cleanup;
			mutated = 1;
		}
		if (!SV_HeldLedgerAppend(&forged,&r,SV_HeldRecordGet(&r,"message",'L')->l,
			SV_HeldRecordGet(&r,"execution",'L')->l)) goto cleanup;
		cursor = newline+1;
	}
	if (!mutated || !Held_ClosedView(&forged,&view,&size) || size > sizeof(f->bytes)-ordinary) goto cleanup;
	memcpy(f->bytes+ordinary,view,size);
	if (!SV_HeldRecordingParse(f->bytes,ordinary+size,&witness,&parsed_ordinary) ||
		witness.status != 3 || parsed_ordinary != ordinary) goto cleanup;
	refused = !SV_HeldTrajectoryRecording(f->bytes,ordinary+size,native,sv.world.worldmodel,&result);
cleanup:
	SV_HeldLedgerDiscard(&forged); Held_PhysicalWipe(&r,sizeof(r)); Held_PhysicalWipe(&result,sizeof(result));
	Held_PhysicalWipe(&witness,sizeof(witness));
	return refused;
}
static unsigned int Held_ClosedTest(const held_record_t *context, const held_record_t *initial,
	unsigned int *checks, unsigned int *acted)
{
	static const struct {const char *name; etype_t type; unsigned int words;} globals[] = {
		{"zone_count",ev_float,1}, {"zone_type[0]",ev_float,1}, {"zone_track[0]",ev_float,1},
		{"zone_first[0]",ev_float,1}, {"zone_npts[0]",ev_float,1}, {"zone_bottom[0]",ev_float,1},
		{"zone_top[0]",ev_float,1}, {"zone_centre[0]",ev_vector,3}, {"zone_radius[0]",ev_float,1},
		{"zone_zmins[0]",ev_vector,3}, {"zone_zmaxs[0]",ev_vector,3}, {"zone_pt[0]",ev_vector,12},
		{"zone_seg[0]",ev_float,1}, {"zone_cp[0]",ev_float,1}
	};
	held_closed_test_t *f = NULL;
	held_physical_t preview = {0};
	held_zone_t wrong_zone = {0};
	held_record_t r;
	held_clock_t clock = {0};
	pmsourcestate_t source_state;
	held_trajectory_t result;
	float *p[countof(globals)], saved[countof(globals)][12], pins[SV_PMPIN_COUNT];
	float rate, time, finalz, releasez = 0, seed[3], top, previous_top;
	float zone_seed[5] = {-1,-1,0,0,0};
	vec3_t final_origin, mins = {-16,-16,0}, maxs = {16,16,62};
	char *ordinary_change, *next_change, original_character, metadata_character;
	unsigned char original[32], restored[32];
	const void *view;
	size_t detail, ordinary = 0;
	unsigned int i, j, bound = 0, failures = 0, kind, index;
	int changed = 0, positive;
#define CT_TEST(x) do {++*checks; if (!(x)) {++failures; Con_Printf("held closed-byte control failed at %i\n",__LINE__);}} while (0)
#define CT_REQUIRE(x) do {unsigned int before = failures; CT_TEST(x); if (failures != before) goto cleanup;} while (0)
	CT_REQUIRE(SV_HeldZoneFingerprint(original));
	f = BZF_Malloc(sizeof(*f)); CT_REQUIRE(f != NULL); memset(f,0,sizeof(*f));
	rate = SV_HeldRecordGet(context,"ctx_ticrate",'F')->f;
	for (i = 0; i < 3; ++i)
	{
		char key[32]; Q_snprintfz(key,sizeof(key),"origin_%c",'x'+i);
		seed[i] = SV_HeldRecordGet(initial,key,'F')->f;
	}
	CT_REQUIRE(SV_HeldPhysicalBegin(&preview,context,initial,sv.world.worldmodel));
	PMSrc_SaveState(&source_state);
	CT_REQUIRE(SV_PMStateText(&source_state,f->seed_state,sizeof(f->seed_state)) != 0);
	Held_PhysicalWipe(&source_state,sizeof(source_state));
	for (i = 0; i < 4; ++i)
	{
		if (i == 1 || i == 2)
		{
			Held_PhysicalTestRecord(&r,i == 1 ? HELD_RECORD_HENTRY:HELD_RECORD_HRELEASE);
			SV_HeldRecordI(&r,i == 1 ? "saved_movetype":"restored_movetype",3);
			CT_REQUIRE(SV_HeldPhysicalControl(&preview,&r));
		}
		CT_REQUIRE(Held_PhysicalTestMove(&preview,&r,rate*1000.0f,acted));
		Held_PhysicalTestDone(&preview,&r); CT_REQUIRE(SV_HeldPhysicalDone(&preview,&r));
		if (i == 2)
		{
			releasez = pmove.origin[2]; Held_PhysicalTestRecord(&r,HELD_RECORD_HTHAW);
			CT_REQUIRE(SV_HeldPhysicalControl(&preview,&r));
		}
	}
	finalz = pmove.origin[2]; top = (releasez+finalz)*.5f;
	CT_REQUIRE(finalz < top && top < releasez);
	for (i = 0; i < countof(globals); ++i)
	{
		p[i] = Held_ZoneTestGlobal(globals[i].name,globals[i].type); CT_REQUIRE(p[i] != NULL);
		memcpy(saved[i],p[i],globals[i].words*sizeof(float)); ++bound;
	}
	changed = 1;
	*p[0] = 1; *p[1] = 1; *p[2] = 0; *p[3] = 0; *p[4] = 4;
	*p[5] = top-128; *p[6] = top; *p[12] = *p[13] = 0;
	for (i = 0; i < 3; ++i)
	{
		p[9][i] = i == 2 ? top-128:seed[i]-64;
		p[10][i] = i == 2 ? top:seed[i]+64;
		p[7][i] = (p[9][i]+p[10][i])*.5f;
	}
	*p[8] = sqrtf(3.0f*128.0f*128.0f)*.5f;
	for (i = 0; i < 4; ++i)
	{
		p[11][3*i] = seed[0]+(i == 1 || i == 2 ? 64:-64);
		p[11][3*i+1] = seed[1]+(i >= 2 ? 64:-64); p[11][3*i+2] = 0;
	}
	CT_REQUIRE(SV_HeldContextBuildNative(&movevars,&f->context));
	SV_PMPinFill(pins); CT_REQUIRE(SV_PMPinText(pins,f->pins,sizeof(f->pins)) != 0);
	SV_HeldPhysicalEnd(&preview);
	f->initial = *initial;
	clock.starttick = clock.lastmt = (float)SV_HeldRecordGet(initial,"raw_moveticks",'L')->l;
	clock.startsec = clock.lastsec = clock.starttick*rate;
	clock.startwall = clock.ckwall = 1; clock.state = 2; clock.counted = 1; clock.frame = 1+rate;
	VectorCopy(seed,clock.lastorg); Held_ClosedClock(&f->initial,&clock);
	CT_REQUIRE(SV_HeldLedgerBegin(&f->ledger,"abababababababababababababababab") && SV_HeldCoverageBegin(&f->coverage));
	f->record = f->context; CT_REQUIRE(Held_ClosedEvent(f));
	CT_REQUIRE(Held_ClosedText(f,HELD_NATIVE_REC_HEADER "clock counted\nholdclock 1\nviewclock 1\nmovetickrate %g\ninprof ok 63 1 0 0 1\npmpin %s\ninstart %d %d\nflags 1\nbegin\n",
		rate,f->pins,(int)clock.starttick,(int)clock.starttick));
	CT_REQUIRE(Held_ClosedText(f,"seed %.9g %.9g %.9g 100 0 0 0 %s\n",seed[0],seed[1],seed[2],f->seed_state));
	for (i = 0; i < 4; ++i)
	{
		time = 1+i*rate;
		CT_REQUIRE(Held_ClosedScope(f,2,1,time,rate) && Held_ClosedScope(f,2,2,time,rate));
		CT_REQUIRE(Held_ClosedScope(f,1,1,time,rate));
		if (i == 0)
		{
			f->record = f->initial; CT_REQUIRE(Held_ClosedEvent(f));
			CT_REQUIRE(Held_ClosedSample(f) && Held_ClosedText(f,"zseed evzone=-1 azone=-1 cpzone=-1 track=0 startseg=0 stagerun=0 twarp=%d\n",
				!!Cvar_VariableValue("run_teleport_warp")));
		}
		if (i == 1 || i == 2)
		{
			CT_REQUIRE(Held_ClosedScope(f,3,1,time,rate));
			if (i == 1)
			{
				Held_PhysicalTestObservation(&f->timeline.physical,&f->record); SV_HeldRecordU(&f->record,"boundary",1);
				CT_REQUIRE(Held_ClosedEvent(f));
			}
			Held_PhysicalTestRecord(&f->record,i == 1 ? HELD_RECORD_HENTRY:HELD_RECORD_HRELEASE);
			SV_HeldRecordI(&f->record,i == 1 ? "saved_movetype":"restored_movetype",3); CT_REQUIRE(Held_ClosedEvent(f));
			CT_REQUIRE(Held_ClosedText(f,"viewclock replay %d %.9g %.9g\n",i == 1,
				f->timeline.clock.clock.frame,SV_HeldClockViewSeconds(&f->timeline.clock)));
			if (i == 1)
				CT_REQUIRE(Held_ClosedText(f,"warp %d %u %u pin %.9g %.9g %.9g 0 0 0 0\n",(int)f->timeline.clock.clock.packet,
					(unsigned int)f->timeline.physical.raw_moveticks,f->normal+f->frozen-1,pmove.origin[0],pmove.origin[1],pmove.origin[2]));
			else
			{
				Held_PhysicalTestObservation(&f->timeline.physical,&f->record); SV_HeldRecordU(&f->record,"boundary",2);
				CT_REQUIRE(Held_ClosedEvent(f));
			}
			CT_REQUIRE(Held_ClosedScope(f,3,2,time,rate));
		}
		CT_REQUIRE(Held_ClosedCallback(f,HELD_RECORD_HPRE,1,time,rate));
		if (i == 1)
		{
			Held_PhysicalTestRecord(&f->record,HELD_RECORD_HMAINT); SV_HeldRecordU(&f->record,"site",1);
			CT_REQUIRE(Held_ClosedEvent(f));
		}
		CT_REQUIRE(Held_ClosedState(f) && Held_ClosedCallback(f,HELD_RECORD_HPRE,2,time,rate));
		kind = i == 1 ? 3:(i == 2 ? 2:1); index = i == 0 ? 0:(i == 1 ? 0:i-1);
		if (kind != 3)
		{
			if (!i)
				CT_REQUIRE(Held_ClosedText(f,"ride %d %u arm 0 0 0\n",(int)f->timeline.clock.clock.packet,
					(unsigned int)f->timeline.physical.raw_moveticks));
			CT_REQUIRE(Held_ClosedText(f,"%s %d %u %.9g 0 0 0 0.0000 0.0000 0.0000 0 0\n",kind == 1 ? "in":"fzin",
				(int)f->timeline.clock.clock.packet,(unsigned int)f->timeline.physical.raw_moveticks,pmove.msec_carry));
			if (kind == 1) ++f->normal; else ++f->frozen;
		}
		Held_PhysicalTestRecord(&f->record,HELD_RECORD_HMOVE);
		SV_HeldRecordU(&f->record,"context",1); SV_HeldRecordU(&f->record,"ownerkind",kind); SV_HeldRecordU(&f->record,"ownerindex",index);
		SV_HeldRecordU(&f->record,"pmtype",kind == 3 ? PM_NONE:PM_NORMAL);
		SV_HeldRecordF(&f->record,"effective_msec",rate*1000.0f); SV_HeldRecordF(&f->record,"gamespeed",1);
		CT_REQUIRE(Held_ClosedEvent(f)); ++*acted;
		if (f->mode != (unsigned int)pmove.pm_type) {f->mode = pmove.pm_type; ++f->epoch;}
		Held_PhysicalTestDone(&f->timeline.physical,&f->record); CT_REQUIRE(Held_ClosedEvent(f));
		CT_REQUIRE(Held_ClosedState(f) && Held_ClosedCallback(f,HELD_RECORD_HPOST,1,time,rate));
		if (i == 1)
		{
			Held_PhysicalTestRecord(&f->record,HELD_RECORD_HMAINT); SV_HeldRecordU(&f->record,"site",2); CT_REQUIRE(Held_ClosedEvent(f));
		}
		if (i == 1 || i == 2)
		{
			Held_PhysicalTestRecord(&f->record,i == 1 ? HELD_RECORD_HFREEZE:HELD_RECORD_HTHAW);
			Held_ClosedClock(&f->record,&f->timeline.clock.clock); CT_REQUIRE(Held_ClosedEvent(f));
			if (i == 2)
			{
				CT_REQUIRE(Held_ClosedText(f,"hold %u %d\n",(unsigned int)f->timeline.physical.raw_moveticks,(int)f->timeline.clock.clock.frzticks));
				Held_PhysicalTestObservation(&f->timeline.physical,&f->record); SV_HeldRecordU(&f->record,"boundary",3);
				CT_REQUIRE(Held_ClosedEvent(f));
			}
			else
			{
				Held_PhysicalTestRecord(&f->record,HELD_RECORD_HMAINT); SV_HeldRecordU(&f->record,"site",3); CT_REQUIRE(Held_ClosedEvent(f));
			}
		}
		if (i == 3)
		{
			CT_REQUIRE(Held_TimelineAdvance(&f->timeline) && f->timeline.finished);
			Held_PhysicalTestObservation(&f->timeline.physical,&f->record); SV_HeldRecordU(&f->record,"boundary",4); CT_REQUIRE(Held_ClosedEvent(f));
			Held_PhysicalTestRecord(&f->record,HELD_RECORD_HTERMINAL); SV_HeldRecordU(&f->record,"closekind",1);
			Held_ClosedClock(&f->record,&f->timeline.clock.clock); CT_REQUIRE(Held_ClosedEvent(f));
		}
		CT_REQUIRE(Held_ClosedCallback(f,HELD_RECORD_HPOST,2,time,rate));
		if (!f->timeline.clock.clock.frzon && !f->timeline.finished) CT_REQUIRE(Held_ClosedSample(f));
		Held_PhysicalTestRecord(&f->record,HELD_RECORD_HPEND); SV_HeldRecordL(&f->record,"last_execution",f->coverage.executions);
		CT_REQUIRE(Held_ClosedEvent(f) && Held_ClosedScope(f,1,2,time,rate));
	}
	CT_REQUIRE(SV_HeldCoverageClosed(&f->coverage) && f->coverage.scene_fences == 33);
	clock = f->timeline.clock.clock;
	CT_REQUIRE(Held_ClosedText(f,"prof ok\nclk %.3f %.3f %.3f %.3f %d %.3f %.3f %.3f\n",
		f->timeline.clock.context.slow,f->timeline.clock.context.hyst,f->timeline.clock.context.allow,
		clock.ckwt,(int)clock.ckgaps,clock.ckcr,clock.ckd,clock.cku));
	CT_REQUIRE(Held_ClosedText(f,"clockend viewing %d %.9g %d %.9g\ninend %u %.9g\nend %d %u 0 0 %u 1 1 %u %u 0 0 1 2 0\nsplit 1 %d\n",
		(int)clock.starttick,clock.tickcarry,(int)clock.frzticks,SV_HeldClockViewSeconds(&f->timeline.clock),(unsigned int)f->timeline.physical.raw_moveticks,
		pmove.msec_carry,(int)clock.ticks,f->samples,f->normal+f->frozen,f->pmrows,f->perows,(int)clock.ticks));
	VectorCopy(pmove.origin,final_origin);
	CT_REQUIRE(SV_HeldTimelineFinish(&f->timeline)); SV_HeldZoneEnd(&f->zone);
	Held_PhysicalTestRecord(&f->record,HELD_RECORD_HSEAL);
#define SEAL(n,v) SV_HeldRecordL(&f->record,n,v)
	SEAL("first_execution",1); SEAL("last_execution",f->coverage.executions); SEAL("executions",f->coverage.executions);
	SEAL("messages",f->coverage.messages); SEAL("pre_enters",f->coverage.pre_enters); SEAL("pre_exits",f->coverage.pre_exits);
	SEAL("movers",f->coverage.movers); SEAL("completions",f->coverage.completions); SEAL("post_enters",f->coverage.post_enters);
	SEAL("post_exits",f->coverage.post_exits); SEAL("commands",f->coverage.commands); SEAL("scene_fences",f->coverage.scene_fences);
	SEAL("control_events",f->coverage.controls); SEAL("effect_dispatches",f->coverage.effects); SV_HeldRecordU(&f->record,"qualification",1);
#undef SEAL
	CT_REQUIRE(Held_ClosedEvent(f) && Held_ClosedView(&f->ledger,&view,&detail));
	ordinary = f->size; CT_REQUIRE(detail <= sizeof(f->bytes)-ordinary);
	memcpy(f->bytes+ordinary,view,detail);
	held_closed_test_diagnostic = 1;
	positive = SV_HeldTrajectoryRecording(f->bytes,ordinary+detail,&f->context,sv.world.worldmodel,&result);
	held_closed_test_diagnostic = 0; CT_REQUIRE(positive);
	CT_TEST(result.native_moves == 4 && f->normal == 2 && f->frozen == 1);
	*acted += result.native_moves;
	metadata_character = f->bytes[ordinary]; f->bytes[ordinary] = 0;
	ordinary_change = strstr(f->bytes," twarp="); f->bytes[ordinary] = metadata_character;
	CT_REQUIRE(ordinary_change != NULL);
	original_character = ordinary_change[7]; ordinary_change[7] = '2';
	CT_TEST(!SV_HeldTrajectoryRecording(f->bytes,ordinary+detail,&f->context,sv.world.worldmodel,&result));
	ordinary_change[7] = original_character == '0' ? '1':'0';
	CT_TEST(!SV_HeldTrajectoryRecording(f->bytes,ordinary+detail,&f->context,sv.world.worldmodel,&result));
	ordinary_change[7] = original_character;
	/* Old normal-only indexes/counts must fail after the released fzin. */
	metadata_character = f->bytes[ordinary]; f->bytes[ordinary] = 0;
	ordinary_change = strstr(f->bytes,"\npm ");
	if (ordinary_change)
		while ((next_change = strstr(ordinary_change+1,"\npm ")) != NULL) ordinary_change = next_change;
	if (ordinary_change) ordinary_change = strchr(ordinary_change+4,' ');
	if (ordinary_change) ++ordinary_change;
	f->bytes[ordinary] = metadata_character;
	CT_REQUIRE(ordinary_change && ordinary_change[0] == '1' && ordinary_change[1] == ' ');
	ordinary_change[0] = '0';
	CT_TEST(!SV_HeldTrajectoryRecording(f->bytes,ordinary+detail,&f->context,sv.world.worldmodel,&result));
	ordinary_change[0] = '1';
	metadata_character = f->bytes[ordinary]; f->bytes[ordinary] = 0;
	ordinary_change = strstr(f->bytes,"\nend "); if (ordinary_change) ++ordinary_change;
	for (j = 0; ordinary_change && j < 5; ++j)
	{
		next_change = strchr(ordinary_change,' ');
		ordinary_change = next_change ? next_change+1:NULL;
	}
	f->bytes[ordinary] = metadata_character;
	CT_REQUIRE(ordinary_change && ordinary_change[0] == '3' && ordinary_change[1] == ' ');
	ordinary_change[0] = '2';
	CT_TEST(!SV_HeldTrajectoryRecording(f->bytes,ordinary+detail,&f->context,sv.world.worldmodel,&result));
	ordinary_change[0] = '3';
	metadata_character = f->bytes[ordinary]; f->bytes[ordinary] = 0;
	ordinary_change = strstr(f->bytes," 100 0 0 0 surfacefriction="); f->bytes[ordinary] = metadata_character;
	CT_REQUIRE(ordinary_change != NULL);
	original_character = ordinary_change[3]; ordinary_change[3] = '1';
	CT_TEST(!SV_HeldTrajectoryRecording(f->bytes,ordinary+detail,&f->context,sv.world.worldmodel,&result));
	ordinary_change[3] = original_character;
	CT_TEST(Held_ClosedRehash(f,ordinary,HELD_RECORD_HOBS,"origin_x",&f->context));
	CT_TEST(Held_ClosedRehash(f,ordinary,HELD_RECORD_HFREEZE,"clock_packet",&f->context));
	CT_REQUIRE(Held_ClosedView(&f->ledger,&view,&detail)); memcpy(f->bytes+ordinary,view,detail);
	previous_top = *p[6]; *p[6] = previous_top-64;
	CT_REQUIRE(SV_HeldZoneFingerprint(restored) && memcmp(restored,SV_HeldRecordGet(&f->context,"zone",'D')->bytes,32));
	CT_REQUIRE(SV_HeldZoneBegin(&wrong_zone,restored,zone_seed));
	CT_TEST(SV_HeldZoneStep(&wrong_zone,final_origin,final_origin,mins,maxs) == 0); SV_HeldZoneEnd(&wrong_zone);
	CT_TEST(!SV_HeldTrajectoryRecording(f->bytes,ordinary+detail,&f->context,sv.world.worldmodel,&result)); *p[6] = previous_top;
	Con_Printf("held closed-byte trajectory: complete native4, rehashed pose/clock and real zone controls acted; synthetic source0\n");
cleanup:
	held_closed_test_diagnostic = 0;
	SV_HeldPhysicalEnd(&preview); SV_HeldZoneEnd(&wrong_zone);
	if (f)
	{
		SV_HeldTimelineEnd(&f->timeline); SV_HeldZoneEnd(&f->zone); SV_HeldLedgerDiscard(&f->ledger);
		Held_PhysicalWipe(f,sizeof(*f)); BZ_Free(f);
	}
	if (changed) for (i = 0; i < bound; ++i) memcpy(p[i],saved[i],globals[i].words*sizeof(float));
	if (changed) CT_TEST(SV_HeldZoneFingerprint(restored) && !memcmp(original,restored,32));
	for (j = 0; j < countof(globals); ++j) Held_PhysicalWipe(saved[j],sizeof(saved[j]));
	Held_PhysicalWipe(&r,sizeof(r)); Held_PhysicalWipe(&clock,sizeof(clock)); Held_PhysicalWipe(&result,sizeof(result));
	Held_PhysicalWipe(&source_state,sizeof(source_state));
	return failures;
#undef CT_TEST
#undef CT_REQUIRE
}
