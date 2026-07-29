/*
 * Quakers in-game delta updater. See common/qkupdate.c.
 *
 * Four call sites, deliberately: registration, shutdown, crash-recovery replay, and the
 * QC-visible cvars/commands (which need no header at all).
 */
#ifndef QKUPDATE_H__
#define QKUPDATE_H__

void QKU_Init(void);			//register cvars + commands. call from COM_Init, after COM_InitFilesystem.
void QKU_Shutdown(void);		//call immediately BEFORE HTTP_CL_Terminate, so in-flight downloads still have a valid target.
void QKU_ReplayJournal(void);	//call at the TAIL of COM_InitFilesystem: paths are resolved, but nothing is mounted or dlopen()ed yet.

#endif
