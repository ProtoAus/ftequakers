#ifndef SV_HELD_LEDGER_H
#define SV_HELD_LEDGER_H
#include <stdint.h>
#include <stddef.h>
#include "sv_held_schema.h"

/* Old12/13 retain absolute MC semantics; new14/15 are recording-relative. */
#define HELD_BASE_REC_VERSION 14
#define HELD_NATIVE_REC_VERSION 15
#define HELD_PREVIOUS_NATIVE_REC_VERSION 13
#define HELD_REC_STRING_(v) #v
#define HELD_REC_STRING(v) HELD_REC_STRING_(v)
#define HELD_BASE_REC_HEADER "FTESURF-REC " HELD_REC_STRING(HELD_BASE_REC_VERSION) "\n"
#define HELD_NATIVE_REC_HEADER "FTESURF-REC " HELD_REC_STRING(HELD_NATIVE_REC_VERSION) "\n"
#define HELD_PREVIOUS_NATIVE_REC_HEADER "FTESURF-REC " HELD_REC_STRING(HELD_PREVIOUS_NATIVE_REC_VERSION) "\n"

#define HELD_MAX_FIELDS 100
#define HELD_MAX_DETAIL (2u * 1024u * 1024u)
#define HELD_IO_BUFFER 65536u
#define HELD_MAX_RECORDS 16384u
#define HELD_MAX_LINE 8192u
#define HELD_MAX_PIECES 4096u

typedef union
{
	float f;
	int32_t i;
	uint32_t u;
	uint64_t l;
	unsigned char bytes[32];
} held_value_t;

typedef struct
{
	held_record_family_t family;
	held_value_t values[HELD_MAX_FIELDS];
	unsigned char assigned[HELD_MAX_FIELDS];
} held_record_t;

typedef struct
{
	unsigned char generation[16], ledger[32], execution_ledger[32];
	unsigned char pending_move[1024];
	size_t pending_move_size, size, capacity;
	char *detail;
	uint64_t ordinal;
	unsigned int failed, initialized, have_initial, sealed;
} held_ledger_t;

/* Bounded structural parser state. status3 means sealed grammar, NOT admission. */
typedef struct
{
	unsigned char generation[16], ledger[32], execution_ledger[32], pending_move[1024];
	unsigned char alias_kind[HELD_MAX_PIECES];
	uint32_t alias_index[HELD_MAX_PIECES];
	unsigned int alias_count;
	size_t size, pending_move_size;
	uint64_t records, message, execution, scope_message[2];
	uint64_t pre_enters, pre_exits, movers, completions, post_enters, post_exits;
	uint64_t commands, scene_fences, control_events;
	unsigned int scope_kind[2], depth, callback, maintenance, stage, pending, nonce_acked;
	unsigned int initialized, entered, released, frozen, thawed, observations, terminal, post_thaw_moves;
	unsigned int active, failed, status, previous_boundary, alias_route_bad;
	held_record_family_t previous_family;
} held_witness_t;

void SV_HeldWitnessBegin(held_witness_t *witness);
int SV_HeldWitnessLine(held_witness_t *witness, const char *line, size_t length);
int SV_HeldWitnessFinish(held_witness_t *witness);
int SV_HeldWitnessParse(const char *data, size_t size, held_witness_t *witness);
/* Exact-size offset reader; short reads fail, never imply EOF or missing data. */
typedef int (*held_reader_t)(void *context, size_t offset, void *buffer, size_t size);
typedef int (*held_line_check_t)(void *context, const char *line, size_t length);
int SV_HeldReadHash(held_reader_t read, void *context, size_t size, unsigned char digest[32]);
/* REC12 framing/ordinary alias census only; ordinary scalar/model validation is separate.
 * check observes ordinary lines only and may reject, never supply/repair bytes. */
int SV_HeldRecordingRead(held_reader_t read, void *context, size_t size, held_witness_t *witness,
	size_t *ordinary_size, held_line_check_t check, void *check_context);
int SV_HeldRecordingParse(const char *data, size_t size, held_witness_t *witness, size_t *ordinary_size);
const held_value_t *SV_HeldRecordGet(const held_record_t *record, const char *name, char type);
int SV_HeldRecordBegin(held_record_t *record, held_record_family_t family);
/* One exact schema line only; not a lifecycle or qualification validator. */
int SV_HeldRecordParse(const char *line, size_t length, held_record_t *record);
int SV_HeldRecordU(held_record_t *record, const char *name, uint32_t value);
int SV_HeldRecordI(held_record_t *record, const char *name, int32_t value);
int SV_HeldRecordL(held_record_t *record, const char *name, uint64_t value);
int SV_HeldRecordF(held_record_t *record, const char *name, float value);
int SV_HeldRecordBytes(held_record_t *record, const char *name, const unsigned char *value, size_t size);
int SV_HeldLedgerBegin(held_ledger_t *ledger, const char *generation);
int SV_HeldLedgerAppend(held_ledger_t *ledger, held_record_t *record, uint64_t message, uint64_t execution);
/* Sealed native transient tape, in-place shortening only. Never public input. */
int SV_HeldLedgerRecode(held_ledger_t *ledger, int (*rewrite)(held_record_t *, void *), void *context);
/* Closed tape relocated within its SAME allocation; result compacts to base. */
int SV_HeldLedgerRecodeFrom(held_ledger_t *ledger, size_t input_offset,
	int (*rewrite)(held_record_t *, void *), void *context);
void SV_HeldLedgerFail(held_ledger_t *ledger, unsigned int status);
void SV_HeldLedgerDiscard(held_ledger_t *ledger);
/* Generic-only IO shares the same global budget; no authority or file access. */
void *SV_HeldBufferAlloc(size_t size);
void SV_HeldBufferFree(void *buffer, size_t size);
unsigned int SV_HeldLedgerTest(unsigned int *checks);
#endif
