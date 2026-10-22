#ifndef SV_HELD_SESSION_H
#define SV_HELD_SESSION_H
#include "quakedef.h"
#include "sv_held_coverage.h"

void SV_HeldSessionInit(void);
int SV_HeldSessionAvailable(void); /* Pure query, never a namespace/sink constructor. */

/* Internal ownership/lifetime only. None of these functions grants a profile. */
int SV_HeldSessionBegin(client_t *client, const char *generation, const char *identity);
int SV_HeldSessionCheck(client_t *client, const char *generation);
/* 0 refused;1 selected complete candidate;2 latched failed12 (no seed restart). */
int SV_HeldSessionSelect(client_t *client, const char *generation);
int SV_HeldSessionAbort(client_t *client, const char *generation);
int SV_HeldSessionRetire(client_t *client, const char *generation);
void SV_HeldSessionDropClient(client_t *client);
void SV_HeldSessionDropAll(void);
/* Original native callsites, with an independent coverage edge on every call. */
void SV_HeldSourceBoundary(client_t *client, held_boundary_t boundary);
/* Command text/nonce are TRANSIENT native authentication, never retained. */
void SV_HeldSourceCommand(client_t *client, const char *command, qboolean entering);
/* Only after the native counter and client Source state are actually committed. */
void SV_HeldSessionMoved(client_t *client);
int SV_HeldSessionEvent(client_t *client, const char *generation, const char *site);
/* Pending actual exits/closed original bytes, never a publication receipt. */
int SV_HeldSessionRequestClose(client_t *client, const char *generation);
/* Closed original/model binding only; NEVER returns a publication receipt. */
const char *SV_HeldSessionPrepare(client_t *client, const char *generation, const char *pair);
/* Receipt only after native source/model recheck and actual fresh no-replace commit. */
const char *SV_HeldSessionFinalize(client_t *client, const char *generation, const char *pair);

#endif
