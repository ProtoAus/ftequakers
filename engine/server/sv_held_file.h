#ifndef SV_HELD_FILE_H
#define SV_HELD_FILE_H
#include "quakedef.h"
#include "sv_held_ledger.h" /* bounded byte-reader interface */

/* Ordinary working source only. No witness bytes or publication capability. */
typedef struct held_file_ticket_s held_file_ticket_t;
typedef struct
{
	const void *vm;
	unsigned int client_uid, entity_index, record_slot, serial;
	char record[96];
} held_source_owner_t;
/* Existing source-owner facts, stamped once while the created file is empty. */
qboolean SV_HeldFileSetOwner(held_file_ticket_t *ticket, const held_source_owner_t *owner);
held_file_ticket_t *SV_HeldFileAcquire(vfsfile_t *stream, const held_source_owner_t *owner);
vfsfile_t *SV_HeldFileCreate(const char *relative, held_file_ticket_t **ticket);
qboolean SV_HeldFileOwnsStream(vfsfile_t *stream, held_file_ticket_t *ticket);
/* -1 means another VFS subtype, 0 refused/failed, 1 resized. */
int SV_HeldFileResize(vfsfile_t *stream, qofs_t length);
qboolean SV_HeldFilePristine(held_file_ticket_t *ticket);
qboolean SV_HeldFileClosed(held_file_ticket_t *ticket);
/* Failed ORIGINAL only: same-object, no-replace private recovery; defers to close. */
qboolean SV_HeldFileRecover(held_file_ticket_t *ticket, const char *stage_root, const char *generation);
/* Existing run_evidence_days age/scope, ORIGINAL runid date, private objects only.
 * Empty map is shared and independently requires run_evidence_sweepall. */
int SV_HeldFileSweep(const char *stage_root, const char *map);
/* Closed ORIGINAL object only; -1 on identity/access/size failure. */
qofs_t SV_HeldFileLength(held_file_ticket_t *ticket);
int SV_HeldFileReadAt(held_file_ticket_t *ticket, qofs_t offset, void *data, int bytes);
/* Caller releases its ticket reference. Failure/preserve never unlinks source. */
qboolean SV_HeldFileRetire(held_file_ticket_t *ticket, qboolean consume);
/* Filesystem sink ONLY: source/model/namespace authority belongs to the native
 * session validator. No QC drain or caller-declared ready/nonserved capability. */
typedef qboolean (*held_store_validator_t)(void *context, const void *bytes, size_t size);
// -1 cleanup/privacy error, 0 refused with source/targets intact, 1 actual commit.
int SV_HeldStorePublish(const char *stage_root, const char *destination, const char *generation,
	const void *bytes, size_t size, const unsigned char digest[32], held_store_validator_t validate, void *context);
/* Same transaction/fences; validator receives NULL bytes and owns its reader context. */
int SV_HeldStorePublishReader(const char *stage_root, const char *destination, const char *generation,
	held_reader_t reader, size_t size, const unsigned char digest[32], held_store_validator_t validate, void *context);
qboolean SV_HeldStoreHealthy(void);

/* Reserved synthetic test namespace, not a recording/admission operation. */
int SV_HeldFileProbe(const char *relative);
int SV_HeldFileTest(unsigned int *checks);
int SV_HeldStoreTest(const char *stage_root, unsigned int *checks);

#endif
