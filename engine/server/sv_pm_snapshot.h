/* P1827: versioned ordinary-save native snapshots. Not a world/time rewind. */
#ifndef SV_PM_SNAPSHOT_H
#define SV_PM_SNAPSHOT_H
#define SV_PMSNAPSHOT_VALUES 58
#define SV_PMSNAPSHOT_TEXT 2048
#define SV_PMSNAPSHOT_FLAGS (FL_ONGROUND | FL_WATERJUMP | FL_JUMPRELEASED)
enum {
 PMS_MODEL, PMS_VERSION, PMS_EPOCH, PMS_MT, PMS_TICK, PMS_PIN,
 PMS_FLAGS, PMS_PMFLAGS, PMS_JUMP, PMS_GROUND,
 PMS_ORG, PMS_VEL = PMS_ORG+3, PMS_MINS = PMS_VEL+3,
 PMS_MAXS = PMS_MINS+3, PMS_VIEW = PMS_MAXS+3, PMS_GRAVDIR = PMS_VIEW+3,
 PMS_GRAVITY = PMS_GRAVDIR+3, PMS_ENTGRAVITY, PMS_WATERLEVEL, PMS_WATERTYPE,
 PMS_TELEPORT, PMS_LIMIT, PMS_COOL, PMS_SLIDE, PMS_STATE
};
typedef struct {
 double v[SV_PMSNAPSHOT_VALUES];
 pmsourcestate_t state;
} sv_pm_snapshot_t;
unsigned int SV_PMSnapshotPin(const float *pin);
qboolean SV_PMSnapshotNear(double a, double b, double tolerance);
qboolean SV_PMSnapshotClock(int base, int counter, int origin, int *ticks);
qboolean SV_PMSnapshotHorizon(int mt, float carry, int next, float nextcarry, float rate, float *duration);
qboolean SV_PMSnapshotInput(const char *args, float rate);
qboolean SV_PMSnapshotGravity(float entity, float world, float *cached, float *multiplier);
qboolean SV_PMSnapshotParse(const char *text, sv_pm_snapshot_t *snapshot);
qboolean SV_PMSnapshotText(const sv_pm_snapshot_t *snapshot, char *out, size_t size);
qboolean SV_PMSnapshot(edict_t *e, const char *text, int operation, char *out, size_t size);
void SV_PMSnapshotVector(const sv_pm_snapshot_t *s, int field, float *out);
qboolean SV_PMSnapshotStateEqual(const pmsourcestate_t *a, const pmsourcestate_t *b);
unsigned int SV_PMSnapshotTest(unsigned int *checks);
#endif
