#if defined(__GNUC__)
#pragma GCC optimize ("no-fast-math", "fp-contract=off")
#endif
#include "quakedef.h"
#include "com_bih.h"
#include "pr_common.h"
#include "sv_held_replay.h"
#include "sv_held_initial.h"
#include "sv_held_session.h"

static int Held_TrajectoryContext(const held_record_t *file, const held_record_t *native);
#include <errno.h>

static held_physical_t *held_physical_owner;
extern int bih_sourceclip, bih_phybrushes, bih_phyfix;
static int Held_InputWhite(unsigned char c)
{
	return c == ' ' || (c >= '\t' && c <= '\r');
}
static int Held_InputInt(const char *text, int64_t minimum, int64_t maximum, int64_t *result)
{
	const char *p = text;
	char *end;
	long long value;
	if (*p == '-' || *p == '+') ++p;
	if (!*p) return 0;
	for (; *p; ++p) if (*p < '0' || *p > '9') return 0;
	errno = 0;
	value = strtoll(text,&end,10);
	if (errno == ERANGE || *end || value < minimum || value > maximum) return 0;
	*result = value;
	return 1;
}
static int Held_InputFloat(const char *text, float minimum, float maximum, float *result)
{
	const char *p;
	char *end;
	double value;
	float scalar;
	unsigned int digit = 0;
	for (p = text; *p; ++p)
	{
		if (*p >= '0' && *p <= '9') digit = 1;
		else if (*p != '+' && *p != '-' && *p != '.' && *p != 'e' && *p != 'E') return 0;
	}
	if (!digit) return 0;
	errno = 0;
	value = strtod(text,&end);
	if (errno == ERANGE || *end || !isfinite(value) || value < minimum || value > maximum) return 0;
	scalar = (float)value;
	if (!isfinite(scalar) || (value != 0 && scalar == 0)) return 0;
	*result = scalar;
	return 1;
}
static void Held_PhysicalWipe(void *data, size_t size)
{
	volatile unsigned char *p = data;
	while (size--) *p++ = 0;
}
static unsigned int Held_InputWords(const char *line, size_t size, char words[12][64])
{
	size_t cursor = 0, start, length, i;
	unsigned int count = 0;
	if (!line || !size || size > HELD_MAX_LINE || memchr(line,0,size)) return 0;
	for (i = 0; i < size; ++i)
		if ((line[i] == '\n' && i+1 != size) ||
			(line[i] == '\r' && (i+2 != size || line[i+1] != '\n'))) return 0;
	while (cursor < size)
	{
		while (cursor < size && Held_InputWhite((unsigned char)line[cursor])) ++cursor;
		if (cursor == size) break;
		start = cursor;
		while (cursor < size && !Held_InputWhite((unsigned char)line[cursor])) ++cursor;
		length = cursor-start;
		if (count == 12 || length >= sizeof(words[0])) return 0;
		memcpy(words[count],line+start,length); words[count++][length] = 0;
	}
	return count;
}
int SV_HeldRelativeCountsParse(const char *line, size_t size, int first, held_counts_t *out)
{
	char words[12][64];
	int64_t integer;
	float values[4];
	unsigned int i;
	if (!out) return 0;
	memset(out,0,sizeof(*out));
	if (Held_InputWords(line,size,words) != 8 || strcmp(words[0],"mc") ||
		!Held_InputInt(words[1],0,16777216,&integer)) return 0;
	out->packet = (uint32_t)integer;
	if (!Held_InputInt(words[2],0,16777216,&integer)) return 0;
	out->row = (uint32_t)integer;
	if (!Held_InputInt(words[7],0,1048575,&integer) || (first && integer)) return 0;
	out->rejected = (uint32_t)integer;
	for (i = 0; i < 4; ++i)
		if (!Held_InputFloat(words[i+3],0,1048576,&values[i]) || values[i] >= 1048576 || (first && values[i])) return 0;
	out->ring[0] = values[0]; out->ring[1] = values[1];
	out->read[0] = values[2]; out->read[1] = values[3];
	return 1;
}

int SV_HeldInputParse(const char *line, size_t size, held_input_t *out)
{
	char words[12][64];
	size_t i;
	unsigned int count;
	int64_t integer;
	float scalar;
	int result = 0, angle;
	if (!out) return 0;
	memset(out,0,sizeof(*out));
	count = Held_InputWords(line,size,words);
	if (count != countof(words) || (strcmp(words[0],"in") && strcmp(words[0],"fzin"))) goto cleanup;
	out->kind = !strcmp(words[0],"in") ? 1 : 2;
	if (!Held_InputInt(words[1],0,16777216,&integer)) goto cleanup;
	out->packet = integer;
	if (!Held_InputInt(words[2],0,16777216,&integer)) goto cleanup;
	out->movetick = integer;
	if (!Held_InputFloat(words[3],0,.05f,&out->carry)) goto cleanup;
	for (i = 0; i < 3; ++i)
	{
		if (!Held_InputInt(words[4+i],-32768,32767,&integer)) goto cleanup;
		if (i == 0) out->command.forwardmove = integer;
		else if (i == 1) out->command.sidemove = integer;
		else out->command.upmove = integer;
		if (!Held_InputFloat(words[7+i],-360,360,&scalar)) goto cleanup;
		/* Existing SV_RecSim_AngleShort recovery from %.4f Source degrees. */
		angle = (int)floor(scalar*(65536/360.0)+.5);
		out->command.angles[i] = (short)(unsigned short)(angle & 0xffff);
	}
	if (!Held_InputInt(words[10],0,7,&integer)) goto cleanup;
	if (integer & 1) out->command.buttons |= BUTTON_JUMP;
	if (integer & 2) out->command.buttons |= BUTTON_DUCK;
	if (integer & 4) out->command.buttons |= BUTTON_SPEED;
	if (!Held_InputInt(words[11],0,15,&integer)) goto cleanup;
	out->flags = integer;
	result = 1;
cleanup:
	Held_PhysicalWipe(words,sizeof(words));
	if (!result) Held_PhysicalWipe(out,sizeof(*out));
	return result;
}
int SV_HeldInitialInputKeyParse(const char *line, size_t size, uint32_t *packet, uint32_t *movetick)
{
	char words[12][64];
	int64_t pk = 0, mt = 0;
	int result = Held_InputWords(line,size,words) == 12 && !strcmp(words[0],"in") &&
		Held_InputInt(words[1],0,16777216,&pk) && Held_InputInt(words[2],0,16777216,&mt);
	if (result)
	{
		if (packet) *packet = pk;
		if (movetick) *movetick = mt;
	}
	Held_PhysicalWipe(words,sizeof(words));
	return result;
}
int SV_HeldNeutralRideParse(const char *line, size_t size, uint32_t *packet, uint32_t *movetick)
{
	char words[12][64];
	int64_t integer;
	uint32_t pk = 0, mt = 0;
	unsigned int i;
	int result = 0;
	if (Held_InputWords(line,size,words) != 7 || strcmp(words[0],"ride") || strcmp(words[3],"arm")) goto cleanup;
	for (i = 1; i < 3; ++i)
		if (words[i][0] < '0' || words[i][0] > '9' || (words[i][0] == '0' && words[i][1])) goto cleanup;
	if (!Held_InputInt(words[1],0,16777216,&integer)) goto cleanup;
	pk = integer;
	if (!Held_InputInt(words[2],0,16777216,&integer)) goto cleanup;
	mt = integer;
	for (i = 4; i < 7; ++i)
		if (strcmp(words[i],"0") && strcmp(words[i],"-0")) goto cleanup;
	if (packet) *packet = pk;
	if (movetick) *movetick = mt;
	result = 1;
cleanup:
	Held_PhysicalWipe(words,sizeof(words));
	return result;
}
int SV_HeldInputEnd(const char *line, size_t size, held_input_t *out)
{
	char words[12][64];
	int64_t integer;
	int result;
	if (!out) return 0;
	memset(out,0,sizeof(*out));
	result = Held_InputWords(line,size,words) == 3 && !strcmp(words[0],"inend") &&
		Held_InputInt(words[1],0,16777216,&integer) && Held_InputFloat(words[2],0,.05f,&out->carry);
	if (result) out->movetick = integer;
	else Held_PhysicalWipe(out,sizeof(*out));
	Held_PhysicalWipe(words,sizeof(words));
	return result;
}
int SV_HeldInputMsec(const held_input_t *input, const held_input_t *successor, float tickrate, float *msec)
{
	float elapsed, result;
	if (!msec) return 0;
	*msec = 0;
	if (!input || !successor || !isfinite(tickrate) || tickrate < .001f || tickrate > .05f ||
		input->movetick > successor->movetick || successor->movetick > 16777216 ||
		!isfinite(input->carry) || !isfinite(successor->carry) ||
		input->carry < 0 || input->carry >= tickrate || successor->carry < 0 || successor->carry >= tickrate) return 0;
	/* SV_RecSim's float32 operations, not a duration reconstructed from HDONE. */
	elapsed = (successor->movetick-input->movetick)*tickrate;
	elapsed = elapsed + (successor->carry-input->carry);
	result = elapsed*1000.0f;
	if (!isfinite(result) || result < 0 || result > 50) return 0;
	*msec = result;
	return 1;
}
unsigned int SV_HeldInputTest(unsigned int *checks)
{
	static const char normal[] = "in 3 100 .0075 250 -125 0 -90.0000 359.9945 0.0000 4 0\n";
	static const char frozen[] = "fzin 3 100 0 0 0 0 0.0000 0.0000 0.0000 3 0\n";
	static const char *bad[] = {
		"in 0 0 0 0 0 0 0 0 0 0\n", "in 0 0 0 0 0 0 0 0 0 0 0 extra\n",
		"in 0 0 nan 0 0 0 0 0 0 0 0\n", "in 0 0 0 0 0 0 inf 0 0 0 0\n",
		"in 0 0 0x1p-10 0 0 0 0 0 0 0 0\n", "in 0 0 0 32768 0 0 0 0 0 0 0\n",
		"in 0 0 0 1.5 0 0 0 0 0 0 0\n", "in 0 0 0 0 0 0 0 0 0 8 0\n",
		"in 0 0 0 0 0 0 0 0 0 0 16\n", "in -1 0 0 0 0 0 0 0 0 0 0\n",
		"in 0 16777217 0 0 0 0 0 0 0 0 0\n", "in 0 0 -.001 0 0 0 0 0 0 0 0\n",
		"in 0 0 0 0 0 0 361 0 0 0 0\n", "in 999999999999999999999999 0 0 0 0 0 0 0 0 0 0\n",
		"hmove 0 0 0 0 0 0 0 0 0 0 0\n", "in 0 0 0\n0 0 0 0 0 0 0 0\n"
	};
	static const char *badfloor[] = {
		"ride 3 100 pay 0 0 0\n", "ride 3 100 arm 1 0 0\n", "ride 3 100 arm 1e-9999 0 0\n",
		"ride 3 100 arm 0.0 0 0\n", "ride 3 100 arm 0 0\n", "ride 3 100 arm 0 0 0 extra\n",
		"ride +3 100 arm 0 0 0\n", "ride 03 100 arm 0 0 0\n", "ride 3 16777217 arm 0 0 0\n"
	};
	held_input_t input, successor, empty = {0};
	held_counts_t counts;
	uint32_t packet = 0, movetick = 0;
	float msec;
	unsigned int i, failures = 0;
#define HI_TEST(x) do {++*checks; if (!(x)) ++failures;} while (0)
	*checks = 0;
#define MC_TEST(text, first, good) HI_TEST(SV_HeldRelativeCountsParse(text,sizeof(text)-1,first,&counts) == good)
	MC_TEST("mc 7 0 0 0 0 0 0\n",1,1);
	MC_TEST("mc 8 1 2 1048575.5 2 1048575.5 3\n",0,1);
	MC_TEST("mc 7 0 1 0 1 0 0\n",1,0);
	MC_TEST("mc 7 0 0 0 0 0 1\n",1,0);
	MC_TEST("mc 7 0 1048576 0 0 0 0\n",0,0);
	MC_TEST("mc 7 0 0 0 0 0 1048576\n",0,0);
	MC_TEST("mc 7 0 nan 0 0 0 0 0\n",0,0);
	MC_TEST("mc 7 0 0 0 0 0 0 extra\n",0,0);
#undef MC_TEST
	HI_TEST(SV_HeldInputParse(normal,sizeof(normal)-1,&input));
	HI_TEST(input.kind == 1 && input.packet == 3 && input.movetick == 100 && input.carry == .0075f &&
		input.command.forwardmove == 250 && input.command.sidemove == -125 && input.command.upmove == 0 &&
		input.command.buttons == BUTTON_SPEED && (unsigned short)input.command.angles[0] == 49152 &&
		(unsigned short)input.command.angles[1] == 65535 && !input.flags);
	HI_TEST(SV_HeldInputParse(frozen,sizeof(frozen)-1,&input));
	HI_TEST(input.kind == 2 && input.command.buttons == (BUTTON_JUMP|BUTTON_DUCK));
	HI_TEST(SV_HeldInputParse("in 3 100 0 0 0 0 0 0 0 0 8\n",strlen("in 3 100 0 0 0 0 0 0 0 0 8\n"),&input) && input.flags == 8);
	SV_HeldInputParse(normal,sizeof(normal)-1,&input);
	successor = input; successor.movetick++;
	HI_TEST(SV_HeldInputMsec(&input,&successor,.015f,&msec) && msec == 15);
	successor = input;
	HI_TEST(SV_HeldInputMsec(&input,&successor,.015f,&msec) && msec == 0);
	successor.carry = .0085f;
	HI_TEST(SV_HeldInputMsec(&input,&successor,.015f,&msec) && msec > 1 && msec != 1);
	successor = input; successor.movetick--;
	HI_TEST(!SV_HeldInputMsec(&input,&successor,.015f,&msec) && msec == 0);
	successor = input;
	HI_TEST(!SV_HeldInputMsec(&input,&successor,0,&msec));
	successor.movetick += 4;
	HI_TEST(!SV_HeldInputMsec(&input,&successor,.015f,&msec));
	successor = input; successor.carry = .015f;
	HI_TEST(!SV_HeldInputMsec(&input,&successor,.015f,&msec));
	Held_PhysicalWipe(&successor,sizeof(successor));
	for (i = 0; i < countof(bad); ++i)
	{
		HI_TEST(!SV_HeldInputParse(bad[i],strlen(bad[i]),&input));
		HI_TEST(!memcmp(&input,&empty,sizeof(input)));
	}
	HI_TEST(!SV_HeldInputParse(normal,sizeof(normal),&input)); /* NUL is data. */
	HI_TEST(!memcmp(&input,&empty,sizeof(input)));
	HI_TEST(SV_HeldNeutralRideParse("ride 3 100 arm 0 -0 0\n",strlen("ride 3 100 arm 0 -0 0\n"),&packet,&movetick) && packet == 3 && movetick == 100);
	HI_TEST(SV_HeldInitialInputKeyParse(normal,sizeof(normal)-1,&packet,&movetick) && packet == 3 && movetick == 100);
	HI_TEST(!SV_HeldInitialInputKeyParse(frozen,sizeof(frozen)-1,&packet,&movetick));
	for (i = 0; i < countof(badfloor); ++i)
		HI_TEST(!SV_HeldNeutralRideParse(badfloor[i],strlen(badfloor[i]),NULL,NULL));
	Held_PhysicalWipe(&input,sizeof(input));
	return failures;
#undef HI_TEST
}

void SV_HeldPhysicalEnd(held_physical_t *p)
{
	if (!p) return;
	if (held_physical_owner == p)
	{
		if (p->saved)
		{
			memcpy(&pmove, p->saved, sizeof(pmove));
			movevars = p->saved_movevars;
			bih_sourceclip = p->saved_trace[0];
			bih_phybrushes = p->saved_trace[1];
			bih_phyfix = p->saved_trace[2];
			Held_PhysicalWipe(p->saved, sizeof(*p->saved));
			BZ_Free(p->saved);
		}
		held_physical_owner = NULL;
	}
	Held_PhysicalWipe(p, sizeof(*p));
}
static int Held_PhysicalFail(held_physical_t *p, unsigned int failure)
{
	if (p)
	{
		SV_HeldPhysicalEnd(p);
		p->failure = failure;
	}
	return 0;
}
static const held_value_t *Held_PhysicalValue(held_physical_t *p, const held_record_t *r, const char *name, char type)
{
	static const held_value_t zero = {0};
	const held_value_t *v = SV_HeldRecordGet(r, name, type);
	if (v) return v;
	p->failure = 1;
	return &zero;
}
#define PF(r,n) (Held_PhysicalValue(p,r,n,'F')->f)
#define PU(r,n) (Held_PhysicalValue(p,r,n,'U')->u)
#define PI(r,n) (Held_PhysicalValue(p,r,n,'I')->i)
#define PL(r,n) (Held_PhysicalValue(p,r,n,'L')->l)
static void Held_PhysicalVector(held_physical_t *p, const held_record_t *r, const char *prefix, vec3_t value)
{
	char name[64];
	unsigned int i;
	for (i = 0; i < 3; ++i)
	{
		Q_snprintfz(name, sizeof(name), "%s_%c", prefix, 'x' + i);
		value[i] = PF(r,name);
	}
}
static int Held_PhysicalFinite(float f)
{
	uint32_t bits;
	memcpy(&bits, &f, sizeof(bits));
	return (bits & 0x7f800000u) != 0x7f800000u;
}
/* Ordinary decimal bytes are comparison targets, never state imports. */
static int Held_LegacyWord(const char **cursor, const char *end, char word[64])
{
	const char *p = *cursor;
	size_t length = 0;
	while (p < end && Held_InputWhite((unsigned char)*p)) ++p;
	if (p == end) {*cursor = p; return 0;}
	while (p < end && !Held_InputWhite((unsigned char)*p))
	{
		if ((unsigned char)*p < 33 || (unsigned char)*p > 126 || length+1 == 64) return -1;
		word[length++] = *p++;
	}
	word[length] = 0; *cursor = p;
	return 1;
}
static int Held_LegacyScalar(const held_record_t *r, const char *name, float *scalar)
{
	const held_value_t *v;
	if ((v = SV_HeldRecordGet(r,name,'F'))) *scalar = v->f;
	else if ((v = SV_HeldRecordGet(r,name,'I'))) *scalar = v->i;
	else if ((v = SV_HeldRecordGet(r,name,'U'))) *scalar = v->u;
	else return 0;
	return Held_PhysicalFinite(*scalar);
}
static int Held_LegacyPinFields(const char *cursor, const char *end, const held_record_t *context, int pmtype)
{
	unsigned char seen[SV_PMPIN_COUNT] = {0};
	char word[64], name[80], *equal;
	unsigned int i, count = 0;
	float actual, expected;
	int token;
	while ((token = Held_LegacyWord(&cursor,end,word)) == 1)
	{
		equal = strchr(word,'='); if (!equal || !equal[1]) return 0;
		*equal++ = 0;
		for (i = 0; i < SV_PMPIN_COUNT; ++i) if (!strcmp(word,sv_pmpin_names[i])) break;
		if (i == SV_PMPIN_COUNT || seen[i] || !Held_InputFloat(equal,-FLT_MAX,FLT_MAX,&actual)) return 0;
		Q_snprintfz(name,sizeof(name),"ctx_%s",word);
		if (pmtype == -2 && !strcmp(word,"pmtype"))
		{
			if (actual != PM_NORMAL && actual != PM_NONE) return 0;
			expected = actual; /* Syntax preflight, never a model-state assignment. */
		}
		else if (pmtype >= 0 && !strcmp(word,"pmtype")) expected = (float)pmtype;
		/* Appended mover10 inputs are fixed zero by the continuous native fence. */
		else if (i >= HELD_CONTEXT_PINS) expected = 0;
		else if (!Held_LegacyScalar(context,name,&expected)) return 0;
		if (memcmp(&actual,&expected,sizeof(float))) return 0;
		seen[i] = 1; ++count;
	}
	return token == 0 && count == SV_PMPIN_COUNT;
}
static int Held_LegacyPin(const char *line, size_t size, const held_record_t *context)
{
	const char *cursor, *end;
	char word[64];
	if (!line || !size || size > HELD_MAX_LINE) return 0;
	cursor = line; end = line+size;
	if (Held_LegacyWord(&cursor,end,word) != 1 || strcmp(word,"pmpin")) return 0;
	return Held_LegacyPinFields(cursor,end,context,-1);
}
static int Held_LegacyProfile(const char *line, size_t size)
{
	char words[12][64];
	float scalar;
	unsigned int i;
	int result = 0;
	/* Existing QC inprof/prof-ok subset, not a mover-version or human proof. */
	if (Held_InputWords(line,size,words) != 7 || strcmp(words[0],"inprof") || strcmp(words[1],"ok")) goto done;
	for (i = 2; i < 7; ++i) if (!Held_InputFloat(words[i],-FLT_MAX,FLT_MAX,&scalar)) goto done;
	result = 1;
done:
	Held_PhysicalWipe(words,sizeof(words));
	return result;
}
static int Held_LegacyRate(const char *line, size_t size, const held_record_t *context)
{
	char words[12][64], expected[64];
	float rate;
	int result = 0;
	/* SV_RecOpen prints this display key with QC %g, not %.9g. */
	if (Held_InputWords(line,size,words) != 2 || strcmp(words[0],"movetickrate") ||
		!Held_LegacyScalar(context,"ctx_ticrate",&rate) ||
		Q_snprintfz(expected,sizeof(expected),"%g",rate) || strcmp(words[1],expected)) goto done;
	result = 1;
done:
	Held_PhysicalWipe(words,sizeof(words)); Held_PhysicalWipe(expected,sizeof(expected));
	return result;
}
static int Held_LegacySeed(const char *line, size_t size, const held_record_t *initial)
{
	static const char *vectors[] = {"origin_x","origin_y","origin_z","velocity_x","velocity_y","velocity_z"};
	static const char *keys[] = {"surfacefriction","ducktime","ducking","ducked","msec_carry","oldbuttons","stamina",
		"rampoff","boardcount","rampcontact","srcladder","ladnx","ladny","ladnz"};
	static const char *fields[] = {"surfacefriction","ducktime","ducking","ducked","msec_carry","oldbuttons","stamina",
		"rampoff","boardcount","rampcontact","srcladder","srcladdernormal_x","srcladdernormal_y","srcladdernormal_z"};
	static const char *water[] = {"wjtime=", "wjx=", "wjy="};
	const char *cursor, *end;
	char word[64], *equal;
	float actual, expected;
	int64_t ground;
	unsigned int i;
	if (!line || !size || size > HELD_MAX_LINE) return 0;
	cursor = line; end = line+size;
	if (Held_LegacyWord(&cursor,end,word) != 1 || strcmp(word,"seed")) return 0;
	for (i = 0; i < countof(vectors); ++i)
		if (Held_LegacyWord(&cursor,end,word) != 1 || !Held_InputFloat(word,-FLT_MAX,FLT_MAX,&actual) ||
			(initial && (!Held_LegacyScalar(initial,vectors[i],&expected) || memcmp(&actual,&expected,sizeof(float))))) return 0;
	if (Held_LegacyWord(&cursor,end,word) != 1 || !Held_InputInt(word,0,1,&ground) ||
		(initial && (!Held_LegacyScalar(initial,"ground",&expected) || expected != (float)ground))) return 0;
	for (i = 0; i < countof(keys); ++i)
	{
		if (Held_LegacyWord(&cursor,end,word) != 1 || !(equal = strchr(word,'=')) || !equal[1]) return 0;
		*equal++ = 0;
		if (strcmp(word,keys[i]) || !Held_InputFloat(equal,-FLT_MAX,FLT_MAX,&actual) ||
			(initial && (!Held_LegacyScalar(initial,fields[i],&expected) || memcmp(&actual,&expected,sizeof(float))))) return 0;
	}
	/* Existing public counter is irrelevant ONLY with fenced momentum inputs0.
	 * Validate its canonical domain; never install it as physical state. */
	if (Held_LegacyWord(&cursor,end,word) != 1 || strncmp(word,"landticks=",10) ||
		word[10] < '0' || word[10] > '9' || (word[10] == '0' && word[11]) ||
		!Held_InputInt(word+10,0,PMLIM_LANDMAX,&ground)) return 0;
	/* Current upstream seed carries these even when no assist is active.
	 * M0 never imports them: require the complete canonical zero suffix. */
	for (i = 0; i < countof(water); ++i)
	{
		size_t length = strlen(water[i]);
		if (Held_LegacyWord(&cursor,end,word) != 1 || strncmp(word,water[i],length) ||
			!Held_InputFloat(word+length,0,0,&actual)) return 0;
	}
	return Held_LegacyWord(&cursor,end,word) == 0;
}

/* 0=malformed/unsupported, 1=match, 2=comparable disagreement. A NULL replay
 * requests decoding only; it supplies no model state and cannot admit a file. */
static int Held_LegacyClockEnd(const char *line, size_t size, const held_clock_replay_t *replay)
{
	const char *cursor, *end;
	char word[64];
	int64_t start, frozen;
	float carry, viewing;
	if (!line || !size || size > HELD_MAX_LINE) return 0;
	cursor = line; end = line+size;
	if (Held_LegacyWord(&cursor,end,word) != 1 || strcmp(word,"clockend") ||
		Held_LegacyWord(&cursor,end,word) != 1 || strcmp(word,"viewing") ||
		Held_LegacyWord(&cursor,end,word) != 1 || !Held_InputInt(word,0,16777216,&start) ||
		Held_LegacyWord(&cursor,end,word) != 1 || !Held_InputFloat(word,-FLT_MAX,FLT_MAX,&carry) ||
		Held_LegacyWord(&cursor,end,word) != 1 || !Held_InputInt(word,0,16777216,&frozen) ||
		Held_LegacyWord(&cursor,end,word) != 1 || !Held_InputFloat(word,0,FLT_MAX,&viewing) ||
		Held_LegacyWord(&cursor,end,word) != 0) return 0;
	if (!replay) return 1;
	if (!replay->initialized || replay->failed || replay->view_owner) return 0;
	return (float)start == replay->clock.starttick && (float)frozen == replay->clock.frzticks &&
		!memcmp(&carry,&replay->clock.tickcarry,sizeof(float)) &&
		!memcmp(&viewing,&replay->view_seconds,sizeof(float)) ? 1:2;
}
/* Output-only edge: wall/total must agree with typed-control-derived state. */
static int Held_LegacyView(const char *line, size_t size, const held_clock_replay_t *replay, int phase)
{
	const char *cursor, *end;
	char word[64];
	int64_t open;
	float wall, seconds, expected;
	if (!line || !size || size > HELD_MAX_LINE) return 0;
	cursor = line; end = line+size;
	if (Held_LegacyWord(&cursor,end,word) != 1 || strcmp(word,"viewclock") ||
		Held_LegacyWord(&cursor,end,word) != 1 || strcmp(word,"replay") ||
		Held_LegacyWord(&cursor,end,word) != 1 || !Held_InputInt(word,0,1,&open) ||
		Held_LegacyWord(&cursor,end,word) != 1 || !Held_InputFloat(word,0,FLT_MAX,&wall) ||
		Held_LegacyWord(&cursor,end,word) != 1 || !Held_InputFloat(word,0,FLT_MAX,&seconds) ||
		Held_LegacyWord(&cursor,end,word) != 0) return 0;
	if (!replay) return 1;
	if (!replay->initialized || replay->failed || !replay->view_policy || phase < 0 || phase > 1) return 0;
	expected = SV_HeldClockViewSeconds(replay);
	return open == phase && replay->view_owner == phase && isfinite(expected) &&
		!memcmp(&wall,&replay->clock.frame,sizeof(float)) &&
		!memcmp(&seconds,&expected,sizeof(float)) ? 1:2;
}
static int Held_LegacyClockMonitor(const char *line, size_t size, const held_clock_replay_t *replay)
{
	const char *cursor, *end;
	char words[8][64], word[64], expected[64];
	float values[8], observed, rise;
	int64_t integer;
	unsigned int i, mismatch = 0;
	if (!line || !size || size > HELD_MAX_LINE) return 0;
	cursor = line; end = line+size;
	if (Held_LegacyWord(&cursor,end,word) != 1 || strcmp(word,"clk")) return 0;
	for (i = 0; i < countof(values); ++i)
	{
		if (Held_LegacyWord(&cursor,end,words[i]) != 1) return 0;
		if (i == 4)
		{
			if (!Held_InputInt(words[i],0,16777216,&integer) ||
				Q_snprintfz(expected,sizeof(expected),"%d",(int)integer)) return 0;
		}
		else if (!Held_InputFloat(words[i],-FLT_MAX,FLT_MAX,&observed) ||
			Q_snprintfz(expected,sizeof(expected),"%.3f",observed)) return 0;
		if (strcmp(words[i],expected)) return 0;
	}
	if (Held_LegacyWord(&cursor,end,word) != 0) return 0;
	if (!replay) return 1;
	if (!replay->initialized || replay->failed) return 0;
	values[0] = replay->context.slow; values[1] = replay->context.hyst; values[2] = replay->context.allow;
	values[3] = replay->clock.ckwt; values[4] = replay->clock.ckgaps; values[5] = replay->clock.ckcr;
	values[6] = replay->clock.ckd; values[7] = replay->clock.cku;
	if (replay->clock.ckup)
	{
		rise = replay->clock.ckpk - replay->clock.cktr; rise = rise - replay->context.hyst;
		if (rise > 0) values[7] = values[7] + rise;
	}
	for (i = 0; i < countof(values); ++i)
	{
		if (!Held_PhysicalFinite(values[i])) return 0;
		if (i == 4)
		{
			if (values[i] < 0 || values[i] > 16777216 || values[i] != floorf(values[i]) ||
				Q_snprintfz(expected,sizeof(expected),"%d",(int)values[i])) return 0;
		}
		else if (Q_snprintfz(expected,sizeof(expected),"%.3f",values[i])) return 0;
		if (strcmp(words[i],expected)) mismatch = 1;
	}
	return mismatch ? 2:1;
}

static int Held_LegacyHold(const char *line, size_t size, uint64_t movetick, float frozen)
{
	char words[12][64];
	int64_t integer;
	return Held_InputWords(line,size,words) == 3 && !strcmp(words[0],"hold") &&
		Held_InputInt(words[1],0,16777216,&integer) && (uint64_t)integer == movetick &&
		Held_InputInt(words[2],0,16777216,&integer) && (float)integer == frozen;
}
static int Held_LegacyPinWarp(const char *line, size_t size, const playermove_t *body,
	uint64_t movetick, float packet, unsigned int input_index)
{
	char words[12][64];
	int64_t integer;
	float scalar;
	unsigned int i;
	if (!body || Held_InputWords(line,size,words) != 12 || strcmp(words[0],"warp") || strcmp(words[4],"pin") ||
		!Held_InputInt(words[1],0,16777216,&integer) || (float)integer != packet ||
		!Held_InputInt(words[2],0,16777216,&integer) || (uint64_t)integer != movetick ||
		!Held_InputInt(words[3],0,HELD_MAX_PIECES-1,&integer) || (unsigned int)integer != input_index) return 0;
	for (i = 0; i < 6; ++i)
	{
		const float *expected = i < 3 ? &body->origin[i] : &body->velocity[i-3];
		if (!Held_InputFloat(words[5+i],-FLT_MAX,FLT_MAX,&scalar) || memcmp(&scalar,expected,sizeof(float))) return 0;
	}
	return Held_InputInt(words[11],0,1,&integer) && integer == !!body->onground;
}

static int Held_LegacyEnd(const char *line, size_t size, const uint32_t expected[13], int park_field)
{
	const char *cursor, *end;
	char word[64];
	int64_t integer;
	unsigned int i;
	if (!line || !size || size > HELD_MAX_LINE) return 0;
	cursor = line; end = line+size;
	if (Held_LegacyWord(&cursor,end,word) != 1 || strcmp(word,"end")) return 0;
	for (i = 0; i < 13; ++i)
		if (Held_LegacyWord(&cursor,end,word) != 1 || !Held_InputInt(word,0,16777216,&integer) || (expected && (uint32_t)integer != expected[i])) return 0;
	/* New native15 inherits public13's trailer, but grants no parked replay. */
	if (park_field && (Held_LegacyWord(&cursor,end,word) != 1 || !Held_InputInt(word,0,0,&integer))) return 0;
	return Held_LegacyWord(&cursor,end,word) == 0;
}
static int Held_LegacyFlags(const char *line, size_t size, const held_clock_replay_t *replay)
{
	char words[12][64];
	float observed, flags;
	if (Held_InputWords(line,size,words) != 2 || strcmp(words[0],"flags") ||
		!Held_InputFloat(words[1],0,16777215,&observed) || observed != floorf(observed)) return 0;
	if (!replay) return 1; /* Whole-body syntax check, not a clock initializer. */
	if (!replay->initialized || replay->failed) return 0;
	flags = replay->clock.flags;
	if (!Held_PhysicalFinite(flags) || flags < 0 || flags > 16777215 || flags != floorf(flags)) return 0;
	return Held_InputWords(line,size,words) == 2 && !strcmp(words[0],"flags") &&
		Held_InputFloat(words[1],0,16777215,&observed) && observed == (float)((uint32_t)flags & ~18u);
}

static int Held_LegacyPM(const char *line, size_t size, const held_record_t *context,
	int pmtype, float packet, unsigned int input_index)
{
	const char *cursor, *end;
	char word[64];
	int64_t integer;
	if (!line || !size || size > HELD_MAX_LINE || (pmtype != PM_NORMAL && pmtype != PM_NONE)) return 0;
	cursor = line; end = line+size;
	if (Held_LegacyWord(&cursor,end,word) != 1 || strcmp(word,"pm") ||
		Held_LegacyWord(&cursor,end,word) != 1 || !Held_InputInt(word,0,16777216,&integer) || (float)integer != packet ||
		Held_LegacyWord(&cursor,end,word) != 1 || !Held_InputInt(word,0,HELD_MAX_PIECES-1,&integer) || (unsigned int)integer != input_index) return 0;
	return Held_LegacyPinFields(cursor,end,context,pmtype);
}
static int Held_LegacyPE(const char *line, size_t size, float packet, unsigned int input_index, unsigned int physcrc)
{
	char words[12][64];
	int64_t integer;
	return Held_InputWords(line,size,words) == 4 && !strcmp(words[0],"pe") &&
		Held_InputInt(words[1],0,16777216,&integer) && (float)integer == packet &&
		Held_InputInt(words[2],0,HELD_MAX_PIECES-1,&integer) && (unsigned int)integer == input_index &&
		Held_InputInt(words[3],0,0xffffff,&integer) && (unsigned int)integer == physcrc;
}

/* View/attack are display telemetry, not a second physical input channel. */
static int Held_LegacySamplePhysical(const char *line, size_t size, const playermove_t *body,
	float elapsed, int initial, unsigned int *failure)
{
	const char *cursor, *end;
	char words[17][64], expected[64];
	float values[17], target;
	int64_t integer;
	unsigned int i, flags, keys, physical, wanted, mismatch = 0;
	if (failure) *failure = 0;
	if (!line || !size || size > HELD_MAX_LINE || (body && !Held_PhysicalFinite(elapsed))) return 0;
	cursor = line; end = line+size;
	for (i = 0; i < countof(words); ++i)
		if (Held_LegacyWord(&cursor,end,words[i]) != 1 || !Held_InputFloat(words[i],-FLT_MAX,FLT_MAX,&values[i])) return 0;
	if (Held_LegacyWord(&cursor,end,expected) != 0 || !Held_InputInt(words[9],0,31,&integer)) return 0;
	flags = (unsigned int)integer;
	if (!Held_InputInt(words[10],0,511,&integer)) return 0;
	keys = (unsigned int)integer;
	/* Validate the complete supported target before reporting a disagreement.
	 * A malformed/display-only/unsupported row must not become a physical HOLD. */
	if (keys & ~319u) return 0;
	for (i = 0; i < 9; ++i)
		if (Q_snprintfz(expected,sizeof(expected),i == 0 ? "%.4f":(i < 7 ? "%.2f":"%.1f"),values[i]) ||
			strcmp(words[i],expected)) return 0;
	for (i = 11; i < 14; ++i)
		if (!Held_InputInt(words[i],-32768,32767,&integer)) return 0;
	wanted = (values[11] > 0 ? 1u:0u) | (values[11] < 0 ? 2u:0u) |
		(values[12] < 0 ? 4u:0u) | (values[12] > 0 ? 8u:0u) |
		(flags & 4u ? 16u:0u) | (flags & 8u ? 256u:0u) | (keys & 32u);
	if (keys != wanted) return 0;
	for (i = 14; i < 17; ++i)
		if (strcmp(words[i],"0.0000")) return 0;
	if (!body) return 1; /* Decode only; NEVER a model/admission or a state import. */
	physical = (body->onground ? 1u:0u) | (body->ducked ? 2u:0u) |
		((body->cmd.buttons & BUTTON_JUMP) ? 4u:0u) | (body->rampcontact ? 16u:0u);
	if ((flags & ~8u) != physical) mismatch = 1;
	for (i = 0; i < 7; ++i)
	{
		target = i == 0 ? elapsed : (i < 4 ? body->origin[i-1] : body->velocity[i-4]);
		if (!Held_PhysicalFinite(target) || Q_snprintfz(expected,sizeof(expected),i == 0 ? "%.4f":"%.2f",target)) return 0;
		if (strcmp(words[i],expected)) mismatch = 1;
	}
	if (!initial)
	{
		for (i = 11; i < 14; ++i)
		{
			target = i == 11 ? body->cmd.forwardmove : (i == 12 ? body->cmd.sidemove : body->cmd.upmove);
			if (values[i] != target) mismatch = 1;
		}
		if (!!(keys & 32u) != !!(body->cmd.buttons & BUTTON_DUCK)) mismatch = 1;
	}
	if (mismatch) {if (failure) *failure = 2; return 0;}
	return 1;
}

static int Held_PhysicalTrace(held_physical_t *p, const held_record_t *r,
	const char *cvarname, const char *field, const char *rawfield)
{
	cvar_t *var = Cvar_FindVar(cvarname);
	return var && PF(r,field) == var->value && PI(r,rawfield) == var->ival;
}
static int Held_PhysicalAir(held_physical_t *p)
{
	vec3_t mins, maxs;
	unsigned int i;
	if (!p->world || p->world->loadstate != MLS_LOADED || !(p->world->engineflags & MDLF_SOURCEBSP)) return 0;
	for (i = 0; i < 3; ++i)
	{
		mins[i] = pmove.origin[i] - (i == 2 ? 3 : 273);
		maxs[i] = pmove.origin[i] + (i == 2 ? 129 : 273);
	}
	return BIH_HeldAirBoxEmpty(p->world, mins, maxs);
}
static int Held_PhysicalState(held_physical_t *p)
{
	unsigned int i;
	if (p->failure || pmove.pm_type != (p->pinned ? PM_NONE : PM_NORMAL) || pmove.onground ||
		pmove.waterlevel || pmove.watertype != FTECONTENTS_EMPTY || pmove.numtouch || pmove.portalcrossings ||
		pmove.onladder || pmove.srcladder || pmove.capsule || pmove.ducked || pmove.ducking ||
		pmove.jump_held || (pmove.oldbuttons & BUTTON_JUMP) || pmove.waterjumptime ||
		pmove.boardcount || pmove.rampcontact || pmove.momlimit || pmove.momcool || pmove.momslide ||
		pmove.landticks < 0 || pmove.landticks > PMLIM_LANDMAX ||
		pmove.waterjumpremaining != 0 || pmove.waterjumpdir[0] != 0 || pmove.waterjumpdir[1] != 0 ||
		pmove.numphysent != 1 || pmove.skipent != -1 ||
		pmove.physents[0].model != p->world || pmove.gravitydir[0] || pmove.gravitydir[1] || pmove.gravitydir[2] != -1 ||
		pmove.player_mins[0] != -16 || pmove.player_mins[1] != -16 || pmove.player_mins[2] ||
		pmove.player_maxs[0] != 16 || pmove.player_maxs[1] != 16 || pmove.player_maxs[2] != 62 ||
		(pmove.surfacefriction != 1 && pmove.surfacefriction != .25f) ||
		!Held_PhysicalFinite(pmove.ducktime) || pmove.ducktime < 0 || pmove.ducktime > 1000 ||
		!Held_PhysicalFinite(pmove.stamina) || pmove.stamina < 0 ||
		!Held_PhysicalFinite(pmove.rampoff) || pmove.rampoff < 0 ||
		!Held_PhysicalFinite(pmove.msec_carry) || pmove.msec_carry < 0 || pmove.msec_carry >= movevars.ticrate ||
		pmove.tickused != movevars.ticrate || pmove.viewheight != movevars.viewheight) return 0;
	for (i = 0; i < 3; ++i)
		if (!Held_PhysicalFinite(pmove.origin[i]) || !Held_PhysicalFinite(pmove.velocity[i]) ||
			fabsf(pmove.velocity[i]) >= p->svmaxvel || fabsf(pmove.velocity[i]) >= movevars.maxvelocity ||
			pmove.basevelocity[i] || pmove.portaloffset[i] || pmove.srcladdernormal[i]) return 0;
	return 1;
}
int SV_HeldPhysicalBegin(held_physical_t *p, const held_record_t *ctx, const held_record_t *initial, model_t *world)
{
	static const char *unsigned_pins[] = {"pmsrcver", "physmode", "pmtype", "capsule", "walljump", "slidefix",
		"airstep", "pground", "stepdown", "slidyslopes", "autobunny", "bunnyfriction"};
	float pins[SV_PMPIN_COUNT];
	char name[64];
	unsigned int i, j, coordtype, flags;
	int unsigned_pin;
	if (p && held_physical_owner == p) return Held_PhysicalFail(p, 1);
	if (!p || held_physical_owner || p->saved || p->failure || !ctx || !initial ||
		PMSRC_VERSION != HELD_SOURCE_VERSION || SV_PMPIN_COUNT != HELD_CONTEXT_PINS+3 ||
		ctx->family != HELD_RECORD_HCTX || initial->family != HELD_RECORD_HINIT ||
		!world || !sv.state || world != sv.world.worldmodel || world->loadstate != MLS_LOADED ||
		!(world->engineflags & MDLF_SOURCEBSP)) return 0;
	if (PU(ctx,"context") != 1 || PU(ctx,"rules") != 1 || PU(initial,"context") != 1 ||
		PL(initial,"execution") != 1 || PU(ctx,"ctx_numphysent") != 1 || PI(ctx,"ctx_hullnum") ||
		(PI(ctx,"ctx_geomtype") != 0 && PI(ctx,"ctx_geomtype") != GEOMTYPE_BOX) ||
		PF(ctx,"ctx_hullmins_z") || PF(ctx,"ctx_hullmaxs_z") != 62 ||
		PF(ctx,"ctx_gravitydir_x") || PF(ctx,"ctx_gravitydir_y") || PF(ctx,"ctx_gravitydir_z") != -1 ||
		PF(ctx,"ctx_gamespeed") != 1 ||
		!Held_PhysicalTrace(p,ctx,"pm_trisoup_bevels","ctx_trisoup","ctx_trisoup_i") ||
		!Held_PhysicalTrace(p,ctx,"pm_rotatedboxhulls","ctx_rotboxes","ctx_rotboxes_i") ||
		!Held_PhysicalTrace(p,ctx,"pm_portalcsg_scanall","ctx_portalcsg","ctx_portalcsg_i")) goto unsupported;
	coordtype = PU(ctx,"ctx_coordtype"); flags = PU(ctx,"ctx_flags");
	if (coordtype > 255 || (flags & ~(MOVEFLAG_VALID|MOVEFLAG_NOGRAVITYONGROUND|MOVEFLAG_QWEDGEBOX)) ||
		!(flags & MOVEFLAG_VALID) || !(flags & MOVEFLAG_NOGRAVITYONGROUND)) goto unsupported;
	memset(pins,0,sizeof(pins)); /* Additional native inputs are known-zero predicates. */
	for (i = 0; i < HELD_CONTEXT_PINS; ++i)
	{
		Q_snprintfz(name, sizeof(name), "ctx_%s", sv_pmpin_names[i]);
		unsigned_pin = 0;
		for (j = 0; j < countof(unsigned_pins); ++j)
			if (!strcmp(sv_pmpin_names[i], unsigned_pins[j])) unsigned_pin = 1;
		if (unsigned_pin)
		{
			uint32_t word = PU(ctx,name);
			if (word > 16777216) goto unsupported;
			pins[i] = (float)word;
		}
		else if (!strcmp(sv_pmpin_names[i], "stepheight"))
		{
			int32_t word = PI(ctx,name);
			if (word < -16777216 || word > 16777216) goto unsupported;
			pins[i] = (float)word;
		}
		else pins[i] = PF(ctx,name);
		if (!Held_PhysicalFinite(pins[i])) goto unsupported;
	}
	if (p->failure) goto unsupported;
	p->saved = BZF_Malloc(sizeof(pmove));
	if (!p->saved) goto unsupported;
	memcpy(p->saved, &pmove, sizeof(pmove));
	p->saved_movevars = movevars;
	p->saved_trace[0] = bih_sourceclip;
	p->saved_trace[1] = bih_phybrushes;
	p->saved_trace[2] = bih_phyfix;
	p->world = world;
	held_physical_owner = p;
	memset(&pmove, 0, sizeof(pmove));
	memset(&movevars, 0, sizeof(movevars));
	SV_PMPinApply(pins);
	movevars.coordtype = coordtype;
	movevars.flags = flags;
	p->svmaxvel = PF(ctx,"ctx_svmaxvel");
	if (PMSRC_VERSION != HELD_SOURCE_VERSION || SV_PMPIN_COUNT != HELD_CONTEXT_PINS+3 ||
		movevars.sourceversion != HELD_SOURCE_VERSION ||
		movevars.physicsmode != PHYSMODE_SOURCE || movevars.ticrate < .001f || movevars.ticrate > .05f ||
		(movevars.entgravity != 0 && movevars.entgravity != 1) || movevars.gravity <= 0 || movevars.gravity > 10000 ||
		movevars.maxspeed <= 0 || movevars.maxvelocity <= 0 || p->svmaxvel <= 0 ||
		movevars.standheight != 62 || movevars.duckheight != 45 || movevars.viewscale != .5f ||
		movevars.groundtracedist <= 0 || movevars.groundtracedist > 2 || pmove.capsule || pmove.pm_type != PM_NORMAL)
		goto unsupported;
	Held_PhysicalVector(p, initial, "origin", pmove.origin);
	Held_PhysicalVector(p, initial, "velocity", pmove.velocity);
	Held_PhysicalVector(p, initial, "safeorigin", pmove.safeorigin);
	Held_PhysicalVector(p, initial, "hullmins", pmove.player_mins);
	Held_PhysicalVector(p, initial, "hullmaxs", pmove.player_maxs);
	Held_PhysicalVector(p, initial, "gravitydir", pmove.gravitydir);
	Held_PhysicalVector(p, initial, "basevelocity", pmove.basevelocity);
	Held_PhysicalVector(p, initial, "srcladdernormal", pmove.srcladdernormal);
	Held_PhysicalVector(p, initial, "portaloffset", pmove.portaloffset);
	pmove.safeorigin_known = PU(initial,"safeorigin_known");
	pmove.onground = PU(initial,"ground"); pmove.groundent = PI(initial,"groundent");
	pmove.pm_type = PU(initial,"pmtype"); pmove.jump_held = PU(initial,"jump_held");
	pmove.jump_msec = PI(initial,"jump_msec"); pmove.waterjumptime = PF(initial,"waterjumptime");
	pmove.onladder = PU(initial,"onladder"); pmove.capsule = PU(initial,"capsule");
	pmove.surfacefriction = PF(initial,"surfacefriction"); pmove.ducktime = PF(initial,"ducktime");
	pmove.ducking = PU(initial,"ducking"); pmove.ducked = PU(initial,"ducked");
	pmove.msec_carry = PF(initial,"msec_carry"); pmove.oldbuttons = PI(initial,"oldbuttons");
	pmove.stamina = PF(initial,"stamina"); pmove.rampoff = PF(initial,"rampoff");
	pmove.boardcount = PF(initial,"boardcount"); pmove.rampcontact = PF(initial,"rampcontact");
	pmove.srcladder = PU(initial,"srcladder"); pmove.viewheight = PF(initial,"viewheight");
	pmove.waterlevel = PU(initial,"waterlevel"); pmove.watertype = PI(initial,"watertype");
	pmove.tickused = PF(initial,"tickused"); pmove.portalcrossings = PU(initial,"portalcrossings");
	pmove.numtouch = PU(initial,"numtouch");
	p->raw_moveticks = PL(initial,"raw_moveticks");
	pmove.world = &sv.world; pmove.numphysent = 1; pmove.physents[0].model = world; pmove.skipent = -1;
	if (pmove.safeorigin_known || pmove.jump_msec || p->raw_moveticks > 16777216 || !Held_PhysicalState(p)) goto unsupported;
	return 1;
unsupported:
	return Held_PhysicalFail(p, 1);
}
int SV_HeldPhysicalStep(held_physical_t *p, const held_record_t *r, const usercmd_t *ordinary, float ordinary_msec)
{
	float msec;
	unsigned int ownerkind, i, guarded;
	if (!p || held_physical_owner != p || !r || p->pending || p->failure || p->calls >= HELD_MAX_PIECES ||
		(r->family != HELD_RECORD_HMOVE && r->family != HELD_RECORD_HORDINARY))
		return p && held_physical_owner == p ? Held_PhysicalFail(p, 1) : 0;
	ownerkind = PU(r,"ownerkind");
	if (PU(r,"context") != 1 || PU(r,"pmtype") != (unsigned int)pmove.pm_type ||
		PL(r,"execution") != p->calls + 1 || ownerkind < 1 || ownerkind > 3 ||
		(p->pinned != (ownerkind == 3)) || (ownerkind == 3 && ordinary) || (ownerkind != 3 && !ordinary)) goto unsupported;
	msec = r->family == HELD_RECORD_HMOVE ? PF(r,"effective_msec") : ordinary_msec;
	if (r->family == HELD_RECORD_HMOVE && PF(r,"gamespeed") != 1) goto unsupported;
	if (!Held_PhysicalFinite(msec) || msec < 0 || msec > 50 || !Held_PhysicalState(p)) goto unsupported;
	guarded = p->pinned || (p->released && !p->guard_done);
	if (guarded && !Held_PhysicalAir(p)) goto unsupported;
	if (ordinary) pmove.cmd = *ordinary;
	else memset(&pmove.cmd, 0, sizeof(pmove.cmd)); /* Model representative, NOT retained held input. */
	if (pmove.cmd.buttons & (BUTTON_JUMP|BUTTON_DUCK)) goto unsupported;
	pmove.cmd.msec = msec;
	for (i = 0; i < 3; ++i) pmove.angles[i] = SHORT2ANGLE(pmove.cmd.angles[i]);
	pmove.onladder = false; pmove.jump_msec = 0; pmove.skipent = -1;
	PM_PlayerMove(1.0f);
	if (pmove.ticksrun > 16777216u || p->raw_moveticks > 16777216u - pmove.ticksrun) goto unsupported;
	p->raw_moveticks += pmove.ticksrun;
	++p->calls; p->pending = 1;
	if (!Held_PhysicalState(p) || (guarded && !Held_PhysicalAir(p))) goto unsupported;
	if (p->thawed && !p->pinned) p->guard_done = 1;
	return 1;
unsupported:
	return Held_PhysicalFail(p, 1);
}
int SV_HeldPhysicalDone(held_physical_t *p, const held_record_t *r)
{
	float carry;
	uint64_t execution, rawticks;
	unsigned int ticks;
	if (!p || held_physical_owner != p || !r || r->family != HELD_RECORD_HDONE || !p->pending)
		return p && held_physical_owner == p ? Held_PhysicalFail(p, 1) : 0;
	carry = PF(r,"carry"); execution = PL(r,"execution");
	rawticks = PL(r,"raw_moveticks"); ticks = PU(r,"ticksrun");
	if (p->failure) return Held_PhysicalFail(p, 1);
	if (execution != p->calls || ticks != pmove.ticksrun || rawticks != p->raw_moveticks ||
		memcmp(&carry, &pmove.msec_carry, sizeof(carry)))
		return Held_PhysicalFail(p, 2);
	p->pending = 0;
	return 1;
}
int SV_HeldPhysicalControl(held_physical_t *p, const held_record_t *r)
{
	unsigned int site;
	if (!p || held_physical_owner != p || !r || p->failure || p->pending)
		return p && held_physical_owner == p ? Held_PhysicalFail(p, 1) : 0;
	switch (r->family)
	{
	case HELD_RECORD_HENTRY:
		if (p->entered || !p->calls || PI(r,"saved_movetype") != MOVETYPE_WALK || !Held_PhysicalState(p) ||
			(!pmove.velocity[0] && !pmove.velocity[1] && !pmove.velocity[2]) || !Held_PhysicalAir(p)) goto unsupported;
		p->entered = p->pinned = 1;
		pmove.pm_type = PM_NONE;
		VectorClear(pmove.velocity); VectorClear(pmove.basevelocity);
		break;
	case HELD_RECORD_HRELEASE:
		if (!p->pinned || p->released || PI(r,"restored_movetype") != MOVETYPE_WALK) goto unsupported;
		p->pinned = 0; p->released = 1; pmove.pm_type = PM_NORMAL;
		VectorClear(pmove.velocity); VectorClear(pmove.basevelocity);
		break;
	case HELD_RECORD_HMAINT:
		site = PU(r,"site");
		if (!p->pinned || site < 1 || site > 3) goto unsupported;
		if (site != 3) pmove.pm_type = PM_NONE;
		else {VectorClear(pmove.velocity); VectorClear(pmove.basevelocity);}
		break;
	case HELD_RECORD_HTHAW:
		if (!p->released || p->thawed) goto unsupported;
		p->thawed = 1;
		break;
	default: goto unsupported;
	}
	if (!Held_PhysicalState(p)) goto unsupported;
	return 1;
unsupported:
	return Held_PhysicalFail(p, 1);
}
static int Held_PhysicalCompareF(const held_record_t *r, const char *name, float actual)
{
	const held_value_t *v = SV_HeldRecordGet(r, name, 'F');
	return v && !memcmp(&v->f, &actual, sizeof(actual));
}
static int Held_PhysicalCompareV(const held_record_t *r, const char *prefix, const vec3_t actual)
{
	char name[64];
	unsigned int i;
	for (i = 0; i < 3; ++i)
	{
		Q_snprintfz(name, sizeof(name), "%s_%c", prefix, 'x' + i);
		if (!Held_PhysicalCompareF(r, name, actual[i])) return 0;
	}
	return 1;
}
int SV_HeldPhysicalObserve(held_physical_t *p, const held_record_t *r)
{
	if (!p || held_physical_owner != p || !r || r->family != HELD_RECORD_HOBS || p->failure || p->pending)
		return p && held_physical_owner == p ? Held_PhysicalFail(p, 1) : 0;
	if (!Held_PhysicalState(p)) return Held_PhysicalFail(p, 1);
#define CF(n,v) Held_PhysicalCompareF(r,n,v)
#define CV(n,v) Held_PhysicalCompareV(r,n,v)
	if (!CV("origin",pmove.origin) || !CV("velocity",pmove.velocity) || !CV("hullmins",pmove.player_mins) ||
		!CV("hullmaxs",pmove.player_maxs) || !CV("srcladdernormal",pmove.srcladdernormal) || !CV("portaloffset",pmove.portaloffset) ||
		PU(r,"ground") != (unsigned int)pmove.onground || PL(r,"raw_moveticks") != p->raw_moveticks ||
		!CF("tickused",pmove.tickused) || !CF("msec_carry",pmove.msec_carry) || !CF("surfacefriction",pmove.surfacefriction) ||
		!CF("ducktime",pmove.ducktime) || PU(r,"ducking") != (unsigned int)pmove.ducking || PU(r,"ducked") != (unsigned int)pmove.ducked ||
		!CF("stamina",pmove.stamina) || !CF("rampoff",pmove.rampoff) || !CF("boardcount",pmove.boardcount) ||
		!CF("rampcontact",pmove.rampcontact) || PU(r,"srcladder") != (unsigned int)pmove.srcladder)
		return Held_PhysicalFail(p, p->failure ? 1 : 2);
#undef CF
#undef CV
	return 1;
}
#undef PF
#undef PU
#undef PI
#undef PL

static void Held_PhysicalTestRecord(held_record_t *r, held_record_family_t family)
{
	unsigned int i;
	SV_HeldRecordBegin(r, family);
	for (i = 0; i < held_schemas[family].count; ++i) r->assigned[i] = 1;
}
static int Held_PhysicalTestMove(held_physical_t *p, held_record_t *r, float msec, unsigned int *acted)
{
	usercmd_t command;
	memset(&command, 0, sizeof(command));
	Held_PhysicalTestRecord(r, HELD_RECORD_HMOVE);
	SV_HeldRecordU(r,"context",1); SV_HeldRecordU(r,"pmtype",p->pinned ? PM_NONE : PM_NORMAL);
	SV_HeldRecordU(r,"ownerkind",p->pinned ? 3 : 1);
	SV_HeldRecordL(r,"execution",p->calls+1);
	SV_HeldRecordF(r,"effective_msec",msec); SV_HeldRecordF(r,"gamespeed",1);
	if (!SV_HeldPhysicalStep(p,r,p->pinned ? NULL : &command,0)) return 0;
	++*acted;
	return 1;
}
static void Held_PhysicalTestDone(held_physical_t *p, held_record_t *r)
{
	Held_PhysicalTestRecord(r, HELD_RECORD_HDONE);
	SV_HeldRecordL(r,"execution",p->calls); SV_HeldRecordL(r,"raw_moveticks",p->raw_moveticks);
	SV_HeldRecordU(r,"ticksrun",pmove.ticksrun); SV_HeldRecordF(r,"carry",pmove.msec_carry);
}
static void Held_PhysicalTestObservation(held_physical_t *p, held_record_t *r)
{
	static const struct { const char *wire; size_t offset; } vectors[] = {
		{"origin",offsetof(playermove_t,origin)}, {"velocity",offsetof(playermove_t,velocity)},
		{"hullmins",offsetof(playermove_t,player_mins)}, {"hullmaxs",offsetof(playermove_t,player_maxs)},
		{"srcladdernormal",offsetof(playermove_t,srcladdernormal)}, {"portaloffset",offsetof(playermove_t,portaloffset)}
	};
	unsigned int i,j;
	char name[64];
	float scalar;
	Held_PhysicalTestRecord(r,HELD_RECORD_HOBS);
	for (i = 0; i < countof(vectors); ++i)
		for (j = 0; j < 3; ++j)
		{
			memcpy(&scalar,(const char*)&pmove+vectors[i].offset+j*sizeof(scalar),sizeof(scalar));
			Q_snprintfz(name,sizeof(name),"%s_%c",vectors[i].wire,'x'+j);
			SV_HeldRecordF(r,name,scalar);
		}
#define TF(n,v) SV_HeldRecordF(r,n,v)
	TF("tickused",pmove.tickused); TF("msec_carry",pmove.msec_carry); TF("surfacefriction",pmove.surfacefriction);
	TF("ducktime",pmove.ducktime); TF("stamina",pmove.stamina); TF("rampoff",pmove.rampoff);
	TF("boardcount",pmove.boardcount); TF("rampcontact",pmove.rampcontact);
#undef TF
	SV_HeldRecordL(r,"raw_moveticks",p->raw_moveticks); SV_HeldRecordU(r,"ground",pmove.onground);
	SV_HeldRecordU(r,"ducking",pmove.ducking); SV_HeldRecordU(r,"ducked",pmove.ducked);
	SV_HeldRecordU(r,"srcladder",pmove.srcladder);
}
/* Native synthetic kernel controls, not a recorded or admitted profile. */
static unsigned int held_closed_test_diagnostic;
/* Test-only projection proof. No observation is installed into production state. */
static unsigned int Held_PhysicalProjectionTest(const held_record_t *ctx, const held_record_t *initial,
	float rate, unsigned int *checks, unsigned int *acted)
{
	held_physical_t replay = {0};
	held_record_t r;
	playermove_t *reference = BZF_Malloc(sizeof(pmove));
	int trace[3] = {bih_sourceclip,bih_phybrushes,bih_phyfix};
	unsigned int mode, pass, i, failures = 0;
	size_t after = offsetof(playermove_t,landticks)+sizeof(pmove.landticks);
#define HPP_TEST(x) do {++*checks; if (!(x)) {++failures; Con_Printf("held projection control failed at %d\n",__LINE__);}} while (0)
#define HPP_REQUIRE(x) do {++*checks; if (!(x)) {++failures; Con_Printf("held projection control failed at %d\n",__LINE__); goto cleanup;}} while (0)
	HPP_REQUIRE(reference != NULL);
	for (mode = 0; mode < 2; ++mode)
		for (pass = 0; pass < 2; ++pass)
		{
			bih_sourceclip = 1; bih_phybrushes = 0; bih_phyfix = 1;
			memset(&replay,0,sizeof(replay));
			HPP_REQUIRE(SV_HeldPhysicalBegin(&replay,ctx,initial,sv.world.worldmodel));
			pmove.landticks = pass ? PMLIM_LANDMAX : 0; /* deliberate test intervention */
			if (mode)
			{
				HPP_REQUIRE(Held_PhysicalTestMove(&replay,&r,0,acted));
				Held_PhysicalTestDone(&replay,&r); HPP_REQUIRE(SV_HeldPhysicalDone(&replay,&r));
				Held_PhysicalTestRecord(&r,HELD_RECORD_HENTRY); SV_HeldRecordI(&r,"saved_movetype",MOVETYPE_WALK);
				HPP_REQUIRE(SV_HeldPhysicalControl(&replay,&r));
			}
			HPP_REQUIRE(Held_PhysicalTestMove(&replay,&r,rate*1500.0f,acted));
			HPP_TEST(pmove.ticksrun == 1 && replay.raw_moveticks == 101 && pmove.msec_carry > 0);
			HPP_TEST(mode ? !pmove.velocity[2] : pmove.velocity[2] < 0);
			HPP_TEST(pmove.landticks == (pass ? PMLIM_LANDMAX : 1));
			if (!pass) memcpy(reference,&pmove,sizeof(pmove));
			else HPP_TEST(!memcmp(reference,&pmove,offsetof(playermove_t,landticks)) &&
				!memcmp((const char*)reference+after,(const char*)&pmove+after,sizeof(pmove)-after));
			Held_PhysicalTestDone(&replay,&r); HPP_REQUIRE(SV_HeldPhysicalDone(&replay,&r));
			SV_HeldPhysicalEnd(&replay);
			HPP_TEST(bih_sourceclip == 1 && !bih_phybrushes && bih_phyfix == 1);
		}
	for (i = 0; i < 15; ++i)
	{
		float value = i < 6 ? 1 : i < 9 ? -1 : i < 12 ? INFINITY : NAN;
		bih_sourceclip = 1; bih_phybrushes = 0; bih_phyfix = 1;
		memset(&replay,0,sizeof(replay));
		HPP_REQUIRE(SV_HeldPhysicalBegin(&replay,ctx,initial,sv.world.worldmodel));
		if (!i) pmove.momlimit = 1;
		else if (i == 1) pmove.momcool = .2f;
		else if (i == 2) pmove.momslide = 1;
		else if (i % 3 == 0) pmove.waterjumpremaining = value;
		else pmove.waterjumpdir[(i-1)%3] = value;
		HPP_TEST(!Held_PhysicalTestMove(&replay,&r,0,acted) && replay.failure == 1);
		HPP_TEST(bih_sourceclip == 1 && !bih_phybrushes && bih_phyfix == 1);
	}
cleanup:
	if (held_physical_owner == &replay) SV_HeldPhysicalEnd(&replay);
	bih_sourceclip = trace[0]; bih_phybrushes = trace[1]; bih_phyfix = trace[2];
	if (reference) {Held_PhysicalWipe(reference,sizeof(*reference)); BZ_Free(reference);}
	Held_PhysicalWipe(&r,sizeof(r)); Held_PhysicalWipe(&replay,sizeof(replay));
	return failures;
#undef HPP_TEST
#undef HPP_REQUIRE
}
static unsigned int Held_BodyTest(const held_record_t *context, unsigned int *checks);
static unsigned int Held_ClosedTest(const held_record_t *context, const held_record_t *initial,
	unsigned int *checks, unsigned int *acted);
unsigned int SV_HeldPhysicalTest(unsigned int *checks, unsigned int *acted)
{
	held_physical_t replay = {0}, probe = {0};
	static const char *artifacts[] = {"world","plugin","engine","progs","zone"};
	held_record_t ctx, native, initial, r;
	const held_value_t *artifact;
	eval_t *clockvalue;
	etype_t clocktype;
	int identities, captured;
	char legacy[HELD_MAX_LINE], tokens[4096], *badnumber;
	pmsourcestate_t source_state;
	playermove_t *original = NULL;
	movevars_t original_movevars;
	float pins[SV_PMPIN_COUNT], rate, carry;
	vec3_t point = {0};
	unsigned int failures = 0, i, x, y, z, found = 0, physcrc, erased, sample_failure;
	char name[64];
	model_t *world = sv.world.worldmodel;
#define HP_TEST(x) do { ++*checks; if (!(x)) {++failures; Con_Printf("held physical control failed at %d\n",__LINE__);} } while (0)
#define HP_REQUIRE(x) do { ++*checks; if (!(x)) {++failures; Con_Printf("held physical control failed at %d\n",__LINE__); goto cleanup;} } while (0)
	*checks = *acted = 0;
	HP_REQUIRE(!held_physical_owner && sv.state && world && (world->engineflags & MDLF_SOURCEBSP));
	original = BZF_Malloc(sizeof(pmove));
	HP_REQUIRE(original != NULL);
	memcpy(original,&pmove,sizeof(pmove)); original_movevars = movevars;
	memset(&pmove,0,sizeof(pmove)); memset(&movevars,0,sizeof(movevars));
	SV_SetMoveVars(); movevars.gravity = Cvar_VariableValue("sv_gravity"); movevars.coordtype = 4;
	pmove.player_mins[0] = pmove.player_mins[1] = -16;
	pmove.player_maxs[0] = pmove.player_maxs[1] = 16; pmove.player_maxs[2] = 62;
	pmove.pm_type = PM_NORMAL;
	SV_PMPinFill(pins);
	Held_PhysicalTestRecord(&ctx,HELD_RECORD_HCTX);
	for (i = 0; i < HELD_CONTEXT_PINS; ++i)
	{
		Q_snprintfz(name,sizeof(name),"ctx_%s",sv_pmpin_names[i]);
		if (SV_HeldRecordF(&ctx,name,pins[i])) continue;
		if (SV_HeldRecordI(&ctx,name,(int32_t)pins[i])) continue;
		HP_REQUIRE(SV_HeldRecordU(&ctx,name,(uint32_t)pins[i]));
	}
	SV_HeldRecordU(&ctx,"context",1); SV_HeldRecordU(&ctx,"rules",1);
	SV_HeldRecordU(&ctx,"ctx_flags",movevars.flags); SV_HeldRecordU(&ctx,"ctx_coordtype",4);
	SV_HeldRecordU(&ctx,"ctx_numphysent",1); SV_HeldRecordF(&ctx,"ctx_hullmaxs_z",62);
	SV_HeldRecordF(&ctx,"ctx_gravitydir_z",-1);
	SV_HeldRecordI(&ctx,"ctx_trisoup_i",Cvar_FindVar("pm_trisoup_bevels")->ival);
	SV_HeldRecordI(&ctx,"ctx_rotboxes_i",Cvar_FindVar("pm_rotatedboxhulls")->ival);
	SV_HeldRecordI(&ctx,"ctx_portalcsg_i",Cvar_FindVar("pm_portalcsg_scanall")->ival);
	clockvalue = PR_FindGlobal(svprogfuncs,"sv_ck_excuse",PR_ANY,&clocktype);
	HP_REQUIRE(clockvalue && (clocktype&0xfff) == ev_float);
	SV_HeldRecordF(&ctx,"ctx_excuse",clockvalue->_float);
	clockvalue = PR_FindGlobal(svprogfuncs,"run_t_slowclk",PR_ANY,&clocktype);
	HP_REQUIRE(clockvalue && (clocktype&0xfff) == ev_float);
	SV_HeldRecordF(&ctx,"ctx_slow",clockvalue->_float);
	SV_HeldRecordF(&ctx,"ctx_hyst",.2f); SV_HeldRecordF(&ctx,"ctx_allow",.5f);
	SV_HeldRecordF(&ctx,"ctx_freecredits",4); SV_HeldRecordF(&ctx,"ctx_gap",.15f);
	HP_REQUIRE(SV_HeldContextBuildNative(&movevars,&native));
	for (i = 0; i < 15; ++i)
	{
		float value = i < 6 ? 1 : i < 9 ? -1 : i < 12 ? INFINITY : NAN;
		if (!i) pmove.momlimit = 1;
		else if (i == 1) pmove.momcool = .2f;
		else if (i == 2) pmove.momslide = 1;
		else if (i % 3 == 0) pmove.waterjumpremaining = value;
		else pmove.waterjumpdir[(i-1)%3] = value;
		memset(&r,0xa5,sizeof(r)); captured = SV_HeldContextBuildNative(&movevars,&r); erased = 0;
		for (x = 0; x < sizeof(r); ++x) erased |= ((unsigned char*)&r)[x];
		HP_TEST(!captured && !erased);
		pmove.momlimit = pmove.momslide = 0; pmove.momcool = 0;
		pmove.waterjumpremaining = pmove.waterjumpdir[0] = pmove.waterjumpdir[1] = 0;
	}
	for (i = HELD_SOURCE_VERSION-1; i <= HELD_SOURCE_VERSION+1; i += 2)
	{
		movevars.sourceversion = i;
		memset(&r,0xa5,sizeof(r)); captured = SV_HeldContextBuildNative(&movevars,&r); erased = 0;
		for (x = 0; x < sizeof(r); ++x) erased |= ((unsigned char*)&r)[x];
		HP_TEST(!captured && !erased);
	}
	movevars.sourceversion = HELD_SOURCE_VERSION;
	memset(&r,0xa5,sizeof(r)); captured = SV_HeldObservationCapture(NULL,1,&r); erased = 0;
	for (i = 0; i < sizeof(r); ++i) erased |= ((unsigned char*)&r)[i];
	HP_TEST(!captured && !erased);
	memset(&r,0xa5,sizeof(r)); captured = SV_HeldClockBoundaryCapture(NULL,HELD_RECORD_HFREEZE,&r); erased = 0;
	for (i = 0; i < sizeof(r); ++i) erased |= ((unsigned char*)&r)[i];
	HP_TEST(!captured && !erased);
	memset(&r,0xa5,sizeof(r)); captured = SV_HeldClockBoundaryCapture(NULL,HELD_RECORD_HCTX,&r); erased = 0;
	for (i = 0; i < sizeof(r); ++i) erased |= ((unsigned char*)&r)[i];
	HP_TEST(!captured && !erased);
	HP_TEST(!SV_HeldSessionSelect(NULL,"00000000000000000000000000000001"));
	HP_TEST(!SV_HeldSessionCheck(NULL,"00000000000000000000000000000001"));
	HP_TEST(!SV_HeldSessionAbort(NULL,"00000000000000000000000000000001"));
	identities = 1;
	for (i = 0; i < countof(artifacts); ++i)
	{
		artifact = SV_HeldRecordGet(&native,artifacts[i],'D');
		if (!artifact) {identities = 0; break;}
		if (!SV_HeldRecordBytes(&ctx,artifacts[i],artifact->bytes,32)) identities = 0;
	}
	HP_TEST(identities && Held_TrajectoryContext(&ctx,&native));
	HP_REQUIRE(SV_PMPinText(pins,tokens,sizeof(tokens)) != 0);
	Q_snprintfz(legacy,sizeof(legacy),"pmpin %s\n",tokens);
	HP_TEST(Held_LegacyPin(legacy,strlen(legacy),&ctx));
	for (i = HELD_CONTEXT_PINS; i < SV_PMPIN_COUNT; ++i)
	{
		pins[i] = 1; HP_REQUIRE(SV_PMPinText(pins,tokens,sizeof(tokens)) != 0);
		Q_snprintfz(legacy,sizeof(legacy),"pmpin %s\n",tokens);
		HP_TEST(!Held_LegacyPin(legacy,strlen(legacy),&ctx)); pins[i] = 0;
	}
	HP_REQUIRE(SV_PMPinText(pins,tokens,sizeof(tokens)) != 0);
	Q_snprintfz(legacy,sizeof(legacy),"pmpin %s gravity=1\n",tokens);
	HP_TEST(!Held_LegacyPin(legacy,strlen(legacy),&ctx));
	Q_snprintfz(legacy,sizeof(legacy),"pmpin %s\n",tokens);
	badnumber = strchr(legacy,'='); if (badnumber) badnumber[1] = 'x';
	HP_TEST(badnumber && !Held_LegacyPin(legacy,strlen(legacy),&ctx));
	rate = movevars.ticrate;
	Held_PhysicalTestRecord(&initial,HELD_RECORD_HINIT);
	SV_HeldRecordU(&initial,"context",1); SV_HeldRecordL(&initial,"execution",1);
	SV_HeldRecordL(&initial,"raw_moveticks",100);
	SV_HeldRecordF(&initial,"velocity_x",100); SV_HeldRecordF(&initial,"surfacefriction",1);
	SV_HeldRecordF(&initial,"hullmins_x",-16); SV_HeldRecordF(&initial,"hullmins_y",-16);
	SV_HeldRecordF(&initial,"hullmaxs_x",16); SV_HeldRecordF(&initial,"hullmaxs_y",16);
	SV_HeldRecordF(&initial,"hullmaxs_z",62); SV_HeldRecordF(&initial,"gravitydir_z",-1);
	SV_HeldRecordF(&initial,"viewheight",movevars.viewheight);
	SV_HeldRecordF(&initial,"tickused",rate); SV_HeldRecordI(&initial,"watertype",FTECONTENTS_EMPTY);
	probe.world = world;
	/* At most36 model-only candidate cells; never read/move a player's body. */
	for (x = 1; x <= 3 && !found; ++x)
		for (y = 1; y <= 3 && !found; ++y)
			for (z = 1; z <= 4 && !found; ++z)
			{
				point[0] = world->mins[0] + (world->maxs[0]-world->mins[0]) * (.25f*x);
				point[1] = world->mins[1] + (world->maxs[1]-world->mins[1]) * (.25f*y);
				point[2] = world->mins[2] + (world->maxs[2]-world->mins[2]) * (.2f*z);
				VectorCopy(point,pmove.origin);
				if (Held_PhysicalAir(&probe)) found = 1;
			}
	/* Setup touched scratch only; restore before the tested owner's own save. */
	memcpy(&pmove,original,sizeof(pmove)); movevars = original_movevars;
	HP_REQUIRE(found && rate > 0 && rate <= .03f);
	for (i = 0; i < 3; ++i)
	{
		Q_snprintfz(name,sizeof(name),"origin_%c",'x'+i); SV_HeldRecordF(&initial,name,point[i]);
		Q_snprintfz(name,sizeof(name),"safeorigin_%c",'x'+i); SV_HeldRecordF(&initial,name,point[i]);
	}
	failures += Held_PhysicalProjectionTest(&ctx,&initial,rate,checks,acted);
	for (i = HELD_SOURCE_VERSION-1; i <= HELD_SOURCE_VERSION+1; i += 2)
	{
		memset(&replay,0,sizeof(replay)); HP_REQUIRE(SV_HeldRecordU(&ctx,"ctx_pmsrcver",i));
		HP_TEST(!SV_HeldPhysicalBegin(&replay,&ctx,&initial,world) && replay.failure == 1);
		HP_TEST(!memcmp(&pmove,original,sizeof(pmove)) && !memcmp(&movevars,&original_movevars,sizeof(movevars)));
	}
	HP_REQUIRE(SV_HeldRecordU(&ctx,"ctx_pmsrcver",HELD_SOURCE_VERSION));
	memset(&replay,0,sizeof(replay));
	HP_REQUIRE(SV_HeldPhysicalBegin(&replay,&ctx,&initial,world));
	PMSrc_SaveState(&source_state);
	HP_REQUIRE(SV_PMStateText(&source_state,tokens,sizeof(tokens)) != 0);
	Q_snprintfz(legacy,sizeof(legacy),"seed %.9g %.9g %.9g %.9g %.9g %.9g 0 %s\n",
		pmove.origin[0],pmove.origin[1],pmove.origin[2],pmove.velocity[0],pmove.velocity[1],pmove.velocity[2],tokens);
	HP_TEST(Held_LegacySeed(legacy,strlen(legacy),&initial));
	{
		static const char *suffixes[] = {"", " wjtime=0", " wjtime=0 wjx=0",
			" wjtime=1 wjx=0 wjy=0", " wjtime=0 wjx=1 wjy=0", " wjtime=0 wjx=0 wjy=1",
			" wjtime=nan wjx=0 wjy=0", " wjtime=0 wjx=inf wjy=0", " wjtime=0 wjx=0 wjy=1e-999",
			" wjtime=0 wjy=0 wjx=0", " wjtime=0 wjx=0 wjy=0 wjy=0", " wjtime=0 wjx=0 wjy=0 other=0"};
		char *suffix = strstr(tokens," wjtime=");
		HP_REQUIRE(suffix != NULL); *suffix = 0;
		for (i = 0; i < countof(suffixes); ++i)
		{
			Q_snprintfz(legacy,sizeof(legacy),"seed %.9g %.9g %.9g %.9g %.9g %.9g 0 %s%s\n",
				pmove.origin[0],pmove.origin[1],pmove.origin[2],pmove.velocity[0],pmove.velocity[1],pmove.velocity[2],tokens,suffixes[i]);
			HP_TEST(!Held_LegacySeed(legacy,strlen(legacy),&initial));
		}
	}
	for (i = 0; i < 3; ++i)
	{
		source_state.landticks = i == 0 ? PMLIM_LANDMAX : i == 1 ? -1 : PMLIM_LANDMAX+1;
		HP_REQUIRE(SV_PMStateText(&source_state,tokens,sizeof(tokens)) != 0);
		Q_snprintfz(legacy,sizeof(legacy),"seed %.9g %.9g %.9g %.9g %.9g %.9g 0 %s\n",
			pmove.origin[0],pmove.origin[1],pmove.origin[2],pmove.velocity[0],pmove.velocity[1],pmove.velocity[2],tokens);
		HP_TEST(Held_LegacySeed(legacy,strlen(legacy),&initial) == (i == 0));
	}
	PMSrc_SaveState(&source_state); HP_REQUIRE(SV_PMStateText(&source_state,tokens,sizeof(tokens)) != 0);
	Q_snprintfz(legacy,sizeof(legacy),"seed %.9g %.9g %.9g %.9g %.9g %.9g 0 %s\n",
		nextafterf(pmove.origin[0],INFINITY),pmove.origin[1],pmove.origin[2],pmove.velocity[0],pmove.velocity[1],pmove.velocity[2],tokens);
	HP_TEST(!Held_LegacySeed(legacy,strlen(legacy),&initial));
	pmove.ticksrun = 999; /* A zero-tick native call must actually replace this sentinel. */
	HP_REQUIRE(Held_PhysicalTestMove(&replay,&r,0,acted));
	HP_TEST(!pmove.ticksrun && replay.raw_moveticks == 100 && !pmove.msec_carry);
	HP_TEST(!memcmp(pmove.origin,point,sizeof(point)) && pmove.velocity[0] == 100);
	Q_snprintfz(legacy,sizeof(legacy),"0.0000 %.2f %.2f %.2f %.2f %.2f %.2f 0.0 0.0 0 0 0 0 0 0.0000 0.0000 0.0000\n",
		pmove.origin[0],pmove.origin[1],pmove.origin[2],pmove.velocity[0],pmove.velocity[1],pmove.velocity[2]);
	HP_TEST(Held_LegacySamplePhysical(legacy,strlen(legacy),&pmove,0,0,&sample_failure) && !sample_failure);
	HP_TEST(!Held_LegacySamplePhysical(legacy,strlen(legacy),&pmove,.0001f,0,&sample_failure) && sample_failure == 2);
	badnumber = strrchr(legacy,' '); if (badnumber) badnumber[1] = '1';
	HP_TEST(badnumber && !Held_LegacySamplePhysical(legacy,strlen(legacy),&pmove,0,0,&sample_failure) && !sample_failure);
	Q_snprintfz(legacy,sizeof(legacy),"0.0000 %.2f %.2f %.2f %.2f %.2f %.2f 0.0 0.0 0 0 0 0 0 0.0000 0.0000 0.0000\n",
		pmove.origin[0]+.25f,pmove.origin[1],pmove.origin[2],pmove.velocity[0],pmove.velocity[1],pmove.velocity[2]);
	HP_TEST(!Held_LegacySamplePhysical(legacy,strlen(legacy),&pmove,0,0,&sample_failure) && sample_failure == 2);
	Held_PhysicalTestDone(&replay,&r); HP_REQUIRE(SV_HeldPhysicalDone(&replay,&r));
	Held_PhysicalTestRecord(&r,HELD_RECORD_HENTRY); SV_HeldRecordI(&r,"saved_movetype",MOVETYPE_WALK);
	HP_REQUIRE(SV_HeldPhysicalControl(&replay,&r));
	Q_snprintfz(legacy,sizeof(legacy),"warp 7 100 0 pin %.9g %.9g %.9g 0 0 0 0\n",pmove.origin[0],pmove.origin[1],pmove.origin[2]);
	HP_TEST(Held_LegacyPinWarp(legacy,strlen(legacy),&pmove,100,7,0));
	Q_snprintfz(legacy,sizeof(legacy),"warp 7 100 0 pin %.9g %.9g %.9g 0 0 0 0\n",nextafterf(pmove.origin[0],INFINITY),pmove.origin[1],pmove.origin[2]);
	HP_TEST(!Held_LegacyPinWarp(legacy,strlen(legacy),&pmove,100,7,0));
	SV_PMPinFill(pins); HP_REQUIRE(SV_PMPinText(pins,tokens,sizeof(tokens)) != 0);
	Q_snprintfz(legacy,sizeof(legacy),"pm 7 0 %s\n",tokens);
	HP_TEST(Held_LegacyPM(legacy,strlen(legacy),&ctx,PM_NONE,7,0));
	HP_TEST(!Held_LegacyPM(legacy,strlen(legacy),&ctx,PM_NORMAL,7,0));
	HP_TEST(!Held_LegacyPM(legacy,strlen(legacy),&ctx,PM_NONE,8,0));
	physcrc = SV_PhysentDigest();
	Q_snprintfz(legacy,sizeof(legacy),"pe 7 0 %u\n",physcrc);
	HP_TEST(Held_LegacyPE(legacy,strlen(legacy),7,0,physcrc));
	HP_TEST(!Held_LegacyPE(legacy,strlen(legacy),7,0,physcrc^1u));
	HP_REQUIRE(Held_PhysicalTestMove(&replay,&r,rate*1500.0f,acted));
	HP_TEST(pmove.ticksrun == 1 && replay.raw_moveticks == 101 && pmove.msec_carry > rate*.4f && pmove.msec_carry < rate*.6f);
	HP_TEST(!memcmp(pmove.origin,point,sizeof(point)) && !pmove.velocity[0] && !pmove.velocity[1] && !pmove.velocity[2]);
	carry = pmove.msec_carry;
	Held_PhysicalTestDone(&replay,&r); HP_REQUIRE(SV_HeldPhysicalDone(&replay,&r));
	failures += Held_BodyTest(&ctx,checks);
	HP_REQUIRE(Held_PhysicalTestMove(&replay,&r,0,acted));
	HP_TEST(!pmove.ticksrun && replay.raw_moveticks == 101 && !memcmp(&carry,&pmove.msec_carry,sizeof(carry)));
	Held_PhysicalTestDone(&replay,&r); HP_REQUIRE(SV_HeldPhysicalDone(&replay,&r));
	Held_PhysicalTestRecord(&r,HELD_RECORD_HRELEASE); SV_HeldRecordI(&r,"restored_movetype",MOVETYPE_WALK);
	HP_REQUIRE(SV_HeldPhysicalControl(&replay,&r));
	Held_PhysicalTestRecord(&r,HELD_RECORD_HTHAW); HP_REQUIRE(SV_HeldPhysicalControl(&replay,&r));
	HP_REQUIRE(Held_PhysicalTestMove(&replay,&r,rate*1000.0f,acted));
	HP_TEST(pmove.ticksrun == 1 && replay.raw_moveticks == 102 && pmove.origin[2] < point[2] && pmove.velocity[2] < 0 && replay.guard_done);
	Held_PhysicalTestDone(&replay,&r); HP_REQUIRE(SV_HeldPhysicalDone(&replay,&r));
	Held_PhysicalTestObservation(&replay,&r); HP_REQUIRE(SV_HeldPhysicalObserve(&replay,&r));
	SV_HeldRecordF(&r,"origin_x",pmove.origin[0]+1);
	HP_TEST(!SV_HeldPhysicalObserve(&replay,&r) && replay.failure == 2);
	HP_TEST(!memcmp(&pmove,original,sizeof(pmove)) && !memcmp(&movevars,&original_movevars,sizeof(movevars)));
	memset(&replay,0,sizeof(replay));
	HP_REQUIRE(SV_HeldPhysicalBegin(&replay,&ctx,&initial,world));
	HP_REQUIRE(Held_PhysicalTestMove(&replay,&r,0,acted));
	Held_PhysicalTestDone(&replay,&r); SV_HeldRecordU(&r,"ticksrun",1);
	HP_TEST(!SV_HeldPhysicalDone(&replay,&r) && replay.failure == 2);
	HP_TEST(!memcmp(&pmove,original,sizeof(pmove)) && !memcmp(&movevars,&original_movevars,sizeof(movevars)));
	memset(&replay,0,sizeof(replay));
	HP_REQUIRE(SV_HeldPhysicalBegin(&replay,&ctx,&initial,world));
	HP_TEST(!SV_HeldPhysicalBegin(&replay,&ctx,&initial,world) && replay.failure == 1);
	HP_TEST(!memcmp(&pmove,original,sizeof(pmove)) && !memcmp(&movevars,&original_movevars,sizeof(movevars)));
	failures += Held_ClosedTest(&ctx,&initial,checks,acted);
	/* Distinct raw default0/explicit1 contexts, identical actual Source moves. */
	HP_REQUIRE(!held_physical_owner);
	memset(&replay,0,sizeof(replay));
	HP_REQUIRE(SV_HeldRecordF(&ctx,"ctx_entgravity",1));
	HP_REQUIRE(SV_HeldPhysicalBegin(&replay,&ctx,&initial,world));
	HP_REQUIRE(Held_PhysicalTestMove(&replay,&r,rate*1000.0f,acted));
	HP_TEST(pmove.ticksrun == 1 && replay.raw_moveticks == 101 && pmove.velocity[2] < 0);
	Held_PhysicalTestDone(&replay,&r); HP_REQUIRE(SV_HeldPhysicalDone(&replay,&r));
	Held_PhysicalTestObservation(&replay,&native);
	SV_HeldPhysicalEnd(&replay);
	memset(&replay,0,sizeof(replay));
	HP_REQUIRE(SV_HeldRecordF(&ctx,"ctx_entgravity",0));
	HP_REQUIRE(SV_HeldPhysicalBegin(&replay,&ctx,&initial,world));
	HP_REQUIRE(Held_PhysicalTestMove(&replay,&r,rate*1000.0f,acted));
	HP_TEST(pmove.ticksrun == 1 && replay.raw_moveticks == 101 && !movevars.entgravity);
	Held_PhysicalTestDone(&replay,&r); HP_REQUIRE(SV_HeldPhysicalDone(&replay,&r));
	HP_REQUIRE(SV_HeldPhysicalObserve(&replay,&native));
	SV_HeldPhysicalEnd(&replay);
	for (i = 0; i < 3; ++i)
	{
		memset(&replay,0,sizeof(replay));
		HP_REQUIRE(SV_HeldRecordF(&ctx,"ctx_entgravity",i == 0 ? .5f : i == 1 ? -1 : 2));
		HP_TEST(!SV_HeldPhysicalBegin(&replay,&ctx,&initial,world) && replay.failure == 1);
		HP_TEST(!memcmp(&pmove,original,sizeof(pmove)) && !memcmp(&movevars,&original_movevars,sizeof(movevars)));
	}
cleanup:
	if (held_physical_owner == &replay) SV_HeldPhysicalEnd(&replay);
	if (original)
	{
		memcpy(&pmove,original,sizeof(pmove)); movevars = original_movevars;
		Held_PhysicalWipe(original,sizeof(*original)); BZ_Free(original);
	}
	Held_PhysicalWipe(&ctx,sizeof(ctx)); Held_PhysicalWipe(&initial,sizeof(initial)); Held_PhysicalWipe(&r,sizeof(r));
	Held_PhysicalWipe(&native,sizeof(native)); Held_PhysicalWipe(&source_state,sizeof(source_state));
	Held_PhysicalWipe(legacy,sizeof(legacy)); Held_PhysicalWipe(tokens,sizeof(tokens));
	return failures;
#undef HP_TEST
#undef HP_REQUIRE
}

static const struct { const char *wire; size_t offset; } held_clock_fields[] = {
#define HCF(n) {"clock_" #n,offsetof(held_clock_t,n)}
	HCF(starttick),HCF(startsec),HCF(startwall),HCF(tickcarry),HCF(lastmt),HCF(lastsec),
	HCF(freeze),HCF(frzon),HCF(frzat),HCF(frzmt),HCF(frzcarry),HCF(frzticks),HCF(frzsec),
	HCF(state),HCF(ticks),HCF(elapsed),HCF(ckwall),HCF(ckwt),HCF(ckgaps),HCF(ckncr),
	HCF(ckcb),HCF(ckcr),HCF(ckd),HCF(cktr),HCF(ckpk),HCF(ckup),HCF(cku),HCF(ckskip),HCF(resumesec),
	{"clock_lastorg_x",offsetof(held_clock_t,lastorg[0])},
	{"clock_lastorg_y",offsetof(held_clock_t,lastorg[1])},
	{"clock_lastorg_z",offsetof(held_clock_t,lastorg[2])},
	HCF(packet),HCF(counted),HCF(flags),HCF(frame)
#undef HCF
};
static int Held_ReplayClockRead(const held_record_t *r, held_clock_t *clock)
{
	unsigned int i;
	const held_value_t *value;
	memset(clock,0,sizeof(*clock));
	if (!r || (r->family != HELD_RECORD_HINIT && r->family != HELD_RECORD_HFREEZE &&
		r->family != HELD_RECORD_HTHAW && r->family != HELD_RECORD_HTERMINAL)) return 0;
	for (i = 0; i < countof(held_clock_fields); ++i)
	{
		value = SV_HeldRecordGet(r,held_clock_fields[i].wire,'F');
		if (!value || !Held_PhysicalFinite(value->f))
		{
			Held_PhysicalWipe(clock,sizeof(*clock));
			return 0;
		}
		memcpy((char*)clock+held_clock_fields[i].offset,&value->f,sizeof(value->f));
	}
	return 1;
}
int SV_HeldReplayClockBegin(held_clock_replay_t *replay, const held_record_t *r, const held_record_t *initial)
{
	static const struct { const char *wire; size_t offset; } fields[] = {
#define HCC(n) {"ctx_" #n,offsetof(held_clock_context_t,n)}
		HCC(excuse),HCC(slow),HCC(hyst),HCC(allow),HCC(freecredits),HCC(gap)
#undef HCC
	};
	held_clock_context_t context = {0};
	held_clock_t clock;
	const held_value_t *value;
	unsigned int i;
	int result = 0;
	if (!replay) return 0;
	memset(&clock,0,sizeof(clock));
	if (!r || !initial || r->family != HELD_RECORD_HCTX || initial->family != HELD_RECORD_HINIT) goto cleanup;
	value = SV_HeldRecordGet(r,"context",'U'); if (!value || value->u != 1) goto cleanup;
	value = SV_HeldRecordGet(r,"rules",'U'); if (!value || value->u != 1) goto cleanup;
	value = SV_HeldRecordGet(initial,"context",'U'); if (!value || value->u != 1) goto cleanup;
	value = SV_HeldRecordGet(initial,"execution",'L'); if (!value || value->l != 1) goto cleanup;
	if (!Held_ReplayClockRead(initial,&clock)) goto cleanup;
	for (i = 0; i < countof(fields); ++i)
	{
		value = SV_HeldRecordGet(r,fields[i].wire,'F');
		if (!value || !Held_PhysicalFinite(value->f)) goto cleanup;
		memcpy((char*)&context+fields[i].offset,&value->f,sizeof(value->f));
	}
	/* Frozen rules1, sh_defs.qc: TF_FROZEN16, TF_SLOWCLOCK524288, TS_RUNNING2. */
	context.frozenflag = 16; context.slowflag = 524288; context.runningstate = 2;
	result = SV_HeldClockBegin(replay,&clock,&context);
cleanup:
	Held_PhysicalWipe(&context,sizeof(context)); Held_PhysicalWipe(&clock,sizeof(clock));
	if (!result)
	{
		Held_PhysicalWipe(replay,sizeof(*replay));
		replay->failed = 1;
	}
	return result;
}
int SV_HeldReplayClockCompare(const held_clock_replay_t *replay, const held_record_t *observation)
{
	held_clock_t clock;
	int result;
	if (!replay) return 0;
	result = Held_ReplayClockRead(observation,&clock) && SV_HeldClockCompare(replay,&clock);
	Held_PhysicalWipe(&clock,sizeof(clock));
	return result;
}
static void Held_ReplayClockTestSnapshot(held_record_t *r, const held_clock_t *clock)
{
	float scalar;
	unsigned int i;
	for (i = 0; i < countof(held_clock_fields); ++i)
	{
		memcpy(&scalar,(const char*)clock+held_clock_fields[i].offset,sizeof(scalar));
		SV_HeldRecordF(r,held_clock_fields[i].wire,scalar);
	}
}
unsigned int SV_HeldReplayClockTest(unsigned int *checks)
{
	held_clock_replay_t replay = {0}, empty = {0};
	held_clock_t seed = {0};
	held_record_t ctx, initial, observed;
	unsigned int i, failures = 0;
	uint32_t bits;
	float scalar;
	const held_value_t *value;
	static const char clockend[] = "clockend viewing 100 0 0 0\n";
	static const char badend[] = "clockend viewing 101 0 0 0\n";
	static const char monitor[] = "clk 5.000 0.020 0.050 0.000 0 0.000 0.000 0.000\n";
	static const char badmonitor[] = "clk 5.000 0.020 0.050 0.000 0 0.000 0.000 0.001\n";
	static const char hold[] = "hold 100 0\n", badhold[] = "hold 101 0\n";
	static const char endline[] = "end 0 1 0 0 1 1 0 0 1 0 0 1 0\n", badendline[] = "end 0 1 0 0 2 1 0 0 1 0 0 1 0\n";
	static const char endpark[] = "end 0 1 0 0 1 1 0 0 1 0 0 1 0 0\n";
	static const char badpark[] = "end 0 1 0 0 1 1 0 0 1 0 0 1 0 1\n";
	static const char flags[] = "flags 131072    \n", badflags[] = "flags 131073    \n";
	static const uint32_t counts[13] = {0,1,0,0,1,1,0,0,1,0,0,1,0};
#define HC_TEST(x) do {++*checks; if (!(x)) ++failures;} while (0)
	*checks = 0;
	Held_PhysicalTestRecord(&ctx,HELD_RECORD_HCTX);
	SV_HeldRecordU(&ctx,"context",1); SV_HeldRecordU(&ctx,"rules",1);
	SV_HeldRecordF(&ctx,"ctx_slow",5); SV_HeldRecordF(&ctx,"ctx_hyst",.02f);
	SV_HeldRecordF(&ctx,"ctx_allow",.05f); SV_HeldRecordF(&ctx,"ctx_freecredits",1);
	SV_HeldRecordF(&ctx,"ctx_gap",.5f);
	seed.state = 2; seed.counted = 1; seed.starttick = seed.lastmt = 100;
	Held_PhysicalTestRecord(&initial,HELD_RECORD_HINIT);
	SV_HeldRecordU(&initial,"context",1); SV_HeldRecordL(&initial,"execution",1);
	Held_ReplayClockTestSnapshot(&initial,&seed);
	HC_TEST(SV_HeldReplayClockBegin(&replay,&ctx,&initial));
	HC_TEST(SV_HeldReplayClockCompare(&replay,&initial));
	HC_TEST(SV_HeldClockNonce(&replay) && replay.clock.flags == 131072);
	HC_TEST(SV_HeldClockNonce(&replay) && replay.clock.flags == 131072);
	HC_TEST(Held_LegacyClockEnd(clockend,sizeof(clockend)-1,&replay) == 1);
	HC_TEST(Held_LegacyClockEnd(badend,sizeof(badend)-1,&replay) == 2);
	HC_TEST(Held_LegacyClockMonitor(monitor,sizeof(monitor)-1,&replay) == 1);
	HC_TEST(Held_LegacyClockMonitor(badmonitor,sizeof(badmonitor)-1,&replay) == 2);
	HC_TEST(Held_LegacyHold(hold,sizeof(hold)-1,100,replay.clock.frzticks));
	HC_TEST(!Held_LegacyHold(badhold,sizeof(badhold)-1,100,replay.clock.frzticks));
	HC_TEST(Held_LegacyEnd(endline,sizeof(endline)-1,counts,0));
	HC_TEST(!Held_LegacyEnd(badendline,sizeof(badendline)-1,counts,0));
	HC_TEST(Held_LegacyEnd(endpark,sizeof(endpark)-1,counts,1));
	HC_TEST(!Held_LegacyEnd(badpark,sizeof(badpark)-1,counts,1));
	HC_TEST(!Held_LegacyEnd(endline,sizeof(endline)-1,counts,1));
	HC_TEST(!Held_LegacyEnd(endpark,sizeof(endpark)-1,counts,0));
	HC_TEST(Held_LegacyFlags(flags,sizeof(flags)-1,&replay));
	HC_TEST(!Held_LegacyFlags(badflags,sizeof(badflags)-1,&replay));
	Held_PhysicalTestRecord(&observed,HELD_RECORD_HFREEZE);
	Held_ReplayClockTestSnapshot(&observed,&replay.clock);
	HC_TEST(SV_HeldReplayClockCompare(&replay,&observed));
	for (i = 0; i < countof(held_clock_fields); ++i)
	{
		value = SV_HeldRecordGet(&observed,held_clock_fields[i].wire,'F');
		scalar = value->f; memcpy(&bits,&scalar,sizeof(bits)); bits ^= 1;
		memcpy(&scalar,&bits,sizeof(scalar));
		SV_HeldRecordF(&observed,held_clock_fields[i].wire,scalar);
		HC_TEST(!SV_HeldReplayClockCompare(&replay,&observed));
		Held_ReplayClockTestSnapshot(&observed,&replay.clock);
	}
	HC_TEST(!SV_HeldReplayClockBegin(&replay,&ctx,&initial));
	empty.failed = 1;
	HC_TEST(!memcmp(&replay,&empty,sizeof(replay)));
	Held_PhysicalWipe(&replay,sizeof(replay)); Held_PhysicalWipe(&seed,sizeof(seed));
	Held_PhysicalWipe(&ctx,sizeof(ctx)); Held_PhysicalWipe(&initial,sizeof(initial)); Held_PhysicalWipe(&observed,sizeof(observed));
	return failures;
#undef HC_TEST
}

static held_zone_t *held_zone_owner;
#define HELD_RPC_FLOATS (OFS_PARM7+3-OFS_RETURN)
static int Held_ZoneEnvironment(void)
{
	return svs.gametype == GT_PROGS && svprogfuncs && progstype == PROG_QW && sv.world.worldmodel;
}
static int Held_ZoneCurrent(void *vm, model_t *world, unsigned int epoch, uint64_t progs_epoch)
{
	return vm == svprogfuncs && world == sv.world.worldmodel && epoch == svs.spawncount &&
		progs_epoch && progs_epoch == SV_HeldProgsEpoch();
}
static int Held_ZoneHex(unsigned char c)
{
	if (c >= '0' && c <= '9') return c-'0';
	if (c >= 'a' && c <= 'f') return c-'a'+10;
	return -1;
}
int SV_HeldZoneFingerprint(unsigned char digest[32])
{
	globalvars_t *pr_globals;
	float rpc[HELD_RPC_FLOATS];
	const char *text;
	func_t function;
	void *vm = svprogfuncs;
	model_t *world = sv.world.worldmodel;
	unsigned int i, epoch = svs.spawncount;
	uint64_t progs_epoch = SV_HeldProgsEpoch();
	int a, b, result = 0;
	if (!digest) return 0;
	memset(digest,0,32);
	if (!progs_epoch || !Held_ZoneEnvironment() || !(function = PR_FindFunction(svprogfuncs,"SV_VerifyZoneFullPin",PR_ANY))) return 0;
	pr_globals = PR_globals(svprogfuncs,PR_CURRENT);
	memcpy(rpc,(float*)pr_globals+OFS_RETURN,sizeof(rpc));
	PR_ExecuteProgram(svprogfuncs,function);
	if (!Held_ZoneCurrent(vm,world,epoch,progs_epoch))
	{
		Held_PhysicalWipe(rpc,sizeof(rpc));
		return 0; /* Old VM/global storage must not be touched after reload. */
	}
	text = PR_GetString(svprogfuncs,G_INT(OFS_RETURN));
	if (text && strlen(text) == 64)
	{
		for (i = 0; i < 32; ++i)
		{
			a = Held_ZoneHex(text[2*i]); b = Held_ZoneHex(text[2*i+1]);
			if (a < 0 || b < 0) break;
			digest[i] = (a<<4)|b;
		}
		result = i == 32;
	}
	memcpy((float*)pr_globals+OFS_RETURN,rpc,sizeof(rpc));
	Held_PhysicalWipe(rpc,sizeof(rpc));
	if (!result) Held_PhysicalWipe(digest,32);
	return result;
}
void SV_HeldZoneEnd(held_zone_t *z)
{
	unsigned int i;
	if (!z) return;
	if (held_zone_owner == z)
	{
		if (z->active && Held_ZoneCurrent(z->vm,z->world,z->epoch,z->progs_epoch))
			for (i = 0; i < countof(z->globals); ++i)
				memcpy(z->globals[i],&z->saved[i],sizeof(float));
		held_zone_owner = NULL;
	}
	Held_PhysicalWipe(z,sizeof(*z));
}
static int Held_ZoneFail(held_zone_t *z)
{
	SV_HeldZoneEnd(z);
	if (z) z->failed = 1;
	return 0;
}
int SV_HeldZoneBegin(held_zone_t *z, const unsigned char expected[32], const float seed[5])
{
	static const char *names[] = {"vf_evzone","vf_azone","vf_track","vf_stagerun","vf_startseg",
		"zs_az","zs_ez","zs_cp","zs_cancel"};
	globalvars_t *pr_globals;
	float rpc[HELD_RPC_FLOATS];
	unsigned char actual[32];
	eval_t *value;
	etype_t type;
	func_t begin;
	unsigned int i, j;
	float count;
	uint64_t progs_epoch = SV_HeldProgsEpoch();
	if (!z || z->active || z->failed || held_zone_owner || !expected || !seed || !Held_ZoneEnvironment() || !progs_epoch) return Held_ZoneFail(z);
	value = PR_FindGlobal(svprogfuncs,"zone_count",PR_ANY,&type);
	if (!value || (type&0xfff) != ev_float) return Held_ZoneFail(z);
	count = value->_float;
	if (!isfinite(count) || count < 1 || count > 256 || count != floorf(count)) return Held_ZoneFail(z);
	for (i = 0; i < 5; ++i)
		if (!isfinite(seed[i]) || seed[i] != floorf(seed[i])) return Held_ZoneFail(z);
	if (seed[0] < -1 || seed[0] >= count || seed[1] < -1 || seed[1] >= count ||
		seed[2] < 0 || seed[2] > 16777216 || (seed[3] != 0 && seed[3] != 1) ||
		seed[4] < 0 || seed[4] > count) return Held_ZoneFail(z);
	if (!SV_HeldZoneFingerprint(actual)) return Held_ZoneFail(z);
	i = !memcmp(actual,expected,32); Held_PhysicalWipe(actual,sizeof(actual));
	if (!i || !(begin = PR_FindFunction(svprogfuncs,"SV_VerifyBegin",PR_ANY)) ||
		!(z->step = PR_FindFunction(svprogfuncs,"SV_VerifyStep",PR_ANY))) return Held_ZoneFail(z);
	for (i = 0; i < countof(names); ++i)
	{
		value = PR_FindGlobal(svprogfuncs,names[i],PR_ANY,&type);
		if (!value || (type&0xfff) != ev_float) return Held_ZoneFail(z);
		z->globals[i] = &value->_float;
		for (j = 0; j < i; ++j) if (z->globals[i] == z->globals[j]) return Held_ZoneFail(z);
		memcpy(&z->saved[i],z->globals[i],sizeof(float));
	}
	z->vm = svprogfuncs; z->world = sv.world.worldmodel; z->epoch = svs.spawncount; z->progs_epoch = progs_epoch;
	memcpy(z->digest,expected,32); z->active = 1; held_zone_owner = z;
	pr_globals = PR_globals(svprogfuncs,PR_CURRENT);
	memcpy(rpc,(float*)pr_globals+OFS_RETURN,sizeof(rpc));
	for (i = 0; i < 5; ++i) G_FLOAT(OFS_PARM0+3*i) = seed[i];
	PR_ExecuteProgram(svprogfuncs,begin);
	if (!Held_ZoneCurrent(z->vm,z->world,z->epoch,z->progs_epoch))
	{
		Held_PhysicalWipe(rpc,sizeof(rpc));
		return Held_ZoneFail(z);
	}
	memcpy((float*)pr_globals+OFS_RETURN,rpc,sizeof(rpc)); Held_PhysicalWipe(rpc,sizeof(rpc));
	for (i = 0; i < 5; ++i) if (*z->globals[i] != seed[i]) return Held_ZoneFail(z);
	return 1;
}
int SV_HeldZoneStep(void *context, const float lastorg[3], const float origin[3], const float mins[3], const float maxs[3])
{
	held_zone_t *z = context;
	globalvars_t *pr_globals;
	float rpc[HELD_RPC_FLOATS], action;
	unsigned int i;
	if (!z || held_zone_owner != z || !z->active || z->failed ||
		!Held_ZoneCurrent(z->vm,z->world,z->epoch,z->progs_epoch) || !lastorg || !origin || !mins || !maxs) goto failed;
	for (i = 0; i < 3; ++i)
		if (!isfinite(lastorg[i]) || !isfinite(origin[i]) || mins[i] != (i == 2 ? 0 : -16) ||
			maxs[i] != (i == 2 ? 62 : 16)) goto failed;
	pr_globals = PR_globals(svprogfuncs,PR_CURRENT);
	memcpy(rpc,(float*)pr_globals+OFS_RETURN,sizeof(rpc));
	VectorCopy(lastorg,G_VECTOR(OFS_PARM0)); VectorCopy(origin,G_VECTOR(OFS_PARM1)); VectorCopy(maxs,G_VECTOR(OFS_PARM2));
	PR_ExecuteProgram(svprogfuncs,z->step);
	if (!Held_ZoneCurrent(z->vm,z->world,z->epoch,z->progs_epoch)) goto failed;
	action = G_FLOAT(OFS_RETURN);
	memcpy((float*)pr_globals+OFS_RETURN,rpc,sizeof(rpc)); Held_PhysicalWipe(rpc,sizeof(rpc));
	if (!isfinite(action) || action < 0 || action > 3 || action != floorf(action)) goto failed;
	return (int)action;
failed:
	Held_PhysicalWipe(rpc,sizeof(rpc));
	Held_ZoneFail(z);
	return -1;
}

static int Held_ZoneTestFile(const char *name, const unsigned char loaded[32])
{
	vfsfile_t *file;
	qofs_t size;
	unsigned char *data, disk[32];
	int result;
	file = FS_OpenVFS(name,"rb",FS_GAME);
	if (!file) return 0;
	size = VFS_GETLEN(file);
	if (size <= 0 || size > 512u*1024u*1024u) {VFS_CLOSE(file); return 0;}
	data = BZF_Malloc(size);
	if (!data) {VFS_CLOSE(file); return 0;}
	result = VFS_READ(file,data,size) == (int)size;
	VFS_CLOSE(file);
	if (result) result = CalcHash(&hash_sha2_256,disk,32,data,size) == 32 && !memcmp(disk,loaded,32);
	Held_PhysicalWipe(data,size); BZ_Free(data); Held_PhysicalWipe(disk,sizeof(disk));
	return result;
}
static float *Held_ZoneTestGlobal(const char *name, etype_t expected)
{
	etype_t type;
	eval_t *value = PR_FindGlobal(svprogfuncs,name,PR_ANY,&type);
	return value && (type&0xfff) == expected ? &value->_float : NULL;
}
unsigned int SV_HeldZoneTest(unsigned int *checks)
{
	static const char *names[] = {"vf_evzone","vf_azone","vf_track","vf_stagerun","vf_startseg",
		"zs_az","zs_ez","zs_cp","zs_cancel"};
	held_zone_t zone = {0}, wrong = {0};
	globalvars_t *pr_globals;
	float *globals[9], saved[9], rpc[HELD_RPC_FLOATS];
	float *points, *types, *tracks, *centres, *bottom, *top, *countptr;
	float original, changed, seed[5] = {-1,-1,0,0,0};
	vec3_t point, mins = {-16,-16,0}, maxs = {16,16,62};
	unsigned char digest[32], repeat[32], bad[32], loaded[32], zero[32] = {0}, bsp[32], plugin[32], engine[32];
	unsigned int i, count, failures = 0;
	uint32_t bits;
	uint64_t progs_epoch, world_epoch;
	int end = -1, modules, unavailable;
#define HZ_TEST(x) do {++*checks; if (!(x)) {++failures; Con_Printf("held zone control failed at %i\n",__LINE__);}} while (0)
#define HZ_REQUIRE(x) do {unsigned int before = failures; HZ_TEST(x); if (failures != before) goto cleanup;} while (0)
	*checks = 0;
	HZ_REQUIRE(Held_ZoneEnvironment() && !held_zone_owner);
	HZ_TEST(SV_HeldProgsFingerprint(loaded) && Held_ZoneTestFile(svs.progsnames[0],loaded));
	progs_epoch = SV_HeldProgsEpoch();
	HZ_TEST(progs_epoch != 0);
	modules = svs.numprogs; svs.numprogs = 2;
	unavailable = !SV_HeldProgsFingerprint(bad) && SV_HeldProgsEpoch() == 0;
	svs.numprogs = modules;
	HZ_TEST(unavailable && !memcmp(bad,zero,32));
	HZ_TEST(SV_HeldProgsFingerprint(repeat) && !memcmp(loaded,repeat,32) && SV_HeldProgsEpoch() == progs_epoch);
	world_epoch = Mod_HeldWorldArtifacts(sv.world.worldmodel,bsp,plugin);
	HZ_TEST(world_epoch && memcmp(bsp,zero,32) && memcmp(plugin,zero,32));
	HZ_TEST(Held_ZoneTestFile(sv.world.worldmodel->name,bsp));
	HZ_TEST(Mod_HeldWorldArtifacts(sv.world.worldmodel,repeat,bad) == world_epoch && !memcmp(bsp,repeat,32) && !memcmp(plugin,bad,32));
	HZ_TEST(!Mod_HeldWorldArtifacts(NULL,repeat,bad) && !memcmp(repeat,zero,32) && !memcmp(bad,zero,32));
#ifdef PLUGINS
	HZ_TEST(Plug_ArtifactFingerprint(NULL,NULL,engine) == 1 && memcmp(engine,zero,32));
	HZ_TEST(Plug_ArtifactFingerprint(NULL,NULL,repeat) == 1 && !memcmp(engine,repeat,32));
	HZ_TEST(!Plug_ArtifactFingerprint(&zone,NULL,repeat) && !memcmp(repeat,zero,32));
	HZ_TEST(!Plug_ArtifactFingerprint(NULL,&zone,repeat) && !memcmp(repeat,zero,32));
#endif
	pr_globals = PR_globals(svprogfuncs,PR_CURRENT);
	memcpy(rpc,(float*)pr_globals+OFS_RETURN,sizeof(rpc));
	for (i = 0; i < countof(names); ++i)
	{
		globals[i] = Held_ZoneTestGlobal(names[i],ev_float);
		HZ_REQUIRE(globals[i] != NULL);
		memcpy(&saved[i],globals[i],sizeof(float));
	}
	points = Held_ZoneTestGlobal("zone_pt[0]",ev_vector);
	types = Held_ZoneTestGlobal("zone_type[0]",ev_float); tracks = Held_ZoneTestGlobal("zone_track[0]",ev_float);
	centres = Held_ZoneTestGlobal("zone_centre[0]",ev_vector);
	bottom = Held_ZoneTestGlobal("zone_bottom[0]",ev_float); top = Held_ZoneTestGlobal("zone_top[0]",ev_float);
	countptr = Held_ZoneTestGlobal("zone_count",ev_float);
	HZ_REQUIRE(points && types && tracks && centres && bottom && top && countptr);
	HZ_REQUIRE(Held_ZoneTestGlobal("zone_pt[2047]",ev_vector) == points+3*2047 &&
		Held_ZoneTestGlobal("zone_type[255]",ev_float) == types+255 &&
		Held_ZoneTestGlobal("zone_track[255]",ev_float) == tracks+255 &&
		Held_ZoneTestGlobal("zone_centre[255]",ev_vector) == centres+3*255 &&
		Held_ZoneTestGlobal("zone_bottom[255]",ev_float) == bottom+255 &&
		Held_ZoneTestGlobal("zone_top[255]",ev_float) == top+255);
	HZ_REQUIRE(isfinite(*countptr) && *countptr >= 1 && *countptr <= 256 && *countptr == floorf(*countptr));
	count = *countptr;
	HZ_REQUIRE(SV_HeldZoneFingerprint(digest));
	HZ_TEST(SV_HeldZoneFingerprint(repeat) && !memcmp(digest,repeat,32));
	memcpy(&original,points,sizeof(float));
	changed = nextafterf(original,original >= 0 ? INFINITY : -INFINITY);
	memcpy(points,&changed,sizeof(float));
	HZ_TEST(SV_HeldZoneFingerprint(repeat) && memcmp(digest,repeat,32));
	bits = 0x7fc00000u; memcpy(points,&bits,sizeof(bits));
	HZ_TEST(!SV_HeldZoneFingerprint(repeat));
	bits = 0x7f800000u; memcpy(points,&bits,sizeof(bits));
	HZ_TEST(!SV_HeldZoneFingerprint(repeat));
	memcpy(points,&original,sizeof(float));
	HZ_TEST(SV_HeldZoneFingerprint(repeat) && !memcmp(digest,repeat,32));
	memcpy(bad,digest,32); bad[0] ^= 1;
	HZ_TEST(!SV_HeldZoneBegin(&wrong,bad,seed) && wrong.failed);
	SV_HeldZoneEnd(&wrong);
	for (i = 0; i < count; ++i)
		if (types[i] == 1 && tracks[i] == 0) {end = i; break;} /* sh_zones ZONE_END1 */
	HZ_REQUIRE(end >= 0);
	VectorCopy(centres+3*end,point); point[2] = (bottom[end]+top[end])*.5f;
	HZ_REQUIRE(SV_HeldZoneBegin(&zone,digest,seed));
	HZ_TEST(SV_HeldZoneStep(&zone,point,point,mins,maxs) == 1);
	HZ_TEST(SV_HeldZoneStep(&zone,point,point,mins,maxs) == 0);
	SV_HeldZoneEnd(&zone);
	for (i = 0; i < countof(names); ++i) HZ_TEST(!memcmp(globals[i],&saved[i],sizeof(float)));
	HZ_TEST(!memcmp((float*)pr_globals+OFS_RETURN,rpc,sizeof(rpc)));
cleanup:
	SV_HeldZoneEnd(&zone); SV_HeldZoneEnd(&wrong);
	Held_PhysicalWipe(digest,sizeof(digest)); Held_PhysicalWipe(repeat,sizeof(repeat)); Held_PhysicalWipe(bad,sizeof(bad));
	Held_PhysicalWipe(loaded,sizeof(loaded)); Held_PhysicalWipe(saved,sizeof(saved)); Held_PhysicalWipe(rpc,sizeof(rpc));
	Held_PhysicalWipe(bsp,sizeof(bsp)); Held_PhysicalWipe(plugin,sizeof(plugin)); Held_PhysicalWipe(engine,sizeof(engine));
	return failures;
#undef HZ_TEST
#undef HZ_REQUIRE
}

void SV_HeldTimelineEnd(held_timeline_t *t)
{
	if (!t) return;
	SV_HeldPhysicalEnd(&t->physical);
	Held_PhysicalWipe(t,sizeof(*t));
}
static int Held_TimelineFail(held_timeline_t *t, unsigned int failure)
{
	if (t)
	{
		SV_HeldTimelineEnd(t);
		t->failure = failure;
	}
	return 0;
}
int SV_HeldTimelineBegin(held_timeline_t *t, const held_record_t *context,
	const held_record_t *initial, model_t *world, held_zone_step_t zone_step, void *zone_context)
{
	float now;
	if (!t || t->failure || t->clock.initialized || t->physical.saved || !zone_step)
		return Held_TimelineFail(t,1);
	if (!SV_HeldReplayClockBegin(&t->clock,context,initial) ||
		!SV_HeldPhysicalBegin(&t->physical,context,initial,world)) return Held_TimelineFail(t,1);
	if (t->clock.clock.lastmt != (float)t->physical.raw_moveticks) return Held_TimelineFail(t,1);
	now = (float)t->physical.raw_moveticks*movevars.ticrate;
	now = now + pmove.msec_carry;
	if (t->clock.clock.lastsec != now) return Held_TimelineFail(t,1);
	/* Native format inherits the actual source's public viewing-clock policy.
	 * Its source initial predicate proves the inactive viewing lane starts zero. */
	t->clock.view_policy = 1;
	t->zone_step = zone_step; t->zone_context = zone_context;
	return 1;
}
static int Held_TimelineAdvance(held_timeline_t *t)
{
	int action = 0;
	if (!t->post || t->transition) return Held_TimelineFail(t,1);
	if (t->advanced) return 1;
	if (!SV_HeldClockPacket(&t->clock)) return Held_TimelineFail(t,1);
	if (!t->clock.clock.frzon)
	{
		/* Shared pure zone scan, never an action copied from HTERMINAL. */
		action = t->zone_step(t->zone_context,t->clock.clock.lastorg,pmove.origin,pmove.player_mins,pmove.player_maxs);
		if (action != 0 && action != 1) return Held_TimelineFail(t,1);
	}
	if (action == 1)
	{
		if (!SV_HeldClockFinish(&t->clock)) return Held_TimelineFail(t,1);
		t->finished = 1;
	}
	else if (!SV_HeldClockRecordFrame(&t->clock)) return Held_TimelineFail(t,1);
	t->advanced = 1;
	return 1;
}
int SV_HeldTimelineEvent(held_timeline_t *t, const held_record_t *r,
	const usercmd_t *ordinary, float ordinary_msec)
{
	const held_value_t *value;
	unsigned int kind, phase, old;
	float time, frametime;
	if (!t || !r || t->failure || !t->clock.initialized || !t->physical.saved)
		return Held_TimelineFail(t,1);
#define TU(n,d) do {value = SV_HeldRecordGet(r,n,'U'); if (!value) goto unsupported; d = value->u;} while (0)
#define TF(n,d) do {value = SV_HeldRecordGet(r,n,'F'); if (!value) goto unsupported; d = value->f;} while (0)
	switch (r->family)
	{
	case HELD_RECORD_HSCOPE:
		TU("kind",kind); TU("phase",phase);
		if (kind == 2 && phase == 1)
		{
			TF("time",time); TF("frametime",frametime);
			if (!SV_HeldClockFrame(&t->clock,time,frametime)) goto unsupported;
		}
		if (kind == 4 && phase == 2)
			if (!SV_HeldClockNonce(&t->clock)) goto unsupported;
		break;
	case HELD_RECORD_HPRE: case HELD_RECORD_HPEND:
		/* Full dispatcher/scene/source binding remains the native caller's duty. */
		break;
	case HELD_RECORD_HMOVE: case HELD_RECORD_HORDINARY:
		if (t->post || !SV_HeldPhysicalStep(&t->physical,r,ordinary,ordinary_msec)) goto physical;
		break;
	case HELD_RECORD_HDONE:
		if (!SV_HeldPhysicalDone(&t->physical,r)) goto physical;
		break;
	case HELD_RECORD_HENTRY: case HELD_RECORD_HRELEASE:
		if (t->post || !SV_HeldPhysicalControl(&t->physical,r)) goto physical;
		if (!SV_HeldClockRequest(&t->clock,r->family == HELD_RECORD_HENTRY)) goto unsupported;
		break;
	case HELD_RECORD_HMAINT:
		TU("site",kind);
		if (kind == 3 && !Held_TimelineAdvance(t)) return 0;
		if (!SV_HeldPhysicalControl(&t->physical,r)) goto physical;
		break;
	case HELD_RECORD_HPOST:
		TU("phase",phase);
		if (phase == 1)
		{
			if (t->post || t->physical.pending || t->finished) goto unsupported;
			TF("time",time);
			old = t->clock.clock.frzon != 0;
			if (!SV_HeldClockAccounting(&t->clock,time,(float)t->physical.raw_moveticks,
				movevars.ticrate,pmove.msec_carry)) goto unsupported;
			t->transition = old == (t->clock.clock.frzon != 0) ? 0 : (old ? 2 : 1);
			t->post = 1; t->advanced = 0;
		}
		else if (phase == 2)
		{
			if (!Held_TimelineAdvance(t)) return 0;
			if (t->finished && !t->terminal) goto unsupported;
			if (!SV_HeldClockLastOrigin(&t->clock,pmove.origin)) goto unsupported;
			t->post = 0; t->advanced = 0;
		}
		else goto unsupported;
		break;
	case HELD_RECORD_HFREEZE: case HELD_RECORD_HTHAW:
		kind = r->family == HELD_RECORD_HFREEZE ? 1 : 2;
		if (!t->post || t->advanced || t->transition != kind) goto unsupported;
		if (!SV_HeldReplayClockCompare(&t->clock,r)) goto mismatch;
		t->transition = 0;
		if (kind == 2 && !SV_HeldPhysicalControl(&t->physical,r)) goto physical;
		break;
	case HELD_RECORD_HOBS:
		TU("boundary",kind);
		if (kind == 4)
		{
			if (!Held_TimelineAdvance(t)) return 0;
			if (!t->finished) goto mismatch;
		}
		if (!SV_HeldPhysicalObserve(&t->physical,r)) goto physical;
		break;
	case HELD_RECORD_HTERMINAL:
		if (!Held_TimelineAdvance(t)) return 0;
		if (!t->finished || t->terminal || !SV_HeldReplayClockCompare(&t->clock,r)) goto mismatch;
		t->terminal = 1;
		break;
	default: goto unsupported;
	}
	return 1;
physical:
	return Held_TimelineFail(t,t->physical.failure == 2 ? 2 : 1);
mismatch:
	return Held_TimelineFail(t,2);
unsupported:
	return Held_TimelineFail(t,1);
#undef TU
#undef TF
}
int SV_HeldTimelineFinish(held_timeline_t *t)
{
	if (!t || t->failure || !t->terminal || !t->finished || t->post ||
		t->clock.post_phase || t->clock.clock.state != 3 || t->physical.pending ||
		!t->physical.guard_done || !t->physical.thawed || !t->physical.released ||
		t->clock.clock.lastmt != (float)t->physical.raw_moveticks)
		return Held_TimelineFail(t,1);
	SV_HeldTimelineEnd(t);
	return 1;
}

static int Held_TrajectoryContext(const held_record_t *file, const held_record_t *native)
{
	const char *p = held_schemas[HELD_RECORD_HCTX].fields, *colon, *end;
	const held_value_t *a, *b;
	char name[64], type;
	unsigned char zero[32] = {0};
	size_t length, width;
	if (!file || !native || file->family != HELD_RECORD_HCTX || native->family != HELD_RECORD_HCTX) return 0;
	while (*p)
	{
		colon = strchr(p,':'); end = strchr(p,' '); if (!end) end = p+strlen(p);
		if (!colon || colon+2 != end || (length = colon-p) >= sizeof(name)) return 0;
		memcpy(name,p,length); name[length] = 0; type = colon[1];
		if (strcmp(name,"generation") && strcmp(name,"ordinal") && strcmp(name,"message") && strcmp(name,"execution"))
		{
			a = SV_HeldRecordGet(file,name,type); b = SV_HeldRecordGet(native,name,type);
			width = type == 'D' ? 32 : type == 'G' ? 16 : type == 'L' ? 8 : 4;
			if (!a || !b || memcmp(a,b,width) || (type == 'D' && !memcmp(b,zero,32))) return 0;
		}
		p = *end ? end+1 : end;
	}
	return 1;
}
static int Held_TrajectoryTag(const char *line, size_t size, const char *tag)
{
	size_t length = strlen(tag);
	return size > length && !memcmp(line,tag,length) && Held_InputWhite((unsigned char)line[length]);
}
typedef struct
{
	const char *cursor, *end;
	unsigned int normal, frozen, epoch, recorded_epoch, mode, physcrc, pcset, neutral_floor, failure, relative_counts;
} held_body_t;
static int Held_BodyPeek(const held_body_t *body, const char **line, size_t *size)
{
	const char *newline, *p;
	if (!body->cursor || body->cursor >= body->end) return 0;
	newline = memchr(body->cursor,'\n',body->end-body->cursor); if (!newline) return 0;
	p = body->cursor; while (p < newline && Held_InputWhite((unsigned char)*p)) ++p;
	if (p == newline || newline+1-p > HELD_MAX_LINE) return 0;
	*line = p; *size = newline+1-p;
	return 1;
}
static int Held_BodyConsume(held_body_t *body, const char *line, size_t size)
{
	const char *actual;
	size_t length;
	if (!Held_BodyPeek(body,&actual,&length) || actual != line || length != size) return 0;
	body->cursor = line+size;
	return 1;
}
static int Held_BodyBootstrap(held_body_t *body, const playermove_t *model, float elapsed)
{
	const char *line, *cursor, *end;
	char word[64];
	size_t size;
	unsigned int i;
	float scalar;
	while (Held_BodyPeek(body,&line,&size) && *line == '-')
	{
		cursor = line; end = line+size;
		for (i = 0; i < 17; ++i)
			if (Held_LegacyWord(&cursor,end,word) != 1 || !Held_InputFloat(word,-FLT_MAX,FLT_MAX,&scalar)) return 0;
		if (Held_LegacyWord(&cursor,end,word) != 0) return 0;
		body->cursor = line+size;
	}
	if (!Held_BodyPeek(body,&line,&size) || !Held_LegacySamplePhysical(line,size,model,elapsed,1,&body->failure)) return 0;
	body->cursor = line+size;
	if (!Held_BodyPeek(body,&line,&size) || !Held_TrajectoryTag(line,size,"zseed")) return 0;
	body->cursor = line+size;
	return 1;
}
/* Reproduce RecState emission, not an opportunistic search for a matching pin. */
static int Held_BodyState(held_body_t *body, const held_record_t *context, const held_clock_replay_t *clock)
{
	const char *line;
	size_t size;
	if (!(body->normal+body->frozen) || (clock->clock.frzon && clock->clock.freeze)) return 1;
	if (body->epoch != body->recorded_epoch)
	{
		if (!Held_BodyPeek(body,&line,&size) ||
			!Held_LegacyPM(line,size,context,body->mode,clock->clock.packet,body->normal+body->frozen-1)) return 0;
		body->cursor = line+size; body->recorded_epoch = body->epoch;
	}
	if (!body->pcset)
	{
		if (!Held_BodyPeek(body,&line,&size) ||
			!Held_LegacyPE(line,size,clock->clock.packet,body->normal+body->frozen-1,body->physcrc)) return 0;
		body->cursor = line+size; body->pcset = 1;
	}
	return 1;
}
static int Held_BodyInput(held_body_t *body, const held_input_t *expected, unsigned int index)
{
	held_input_t parsed;
	const char *line;
	size_t size;
	uint32_t packet, movetick;
	int result;
	if (index != body->normal+body->frozen || !Held_BodyPeek(body,&line,&size)) return 0;
	if (Held_TrajectoryTag(line,size,"ride"))
	{
		if (index || body->neutral_floor || expected->kind != 1 ||
			!SV_HeldNeutralRideParse(line,size,&packet,&movetick) || packet != expected->packet ||
			movetick != expected->movetick) return 0;
		body->cursor = line+size; body->neutral_floor = 1;
		if (!Held_BodyPeek(body,&line,&size)) return 0;
	}
	result = SV_HeldInputParse(line,size,&parsed) && !memcmp(&parsed,expected,sizeof(parsed));
	Held_PhysicalWipe(&parsed,sizeof(parsed));
	if (!result) return 0;
	if (expected->kind == 1) ++body->normal; else ++body->frozen;
	body->cursor = line+size;
	return 1;
}
/* Existing ordinary mc is display/device evidence, never a physical input. */
static int Held_BodyCounts(held_body_t *body, float packet, int pinned)
{
	const char *line;
	char words[12][64];
	size_t size;
	int64_t integer;
	float scalar;
	unsigned int i;
	int result = 1;
	held_counts_t relative;
	if (!Held_BodyPeek(body,&line,&size) || !Held_TrajectoryTag(line,size,"mc")) goto done;
	result = 0;
	if (body->relative_counts)
	{
		if (!SV_HeldRelativeCountsParse(line,size,0,&relative)) goto done;
		if (relative.ring[0] != relative.read[0] || relative.ring[1] != relative.read[1])
		{
			body->failure = 2;
			goto done;
		}
	}
	if (pinned || !(body->normal+body->frozen) || Held_InputWords(line,size,words) != 8 ||
		!Held_InputInt(words[1],0,16777216,&integer) || (float)integer != packet ||
		!Held_InputInt(words[2],0,HELD_MAX_PIECES-1,&integer) || (unsigned int)integer != body->normal+body->frozen-1 ||
		!Held_InputInt(words[7],0,16777216,&integer)) goto done;
	for (i = 3; i < 7; ++i) if (!Held_InputFloat(words[i],-FLT_MAX,FLT_MAX,&scalar)) goto done;
	body->cursor = line+size; result = 1;
done:
	Held_PhysicalWipe(words,sizeof(words));
	return result;
}
static int Held_BodySample(held_body_t *body, const playermove_t *model, const held_clock_replay_t *clock, int pinned)
{
	const char *line;
	size_t size;
	if (clock->clock.state != clock->context.runningstate || clock->clock.frzon) return 1;
	if (!Held_BodyPeek(body,&line,&size) || !Held_LegacySamplePhysical(line,size,model,SV_HeldClockSeconds(clock),0,&body->failure)) return 0;
	body->cursor = line+size;
	return Held_BodyCounts(body,clock->clock.packet,pinned);
}
static int Held_BodyFooter(held_body_t *body, float ticks)
{
	const char *line;
	char words[12][64];
	size_t size;
	unsigned int seen = 0, bit;
	int64_t integer;
	while (body->cursor < body->end)
	{
		if (!Held_BodyPeek(body,&line,&size)) return 0;
		if (Held_TrajectoryTag(line,size,"clk")) bit = 1;
		else if (Held_TrajectoryTag(line,size,"inend")) bit = 2;
		else if (Held_TrajectoryTag(line,size,"clockend")) bit = 4;
		else if (Held_TrajectoryTag(line,size,"end")) bit = 8;
		else if (Held_TrajectoryTag(line,size,"prof"))
		{
			if (Held_InputWords(line,size,words) != 2 || strcmp(words[1],"ok")) return 0;
			bit = 16;
		}
		else if (Held_TrajectoryTag(line,size,"split"))
		{
			if (Held_InputWords(line,size,words) != 3 || strcmp(words[1],"1") ||
				!Held_InputInt(words[2],0,16777216,&integer) || (float)integer != ticks) return 0;
			bit = 32;
		}
		else return 0;
		if (seen & bit) return 0;
		seen |= bit; body->cursor = line+size;
	}
	Held_PhysicalWipe(words,sizeof(words));
	return (seen & 31u) == 31u;
}

/* Validate the entire supported ordinary payload BEFORE comparisons may stop
 * replay. Reuse target decoders, never construct/import a model from targets.
 * The existing header scan validates aliases, zseed and inend independently. */
static int Held_BodySyntax(const char *start, const char *end, const held_record_t *context, int relative_counts)
{
	held_body_t body = {0};
	held_input_t input;
	held_counts_t counts;
	unsigned int input_rows = 0, count_rows = 0;
	const char *line, *cursor, *finish;
	char words[12][64], word[64];
	size_t size;
	int64_t integer;
	float scalar;
	unsigned int i, fields, seen = 0, bit, seeds = 0, physents = 0;
	uint32_t packet, movetick;
	body.cursor = start; body.end = end;
	while (body.cursor < body.end)
	{
		if (!Held_BodyPeek(&body,&line,&size)) return 0;
		bit = 0;
		if (Held_TrajectoryTag(line,size,"prof")) bit = 16;
		else if (Held_TrajectoryTag(line,size,"clk")) bit = 1;
		else if (Held_TrajectoryTag(line,size,"inend")) bit = 2;
		else if (Held_TrajectoryTag(line,size,"clockend")) bit = 4;
		else if (Held_TrajectoryTag(line,size,"end")) bit = 8;
		else if (Held_TrajectoryTag(line,size,"split")) bit = 32;
		if ((seen && !bit) || (seen & bit)) return 0;
		seen |= bit;
		if (*line == '-')
		{
			cursor = line; finish = line+size;
			for (i = 0; i < 17; ++i)
				if (Held_LegacyWord(&cursor,finish,word) != 1 || !Held_InputFloat(word,-FLT_MAX,FLT_MAX,&scalar)) return 0;
			if (Held_LegacyWord(&cursor,finish,word) != 0) return 0;
		}
		else if (*line >= '0' && *line <= '9')
		{
			if (!Held_LegacySamplePhysical(line,size,NULL,0,0,NULL)) return 0;
		}
		else if (Held_TrajectoryTag(line,size,"seed"))
		{
			if (seeds++ || !Held_LegacySeed(line,size,NULL)) return 0;
		}
		else if (Held_TrajectoryTag(line,size,"zseed") || bit == 2)
		{
			/* Fully checked (exactly once) by the complete header/input scan. */
		}
		else if (Held_TrajectoryTag(line,size,"in") || Held_TrajectoryTag(line,size,"fzin"))
		{
			if (!SV_HeldInputParse(line,size,&input) || (input.flags & ~8u)) return 0;
			++input_rows;
		}
		else if (Held_TrajectoryTag(line,size,"pm"))
		{
			cursor = line; finish = line+size;
			if (Held_LegacyWord(&cursor,finish,word) != 1 ||
				Held_LegacyWord(&cursor,finish,word) != 1 || !Held_InputInt(word,0,16777216,&integer) ||
				Held_LegacyWord(&cursor,finish,word) != 1 || !Held_InputInt(word,0,HELD_MAX_PIECES-1,&integer) ||
				!Held_LegacyPinFields(cursor,finish,context,-2)) return 0;
		}
		else if (Held_TrajectoryTag(line,size,"ride"))
		{
			if (!SV_HeldNeutralRideParse(line,size,&packet,&movetick)) return 0;
		}
		else if (Held_TrajectoryTag(line,size,"viewclock"))
		{
			if (Held_LegacyView(line,size,NULL,-1) != 1) return 0;
		}
		else if (bit == 1)
		{
			if (Held_LegacyClockMonitor(line,size,NULL) != 1) return 0;
		}
		else if (bit == 4)
		{
			if (Held_LegacyClockEnd(line,size,NULL) != 1) return 0;
		}
		else if (bit == 8)
		{
			if (!Held_LegacyEnd(line,size,NULL,relative_counts)) return 0;
		}
		else
		{
			fields = Held_InputWords(line,size,words);
			if (bit == 16)
			{
				if (fields != 2 || strcmp(words[1],"ok")) return 0;
			}
			else if (bit == 32)
			{
				if (fields != 3 || strcmp(words[1],"1") || !Held_InputInt(words[2],0,16777216,&integer)) return 0;
			}
			else if (Held_TrajectoryTag(line,size,"pe"))
			{
				if (physents++ || fields != 4 || !Held_InputInt(words[1],0,16777216,&integer) ||
					!Held_InputInt(words[2],0,HELD_MAX_PIECES-1,&integer) || !Held_InputInt(words[3],0,0xffffff,&integer)) return 0;
			}
			else if (Held_TrajectoryTag(line,size,"mc"))
			{
				if (relative_counts)
				{
					if (!input_rows || !SV_HeldRelativeCountsParse(line,size,!count_rows,&counts) ||
						counts.packet != input.packet || counts.row != input_rows-1) return 0;
					++count_rows;
				}
				if (fields != 8 || !Held_InputInt(words[1],0,16777216,&integer) ||
					!Held_InputInt(words[2],0,HELD_MAX_PIECES-1,&integer) || !Held_InputInt(words[7],0,16777216,&integer)) return 0;
				for (i = 3; i < 7; ++i) if (!Held_InputFloat(words[i],-FLT_MAX,FLT_MAX,&scalar)) return 0;
			}
			else if (Held_TrajectoryTag(line,size,"hold"))
			{
				if (fields != 3 || !Held_InputInt(words[1],0,16777216,&integer) || !Held_InputInt(words[2],0,16777216,&integer)) return 0;
			}
			else if (Held_TrajectoryTag(line,size,"warp"))
			{
				if (fields != 12 || strcmp(words[4],"pin") || !Held_InputInt(words[1],0,16777216,&integer) ||
					!Held_InputInt(words[2],0,16777216,&integer) || !Held_InputInt(words[3],0,HELD_MAX_PIECES-1,&integer) ||
					!Held_InputInt(words[11],0,1,&integer)) return 0;
				for (i = 5; i < 11; ++i) if (!Held_InputFloat(words[i],-FLT_MAX,FLT_MAX,&scalar)) return 0;
			}
			else return 0;
		}
		body.cursor = line+size;
	}
	return seeds == 1 && physents == 1 && (seen & 31u) == 31u;
}

static int Held_BodyBeforeEvent(held_body_t *body, const held_record_t *context,
	const held_clock_replay_t *clock, const held_record_t *event)
{
	const held_value_t *phase;
	if (event->family != HELD_RECORD_HPRE && event->family != HELD_RECORD_HPOST) return 1;
	phase = SV_HeldRecordGet(event,"phase",'U'); if (!phase) return 0;
	if ((event->family == HELD_RECORD_HPRE && phase->u == 2) ||
		(event->family == HELD_RECORD_HPOST && phase->u == 1)) return Held_BodyState(body,context,clock);
	return 1;
}
static int Held_BodyAfterEvent(held_body_t *body, const playermove_t *model,
	const held_clock_replay_t *clock, int pinned, const held_record_t *event)
{
	const held_value_t *phase;
	if (event->family != HELD_RECORD_HPOST) return 1;
	phase = SV_HeldRecordGet(event,"phase",'U'); if (!phase) return 0;
	return phase->u != 2 || Held_BodySample(body,model,clock,pinned);
}

static unsigned int Held_BodyTest(const held_record_t *context, unsigned int *checks)
{
	static const char inputline[] = "in 7 100 0 0 0 0 0 0 0 0 0\n";
	static const char counts[] = "mc 7 0 0 0 0 0 1\n";
	static const char footer[] = "prof ok\nclk 5.000 0.020 0.050 0.000 0 0.000 0.000 0.000\ninend 100 0\nsplit 1 5\nclockend viewing 100 0 0 0\nend 5 1 0 0 1 1 0 0 1 0 0 1 0 0\n";
	held_body_t body = {0};
	held_clock_replay_t clock = {0};
	held_record_t event = {0}, rounded;
	held_input_t input = {0};
	float pins[SV_PMPIN_COUNT];
	char line[HELD_MAX_LINE], tokens[4096];
	unsigned int failures = 0;
#define HB_TEST(x) do {++*checks; if (!(x)) {++failures; Con_Printf("held body control failed at %d\n",__LINE__);} } while (0)
#define HB_REQUIRE(x) do {++*checks; if (!(x)) {++failures; Con_Printf("held body control failed at %d\n",__LINE__); goto cleanup;} } while (0)
	HB_TEST(Held_LegacyProfile("inprof ok 63 1 0 0 1\n",strlen("inprof ok 63 1 0 0 1\n")));
	HB_TEST(!Held_LegacyProfile("inprof mover 9\n",strlen("inprof mover 9\n")));
	HB_TEST(!Held_LegacyProfile("inprof ok 63 1 0 nan 1\n",strlen("inprof ok 63 1 0 nan 1\n")));
	rounded = *context; SV_HeldRecordF(&rounded,"ctx_ticrate",.0123456789f);
	Q_snprintfz(line,sizeof(line),"movetickrate %g\n",.0123456789f);
	HB_TEST(Held_LegacyRate(line,strlen(line),&rounded));
	HB_TEST(!Held_LegacyRate("movetickrate .02\n",strlen("movetickrate .02\n"),&rounded));
	SV_PMPinFill(pins); HB_REQUIRE(SV_PMPinText(pins,tokens,sizeof(tokens)) != 0);
	Q_snprintfz(line,sizeof(line),"pm 7 0 %s\npe 7 0 %u\n",tokens,SV_PhysentDigest());
	body.cursor = line; body.end = line+strlen(line); body.normal = 1;
	body.mode = PM_NONE; body.epoch = 1; body.physcrc = SV_PhysentDigest(); clock.clock.packet = 7;
	SV_HeldRecordBegin(&event,HELD_RECORD_HPRE); SV_HeldRecordU(&event,"phase",2);
	HB_TEST(Held_BodyBeforeEvent(&body,context,&clock,&event) && body.cursor == body.end && body.pcset && body.recorded_epoch == 1);
	body.cursor = line; body.mode = PM_NORMAL; body.recorded_epoch = 0; body.pcset = 0;
	HB_TEST(!Held_BodyState(&body,context,&clock) && body.cursor == line);
	body.mode = PM_NONE;
	SV_HeldRecordBegin(&event,HELD_RECORD_HSCOPE); SV_HeldRecordU(&event,"kind",1); SV_HeldRecordU(&event,"phase",2);
	HB_TEST(Held_BodyBeforeEvent(&body,context,&clock,&event) && body.cursor == line && !body.pcset);
	clock.clock.frzon = clock.clock.freeze = 1;
	HB_TEST(Held_BodyState(&body,context,&clock) && body.cursor == line && !body.pcset);
	HB_REQUIRE(SV_HeldInputParse(inputline,sizeof(inputline)-1,&input));
	memset(&body,0,sizeof(body)); body.cursor = inputline; body.end = inputline+sizeof(inputline)-1;
	HB_TEST(Held_BodyInput(&body,&input,0) && body.cursor == body.end && body.normal == 1);
	body.cursor = inputline; body.normal = 0;
	HB_TEST(!Held_BodyInput(&body,&input,1) && body.cursor == inputline);
	Q_snprintfz(line,sizeof(line),"ride 7 100 arm 0 0 -0\n%s",inputline);
	memset(&body,0,sizeof(body)); body.cursor = line; body.end = line+strlen(line);
	HB_TEST(Held_BodyInput(&body,&input,0) && body.cursor == body.end && body.neutral_floor == 1 && body.normal == 1);
	body.cursor = line;
	HB_TEST(!Held_BodyInput(&body,&input,1)); /* no late/duplicate floor */
	memset(&body,0,sizeof(body)); body.cursor = line; body.end = line+strlen(line);
	++input.movetick;
	HB_TEST(!Held_BodyInput(&body,&input,0)); /* stamps cannot borrow another initial state */
	--input.movetick;
	body.cursor = counts; body.end = counts+sizeof(counts)-1; body.normal = 1;
	HB_TEST(Held_BodyCounts(&body,7,0) && body.cursor == body.end);
	body.cursor = counts;
	HB_TEST(!Held_BodyCounts(&body,7,1) && body.cursor == counts);
	Q_snprintfz(line,sizeof(line),"seed prefix\n0.0000 %.2f %.2f %.2f %.2f %.2f %.2f 0.0 0.0 0 0 0 0 0 0.0000 0.0000 0.0000\nzseed evzone=-1 azone=-1 cpzone=-1 track=0 startseg=0 stagerun=0 twarp=0\n",
		pmove.origin[0],pmove.origin[1],pmove.origin[2],pmove.velocity[0],pmove.velocity[1],pmove.velocity[2]);
	body.cursor = line; body.end = line+strlen(line);
	HB_TEST(!Held_BodyBootstrap(&body,&pmove,0) && body.cursor == line);
	HB_TEST(Held_BodyConsume(&body,line,strlen("seed prefix\n")) && Held_BodyBootstrap(&body,&pmove,0) && body.cursor == body.end);
	body.cursor = footer; body.end = footer+sizeof(footer)-1;
	HB_TEST(Held_BodyFooter(&body,5) && body.cursor == body.end);
	Q_snprintfz(line,sizeof(line),"%sunknown\n",footer); body.cursor = line; body.end = line+strlen(line);
	HB_TEST(!Held_BodyFooter(&body,5));
cleanup:
	Held_PhysicalWipe(line,sizeof(line)); Held_PhysicalWipe(tokens,sizeof(tokens)); Held_PhysicalWipe(pins,sizeof(pins));
	Held_PhysicalWipe(&body,sizeof(body)); Held_PhysicalWipe(&clock,sizeof(clock)); Held_PhysicalWipe(&input,sizeof(input));
	Held_PhysicalWipe(&event,sizeof(event)); Held_PhysicalWipe(&rounded,sizeof(rounded));
	return failures;
#undef HB_TEST
#undef HB_REQUIRE
}

int SV_HeldTrajectoryRecording(const char *data, size_t size, const held_record_t *native_context,
	model_t *world, held_trajectory_t *trajectory)
{
	static const char *seednames[] = {"evzone=","azone=","cpzone=","track=","startseg=","stagerun=","twarp="};
	held_witness_t witness = {0};
	held_record_t record = {0}, context = {0};
	held_timeline_t timeline = {0};
	held_zone_t zone = {0};
	held_body_t body = {0};
	held_input_t *inputs = NULL, endpoint = {0};
	cvar_t *teleport_rule;
	const held_input_t *input, *successor;
	const held_value_t *value;
	const char *cursor, *end, *newline, *tag, *legacy_seed = NULL, *clockend = NULL, *monitor = NULL, *hold = NULL, *pinwarp = NULL;
	const char *endline = NULL, *flags = NULL, *body_start = NULL;
	const usercmd_t *command;
	char words[12][64];
	unsigned char digest[32], bsp[32], plugin[32];
	float seed[5] = {0}, msec, ticks;
	int64_t integer;
	uint64_t world_epoch = 0, progs_epoch = 0;
	uint32_t first_raw = 0, first_tick = 0;
	size_t ordinary_size = 0, length, legacy_seed_size = 0, clockend_size = 0, monitor_size = 0, hold_size = 0, pinwarp_size = 0;
	size_t endline_size = 0, flags_size = 0;
	uint32_t counts[13] = {0};
	unsigned int count = 0, seedcount = 0, startcount = 0, endcount = 0, fields, i, kind, index, moves;
	unsigned int pins = 0, counted = 0, rate = 0, profile = 0, holdclock = 0, viewcap = 0, last_input = HELD_MAX_PIECES;
	unsigned int native_mode = PM_NORMAL, diagnostic_stage = 1, failure = HELD_VERIFY_UNSUPPORTED;
	int have_context = 0, initialized = 0, preinitial_ack = 0, pin_checked = 0, hold_checked = 0, expect_pin = 0, result = 0;
	if (!trajectory) return 0;
	memset(trajectory,0,sizeof(*trajectory));
	if (!data || !native_context || world != sv.world.worldmodel || size > 2u*HELD_MAX_DETAIL) goto cleanup;
	if (!SV_HeldRecordingParse(data,size,&witness,&ordinary_size)) {failure = HELD_VERIFY_MALFORMED; goto cleanup;}
	if (witness.status != 3)
	{
		failure = witness.status == 2 ? HELD_VERIFY_INCOMPLETE : HELD_VERIFY_UNSUPPORTED;
		goto cleanup;
	}
	inputs = BZF_Malloc(HELD_MAX_PIECES*sizeof(*inputs));
	if (!inputs) goto cleanup;
	diagnostic_stage = 2;
	cursor = data; end = data+ordinary_size;
	while (cursor < end)
	{
		newline = memchr(cursor,'\n',end-cursor); if (!newline) goto cleanup;
		tag = cursor; while (tag < newline && Held_InputWhite((unsigned char)*tag)) ++tag;
		length = newline+1-tag;
		if (*tag == '-' || (*tag >= '0' && *tag <= '9'))
		{
			++counts[1]; if (*tag == '-') ++counts[2];
		}
		else if (Held_TrajectoryTag(tag,length,"pm")) ++counts[7];
		else if (Held_TrajectoryTag(tag,length,"pe")) ++counts[8];
		else if (Held_TrajectoryTag(tag,length,"ride"))
		{
			if (count || counts[6] || !SV_HeldNeutralRideParse(tag,length,NULL,NULL)) goto cleanup;
			++counts[6];
		}
		else if (Held_TrajectoryTag(tag,length,"cp") || Held_TrajectoryTag(tag,length,"portal") ||
			Held_TrajectoryTag(tag,length,"session")) goto cleanup;
		if (Held_TrajectoryTag(tag,length,"in") || Held_TrajectoryTag(tag,length,"fzin"))
		{
			if (count == HELD_MAX_PIECES || !SV_HeldInputParse(tag,length,&inputs[count])) goto cleanup;
			++counts[4]; /* SV_RecInput counts BOTH normal and released frozen aliases. */
			++count;
		}
		else if (Held_TrajectoryTag(tag,length,"pmpin"))
		{
			if (pins++ || !Held_LegacyPin(tag,length,native_context)) goto cleanup;
		}
		else if (Held_TrajectoryTag(tag,length,"clock"))
		{
			if (counted++ || Held_InputWords(tag,length,words) != 2 || strcmp(words[1],"counted")) goto cleanup;
		}
		else if (Held_TrajectoryTag(tag,length,"movetickrate"))
		{
			if (rate++ || !Held_LegacyRate(tag,length,native_context)) goto cleanup;
		}
		else if (Held_TrajectoryTag(tag,length,"inprof"))
		{
			if (profile++ || !Held_LegacyProfile(tag,length)) goto cleanup;
		}
		else if (Held_TrajectoryTag(tag,length,"holdclock"))
		{
			if (holdclock++ || Held_InputWords(tag,length,words) != 2 || strcmp(words[1],"1")) goto cleanup;
		}
		else if (Held_TrajectoryTag(tag,length,"viewclock"))
		{
			if (!body_start)
			{
				if (viewcap++ || Held_InputWords(tag,length,words) != 2 || strcmp(words[1],"1")) goto cleanup;
			}
			else
			{
				if (Held_LegacyView(tag,length,NULL,-1) != 1) goto cleanup;
				++counts[12];
			}
		}
		else if (Held_TrajectoryTag(tag,length,"seed"))
		{
			if (legacy_seed) goto cleanup;
			legacy_seed = tag; legacy_seed_size = length;
		}
		else if (Held_TrajectoryTag(tag,length,"clockend"))
		{
			if (clockend) goto cleanup;
			clockend = tag; clockend_size = length; ++counts[11];
		}
		else if (Held_TrajectoryTag(tag,length,"clk"))
		{
			if (monitor) goto cleanup;
			monitor = tag; monitor_size = length;
		}
		else if (Held_TrajectoryTag(tag,length,"hold"))
		{
			if (hold) goto cleanup;
			hold = tag; hold_size = length;
		}
		else if (Held_TrajectoryTag(tag,length,"warp"))
		{
			if (pinwarp) goto cleanup;
			pinwarp = tag; pinwarp_size = length; ++counts[5];
		}
		else if (Held_TrajectoryTag(tag,length,"end"))
		{
			if (endline) goto cleanup;
			endline = tag; endline_size = length;
		}
		else if (Held_TrajectoryTag(tag,length,"flags"))
		{
			if (flags) goto cleanup;
			flags = tag; flags_size = length;
		}
		else if (Held_TrajectoryTag(tag,length,"begin"))
		{
			if (body_start) goto cleanup;
			body_start = newline+1;
		}
		else if (Held_TrajectoryTag(tag,length,"zseed"))
		{
			if (seedcount++) goto cleanup;
			fields = Held_InputWords(tag,length,words); if (fields != 8) goto cleanup;
			for (i = 0; i < 7; ++i)
			{
				length = strlen(seednames[i]);
				if (strncmp(words[i+1],seednames[i],length) ||
					!Held_InputInt(words[i+1]+length,i < 3 ? -1 : 0,16777216,&integer)) goto cleanup;
				/* Actual SV_VerifyBegin: evzone, azone, track, stagerun, startseg.
				 * cpzone is a legacy observation, not its third argument. */
				if (i < 2) seed[i] = integer;
				else if (i == 3) seed[2] = integer;
				else if ((i == 4 || i == 5) && integer) goto cleanup; /* Fresh whole-run, not a stage. */
				else if (i == 6)
				{
					/* SV_Rec's twarp is the policy Boolean, NOT a past warp. */
					teleport_rule = Cvar_FindVar("run_teleport_warp");
					if (!teleport_rule || !isfinite(teleport_rule->value) || integer > 1 ||
						integer != !!teleport_rule->value) goto cleanup;
				}
			}
		}
		else if (Held_TrajectoryTag(tag,length,"instart"))
		{
			if (startcount++ || Held_InputWords(tag,length,words) != 3 ||
				!Held_InputInt(words[1],0,16777216,&integer)) goto cleanup;
			first_raw = integer;
			if (!Held_InputInt(words[2],0,16777216,&integer)) goto cleanup;
			first_tick = integer;
		}
		else if (Held_TrajectoryTag(tag,length,"inend"))
		{
			if (endcount++ || Held_InputWords(tag,length,words) != 3 ||
				!Held_InputInt(words[1],0,16777216,&integer)) goto cleanup;
			endpoint.movetick = integer;
			if (!Held_InputFloat(words[2],0,.05f,&endpoint.carry)) goto cleanup;
		}
		cursor = newline+1;
	}
	if (!count || count != witness.alias_count || seedcount != 1 || startcount != 1 || endcount != 1 ||
		pins != 1 || counted != 1 || rate != 1 || profile != 1 || holdclock != 1 || viewcap != 1 || counts[12] != 2 || !legacy_seed || !clockend || !monitor || !hold || !endline || !flags || !body_start) goto cleanup;
	if (!Held_LegacyFlags(flags,flags_size,NULL) ||
		!Held_BodySyntax(body_start,data+ordinary_size,native_context,
			!memcmp(data,HELD_NATIVE_REC_HEADER,sizeof(HELD_NATIVE_REC_HEADER)-1)))
	{
		if (!memcmp(data,HELD_NATIVE_REC_HEADER,sizeof(HELD_NATIVE_REC_HEADER)-1)) failure = HELD_VERIFY_MALFORMED;
		goto cleanup;
	}
	cursor = end; end = data+size;
	while (cursor < end)
	{
		newline = memchr(cursor,'\n',end-cursor);
		if (!newline || !SV_HeldRecordParse(cursor,newline+1-cursor,&record)) goto cleanup;
		cursor = newline+1;
		if (record.family == HELD_RECORD_HCTX)
		{
			diagnostic_stage = 3;
			if (have_context || !Held_TrajectoryContext(&record,native_context)) goto cleanup;
			world_epoch = Mod_HeldWorldArtifacts(world,bsp,plugin); progs_epoch = SV_HeldProgsEpoch();
			if (!world_epoch || !progs_epoch) goto cleanup;
			value = SV_HeldRecordGet(&record,"world",'D'); if (!value || memcmp(value->bytes,bsp,32)) goto cleanup;
			value = SV_HeldRecordGet(&record,"plugin",'D'); if (!value || memcmp(value->bytes,plugin,32)) goto cleanup;
#ifdef PLUGINS
			if (!Plug_ArtifactFingerprint(NULL,NULL,digest)) goto cleanup;
#else
			goto cleanup;
#endif
			value = SV_HeldRecordGet(&record,"engine",'D'); if (!value || memcmp(value->bytes,digest,32)) goto cleanup;
			value = SV_HeldRecordGet(&record,"progs",'D');
			if (!value || !SV_HeldProgsFingerprint(digest) || memcmp(value->bytes,digest,32)) goto cleanup;
			value = SV_HeldRecordGet(&record,"zone",'D');
			if (!value || !SV_HeldZoneFingerprint(digest) || memcmp(value->bytes,digest,32)) goto cleanup;
			context = record; have_context = 1; continue;
		}
		if (!initialized && record.family == HELD_RECORD_HSCOPE)
		{
			value = SV_HeldRecordGet(&record,"kind",'U');
			if (value && value->u == 4)
			{
				value = SV_HeldRecordGet(&record,"phase",'U');
				if (value && value->u == 2) preinitial_ack = 1;
			}
		}
		if (record.family == HELD_RECORD_HINIT)
		{
			diagnostic_stage = 4;
			value = SV_HeldRecordGet(&record,"clock_flags",'F');
			if (!value || value->f < 0 || value->f > 16777215 || value->f != floorf(value->f) ||
				!!((uint32_t)value->f & 131072u) != preinitial_ack) goto cleanup;
			if (!have_context || initialized || !Held_LegacySeed(legacy_seed,legacy_seed_size,&record) ||
				!SV_HeldZoneBegin(&zone,digest,seed) ||
				!SV_HeldTimelineBegin(&timeline,&context,&record,world,SV_HeldZoneStep,&zone)) goto cleanup;
			initialized = 1;
			if (timeline.physical.raw_moveticks != first_raw || timeline.clock.clock.starttick != (float)first_tick) goto cleanup;
			diagnostic_stage = 5;
			body.cursor = body_start; body.end = data+ordinary_size; body.mode = pmove.pm_type; body.physcrc = SV_PhysentDigest();
			body.relative_counts = !memcmp(data,HELD_NATIVE_REC_HEADER,sizeof(HELD_NATIVE_REC_HEADER)-1);
			if (!Held_BodyConsume(&body,legacy_seed,legacy_seed_size) ||
				!Held_BodyBootstrap(&body,&pmove,SV_HeldClockSeconds(&timeline.clock))) goto cleanup;
			continue;
		}
		if (record.family == HELD_RECORD_HSEAL)
		{
			diagnostic_stage = 13;
			/* Witness already independently checked the terminal census. It is
			 * not another physics/clock event, nor permission to publish. */
			if (!initialized || cursor != end || !timeline.terminal || timeline.post) goto cleanup;
			continue;
		}
		if (!initialized) continue; /* Earlier scopes are covered by the one initial seed. */
		command = NULL; msec = 0; diagnostic_stage = 6;
		if (!Held_BodyBeforeEvent(&body,&context,&timeline.clock,&record)) goto cleanup;
		if (record.family == HELD_RECORD_HMOVE || record.family == HELD_RECORD_HORDINARY)
		{
			diagnostic_stage = 7;
			value = SV_HeldRecordGet(&record,"ownerkind",'U'); if (!value) goto cleanup; kind = value->u;
			if (kind != 3)
			{
				value = SV_HeldRecordGet(&record,"ownerindex",'U'); if (!value) goto cleanup; index = value->u;
				if (index >= count) goto cleanup;
				input = &inputs[index]; last_input = index;
				if (kind != input->kind || kind != (timeline.clock.clock.frzon ? 2u : 1u) ||
					(input->flags & ~8u) || /* Existing split-part marker is not physical state. */
					input->movetick != timeline.physical.raw_moveticks || (float)input->packet != timeline.clock.clock.packet ||
					memcmp(&input->carry,&pmove.msec_carry,sizeof(float))) goto cleanup;
				if (!Held_BodyInput(&body,input,index) || !Held_BodyCounts(&body,timeline.clock.clock.packet,timeline.physical.pinned)) goto cleanup;
				command = &input->command;
				if (record.family == HELD_RECORD_HORDINARY)
				{
					successor = index+1 < count ? &inputs[index+1] : &endpoint;
					if (!SV_HeldInputMsec(input,successor,movevars.ticrate,&msec)) goto cleanup;
				}
			}
		}
		if (record.family == HELD_RECORD_HENTRY)
		{
			if (pin_checked || last_input >= count) goto cleanup;
			expect_pin = pmove.velocity[0] != 0 || pmove.velocity[1] != 0 || pmove.velocity[2] != 0;
		}
		if (record.family == HELD_RECORD_HMOVE || record.family == HELD_RECORD_HORDINARY) native_mode = pmove.pm_type;
		diagnostic_stage = 8;
		if (!SV_HeldTimelineEvent(&timeline,&record,command,msec)) goto cleanup;
		if (record.family == HELD_RECORD_HMOVE || record.family == HELD_RECORD_HORDINARY)
		{
			if (body.mode != native_mode) {body.mode = native_mode; ++body.epoch;}
		}
		diagnostic_stage = 9;
		if (record.family == HELD_RECORD_HENTRY || record.family == HELD_RECORD_HRELEASE)
		{
			const char *viewline;
			size_t viewsize;
			if (!Held_BodyPeek(&body,&viewline,&viewsize)) goto cleanup;
			i = Held_LegacyView(viewline,viewsize,&timeline.clock,record.family == HELD_RECORD_HENTRY);
			if (i != 1) {if (i == 2) body.failure = 2; goto cleanup;}
			body.cursor = viewline+viewsize;
		}
		if (!Held_BodyAfterEvent(&body,&pmove,&timeline.clock,timeline.physical.pinned,&record)) goto cleanup;
		if (record.family == HELD_RECORD_HENTRY)
		{
			diagnostic_stage = 10;
			if (expect_pin)
			{
				if (!pinwarp || !Held_LegacyPinWarp(pinwarp,pinwarp_size,&pmove,timeline.physical.raw_moveticks,timeline.clock.clock.packet,last_input) ||
					!Held_BodyConsume(&body,pinwarp,pinwarp_size)) goto cleanup;
			}
			else if (pinwarp) goto cleanup;
			pin_checked = 1;
		}
		if (record.family == HELD_RECORD_HTHAW)
		{
			diagnostic_stage = 11;
			if (hold_checked || !Held_LegacyHold(hold,hold_size,timeline.physical.raw_moveticks,timeline.clock.clock.frzticks) ||
				!Held_BodyConsume(&body,hold,hold_size)) goto cleanup;
			hold_checked = 1;
		}
	}
	diagnostic_stage = 12;
	if (!pin_checked || !hold_checked) goto cleanup;
	if (!initialized || timeline.physical.raw_moveticks != endpoint.movetick ||
		memcmp(&pmove.msec_carry,&endpoint.carry,sizeof(float)) || progs_epoch != SV_HeldProgsEpoch() ||
		world_epoch != Mod_HeldWorldArtifacts(world,bsp,plugin)) goto cleanup;
	i = Held_LegacyClockEnd(clockend,clockend_size,&timeline.clock);
	if (i != 1) {if (i == 2) body.failure = 2; goto cleanup;}
	i = Held_LegacyClockMonitor(monitor,monitor_size,&timeline.clock);
	if (i != 1) {if (i == 2) body.failure = 2; goto cleanup;}
	ticks = timeline.clock.clock.ticks; moves = timeline.physical.calls;
	if (!Held_PhysicalFinite(ticks) || ticks < 0 || ticks > 16777216 || ticks != floorf(ticks)) goto cleanup;
	counts[0] = (uint32_t)ticks;
	if (!Held_LegacyEnd(endline,endline_size,counts,
		!memcmp(data,HELD_NATIVE_REC_HEADER,sizeof(HELD_NATIVE_REC_HEADER)-1)) ||
		!Held_LegacyFlags(flags,flags_size,&timeline.clock) || !Held_BodyFooter(&body,ticks) ||
		body.normal+body.frozen != counts[4] || body.normal+body.frozen != count) goto cleanup;
	if (!SV_HeldTimelineFinish(&timeline)) goto cleanup;
	trajectory->ticks = ticks; trajectory->native_moves = moves; result = 1;
cleanup:
	if (!result && (timeline.failure == 2 || timeline.physical.failure == 2 || body.failure == 2)) failure = HELD_VERIFY_MISMATCH;
	if (held_closed_test_diagnostic && !result)
		Con_Printf("held synthetic byte walk stage %u family %u timeline %u physical %u\n",
			diagnostic_stage,(unsigned int)record.family,timeline.failure,timeline.physical.failure);
	SV_HeldTimelineEnd(&timeline); SV_HeldZoneEnd(&zone);
	if (inputs) {Held_PhysicalWipe(inputs,HELD_MAX_PIECES*sizeof(*inputs)); BZ_Free(inputs);}
	Held_PhysicalWipe(&witness,sizeof(witness)); Held_PhysicalWipe(&record,sizeof(record)); Held_PhysicalWipe(&context,sizeof(context));
	Held_PhysicalWipe(&endpoint,sizeof(endpoint)); Held_PhysicalWipe(words,sizeof(words)); Held_PhysicalWipe(digest,sizeof(digest)); Held_PhysicalWipe(seed,sizeof(seed));
	Held_PhysicalWipe(bsp,sizeof(bsp)); Held_PhysicalWipe(plugin,sizeof(plugin)); Held_PhysicalWipe(counts,sizeof(counts)); Held_PhysicalWipe(&body,sizeof(body));
	if (!result) {Held_PhysicalWipe(trajectory,sizeof(*trajectory)); trajectory->failure = failure;}
	return result;
}

/* Decode declared configuration through the existing native importer; this does
 * not manufacture an initial state. Only the one recorded native hinit is used.
 * Parameters are inputs, NOT artifact/scene authority. Rebuild the expected
 * context with the loaded native artifacts, never pass the file's HCTX through. */
static int Held_ClosedNativeContext(const held_record_t *declaration, const held_record_t *initial,
	model_t *world, held_record_t *native)
{
	static const char *source_float[] = {"ctx_svmaxvel","ctx_trisoup","ctx_rotboxes","ctx_portalcsg","ctx_excuse","ctx_slow"};
	static const char *source_integer[] = {"ctx_geomtype","ctx_trisoup_i","ctx_rotboxes_i","ctx_portalcsg_i"};
	held_physical_t imported = {0};
	movevars_t source;
	unsigned int i;
	int result = 0;
	if (!SV_HeldPhysicalBegin(&imported,declaration,initial,world)) goto cleanup;
	source = movevars;
	if (!SV_HeldContextBuildNative(&source,native)) goto cleanup;
	/* These historical source parameters are explicitly represented, typed and
	 * validated by the importer/clock model. Do not change live cvars/globals to
	 * impersonate them; native rule constants and all five artifact IDs remain
	 * independently reconstructed. No state/observation/flag is copied as a seed. */
	for (i = 0; i < countof(source_float); ++i)
		if (!SV_HeldRecordF(native,source_float[i],Held_PhysicalValue(&imported,declaration,source_float[i],'F')->f)) goto cleanup;
	for (i = 0; i < countof(source_integer); ++i)
		if (!SV_HeldRecordI(native,source_integer[i],Held_PhysicalValue(&imported,declaration,source_integer[i],'I')->i)) goto cleanup;
	if (imported.failure || !Held_TrajectoryContext(declaration,native)) goto cleanup;
	result = 1;
cleanup:
	SV_HeldPhysicalEnd(&imported);
	Held_PhysicalWipe(&source,sizeof(source));
	if (!result) Held_PhysicalWipe(native,sizeof(*native));
	return result;
}

held_verify_result_t SV_HeldVerifyRecording(const char *data, size_t size,
	model_t *world, held_trajectory_t *trajectory)
{
	held_witness_t witness = {0};
	held_record_t record = {0}, declaration = {0}, initial = {0}, native = {0};
	const char *cursor, *end, *newline;
	size_t ordinary = 0;
	unsigned int contexts = 0, initials = 0;
	held_verify_result_t result = HELD_VERIFY_MALFORMED;
	if (!trajectory) return result;
	memset(trajectory,0,sizeof(*trajectory));
	if (!data || !SV_HeldRecordingParse(data,size,&witness,&ordinary)) goto cleanup;
	if (witness.status != 3)
	{
		result = witness.status == 2 ? HELD_VERIFY_INCOMPLETE : HELD_VERIFY_UNSUPPORTED;
		goto cleanup;
	}
	cursor = data+ordinary; end = data+size;
	while (cursor < end)
	{
		newline = memchr(cursor,'\n',end-cursor);
		if (!newline || !SV_HeldRecordParse(cursor,newline+1-cursor,&record)) goto cleanup;
		if (record.family == HELD_RECORD_HCTX) {declaration = record; ++contexts;}
		if (record.family == HELD_RECORD_HINIT) {initial = record; ++initials;}
		cursor = newline+1;
	}
	if (contexts != 1 || initials != 1) goto cleanup;
	result = HELD_VERIFY_UNSUPPORTED;
	if (!Held_ClosedNativeContext(&declaration,&initial,world,&native)) goto cleanup;
	if (SV_HeldTrajectoryRecording(data,size,&native,world,trajectory)) result = HELD_VERIFY_MATCH;
	else if (trajectory->failure) result = (held_verify_result_t)trajectory->failure;
cleanup:
	Held_PhysicalWipe(&witness,sizeof(witness)); Held_PhysicalWipe(&record,sizeof(record));
	Held_PhysicalWipe(&declaration,sizeof(declaration)); Held_PhysicalWipe(&initial,sizeof(initial)); Held_PhysicalWipe(&native,sizeof(native));
	if (result != HELD_VERIFY_MATCH) {trajectory->ticks = 0; trajectory->native_moves = 0; trajectory->failure = result;}
	return result;
}

/* Same-TU control reuses the private walker without exporting test authority. */
#include "sv_held_closed_test.inc"
