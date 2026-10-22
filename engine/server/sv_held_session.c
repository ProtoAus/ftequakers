#if defined(__GNUC__)
#pragma GCC optimize ("no-fast-math", "fp-contract=off")
#endif
#include "quakedef.h"
#include "pr_common.h"
#include "sv_held_file.h"
#include "sv_held_ledger.h"
#include "sv_held_session.h"
#include "sv_held_initial.h" /* IWYU pragma: keep - collector implementation below. */
#include "sv_held_replay.h" /* IWYU pragma: keep - original-byte end validator below. */

extern int fs_readonly;
#define HELD_MAX_SESSIONS 128

typedef struct
{
	held_record_t context;
	held_coverage_t census;
	movevars_t source_vars;
	uint64_t progs_epoch, command_controls;
	float command_time, command_frametime;
	double started_time, protected_time;
	unsigned char protected_piece[HELD_MAX_PIECES];
	unsigned int guard, guard_close, thawed, projected;
	unsigned int started, initial_written, pending_move, aliases;
	unsigned int command_open, command_written, command_kind;
	unsigned int terminal, close_requested, sealed;
} held_capture_t;

typedef struct
{
	char *data;
	size_t size, source_size, capacity;
	unsigned int ledger_owned, streamed;
	char generic[64];
	unsigned char source_digest[32], digest[32];
	uint64_t progs_epoch;
	unsigned int ready, classification;
} held_prepared_t;

typedef struct
{
	client_t *client;
	edict_t *entity;
	pubprogfuncs_t *vm;
	model_t *world;
	held_source_owner_t owner;
	held_file_ticket_t *source;
	held_ledger_t ledger;
	held_capture_t capture;
	held_prepared_t prepared;
	char generation[33], nonce[33], path[MAX_QPATH], destination[MAX_OSPATH], map[MAX_QPATH];
	char recovery_root[MAX_OSPATH];
	float claimed_ticks;
	unsigned int track, leg, lobby;
	uint64_t progs_epoch;
	unsigned int failed, selected, initialized, revoked, close_requested, published;
} held_session_t;

static held_session_t *held_sessions[HELD_MAX_SESSIONS];
static unsigned int held_session_count;
static cvar_t held_staging = CVARD("sv_held_staging","","OS-private held transaction directory, outside every game/download/web root. Empty disables the producer.");
void SV_HeldSessionInit(void)
{
	Cvar_Register(&held_staging,"Server control");
}
static int Held_SessionRecording(void)
{
	float value = Cvar_VariableValue("rec_enable");
	uint32_t bits;
	memcpy(&bits,&value,sizeof(bits));
	return (bits & 0x7f800000u) != 0x7f800000u && value != 0;
}
int SV_HeldSessionAvailable(void)
{
	/* PURE feature query: no files/actors/epochs are created or mutated. A
	 * configured path is NOT a sink capability; native construction rechecks it. */
	return sizeof(void*) == 8 && !fs_readonly && svs.gametype == GT_PROGS && progstype == PROG_QW &&
		sv.world.worldmodel && *held_staging.string && SV_HeldStoreHealthy() && Held_SessionRecording() &&
		Cvar_VariableValue("rec_held_metadata") == 1;
}

static void Held_SessionWipe(void *ptr, size_t length)
{
	volatile unsigned char *p = ptr;
	while (length--) *p++ = 0;
}
static void Held_PreparedDiscard(held_session_t *session)
{
	if (session->prepared.data && !session->prepared.ledger_owned)
	{
		SV_HeldBufferFree(session->prepared.data,session->prepared.capacity);
	}
	Held_SessionWipe(&session->prepared,sizeof(session->prepared));
}
static int Held_SessionScalar(edict_t *entity, const char *name, float *out)
{
	eval_t *value;
	uint32_t bits;
	if (!svprogfuncs || !entity) return 0;
	value = svprogfuncs->GetEdictFieldValue(svprogfuncs, entity, name, ev_float, NULL);
	if (!value) return 0;
	memcpy(&bits, &value->_float, sizeof(bits));
	if ((bits & 0x7f800000u) == 0x7f800000u) return 0;
	*out = value->_float;
	return 1;
}
static const char *Held_SessionString(edict_t *entity, const char *name)
{
	eval_t *value;
	if (!svprogfuncs || !entity) return NULL;
	value = svprogfuncs->GetEdictFieldValue(svprogfuncs, entity, name, ev_string, NULL);
	return value ? PR_GetString(svprogfuncs, value->string) : NULL;
}
static int Held_SessionHex32(const char *value)
{
	unsigned int i;
	if (!value || strlen(value) != 32) return 0;
	for (i = 0; i != 32; ++i)
		if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) return 0;
	return 1;
}
static int Held_SessionActor(client_t *client)
{
	int index;
	if (!client || !client->edict || !svprogfuncs || svs.gametype != GT_PROGS || progstype != PROG_QW ||
	    client->state != cs_spawned || client->protocol != SCP_QUAKEWORLD || client->spectator ||
	    client->controller || client->controlled || client->userid <= 0 || client->edict->ereftype != ER_ENTITY)
		return 0;
	index = client->edict->entnum - 1;
	return index >= 0 && index < sv.allocated_client_slots && &svs.clients[index] == client;
}
static int Held_SessionPermission(edict_t *entity)
{
	float value;
	return Held_SessionRecording() && *held_staging.string && SV_HeldStoreHealthy() &&
		Cvar_VariableValue("rec_held_metadata") == 1 &&
		Held_SessionScalar(entity, "rec_hm_cap", &value) && value == 1 &&
		Held_SessionScalar(entity, "rec_hm_allow", &value) && value == 1 &&
		Held_SessionScalar(entity, "run_t_terms", &value) && value == 8;
}
static held_session_t *Held_SessionFind(client_t *client)
{
	unsigned int i;
	if (!held_session_count) return NULL;
	for (i = 0; i != HELD_MAX_SESSIONS; ++i)
		if (held_sessions[i] && held_sessions[i]->client == client) return held_sessions[i];
	return NULL;
}
static void Held_SessionFail(held_session_t *session, unsigned int status)
{
	if (!session->failed) session->failed = status == 1 ? 1 : 2;
	Held_PreparedDiscard(session);
	SV_HeldLedgerFail(&session->ledger, session->failed);
	Held_SessionWipe(&session->capture,sizeof(session->capture)); session->initialized = 0;
	if (session->client && session->client->userid == (int)session->owner.client_uid)
		memset(&session->client->held_coverage,0,sizeof(session->client->held_coverage));
	/* Failure is irreversible: arm before any later QC close/remove path. */
	if (session->selected && session->source)
		SV_HeldFileRecover(session->source,session->recovery_root,session->generation);
	/* No-held failure must not hold DELETE access across an ordinary close. */
	if (!session->selected && session->source)
	{
		SV_HeldFileRetire(session->source, false);
		session->source = NULL;
	}
}
static void Held_SessionDrop(unsigned int index)
{
	held_session_t *session = held_sessions[index];
	if (!session) return;
	Held_PreparedDiscard(session);
	SV_HeldLedgerDiscard(&session->ledger);
	if (session->source)
	{
		if (session->selected)
			SV_HeldFileRecover(session->source,session->recovery_root,session->generation);
		SV_HeldFileRetire(session->source, false);
	}
	/* A recycled client address does not own a newer connection's counters. */
	if (session->client && session->client->userid == (int)session->owner.client_uid)
		memset(&session->client->held_coverage, 0, sizeof(session->client->held_coverage));
	held_sessions[index] = NULL;
	--held_session_count;
	Held_SessionWipe(session, sizeof(*session));
	BZ_Free(session);
}
static int Held_SessionOwnerIdentity(held_session_t *session)
{
	edict_t *entity;
	const char *value;
	float scalar;
	if (!Held_SessionActor(session->client) || svprogfuncs != session->vm ||
	    session->client->edict != session->entity || sv.world.worldmodel != session->world ||
	    session->client->userid != (int)session->owner.client_uid || strcmp(session->map,svs.name) ||
	    session->progs_epoch != SV_HeldProgsEpoch()) return 0;
	entity = session->entity;
	if (!Held_SessionScalar(entity, "run_t_slot", &scalar) || scalar != session->owner.record_slot ||
	    !Held_SessionScalar(entity, "rec_rec_serial", &scalar) || scalar != session->owner.serial ||
	    !Held_SessionScalar(entity, "rec_hm_mode", &scalar) || scalar != (session->selected ? 12 : 0)) return 0;
	value = Held_SessionString(entity, "rec_rec_rid");
	if (!value || strcmp(value, session->owner.record)) return 0;
	value = Held_SessionString(entity, "rec_rec_nonce");
	if (!value || strcmp(value, session->nonce)) return 0;
	value = Held_SessionString(entity, "rec_hm_generation");
	if (!value || strcmp(value, session->generation)) return 0;
	value = Held_SessionString(entity, "rec_hm_source");
	return value && !strcmp(value, session->path);
}
static vfsfile_t *Held_SessionStream(held_session_t *session)
{
	float scalar;
	if (!Held_SessionScalar(session->entity, "rec_rec_stream", &scalar) || scalar != 1 ||
	    !Held_SessionScalar(session->entity, "rec_rec_fh", &scalar) || scalar < 0 || scalar > 16777215 ||
	    (unsigned int)scalar != scalar) return NULL;
	return PR_QCFile_StreamVFS(session->vm, (int)scalar, session->path);
}
static int Held_SessionOwnerLive(held_session_t *session)
{
	return Held_SessionOwnerIdentity(session) && session->source && SV_HeldFilePristine(session->source) &&
		(SV_HeldFileClosed(session->source) || SV_HeldFileOwnsStream(Held_SessionStream(session),session->source));
}

int SV_HeldSessionBegin(client_t *client, const char *generation, const char *identity)
{
	held_session_t *session, *previous;
	held_source_owner_t owner;
	held_file_ticket_t *source;
	edict_t *entity;
	vfsfile_t *stream;
	const char *rid, *nonce, *path, *value;
	char expected[160];
	float slot, serial, handle, scalar, track, startseg, stagerun, lobby;
	unsigned int i, free_index = HELD_MAX_SESSIONS, previous_index = HELD_MAX_SESSIONS;
	if (!SV_HeldSessionAvailable() || !Held_SessionActor(client) || !Held_SessionHex32(generation) || !identity ||
		!*svs.name || strlen(svs.name) >= MAX_QPATH ||
		strlen(held_staging.string) >= sizeof(session->recovery_root) || !SV_HeldProgsEpoch()) return 0;
	previous = Held_SessionFind(client);
	entity = client->edict;
	if (!Held_SessionPermission(entity) ||
	    !Held_SessionScalar(entity, "run_t_state", &scalar) || scalar != 2 ||
	    !Held_SessionScalar(entity, "rec_hm_mode", &scalar) || scalar != 0 ||
	    !Held_SessionScalar(entity, "rec_rec_ver", &scalar) || scalar != HELD_BASE_REC_VERSION ||
	    !Held_SessionScalar(entity, "rec_rec_stream", &scalar) || scalar != 1 ||
	    !Held_SessionScalar(entity, "rec_rec_buf", &scalar) || scalar != -1 ||
	    !Held_SessionScalar(entity, "run_t_slot", &slot) || slot < 0 || slot >= 2048 || (unsigned int)slot != slot ||
	    !Held_SessionScalar(entity, "rec_rec_serial", &serial) || serial < 1 || serial > 16777215 || (unsigned int)serial != serial ||
	    !Held_SessionScalar(entity, "rec_rec_fh", &handle) || handle < 0 || handle > 16777215 || (unsigned int)handle != handle ||
	    !Held_SessionScalar(entity, "run_t_track", &track) || track < 0 || track >= 2048 || (unsigned int)track != track ||
	    !Held_SessionScalar(entity, "run_t_startseg", &startseg) || startseg < 0 || startseg >= 24 || (unsigned int)startseg != startseg ||
	    !Held_SessionScalar(entity, "run_t_stagerun", &stagerun) || (stagerun != 0 && stagerun != 1) ||
	    !Held_SessionScalar(entity, "rec_rec_partlob", &lobby) || (lobby != 1 && lobby != 2))
		return 0;
	rid = Held_SessionString(entity, "rec_rec_rid");
	nonce = Held_SessionString(entity, "rec_rec_nonce");
	path = Held_SessionString(entity, "rec_hm_source");
	value = Held_SessionString(entity, "rec_hm_generation");
	if (!rid || !*rid || strlen(rid) >= sizeof(owner.record) || !Held_SessionHex32(nonce) ||
	    !path || !*path || strlen(path) >= MAX_QPATH || !value || strcmp(value, generation)) return 0;
	Q_snprintfz(expected, sizeof(expected), "%s %u", rid, (unsigned int)serial);
	if (strcmp(identity, expected)) return 0;
	/* Failed candidacy may be retired for a different creator-owned recording,
	 * never for a new seed/generation on the same RID and serial. */
	if (previous && (!previous->failed || previous->selected || previous->source ||
		!strcmp(previous->generation,generation) ||
		(previous->owner.serial == (unsigned int)serial && !strcmp(previous->owner.record,rid)))) return 0;
	for (i = 0; i != HELD_MAX_SESSIONS; ++i)
	{
		if (!held_sessions[i])
		{
			if (free_index == HELD_MAX_SESSIONS) free_index = i;
		}
		else
		{
			if (!strcmp(held_sessions[i]->generation, generation)) return 0;
			if (held_sessions[i] == previous) previous_index = i;
		}
	}
	if (previous_index != HELD_MAX_SESSIONS) free_index = previous_index;
	if (free_index == HELD_MAX_SESSIONS) return 0;
	memset(&owner, 0, sizeof(owner));
	owner.vm = svprogfuncs; owner.client_uid = client->userid; owner.entity_index = entity->entnum;
	owner.record_slot = (unsigned int)slot; owner.serial = (unsigned int)serial;
	Q_strncpyz(owner.record, rid, sizeof(owner.record));
	stream = PR_QCFile_StreamVFS(svprogfuncs, (int)handle, path);
	source = SV_HeldFileAcquire(stream, &owner);
	if (!source) return 0;
	session = BZF_Malloc(sizeof(*session));
	if (!session)
	{
		SV_HeldFileRetire(source, false);
		return 0;
	}
	memset(session, 0, sizeof(*session));
	session->client = client; session->entity = entity; session->vm = svprogfuncs;
	session->world = sv.world.worldmodel; session->owner = owner; session->source = source;
	session->track = (unsigned int)track; session->leg = stagerun ? (unsigned int)startseg+1:0;
	session->lobby = lobby == 1; session->progs_epoch = SV_HeldProgsEpoch();
	Q_strncpyz(session->map,svs.name,sizeof(session->map));
	Q_strncpyz(session->generation, generation, sizeof(session->generation));
	Q_strncpyz(session->nonce, nonce, sizeof(session->nonce));
	Q_strncpyz(session->path, path, sizeof(session->path));
	Q_strncpyz(session->recovery_root,held_staging.string,sizeof(session->recovery_root));
	if (previous_index != HELD_MAX_SESSIONS) Held_SessionDrop(previous_index);
	held_sessions[free_index] = session;
	++held_session_count;
	Con_DPrintf("held native source: bound\n"); /* No RID/GUID/nonce or witness dump. */
	return 1; /* Ownership bound only. No ledger, coverage, seed or qualification. */
}
int SV_HeldSessionCheck(client_t *client, const char *generation)
{
	held_session_t *session = Held_SessionFind(client);
	if (!session || !generation || strcmp(session->generation, generation)) return 0;
	if (!Held_SessionOwnerIdentity(session)) {Held_SessionFail(session,2); return 0;}
	if (!Held_SessionPermission(session->entity)) session->revoked = 1;
	if (session->revoked) {Held_SessionFail(session,1); return 0;}
	if (session->failed) return 0;
	if (!Held_SessionOwnerLive(session)) {Held_SessionFail(session,2); return 0;}
	return 1;
}
int SV_HeldSessionSelect(client_t *client, const char *generation)
{
	held_session_t *session = Held_SessionFind(client);
	eval_t *mode;
	if (!session || !generation || strcmp(session->generation,generation) || !Held_SessionOwnerIdentity(session)) return 0;
	if (!Held_SessionPermission(session->entity)) session->revoked = 1;
	if (session->revoked) Held_SessionFail(session,1);
	if (session->selected) return session->failed ? 2:1;
	mode = session->vm->GetEdictFieldValue(session->vm,session->entity,"rec_hm_mode",ev_float,NULL);
	if (!mode || mode->_float != 0) return 0;
	/* Selection reacquires the exact-owner preservation lease, not permission
	 * to capture or publish after revocation. Nonselected close stays free. */
	if (!session->source)
		session->source = SV_HeldFileAcquire(Held_SessionStream(session),&session->owner);
	session->selected = 1; mode->_float = 12;
	/* A constructor or namespace alone is not a preceding actual native step. */
	if (session->revoked || !session->source || !session->initialized || !session->capture.started ||
		!session->capture.aliases || session->capture.pending_move || !client->held_coverage.command_open)
		Held_SessionFail(session,1);
	return session->failed ? 2:1;
}
int SV_HeldSessionAbort(client_t *client, const char *generation)
{
	held_session_t *session = Held_SessionFind(client);
	float closing = 1;
	if (!session || !generation || strcmp(session->generation, generation)) return 0;
	if (session->published) return SV_HeldSessionRetire(client,generation);
	if (!Held_SessionOwnerIdentity(session) || !Held_SessionPermission(session->entity)) session->revoked = 1;
	Held_SessionFail(session, 2);
	/* Timeout while closing may retry GENERIC, never a new seed. Teardown after
	 * close has no live creator stream and can retire only its own namespace. */
	if (Held_SessionScalar(session->entity,"rec_hm_closing",&closing) && closing == 0 &&
		(!session->source || SV_HeldFileClosed(session->source)) && !Held_SessionStream(session))
		return SV_HeldSessionRetire(client,generation);
	return 1;
}
int SV_HeldSessionRetire(client_t *client, const char *generation)
{
	unsigned int i;
	if (!generation) return 0;
	for (i = 0; i != HELD_MAX_SESSIONS; ++i)
		if (held_sessions[i] && held_sessions[i]->client == client && !strcmp(held_sessions[i]->generation, generation))
		{
			Held_SessionDrop(i);
			return 1;
		}
	return 0;
}
void SV_HeldSessionDropClient(client_t *client)
{
	unsigned int i;
	if (!held_session_count) return;
	for (i = 0; i != HELD_MAX_SESSIONS; ++i)
		if (held_sessions[i] && held_sessions[i]->client == client) Held_SessionDrop(i);
}
void SV_HeldSessionDropAll(void)
{
	unsigned int i;
	if (!held_session_count) return;
	for (i = 0; i != HELD_MAX_SESSIONS; ++i) Held_SessionDrop(i);
}

#include "sv_held_capture.inc"
#include "sv_held_prepare.inc"
