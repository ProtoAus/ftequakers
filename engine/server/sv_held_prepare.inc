/* ORIGINAL-byte end validation, not a filesystem publisher/readiness flag.
 * Ordinary source I/O uses bounded windows, never a64KiB record ceiling.
 * Details share the fixed native allocation; generic publication streams. */
static int Held_PrepareDestination(const char *path)
{
	const char *p, *start;
	size_t size;
	if (!path || (strncmp(path,"data/runs/",10) && strncmp(path,"data/evidence/",14))) return 0;
	size = strlen(path);
	if (size < 15 || size >= MAX_OSPATH || strcmp(path+size-4,".rec")) return 0;
	start = path;
	for (p = path; ; ++p)
	{
		if (!*p || *p == '/')
		{
			if (p == start || *start == '.' || p[-1] == '.') return 0;
			if (!*p) return 1;
			start = p+1;
		}
		else if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
			(*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.')) return 0;
	}
}
static int Held_PrepareWho(const held_session_t *s, const char *who)
{
	unsigned char digest[32];
	char id[9], slot[24];
	const char *p;
	size_t size, prefix;
	unsigned int i;
	int valid;
	if (!who) return 0;
	if (!s->lobby) return !*who;
	if (!*s->client->guid)
	{
		Q_snprintfz(slot,sizeof(slot),"s%u",s->owner.record_slot);
		return !strcmp(slot,who);
	}
	if (CalcHash(&hash_sha2_256,digest,32,(const unsigned char*)s->client->guid,strlen(s->client->guid)) != 32) return 0;
	for (i = 0; i < 4; ++i) Q_snprintfz(id+2*i,sizeof(id)-2*i,"%02x",digest[i]);
	size = strlen(who);
	valid = size >= 8 && !strcmp(who+size-8,id);
	if (valid && size != 8)
	{
		valid = size >= 10 && size <= 21 && who[size-9] == '-';
		prefix = size-9;
		/* The slug is DISPLAY ONLY, never an actor/publication authority. Only
		 * the existing GUID hash suffix routes identity; UID/VM/serial bind source. */
		for (p = who; valid && p < who+prefix; ++p)
			if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))) valid = 0;
	}
	Held_SessionWipe(digest,sizeof(digest)); Held_SessionWipe(id,sizeof(id));
	return valid;
}
static int Held_PrepareCloseNamespace(const held_session_t *s)
{
	static const char *tags[] = {NULL,"pb","last","shadow","cheat","run","lobby"};
	const char *who, *rid, *nonce, *destination;
	char expected[MAX_OSPATH], directory[80], leaf[160], stamp[32];
	float value, tag, ticks;
	int archived;
	if (!Held_SessionScalar(s->entity,"rec_hm_track",&value) || value != s->track ||
		!Held_SessionScalar(s->entity,"rec_hm_leg",&value) || value != s->leg ||
		!Held_SessionScalar(s->entity,"rec_hm_ticks",&ticks) || ticks != s->claimed_ticks ||
		!Held_SessionScalar(s->entity,"rec_hm_tag",&tag) || tag < 0 || tag > 6 || (unsigned int)tag != tag) return 0;
	rid = Held_SessionString(s->entity,"rec_hm_rid"); nonce = Held_SessionString(s->entity,"rec_hm_nonce");
	who = Held_SessionString(s->entity,"rec_hm_who"); destination = Held_SessionString(s->entity,"rec_hm_destination");
	if (!rid || !nonce || !destination || strcmp(rid,s->owner.record) || strcmp(nonce,s->nonce) || strcmp(destination,s->destination)) return 0;
	if (tag == 0)
	{
		Q_snprintfz(expected,sizeof(expected),"data/evidence/%s/%s.rec",s->map,s->owner.record);
		return !strcmp(expected,s->destination);
	}
	if (!Held_PrepareWho(s,who)) return 0;
	if (!s->track)
	{
		if (!s->leg) Q_strncpyz(directory,"main",sizeof(directory));
		else Q_snprintfz(directory,sizeof(directory),"stage_%u",s->leg);
	}
	else if (!s->leg) Q_snprintfz(directory,sizeof(directory),"bonus_%u",s->track);
	else Q_snprintfz(directory,sizeof(directory),"bonus_%u_stage_%u",s->track,s->leg);
	archived = tag == 1 || tag == 3 || tag == 5 || tag == 6;
	if (!Held_SessionScalar(s->entity,"rec_hm_stamp",&value) || value != (archived ? ticks:-1) || (archived && ticks < 0)) return 0;
	if (archived)
	{
		Q_snprintfz(stamp,sizeof(stamp),"%07u",(unsigned int)floor(ticks));
		Q_snprintfz(leaf,sizeof(leaf),"%s_%s%s%s.rec",stamp,who,*who ? "_":"",tags[(unsigned int)tag]);
	}
	else Q_snprintfz(leaf,sizeof(leaf),"%s%s%s.rec",who,*who ? "_":"",tags[(unsigned int)tag]);
	Q_snprintfz(expected,sizeof(expected),"data/runs/%s/%s/%s",s->map,directory,leaf);
	return !strcmp(expected,s->destination);
}
typedef struct
{
	const held_session_t *session;
	unsigned int seen, done;
} held_namespace_t;
static int Held_PrepareNamespaceLine(void *opaque, const char *line, size_t size)
{
	static const char *keys[] = {"map ","track ","leg ","runid ","nonce "};
	held_namespace_t *n = opaque;
	const held_session_t *s = n->session;
	char expected[MAX_OSPATH];
	unsigned int i;
	size_t length;
	if (n->done) return 1;
	if (!size || size > HELD_MAX_LINE || line[size-1] != '\n') return 0;
	length = size-1;
	if (length == 5 && !memcmp(line,"begin",5)) {n->done = n->seen == 31; return n->done;}
	for (i = 0; i < countof(keys); ++i)
		if (length >= strlen(keys[i]) && !memcmp(line,keys[i],strlen(keys[i]))) break;
	if (i == countof(keys)) return 1;
	if (n->seen & (1u << i)) return 0;
	n->seen |= 1u << i;
	switch (i)
	{
	case 0: Q_snprintfz(expected,sizeof(expected),"map %s",s->map); break;
	case 1: Q_snprintfz(expected,sizeof(expected),"track %u",s->track); break;
	case 2: Q_snprintfz(expected,sizeof(expected),"leg %u",s->leg); break;
	case 3: Q_snprintfz(expected,sizeof(expected),"runid %s",s->owner.record); break;
	default: Q_snprintfz(expected,sizeof(expected),"nonce %s",s->nonce); break;
	}
	return length == strlen(expected) && !memcmp(line,expected,length);
}
static int Held_PrepareHeaderNamespace(const held_session_t *s, const char *data, size_t size)
{
	held_namespace_t n = {s,0,0};
	const char *cursor = data, *end = data+size, *newline;
	while (cursor < end)
	{
		newline = memchr(cursor,'\n',end-cursor);
		if (!newline || !Held_PrepareNamespaceLine(&n,cursor,newline+1-cursor)) return 0;
		if (n.done) return 1;
		cursor = newline+1;
	}
	return 0;
}
static int Held_PrepareRawRead(void *opaque, size_t offset, void *buffer, size_t size)
{
	held_session_t *s = opaque;
	if (size > 4096 || offset > s->prepared.source_size || size > s->prepared.source_size-offset ||
		SV_HeldFileLength(s->source) != (qofs_t)s->prepared.source_size) return 0;
	return SV_HeldFileReadAt(s->source,(qofs_t)offset,buffer,(int)size);
}
static int Held_PreparedRead(void *opaque, size_t offset, void *buffer, size_t size)
{
	static const char header11[] = HELD_BASE_REC_HEADER, header12[] = HELD_NATIVE_REC_HEADER;
	held_session_t *s = opaque;
	held_prepared_t *p = &s->prepared;
	unsigned char *out = buffer;
	char header[sizeof(header11)-1];
	size_t count, original = size;
	if (!p->streamed || size > 4096 || offset > p->size || size > p->size-offset ||
		SV_HeldFileLength(s->source) != (qofs_t)p->source_size) return 0;
	if (offset < p->source_size)
	{
		count = min(size,p->source_size-offset);
		if (Held_PrepareRawRead(s,offset,out,count) != count) return 0;
		if (offset < sizeof(header11)-1)
		{
			if (Held_PrepareRawRead(s,0,header,sizeof(header)) != sizeof(header) || memcmp(header,header11,sizeof(header))) return 0;
			memcpy(out,header12+offset,min(count,sizeof(header12)-1-offset));
		}
		offset += count; out += count; size -= count;
	}
	if (size) memcpy(out,p->generic+offset-p->source_size,size);
	return (int)original;
}
static int Held_PrepareStreamParse(held_session_t *s, held_witness_t *w, size_t *ordinary)
{
	held_namespace_t n = {s,0,0};
	return SV_HeldRecordingRead(Held_PreparedRead,s,s->prepared.size,w,ordinary,Held_PrepareNamespaceLine,&n) &&
		n.done && *ordinary == s->prepared.source_size;
}
static int Held_PrepareReadInto(held_session_t *s, char *bytes, size_t size)
{
	size_t offset, count;
	for (offset = 0; offset < size; offset += count)
	{
		count = min((size_t)4096,size-offset);
		if (Held_PrepareRawRead(s,offset,bytes+offset,count) != count) return 0;
	}
	return 1;
}
int SV_HeldSessionRequestClose(client_t *client, const char *generation)
{
	held_session_t *s = Held_SessionFind(client);
	const char *destination;
	float ticks;
	if (!s || !generation || strcmp(generation,s->generation) || !s->selected ||
		!Held_SessionOwnerLive(s) || !Held_SessionPermission(s->entity) || s->revoked ||
		!SV_HeldFileClosed(s->source)) return 0;
	destination = Held_SessionString(s->entity,"rec_hm_destination");
	if (!Held_PrepareDestination(destination) || !Held_SessionScalar(s->entity,"rec_hm_ticks",&ticks) ||
		ticks < -1 || ticks > 16777216) {Held_SessionFail(s,2); return 0;}
	if (s->close_requested) return !strcmp(destination,s->destination) && ticks == s->claimed_ticks;
	Q_strncpyz(s->destination,destination,sizeof(s->destination));
	s->claimed_ticks = ticks;
	if (!Held_PrepareCloseNamespace(s)) {Held_SessionFail(s,2); return 0;}
	s->close_requested = 1;
	if (s->failed) return 1;
	if (!s->initialized || !s->capture.started || !s->capture.terminal || s->capture.pending_move ||
		!client->held_coverage.post_open || !client->held_coverage.message_open || ticks < 0)
	{
		Held_SessionFail(s,2);
		return 1; /* Immutable ordinary source may still close generically. */
	}
	s->capture.close_requested = 1;
	return 1; /* Actual native exits pending; not a publication receipt. */
}
static int Held_PrepareCensus(const held_session_t *s, const held_witness_t *w)
{
	const held_coverage_t *c = &s->capture.census;
	return s->capture.sealed && s->ledger.sealed && SV_HeldCoverageClosed(c) && w->status == 3 &&
		w->records == s->ledger.ordinal && !memcmp(w->generation,s->ledger.generation,16) &&
		!memcmp(w->ledger,s->ledger.ledger,32) && w->execution == c->executions && w->message == c->messages &&
		w->pre_enters == c->pre_enters && w->pre_exits == c->pre_exits && w->movers == c->movers &&
		w->completions == c->completions && w->post_enters == c->post_enters && w->post_exits == c->post_exits &&
		w->commands == c->commands && w->scene_fences == c->scene_fences && w->control_events == c->controls &&
		w->alias_count == s->capture.aliases && !c->effects;
}
static int Held_PrepareNativeContext(held_session_t *s, held_record_t *context)
{
	movevars_t current;
	int result;
	memset(&current,0,sizeof(current));
	result = SV_HeldMoveVarsCapture(s->client,&current) && SV_HeldContextBuildNative(&current,context) &&
		SV_HeldRecordI(context,"ctx_geomtype",(int32_t)s->entity->xv->geomtype) &&
		Held_CaptureContextEqual(context,&s->capture.context);
	Held_SessionWipe(&current,sizeof(current));
	return result;
}
static int Held_PrepareSourceMatches(held_session_t *s, const char *candidate, size_t size)
{
	static const char header11[] = HELD_BASE_REC_HEADER, header12[] = HELD_NATIVE_REC_HEADER;
	unsigned char buffer[4096];
	size_t offset, count;
	int result = 0;
	if (SV_HeldFileLength(s->source) != (qofs_t)size) goto done;
	for (offset = 0; offset < size; offset += count)
	{
		count = min(sizeof(buffer),size-offset);
		if (SV_HeldFileReadAt(s->source,(qofs_t)offset,buffer,(int)count) != count) goto done;
		if (!offset)
		{
			if (count < sizeof(header11)-1 || memcmp(buffer,header11,sizeof(header11)-1)) goto done;
			memcpy(buffer,header12,sizeof(header12)-1);
		}
		if (memcmp(buffer,candidate+offset,count)) goto done;
	}
	result = 1;
done:
	Held_SessionWipe(buffer,sizeof(buffer));
	return result;
}
typedef struct
{
	held_session_t *session;
	held_input_t *inputs, endpoint;
	unsigned int count;
} held_projection_t;
static int Held_PrepareProjectRecord(held_record_t *r, void *opaque)
{
	held_projection_t *p = opaque;
	const held_value_t *v;
	uint64_t execution;
	unsigned int kind, index, context, pmtype;
	float actual, derived;
	if (r->family != HELD_RECORD_HMOVE) return 1;
	v = SV_HeldRecordGet(r,"execution",'L'); if (!v || !v->l || v->l > HELD_MAX_PIECES) return 0; execution = v->l;
	v = SV_HeldRecordGet(r,"ownerkind",'U'); if (!v) return 0; kind = v->u;
	if (p->session->capture.protected_piece[execution-1]) return 1;
	if (kind != 1 && kind != 2) return 0; /* Held is always protected. */
	v = SV_HeldRecordGet(r,"ownerindex",'U'); if (!v || v->u >= p->count) return 0; index = v->u;
	v = SV_HeldRecordGet(r,"context",'U'); if (!v || v->u != 1) return 0; context = v->u;
	v = SV_HeldRecordGet(r,"pmtype",'U'); if (!v || v->u != PM_NORMAL) return 0; pmtype = v->u;
	v = SV_HeldRecordGet(r,"effective_msec",'F'); if (!v) return 0; actual = v->f;
	if (p->inputs[index].kind != kind || !SV_HeldInputMsec(&p->inputs[index],
		index+1 < p->count ? &p->inputs[index+1]:&p->endpoint,p->session->capture.source_vars.ticrate,&derived) ||
		memcmp(&actual,&derived,sizeof(actual))) return 0;
	/* No ordinary actual timing is exported. It was transient until this exact
	 * comparison against EXISTING successor/inend rows, never across a hold. */
	SV_HeldRecordBegin(r,HELD_RECORD_HORDINARY);
	return SV_HeldRecordU(r,"ownerkind",kind) && SV_HeldRecordU(r,"ownerindex",index) &&
		SV_HeldRecordU(r,"context",context) && SV_HeldRecordU(r,"pmtype",pmtype);
}
static int Held_PrepareProject(held_session_t *s, const char *source, size_t size, size_t tape_offset)
{
	held_projection_t p;
	const char *cursor = source, *end = source+size, *newline;
	size_t length;
	unsigned int endpoints = 0;
	int result = 0;
	memset(&p,0,sizeof(p)); p.session = s;
	p.inputs = BZF_Malloc(HELD_MAX_PIECES*sizeof(*p.inputs));
	if (!p.inputs) return 0;
	while (cursor < end)
	{
		newline = memchr(cursor,'\n',end-cursor);
		if (!newline || (length = newline+1-cursor) > HELD_MAX_LINE) goto done;
		if ((length >= 3 && !memcmp(cursor,"in ",3)) || (length >= 5 && !memcmp(cursor,"fzin ",5)))
		{
			if (endpoints || p.count == HELD_MAX_PIECES || !SV_HeldInputParse(cursor,length,&p.inputs[p.count])) goto done;
			++p.count;
		}
		else if (length >= 6 && !memcmp(cursor,"inend ",6))
		{
			if (endpoints++ || !SV_HeldInputEnd(cursor,length,&p.endpoint)) goto done;
		}
		cursor = newline+1;
	}
	if (endpoints != 1 || p.count != s->capture.aliases || !SV_HeldLedgerRecodeFrom(&s->ledger,tape_offset,Held_PrepareProjectRecord,&p)) goto done;
	s->capture.projected = 1;
	result = 1;
done:
	Held_SessionWipe(p.inputs,HELD_MAX_PIECES*sizeof(*p.inputs)); BZ_Free(p.inputs);
	Held_SessionWipe(&p,sizeof(p));
	return result;
}
static int Held_PrepareGeneric(held_session_t *s, size_t length)
{
	held_prepared_t *p = &s->prepared;
	held_witness_t *w = NULL;
	unsigned char digest[32];
	size_t ordinary, suffix;
	int valid = 0;
	Q_snprintfz(p->generic,sizeof(p->generic),"hfail %s %08x\n",s->generation,s->failed == 1 ? 1:2);
	suffix = strlen(p->generic);
	if (length > (size_t)INT64_MAX-suffix) goto done;
	p->source_size = length; p->size = length+suffix; p->streamed = 1;
	w = BZF_Malloc(sizeof(*w));
	if (!w || !SV_HeldReadHash(Held_PrepareRawRead,s,length,p->source_digest) ||
		!SV_HeldReadHash(Held_PreparedRead,s,p->size,p->digest) || !Held_PrepareStreamParse(s,w,&ordinary) ||
		w->status != (s->failed == 1 ? 1u:2u) ||
		!SV_HeldReadHash(Held_PrepareRawRead,s,length,digest) || memcmp(digest,p->source_digest,32) ||
		!Held_SessionOwnerLive(s) || !Held_SessionPermission(s->entity) || s->revoked ||
		!Held_PrepareCloseNamespace(s)) goto done;
	p->classification = 2; p->progs_epoch = SV_HeldProgsEpoch(); p->ready = 1;
	valid = 1; /* Existing ordinary bytes plus a coarse failure, no qualification. */
done:
	if (w) {Held_SessionWipe(w,sizeof(*w)); BZ_Free(w);}
	Held_SessionWipe(digest,sizeof(digest));
	if (!valid) Held_PreparedDiscard(s);
	return valid;
}
static int Held_PrepareOriginal(held_session_t *s)
{
	static const char header11[] = HELD_BASE_REC_HEADER, header12[] = HELD_NATIVE_REC_HEADER;
	held_witness_t *w;
	held_record_t context;
	held_trajectory_t trajectory;
	qofs_t length;
	char *bytes;
	size_t suffix, total, ordinary, capacity, tape_offset;
	unsigned int attempt;
	unsigned char projected_digest[32];
	int valid;
	if (s->prepared.ready) return 1;
	for (attempt = 0; attempt != 2; ++attempt)
	{
		if (!s->close_requested || !Held_SessionOwnerLive(s) || !Held_SessionPermission(s->entity) ||
			s->revoked || !SV_HeldFileClosed(s->source) ||
			(!s->failed && (!s->capture.sealed || !s->ledger.sealed))) return 0;
		length = SV_HeldFileLength(s->source);
		if (length < (qofs_t)(sizeof(header11)-1) || (uint64_t)length > SIZE_MAX) return 0;
		if (s->failed)
		{
			valid = Held_PrepareGeneric(s,(size_t)length);
			Held_SessionWipe(projected_digest,sizeof(projected_digest));
			return valid;
		}
		/* ONE fixed, globally budgeted allocation, no new whole-body duplicate.
		 * Move closed tape to its tail; source uses remaining space, not an IO cap.
		 * Resource decline affects DETAILS only; generic streams the whole source. */
		capacity = s->ledger.capacity; suffix = s->ledger.size;
		if (s->capture.projected || capacity < HELD_MAX_DETAIL+HELD_IO_BUFFER+1 ||
			suffix >= capacity || (size_t)length >= capacity-suffix)
			{Held_SessionFail(s,1); continue;}
		bytes = s->ledger.detail; tape_offset = capacity-suffix;
		memmove(bytes+tape_offset,bytes,suffix);
		s->prepared.source_size = (size_t)length;
		valid = Held_PrepareReadInto(s,bytes,(size_t)length) &&
			!memcmp(bytes,header11,sizeof(header11)-1) && bytes[length-1] == '\n' &&
			CalcHash(&hash_sha2_256,projected_digest,32,bytes,(int)length) == 32 &&
			Held_PrepareProject(s,bytes,(size_t)length,tape_offset);
		if (!valid) {Held_SessionFail(s,1); continue;}
		suffix = s->ledger.size;
		memmove(bytes+length,bytes,suffix);
		/* Projection may overwrite the temporary source, so read ORIGINAL again. */
		valid = Held_PrepareReadInto(s,bytes,(size_t)length);
		total = (size_t)length+suffix;
		valid = valid && !memcmp(bytes,header11,sizeof(header11)-1) && bytes[length-1] == '\n' &&
			Held_PrepareHeaderNamespace(s,bytes,(size_t)length) &&
			CalcHash(&hash_sha2_256,s->prepared.source_digest,32,bytes,(int)length) == 32 &&
			!memcmp(projected_digest,s->prepared.source_digest,32);
		if (valid)
		{
			memcpy(bytes,header12,sizeof(header12)-1); bytes[total] = 0;
			w = BZF_Malloc(sizeof(*w));
			if (w)
			{
				valid = SV_HeldRecordingParse(bytes,total,w,&ordinary) && ordinary == (size_t)length;
				if (valid)
				{
					memset(&context,0,sizeof(context)); memset(&trajectory,0,sizeof(trajectory));
					valid = Held_PrepareCensus(s,w) && Held_PrepareNativeContext(s,&context) &&
						SV_HeldTrajectoryRecording(bytes,total,&context,s->world,&trajectory) &&
						trajectory.ticks == s->claimed_ticks;
					Held_SessionWipe(&context,sizeof(context)); Held_SessionWipe(&trajectory,sizeof(trajectory));
				}
				Held_SessionWipe(w,sizeof(*w)); BZ_Free(w);
			}
			else valid = 0;
		}
		if (valid && Held_SessionOwnerLive(s) && Held_SessionPermission(s->entity) && !s->revoked &&
			Held_PrepareSourceMatches(s,bytes,(size_t)length) &&
			CalcHash(&hash_sha2_256,s->prepared.digest,32,bytes,(int)total) == 32)
		{
			Held_SessionWipe(bytes+total,capacity-total);
			s->prepared.data = bytes; s->prepared.size = total; s->prepared.source_size = (size_t)length;
			s->prepared.capacity = capacity; s->prepared.ledger_owned = 1;
			s->prepared.progs_epoch = SV_HeldProgsEpoch(); s->prepared.classification = 1;
			s->prepared.ready = 1;
			Held_SessionWipe(projected_digest,sizeof(projected_digest));
			return 1; /* Original/model binding only, still NO store authority. */
		}
		Held_SessionFail(s,2);
	}
	Held_SessionWipe(projected_digest,sizeof(projected_digest));
	return 0;
}
const char *SV_HeldSessionPrepare(client_t *client, const char *generation, const char *pair)
{
	held_session_t *s = Held_SessionFind(client);
	size_t source_size;
	if (!s || !generation || !pair || strcmp(generation,s->generation) || !s->selected ||
		!s->close_requested || !Held_SessionOwnerLive(s)) return "unsupported";
	if (!Held_SessionPermission(s->entity)) {s->revoked = 1; Held_SessionFail(s,1); return "unsupported";}
	if (s->revoked) return "unsupported";
	source_size = strlen(s->path);
	if (strlen(pair) != source_size+1+strlen(s->destination) || memcmp(pair,s->path,source_size) ||
		pair[source_size] != ' ' || strcmp(pair+source_size+1,s->destination) || !Held_PrepareCloseNamespace(s))
	{
		Held_SessionFail(s,2);
		return "unsupported";
	}
	if (!s->failed && !s->capture.sealed) return "pending";
	return Held_PrepareOriginal(s) ? "sealed":"unsupported";
	/* Never "published" until separate checked native store commit exists. */
}
static qboolean Held_PreparedValidate(void *opaque, const void *data, size_t size)
{
	held_session_t *s = opaque;
	held_witness_t *w = NULL;
	held_record_t context;
	held_trajectory_t trajectory;
	unsigned char digest[32];
	size_t ordinary = 0;
	int valid = 0;
	memset(&context,0,sizeof(context)); memset(&trajectory,0,sizeof(trajectory));
	if (!s || !s->prepared.ready || data != s->prepared.data || size != s->prepared.size ||
		!s->selected || !s->close_requested || s->revoked || s->prepared.progs_epoch != SV_HeldProgsEpoch() ||
		!Held_SessionOwnerLive(s) || !Held_SessionPermission(s->entity) || !SV_HeldFileClosed(s->source) ||
		!Held_PrepareCloseNamespace(s)) goto done;
	w = BZF_Malloc(sizeof(*w)); if (!w) goto done;
	if (s->prepared.streamed)
	{
		if (data || s->prepared.classification != 2 || s->prepared.ledger_owned || s->prepared.capacity ||
			!SV_HeldReadHash(Held_PreparedRead,s,size,digest) || memcmp(digest,s->prepared.digest,32) ||
			!Held_PrepareStreamParse(s,w,&ordinary) ||
			!SV_HeldReadHash(Held_PrepareRawRead,s,s->prepared.source_size,digest) ||
			memcmp(digest,s->prepared.source_digest,32)) goto done;
	}
	else if (!Held_PrepareHeaderNamespace(s,data,s->prepared.source_size) ||
		CalcHash(&hash_sha2_256,digest,32,data,size) != 32 || memcmp(digest,s->prepared.digest,32) ||
		!Held_PrepareSourceMatches(s,data,s->prepared.source_size) ||
		!SV_HeldRecordingParse(data,size,w,&ordinary)) goto done;
	if (ordinary != s->prepared.source_size) goto done;
	if (s->prepared.classification == 1)
		valid = !s->prepared.streamed && !s->failed && Held_PrepareCensus(s,w) && Held_PrepareNativeContext(s,&context) &&
			SV_HeldTrajectoryRecording(data,size,&context,s->world,&trajectory) && trajectory.ticks == s->claimed_ticks;
	else if (s->prepared.classification == 2) valid = s->failed && w->status == (s->failed == 1 ? 1u:2u);
	valid = valid && Held_SessionOwnerLive(s) && Held_SessionPermission(s->entity) && !s->revoked &&
		s->prepared.progs_epoch == SV_HeldProgsEpoch();
 done:
	if (w) {Held_SessionWipe(w,sizeof(*w)); BZ_Free(w);}
	Held_SessionWipe(digest,sizeof(digest)); Held_SessionWipe(&context,sizeof(context)); Held_SessionWipe(&trajectory,sizeof(trajectory));
	return valid;
}
const char *SV_HeldSessionFinalize(client_t *client, const char *generation, const char *pair)
{
	held_session_t *s = Held_SessionFind(client);
	held_file_ticket_t *original;
	const char *state;
	size_t prefix;
	int committed, consumed;
	if (!s || !generation || !pair || strcmp(generation,s->generation)) return "unsupported";
	if (s->published)
	{
		prefix = strlen(s->path);
		return Held_SessionOwnerIdentity(s) && strlen(pair) == prefix+1+strlen(s->destination) &&
			!memcmp(pair,s->path,prefix) && pair[prefix] == ' ' && !strcmp(pair+prefix+1,s->destination) ? "published":"unsupported";
	}
	state = SV_HeldSessionPrepare(client,generation,pair);
	if (!strcmp(state,"pending")) return state;
	if (strcmp(state,"sealed") || !Held_PreparedValidate(s,s->prepared.data,s->prepared.size))
	{
		Held_SessionFail(s,2);
		return "unsupported";
	}
	/* A generic public copy never consumes the failed ORIGINAL. */
	if (s->prepared.classification == 2 &&
		!SV_HeldFileRecover(s->source,s->recovery_root,s->generation)) return "unsupported";
	if (s->prepared.streamed)
		committed = SV_HeldStorePublishReader(s->recovery_root,s->destination,s->generation,Held_PreparedRead,
			s->prepared.size,s->prepared.digest,Held_PreparedValidate,s);
	else committed = SV_HeldStorePublish(s->recovery_root,s->destination,s->generation,s->prepared.data,
		s->prepared.size,s->prepared.digest,Held_PreparedValidate,s);
	if (committed != 1)
	{
		Held_SessionFail(s,2); /* No partial/private detail survives any refusal. */
		return "unsupported";
	}
	/* Only a real atomic commit creates this receipt. Exact original cleanup is
	 * AFTER commit and may not turn that fact into a false failed-swap report. */
	s->published = 1; original = s->source; s->source = NULL;
	consumed = SV_HeldFileRetire(original,!s->failed && SV_HeldStoreHealthy());
	if (!consumed || !SV_HeldStoreHealthy())
		Con_Printf(CON_WARNING "held: published; ordinary source retained for recovery\n");
	Held_PreparedDiscard(s); SV_HeldLedgerDiscard(&s->ledger);
	Held_SessionWipe(&s->capture,sizeof(s->capture)); s->initialized = 0;
	return "published";
}
