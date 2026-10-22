#ifndef SV_HELD_INITIAL_H
#define SV_HELD_INITIAL_H
#include "quakedef.h"
#include "sv_held_ledger.h"
#include "sv_held_clock.h"
struct client_s;
/* Explicit reviewed host, not automatic support for future mover revisions.
 * CTX retains its fixed70 fields; appended mover10 inputs and mover11's
 * water-assist state are fenced zero. No water/ceiling profile expansion. */
#define HELD_SOURCE_VERSION 11
#define HELD_CONTEXT_PINS 70
/* Native owner-bound candidate0 at the admitted complete-message horizon only.
 * Consent/data/source predicates still apply; never a later import/reseed. */
int SV_HeldInitialCapture(struct client_s *client, held_record_t *record,
	held_clock_t *clock, held_clock_context_t *clock_context, movevars_t *context);
int SV_HeldPhysicalFence(struct client_s *client, int watch_owned);
int SV_HeldAirCell(struct client_s *client);
/* Fresh native parameter packing only, never a later seed/admission result. */
int SV_HeldMoveVarsCapture(struct client_s *client, movevars_t *context);
/* Packing native MV + strict-profile declared shape; caller owns source fence.
 * Never a qualification/readiness result. Used by source capture and model controls. */
int SV_HeldContextBuildNative(const movevars_t *source, held_record_t *record);
/* Native complete-message source context only; includes the actual client fence. */
int SV_HeldContextCapture(struct client_s *client, const movevars_t *source, held_record_t *record);
/* Fixed approved source observations only; ledger owns generation/ordinal. */
int SV_HeldClockBoundaryCapture(struct client_s *client, held_record_family_t family, held_record_t *record);
int SV_HeldObservationCapture(struct client_s *client, unsigned int boundary, held_record_t *record);
#endif
