#include "quakedef.h"
#include "sv_held_ledger.h"
#include "sv_held_replay.h" /* shared ordinary neutral-floor grammar */

#define HELD_LEDGER_MEMORY (256u * 1024u * 1024u)
static size_t held_ledger_memory;
static unsigned int held_ledger_count;

static void Held_Wipe(void *ptr, size_t size)
{
	volatile unsigned char *p = ptr;
	while (size--)
		*p++ = 0;
}

static int Held_Hex(unsigned char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
}

static int Held_Field(const held_record_t *r, const char *name, char type)
{
	const char *p;
	unsigned int i;
	size_t length = strlen(name);
	if (r->family >= HELD_RECORD_COUNT)
		return -1;
	p = held_schemas[r->family].fields;
	for (i = 0; i < held_schemas[r->family].count; ++i)
	{
		const char *colon = strchr(p, ':');
		if (!colon) return -1;
		if ((size_t)(colon - p) == length && !memcmp(p, name, length) && colon[1] == type)
			return (int)i;
		p = colon + 2;
		if (*p == ' ') ++p;
	}
	return -1;
}

const held_value_t *SV_HeldRecordGet(const held_record_t *r, const char *name, char type)
{
	int index;
	if (!r || !name) return NULL;
	index = Held_Field(r, name, type);
	return index >= 0 && r->assigned[index] ? &r->values[index] : NULL;
}

int SV_HeldRecordBegin(held_record_t *r, held_record_family_t family)
{
	memset(r, 0, sizeof(*r));
	if (family >= HELD_RECORD_COUNT || held_schemas[family].count > HELD_MAX_FIELDS)
		return 0;
	r->family = family;
	return 1;
}

int SV_HeldRecordU(held_record_t *r, const char *name, uint32_t value)
{
	int i = Held_Field(r, name, 'U');
	if (i < 0) return 0;
	r->values[i].u = value;
	r->assigned[i] = 1;
	return 1;
}

int SV_HeldRecordI(held_record_t *r, const char *name, int32_t value)
{
	int i = Held_Field(r, name, 'I');
	if (i < 0) return 0;
	r->values[i].i = value;
	r->assigned[i] = 1;
	return 1;
}

int SV_HeldRecordL(held_record_t *r, const char *name, uint64_t value)
{
	int i = Held_Field(r, name, 'L');
	if (i < 0) return 0;
	r->values[i].l = value;
	r->assigned[i] = 1;
	return 1;
}

int SV_HeldRecordF(held_record_t *r, const char *name, float value)
{
	unsigned int bits;
	int i = Held_Field(r, name, 'F');
	memcpy(&bits, &value, sizeof(bits));
	if (i < 0 || (bits & 0x7f800000u) == 0x7f800000u) return 0;
	r->values[i].f = value;
	r->assigned[i] = 1;
	return 1;
}

int SV_HeldRecordBytes(held_record_t *r, const char *name, const unsigned char *value, size_t size)
{
	int i;
	char type = size == 16 ? 'G' : size == 32 ? 'D' : 0;
	if (!type || !value) return 0;
	i = Held_Field(r, name, type);
	if (i < 0) return 0;
	memcpy(r->values[i].bytes, value, size);
	r->assigned[i] = 1;
	return 1;
}

void *SV_HeldBufferAlloc(size_t size)
{
	void *buffer;
	if (!size || size > HELD_IO_BUFFER+1 || held_ledger_memory > HELD_LEDGER_MEMORY-size) return NULL;
	buffer = BZF_Malloc(size);
	if (buffer) held_ledger_memory += size;
	return buffer;
}
void SV_HeldBufferFree(void *buffer, size_t size)
{
	if (!buffer) return;
	Held_Wipe(buffer,size); BZ_Free(buffer);
	held_ledger_memory -= size;
}
void SV_HeldLedgerFail(held_ledger_t *l, unsigned int status)
{
	if (l->detail)
	{
		Held_Wipe(l->detail, l->capacity);
		BZ_Free(l->detail);
		l->detail = NULL;
		held_ledger_memory -= l->capacity + sizeof(*l);
		--held_ledger_count;
	}
	l->capacity = 0;
	Held_Wipe(l->ledger, sizeof(l->ledger));
	Held_Wipe(l->execution_ledger, sizeof(l->execution_ledger));
	Held_Wipe(l->pending_move, sizeof(l->pending_move));
	l->pending_move_size = l->size = 0;
	l->ordinal = 0;
	l->sealed = l->have_initial = 0;
	l->failed = status == 1 ? 1 : 2;
}

void SV_HeldLedgerDiscard(held_ledger_t *l)
{
	SV_HeldLedgerFail(l, 2);
	Held_Wipe(l, sizeof(*l));
}

int SV_HeldLedgerBegin(held_ledger_t *l, const char *generation)
{
	unsigned int i;
	if (l->initialized || l->detail || l->failed || !generation || strlen(generation) != 32)
		return 0;
	for (i = 0; i != 32; ++i)
		if (Held_Hex(generation[i]) < 0) return 0;
	l->initialized = 1;
	for (i = 0; i != 16; ++i)
		l->generation[i] = (Held_Hex(generation[2*i]) << 4) | Held_Hex(generation[2*i+1]);
	l->capacity = HELD_MAX_DETAIL + HELD_IO_BUFFER + 1;
	if (held_ledger_count >= 128 || held_ledger_memory > HELD_LEDGER_MEMORY - l->capacity - sizeof(*l))
	{
		SV_HeldLedgerFail(l, 2);
		return 0;
	}
	l->detail = BZF_Malloc(l->capacity);
	if (!l->detail)
	{
		SV_HeldLedgerFail(l, 2);
		return 0;
	}
	held_ledger_memory += l->capacity + sizeof(*l);
	++held_ledger_count;
	if (CalcHash(&hash_sha2_256, l->ledger, 32,
		(const unsigned char*)"FTESURF-aircell1-ledger-1", strlen("FTESURF-aircell1-ledger-1")) != 32 ||
		CalcHash(&hash_sha2_256, l->execution_ledger, 32,
		(const unsigned char*)"FTESURF-aircell1-exec-1", strlen("FTESURF-aircell1-exec-1")) != 32)
	{
		SV_HeldLedgerFail(l, 2);
		return 0;
	}
	return 1;
}

static int Held_Encode(const held_record_t *r, char line[8192], size_t *linesize,
	unsigned char canonical[1024], size_t *canonicalsize)
{
	const held_schema_t *schema = &held_schemas[r->family];
	const char *p = schema->fields;
	const char hex[] = "0123456789abcdef";
	size_t text = strlen(schema->name), binary = text + 1;
	unsigned int i, j;
	memcpy(line, schema->name, text);
	memcpy(canonical, schema->name, binary);
	for (i = 0; i < schema->count; ++i)
	{
		const char *colon = strchr(p, ':');
		unsigned char bytes[32];
		unsigned int width;
		uint64_t number = 0;
		uint32_t bits;
		if (!colon || !r->assigned[i]) return 0;
		switch (colon[1])
		{
		case 'G': width = 16; memcpy(bytes, r->values[i].bytes, width); break;
		case 'D': width = 32; memcpy(bytes, r->values[i].bytes, width); break;
		case 'L': width = 8; number = r->values[i].l; break;
		case 'U': width = 4; number = r->values[i].u; break;
		case 'I': width = 4; number = (uint32_t)r->values[i].i; break;
		case 'F':
			width = 4;
			memcpy(&bits, &r->values[i].f, sizeof(bits));
			if ((bits & 0x7f800000u) == 0x7f800000u) return 0;
			number = bits;
			break;
		default: return 0;
		}
		if (colon[1] != 'G' && colon[1] != 'D')
			for (j = 0; j < width; ++j)
				bytes[j] = (unsigned char)(number >> (8 * (width - 1 - j)));
		if (text + 1 + 2*width + 1 > 8192 || binary + width > 1024) return 0;
		line[text++] = ' ';
		for (j = 0; j < width; ++j)
		{
			line[text++] = hex[bytes[j] >> 4];
			line[text++] = hex[bytes[j] & 15];
		}
		memcpy(canonical + binary, bytes, width);
		binary += width;
		p = colon + 2;
		if (*p == ' ') ++p;
	}
	line[text++] = '\n';
	*linesize = text;
	*canonicalsize = binary;
	return 1;
}

int SV_HeldRecordParse(const char *line, size_t length, held_record_t *r)
{
	static const char *nonnegative[] = {"effective_msec", "gamespeed", "frametime", "carry", "msec_carry",
		"tickused", "ctx_ticrate", "ctx_gamespeed", "clock_frzcarry"};
	static const char *booleans[] = {"safeorigin_known", "ground", "jump_held", "onladder", "capsule", "ducking",
		"ducked", "srcladder", "surfing", "ctx_capsule"};
	const char *cursor, *end, *token, *spec, *colon;
	unsigned int family, field, i, width;
	int index;
	size_t count;
	uint64_t number;
	uint32_t bits;
	int digit;
	if (!r) return 0;
	memset(r, 0, sizeof(*r));
	if (!line || !length || length > HELD_MAX_LINE || line[length-1] != '\n') goto malformed;
	cursor = line;
	end = line + length - 1;
	if (end == line) goto malformed;
	token = cursor;
	while (cursor < end && *cursor != ' ') ++cursor;
	count = cursor - token;
	for (family = 0; family < HELD_RECORD_COUNT; ++family)
		if (strlen(held_schemas[family].name) == count && !memcmp(token, held_schemas[family].name, count)) break;
	if (family == HELD_RECORD_COUNT) goto malformed;
	SV_HeldRecordBegin(r, (held_record_family_t)family);
	spec = held_schemas[family].fields;
	for (field = 0; field < held_schemas[family].count; ++field)
	{
		if (cursor == end || *cursor++ != ' ') goto malformed;
		token = cursor;
		while (cursor < end && *cursor != ' ') ++cursor;
		count = cursor - token;
		colon = strchr(spec, ':');
		if (!colon) goto malformed;
		switch (colon[1])
		{
		case 'G': width = 16; break;
		case 'D': width = 32; break;
		case 'L': width = 8; break;
		case 'U': case 'I': case 'F': width = 4; break;
		default: goto malformed;
		}
		if (count != width * 2) goto malformed;
		number = 0;
		for (i = 0; i < count; ++i)
		{
			digit = Held_Hex((unsigned char)token[i]);
			if (digit < 0) goto malformed;
			if (colon[1] == 'G' || colon[1] == 'D')
			{
				if (!(i & 1)) r->values[field].bytes[i >> 1] = digit << 4;
				else r->values[field].bytes[i >> 1] |= digit;
			}
			else number = (number << 4) | (unsigned int)digit;
		}
		switch (colon[1])
		{
		case 'L': r->values[field].l = number; break;
		case 'U': r->values[field].u = (uint32_t)number; break;
		case 'I':
			bits = (uint32_t)number;
			memcpy(&r->values[field].i, &bits, sizeof(bits));
			break;
		case 'F':
			bits = (uint32_t)number;
			if ((bits & 0x7f800000u) == 0x7f800000u) goto malformed;
			memcpy(&r->values[field].f, &bits, sizeof(bits));
			break;
		default: break;
		}
		r->assigned[field] = 1;
		spec = colon + 2;
		if (*spec == ' ') ++spec;
	}
	if (cursor != end) goto malformed;
	for (i = 0; i < countof(nonnegative); ++i)
	{
		index = Held_Field(r, nonnegative[i], 'F');
		if (index < 0) continue;
		memcpy(&bits, &r->values[index].f, sizeof(bits));
		if (bits & 0x80000000u) goto malformed; /* Includes negative zero. */
	}
	for (i = 0; i < countof(booleans); ++i)
	{
		index = Held_Field(r, booleans[i], 'U');
		if (index >= 0 && r->values[index].u > 1) goto malformed;
	}
	return 1;
malformed:
	Held_Wipe(r, sizeof(*r));
	return 0;
}

int SV_HeldLedgerAppend(held_ledger_t *l, held_record_t *r, uint64_t message, uint64_t execution)
{
	unsigned char canonical[1024], chained[32 + 2*1024];
	char line[8192];
	size_t linesize, canonicalsize;
	if (!l->initialized || l->failed || !l->detail || l->sealed ||
		r->family >= HELD_RECORD_COUNT || r->family == HELD_RECORD_HFAIL)
		return 0;
	if (l->ordinal >= HELD_MAX_RECORDS ||
		(l->ordinal == 0 && r->family != HELD_RECORD_HCTX) ||
		(l->ordinal == 1 && r->family != HELD_RECORD_HSCOPE) ||
		(l->ordinal > 0 && r->family == HELD_RECORD_HCTX) ||
		(r->family == HELD_RECORD_HINIT && (l->have_initial || l->ordinal < 2)) ||
		(!l->have_initial && l->ordinal > 0 && r->family != HELD_RECORD_HINIT &&
		 r->family != HELD_RECORD_HSCOPE && r->family != HELD_RECORD_HPEND))
		goto failure;
	if (!SV_HeldRecordBytes(r, "generation", l->generation, 16) ||
		!SV_HeldRecordL(r, "ordinal", l->ordinal + 1) ||
		!SV_HeldRecordL(r, "message", message) || !SV_HeldRecordL(r, "execution", execution))
		goto failure;
	if (r->family == HELD_RECORD_HSEAL)
	{
		if (l->pending_move_size || message || execution ||
			!SV_HeldRecordBytes(r, "ledger", l->ledger, 32) ||
			!SV_HeldRecordBytes(r, "execution_ledger", l->execution_ledger, 32)) goto failure;
	}
	if (!Held_Encode(r, line, &linesize, canonical, &canonicalsize) ||
		linesize > HELD_MAX_DETAIL - l->size) goto failure;
	if (r->family == HELD_RECORD_HMOVE || r->family == HELD_RECORD_HORDINARY)
	{
		if (l->pending_move_size) goto failure;
		memcpy(l->pending_move, canonical, canonicalsize);
		l->pending_move_size = canonicalsize;
	}
	else if (r->family == HELD_RECORD_HDONE)
	{
		if (!l->pending_move_size) goto failure;
		memcpy(chained, l->execution_ledger, 32);
		memcpy(chained + 32, l->pending_move, l->pending_move_size);
		memcpy(chained + 32 + l->pending_move_size, canonical, canonicalsize);
		if (CalcHash(&hash_sha2_256, l->execution_ledger, 32, chained,
			32 + l->pending_move_size + canonicalsize) != 32) goto failure;
		Held_Wipe(l->pending_move, sizeof(l->pending_move));
		l->pending_move_size = 0;
	}
	if (r->family != HELD_RECORD_HSEAL)
	{
		memcpy(chained, l->ledger, 32);
		memcpy(chained + 32, canonical, canonicalsize);
		if (CalcHash(&hash_sha2_256, l->ledger, 32, chained, 32 + canonicalsize) != 32) goto failure;
	}
	memcpy(l->detail + l->size, line, linesize);
	l->size += linesize;
	++l->ordinal;
	if (r->family == HELD_RECORD_HINIT) l->have_initial = 1;
	l->sealed = (r->family == HELD_RECORD_HSEAL);
	return 1;
failure:
	SV_HeldLedgerFail(l, 2);
	return 0;
}

int SV_HeldLedgerRecodeFrom(held_ledger_t *l, size_t input_offset,
	int (*rewrite)(held_record_t *, void *), void *context)
{
	held_record_t r;
	const held_value_t *value;
	unsigned char canonical[1024];
	char encoded[HELD_MAX_LINE];
	size_t offset, oldsize, length, encodedsize, canonicalsize;
	uint64_t message, execution;
	char *end;
	if (!l || !rewrite || !l->detail || !l->initialized || l->failed || !l->sealed ||
		l->pending_move_size || l->size > HELD_MAX_DETAIL || input_offset > l->capacity ||
		l->size > l->capacity-input_offset) return 0;
	oldsize = l->size;
	l->size = l->ordinal = l->have_initial = l->sealed = 0;
	if (CalcHash(&hash_sha2_256,l->ledger,32,(const unsigned char*)"FTESURF-aircell1-ledger-1",strlen("FTESURF-aircell1-ledger-1")) != 32 ||
		CalcHash(&hash_sha2_256,l->execution_ledger,32,(const unsigned char*)"FTESURF-aircell1-exec-1",strlen("FTESURF-aircell1-exec-1")) != 32) goto failure;
	for (offset = 0; offset < oldsize; offset += length)
	{
		end = memchr(l->detail+input_offset+offset,'\n',oldsize-offset);
		if (!end) goto failure;
		length = end+1-(l->detail+input_offset+offset);
		if (!SV_HeldRecordParse(l->detail+input_offset+offset,length,&r)) goto failure;
		value = SV_HeldRecordGet(&r,"message",'L'); if (!value) goto failure; message = value->l;
		value = SV_HeldRecordGet(&r,"execution",'L'); if (!value) goto failure; execution = value->l;
		if (!rewrite(&r,context) || !SV_HeldRecordBytes(&r,"generation",l->generation,16) ||
			!SV_HeldRecordL(&r,"ordinal",l->ordinal+1) || !SV_HeldRecordL(&r,"message",message) ||
			!SV_HeldRecordL(&r,"execution",execution) ||
			!Held_Encode(&r,encoded,&encodedsize,canonical,&canonicalsize) || encodedsize > length) goto failure;
		/* Shortening to base only: cannot overwrite unread relocated tape. */
		if (!SV_HeldLedgerAppend(l,&r,message,execution)) goto done;
	}
	if (!l->sealed || l->size > oldsize) goto failure;
	Held_Wipe(l->detail+l->size,HELD_MAX_DETAIL-l->size);
	Held_Wipe(&r,sizeof(r)); Held_Wipe(encoded,sizeof(encoded)); Held_Wipe(canonical,sizeof(canonical));
	return 1;
failure:
	SV_HeldLedgerFail(l,2);
done:
	Held_Wipe(&r,sizeof(r)); Held_Wipe(encoded,sizeof(encoded)); Held_Wipe(canonical,sizeof(canonical));
	return 0;
}
int SV_HeldLedgerRecode(held_ledger_t *l, int (*rewrite)(held_record_t *, void *), void *context)
{
	return SV_HeldLedgerRecodeFrom(l,0,rewrite,context);
}

static int Held_WitnessFail(held_witness_t *w)
{
	if (w)
	{
		Held_Wipe(w, sizeof(*w));
		w->failed = 1;
	}
	return 0;
}
void SV_HeldWitnessBegin(held_witness_t *w)
{
	if (!w) return;
	memset(w, 0, sizeof(*w));
	w->active = 1;
	w->previous_family = HELD_RECORD_COUNT;
	if (CalcHash(&hash_sha2_256, w->ledger, 32,
		(const unsigned char*)"FTESURF-aircell1-ledger-1", strlen("FTESURF-aircell1-ledger-1")) != 32 ||
		CalcHash(&hash_sha2_256, w->execution_ledger, 32,
		(const unsigned char*)"FTESURF-aircell1-exec-1", strlen("FTESURF-aircell1-exec-1")) != 32)
		Held_WitnessFail(w);
}
static const held_value_t *Held_WitnessValue(held_witness_t *w, const held_record_t *r,
	const char *name, char type)
{
	static const held_value_t zero = {0};
	int index = Held_Field(r, name, type);
	if (index >= 0 && r->assigned[index]) return &r->values[index];
	w->failed = 1;
	return &zero;
}

/* Structural grammar only. No physical, clock, zone or source admission here. */
int SV_HeldWitnessLine(held_witness_t *w, const char *line, size_t length)
{
	held_record_t r;
	char encoded[HELD_MAX_LINE];
	unsigned char canonical[1024], chained[32 + 2048];
	size_t textsize, canonicalsize;
	uint64_t message, execution;
	unsigned int kind, phase, ownerkind, ownerindex, boundary, i;
	int index, result = 0;
	double flags;
	held_record_family_t family = HELD_RECORD_COUNT;
#define WL(n) (Held_WitnessValue(w, &r, n, 'L')->l)
#define WU(n) (Held_WitnessValue(w, &r, n, 'U')->u)
#define WI(n) (Held_WitnessValue(w, &r, n, 'I')->i)
#define WF(n) (Held_WitnessValue(w, &r, n, 'F')->f)
#define WB(n,t) (Held_WitnessValue(w, &r, n, t)->bytes)
	if (!w) return 0;
	if (!w->active || w->failed || w->status || w->records >= HELD_MAX_RECORDS ||
		length > HELD_MAX_DETAIL - w->size) goto malformed;
	if (!SV_HeldRecordParse(line, length, &r)) goto malformed;
	family = r.family;
	if (!w->records) memcpy(w->generation, WB("generation", 'G'), 16);
	else if (memcmp(w->generation, WB("generation", 'G'), 16)) goto malformed;
	w->size += length;
	++w->records;
	if (family == HELD_RECORD_HFAIL)
	{
		kind = WU("status");
		if (w->records != 1 || (kind != 1 && kind != 2)) goto malformed;
		w->status = kind;
		goto success;
	}
	if (WL("ordinal") != w->records || (w->records == 1 && family != HELD_RECORD_HCTX)) goto malformed;
	message = w->depth && w->scope_kind[0] == 1 ? w->scope_message[0] : 0;
	if (family == HELD_RECORD_HSCOPE && WU("kind") == 1 && WU("phase") == 1)
		message = w->message + 1;
	if (WL("message") != message) goto malformed;
	if (w->pending && w->depth && !(family == HELD_RECORD_HSCOPE && WU("kind") == 1 && WU("phase") == 2)) goto malformed;
	execution = WL("execution");
	switch (family)
	{
	case HELD_RECORD_HSCOPE: case HELD_RECORD_HPOST: case HELD_RECORD_HPEND:
	case HELD_RECORD_HSEAL: case HELD_RECORD_HFREEZE: case HELD_RECORD_HTHAW: case HELD_RECORD_HTERMINAL:
		if (execution) goto malformed;
		break;
	case HELD_RECORD_HENTRY: case HELD_RECORD_HRELEASE: case HELD_RECORD_HMAINT: case HELD_RECORD_HOBS:
		if (execution != (w->callback == 1 ? w->execution + 1 : 0)) goto malformed;
		break;
	default: break;
	}
	index = Held_Field(&r, "clock_flags", 'F');
	if (index >= 0)
	{
		flags = r.values[index].f;
		if (flags < 0 || flags > 4294967295.0 || flags != (double)(uint32_t)flags ||
			!!((uint32_t)flags & 131072u) != !!w->nonce_acked) goto malformed;
	}
	if (family == HELD_RECORD_HSEAL)
	{
		static const struct { const char *name; size_t offset; } counters[] = {
#define WC(n) {#n, offsetof(held_witness_t, n)}
			WC(pre_enters), WC(pre_exits), WC(movers), WC(completions), WC(post_enters), WC(post_exits),
			WC(commands), WC(scene_fences), WC(control_events)
#undef WC
		};
		if (w->depth || w->callback || w->maintenance || w->stage || !w->terminal || w->observations != 4 ||
			w->pending_move_size || WL("first_execution") != 1 || WL("last_execution") != w->execution ||
			WL("executions") != w->execution || WL("messages") != w->message ||
			WL("effect_dispatches") || WU("qualification") != 1 ||
			memcmp(WB("ledger", 'D'), w->ledger, 32) ||
			memcmp(WB("execution_ledger", 'D'), w->execution_ledger, 32) ||
			w->initialized != 1 || w->entered != 1 || w->released != 1 || w->frozen != 1 || w->thawed != 1 ||
			!w->post_thaw_moves) goto malformed;
		for (i = 0; i < countof(counters); ++i)
		{
			uint64_t count;
			memcpy(&count, (const char*)w + counters[i].offset, sizeof(count));
			if (WL(counters[i].name) != count) goto malformed;
		}
		w->status = 3;
		goto success;
	}
	if (!Held_Encode(&r, encoded, &textsize, canonical, &canonicalsize)) goto malformed;
	memcpy(chained, w->ledger, 32);
	memcpy(chained + 32, canonical, canonicalsize);
	if (CalcHash(&hash_sha2_256, w->ledger, 32, chained, 32 + canonicalsize) != 32) goto malformed;
	if (w->terminal)
	{
		if (family != HELD_RECORD_HPOST && family != HELD_RECORD_HPEND && family != HELD_RECORD_HSCOPE) goto malformed;
		if ((family == HELD_RECORD_HPOST || family == HELD_RECORD_HSCOPE) && WU("phase") != 2) goto malformed;
	}
	switch (family)
	{
	case HELD_RECORD_HCTX:
		if (w->records != 1 || WU("context") != 1 || execution || WU("rules") != 1) goto malformed;
		break;
	case HELD_RECORD_HSCOPE:
		kind = WU("kind"); phase = WU("phase");
		if (kind < 1 || kind > 4 || phase < 1 || phase > 2 || w->callback || w->stage) goto malformed;
		if (kind != 1) ++w->scene_fences;
		if (phase == 1)
		{
			if (kind == 1)
			{
				if (w->depth || w->message >= HELD_MAX_PIECES) goto malformed;
				++w->message;
				w->pending = 0;
			}
			else if (kind == 2)
			{
				if (w->depth) goto malformed;
			}
			else
			{
				if (w->depth != 1 || w->scope_kind[0] != 1) goto malformed;
				++w->commands;
			}
			if (w->depth >= countof(w->scope_kind)) goto malformed;
			w->scope_kind[w->depth] = kind;
			w->scope_message[w->depth++] = message;
		}
		else
		{
			if (!w->depth || w->scope_kind[w->depth-1] != kind || w->scope_message[w->depth-1] != message) goto malformed;
			--w->depth;
			if (kind == 1)
			{
				if (!w->pending) goto malformed;
				w->pending = 0;
			}
			if (kind == 4) w->nonce_acked = 1;
		}
		break;
	case HELD_RECORD_HINIT:
		if (w->initialized || w->execution || w->callback || w->stage || w->depth != 1 ||
			w->scope_kind[0] != 1 || execution != 1 || WU("context") != 1) goto malformed;
		w->initialized = 1;
		break;
	case HELD_RECORD_HPRE:
		++w->scene_fences;
		if (!w->initialized || !w->depth || w->scope_kind[w->depth-1] != 1 || execution != w->execution + 1) goto malformed;
		phase = WU("phase");
		if (phase == 1 && !w->callback && !w->stage)
			{w->callback = 1; w->maintenance = 0; ++w->pre_enters;}
		else if (phase == 2 && w->callback == 1)
		{
			if (w->maintenance != (w->entered && !w->released ? 1u:0u)) goto malformed;
			w->callback = 0; w->maintenance = 0; w->stage = 1; ++w->pre_exits;
		}
		else goto malformed;
		break;
	case HELD_RECORD_HMOVE: case HELD_RECORD_HORDINARY:
		if (w->stage != 1 || w->callback || execution != w->execution + 1 || WU("context") != 1 ||
			w->execution >= HELD_MAX_PIECES || WU("pmtype") != (w->entered && !w->released ? 5u : 0u)) goto malformed;
		if (family == HELD_RECORD_HMOVE && (WF("effective_msec") > 50 || WF("gamespeed") != 1)) goto malformed;
		ownerkind = WU("ownerkind"); ownerindex = WU("ownerindex");
		if (ownerkind < 1 || ownerkind > 3 || (ownerkind == 3 && (ownerindex || family != HELD_RECORD_HMOVE))) goto malformed;
		if ((WU("pmtype") == 5) != (ownerkind == 3)) w->alias_route_bad = 1;
		if (ownerkind != 3)
		{
			for (i = 0; i < w->alias_count; ++i)
				if (w->alias_kind[i] == ownerkind && w->alias_index[i] == ownerindex) goto malformed;
			if (w->alias_count >= HELD_MAX_PIECES) goto malformed;
			w->alias_kind[w->alias_count] = ownerkind;
			w->alias_index[w->alias_count++] = ownerindex;
		}
		++w->execution; ++w->movers;
		if (w->thawed) ++w->post_thaw_moves;
		if (w->pending_move_size) goto malformed;
		memcpy(w->pending_move, canonical, canonicalsize);
		w->pending_move_size = canonicalsize;
		w->stage = 2;
		break;
	case HELD_RECORD_HDONE:
		if (w->stage != 2 || execution != w->execution || !w->pending_move_size) goto malformed;
		memcpy(chained, w->execution_ledger, 32);
		memcpy(chained + 32, w->pending_move, w->pending_move_size);
		memcpy(chained + 32 + w->pending_move_size, canonical, canonicalsize);
		if (CalcHash(&hash_sha2_256, w->execution_ledger, 32, chained, 32 + w->pending_move_size + canonicalsize) != 32) goto malformed;
		Held_Wipe(w->pending_move, sizeof(w->pending_move));
		w->pending_move_size = 0;
		++w->completions; w->stage = 0;
		break;
	case HELD_RECORD_HPOST:
		++w->scene_fences;
		if (!w->initialized || !w->depth || w->scope_kind[w->depth-1] != 1 || w->stage) goto malformed;
		phase = WU("phase");
		if (phase == 1 && !w->callback) {w->callback = 2; w->maintenance = 0; ++w->post_enters;}
		else if (phase == 2 && w->callback == 2)
		{
			if (w->maintenance != (w->entered && !w->released ? 6u:0u)) goto malformed;
			w->callback = 0; w->maintenance = 0; ++w->post_exits;
		}
		else goto malformed;
		break;
	case HELD_RECORD_HENTRY:
		if (w->entered || !w->execution || w->observations != 1 || WI("saved_movetype") != 3 ||
			w->callback || !w->depth || w->scope_kind[w->depth-1] != 3 ||
			w->previous_family != HELD_RECORD_HOBS || w->previous_boundary != 1) goto malformed;
		w->entered = 1; ++w->control_events; ++w->scene_fences;
		break;
	case HELD_RECORD_HRELEASE:
		if (!w->entered || w->released || WI("restored_movetype") != 3 || w->callback ||
			!w->depth || w->scope_kind[w->depth-1] != 3) goto malformed;
		w->released = 1; ++w->control_events; ++w->scene_fences;
		break;
	case HELD_RECORD_HMAINT:
		kind = WU("site");
		if (!w->entered || w->released || kind < 1 || kind > 3 || w->callback != (kind == 1 ? 1u : 2u) ||
			w->maintenance != (kind == 3 ? 2u:0u) || (kind == 3 && !w->frozen)) goto malformed;
		w->maintenance |= 1u << (kind-1);
		++w->control_events; ++w->scene_fences;
		break;
	case HELD_RECORD_HFREEZE:
		if (!w->entered || w->frozen || w->callback != 2 || w->maintenance != 2) goto malformed;
		w->frozen = 1; ++w->control_events;
		break;
	case HELD_RECORD_HTHAW:
		if (!w->released || !w->frozen || w->thawed || w->callback != 2) goto malformed;
		w->thawed = 1; ++w->control_events;
		break;
	case HELD_RECORD_HOBS:
		boundary = WU("boundary");
		if (boundary != w->observations + 1 || (w->observations == 1 && !w->released) ||
			(w->observations >= 2 && !w->thawed)) goto malformed;
		if (w->observations < 2)
		{
			if (w->callback || !w->depth || w->scope_kind[w->depth-1] != 3 ||
				(w->observations == 1 && w->previous_family != HELD_RECORD_HRELEASE)) goto malformed;
		}
		else if (w->callback != 2 || (w->observations == 2 && w->previous_family != HELD_RECORD_HTHAW)) goto malformed;
		++w->observations;
		break;
	case HELD_RECORD_HPEND:
		if (w->callback || w->stage || !w->depth || w->scope_kind[w->depth-1] != 1 || w->pending ||
			WL("last_execution") != w->execution) goto malformed;
		w->pending = 1;
		break;
	case HELD_RECORD_HTERMINAL:
		if (w->terminal || WU("closekind") != 1 || !w->thawed || w->observations != 4 || w->callback != 2 ||
			w->previous_family != HELD_RECORD_HOBS || w->previous_boundary != 4) goto malformed;
		w->terminal = 1;
		break;
	default: goto malformed;
	}
success:
	if (w->failed) goto malformed;
	w->previous_family = family;
	w->previous_boundary = family == HELD_RECORD_HOBS ? WU("boundary") : 0;
	result = 1;
	goto cleanup;
malformed:
	Held_WitnessFail(w);
cleanup:
	Held_Wipe(&r, sizeof(r));
	Held_Wipe(encoded, sizeof(encoded));
	Held_Wipe(canonical, sizeof(canonical));
	Held_Wipe(chained, sizeof(chained));
	return result;
#undef WL
#undef WU
#undef WI
#undef WF
#undef WB
}
int SV_HeldWitnessFinish(held_witness_t *w)
{
	if (!w || !w->active || w->failed || !w->status) return Held_WitnessFail(w);
	w->active = 0;
	return 1;
}
int SV_HeldWitnessParse(const char *data, size_t size, held_witness_t *w)
{
	const char *cursor = data, *end, *newline;
	SV_HeldWitnessBegin(w);
	if (!w || w->failed || !data || !size || size > HELD_MAX_DETAIL) return Held_WitnessFail(w);
	end = data + size;
	while (cursor < end)
	{
		newline = memchr(cursor, '\n', end - cursor);
		if (!newline || !SV_HeldWitnessLine(w, cursor, newline + 1 - cursor)) return Held_WitnessFail(w);
		cursor = newline + 1;
	}
	return SV_HeldWitnessFinish(w);
}

static int Held_White(unsigned char c)
{
	return c == ' ' || (c >= '\t' && c <= '\r');
}
static int Held_TagIs(const char *tag, size_t length, const char *expected)
{
	return length == strlen(expected) && !memcmp(tag, expected, length);
}
typedef struct
{
	held_reader_t read;
	void *context;
	size_t offset, size, position, count;
	unsigned char cache[4096];
} held_read_cursor_t;

static size_t Held_ReadLine(held_read_cursor_t *c, char line[HELD_MAX_LINE])
{
	size_t length = 0, count;
	unsigned char byte;
	while (c->offset < c->size && length < HELD_MAX_LINE)
	{
		if (c->position == c->count)
		{
			count = min(sizeof(c->cache),c->size-c->offset);
			if (c->read(c->context,c->offset,c->cache,count) != count) return 0;
			c->position = 0; c->count = count;
		}
		byte = c->cache[c->position++]; ++c->offset;
		if (!byte) return 0;
		line[length++] = byte;
		if (byte == '\n') return length;
	}
	return 0;
}
int SV_HeldReadHash(held_reader_t read, void *context, size_t size, unsigned char digest[32])
{
	union {uint64_t align; unsigned char bytes[512];} hashcontext;
	unsigned char buffer[4096];
	size_t offset = 0, count;
	int result = 0;
	if (!digest) return 0;
	memset(digest,0,32);
	if (!read || hash_sha2_256.contextsize > sizeof(hashcontext.bytes) || hash_sha2_256.digestsize != 32) goto done;
	hash_sha2_256.init(hashcontext.bytes);
	while (offset < size)
	{
		count = min(sizeof(buffer),size-offset);
		if (read(context,offset,buffer,count) != count) goto done;
		hash_sha2_256.process(hashcontext.bytes,buffer,count);
		offset += count;
	}
	hash_sha2_256.terminate(digest,hashcontext.bytes); result = 1;
done:
	Held_Wipe(&hashcontext,sizeof(hashcontext)); Held_Wipe(buffer,sizeof(buffer));
	return result;
}
int SV_HeldRecordingRead(held_reader_t read, void *context, size_t size, held_witness_t *w,
	size_t *ordinary_size, held_line_check_t check, void *check_context)
{
	static const char header[] = HELD_NATIVE_REC_HEADER;
	static const char *incompatible[] = {"pause", "session", "retry", "resume", "ghost", "spec", "portal"};
	held_read_cursor_t input;
	const char *newline, *tag, *tokenend;
	char line[HELD_MAX_LINE];
	unsigned char rowkinds[HELD_MAX_PIECES];
	unsigned int body = 0, ended = 0, seeds = 0, history = 0, rows = 0, floor_seen = 0, i, family;
	uint32_t floor_packet = 0, floor_movetick = 0, first_packet = 0, first_movetick = 0;
	size_t length, line_size, suffix = 0;
	int result = 0;
	if (ordinary_size) *ordinary_size = 0;
	if (!w) return 0;
	memset(w,0,sizeof(*w)); memset(&input,0,sizeof(input));
	input.read = read; input.context = context; input.size = size;
	if (!read || size < sizeof(header)-1) goto malformed;
	line_size = Held_ReadLine(&input,line);
	if (line_size != sizeof(header)-1 ||
		(memcmp(line,header,sizeof(header)-1) && memcmp(line,HELD_PREVIOUS_NATIVE_REC_HEADER,sizeof(header)-1)) ||
		(check && !check(check_context,line,line_size))) goto malformed;
	while (input.offset < input.size)
	{
		line_size = Held_ReadLine(&input,line); if (!line_size) goto malformed;
		newline = line+line_size-1; tag = line;
		while (tag < newline && Held_White((unsigned char)*tag)) ++tag;
		tokenend = tag;
		while (tokenend < newline && !Held_White((unsigned char)*tokenend)) ++tokenend;
		length = tokenend-tag;
		if (!length) continue;
		for (family = 0; family < HELD_RECORD_COUNT; ++family)
			if (Held_TagIs(tag,length,held_schemas[family].name)) break;
		if (family < HELD_RECORD_COUNT || (body && *tag == 'h' && !Held_TagIs(tag,length,"hold")))
		{
			if (!body || !ended) goto malformed;
			suffix = input.offset-line_size;
			SV_HeldWitnessBegin(w);
			if (!SV_HeldWitnessLine(w,line,line_size)) goto malformed;
			break;
		}
		if (check && !check(check_context,line,line_size)) goto malformed;
		if (Held_TagIs(tag,length,"FTESURF-REC") || (body && Held_TagIs(tag,length,"begin"))) goto malformed;
		if (!body)
		{
			if (Held_TagIs(tag,length,"begin")) body = 1;
			continue;
		}
		if (ended && !Held_TagIs(tag,length,"split")) goto malformed;
		if (Held_TagIs(tag,length,"end")) ended = 1;
		else if (Held_TagIs(tag,length,"in") || Held_TagIs(tag,length,"fzin"))
		{
			if (!rows && floor_seen)
			{
				if (!SV_HeldInitialInputKeyParse(tag,(size_t)(newline+1-tag),&first_packet,&first_movetick) ||
					first_packet != floor_packet || first_movetick != floor_movetick) history = 1;
			}
			if (rows < HELD_MAX_PIECES) rowkinds[rows] = Held_TagIs(tag,length,"in") ? 1:2;
			if (rows <= HELD_MAX_PIECES) ++rows;
		}
		else if (Held_TagIs(tag,length,"seed"))
		{
			if (seeds < 2) ++seeds;
		}
		else if (Held_TagIs(tag,length,"ride"))
		{
			if (floor_seen || rows || seeds != 1 ||
				!SV_HeldNeutralRideParse(tag,(size_t)(newline+1-tag),&floor_packet,&floor_movetick)) history = 1;
			floor_seen = 1;
		}
		else
			for (i = 0; i < countof(incompatible); ++i)
				if (Held_TagIs(tag,length,incompatible[i])) history = 1;
	}
	if (!suffix) goto malformed;
	while (input.offset < input.size)
	{
		line_size = Held_ReadLine(&input,line);
		if (!line_size || !SV_HeldWitnessLine(w,line,line_size)) goto malformed;
	}
	if (!SV_HeldWitnessFinish(w)) goto malformed;
	if (w->status == 3)
	{
		if (seeds != 1 || history || rows > HELD_MAX_PIECES || w->alias_route_bad || w->alias_count != rows) goto malformed;
		for (i = 0; i < rows; ++i)
			if (w->alias_index[i] != i || w->alias_kind[i] != rowkinds[i]) goto malformed;
	}
	if (ordinary_size) *ordinary_size = suffix;
	result = 1;
	goto done;
malformed:
	Held_WitnessFail(w);
done:
	Held_Wipe(&input,sizeof(input)); Held_Wipe(line,sizeof(line)); Held_Wipe(rowkinds,sizeof(rowkinds));
	return result;
}
typedef struct {const char *data; size_t size;} held_memory_reader_t;
static int Held_MemoryRead(void *opaque, size_t offset, void *buffer, size_t size)
{
	held_memory_reader_t *memory = opaque;
	if (!memory->data || offset > memory->size || size > memory->size-offset || size > 4096) return 0;
	memcpy(buffer,memory->data+offset,size);
	return (int)size;
}
int SV_HeldRecordingParse(const char *data, size_t size, held_witness_t *w, size_t *ordinary_size)
{
	held_memory_reader_t memory;
	memory.data = data; memory.size = size;
	return SV_HeldRecordingRead(Held_MemoryRead,&memory,size,w,ordinary_size,NULL,NULL);
}

/* Encoder controls only: zero-filled records are not qualified witnesses. */
static int Held_LedgerTestAppend(held_ledger_t *l, held_record_family_t family,
	uint64_t message, uint64_t execution, unsigned int kind, unsigned int phase)
{
	held_record_t r;
	unsigned int field;
	SV_HeldRecordBegin(&r, family);
	for (field = 0; field < held_schemas[family].count; ++field) r.assigned[field] = 1;
	if (family == HELD_RECORD_HSCOPE)
	{
		SV_HeldRecordU(&r, "kind", kind);
		SV_HeldRecordU(&r, "phase", phase);
	}
	return SV_HeldLedgerAppend(l, &r, message, execution);
}

typedef struct
{
	size_t line, repeat, maximum, calls;
	unsigned int fault, acted;
} held_read_test_t;
static const char held_read_test_header[] = HELD_NATIVE_REC_HEADER "begin\n";
static const char held_read_test_tail[] = "end 0\nhfail 00000000000000000000000000000000 00000001\n";
static int Held_LedgerTestRead(void *opaque, size_t offset, void *buffer, size_t size)
{
	held_read_test_t *t = opaque;
	unsigned char *out = buffer;
	size_t i, position, head = sizeof(held_read_test_header)-1, body = t->line*t->repeat;
	++t->calls; if (size > t->maximum) t->maximum = size;
	if (offset > head+body+sizeof(held_read_test_tail)-1 || size > head+body+sizeof(held_read_test_tail)-1-offset) return 0;
	if (t->fault == 1 && offset >= 4096) {++t->acted; return (int)size-1;}
	for (i = 0; i < size; ++i)
	{
		position = offset+i;
		if (position < head) out[i] = held_read_test_header[position];
		else if (position < head+body)
		{
			position = (position-head)%t->line;
			out[i] = !position ? '#' : (position == t->line-1 ? '\n':'a');
		}
		else out[i] = held_read_test_tail[position-head-body];
		if (t->fault == 2 && offset+i == head+4096) {out[i] = 0; ++t->acted;}
		if (t->fault == 3 && offset+i == head+body+6) {out[i] = 'z'; ++t->acted;}
	}
	return (int)size;
}
static int Held_LedgerTestRejectLine(void *context, const char *line, size_t size)
{
	unsigned int *acted = context;
	(void)line; (void)size;
	++*acted;
	return 0;
}
static unsigned int Held_LedgerReadTest(unsigned int *checks)
{
	static const char small[] = HELD_NATIVE_REC_HEADER "begin\nend 0\nhfail 00000000000000000000000000000000 00000001\n";
	held_read_test_t t;
	held_memory_reader_t memory;
	held_witness_t w;
	unsigned char digest[32], expected[32];
	size_t size, ordinary;
	unsigned int failures = 0, fault, acted = 0;
#define READ_CHECK(x) do {++*checks; if (!(x)) ++failures;} while (0)
	memory.data = small; memory.size = sizeof(small)-1;
	READ_CHECK(SV_HeldRecordingParse(small,sizeof(small)-1,&w,&ordinary) && w.status == 1 && ordinary == sizeof(held_read_test_header)-1+6);
	READ_CHECK(CalcHash(&hash_sha2_256,expected,32,small,sizeof(small)-1) == 32 &&
		SV_HeldReadHash(Held_MemoryRead,&memory,sizeof(small)-1,digest) && !memcmp(digest,expected,32));
	READ_CHECK(!SV_HeldRecordingRead(Held_MemoryRead,&memory,sizeof(small)-1,&w,&ordinary,Held_LedgerTestRejectLine,&acted) && acted == 1);
	for (fault = 0; fault <= 3; ++fault)
	{
		memset(&t,0,sizeof(t)); t.line = 32; t.repeat = HELD_IO_BUFFER/t.line+17; t.fault = fault;
		size = sizeof(held_read_test_header)-1+t.line*t.repeat+sizeof(held_read_test_tail)-1;
		READ_CHECK(size > HELD_IO_BUFFER);
		if (!fault)
		{
			READ_CHECK(SV_HeldRecordingRead(Held_LedgerTestRead,&t,size,&w,&ordinary,NULL,NULL) && w.status == 1 && ordinary == size-48);
			READ_CHECK(SV_HeldReadHash(Held_LedgerTestRead,&t,size,digest) && t.calls > 16 && t.maximum <= 4096);
			READ_CHECK(!SV_HeldRecordingRead(Held_LedgerTestRead,&t,size-1,&w,&ordinary,NULL,NULL));
		}
		else
		{
			READ_CHECK(!SV_HeldRecordingRead(Held_LedgerTestRead,&t,size,&w,&ordinary,NULL,NULL) && t.acted);
			if (fault == 1) READ_CHECK(!SV_HeldReadHash(Held_LedgerTestRead,&t,size,digest));
		}
	}
	for (fault = 0; fault < 2; ++fault)
	{
		memset(&t,0,sizeof(t)); t.line = HELD_MAX_LINE+fault; t.repeat = 1;
		size = sizeof(held_read_test_header)-1+t.line+sizeof(held_read_test_tail)-1;
		READ_CHECK(!!SV_HeldRecordingRead(Held_LedgerTestRead,&t,size,&w,&ordinary,NULL,NULL) == !fault && t.calls >= 3);
	}
	Held_Wipe(&w,sizeof(w)); Held_Wipe(&t,sizeof(t)); Held_Wipe(digest,sizeof(digest)); Held_Wipe(expected,sizeof(expected));
#undef READ_CHECK
	return failures;
}
static int Held_LedgerTestRecode(held_record_t *record, void *reject)
{
	(void)record;
	return reject == NULL;
}
unsigned int SV_HeldLedgerTest(unsigned int *checks)
{
	held_record_t r, decoded;
	held_ledger_t l;
	char line[HELD_MAX_LINE], decoded_line[HELD_MAX_LINE];
	unsigned char canonical[1024], decoded_canonical[1024];
	size_t textsize, binarysize, decoded_textsize, decoded_binarysize;
	unsigned int failures = 0, family, field;
	*checks = 0;
#define HELD_LEDGER_CHECK(x) do { ++*checks; if (!(x)) ++failures; } while (0)
	for (family = 0; family < HELD_RECORD_COUNT; ++family)
	{
		SV_HeldRecordBegin(&r, family);
		for (field = 0; field < held_schemas[family].count; ++field) r.assigned[field] = 1;
		if (family == HELD_RECORD_HCTX) SV_HeldRecordI(&r, "ctx_stepheight", INT32_MIN);
		HELD_LEDGER_CHECK(Held_Encode(&r, line, &textsize, canonical, &binarysize));
		HELD_LEDGER_CHECK(textsize < sizeof(line) && binarysize < sizeof(canonical));
		HELD_LEDGER_CHECK(SV_HeldRecordParse(line, textsize, &decoded));
		HELD_LEDGER_CHECK(Held_Encode(&decoded, decoded_line, &decoded_textsize, decoded_canonical, &decoded_binarysize) &&
			decoded_textsize == textsize && !memcmp(line, decoded_line, textsize) &&
			decoded_binarysize == binarysize && !memcmp(canonical, decoded_canonical, binarysize));
	}
	{
#define HELD_TEST_G "00000000000000000000000000000000"
#define HELD_TEST_L "0000000000000000"
#define HELD_TEST_P "hpend " HELD_TEST_G " " HELD_TEST_L " " HELD_TEST_L " " HELD_TEST_L
		static const char *bad[] = {
			"hunknown " HELD_TEST_G "\n",
			HELD_TEST_P "\n",                         /* Missing last_execution. */
			HELD_TEST_P " " HELD_TEST_L " extra\n",
			"hpend A0000000000000000000000000000000 " HELD_TEST_L " " HELD_TEST_L " " HELD_TEST_L " " HELD_TEST_L "\n",
			"hpend " HELD_TEST_G " 000000000000000 " HELD_TEST_L " " HELD_TEST_L " " HELD_TEST_L "\n",
			"hpend " HELD_TEST_G " -000000000000001 " HELD_TEST_L " " HELD_TEST_L " " HELD_TEST_L "\n",
			"hpend " HELD_TEST_G " 10000000000000000 " HELD_TEST_L " " HELD_TEST_L " " HELD_TEST_L "\n",
			HELD_TEST_P " " HELD_TEST_L " \n",
			"hpend  " HELD_TEST_G " " HELD_TEST_L " " HELD_TEST_L " " HELD_TEST_L " " HELD_TEST_L "\n",
			HELD_TEST_P " " HELD_TEST_L "\nx",
			HELD_TEST_P " " HELD_TEST_L "\r\n",
			HELD_TEST_P " " HELD_TEST_L,
			"hscope " HELD_TEST_G " " HELD_TEST_L " " HELD_TEST_L " " HELD_TEST_L " 00000001 00000001 7f800000 00000000\n"
		};
		static const char good[] = "hpend " HELD_TEST_G " ffffffffffffffff " HELD_TEST_L " " HELD_TEST_L " " HELD_TEST_L "\n";
		static const held_record_t empty = {0};
		for (field = 0; field < countof(bad); ++field)
		{
			HELD_LEDGER_CHECK(!SV_HeldRecordParse(bad[field], strlen(bad[field]), &decoded));
			HELD_LEDGER_CHECK(!memcmp(&decoded, &empty, sizeof(decoded)));
		}
		HELD_LEDGER_CHECK(SV_HeldRecordParse(good, sizeof(good)-1, &decoded));
		HELD_LEDGER_CHECK(!SV_HeldRecordParse(good, sizeof(good), &decoded)); /* NUL is data. */
#undef HELD_TEST_G
#undef HELD_TEST_L
#undef HELD_TEST_P
	}
	SV_HeldRecordBegin(&r, HELD_RECORD_HCTX);
	for (field = 0; field < held_schemas[r.family].count; ++field) r.assigned[field] = 1;
	{
		uint32_t negativezero = 0x80000000u;
		int timing = Held_Field(&r, "ctx_ticrate", 'F');
		if (timing >= 0) memcpy(&r.values[timing].f, &negativezero, sizeof(negativezero));
	}
	HELD_LEDGER_CHECK(Held_Encode(&r, line, &textsize, canonical, &binarysize) && !SV_HeldRecordParse(line, textsize, &decoded));
	SV_HeldRecordF(&r, "ctx_ticrate", -1.0f);
	HELD_LEDGER_CHECK(Held_Encode(&r, line, &textsize, canonical, &binarysize) && !SV_HeldRecordParse(line, textsize, &decoded));
	SV_HeldRecordF(&r, "ctx_ticrate", 0.0f);
	SV_HeldRecordU(&r, "ctx_capsule", 2);
	HELD_LEDGER_CHECK(Held_Encode(&r, line, &textsize, canonical, &binarysize) && !SV_HeldRecordParse(line, textsize, &decoded));
	SV_HeldRecordBegin(&r, HELD_RECORD_HMOVE);
	HELD_LEDGER_CHECK(SV_HeldRecordF(&r, "effective_msec", 7.5f));
	HELD_LEDGER_CHECK(!SV_HeldRecordU(&r, "effective_msec", 7));
	HELD_LEDGER_CHECK(!SV_HeldRecordF(&r, "angle", 0));
	HELD_LEDGER_CHECK(!Held_Encode(&r, line, &textsize, canonical, &binarysize));
	memset(&l, 0, sizeof(l));
	HELD_LEDGER_CHECK(!SV_HeldLedgerBegin(&l, "BAD"));
	HELD_LEDGER_CHECK(SV_HeldLedgerBegin(&l, "01010101010101010101010101010101"));
	SV_HeldRecordBegin(&r, HELD_RECORD_HCTX);
	for (field = 0; field < held_schemas[r.family].count; ++field) r.assigned[field] = 1;
	HELD_LEDGER_CHECK(SV_HeldLedgerAppend(&l, &r, 0, 0));
	HELD_LEDGER_CHECK(l.ordinal == 1 && l.size > 0);
	SV_HeldRecordBegin(&r, HELD_RECORD_HINIT); /* missing whole message entry */
	HELD_LEDGER_CHECK(!SV_HeldLedgerAppend(&l, &r, 1, 1));
	HELD_LEDGER_CHECK(l.failed == 2 && !l.detail && !l.size && !l.ordinal);
	SV_HeldLedgerDiscard(&l);
	HELD_LEDGER_CHECK(!held_ledger_count && !held_ledger_memory);
	/* Empty accepted message, intervening frame and ACK command before E1. */
	HELD_LEDGER_CHECK(SV_HeldLedgerBegin(&l, "02020202020202020202020202020202"));
	HELD_LEDGER_CHECK(Held_LedgerTestAppend(&l, HELD_RECORD_HCTX, 0, 0, 0, 0));
	HELD_LEDGER_CHECK(Held_LedgerTestAppend(&l, HELD_RECORD_HSCOPE, 1, 0, 1, 1));
	HELD_LEDGER_CHECK(Held_LedgerTestAppend(&l, HELD_RECORD_HPEND, 1, 0, 0, 0));
	HELD_LEDGER_CHECK(Held_LedgerTestAppend(&l, HELD_RECORD_HSCOPE, 1, 0, 1, 2));
	HELD_LEDGER_CHECK(Held_LedgerTestAppend(&l, HELD_RECORD_HSCOPE, 0, 0, 2, 1));
	HELD_LEDGER_CHECK(Held_LedgerTestAppend(&l, HELD_RECORD_HSCOPE, 0, 0, 2, 2));
	HELD_LEDGER_CHECK(Held_LedgerTestAppend(&l, HELD_RECORD_HSCOPE, 2, 0, 1, 1));
	HELD_LEDGER_CHECK(Held_LedgerTestAppend(&l, HELD_RECORD_HSCOPE, 2, 0, 4, 1));
	HELD_LEDGER_CHECK(Held_LedgerTestAppend(&l, HELD_RECORD_HSCOPE, 2, 0, 4, 2));
	HELD_LEDGER_CHECK(Held_LedgerTestAppend(&l, HELD_RECORD_HINIT, 2, 1, 0, 0));
	HELD_LEDGER_CHECK(l.have_initial && l.ordinal == 10);
	HELD_LEDGER_CHECK(!Held_LedgerTestAppend(&l, HELD_RECORD_HINIT, 2, 1, 0, 0));
	HELD_LEDGER_CHECK(l.failed == 2 && !l.detail && !l.have_initial);
	SV_HeldLedgerDiscard(&l);
	HELD_LEDGER_CHECK(!held_ledger_count && !held_ledger_memory);
	for (family = 0; family != 2; ++family)
	{
		HELD_LEDGER_CHECK(SV_HeldLedgerBegin(&l, "03030303030303030303030303030303"));
		HELD_LEDGER_CHECK(Held_LedgerTestAppend(&l, HELD_RECORD_HCTX, 0, 0, 0, 0));
		HELD_LEDGER_CHECK(Held_LedgerTestAppend(&l, HELD_RECORD_HSCOPE, 1, 0, 1, 1));
		HELD_LEDGER_CHECK(!Held_LedgerTestAppend(&l, family ? HELD_RECORD_HSEAL : HELD_RECORD_HPRE, 1, 1, 0, 1));
		HELD_LEDGER_CHECK(l.failed == 2 && !l.detail && !l.have_initial);
		SV_HeldLedgerDiscard(&l);
		HELD_LEDGER_CHECK(!held_ledger_count && !held_ledger_memory);
	}
	/* Actual recode control: same canonical tape/seed/chains; rejection must
	 * erase the entire owned allocation and return its global budget. */
	HELD_LEDGER_CHECK(SV_HeldLedgerBegin(&l, "04040404040404040404040404040404"));
	HELD_LEDGER_CHECK(Held_LedgerTestAppend(&l,HELD_RECORD_HCTX,0,0,0,0) &&
		Held_LedgerTestAppend(&l,HELD_RECORD_HSCOPE,1,0,1,1) &&
		Held_LedgerTestAppend(&l,HELD_RECORD_HINIT,1,1,0,0) &&
		Held_LedgerTestAppend(&l,HELD_RECORD_HPEND,1,0,0,0) &&
		Held_LedgerTestAppend(&l,HELD_RECORD_HSCOPE,1,0,1,2) &&
		Held_LedgerTestAppend(&l,HELD_RECORD_HSEAL,0,0,0,0));
	memcpy(canonical,l.ledger,32); memcpy(canonical+32,l.execution_ledger,32); textsize = l.size;
	HELD_LEDGER_CHECK(SV_HeldLedgerRecode(&l,Held_LedgerTestRecode,NULL) && l.size == textsize &&
		!memcmp(canonical,l.ledger,32) && !memcmp(canonical+32,l.execution_ledger,32));
	memmove(l.detail+l.capacity-l.size,l.detail,l.size);
	HELD_LEDGER_CHECK(!SV_HeldLedgerRecodeFrom(&l,l.capacity,Held_LedgerTestRecode,NULL) && l.sealed);
	HELD_LEDGER_CHECK(SV_HeldLedgerRecodeFrom(&l,l.capacity-l.size,Held_LedgerTestRecode,NULL) && l.size == textsize &&
		!memcmp(canonical,l.ledger,32) && !memcmp(canonical+32,l.execution_ledger,32));
	HELD_LEDGER_CHECK(!SV_HeldLedgerRecode(&l,Held_LedgerTestRecode,&l) && l.failed == 2 && !l.detail && !l.capacity);
	SV_HeldLedgerDiscard(&l);
	HELD_LEDGER_CHECK(!held_ledger_count && !held_ledger_memory);
	failures += Held_LedgerReadTest(checks);
#undef HELD_LEDGER_CHECK
	return failures;
}
