/* FTESurf Patch 1467: the movement code's pmove and movevars through this thread's pointers (pmove.c), so a
 * tas_rollout_batch worker moves its own copy. Included LAST by the files whose code a worker runs (pmove.c,
 * pm_source.c, pmovetst.c, sv_user.c): the headers before it, and every other file, see the plain globals --
 * the main thread's -- and Quake 2's player state has members named pmove, so no header may carry this.
 * Pointers, not thread-local objects: GCC 16's native TLS took the address of a thread-local pmove through its
 * plain symbol (.refptr) for indexed writes, and a worker's velocity changes landed in the TLS template. A
 * thread-local pointer is only ever loaded whole (engine/tools/check_tls_refptr.py checks the objects). */
#define pmove (*pm_ctx)
#define movevars (*mv_ctx)
