/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
#include "quakedef.h"
#include "pr_common.h"
#include "fs.h"
#include "netinc.h"

#ifndef CLIENTONLY

#ifndef INVALID_SOCKET
#define INVALID_SOCKET -1
#endif



int	sv_allow_cheats;
qboolean SV_MayCheat(void)
{
	if (sv_allow_cheats == 2)
		return sv.allocated_client_slots == 1;
	return sv_allow_cheats!=0;
}

#ifdef SUBSERVERS
cvar_t sv_autooffload = CVARD("sv_autooffload", "0", "Automatically start the server in a separate process, so that sporadic or persistent gamecode slowdowns do not affect visual framerates (equivelent to the mapcluster command). Note: Offloaded servers have separate cvar+command states which may complicate usage.");
#endif
extern cvar_t cl_warncmd;
/*
FTESurf Patch 153: sv_cheats takes effect NOW, not at the next map load.

It was CVAR_MAPLATCH, so `sv_cheats 1` printed "will be changed after a map
load" and every cheat-flagged cvar stayed rejected until you reloaded -- which,
on a 25-second Source map, is an expensive way to look at r_showtris.

The two things a map spawn does with it (sv_init.c:980-995) are set
sv_allow_cheats and publish the *cheats serverinfo key, and both are just as
valid mid-map: nothing about them is bound to the map being loaded.  So the
callback does exactly that work and then re-derives cls.allow_cheats through
CL_CheckServerInfo, which is the same call the spawn path already makes.

Both directions are live.  Turning cheats back OFF mid-map immediately
re-latches every cheat cvar, which is the honest counterpart -- an on-only
switch would leave a server advertising *cheats "" while the client still
believed it could set them.
*/
static void QDECL SV_Cheats_Callback (struct cvar_s *var, char *oldvalue)
{
	if (sv.state != ss_active)
		return;	//nothing to update yet; the map spawn will do it.

	if (var->ival)
	{
		sv_allow_cheats = true;
		InfoBuf_SetStarKey(&svs.info, "*cheats", "ON");
	}
	else
	{
		sv_allow_cheats = 2;	//"single player only", same as the spawn path
		InfoBuf_SetStarKey(&svs.info, "*cheats", "");

		//FTESurf Patch 170: turning cheats off puts the movement ruleset back.
		//Same principle as the re-latch above -- an off switch that left the
		//physics wherever you had dragged them would be worse than no switch,
		//because the console would then agree with the HUD that cheats were
		//off while you were still playing on your own numbers.
		SV_LockMovementVars();
	}

	//FTESurf Patch 313: sv_cheats is the live half of SV_MovementLocked(), so
	//*ruleset has to move with it in BOTH directions -- same argument the
	//comment above makes for the re-latch.  Placed after the branch rather than
	//in each arm so the two can never drift apart.
	SV_PublishRuleset();

#ifndef SERVERONLY
	//the local client caches its own copy of serverinfo and derives
	//cls.allow_cheats from it; without this the cvar changes and nothing that
	//reads allow_cheats notices.
	InfoBuf_Clone(&cl.serverinfo, &svs.info);
	if (!isDedicated)
		CL_CheckServerInfo();
#endif
}
cvar_t sv_cheats = CVARFC("sv_cheats", "0", 0, SV_Cheats_Callback);
	extern		redirect_t	sv_redirected;

extern cvar_t sv_public;

static const struct banflags_s
{
	unsigned int banflag;
	const char *names[2];
} banflags[] =
{
	{BAN_BAN,		{"ban"}},
	{BAN_PERMIT,	{"safe",		"permit"}},
	{BAN_CUFF,		{"cuff"}},
	{BAN_MUTE,		{"mute"}},
	{BAN_VMUTE,		{"vmute"}},
	{BAN_CRIPPLED,	{"cripple"}},
	{BAN_DEAF,		{"deaf"}},
	{BAN_LAGGED,	{"lag",		"lagged"}},
	{BAN_VIP,		{"vip"}},
	{BAN_BLIND,		{"blind"}},
	{BAN_SPECONLY,	{"spec"}},
	{BAN_STEALTH,	{"stealth"}},
	{BAN_MAPPER,	{"mapper"}},

	{BAN_USER1,		{"user1"}},
	{BAN_USER2,		{"user2"}},
	{BAN_USER3,		{"user3"}},
	{BAN_USER4,		{"user4"}},
	{BAN_USER5,		{"user5"}},
	{BAN_USER6,		{"user6"}},
	{BAN_USER7,		{"user7"}},
	{BAN_USER8,		{"user8"}}
};

//generic helper function for naming players.
client_t *SV_GetClientForString(const char *name, int *id)
{
	int i;
	const char *s;
	char nicename[80];
	char niceclname[80];
	client_t *cl;

	int first=0;
	if (id && *id != -1)
		first = *id;
	if (first < 0)
	{
		if (id)
			*id=sv.allocated_client_slots;
		return NULL;
	}

	if (!strcmp(name, "*"))	//match with all
	{
		for (i = first, cl = svs.clients+first; i < sv.allocated_client_slots; i++, cl++)
		{
			if (cl->state<=cs_loadzombie)
				continue;

			if (id)
				*id=i+1;
			return cl;
		}
		if (id)
			*id=sv.allocated_client_slots;
		return NULL;
	}

	//check to make sure it's all an int

	for (s = name; *s; s++)
	{
		if (*s < '0' || *s > '9')
			break;
	}

	//we got to the end of the string and found only numbers. - it's a uid.
	if (!*s)
	{
		int uid = Q_atoi(name);
		for (i = first, cl = svs.clients+first; i < sv.allocated_client_slots; i++, cl++)
		{
			if (cl->state<=cs_loadzombie)
				continue;
			if (cl->userid == uid)
			{
				if (id)
					*id=sv.allocated_client_slots;
				return cl;
			}
		}

		return NULL;
	}

	for (i = first, cl = svs.clients+first; i < sv.allocated_client_slots; i++, cl++)
	{
		if (cl->state<=cs_loadzombie)
			continue;


		deleetstring(niceclname, cl->name);
		deleetstring(nicename, name);

		if (strstr(niceclname, nicename))
		{
			if (id)
				*id=i+1;
			return cl;
		}
	}

	return NULL;
}

/*
===============================================================================

OPERATOR CONSOLE ONLY COMMANDS

These commands can only be entered from stdin or by a remote operator datagram
===============================================================================
*/

/*
==================
SV_Quit_f
==================
*/
static void SV_Quit_f (void)
{
	if (sv.state >= ss_loading)
		SV_FinalMessage ("server shutdown\n");
	Con_TPrintf ("Shutting down.\n");
	SV_Shutdown ();
	Sys_Quit ();
}

/*
============
SV_Fraglogfile_f
============
*/
static void SV_Fraglogfile_f (void)
{
	char	name[MAX_OSPATH];
	int		i;

	if (sv_fraglogfile)
	{
		Con_TPrintf ("Frag file logging off.\n");
		VFS_CLOSE (sv_fraglogfile);
		sv_fraglogfile = NULL;
		return;
	}

	// find an unused name
	for (i=0 ; i<1000 ; i++)
	{
		sprintf (name, "frag_%i.log", i);
		sv_fraglogfile = FS_OpenVFS(name, "rb", FS_GAME);
		if (!sv_fraglogfile)
		{	// can't read it, so create this one
			sv_fraglogfile = FS_OpenVFS (name, "wb", FS_GAME);
			if (!sv_fraglogfile)
				i=1000;	// give error
			break;
		}
		VFS_CLOSE (sv_fraglogfile);
	}
	if (i==1000)
	{
		Con_TPrintf ("Can't open any logfiles.\n");
		sv_fraglogfile = NULL;
		return;
	}

	Con_TPrintf ("Logging frags to %s.\n", name);
}


/*
==================
SV_SetPlayer

Sets host_client and sv_player to the player with idnum Cmd_Argv(1)
==================
*/
static qboolean SV_SetPlayer (void)
{
	client_t	*cl;
	int			i;
	int			idnum;

	idnum = atoi(Cmd_Argv(1));

	for (i=0,cl=svs.clients ; i<sv.allocated_client_slots ; i++,cl++)
	{
		if (!cl->state)
			continue;
		if (cl->userid == idnum)
		{
			host_client = cl;
			sv_player = host_client->edict;
			return true;
		}
	}
	Con_TPrintf ("Userid %i is not on the server\n", idnum);
	return false;
}


/*
==================
SV_God_f

Sets client to godmode
==================
*/
static void SV_God_f (void)
{
	if (!SV_MayCheat())
	{
		Con_TPrintf ("Please set sv_cheats 1 and restart the map first.\n");
		return;
	}

	if (!SV_SetPlayer ())
		return;

	SV_LogPlayer(host_client, "god cheat");
	sv_player->v->flags = (int)sv_player->v->flags ^ FL_GODMODE;
	if ((int)sv_player->v->flags & FL_GODMODE)
		SV_ClientTPrintf (host_client, PRINT_HIGH, "godmode ON\n");
	else
		SV_ClientTPrintf (host_client, PRINT_HIGH, "godmode OFF\n");
}


static void SV_Noclip_f (void)
{
	if (!SV_MayCheat())
	{
		Con_TPrintf ("Please set sv_cheats 1 and restart the map first.\n");
		return;
	}

	if (!SV_SetPlayer ())
		return;

	SV_LogPlayer(host_client, "noclip cheat");
	if (sv_player->v->movetype != MOVETYPE_NOCLIP)
	{
		sv_player->v->movetype = MOVETYPE_NOCLIP;
		SV_ClientTPrintf (host_client, PRINT_HIGH, "noclip ON\n");
	}
	else
	{
		sv_player->v->movetype = MOVETYPE_WALK;
		SV_ClientTPrintf (host_client, PRINT_HIGH, "noclip OFF\n");
	}
}

#ifdef QUAKESTATS
/*
==================
SV_Give_f
==================
*/
static void SV_Give_f (void)
{
	char	*t = Cmd_Argv(2);
	int		v;

	if (!svprogfuncs)
		return;

	if (!strcmp(t, "damn"))
	{
		Con_TPrintf ("%s not given.\n", t);
		return;
	}

	if (!SV_MayCheat())
	{
		Con_TPrintf ("Please set sv_cheats 1 and restart the map first.\n");
		return;
	}

/*	if (developer.value)
	{
		int oldself;
		oldself = pr_global_struct->self;
		pr_global_struct->self = EDICT_TO_PROG(svprogfuncs, sv.world.edicts);
		Con_Printf("Result: %s\n", svprogfuncs->EvaluateDebugString(svprogfuncs, Cmd_Args()));
		pr_global_struct->self = oldself;
	}
*/
	if (!SV_SetPlayer ())
	{
		return;
	}

	SV_LogPlayer(host_client, "give cheat");

	v = atoi (Cmd_Argv(3));

	switch ((t[1]==0)?t[0]:0)
	{
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
	case '8':
	case '9':
		sv_player->v->items = (int)sv_player->v->items | IT_SHOTGUN<< (t[0] - '2');
		break;

	case 's':
		sv_player->v->ammo_shells = v;
		break;
	case 'n':
		sv_player->v->ammo_nails = v;
		break;
	case 'r':
		sv_player->v->ammo_rockets = v;
		break;
	case 'h':
		sv_player->v->health = v;
		break;
	case 'c':
		sv_player->v->ammo_cells = v;
		break;
/*	default:
		{
			int oldself;
			oldself = pr_global_struct->self;
			pr_global_struct->self = EDICT_TO_PROG(svprogfuncs, sv_player);
			Cmd_ShiftArgs(1, false);
			Con_TPrintf("Result: %s\n", svprogfuncs->EvaluateDebugString(svprogfuncs, Cmd_Args()));
			pr_global_struct->self = oldself;
		}
*/
	}
}
#endif


#if defined(HAVE_LEGACY) && defined(HAVE_SERVER)
static void SV_redundantcommand_f(void)
{
	if (cl_warncmd.ival)
		Con_Printf("%s is obsolete, redundant, or otherwise outdated.\n", Cmd_Argv(0));
}
#endif

static int QDECL ShowMapList (const char *name, qofs_t flags, time_t mtime, void *parm, searchpathfuncs_t *spath)
{
	searchpathfuncs_t **oldspath = parm;
	const char *levelshots[] =
	{
		"levelshots/%s.tga",
		"levelshots/%s.jpg",
		"levelshots/%s.png",
		"maps/%s.tga",
		"maps/%s.jpg",
		"maps/%s.png"
	};
	size_t u;
	char stripped[MAX_QPATH];
	char completed[256];
	const char *cmd = name+5; //the arg to pass to `map`
	const char *ext;
	flocation_t loc;
	if (name[5] == 'b' && name[6] == '_')	//skip box models
		return true;

	if (FS_FLocateFile(name, FSLF_IFFOUND, &loc))
	{
		if (loc.search->handle != spath)
			return true; //shadowed
	}
	else
		return true; //wtf?

	ext = COM_GetFileExtension (name+5, NULL);
	if (!strcmp(ext, ".gz") || !strcmp(ext, ".xz"))
		ext = COM_GetFileExtension (name+5, ext);	//.gz files should be listed too.

	if (!strcmp(ext, ".bsp") || !Q_strcasecmp(ext, ".d3dbsp") || !Q_strcasecmp(ext, ".cm"))
	{
		ext = "";	//hide it
		cmd = stripped;	//omit it, might as well. should give less confusing mapname serverinfo etc.
	}
	else if (!Q_strcasecmp(ext, ".bsp") || !Q_strcasecmp(ext, ".bsp.gz") || !Q_strcasecmp(ext, ".bsp.xz"))
		;
	else if (!Q_strcasecmp(ext, ".d3dbsp") || !Q_strcasecmp(ext, ".d3dbsp.gz") || !Q_strcasecmp(ext, ".d3dbsp.xz"))
		;	//cod2 compat. vile.
#ifdef TERRAIN
	else if (!Q_strcasecmp(ext, ".map") || !Q_strcasecmp(ext, ".map.gz") || !Q_strcasecmp(ext, ".hmp"))
		;
#endif
	else if (!Q_strcasecmp(ext, ".ent") && strchr(name+5, '#'))
	{	//FIXME hide if earlier that the .bsp
		ext = ""; //hide it.
		cmd = stripped;	//do NOT use the .ent extension here
	}
	else
		return true; //probably a .lit

	if (*oldspath != spath)
	{
		*oldspath = spath;
		Con_Printf(S_COLOR_GRAY"From %s\n", loc.search->purepath);
	}

	*completed = 0;
#ifdef HAVE_CLIENT
	{
		float besttime, fulltime, kills, secrets;
		if (Log_CheckMapCompletion(NULL, name, &besttime, &fulltime, &kills, &secrets))
		{
			if (kills || secrets)
				Q_snprintfz(completed, sizeof(completed), "^7 - ^2best: ^1%.1f^2, full: ^1%.1f^2 (^1%.0f^2 kills, ^1%.0f^2 secrets)", besttime, fulltime, kills, secrets);
			else
				Q_snprintfz(completed, sizeof(completed), "^7 - ^2best: ^1%.1f^2", besttime);
		}
	}
#endif

	COM_StripExtension(name+5, stripped, sizeof(stripped));
	for (u = 0; u < countof(levelshots); u++)
	{
		const char *ls = va(levelshots[u], stripped);
		if (COM_FCheckExists(ls))
		{
			Con_Printf("^[\\map\\%s\\img\\%s\\w\\64\\h\\48^]", cmd, ls);
			Con_Printf("^[[%s%s]%s\\map\\%s\\tipimg\\%s\\tip\\from %s/%s^]\n", stripped, ext, completed, cmd, ls, loc.search->logicalpath, name);
			return true;
		}
	}
	Con_Printf("^[[%s%s]%s\\map\\%s\\tip\\from %s/%s^]\n", stripped, ext, completed, cmd, loc.search->logicalpath, name);
	return true;
}
static void SV_MapList_f(void)
{
	searchpathfuncs_t *spath = NULL;
	COM_EnumerateFilesReverse("maps/*.*", ShowMapList, &spath);
	COM_EnumerateFilesReverse("maps/*/*.*", ShowMapList, &spath);
	COM_EnumerateFilesReverse("maps/*/*/*.*", ShowMapList, &spath);
}

/*
FTESurf Patch 215: lenient map completion.

	"is it possible to make the map command be full lenient? so "map kits" in
	 console will auto populate all maps with kits, like surf_kitsune and
	 surf_kitsune_mom ect."

SV_Map_c globs "maps/<typed>*.bsp", so the typed text has to be the START of the
name.  On a library whose maps are all called surf_<something> that means the
first token you can usefully type is "surf_", and the part you actually remember
-- kitsune -- can never find anything.

1 makes that final component "*<typed>*.bsp" instead, so the text may appear
anywhere.  Nothing else changes: wildcmp() (common.c) is a real recursive glob in
which '*' matches any run of characters except a path separator, and every
searchpath backend this game uses filters through it -- the raw directory
(sys_win.c asks the OS for a bare "everything" pattern and runs its own wildcmp on
each name, so FindFirstFile never sees ours and its DOS wildcard quirks cannot
apply), .pak, .pk3 and zip, dzip, and the Source .vpk via the plugin's
filefuncs->WildCmp.  So there is no backend here where a leading '*' is a special
case.

Portability note rather than a live one: an SDL3 non-Windows build routes
Sys_EnumerateFiles to SDL_GlobDirectory instead, whose own comment in sys_sdl.c
records '*' crossing the separator and walking the whole tree under wine.  This
build is native win32, and the default is 0, so that path is not ours today.

Cost on the leaf globs is nothing: an empty argument ALREADY enumerates every map,
so a full pass is the existing baseline rather than a new one.

Two things deliberately left alone:
 - PM_EnumerateMaps takes the ORIGINAL text.  It is a Q_strncasecmp prefix test
   over package names (m_download.c), not a glob, so handing it "*kits" would
   silently match nothing.  Its namespace is pkg:map and leniency over it is not
   what was asked for.
 - a text that already contains '*' or '?' is passed through untouched -- if you
   are globbing by hand, that is the glob you meant.
*/
cvar_t sv_mapcompletion = CVARD("sv_mapcompletion", "0", "How the map command's tab-completion and dropdown match what you have typed.\n0: the text must be a PREFIX of the map name (default).\n1: lenient -- the text may appear ANYWHERE in the name, so `map kits` offers surf_kitsune and surf_kitsune_mom.");

static int QDECL CompleteMapList (const char *name, qofs_t flags, time_t mtime, void *parm, searchpathfuncs_t *spath)
{
	struct xcommandargcompletioncb_s *ctx = parm;
	char stripped[64];
	if (name[5] == 'b' && name[6] == '_')	//skip box models
		return true;

	COM_StripExtension(name+5, stripped, sizeof(stripped));
	ctx->cb(stripped, NULL, NULL, ctx);
	return true;
}
static int QDECL CompleteMapListEnt (const char *name, qofs_t flags, time_t mtime, void *parm, searchpathfuncs_t *spath)
{
	struct xcommandargcompletioncb_s *ctx = parm;
	char stripped[64];
	char *modifier = strchr(name, '#');
	if (!modifier)	//skip non-modifiers.
		return true;
	if (modifier-name+4 > sizeof(stripped))	//too long...
		return true;

	//make sure we have its .bsp
	memcpy(stripped, name, modifier-name);
	strcpy(stripped+(modifier-name), ".bsp");
	if (!COM_FCheckExists(stripped))
		return true;

	COM_StripExtension(name+5, stripped, sizeof(stripped));
	ctx->cb(stripped, NULL, NULL, ctx);
	return true;
}

static int QDECL CompleteMapListExt (const char *name, qofs_t flags, time_t mtime, void *parm, searchpathfuncs_t *spath)
{
	struct xcommandargcompletioncb_s *ctx = parm;
	if (name[5] == 'b' && name[6] == '_')	//skip box models
		return true;

	ctx->cb(name+5, NULL, NULL, ctx);
	return true;
}
static void SV_Map_c(int argn, const char *partial, struct xcommandargcompletioncb_s *ctx)
{
	if (argn == 1)
	{
		const char *raw = partial;	//the text exactly as typed
		char lenient[MAX_QPATH];

		/*
		FTESurf Patch 215: see the essay above sv_mapcompletion. Rewriting the
		text ONCE here rather than at each glob keeps the nine leaf globs below
		byte-identical to build 28, so sv_mapcompletion 0 is provably today.

		`raw` survives for the two places leniency must NOT reach:

		 - the eight subdirectory globs below, the ones whose %s names a
		   DIRECTORY rather than a map. The win32 enumerator uses that component
		   as its recursion gate: it opens and walks every directory the pattern
		   matches. A leading '*' widens that from "subdirectories starting with
		   the text" to "subdirectories containing it", once per glob per
		   searchpath, to complete a thing nobody asked to complete.
		 - PM_EnumerateMaps, which is a Q_strncasecmp prefix test over package
		   names rather than a glob, so a '*' would silently match nothing.
		*/
		if (sv_mapcompletion.ival && *partial && !strchr(partial, '*') && !strchr(partial, '?'))
		{
			Q_snprintfz(lenient, sizeof(lenient), "*%s", partial);
			partial = lenient;
		}

		//FIXME: maps/mapname#modifier.ent
		COM_EnumerateFiles(va("maps/%s*.bsp", partial), CompleteMapList, ctx);
		COM_EnumerateFiles(va("maps/%s*.d3dbsp", partial), CompleteMapList, ctx);
		COM_EnumerateFiles(va("maps/%s*.bsp.gz", partial), CompleteMapListExt, ctx);
		COM_EnumerateFiles(va("maps/%s*.bsp.xz", partial), CompleteMapListExt, ctx);
		COM_EnumerateFiles(va("maps/%s*.map", partial), CompleteMapListExt, ctx);
		COM_EnumerateFiles(va("maps/%s*.map.gz", partial), CompleteMapListExt, ctx);
		COM_EnumerateFiles(va("maps/%s*.cm", partial), CompleteMapList, ctx);
		COM_EnumerateFiles(va("maps/%s*.hmp", partial), CompleteMapList, ctx);

		COM_EnumerateFiles(va("maps/%s*.ent", partial), CompleteMapListEnt, ctx);

		COM_EnumerateFiles(va("maps/%s*/*.bsp", raw), CompleteMapList, ctx);
		COM_EnumerateFiles(va("maps/%s*/*.d3dbsp", raw), CompleteMapList, ctx);
		COM_EnumerateFiles(va("maps/%s*/*.bsp.gz", raw), CompleteMapListExt, ctx);
		COM_EnumerateFiles(va("maps/%s*/*.bsp.xz", raw), CompleteMapListExt, ctx);
		COM_EnumerateFiles(va("maps/%s*/*.map", raw), CompleteMapListExt, ctx);
		COM_EnumerateFiles(va("maps/%s*/*.map.gz", raw), CompleteMapListExt, ctx);
		COM_EnumerateFiles(va("maps/%s*/*.cm", raw), CompleteMapList, ctx);
		COM_EnumerateFiles(va("maps/%s*/*.hmp", raw), CompleteMapList, ctx);

#ifdef PACKAGEMANAGER
		PM_EnumerateMaps(raw, ctx);
#endif
	}
}

#ifdef WEBCLIENT
static char *uri_escape(const char *in, char *out, size_t outsize)
{
	static const char *hex = "0123456789ABCDEF";

	const unsigned char *s = in;
	unsigned char *o = out;
	while (*s && o < (unsigned char*)out+outsize-4)
	{
		//unreserved chars according to RFC3986
		if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9')
				|| *s == '.' || *s == '-' || *s == '_' || *s == '~')
			*o++ = *s++;
		else
		{
			*o++ = '%';
			*o++ = hex[*s>>4];
			*o++ = hex[*s&0xf];
			s++;
		}
	}
	*o = 0;
	return out;
}
static void SV_Map_DownloadCanceled(const char *mapname)
{
#ifdef HAVE_SERVER
	if (SSV_IsSubServer() && !sv.state)	//subservers don't leave defunct servers with no maps lying around.
		Cbuf_AddText("\nquit\n", RESTRICT_LOCAL);
	if (isDedicated && !sv.state && !COM_CheckParm("-allowmapless"))
		SV_Error (CON_ERROR"Couldn't download map %s.", mapname);
#endif
#ifdef HAVE_CLIENT
	SCR_SetLoadingStage(LS_NONE);
#endif
}
static void SV_Map_Downloaded(struct dl_download *dl)
{
	char buf[1024];
#ifdef HAVE_CLIENT
	SCR_SetLoadingStage(LS_NONE);
#endif
	if (dl->status == DL_FINISHED)
	{
		Cbuf_AddText(va("map %s\n", COM_QuotedString(dl->user_ctx, buf, sizeof(buf), false)), RESTRICT_LOCAL);
	}
	else
	{
		Con_Printf("Unable to download map %s\n", COM_QuotedString(dl->user_ctx, buf, sizeof(buf), false));
		SV_Map_DownloadCanceled(dl->user_ctx);
	}
	Z_Free(dl->user_ctx);
}
extern cvar_t cl_download_mapsrc;
static qboolean SV_Map_DownloadStart(char *mapname/*must be duped*/)
{
	char buf[512];
	struct dl_download *dl = HTTP_CL_Get(va("%s%s.bsp", cl_download_mapsrc.string, uri_escape(mapname, buf, sizeof(buf))), va("maps/%s.bsp", mapname), SV_Map_Downloaded);
	if (dl)
	{
		dl->user_ctx = mapname;
		DL_CreateThread(dl, NULL, NULL);	//allows it to run at its own rate. yay speedups.
		return true;
	}
	Z_Free(mapname);
	return false;
}
#ifdef HAVE_CLIENT
static void SV_Map_DownloadPrompted(void *ctx, promptbutton_t buttn)
{
	if (buttn == PROMPT_YES)
	{
		if (SV_Map_DownloadStart(ctx))
			return;
		SV_Map_DownloadCanceled(ctx);
	}
	else
	{
		SV_Map_DownloadCanceled(ctx);
		Z_Free(ctx);
	}
}
#endif
static qboolean SV_Map_DownloadPrompt(const char *mapname)
{
#ifdef HAVE_SERVER
	if (isDedicated)
	{
		return SV_Map_DownloadStart(Z_StrDup(mapname));
	}
#endif
#ifdef HAVE_CLIENT
	Menu_Prompt(SV_Map_DownloadPrompted, Z_StrDup(mapname), va(localtext("Download map %s from "S_COLOR_BLUE "%s" S_COLOR_WHITE"?"), mapname, cl_download_mapsrc.string), "Download", NULL, "Cancel", true);
	return true;
#else
	return false;
#endif
}
#endif

//static void gtcallback(struct cvar_s *var, char *oldvalue)
//{
//	Con_Printf("g_gametype changed\n");
//}

/*
======================
SV_Map_f

handle a
map <mapname>
command from the console or progs.

quirks:
a leading '*' means new unit, meaning all old map state is flushed regardless of startspot
a '+' means 'set nextmap cvar to the following value and otherwise ignore, for q2 compat. only applies if there's also a '.' and the specified bsp doesn't exist, for q1 compat.
just a '.' is taken to mean 'restart'. parms are not changed from their current values, startspot is also unchanged. Loads the last saved game instead when applicable.

variations:
'map' will change map, for most games. strips parms+serverflags+cache. note that vanilla NQ kicks everyone (NQ expects you to use changelevel for that).
'changelevel' will not flush the level cache, for h2 compat (won't save current level state in such a situation, as nq would prefer not)
'gamemap' will save the game to 'save0' after loading, for q2 compat
'spmap' is for q3 and sets 'gametype' to '2', otherwise identical to 'map'. all other map commands will reset it to '0' if its '2' at the time.
'spdevmap' forces sv_cheats 1, otherwise spmap
'devmap' forces sv_cheats 1, otherwise map
'map_restart' restarts the current map. Name is needed for q3 compat.
'restart' is an alias for 'map_restart'. Exists for NQ compat, but as an alias for QW mods that tried to use it for mod-specific things.

hexen2 fixme:
'restart restore' restarts the map, reloading from a saved game if applicable.
'restart' forgets the current map (potentially breaking the game). we don't care much for that behaviour (could make it a 'restart unit' I guess).

quake2:
'gamemap [*]foo.dm2[$spot][+nextserver]'
	* == new unit
	$ == start spot
	+ == value for nextserver cvar (used for cinematics).
'map' is always a new unit.

quake:
+ is used in certain map names. * cannot be, but $ potentially could be.

fte:
'map package:mapname' should download the specified map package and load up its maps.

mvdsv:
'map foo bar' should load 'maps/bar.ent' instead of the regular ent file. this 'bar' will usually be something like 'foo#modified'

======================
*/
//nettest (P26 Part 2): the game spec from a "map @<spec>/<mapname>" qualifier, consumed once by the next
//SV_SpawnServer to bias the worldmodel locate to that game's copy.  "" = no qualifier (plain priority).
char sv_mappreferhint[MAX_OSPATH];

void SV_Map_f (void)
{
	char	level[MAX_QPATH];
	char	spot[MAX_QPATH];
	char	expanded[MAX_QPATH+64];
	char	*nextserver = NULL;
	qboolean preserveplayers= false;
	qboolean isrestart		= false;	//don't hurt settings
#ifdef SAVEDGAMES
	qboolean newunit		= false;	//no hubcache
	qboolean q2savetos0		= false;
#endif
	qboolean flushparms		= false;	//flush parms+serverflags
	qboolean cinematic		= false;	//new map is .cin / .roq or something
#ifdef Q3SERVER
	qboolean q3singleplayer	= false;	//forces g_gametype to 2 (otherwise clears if it was 2).
#endif
	qboolean waschangelevel	= false;
	qboolean mapeditor		= false;
	qboolean forceCheats = false;
	int i;
	char *startspot;
	const char *cmd = Cmd_Argv(0);

	//nettest (P26 Part 2): default to NO map-prefer-hint.  Only an explicit "@spec/map" arg (parsed below)
	//sets it, so paths that skip the parse (map_restart, changelevel, savegame restore) can't inherit a
	//stale hint from a previous qualified load.  SV_SpawnServer also consumes it one-shot.
	sv_mappreferhint[0] = 0;

#ifndef SERVERONLY
	if (!Renderer_Started() && !isDedicated)
	{
		Cbuf_AddText(va("wait;%s %s\n", cmd, Cmd_Args()), Cmd_ExecLevel);
		return;
	}
#endif

#ifdef SUBSERVERS
	//disconnect first if you want to stop your current server getting the command instead.
	if (sv.state == ss_clustermode && MSV_ForwardToAutoServer())
		return;
#endif

	if (!Q_strcasecmp(cmd, "map_restart"))
	{
		const char *arg = Cmd_Argv(1);

#ifdef Q3SERVER
		Cvar_ApplyLatches(CVAR_MAPLATCH, false);
		if (sv.state==ss_active && svs.gametype==GT_QUAKE3 && q3->sv.RestartGamecode())
		{
			sv.time = sv.world.physicstime;
			sv.starttime = Sys_DoubleTime() - sv.time;
			return;
		}
#endif

#ifdef SAVEDGAMES
		if (!strcmp(arg, "restore"))		//hexen2 reload-saved-game
			;
		else if (!strcmp(arg, "initial"))	//force initial, even if it breaks saved games.
			*sv.loadgame_on_restart = 0;
		else
#endif
		{
			float delay = atof(arg);
			if (delay)			//q3's restart-after-delay
				Con_DPrintf ("map_restart delay not implemented yet\n");
		}
		Q_strncpyz (level, ".", sizeof(level));
		startspot = NULL;
		isrestart = true;

		//FIXME: if precaches+statics don't change, don't do the whole networking thing.
	}
	else
	{
		if (Cmd_Argc() != 2 && Cmd_Argc() != 3)
		{
			if (Cmd_IsInsecure())
				return;
			Con_TPrintf ("Available maps:\n", Cmd_Argv(0));
			SV_MapList_f();
			return;
		}

#ifdef PACKAGEMANAGER
		if (Cmd_Argc() == 2)
		{
			char *mangled = Cmd_Argv(1);
			char *sep = strchr(mangled, ':');
			if (sep && *mangled != '@' && strncmp(mangled, "file:", 5) && strncmp(mangled, "http:", 5) && strncmp(mangled, "https:", 5))	//nettest (P26 Part 2): '@spec/map' is our prefer-hint form, not a package, even though the spec contains ':'
			{
				*sep++ = 0;
				if (Cmd_FromGamecode())
				{
					Con_TPrintf ("switching packages via %s command is blocked from gamecode, just in case.\n", Cmd_Argv(0));
					sv.mapchangelocked = false;
				}
				else
					PM_LoadMap(mangled, va("%s %s\n", Cmd_Argv(0), COM_QuotedString(sep, expanded, sizeof(expanded), false)));
				return;
			}
		}
#endif

		//nettest (P26 Part 2): a "@<gamespec>/<mapname>" arg (always QUOTED by the menu, so it is ONE token)
		//asks to load a SPECIFIC game's copy of a same-named map.  Split on the LAST '/' (the spec itself
		//contains '/' and ':'), stash the spec for SV_SpawnServer to bias the worldmodel locate, and use the
		//bare mapname for everything downstream.  A plain map name clears the hint.
		{
			const char *a1 = Cmd_Argv(1);
			sv_mappreferhint[0] = 0;
			if (*a1 == '@')
			{
				const char *lastslash = strrchr(a1, '/');
				if (lastslash && lastslash > a1+1)
				{
					size_t taglen = lastslash - (a1+1);
					if (taglen >= sizeof(sv_mappreferhint))
						taglen = sizeof(sv_mappreferhint)-1;
					memcpy(sv_mappreferhint, a1+1, taglen);
					sv_mappreferhint[taglen] = 0;
					Q_strncpyz (level, lastslash+1, sizeof(level));
				}
				else
					Q_strncpyz (level, a1, sizeof(level));	//malformed (@ with no '/') -> load as-is
			}
			else
				Q_strncpyz (level, a1, sizeof(level));
		}
		startspot = ((Cmd_Argc() == 2)?NULL:Cmd_Argv(2));
	}

#ifdef Q3SERVER
	q3singleplayer = !strncmp(cmd, "sp", 2);
#endif
	if ((svs.gametype == GT_PROGS || svs.gametype == GT_Q1QVM) && progstype == PROG_QW)
		flushparms = !strncmp(cmd, "sp", 2);	//quakeworld's map command preserves spawnparms. q3 doesn't do parms, so we might as well reuse sp[dev]map to flush in qw
	else
		flushparms = !strcmp(cmd, "map") || !strncmp(cmd, "sp", 2); //[sp]map flushes in nq+h2+q2+etc
#ifdef SAVEDGAMES
	newunit = flushparms || (!strcmp(Cmd_Argv(0), "changelevel") && !startspot);
	q2savetos0 = !strcmp(cmd, "gamemap") && !isDedicated;	//q2
#endif
	mapeditor = !strcmp(Cmd_Argv(0), "mapedit");
	forceCheats = !strcmp(Cmd_Argv(0), "devmap");

	sv.mapchangelocked = false;

	if (!strcmp(level, "."))
		;//restart current - deprecated.
	else
	{
		snprintf (expanded, sizeof(expanded), "maps/%s.bsp", level); // this function and the if statement below, is a quake bugfix which stopped a map called "dm6++.bsp" from loading because of the + sign, quake2 map syntax interprets + character as "intro.cin+base1.bsp", to play a cinematic then load a map after
		if (!COM_FCheckExists (level) && !COM_FCheckExists (expanded))
		{
			nextserver = strchr(level, '+');
			if (nextserver)
			{
				*nextserver = '\0';
				nextserver++;
			}
		}
	}

	if (startspot)
	{
		strcpy(spot, startspot);
		startspot = spot;
	}
	else if ((startspot = strchr(level, '$')))
	{
		strcpy(spot, startspot+1);
		*startspot = '\0';
		startspot = spot;
	}
	else
		startspot = NULL;

	if (!strcmp(level, "."))	//restart current
	{
		//grab the current map name
		Q_strncpyz(level, svs.name, sizeof(level));
		isrestart = true;
		flushparms = false;
#ifdef SAVEDGAMES
		newunit = false;
		q2savetos0 = false;
#endif

		if (!*level)
		{
			sv.mapchangelocked = true;
			if (Cmd_AliasExist("startmap_dm", RESTRICT_LOCAL))
			{
				Cbuf_AddText("startmap_dm", Cmd_ExecLevel);
				return;
			}
			Q_strncpyz(level, "start", sizeof(level));
		}

		if (startspot && !strcmp(startspot, "."))
		{
			preserveplayers = true;
			startspot = NULL;
		}
		if (!startspot)
		{
			//revert the startspot if its not overridden
			Q_strncpyz(spot, InfoBuf_ValueForKey(&svs.info, "*startspot"), sizeof(spot));
			startspot = spot;
		}
	}

#ifdef SAVEDGAMES
	if (isrestart && *sv.loadgame_on_restart && SV_Loadgame(sv.loadgame_on_restart))
	{	//we managed to reload a saved game instead!
		//this is required in order to keep hub state consistent (dying mid-map would require saved games to store both current and start of map(not to be confused with initial state, which would be trivial))
		return;
	}
#endif

	// check to make sure the level exists
	if (*level == '*')
	{
		memmove(level, level+1, strlen(level));
#ifdef SAVEDGAMES
		newunit=true;
#endif
	}
#ifndef SERVERONLY
	SCR_ImageName(level);
	SCR_SetLoadingStage(LS_SERVER);
	SCR_SetLoadingFile("finalize server");
#else
	#define SCR_SetLoadingFile(s)
#endif

	/*
	ftesurf (P175): mount this map's extra asset pack before anything of the map
	is located.

	Here rather than in the menu because this is the ONE funnel every map load
	goes through -- the browser, a typed `map`, `changelevel`, `retry` and a
	savegame all arrive at this function, and a mount wired into the menu would
	have covered only the first.  After `level` is final (the `*` prefix is
	stripped above) and immediately BEFORE the cache flush, which is what makes
	the newly-added searchpath visible to the existence check below it.
	*/
	FS_AutoMountForMap(level);

	COM_FlushFSCache(false, true);

#ifdef Q2SERVER
	if (strlen(level) > 4 &&
		(!strcmp(level + strlen(level)-4, ".cin") ||
		!strcmp(level + strlen(level)-4, ".roq") ||
		!strcmp(level + strlen(level)-4, ".ogv") ||
		!strcmp(level + strlen(level)-4, ".pcx") ||
		!strcmp(level + strlen(level)-4, ".avi")))
	{
		cinematic = true;
	}
	else
#endif
#ifdef TERRAIN
	//'map doesntexist.map' should just auto-generate that map or something
	if (!Q_strcasecmp("map", COM_FileExtension(level, expanded, sizeof(expanded))))
		;
	else
#endif
	{
		char *exts[] = {"%s", "maps/%s", "maps/%s.bsp", "maps/%s.bsp.gz", "maps/%s.bsp.xz", "maps/%s.d3dbsp", "maps/mp/%s.bsp", "maps/mp/%s.d3dbsp", "maps/%s.cm", "maps/%s.hmp", /*"maps/%s.map",*/ /*"maps/%s.ent",*/ NULL};
		int i, j;

		for (i = 0; exts[i]; i++)
		{
			snprintf (expanded, sizeof(expanded), exts[i], level);
			if (COM_FCheckExists (expanded))
				break;
		}
		if (!exts[i])
		{	//try again.
			char *mod = strchr(level, '#');
			if (mod)
			{
				*mod = 0;
				for (i = 0; exts[i]; i++)
				{
					snprintf (expanded, sizeof(expanded), exts[i], level);
					if (COM_FCheckExists (expanded))
						break;
				}
				*mod = '#';
			}
		}
		if (!exts[i])
		{
			for (i = 0; exts[i]; i++)
			{
				//doesn't exist, so try lowercase. Q3 does this. really our fs_cache stuff should be handling this, but its possible its disabled.
				for (j = 0; j < sizeof(level) && level[j]; j++)
				{
					if (level[j] >= 'A' && level[j] <= 'Z')
						level[j] = level[j] - 'A' + 'a';
				}
				snprintf (expanded, sizeof(expanded), exts[i], level);
				if (COM_FCheckExists (expanded))
					break;
			}
			if (!exts[i])
			{
#ifdef HAVE_CLIENT
				SCR_SetLoadingStage(LS_NONE);
#endif
#ifdef WEBCLIENT
				if (*cl_download_mapsrc.string &&
						!strcmp(cmd, "map") && !startspot &&
						Cmd_ExecLevel==RESTRICT_LOCAL && !strchr(level, '.'))
				{
					if (SV_Map_DownloadPrompt(level))
						return;
				}
#endif

				// FTE is still a Quake engine so report BSP missing
				Con_TPrintf ("Can't find %s\n", COM_QuotedString(va("maps/%s.bsp", level), expanded, sizeof(expanded), false));

				if (SSV_IsSubServer() && !sv.state)	//subservers don't leave defunct servers with no maps lying around.
					Cbuf_AddText("\nquit\n", RESTRICT_LOCAL);
				return;
			}
		}
	}

#ifdef SUBSERVERS
	if (!isDedicated && sv_autooffload.ival && !sv.state && !SSV_IsSubServer() && (
			isrestart
			|| (!strcmp(Cmd_Argv(0), "map") && Cmd_Argc()==2)
		))
	{
		MSV_MapCluster_Setup(level, false, true);
		return;
	}
#endif

#ifdef MVD_RECORDING
	if (sv.mvdrecording)
		SV_MVDStop_f();
#endif

#ifndef SERVERONLY
	if (!isDedicated)	//otherwise, info used on map loading isn't present
	{
		cl.haveserverinfo = true;
		InfoBuf_Clone(&cl.serverinfo, &svs.info);
		CL_CheckServerInfo();
	}

	if (!sv.state && cls.state)
		CL_Disconnect(NULL);
#endif

	if (!isrestart)
		SV_SaveSpawnparms ();

#ifdef SAVEDGAMES
	if (newunit)
		SV_FlushLevelCache();	//forget all on new unit
	else if (startspot && !isrestart && !newunit)
	{
#ifdef Q2SERVER
		if (ge)
		{
			qboolean savedinuse[MAX_CLIENTS];
			for (i=0 ; i<sv.allocated_client_slots; i++)
			{
				savedinuse[i] = svs.clients[i].q2edict->inuse;
				svs.clients[i].q2edict->inuse = false;
			}
			SV_SaveLevelCache(NULL, false);
			for (i=0 ; i<sv.allocated_client_slots; i++)
			{
				svs.clients[i].q2edict->inuse = savedinuse[i];
			}
		}
		else
#endif
			SV_SaveLevelCache(NULL, false);
	}
#endif

	if (forceCheats) {
		Cvar_ForceSet(&sv_cheats, "1");
	}

#ifdef Q3SERVER
	{
		cvar_t *var, *gametype;

		Cvar_ApplyLatches(CVAR_MAPLATCH, false);

		host_mapname.flags |= CVAR_SERVERINFO;

		var = Cvar_Get("nextmap", "", 0, "Q3 compatibility");
		Cvar_ForceSet(var, "map_restart 0");	//on every map change matches q3.

		gametype = Cvar_Get("g_gametype", "", CVAR_MAPLATCH|CVAR_SERVERINFO, "Q3 compatability");
//		gametype->callback = gtcallback;

		/* map_restart doesn't need to handle gametype changes - eukara */
		if (!isrestart)
		{
			if (q3singleplayer)
			{
				Cvar_ForceSet(gametype, "2");//singleplayer
				Cvar_ForceSet(&deathmatch, "0");//for non-q3 type stuff to not get confused..
			}
			else if (gametype->value == 2)
				Cvar_ForceSet(gametype, "");//force to ffa deathmatch
		}
	}
#endif

	Cvar_ForceSet(&host_mapname, level);

#ifdef HAVE_CLIENT
	Menu_PopAll();
#endif

	if (preserveplayers && svprogfuncs)
	{
		for (i=0 ; i<svs.allocated_client_slots ; i++)	//we need to drop all q2 clients. We don't mix q1w with q2.
		{
			char buffer[8192], *buf;
			size_t bufsize = 0;
			if (svs.clients[i].state>cs_connected)
			{
				buf = svprogfuncs->saveent(svprogfuncs, buffer, &bufsize, sizeof(buffer), svs.clients[i].edict);
				if (svs.clients[i].spawninfo)
					Z_Free(svs.clients[i].spawninfo);
				svs.clients[i].spawninfo = Z_Malloc(bufsize+1);
				memcpy(svs.clients[i].spawninfo, buf, bufsize+1);
				svs.clients[i].spawninfotime = sv.time;
			}
		}
	}
	else
	{
		for (i=0 ; i<svs.allocated_client_slots ; i++)	//we need to drop all q2 clients. We don't mix q1w with q2.
		{
			if (svs.clients[i].state>cs_connected)	//so that we don't send a datagram
				svs.clients[i].state=cs_connected;
		}
	}

#ifndef SERVERONLY
	S_StopAllSounds (true);
//	SCR_BeginLoadingPlaque();
	SCR_ImageName(level);
#endif

//	if (!preserveplayers)
	{
		for (i=0, host_client = svs.clients ; i<svs.allocated_client_slots ; i++, host_client++)
		{
			/*pass the new map's name as an extension, so appropriate loading screens can be shown*/
			if (host_client->controller == NULL)
			{
				if (ISNQCLIENT(host_client))
				{
					if (ISDPCLIENT(host_client))
					{
						//DP clients cannot cope with being told the next map's name
						SV_StuffcmdToClient(host_client, "reconnect\n");
					}
					else
						SV_StuffcmdToClient(host_client, va("reconnect \"%s\"\n", level));
				}
				else
					SV_StuffcmdToClient(host_client, va("changing \"%s\"\n", level));
			}
			host_client->prespawn_stage = PRESPAWN_INVALID;
			host_client->prespawn_idx = 0;
		}

#ifdef NQPROT
		if (dpcompat_nopreparse.ival)
		{	//wipe broadcasts here...
			sv.reliable_datagram.cursize = 0;
			sv.datagram.cursize = 0;
			sv.nqreliable_datagram.cursize = 0;
			sv.nqdatagram.cursize = 0;
		}
#endif

		SV_SendMessagesToAll ();

		if (flushparms)
			svs.serverflags = 0;
	}

	SCR_SetLoadingFile("spawnserver");
#ifdef SAVEDGAMES
	if (newunit || !startspot || cinematic || !SV_LoadLevelCache(NULL, level, startspot, false))
#endif
	{
		if (waschangelevel && !startspot)
			startspot = "";
		SV_SpawnServer (level, startspot, mapeditor, cinematic, 0);
	}
	SCR_SetLoadingFile("server spawned");

	//SV_BroadcastCommand ("cmd new\n");
	for (i=0, host_client = svs.clients ; i<svs.allocated_client_slots ; i++, host_client++)
	{	//this expanded code cuts out a packet when changing maps...
		//but more usefully, it stops dp(and probably nq too) from timing out.
		//make sure its all reset.
		host_client->sentents.num_entities = 0;
		host_client->ratetime = 0;
		if (host_client->pendingdeltabits)
			host_client->pendingdeltabits[0] = UF_SV_REMOVE;

		if (flushparms)
		{
			if (host_client->spawninfo)
				Z_Free(host_client->spawninfo);
			host_client->spawninfo = NULL;
			memset(host_client->spawn_parms, 0, sizeof(host_client->spawn_parms));
			if (host_client->state > cs_zombie)
				SV_GetNewSpawnParms(host_client);
		}

		if (preserveplayers && svprogfuncs && host_client->state == cs_spawned && host_client->spawninfo)
		{
			size_t j = 0;
			svprogfuncs->restoreent(svprogfuncs, host_client->spawninfo, &j, host_client->edict);
			host_client->istobeloaded = true;
			host_client->state=cs_connected;
			if (host_client->spectator)
				sv.spawned_observer_slots++;
			else
				sv.spawned_client_slots++;
		}

		if (host_client->controller)
			continue;
		if (host_client->state>=cs_connected)
		{
			if (host_client->protocol == SCP_QUAKE3)
				continue;
			if (host_client->protocol == SCP_BAD)
				continue;

#ifdef NQPROT
			if (ISNQCLIENT(host_client))
			{
				SVNQ_New_f();
				host_client->send_message = true;
			}
			else
#endif
				SV_New_f();
		}
	}

	if (!isrestart)
	{
		cvar_t *nsv;
		nsv = Cvar_Get("nextserver", "", 0, "");
		if (nextserver)
			Cvar_Set(nsv, va("gamemap \"%s\"", nextserver));
		else
			Cvar_Set(nsv, "");
	}

#ifdef SAVEDGAMES
	if (q2savetos0)
	{
		if (sv.state != ss_cinematic)	//too weird.
			SV_Savegame("s0", true);
	}
#endif

	if (isDedicated)
		Mod_Purge(MP_MAPCHANGED);
}

static void SV_KillServer_f(void)
{
	SV_UnspawnServer();
}


/*
==================
SV_Kick_f

Kick a user off of the server
==================
*/
static void SV_Kick_f (void)
{
	client_t	*cl;
	int clnum=-1;

	if (!sv.state)
		return;

	if (!strcmp(Cmd_Argv(1), "#"))
	{
		clnum = atoi(Cmd_Argv(2)) - 1;
		if (clnum >= 0 && clnum < sv.allocated_client_slots)
		{
			cl = &svs.clients[clnum];
			if (cl->state >= cs_connected)
			{
				SV_BroadcastTPrintf (PRINT_HIGH, "%s was kicked\n", cl->name);
				// print directly, because the dropped client won't get the
				// SV_BroadcastPrintf message
				SV_ClientTPrintf (cl, PRINT_HIGH, "You were kicked\n");

				SV_LogPlayer(cl, "kicked");
				SV_DropClient (cl);
			}
		}
		return;
	}

	while((cl = SV_GetClientForString(Cmd_Argv(1), &clnum)))
	{
		SV_BroadcastTPrintf (PRINT_HIGH, "%s was kicked\n", cl->name);
		// print directly, because the dropped client won't get the
		// SV_BroadcastPrintf message
		SV_ClientTPrintf (cl, PRINT_HIGH, "You were kicked\n");

		SV_LogPlayer(cl, "kicked");
		SV_DropClient (cl);
	}

	if (clnum == -1)
		Con_TPrintf ("Couldn't find user number %s\n", Cmd_Argv(1));
}

/*for q3's kick bot menu*/
static void SV_KickSlot_f (void)
{
	client_t	*cl;
	int clnum=atoi(Cmd_Argv(1));

	if (!sv.state)
		return;

	if (clnum < sv.allocated_client_slots && svs.clients[clnum].state)
	{
		cl = &svs.clients[clnum];

		SV_BroadcastTPrintf (PRINT_HIGH, "%s was kicked\n", cl->name);
		// print directly, because the dropped client won't get the
		// SV_BroadcastPrintf message
		SV_ClientTPrintf (cl, PRINT_HIGH, "You were kicked\n");

		SV_LogPlayer(cl, "kicked");
		SV_DropClient (cl);
	}
	else
		Con_Printf("Client %i is not active\n", clnum);
}

//ipv4ify if its an ipv6 ipv4-mapped address.
static netadr_t *NET_IPV4ify(netadr_t *a, netadr_t *tmp)
{
	if (a->type == NA_IPV6 &&
		!*(int*)&a->address.ip6[0] &&
		!*(int*)&a->address.ip6[4] &&
		!*(short*)&a->address.ip6[8] &&
		*(short*)&a->address.ip6[10]==(short)0xffff)
	{
		tmp->type = NA_IP;
		tmp->connum = a->connum;
		tmp->scopeid = a->scopeid;
		tmp->port = a->port;
		tmp->prot = a->prot;
		tmp->address.ip[0] = a->address.ip6[12];
		tmp->address.ip[1] = a->address.ip6[13];
		tmp->address.ip[2] = a->address.ip6[14];
		tmp->address.ip[3] = a->address.ip6[15];
		a = tmp;
	}
	return a;
}

//will kick clients if they got banned (without being safe)
void SV_EvaluatePenalties(client_t *cl)
{
	bannedips_t *banip;
	unsigned int penalties = 0, delta, p;
	char *penaltyreason[countof(banflags)] = {NULL};
	const char *activepenalties[countof(banflags)];
	char *reasons[countof(banflags)] = {NULL};
	int numpenalties = 0;
	int numreasons = 0;
	int i;
	netadr_t tmp, *a;

	if (cl->realip.type != NA_INVALID)
	{
		a = NET_IPV4ify(&cl->realip, &tmp);
		for (banip = svs.bannedips; banip; banip=banip->next)
		{
			if (NET_CompareAdrMasked(a, &banip->adr, &banip->adrmask))
			{
				for (i = 0; i < sizeof(penaltyreason)/sizeof(penaltyreason[0]); i++)
				{
					p = 1u<<i;
					if (banip->banflags & p)
					{
						if (!penaltyreason[i])
							penaltyreason[i] = banip->reason;
						penalties |= p;
					}
				}
			}
		}
	}
	a = NET_IPV4ify(&cl->netchan.remote_address, &tmp);
	for (banip = svs.bannedips; banip; banip=banip->next)
	{
		if (NET_CompareAdrMasked(a, &banip->adr, &banip->adrmask))
		{
			for (i = 0; i < sizeof(penaltyreason)/sizeof(penaltyreason[0]); i++)
			{
				p = 1u<<i;
				if (banip->banflags & p)
				{
					if (!penaltyreason[i])
						penaltyreason[i] = banip->reason;
					penalties |= p;
				}
			}
		}
	}

	delta = cl->penalties ^ penalties;
	cl->penalties = penalties;

	if ((penalties & (BAN_BAN | BAN_PERMIT)) == BAN_BAN)
	{
		//we should only reach here by a player getting banned mid-game.
		if (penaltyreason[BAN_BAN])
			SV_BroadcastPrintf(PRINT_HIGH, "%s was banned: %s\n", cl->name, penaltyreason[BAN_BAN]);
		else
			SV_BroadcastPrintf(PRINT_HIGH, "%s was banned\n", cl->name);
		cl->drop = true;
	}

	//don't announce these now.
	delta &= ~(BAN_BAN | BAN_PERMIT);

	//deaf+mute sees no (other) penalty messages
	if (((penalties|delta) & (BAN_MUTE|BAN_DEAF)) == (BAN_MUTE|BAN_DEAF))
		delta &= ~(BAN_MUTE|BAN_DEAF);

	if ((delta|penalties) & BAN_STEALTH)
		delta = 0;	//don't announce ANY.

	if (cl->controller)
		delta = 0;	//don't spam it for every player in a splitscreen client.

	if (delta & BAN_VIP)
	{
		delta &= ~BAN_VIP;	//don't refer to this as a penalty
		if (penalties & BAN_VIP)
			SV_PrintToClient(cl, PRINT_HIGH, "You are a VIP, apparently\n");
		else
			SV_PrintToClient(cl, PRINT_HIGH, "VIP expired\n");
	}

	for (i = 0; i < countof(banflags); i++)
	{
		p = banflags[i].banflag;
		if (delta & p)
		{
			if (penalties & p)
			{
				if (banflags[i].names[0])
					activepenalties[numpenalties++] = banflags[i].names[0];
				if (reasons[i] && *reasons[i])
					reasons[numreasons++] = reasons[i];
			}
			else
				SV_PrintToClient(cl, PRINT_HIGH, va("Penalty expired: %s\n", banflags[i].names[0]));
		}
	}

	if (numpenalties)
	{
		char penaltystring[1024];
		int i, j;
		Q_strncpyz(penaltystring, "You are penalised: ", sizeof(penaltystring));
		for (i = 0; i < numpenalties; i++)
		{
			if (i && i == numpenalties-1)
				Q_strncatz(penaltystring, " and ", sizeof(penaltystring));
			else if (i)
				Q_strncatz(penaltystring, ", ", sizeof(penaltystring));
			Q_strncatz(penaltystring, activepenalties[i], sizeof(penaltystring));
		}
		Q_strncatz(penaltystring, "\n", sizeof(penaltystring));
		SV_PrintToClient(cl, PRINT_HIGH, penaltystring);
		for (i = 0; i < numreasons; i++)
		{
			if (*reasons[i])
			{
				for(j = 0; j < i; j++)
					if (!strcmp(reasons[i], reasons[j]))
						break;
				if (i == j)
					SV_PrintToClient(cl, PRINT_HIGH, va("  %s\n", reasons[i]));
			}
		}
	}

	if (delta & BAN_VIP)
		InfoBuf_SetStarKey(&cl->userinfo, "*VIP", (cl->penalties & BAN_VIP)?"1":"");
	if (delta & BAN_MAPPER)
		InfoBuf_SetStarKey(&cl->userinfo, "*mapper", (cl->penalties & BAN_MAPPER)?"1":"");
}

static time_t reevaluatebantime;
static qboolean reevaluatebans;
//could use time(NULL) instead, but this avoids a system call.
static time_t SV_BanTime(void)
{
	static double bantimemark;
	static time_t banstarttime;
	if (!banstarttime)
	{
		banstarttime = time(NULL);
		bantimemark = realtime;
	}
	return banstarttime + (realtime - bantimemark);
}
//removes anything with an expiry time in the past.
//avoids walking the list if there's nothing changed.
//can be used to force penalty reevaluation.
void SV_KillExpiredBans(void)
{
	bannedips_t **link, *banip;
	time_t curtime = SV_BanTime();
	int i;
	if (reevaluatebantime && curtime > reevaluatebantime)
	{
		reevaluatebantime = 0;
		for(link = &svs.bannedips; (banip = *link) != NULL; )
		{
			if (banip->expiretime)
			{
				if (banip->expiretime < curtime)
				{
					reevaluatebans = true;
					*link = banip->next;
					Z_Free(banip);
					continue;
				}
				if (!reevaluatebantime || reevaluatebantime > banip->expiretime)
					reevaluatebantime = banip->expiretime+1;
			}
			link = &banip->next;
		}
	}

	if (reevaluatebans)
	{
		reevaluatebans = false;
		for (i = 0; i < svs.allocated_client_slots; i++)
		{
			if (svs.clients[i].state<=cs_loadzombie)
				continue;

			SV_EvaluatePenalties(&svs.clients[i]);
		}
	}
}

//adds a new ban/penalty.
//will remove old penalties if the new one has a longer duration, otherwise will ignore the add.
static qboolean SV_AddBanEntry(bannedips_t *proto, char *reason)
{
	bannedips_t *nb, **link;
	nb = svs.bannedips;
	while (nb)
	{
		if (NET_CompareAdr(&nb->adr, &proto->adr) && NET_CompareAdr(&nb->adrmask, &proto->adrmask))
		{
			//found a match, figure out which lasts longer
			//the shorter ban duration gets its effective banflags stripped.
			if ((proto->expiretime && proto->expiretime < nb->expiretime) || !nb->expiretime)
				proto->banflags &= ~nb->banflags;
			else
				nb->banflags &= ~proto->banflags;

			if (!proto->banflags)
			{
				//we should not have been able to strip a previous nb->banflags if this ban was duped later.
				return false;
			}
			if (!nb->banflags)
				reevaluatebantime = nb->expiretime = 1;	//make sure it expires 'soon'.
		}
		nb = nb->next;
	}

	link = &svs.bannedips;

	// add IP and mask to filter list
	nb = Z_Malloc(sizeof(bannedips_t) + strlen(reason));
	nb->adr = proto->adr;
	nb->adrmask = proto->adrmask;
	nb->banflags = proto->banflags;
	nb->expiretime = proto->expiretime;
	Q_strcpy(nb->reason, reason);

	nb->next = *link;
	*link = nb;

	reevaluatebans = true;	//make sure the new ban/penalty applies to the right IPs.
	if (nb->expiretime && (!reevaluatebantime || reevaluatebantime > nb->expiretime))
		reevaluatebantime = nb->expiretime;
	return true;
}

//slightly different logic.
//if duration is specified, just does an add instead.
//otherwise ignores durations.
//only really works with a single toggle. if any are found, will not add.
//returns 1 if added, 0 if removed, and -1 if tried to add and it already existed.
static int SV_ToggleBan(bannedips_t *proto, char *reason)
{
	qboolean found = false;
	bannedips_t *nb;
	if (proto->expiretime)
		return SV_AddBanEntry(proto, reason)?true:-1;

	nb = svs.bannedips;
	while (nb)
	{
		if (NET_CompareAdr(&nb->adr, &proto->adr) && NET_CompareAdr(&nb->adrmask, &proto->adrmask))
		{
			if (nb->banflags & proto->banflags)
			{
				found = true;
				nb->banflags &= ~proto->banflags;
				reevaluatebans = true;
				if (!nb->banflags)
					reevaluatebantime = nb->expiretime = 1;	//make sure it expires 'soon' (in the past).
			}
		}
		nb = nb->next;
	}

	if (found)
		return 0;
	return SV_AddBanEntry(proto, reason)?true:-1;
}

extern cvar_t filterban;
//returns a reason if the client is banned. ignores other penalties.
char *SV_BannedReason (netadr_t *a)
{
	char *reason = filterban.value?NULL:"";	//"" = banned with no explicit reason
	bannedips_t *banip;
	netadr_t tmp;

	if (NET_IsLoopBackAddress(a))
		return NULL; // never filter loopback

	a = NET_IPV4ify(a, &tmp);

	for (banip = svs.bannedips; banip; banip=banip->next)
	{
		if (NET_CompareAdrMasked(a, &banip->adr, &banip->adrmask))
		{
			if (banip->banflags & BAN_BAN)
				return banip->reason;	//banned, with reason.
			if (banip->banflags & BAN_PERMIT)
				return NULL;	//allowed
		}
	}
	return reason;
}

static void SV_FilterIP_f (void)
{
	bannedips_t proto;
	extern cvar_t filterban;
	char *s;
	int i;

	if (Cmd_Argc() < 2)
	{
		Con_Printf("%s <address/mask|adress/maskbits> [flags] [+time] [reason]\n", Cmd_Argv(0));
		Con_Printf("allowed flags: %s", banflags[0].names[0]);
		for (i = 1; i < countof(banflags); i++)
			Con_Printf(",%s", banflags[i].names[0]);
		Con_Printf(". time is in seconds (omitting the plus will be taken to mean unix time).\n");
		return;
	}

	if (!NET_StringToAdrMasked(Cmd_Argv(1), true, &proto.adr, &proto.adrmask))
	{
		Con_Printf("invalid address or mask\n");
		return;
	}

	s = Cmd_Argv(2);
	proto.banflags = 0;
	while(*s)
	{
		s=COM_ParseToken(s,",");
		if (!Q_strcasecmp(com_token, ","))
			i = -1;
		else for (i = 0; i < countof(banflags); i++)
		{
			if (!Q_strcasecmp(com_token, banflags[i].names[0]) || (banflags[i].names[1] && !Q_strcasecmp(com_token, banflags[i].names[1])))
			{
				proto.banflags |= banflags[i].banflag;
				break;
			}
		}
		if (i == countof(banflags))
			Con_Printf("Unknown ban/penalty flag: %s. ignoring.\n", com_token);
	}
	//if no flags were specified,
	if (!proto.banflags)
	{
		if (!strcmp(Cmd_Argv(0), "ban"))
			proto.banflags = BAN_BAN;
		else
			proto.banflags = filterban.ival?BAN_BAN:BAN_PERMIT;
	}

	if (NET_IsLoopBackAddress(&proto.adr) && (proto.banflags & BAN_NOLOCALHOST))
	{	//do allow them to be muted etc, just not banned outright.
		Con_Printf("You're not allowed to filter loopback!\n");
		return;
	}

	s = Cmd_Argv(3);
	if (*s == '+')
	{
		time_t secs = strtoull(s+1, &s, 0);
		if (*s == ':')
		{
			secs*=60;
			secs+=strtoull(s+1, &s, 0);
		}
		proto.expiretime = SV_BanTime() + secs;
	}
	else
		proto.expiretime = strtoull(s, NULL, 0);

	//and then add it
	if (!SV_AddBanEntry(&proto, Cmd_Argv(4)))
		Con_Printf("addip: entry already exists\n");
}

static void SV_BanList_f (void)
{
	int bancount = 0;
	bannedips_t *nb;
	char adr[MAX_ADR_SIZE];
	char middlebit[256];
	time_t bantime = SV_BanTime();

	SV_KillExpiredBans();

	for (nb = svs.bannedips; nb; nb = nb->next)
	{
		if (nb->banflags & BAN_BAN)
		{
			*middlebit = 0;
			if (nb->expiretime)
				Q_strncatz(middlebit, va(",\t+%"PRIu64, (quint64_t)(nb->expiretime - bantime)), sizeof(middlebit));
			if (nb->reason[0])
				Q_strncatz(middlebit, ",\t", sizeof(middlebit));
			Con_Printf("%s%s%s\n", NET_AdrToStringMasked(adr, sizeof(adr), &nb->adr, &nb->adrmask), middlebit, nb->reason);
			bancount++;
		}
	}

	Con_Printf("%i total entries in ban list\n", bancount);
}

static void SV_FilterList_f (void)
{
	int filtercount = 0;
	bannedips_t *nb;
	char adr[MAX_ADR_SIZE];
	char banflagtext[1024];
	int i;
	time_t curtime = SV_BanTime();

	SV_KillExpiredBans();

	for (nb = svs.bannedips; nb; )
	{
		*banflagtext = 0;
		for (i = 0; i < countof(banflags); i++)
		{
			if (nb->banflags & banflags[i].banflag)
			{
				if (*banflagtext)
					Q_strncatz(banflagtext, ",", sizeof(banflagtext));
				Q_strncatz(banflagtext, banflags[i].names[0], sizeof(banflagtext));
			}
		}

		if (nb->expiretime)
		{
			time_t secs = nb->expiretime - curtime;
			Con_Printf("%s %s +%"PRIu64":%02u\n", NET_AdrToStringMasked(adr, sizeof(adr), &nb->adr, &nb->adrmask), banflagtext, (quint64_t)(secs/60), (unsigned int)(secs%60));
		}
		else
			Con_Printf("%s %s\n", NET_AdrToStringMasked(adr, sizeof(adr), &nb->adr, &nb->adrmask), banflagtext);
		filtercount++;
		nb = nb->next;
	}

	Con_Printf("%i total entries in filter list\n", filtercount);
}

static void SV_Unfilter_f (void)
{
	qboolean found = false;
	qboolean all = false;
	bannedips_t **link;
	bannedips_t *nb;
	netadr_t unbanadr = {0};
	netadr_t unbanmask = {0};
	char adr[MAX_ADR_SIZE];
	unsigned int clearbanflags, nf;
	char *s;
	int i;

	SV_KillExpiredBans();

	if (Cmd_Argc() < 2)
	{
		Con_Printf("%s address/mask|address/maskbits|all [flags]\n", Cmd_Argv(0));
		return;
	}

	if (!Q_strcasecmp(Cmd_Argv(1), "all"))
	{
		Con_Printf("removing all filtered addresses\n");
		all = true;
	}
	else if (!NET_StringToAdrMasked(Cmd_Argv(1), true, &unbanadr, &unbanmask))
	{
		Con_Printf("invalid address or mask\n");
		return;
	}

	s = Cmd_Argv(2);
	clearbanflags = 0;
	while(*s)
	{
		s=COM_ParseToken(s,",");
		if (!Q_strcasecmp(com_token, ","))
			i = -1;
		else for (i = 0; i < countof(banflags); i++)
		{
			if (!Q_strcasecmp(com_token, banflags[i].names[0]) || (banflags[i].names[1] && !Q_strcasecmp(com_token, banflags[i].names[1])))
			{
				clearbanflags |= banflags[i].banflag;
				break;
			}
		}
		if (i == countof(banflags))
			Con_Printf("Unknown ban/penalty flag: %s. ignoring.\n", com_token);
	}
	//if no flags were specified, assume all
	if (!clearbanflags)
		clearbanflags = ~0u;

	for (link = &svs.bannedips ; (nb = *link) ; )
	{
		if ((nb->banflags & clearbanflags) && (all || (NET_CompareAdr(&nb->adr, &unbanadr) && NET_CompareAdr(&nb->adrmask, &unbanmask))))
		{
			found = true;
			if (!all)
				Con_Printf("unfiltered %s\n", NET_AdrToStringMasked(adr, sizeof(adr), &nb->adr, &nb->adrmask));

			nf = nb->banflags & clearbanflags;
			nb->banflags -= nf;
			if (!nb->banflags)
			{
				//this entry no longer has any flags
				*link = nb->next;
				Z_Free(nb);
			}
			else
				link = &(*link)->next;
		}
		else
		{
			link = &(*link)->next;
		}
	}

	if (!all && !found)
		Con_Printf("address was not filtered\n");

	if (found)
	{
		reevaluatebans = true;
		SV_KillExpiredBans();
	}
}
static void SV_PenaltyToggle (unsigned int banflag, char *penaltyname)
{
	char *clname = Cmd_Argv(1);
	char *duration = Cmd_Argv(2);
	char *reason = Cmd_Argv(3);
	bannedips_t proto = {0};
	client_t *cl;
	qboolean found = false;
	int clnum=-1;
	netadr_t tmp;

	proto.banflags = banflag;

	if (*duration == '+')
		proto.expiretime = SV_BanTime() + strtoull(duration+1, &duration, 0);
	else
		proto.expiretime = strtoull(duration, &duration, 0);

	//both of these should work
	//cuff foo "cos they're morons"
	//cuff foo +10 "cos they're morons"
	if (!*reason && *duration)
		reason = duration;

	memset(&proto.adrmask.address, 0xff, sizeof(proto.adrmask.address));
	while((cl = SV_GetClientForString(clname, &clnum)))
	{
		found = true;
		proto.adr = *NET_IPV4ify(&cl->netchan.remote_address, &tmp);
		proto.adr.port = 0;
		proto.adrmask.type = proto.adr.type;

		if (NET_IsLoopBackAddress(&proto.adr) && (proto.banflags & BAN_NOLOCALHOST))
		{
			Con_Printf("You're not allowed to filter loopback!\n");
			continue;
		}

		switch(SV_ToggleBan(&proto, reason))
		{
		case 1:
			Con_Printf("%s: %s is now %s\n", Cmd_Argv(0), cl->name, penaltyname);
			break;
		case 0:
			Con_Printf("%s: %s is no longer %s\n", Cmd_Argv(0), cl->name, penaltyname);
			break;
		default:
		case -1:
			Con_Printf("%s: %s already %s\n", Cmd_Argv(0), cl->name, penaltyname);
			break;
		}
	}
	if (!found)
		Con_Printf("%s: no clients\n", Cmd_Argv(0));
}

void SV_AutoAddPenalty (client_t *cl, unsigned int banflag, int duration, char *reason)
{
	bannedips_t proto;

	proto.banflags = banflag;
	proto.expiretime = SV_BanTime() + duration;
	memset(&proto.adrmask.address, 0xff, sizeof(proto.adrmask.address));
	proto.adr = cl->netchan.remote_address;
	proto.adr.port = 0;
	proto.adrmask.type = proto.adr.type;

	SV_AddBanEntry(&proto, reason);

	for (cl = (cl->controller?cl->controller:cl); cl; cl = cl->controlled)
		SV_EvaluatePenalties(cl);
}
void SV_AutoBanSender (int duration, char *reason)
{
	bannedips_t proto;

	proto.banflags = BAN_BAN;
	proto.expiretime = SV_BanTime() + duration;
	memset(&proto.adrmask.address, 0xff, sizeof(proto.adrmask.address));
	proto.adr = net_from;
	proto.adr.port = 0;
	proto.adrmask.type = proto.adr.type;

	SV_AddBanEntry(&proto, reason);
}

static void SV_WriteIP_f (void)
{
	vfsfile_t	*f;
	char	name[MAX_OSPATH];
	bannedips_t *bi;
	char *s;
	char adr[MAX_ADR_SIZE];
	char banflagtext[1024];
	int i;

	SV_KillExpiredBans();

	strcpy (name, "listip.cfg");

	Con_Printf ("Writing %s.\n", name);

	f = FS_OpenVFS(name, "wb", FS_GAMEONLY);
	if (!f)
	{
		Con_Printf ("Couldn't open %s\n", name);
		return;
	}

	bi = svs.bannedips;
	while (bi)
	{
		*banflagtext = 0;
		for (i = 0; i < countof(banflags); i++)
		{
			if (bi->banflags & banflags[i].banflag)
			{
				if (*banflagtext)
					Q_strncatz(banflagtext, ",", sizeof(banflagtext));
				Q_strncatz(banflagtext, banflags[i].names[0], sizeof(banflagtext));
			}
		}
		if (bi->reason[0])
			s = va("addip %s %s %"PRIu64" \"%s\"\n", NET_AdrToStringMasked(adr, sizeof(adr), &bi->adr, &bi->adrmask), banflagtext, (quint64_t) bi->expiretime, bi->reason);
		else if (bi->expiretime)
			s = va("addip %s %s %"PRIu64"\n", NET_AdrToStringMasked(adr, sizeof(adr), &bi->adr, &bi->adrmask), banflagtext, (quint64_t) bi->expiretime);
		else
			s = va("addip %s %s\n", NET_AdrToStringMasked(adr, sizeof(adr), &bi->adr, &bi->adrmask), banflagtext);
		VFS_WRITE(f, s, strlen(s));
		bi = bi->next;
	}

	VFS_CLOSE (f);
}


static void SV_ForceName_f (void)
{
	client_t	*cl;
	int clnum=-1;

	while((cl = SV_GetClientForString(Cmd_Argv(1), &clnum)))
	{
		InfoBuf_SetKey(&cl->userinfo, "name", Cmd_Argv(2));
		SV_LogPlayer(cl, "name forced");
		SV_ExtractFromUserinfo(cl, true);
		Q_strncpyz(cl->name, Cmd_Argv(2), sizeof(cl->namebuf));
		SV_BroadcastUserinfoChange(cl, true, "name", cl->name);
		return;
	}

	if (clnum == -1)
		Con_TPrintf ("Couldn't find user number %s\n", Cmd_Argv(1));
}

static void SV_CripplePlayer_f (void)
{
	SV_PenaltyToggle(BAN_CRIPPLED, "crippled");
}

static void SV_Mute_f (void)
{
	SV_PenaltyToggle(BAN_MUTE, "muted");
}
static void SV_StealthMute_f (void)
{
	SV_PenaltyToggle(BAN_MUTE|BAN_STEALTH, "stealth-muted");
}

static void SV_Cuff_f (void)
{
	SV_PenaltyToggle(BAN_CUFF, "cuffed");
}

static void SV_BanClientIP_f (void)
{
	SV_PenaltyToggle(BAN_BAN, "banned");
}

static void SV_Floodprot_f(void)
{
	extern cvar_t sv_floodprotect;
	extern cvar_t sv_floodprotect_messages;
	extern cvar_t sv_floodprotect_interval;
	extern cvar_t sv_floodprotect_silencetime;

	if (Cmd_Argc() == 1)
	{
		if (sv_floodprotect_messages.value <= 0 || !sv_floodprotect.value)
			Con_Printf("Flood protection is off.\n");
		else
			Con_Printf("Current flood protection settings: \nAfter %g msgs for %g seconds, silence for %g seconds\n",
				sv_floodprotect_messages.value,
				sv_floodprotect_interval.value,
				sv_floodprotect_silencetime.value);
		return;
	}

	if (Cmd_Argc() != 4)
	{
		Con_Printf("Usage: %s <messagerate> <ratepersecond> <silencetime>\n", Cmd_Argv(0));
		return;
	}

	Cvar_SetValue(&sv_floodprotect_messages, atof(Cmd_Argv(1)));
	Cvar_SetValue(&sv_floodprotect_interval, atof(Cmd_Argv(2)));
	Cvar_SetValue(&sv_floodprotect_silencetime, atof(Cmd_Argv(3)));
}

static void SV_StuffToClient_f(void)
{	//with this we emulate the progs 'stuffcmds' builtin

	client_t	*cl;

	int clnum=-1;
	char *clientname = Cmd_Argv(1);
	char *str;
	char *c;
	char *key;

	if (Cmd_Argc() < 3)
	{
		Con_Printf("%s <clientname> <consolecommand>\n", Cmd_Argv(0));
		return;
	}

	Cmd_ShiftArgs(1, Cmd_ExecLevel==RESTRICT_LOCAL);
	if (!strcmp(Cmd_Argv(1), "bind"))
	{
		key = Z_Malloc(strlen(Cmd_Argv(2))+1);
		strcpy(key, Cmd_Argv(2));
		Cmd_ShiftArgs(2, Cmd_ExecLevel==RESTRICT_LOCAL);
	}
	else
		key = NULL;
	str = Cmd_Args();

	while(*str <= ' ')	//strim leading spaces
	{
		if (!*str)
			break;
		str++;
	}

	//a list of safe, allowed commands. Allows any extention of this.
	if (strchr(str, '\n') || strchr(str, ';') || (
		!strncmp(str, "setinfo", 7) &&
		!strncmp(str, "quit", 4) &&
		!strncmp(str, "gl_fb", 5) &&
		!strncmp(str, "r_fb", 4) &&
		!strncmp(str, "say", 3) &&	//note that the say parsing could be useful here.
		!strncmp(str, "echo", 4) &&
		!strncmp(str, "name", 4) &&
		!strncmp(str, "skin", 4) &&
		!strncmp(str, "color", 5) &&
		!strncmp(str, "cmd", 3) &&
		!strncmp(str, "fov", 3) &&
		!strncmp(str, "connect", 7) &&
		!strncmp(str, "rate", 4) &&
		!strncmp(str, "cd", 2) &&
		!strncmp(str, "easyrecord", 10) &&
		!strncmp(str, "leftisright", 11) &&
		!strncmp(str, "menu_", 5) &&
		!strncmp(str, "r_fullbright", 12) &&
		!strncmp(str, "toggleconsole", 13) &&
		!strncmp(str, "v_i", 3) &&	//idlescale vars
		!strncmp(str, "bf", 2) &&
		!strncmp(str, "+", 1) &&
		!strncmp(str, "-", 1) &&
		!strncmp(str, "impulse", 7) &&
		1))
	{
		Con_Printf("You're not allowed to stuffcmd that\n");

		if (key)
			Z_Free(key);
		return;
	}

	while((cl = SV_GetClientForString(clientname, &clnum)))
	{
		if (ISQ2CLIENT(cl))
			ClientReliableWrite_Begin (cl, svcq2_stufftext, 3+strlen(str) + (key?strlen(key)+6:0));
		else
			ClientReliableWrite_Begin (cl, svc_stufftext, 3+strlen(str) + (key?strlen(key)+6:0));

		if (key)
		{
			for (c = "bind "; *c; c++)
				ClientReliableWrite_Byte (cl, *c);

			for (c = key; *c; c++)
				ClientReliableWrite_Byte (cl, *c);

			ClientReliableWrite_Byte (cl, ' ');
		}

		for (c = str; *c; c++)
			ClientReliableWrite_Byte (cl, *c);
		ClientReliableWrite_Byte (cl, '\n');
		ClientReliableWrite_Byte (cl, '\0');
	}

	if (key)
		Z_Free(key);
}

static char *ShowTime(unsigned int seconds)
{
	char buf[1024];
	char *b = buf;
	*b = 0;

	if (seconds > 60)
	{
		if (seconds > 60*60)
		{
			if (seconds > 24*60*60)
			{
				strcpy(b, va("%id ", seconds/(24*60*60)));
				b += strlen(b);
				seconds %= 24*60*60;
			}

			strcpy(b, va("%ih ", seconds/(60*60)));
			b += strlen(b);
			seconds %= 60*60;
		}
		strcpy(b, va("%im ", seconds/60));
		b += strlen(b);
		seconds %= 60;
	}
	strcpy(b, va("%is", seconds));
	b += strlen(b);

	return va("%s", buf);
}

/*
================
SV_Status_f
================
*/
const char *SV_ProtocolNameForClient(client_t *cl);
static void SV_Status_f (void)
{
	int			i;
	client_t	*cl;
	float		cpu;
	char		*s, *sec;
	const char	*p;
	char		adr[MAX_ADR_SIZE];
	float pi, po, bi, bo;

	int columns = 80;
	extern cvar_t sv_listen_qw;
#if defined(TCPCONNECT) && !defined(CLIENTONLY)
	#if defined(HAVE_SSL)
		extern cvar_t net_enable_tls;
	#endif
	#ifdef HAVE_HTTPSV
		extern cvar_t net_enable_http, net_enable_rtcbroker, net_enable_websockets;
	#endif
	extern cvar_t net_enable_qizmo;
	#ifdef MVD_RECORDING
		extern cvar_t net_enable_qtv;
	#endif
#endif
#ifdef NQPROT
	extern cvar_t sv_listen_nq, sv_listen_dp;
#endif
#ifdef QWOVERQ3
	extern cvar_t sv_listen_q3;
#endif

#ifndef SERVERONLY
	if (!sv.state && cls.state >= ca_connected && !cls.demoplayback && cls.protocol == CP_NETQUAKE)
	{	//nq can normally forward the request to the server.
		Cmd_ForwardToServer();
		return;
	}
#endif

	if (sv_redirected != RD_OBLIVION && (sv_redirected != RD_NONE
#ifndef SERVERONLY
		|| (vid.width < 68*8 && qrenderer != QR_NONE)
#endif
		))
		columns = 40;

	NET_PrintAddresses(svs.sockets);

	if (!sv.state)
	{
		Con_TPrintf("Server is not running\n");
		return;
	}

	if (Cmd_Argc()>1)
		columns = atoi(Cmd_Argv(1));

	cpu = (svs.stats.latched_active+svs.stats.latched_idle);
	if (cpu)
		cpu = 100*svs.stats.latched_active/cpu;

	Con_TPrintf("cpu utilization  : %3i%%\n",(int)cpu);
	if (sv.state == ss_active)
	{
		Con_TPrintf("avg response time: %i ms (%i max)\n",(int)(1000*svs.stats.latched_active/svs.stats.latched_count), (int)(1000*svs.stats.latched_maxresponse));
		Con_TPrintf("packets/frame    : %5.2f (%i max)\n", (float)svs.stats.latched_packets/svs.stats.latched_count, svs.stats.latched_maxpackets);	//not relevent as a limit.
	}
	if (NET_GetRates(svs.sockets, &pi, &po, &bi, &bo))
		Con_TPrintf("packets,bytes/sec: in: %g %g  out: %g %g\n", pi, bi, po, bo);	//not relevent as a limit.
	Con_TPrintf("server uptime    : %s\n", ShowTime(realtime));
	if (sv_public.ival < 0)
		s = "hidden";
	else if (sv_public.ival == 2)
		s = "hole punching";
	else if (sv_public.ival)
		s = "direct";
	else
		s = "private";
	Con_TPrintf("public           : %s\n", localtext(s));

	switch(svs.gametype)
	{
#ifdef Q3SERVER
	case GT_QUAKE3:
		Con_TPrintf("client types     :%s\n", sv_listen_qw.ival?" Q3":"");
		break;
#endif
#ifdef Q2SERVER
	case GT_QUAKE2:
		Con_TPrintf("client types     :%s\n", sv_listen_qw.ival?" Q2":"");
		break;
#endif

	default:
		Con_TPrintf("client types     :%s", sv_listen_qw.ival?" ^[QW\\tip\\This is "FULLENGINENAME"'s standard protocol.^]":"");
#ifdef NQPROT
		Con_TPrintf("%s%s", (sv_listen_nq.ival==2)?" ^[NQ+\\tip\\Allows 'Net'/'Normal' Quake clients to connect, with cookies and extensions that might confuse some old clients^]":(sv_listen_nq.ival?" ^[NQ(15)\\tip\\Vanilla/Normal Quake protocol with maximum compatibility^]":""), sv_listen_dp.ival?" ^[DP\\tip\\Explicitly recognise connection requests from DP clients, no handshakes.^]":"");
#endif
#ifdef QWOVERQ3
		if (sv_listen_q3.ival) Con_Printf(" Q3");
#endif
#ifdef HAVE_DTLS
		if (svs.sockets && svs.sockets->dtlsfuncs)
		{
			if (net_enable_dtls.ival >= 3)
				Con_TPrintf(" ^[DTLS-only\\tip\\Insecure clients (those without support for DTLS) will be barred from connecting.^]");
			else if (net_enable_dtls.ival)
				Con_TPrintf(" ^[DTLS\\tip\\Clients may optionally connect via DTLS for added security^]");
		}
#endif
		Con_Printf("\n");
#if defined(TCPCONNECT) && !defined(CLIENTONLY)
		Con_TPrintf("tcp services     :");

		if (svs.sockets)
		{
			int i, m;
			netadr_t	addr[64];
			struct ftenet_generic_connection_s			*con[sizeof(addr)/sizeof(addr[0])];
			int			flags[sizeof(addr)/sizeof(addr[0])];
			const char *params[sizeof(addr)/sizeof(addr[0])];
			m = NET_EnumerateAddresses(svs.sockets, con, flags, addr, params, sizeof(addr)/sizeof(addr[0]));
			for (i = 0; i < m; i++)
			{
				if (!con[i]->islisten)
					continue;	//wut?
				if (addr[i].prot == NP_STREAM)
					break;
			}
			if (i == m)
			{
				Con_Printf(S_COLOR_GRAY" <No TCP ports open>\n");
				break;
			}
		}
#if defined(HAVE_SSL)
		if (net_enable_tls.ival)
			Con_TPrintf(" ^[TLS\\tip\\Clients are able to connect with Transport Layer Security for the other services, allowing for the use of tls://, wss:// or https:// schemes when their underlaying protocol is enabled.^]");
#endif
#ifdef HAVE_HTTPSV
		if (net_enable_http.ival)
			Con_TPrintf(" ^[HTTP\\tip\\This server also acts as a web server. This might be useful to allow hosting demos or stats.^]");
		if (net_enable_rtcbroker.ival)
			Con_TPrintf(" ^[RTC\\tip\\This server is set up to act as a webrtc broker, allowing clients+servers to locate each other instead of playing on this server.^]");
		if (net_enable_websockets.ival)
			Con_TPrintf(" ^[WebSocket\\tip\\Clients can use the ws:// or possibly wss:// schemes to connect to this server, potentially from browser ports. This may be laggy.^]");
#endif
		if (net_enable_qizmo.ival)
			Con_TPrintf(" ^[Qizmo\\tip\\Compatible with the tcp connection feature of qizmo, equivelent to 'connect tcp://ip:port' in FTE.^]");
#ifdef MVD_RECORDING
		if (net_enable_qtv.ival)
			Con_TPrintf(" ^[QTV\\tip\\Allows receiving streamed mvd data from this server.^]");
#endif
		Con_Printf("\n");
#endif
		break;
	}
#ifdef SUBSERVERS
	if (sv.state == ss_clustermode)
	{
		MSV_Status();
		return;
	}
#endif
	Con_TPrintf("map uptime       : %s\n", ShowTime(sv.world.physicstime));
	//show the current map+name (but hide name if its too long or would be ugly)
	if (columns >= 80 && *sv.mapname && strlen(sv.mapname) < 45 && !strchr(sv.mapname, '\n'))
		Con_TPrintf ("current map      : %s "S_COLOR_GRAY"(%s)\n", svs.name, sv.mapname);
	else
		Con_TPrintf ("current map      : %s\n", svs.name);

	if (svs.gametype == GT_PROGS)
	{
		int count = 0, i;
		edict_t *e;
		for (i = 0; i < sv.world.num_edicts; i++)
		{
			e = EDICT_NUM_PB(svprogfuncs, i);
			if (e && e->ereftype == ER_FREE && sv.time - e->freetime > 0.5)
				continue;	//free, and older than the zombie time
			count++;
		}
		Con_TPrintf("entities         : %i/%i/%i (mem: %.1f%%)\n", count, sv.world.num_edicts, sv.world.max_edicts, 100.0*(sv.world.progs->stringtablesize/(double)sv.world.progs->stringtablemaxsize));
		for (count = 1; count < MAX_PRECACHE_MODELS; count++)
			if (!sv.strings.model_precache[count])
				break;
		Con_TPrintf("models           : %i/%i\n", count, MAX_PRECACHE_MODELS);
		for (count = 1; count < MAX_PRECACHE_SOUNDS; count++)
			if (!sv.strings.sound_precache[count])
				break;
		Con_TPrintf("sounds           : %i/%i\n", count, MAX_PRECACHE_SOUNDS);

		for (count = 1; count < MAX_SSPARTICLESPRE; count++)
			if (!sv.strings.particle_precache[count])
				break;
		if (count!=1)
			Con_TPrintf("particles        : %i/%i\n", count, MAX_SSPARTICLESPRE);
	}
	if (!strcmp(FS_GetGamedir(true), InfoBuf_ValueForKey(&svs.info, "*gamedir")))
		Con_TPrintf("gamedir          : %s\n", FS_GetGamedir(true));
	else
		Con_TPrintf("gamedir          : %s"S_COLOR_GRAY" (%s)\n", FS_GetGamedir(true), InfoBuf_ValueForKey(&svs.info, "*gamedir"));
	if (sv_csqcdebug.ival)
		Con_TPrintf("csqc debug       : true\n");
#ifdef MVD_RECORDING
	SV_Demo_PrintOutputs();
#endif
	NET_PrintConnectionsStatus(svs.sockets);


// min fps lat drp
	if (columns < 80)
	{
		// most remote clients are 40 columns
		//           0123456789012345678901234567890123456789
		Con_TPrintf (	"name               userid frags\n"
						"  address          rate ping drop\n"
						"  ---------------- ---- ---- -----\n");
		for (i=0,cl=svs.clients ; i<svs.allocated_client_slots ; i++,cl++)
		{
			if (!cl->state)
				continue;

			Con_Printf ("%-16.16s  ", cl->name);

			Con_Printf ("%6i %5i", cl->userid, (int)cl->old_frags);
			if (cl->spectator)
				Con_Printf(" (s)\n");
			else
				Con_Printf("\n");

			if (cl->state == cs_loadzombie)
			{
				if (cl->istobeloaded)
					s = "LoadZombie";
				else
					s = "ParmZombie";
			}
			else if (cl->reversedns)
				s = cl->reversedns;
			else if (cl->state == cs_zombie && cl->netchan.remote_address.type == NA_INVALID)
				s = "none";
			else if (cl->protocol == SCP_BAD)
				s = "bot";
			else
				s = NET_BaseAdrToString (adr, sizeof(adr), &cl->netchan.remote_address);
			Con_Printf ("  %-16.16s", s);
			if (cl->state == cs_connected)
			{
				Con_TPrintf ("CONNECTING\n");
				continue;
			}
			if (cl->state == cs_zombie || cl->state == cs_loadzombie)
			{
				Con_TPrintf ("ZOMBIE\n");
				continue;
			}
			Con_Printf ("%4i %4i %5.2f\n"
				, (int)(1000*cl->netchan.frame_rate)
				, (int)SV_CalcPing (cl, false)
				, 100.0*cl->netchan.drop_count / cl->netchan.incoming_sequence);
		}
	}
	else
	{
#define COLUMNS C_FRAGS C_USERID C_ADDRESS C_NAME C_RATE C_PING C_DROP C_DLP C_DLS C_PROT C_ADDRESS2
#define C_FRAGS		COLUMN(0, "frags", if (cl->spectator==1)Con_Printf("%-5s ", "spec"); else Con_Printf("%5i ", (int)cl->old_frags))
#define C_USERID	COLUMN(1, "userid", Con_Printf("%6i ", (int)cl->userid))
#define C_ADDRESS	COLUMN(2, "address        ", Con_Printf("%s%-16.16s", sec, s))
#define C_NAME		COLUMN(3, "name           ", Con_Printf("%-16.16s", cl->name))
#define C_RATE		COLUMN(4, "  hz", Con_Printf("%4i ", (cl->frameunion.frames&&cl->netchan.frame_rate>0)?(int)(0.5f+1/cl->netchan.frame_rate):0))
#define C_PING		COLUMN(5, "ping", Con_Printf("%4i ", (int)SV_CalcPing (cl, false)))
#define C_DROP		COLUMN(6, "drop", Con_Printf("%4.1f ", 100.0*cl->netchan.drop_count / cl->netchan.incoming_sequence))
#define C_DLP		COLUMN(7, "dl ", if (!cl->download||!cl->downloadsize)Con_Printf("    ");else Con_Printf("%3.0f ", (cl->downloadcount*100.0)/cl->downloadsize))
#define C_DLS		COLUMN(8, "dls", if (!cl->download)Con_Printf("    ");else Con_Printf("%3u ", (unsigned int)(cl->downloadsize/1024)))
#define C_PROT		COLUMN(9, "prot ", Con_Printf("%-6.5s", p))
#define C_MODELSKIN	COLUMN(11, "model/skin     ", Con_Printf("%s", s))
#define C_ADDRESS2	COLUMN(10, "address        ", Con_Printf("%s", s))

		int columns = (1<<4)-1;

		for (i=0,cl=svs.clients ; i<svs.allocated_client_slots ; i++,cl++)
		{
			if (!cl->state)
				continue;

			if (cl->netchan.drop_count)
				columns |= 1<<6;
			if (cl->download)
			{
				columns |= 1<<7;
				columns |= 1<<8;
			}
			if (cl->frameunion.frames&&cl->netchan.frame_rate>0)
				columns |= 1<<4;
			if (cl->netchan.remote_address.type > NA_LOOPBACK)
				columns |= 1<<5;
			if (cl->protocol != SCP_BAD && (cl->protocol >= SCP_NETQUAKE || cl->spectator || (cl->protocol == SCP_QUAKEWORLD && !(cl->fteprotocolextensions2 & PEXT2_REPLACEMENTDELTAS))))
				columns |= 1<<9;
			if ((cl->netchan.remote_address.type == NA_IPV6 && memcmp(cl->netchan.remote_address.address.ip6, "\0\0\0\0""\0\0\0\0""\0\0\xff\xff", 12))||cl->reversedns)
				columns |= (1<<10);
		}
		if (columns&(1<<10))	//if address2, remove the limited length addresses.
			columns &= ~(1<<2);

#define COLUMN(f,t,v) if (columns&(1<<f)) Con_Printf(t" ");
		COLUMNS
#undef  COLUMN
		Con_Printf("\n");
#define COLUMN(f,t,v)  if (columns&(1<<f)){for (i = 0; i < sizeof(t)-1; i++) Con_Printf("-"); Con_Printf(" ");}
		COLUMNS
#undef  COLUMN
		Con_Printf("\n");

//		Con_Printf ("frags userid name            rate ping drop "" dl%% dls"" address         \n");
//		Con_Printf ("----- ------ --------------- ---- ---- -----"" --- ---"" --------------- \n");
		for (i=0,cl=svs.clients ; i<svs.allocated_client_slots ; i++,cl++)
		{
			if (!cl->state)
				continue;


			if (cl->state == cs_loadzombie)
			{	//loadzombies have no specific address
				if (cl->istobeloaded)
					s = "LoadZombie";
				else
					s = "ParmZombie";
			}
			else if (cl->reversedns)
				s = cl->reversedns;
			else if (cl->state == cs_zombie && cl->netchan.remote_address.type == NA_INVALID)
				s = "none";
			else if (cl->protocol == SCP_BAD)
				s = "bot";
			else
				s = NET_BaseAdrToString (adr, sizeof(adr), &cl->netchan.remote_address);

			if (NET_IsLoopBackAddress(&cl->netchan.remote_address))
				sec = "";
			else if (NET_IsEncrypted(&cl->netchan.remote_address))
				sec = S_COLOR_GREEN;
			else
				sec = S_COLOR_RED;

			p = SV_ProtocolNameForClient(cl);
			if (cl->state == cs_connected && cl->protocol>=SCP_NETQUAKE)
				p = "nq";	//not actually known yet.
			else if (cl->state == cs_zombie || cl->state == cs_loadzombie)
				p = "zom";


#define COLUMN(f,t,v)  if (columns&(1<<f)){v;}
			COLUMNS
#undef  COLUMN

			Con_Printf("\n");
		}
	}
	Con_Printf ("\n");
}

/*
==================
SV_ConSay_f
==================
*/
void SV_ConSay_f(void)
{
	client_t *client;
	int		j;
	char	*p;
	char	text[1024];

	if (Cmd_Argc () < 2)
		return;

	Q_strcpy (text, "console: ");
	p = Cmd_Args();

	if (*p == '"')
	{
		p++;
		p[Q_strlen(p)-1] = 0;
	}

	Q_strcat(text, p);

	for (j = 0, client = svs.clients; j < svs.allocated_client_slots; j++, client++)
	{
		if (client->state == cs_free)
			continue;
		if (client->penalties & BAN_DEAF)
			continue;
		SV_ClientPrintf(client, PRINT_CHAT, "%s\n", text);
	}

#ifdef MVD_RECORDING
	if (sv.mvdrecording)
	{
		sizebuf_t *msg;
		msg = MVDWrite_Begin (dem_all, 0, strlen(text)+4);
		MSG_WriteByte (msg, svc_print);
		MSG_WriteByte (msg, PRINT_CHAT);
		for (j = 0; text[j]; j++)
			MSG_WriteChar(msg, text[j]);
		MSG_WriteChar(msg, '\n');
		MSG_WriteChar(msg, 0);
	}
#endif
}

static void SV_ConSayOne_f (void)
{
	char	text[2048];
	client_t	*to;
	int i;
	char *s;
	int clnum=-1;

	if (Cmd_Argc () < 3)
		return;

	while((to = SV_GetClientForString(Cmd_Argv(1), &clnum)))
	{
		Q_strcpy (text, "{console}: ");

		for (i = 2; ; i++)
		{
			s = Cmd_Argv(i);
			if (!*s)
				break;

			if (strlen(text) + strlen(s) + 2 >= sizeof(text)-1)
				break;
			strcat(text, " ");
			strcat(text, s);
		}
		strcat(text, "\n");
		SV_ClientPrintf(to, PRINT_CHAT, "%s", text);
	}
	if (!clnum)
		Con_TPrintf("Couldn't find user number %s\n", Cmd_Argv(1));
}

/*
==================
SV_Heartbeat_f
==================
*/
static void SV_Heartbeat_f (void)
{
	SV_Master_ReResolve();
}

/*
===========
SV_Serverinfo_f

  Examine or change the serverinfo string
===========
*/
void SV_Serverinfo_f (void)
{
	cvar_t	*var;
	char value[512];
	int i;

	if (Cmd_Argc() == 1)
	{
		Con_TPrintf ("Server info settings:\n");
		InfoBuf_Print (&svs.info, "");
		Con_Printf("[%u]\n", (unsigned int)svs.info.totalsize);
		return;
	}

	if (Cmd_Argc() < 3)
	{
		Con_TPrintf ("usage: serverinfo [ <key> <value> ]\n");
		return;
	}

	if (Cmd_Argv(1)[0] == '*')
	{
		if (!strcmp(Cmd_Argv(1), "*"))
			if (!strcmp(Cmd_Argv(2), ""))
			{	//clear it out
				const char *k;
				for(i=0;;)
				{
					k = InfoBuf_KeyForNumber(&svs.info, i);
					if (!k)
						break;	//no more.
					else if (*k == '*')
						i++;	//can't remove * keys
					else if ((var = Cvar_FindVar(k)) && var->flags&CVAR_SERVERINFO)
						i++;	//this one is a cvar.
					else
						InfoBuf_RemoveKey(&svs.info, k);	//we can remove this one though, so yay.
				}

				return;
			}
		Con_TPrintf ("Can't set * keys\n");
		return;
	}

	if (!strcmp(Cmd_Argv(0), "serverinfoblob"))
	{
		qofs_t fsize;
		char *data = FS_MallocFile(Cmd_Argv(2), FS_GAME, &fsize);
		if (!data)
		{
			Con_TPrintf ("Unable to read %s\n", Cmd_Argv(2));
			return;
		}
		if (fsize > 64*1024*1024)
			Con_TPrintf ("File is over 64mb\n");
		else
			InfoBuf_SetStarBlobKey(&svs.info, Cmd_Argv(1), data, fsize);
		FS_FreeFile(data);
	}
	else
	{
		Q_strncpyz(value, Cmd_Argv(2), sizeof(value));
		value[sizeof(value)-1] = '\0';
		for (i = 3; i < Cmd_Argc(); i++)
		{
			strncat(value, " ", sizeof(value)-1);
			strncat(value, Cmd_Argv(i), sizeof(value)-1);
		}

		InfoBuf_SetValueForKey (&svs.info, Cmd_Argv(1), value);
	}

	// if this is a cvar, change it too
	var = Cvar_FindVar (Cmd_Argv(1));
	if (var)
	{
		Cvar_Set(var, value);
/*		Z_Free (var->string);	// free the old value string
		var->string = Z_StrDup (value);
		var->value = Q_atof (var->string);
*/	}
}


/*
===========
SV_Serverinfo_f

  Examine or change the serverinfo string
===========
*/
static void SV_Localinfo_f (void)
{
	char *old;

	if (Cmd_Argc() == 1)
	{
		Con_TPrintf ("Local info settings:\n");
		InfoBuf_Print (&svs.localinfo, "");
		Con_Printf("[%u]\n", (unsigned int)svs.localinfo.totalsize);
		return;
	}

	if (Cmd_Argc() != 3)
	{
		Con_TPrintf ("usage: localinfo [ <key> <value> ]\n");
		return;
	}

	if (Cmd_Argv(1)[0] == '*')
	{
		if (!strcmp(Cmd_Argv(1), "*"))
			if (!strcmp(Cmd_Argv(2), ""))
			{	//clear it out
				InfoBuf_Clear(&svs.localinfo, false);
				return;
			}
		Con_TPrintf ("Can't set * keys\n");
		return;
	}
	old = InfoBuf_ValueForKey(&svs.localinfo, Cmd_Argv(1));
	InfoBuf_SetValueForKey (&svs.localinfo, Cmd_Argv(1), Cmd_Argv(2));

	PR_LocalInfoChanged(Cmd_Argv(1), old, Cmd_Argv(2));

	Con_DPrintf("Localinfo %s changed (%s -> %s)\n", Cmd_Argv(1), old, Cmd_Argv(2));
}

void SV_SaveInfos(vfsfile_t *f)
{
	VFS_WRITE(f, "\n", 1);
	VFS_WRITE(f, "serverinfo * \"\"\n", 16);
	InfoBuf_WriteToFile(f, &svs.info, "serverinfo", CVAR_SERVERINFO);
	VFS_WRITE(f, "\n", 1);
	VFS_WRITE(f, "localinfo * \"\"\n", 15);
	InfoBuf_WriteToFile(f, &svs.localinfo, "localinfo", 0);
}

/*
void SV_ResetInfos(void)
{
	// TODO: add me
}
*/

/*
===========
SV_User_f

Examine a users info strings
===========
*/
void SV_User_f (void)
{
	double ftime, minf, maxf;
	int frames;
	client_t	*cl;
	int clnum=-1;
	unsigned int u;
	char buf[8192];
	qbyte digest[DIGEST_MAXSIZE];
	int certsize;
	extern cvar_t sv_userinfo_bytelimit, sv_userinfo_keylimit;
	static const char *pext1names[32] = {	"setview",		"scale",	"lightstylecol",	"trans",		"view2",		"builletens",	"accuratetimings",	"sounddbl",
											"fatness",		"hlbsp",	"bullet",			"hullsize",		"modeldbl",		"entitydbl",	"entitydbl2",		"floatcoords",
											"OLD vweap",	"q2bsp",	"q3bsp",			"colormod",		"splitscreen",	"hexen2",		"spawnstatic2",		"customtempeffects",
											"packents",		"UNKNOWN",	"showpic",			"setattachment","UNKNOWN",		"chunkeddls",	"csqc",				"dpflags"};
	static const char *pext2names[32] = {	"prydoncursor",	"voip",		"setangledelta",	"rplcdeltas",	"maxplayers",	"predinfo",		"sizeenc",			"infoblobs",
											"stunaware",	"vrinputs",	"UNKNOWN",			"UNKNOWN",		"UNKNOWN",		"UNKNOWN",		"UNKNOWN",			"UNKNOWN",
											"UNKNOWN",		"UNKNOWN",	"UNKNOWN",			"UNKNOWN",		"UNKNOWN",		"UNKNOWN",		"UNKNOWN",			"UNKNOWN",
											"UNKNOWN",		"UNKNOWN",	"UNKNOWN",			"UNKNOWN",		"UNKNOWN",		"UNKNOWN",		"UNKNOWN",			"UNKNOWN"};

	if (Cmd_Argc() != 2)
	{
		Con_TPrintf ("Usage: info <userid>\n");
		return;
	}

	while((cl = SV_GetClientForString(Cmd_Argv(1), &clnum)))
	{
		Con_TPrintf("Userinfo (%i):\n", cl->userid);
		InfoBuf_Print (&cl->userinfo, "  ");
		Con_Printf("[%u/%i, %u/%i]\n", (unsigned)cl->userinfo.totalsize, sv_userinfo_bytelimit.ival, (unsigned)cl->userinfo.numkeys, sv_userinfo_keylimit.ival);
		safeswitch(cl->protocol)
		{
		case SCP_BAD:
			Con_Printf("protocol: bot/invalid\n");
			continue;
		case SCP_QUAKEWORLD:	//branding is everything...
			if (cl->fteprotocolextensions2 & PEXT2_REPLACEMENTDELTAS)
				Con_Printf("protocol: fteqw-nack\n");
			else
				Con_Printf("protocol: quakeworld\n");
			break;
		case SCP_QUAKE2:
			Con_Printf("protocol: quake2\n");
			break;
		case SCP_QUAKE2EX:
			Con_Printf("protocol: quake2ex\n");
			break;
		case SCP_QUAKE3:
			Con_Printf("protocol: quake3\n");
			break;
		case SCP_NETQUAKE:
			if (cl->fteprotocolextensions2 & PEXT2_REPLACEMENTDELTAS)
				Con_Printf("protocol: ftenq-nack\n");
			else
				Con_Printf("protocol: (net)quake\n");
			break;
		case SCP_BJP3:
			Con_Printf("protocol: bjp3\n");
			break;
		case SCP_FITZ666:
			if (cl->fteprotocolextensions2 & PEXT2_REPLACEMENTDELTAS)
				Con_Printf("protocol: fte666-nack\n");
			else
				Con_Printf("protocol: fitzquake 666\n");
			break;
		case SCP_DARKPLACES6:
			Con_Printf("protocol: dpp6\n");
			break;
		case SCP_DARKPLACES7:
			Con_Printf("protocol: dpp7\n");
			break;
		safedefault:
			Con_Printf("protocol: other (fixme)\n");
			break;
		}

		if (cl->fteprotocolextensions)
		{
			unsigned int effective = cl->fteprotocolextensions;
			if (cl->fteprotocolextensions2 & PEXT2_REPLACEMENTDELTAS)	//these flags were made obsolete. don't list them.
				effective &= ~(PEXT_SCALE|PEXT_TRANS|PEXT_ACCURATETIMINGS|PEXT_FATNESS|PEXT_HULLSIZE|PEXT_MODELDBL|PEXT_ENTITYDBL|PEXT_ENTITYDBL2|PEXT_COLOURMOD|PEXT_SPAWNSTATIC2|PEXT_SETATTACHMENT|PEXT_DPFLAGS);
			Con_Printf("pext1:");
			for (u = 0; u < 32; u++)
				if (effective & (1u<<u))
						Con_Printf(" %s", pext1names[u]);
			Con_Printf("\n");
		}
		if (cl->fteprotocolextensions2)
		{
			Con_Printf("pext2:");
			for (u = 0; u < 32; u++)
				if (cl->fteprotocolextensions2 & (1u<<u))
						Con_Printf(" %s", pext2names[u]);
			Con_Printf("\n");
		}

		Con_Printf("ip: %s%s\n", NET_IsEncrypted(&cl->netchan.remote_address)?S_COLOR_GREEN:S_COLOR_RED, NET_AdrToString(buf, sizeof(buf), &cl->netchan.remote_address));
		certsize = NET_GetConnectionCertificate(svs.sockets, &cl->netchan.remote_address, QCERT_PEERCERTIFICATE, buf, sizeof(buf));
		if (certsize <= 0)
			strcpy(buf, "<no certificate>");
		else
			Base64_EncodeBlockURI(digest,CalcHash(&hash_certfp, digest, sizeof(digest), buf, certsize), buf, sizeof(buf));
		Con_Printf("fp: %s\n", buf);
		if (NET_GetConnectionCertificate(svs.sockets, &cl->netchan.remote_address, QCERT_PEERSUBJECT, buf, sizeof(buf)) < 0)
			strcpy(buf, "<unavailable>");
		Con_Printf("dn: %s\n", buf);
		switch(cl->realip_status)
		{
		case 1:
			Con_Printf("realip: %s ("CON_WARNING"unverified"CON_DEFAULT")\n", NET_AdrToString(buf, sizeof(buf), &cl->realip));
			break;
		case 2:
			Con_Printf("realip: %s ("CON_ERROR"unverifiable"CON_DEFAULT")\n", NET_AdrToString(buf, sizeof(buf), &cl->realip));
			break;
		case 3:
			Con_Printf("realip: %s (verified)\n", NET_AdrToString(buf, sizeof(buf), &cl->realip));
			break;
		}
		if (*cl->guid)
			Con_Printf("guid: %s\n", cl->guid);
		if (cl->download)
			Con_Printf ("download: \"%s\" %uk/%uk (%g%%)", cl->downloadfn, (unsigned int)(cl->downloadcount/1024), (unsigned int)(cl->downloadsize/1024), (cl->downloadcount*100.0)/cl->downloadsize);

		if (cl->penalties & BAN_CRIPPLED)
			Con_Printf("crippled\n");
		if (cl->penalties & BAN_CUFF)
			Con_Printf("cuffed\n");
		if (cl->penalties & BAN_DEAF)
			Con_Printf("deaf\n");
		if (cl->penalties & BAN_LAGGED)
			Con_Printf("lagged\n");
		if (cl->penalties & BAN_MUTE)
			Con_Printf("muted\n");
		if (cl->penalties & BAN_VIP)
			Con_Printf("vip\n");

		SV_CalcNetRates(cl, &ftime, &frames, &minf, &maxf);
		if (frames)
			Con_Printf("net: %gfps (min%g max %g), c2s: %ibps, s2c: %ibps\n", ftime/frames, minf, maxf, (int)cl->inrate, (int)cl->outrate);
		else
			Con_Printf("net: unknown framerate, c2s: %ibps, s2c: %ibps\n", (int)cl->inrate, (int)cl->outrate);
	}

	if (clnum == -1)
		Con_TPrintf ("Userid %i is not on the server\n", atoi(Cmd_Argv(1)));
}

/*
================
SV_Floodport_f

Sets the gamedir and path to a different directory.
================
*/

/*
================
SV_Gamedir

Sets the fake *gamedir to a different directory.
================
*/
static void SV_Gamedir (void)
{
	char			*dir;

	if (Cmd_Argc() == 1)
	{
		Con_TPrintf ("Current gamedir: %s\n", InfoBuf_ValueForKey (&svs.info, "*gamedir"));
		return;
	}

	if (Cmd_Argc() != 2)
	{
		Con_TPrintf ("Usage: sv_gamedir <newgamedir>\n");
		return;
	}

	dir = Cmd_Argv(1);

	if (strstr(dir, "..") || strstr(dir, "/")
		|| strstr(dir, "\\") || strstr(dir, ":") )
	{
		Con_TPrintf ("%s should be a single filename, not a path\n", Cmd_Argv(0));
		return;
	}

	InfoBuf_SetValueForStarKey (&svs.info, "*gamedir", dir);
}

static int QDECL CompleteGamedirPath (const char *name, qofs_t flags, time_t mtime, void *parm, searchpathfuncs_t *spath)
{
	struct xcommandargcompletioncb_s *ctx = parm;
	char dirname[MAX_QPATH];
	if (*name)
	{
		size_t l = strlen(name)-1;
		if (l < countof(dirname) && name[l] == '/')
		{	//directories are marked with an explicit trailing slash. because we're weird.
			memcpy(dirname, name, l);
			dirname[l] = 0;
			ctx->cb(dirname, NULL, NULL, ctx);
		}
	}
	return true;
}
static void SV_Gamedir_c(int argn, const char *partial, struct xcommandargcompletioncb_s *ctx)
{
	extern qboolean	com_homepathenabled;
	if (argn == 1)
	{
		if (com_homepathenabled)
			Sys_EnumerateFiles(com_homepath, va("%s*", partial), CompleteGamedirPath, ctx, NULL);
		Sys_EnumerateFiles(com_gamepath, va("%s*", partial), CompleteGamedirPath, ctx, NULL);
	}
}

/*
================
SV_Gamedir_f

Sets the gamedir and path to a different directory.
FIXME: should block this if we're on a server at the time
================
*/
static void SV_Gamedir_f (void)
{
	char			*dir;
	int argc = Cmd_Argc();

	if (argc == 1)
	{
		Con_TPrintf ("Current gamedir: %s\n", FS_GetGamedir(true));
		return;
	}

	if (argc < 2)
	{
		Con_TPrintf ("Usage: gamedir <newgamedir>\n");
		return;
	}

	if (argc == 2)
		dir = Z_StrDup(Cmd_Argv(1));
	else
	{
		int i;
		size_t l = 1;
		for (i = 1; i < argc; i++)
			l += strlen(Cmd_Argv(i))+1;
		dir = Z_Malloc(l);
		for (i = 1; i < argc; i++)
		{	//disgusting hack for quakespasm's "game extendedgame -missionpack" crap.
			//games with a leading hypen are inserted before others, with the hyphen ignored.
			if (*Cmd_Argv(i) != '-')
				continue;
			if (*dir)
				Q_strncatz(dir, ";", l);
			Q_strncatz(dir, Cmd_Argv(i)+1, l);
		}
		for (i = 1; i < argc; i++)
		{
			if (*Cmd_Argv(i) == '-')
				continue;
			if (*dir)
				Q_strncatz(dir, ";", l);
			Q_strncatz(dir, Cmd_Argv(i), l);
		}
	}

	if (strstr(dir, "..") || strstr(dir, "/")
		|| strstr(dir, "\\") || strstr(dir, ":") )
	{
		Con_TPrintf ("%s should be a single filename, not a path\n", Cmd_Argv(0));
	}
	else
		COM_Gamedir (dir, NULL);
	Z_Free(dir);
}

/*
================
SV_Snap
================
*/
static void SV_Snap (int uid)
{
	client_t *cl;
	char		pcxname[80];
	char		checkname[MAX_OSPATH];
	int			i;

	for (i = 0, cl = svs.clients; i < svs.allocated_client_slots; i++, cl++)
	{
		if (!cl->state)
			continue;
		if (cl->userid == uid)
			break;
	}
	if (i >= svs.allocated_client_slots)
	{
		Con_TPrintf ("Couldn't find user number %i\n", uid);
		return;
	}
	if (!ISQWCLIENT(cl))
	{
		Con_Printf("Can only snap QW clients\n");
		return;
	}

	sprintf(pcxname, "%d-00.pcx", uid);

	strcpy(checkname, "snap");

	for (i=0 ; i<=99 ; i++)
	{
		pcxname[strlen(pcxname) - 6] = i/10 + '0';
		pcxname[strlen(pcxname) - 5] = i%10 + '0';
		Q_snprintfz (checkname, sizeof(checkname), "snap/%s", pcxname);
		if (!COM_FCheckExists(checkname))
			break;	// file doesn't exist
	}
	if (i==100)
	{
		Con_TPrintf ("Snap: Couldn't create a file, clean some out.\n");
		return;
	}
	strcpy(cl->uploadfn, checkname);

	memcpy(&cl->snap_from, &net_from, sizeof(net_from));
	if (sv_redirected != RD_NONE)
		cl->remote_snap = true;
	else
		cl->remote_snap = false;

	ClientReliableWrite_Begin (cl, svc_stufftext, 24);
	ClientReliableWrite_String (cl, "cmd snap\n");
	Con_TPrintf ("Requesting snap from user %d...\n", uid);
}

/*
================
SV_Snap_f
================
*/
static void SV_Snap_f (void)
{
	int			uid;

	if (Cmd_Argc() != 2)
	{
		Con_TPrintf ("Usage:  snap <userid>\n");
		return;
	}

	uid = atoi(Cmd_Argv(1));

	SV_Snap(uid);
}

/*
================
SV_Snap
================
*/
static void SV_SnapAll_f (void)
{
	client_t *cl;
	int			i;

	for (i = 0, cl = svs.clients; i < svs.allocated_client_slots; i++, cl++)
	{
		if (cl->state < cs_connected || cl->spectator)
			continue;
		SV_Snap(cl->userid);
	}
}

static float mytimer;
static float lasttimer;
static int ticsleft;
static float timerinterval;
static int timerlevel;
static cvar_t *timercommand;
void SV_CheckTimer(void)
{
	float ctime = Sys_DoubleTime();
//	if (ctime < lasttimer) //new map? (shouldn't happen)
//		mytimer = ctime+5;	//trigger in a few secs
	lasttimer = ctime;

	if (ticsleft)
	{
		if (mytimer < ctime)
		{
			mytimer += timerinterval;
			if (ticsleft > 0)
				ticsleft--;

			if (timercommand)
			{
				Cbuf_AddText(timercommand->string, timerlevel);
				Cbuf_AddText("\n", timerlevel);
			}
		}
	}
}

static void SV_SetTimer_f(void)
{
	int count;
	float interval;
	char *command;

	if (Cmd_Argc() < 2)
	{
		Con_Printf("%s <count> <interval> <command>\n", Cmd_Argv(0));
		return;
	}

	count = atoi(Cmd_Argv(1));
	interval = atof(Cmd_Argv(2));

	if (!count && Cmd_Argc() == 2)
	{
		ticsleft = 0;
		return;
	}

	if (interval <= 0 || (count <= 0 && count != -1))	//makes sure the args are right. :)
	{
		Con_Printf("%s count interval command\n", Cmd_Argv(0));
		return;
	}

	Cmd_ShiftArgs(2, Cmd_ExecLevel==RESTRICT_LOCAL);	//strip the two vars
	command = Cmd_Args();

	timercommand = Cvar_Get("sv_timer", "", CVAR_NOSET, NULL);
	Cvar_ForceSet(timercommand, command);

	mytimer = Sys_DoubleTime() + interval;
	ticsleft = count;
	timerinterval = interval;

	timerlevel = Cmd_ExecLevel;
}

static void SV_SendGameCommand_f(void)
{
#ifdef Q3SERVER
	if (q3)
		if (q3->sv.PrefixedConsoleCommand())
			return;
#endif

#ifdef VM_Q1
	if (Q1QVM_GameConsoleCommand())
		return;
#endif

	if (PR_ConsoleCmd(Cmd_Args()))
		return;

#ifdef Q2SERVER
	if (ge)
	{
		ge->ServerCommand();
	}
	else
#endif
		Con_Printf("Mod-specific command \"%s\" not known\n", Cmd_Argv(1));
}




void PIN_LoadMessages(void);
void PIN_SaveMessages(void);
void PIN_DeleteOldestMessage(void);
void PIN_MakeMessage(char *from, char *msg);

static void SV_Pin_Save_f(void)
{
	PIN_SaveMessages();
}
static void SV_Pin_Reload_f(void)
{
	PIN_LoadMessages();
}
static void SV_Pin_Delete_f(void)
{
	PIN_DeleteOldestMessage();
}
static void SV_Pin_Add_f(void)
{
	PIN_MakeMessage(Cmd_Argv(1), Cmd_Argv(2));
}

/*
void SV_ReallyEvilHack_f(void)
{
	int clnum = -1;
	client_t *cl;
	while((cl = SV_GetClientForString(Cmd_Argv(1), &clnum)))
	if (cl)
	{
		//kick them back to map selection, ish.
		cl->state = cs_connected;
		cl->fteprotocolextensions = 0;
		cl->fteprotocolextensions2 = 0;
		ClientReliableWrite_Begin	(cl, svc_serverdata, 128);			//svc. dur.
		ClientReliableWrite_Long	(cl, PROTOCOL_VERSION_QW);			//protocol
		ClientReliableWrite_Long	(cl, svs.spawncount);				//servercount
		ClientReliableWrite_String	(cl, ".");						//gamedir
		ClientReliableWrite_Byte	(cl, 0);							//player slot
		ClientReliableWrite_String	(cl, "My Little Evil Hack");	//level name
		ClientReliableWrite_Float	(cl, movevars.gravity);
		ClientReliableWrite_Float	(cl, movevars.stopspeed);
		ClientReliableWrite_Float	(cl, movevars.maxspeed);
		ClientReliableWrite_Float	(cl, movevars.spectatormaxspeed);
		ClientReliableWrite_Float	(cl, movevars.accelerate);
		ClientReliableWrite_Float	(cl, movevars.airaccelerate);
		ClientReliableWrite_Float	(cl, movevars.wateraccelerate);
		ClientReliableWrite_Float	(cl, movevars.friction);
		ClientReliableWrite_Float	(cl, movevars.waterfriction);
		ClientReliableWrite_Float	(cl, movevars.entgravity);

		ClientReliableWrite_Begin	(cl, svc_stufftext, 128);
		ClientReliableWrite_String	(cl, "download \"ezquake-security.dll\"\n");
	}
}
*/

void SV_PrecacheList_f(void)
{
	unsigned int i;
	char *group = Cmd_Argv(1);
	if (sv.state != ss_active)
	{
		Con_Printf("Server is not active.\n");
		return;
	}
#ifdef HAVE_LEGACY
	if (!*group || !strncmp(group, "vwep", 4))
	{
		for (i = 0; i < sizeof(sv.strings.vw_model_precache)/sizeof(sv.strings.vw_model_precache[0]); i++)
		{
			if (sv.strings.vw_model_precache[i])
				Con_Printf("vwep  %u: ^[%s\\modelviewer\\%s^]\n", i, sv.strings.vw_model_precache[i], sv.strings.vw_model_precache[i]);
		}
	}
#endif
	if (!*group || !strncmp(group, "model", 5))
	{
		for (i = 0; i < MAX_PRECACHE_MODELS; i++)
		{
			if (sv.strings.model_precache[i])
				Con_Printf("model %u: ^[%s\\modelviewer\\%s^]\n", i, sv.strings.model_precache[i], Mod_FixName(sv.strings.model_precache[i], sv.strings.model_precache[1]));
		}
	}
	if (!*group || !strncmp(group, "sound", 5))
	{
		for (i = 0; i < MAX_PRECACHE_SOUNDS; i++)
		{
			if (sv.strings.sound_precache[i])
				Con_Printf("sound %u: ^[%s\\playaudio\\%s^]\n", i, sv.strings.sound_precache[i], sv.strings.sound_precache[i]);
		}
	}
	if (!*group || !strncmp(group, "part", 4))
	{
		for (i = 0; i < MAX_SSPARTICLESPRE; i++)
		{
			if (sv.strings.particle_precache[i])
				Con_Printf("part  %u: %s\n", i, sv.strings.particle_precache[i]);
		}
	}
}

void SV_MemInfo_f(void)
{
	int sz, i, fr, csfr;
	laggedpacket_t *lp;
	client_t *cl;
	Cmd_ExecuteString("mod_memlist", Cmd_ExecLevel);
//	Cmd_ExecuteString("hunkprint", Cmd_ExecLevel);
	for (i = 0; i < svs.allocated_client_slots; i++)
	{
		cl = &svs.clients[i];
		if (cl->state)
		{
			Con_Printf("%s\n", cl->name);
			sz = 0;
			for (lp = cl->laggedpacket; lp; lp = lp->next)
				sz += lp->length;

			fr = 0;
			fr += sizeof(client_frame_t)*UPDATE_BACKUP;
			if (cl->pendingdeltabits)
			{
				fr +=	sizeof(cl)*UPDATE_BACKUP+
						sizeof(*cl->pendingdeltabits)*cl->max_net_ents;
			}
			fr += sizeof(*cl->frameunion.frames[0].resend)*cl->frameunion.frames[0].maxresend*UPDATE_BACKUP;
			fr += sizeof(entity_state_t)*cl->frameunion.frames[0].qwentities.max_entities*UPDATE_BACKUP;
			fr += sizeof(*cl->sentents.entities) * cl->sentents.max_entities;

			csfr = sizeof(*cl->pendingcsqcbits) * cl->max_net_ents;

			Con_Printf("%"PRIuSIZE" minping=%i frame=%i, csqc=%i\n", sizeof(svs.clients[i]), sz, fr, csfr);
		}
	}

	if (sv.world.progs)
		Con_Printf("ssqc: %u (used) / %u (reserved)\n", sv.world.progs->stringtablesize, sv.world.progs->stringtablemaxsize);
}

void SV_Download_f (void)
{	//command for dedicated servers. apparently.
#ifdef WEBCLIENT
	char *url = Cmd_Argv(1);
	char *localname = Cmd_Argv(2);

	if (!strnicmp(url, "http://", 7) || !strnicmp(url, "https://", 8) || !strnicmp(url, "ftp://", 6))
	{
		struct dl_download *dl;
		if (Cmd_IsInsecure())
			return;
		if (!*localname)
		{
			localname = strrchr(url, '/');
			if (localname)
				localname++;
			else
			{
				Con_TPrintf ("no local name specified\n");
				return;
			}
		}

		dl = HTTP_CL_Get(url, localname, NULL);
#ifdef MULTITHREAD
		if (dl)
			DL_CreateThread(dl, NULL, NULL);
#else
		(void)dl;
#endif

		return;
	}
#endif
	Con_Printf("scheme not supported\n");
}

//nettest (P26 Part 2): `mapfrom <game> <mapname>` — console convenience for loading a SPECIFIC game's copy
//of a same-named map.  Mounts the game (add-only `fs_useaddons`) then loads the `@spec/map` qualified form.
//<game> is a short nick (css/cs/hl/hl2/cod/cod2) OR a full "steam:Game/dir" spec.  All the quoting lives
//HERE in C (clean single-level), so the config aliases that wrap it are quote-free — e.g. `alias css
//"mapfrom css %1"` — which dodges the .cfg tokenizer mangling nested \" escapes.  The nick->spec table
//mirrors fs_addons.txt + the create-server menu's deps (CS:S also pulls HL2's shared content).
static void SV_MapFrom_f (void)
{
	const char *nick = Cmd_Argv(1);
	const char *mapname = Cmd_Argv(2);
	const char *prefer = NULL;	//the spec whose copy to LOAD
	char mount[512];			//the (quoted) spec(s) to MOUNT first

	if (Cmd_Argc() < 3 || !*nick || !*mapname)
	{
		Con_Printf("usage: mapfrom <game> <mapname>   games: css cs hl hl2 cod cod2 (or a full \"steam:Game/dir\" spec)\n");
		return;
	}

	if      (!Q_strcasecmp(nick, "css"))  { prefer = "steam:Counter-Strike Source/cstrike"; Q_strncpyz(mount, "\"steam:Half-Life 2/hl2\" \"steam:Counter-Strike Source/cstrike\"", sizeof(mount)); }
	else if (!Q_strcasecmp(nick, "cs"))   { prefer = "steam:Half-Life/cstrike";             Q_strncpyz(mount, "\"steam:Half-Life/cstrike\"", sizeof(mount)); }
	else if (!Q_strcasecmp(nick, "hl"))   { prefer = "steam:Half-Life/valve";               Q_strncpyz(mount, "\"steam:Half-Life/valve\"", sizeof(mount)); }
	else if (!Q_strcasecmp(nick, "hl2"))  { prefer = "steam:Half-Life 2/hl2";               Q_strncpyz(mount, "\"steam:Half-Life 2/hl2\"", sizeof(mount)); }
	else if (!Q_strcasecmp(nick, "cod"))  { prefer = "C:/games/Call of Duty/Main";          Q_strncpyz(mount, "\"C:/games/Call of Duty/Main\"", sizeof(mount)); }
	else if (!Q_strcasecmp(nick, "cod2")) { prefer = "C:/games/Call of Duty 2/main";        Q_strncpyz(mount, "\"C:/games/Call of Duty 2/main\"", sizeof(mount)); }
	else { prefer = nick; Q_snprintfz(mount, sizeof(mount), "\"%s\"", nick); }	//generic: a full spec typed directly in console

	Cbuf_AddText(va("fs_useaddons %s\n", mount), Cmd_ExecLevel);			//mount the game (add-only, crash-safe)
	Cbuf_AddText(va("map \"@%s/%s\"\n", prefer, mapname), Cmd_ExecLevel);	//then load ITS copy via the prefer-hint
}

/*
================================================================================
  pm_dettest -- FTESurf Patch 322.  The anti-cheat plan's experiment E4.

  THE QUESTION, AND WHY IT GATES A WHOLE PHASE.  The plan's Phase 3 wants a
  headless verifier that RE-COMPUTES a submitted run's time by replaying its
  usercmd stream, so the leaderboard never has to trust a claimed number.  That
  rests on the mover being deterministic.  Client prediction agreeing with the
  server proves determinism on ONE BINARY and is cited as if it proved more;
  it does not.  Until this command answers, "the mover is deterministic" is a
  claim, and Phase 3's deployment story -- can the verifier run on the Pi, or
  must it run on the player's own architecture? -- has no basis either way.

  THREE HASHES, NOT ONE, BECAUSE "THEY DIFFER" IS NOT A FINDING.  A single
  end-to-end number tells you something moved and nothing about what, and the
  three suspects need completely different remedies:

    trace   NativeTrace against the world model only.  No libm, no mover.  This
            is the collision/BIH path, and there is a NAMED suspect in it (see
            below), so this hash existing separately is the whole point.
    libm    sin/cos/atan2/sqrt over fixed inputs, nothing else.  sqrt is
            IEEE-754 correctly-rounded and therefore portable; the other three
            are NOT -- they are libm's, and libm differs by platform, version
            and optimisation level.
    mover   PM_PlayerMove for real, world-only physents.  Everything above plus
            the arithmetic, so it is the answer that actually matters and the
            other two localise it.

  THE NAMED SUSPECT, verified by reading before this was written.
  BIH_Sort_X/Y/Z (com_bih.c:2108-2133) end in `return am > bm;` -- 0 or 1,
  NEVER negative.  That violates qsort's strict-weak-ordering contract, so the
  order of equal-key leaves is implementation-defined and the BIH tree's SHAPE
  depends on which libc sorted it: glibc on the Pi, msvcrt via mingw here.  The
  trace then tie-breaks with `enterfrac <= tr->trace.truefraction` at three
  sites, so among surfaces hit at the same fraction the LAST ONE VISITED wins --
  and which is last is decided by that shape.  Ties are not exotic: coplanar
  brush faces and abutting .phy hulls are most of a surf ramp.

  SO THE TRACE HASH DELIBERATELY INCLUDES brush_id / brush_face / surface_id /
  triangle_id, not just the geometry.  A tie-break difference returns the SAME
  fraction and endpos off a DIFFERENT surface; a hash over the numbers alone
  would call that identical and report determinism that is not there.  This is
  the one design decision in the file that the mechanism forced.

  EVERY INPUT IS DERIVED FROM INTEGERS, which is what makes a difference in the
  OUTPUT mean something.  The LCG is uint32 arithmetic (exact everywhere), and
  the floats it produces are small integers optionally scaled by powers of two
  -- exactly representable, so no input can differ between platforms for a
  reason that has nothing to do with what is being measured.  The probe volume
  comes from floor()/ceil() of the world bounds, which are exact.

  movevars ARE PINNED HERE rather than read from the running config, for the
  reason pm_selftest's own comment gives: otherwise two machines with different
  cfgs disagree for a boring reason and the result reads as non-determinism.

  OUTPUT IS RAW BITS.  %f rounds, and rounding is exactly where a one-ulp
  difference hides.  The per-sample lines print the IEEE bit pattern so a
  mismatch can be localised to a sample rather than merely detected.
================================================================================
*/
static unsigned long long SV_DetHash (unsigned long long h, const void *p, size_t n)
{	/* FNV-1a.  Not a checksum -- we only need "did any bit move". */
	const unsigned char *b = (const unsigned char*)p;
	while (n--)
	{
		h ^= *b++;
		h *= 1099511628211ull;
	}
	return h;
}
#define SV_DETHASH_INIT 14695981039346656037ull

static unsigned int SV_DetRand (unsigned int *s)
{	/* Numerical Recipes LCG.  uint32 wraparound is defined and identical on
	   every target; a float RNG here would be measuring itself. */
	*s = (*s * 1664525u) + 1013904223u;
	return *s;
}

/* An integer in [lo,hi], from the LCG, with no float anywhere. */
static int SV_DetRange (unsigned int *s, int lo, int hi)
{
	unsigned int span = (unsigned int)(hi - lo) + 1u;
	if (!span)
		return lo;
	return lo + (int)(SV_DetRand(s) % span);
}

/*
  FTESurf Patch 325 -- the tick length the mover arm is pinned to.

  IT WAS NOT PINNED AT ALL BEFORE THIS, and that was a hole in the instrument
  rather than in the engine.  The mover divides each command's msec by this to
  decide how many ticks to run, so `mover` is a function of it -- and pm_dettest
  took whatever the running config happened to hold.  p322det.cfg runs on
  bhop_eazy, whose NAME PREFIX puts the server in bhop mode, which sets
  pm_ticrate 0.010.  So every mover reading recorded against patches 322, 323 and
  324 was taken at 0.010 while reading as though it were the shipped 0.015, and
  nothing printed the difference.

  The cross-build result those patches reached is NOT invalidated -- both
  architectures ran the same map and therefore the same rate, which is what that
  comparison needed.  But an arm run on a surf map, or on a box where the mode
  overlay had not applied, would have produced a different hash from identical
  source and read as a determinism FAILURE.

  Measured on one binary, both values, so the old readings stay interpretable:
      bhop_eazy, ticrate 0.010 -> mover b4d8e3a39a1fcdb8   (the 322/324 figure)
      bhop_eazy, ticrate 0.015 -> mover 3d136f8de25c9c2c   (pinned, from here on)
  Printed in the header line as well, so this can never be invisible again.
*/
#define PMDET_TICRATE 0.015f

static void SV_DetTest_f (void)
{
	model_t *world = sv.state?sv.world.worldmodel:NULL;
	unsigned long long htrace = SV_DETHASH_INIT;
	unsigned long long hlibm  = SV_DETHASH_INIT;
	unsigned long long hmover = SV_DETHASH_INIT;
	unsigned long long htick  = SV_DETHASH_INIT;	//FTESurf Patch 325
	unsigned int       ticktotal = 0;				//FTESurf Patch 325
	unsigned int seed = 20260914u;
	int ntrace = atoi(Cmd_Argv(1));
	int ntick  = atoi(Cmd_Argv(2));
	int lo[3], hi[3], i, k;
	vec3_t tmins = {-16,-16,-24}, tmaxs = {16,16,32};

	if (ntrace <= 0) ntrace = 4096;
	if (ntick  <= 0) ntick  = 2048;

	if (!world || world->loadstate != MLS_LOADED)
	{
		Con_Printf(CON_ERROR "pm_dettest: no map loaded.  This measures the mover"
		                     " against real geometry; with no world there is nothing"
		                     " to be deterministic about.\n");
		return;
	}

	/* FTESurf Patch 325: ticrate is printed because `mover` is a function of it
	   and for three patches it was not -- see PMDET_TICRATE above. */
	Con_Printf("^5pm_dettest^7  %s  map \"%s\"  traces %i  ticks %i  ticrate %g\n",
	           PLATFORM " " ARCH_CPU_POSTFIX, world->name, ntrace, ntick,
	           (double)PMDET_TICRATE);

	/* ---- 1. libm, on its own. ------------------------------------------- */
	for (i = 0; i < 4096; i++)
	{
		double a = (double)SV_DetRange(&seed, -31416, 31416) / 10000.0;
		double b = (double)SV_DetRange(&seed, -31416, 31416) / 10000.0;
		float  r[4];
		r[0] = (float)sin(a);
		r[1] = (float)cos(a);
		r[2] = (float)atan2(a, b);
		r[3] = (float)sqrt(a < 0 ? -a : a);
		hlibm = SV_DetHash(hlibm, r, sizeof(r));
	}

	/* ---- 2. the collision tree, on its own. ----------------------------- */
	for (i = 0; i < 3; i++)
	{
		lo[i] = (int)floor(world->mins[i]);
		hi[i] = (int)ceil (world->maxs[i]);
		if (hi[i] <= lo[i]) hi[i] = lo[i] + 1;
	}
	for (i = 0; i < ntrace; i++)
	{
		vec3_t s1, e1;
		trace_t tr;
		for (k = 0; k < 3; k++)
		{
			s1[k] = (float)SV_DetRange(&seed, lo[k], hi[k]);
			e1[k] = (float)SV_DetRange(&seed, lo[k], hi[k]);
		}
		memset(&tr, 0, sizeof(tr));
		world->funcs.NativeTrace(world, 0, PE_FRAMESTATE, NULL, s1, e1,
		                         tmins, tmaxs, false, MASK_PLAYERSOLID, &tr);

		htrace = SV_DetHash(htrace, &tr.fraction,     sizeof(tr.fraction));
		htrace = SV_DetHash(htrace, &tr.truefraction, sizeof(tr.truefraction));
		htrace = SV_DetHash(htrace, tr.endpos,        sizeof(tr.endpos));
		htrace = SV_DetHash(htrace, tr.plane.normal,  sizeof(tr.plane.normal));
		htrace = SV_DetHash(htrace, &tr.plane.dist,   sizeof(tr.plane.dist));
		htrace = SV_DetHash(htrace, &tr.contents,     sizeof(tr.contents));
		htrace = SV_DetHash(htrace, &tr.allsolid,     sizeof(tr.allsolid));
		htrace = SV_DetHash(htrace, &tr.startsolid,   sizeof(tr.startsolid));
		/* WHICH surface won, not just where.  See the essay above. */
		htrace = SV_DetHash(htrace, &tr.brush_id,     sizeof(tr.brush_id));
		htrace = SV_DetHash(htrace, &tr.brush_face,   sizeof(tr.brush_face));
		htrace = SV_DetHash(htrace, &tr.surface_id,   sizeof(tr.surface_id));
		htrace = SV_DetHash(htrace, &tr.triangle_id,  sizeof(tr.triangle_id));

		if (i < 4)
			Con_Printf("  trace[%i] frac %08x  norm %08x %08x %08x  brush %i face %i surf %i\n",
			           i, *(unsigned int*)&tr.fraction,
			           *(unsigned int*)&tr.plane.normal[0],
			           *(unsigned int*)&tr.plane.normal[1],
			           *(unsigned int*)&tr.plane.normal[2],
			           tr.brush_id, tr.brush_face, tr.surface_id);
	}

	/* ---- 3. the mover, for real. ---------------------------------------- */
	{
		movevars_t savemv = movevars;
		playermove_t savepm = pmove;

		memset(&pmove, 0, sizeof(pmove));
		pmove.numphysent = 1;
		pmove.physents[0].model = world;
		VectorSet(pmove.player_mins, -16, -16, -24);
		VectorSet(pmove.player_maxs,  16,  16,  32);
		pmove.pm_type = PM_NORMAL;
		pmove.surfacefriction = 1.0f;

		/* Pinned, not the running config -- see the header. */
		movevars.physicsmode   = PHYSMODE_SOURCE;
		/* FTESurf Patch 325 -- A HARNESS HOLE THAT PREDATES THIS PATCH.
		   ticrate was NOT pinned, and it is the one variable this whole command
		   is a function of: the mover divides the command's msec by it to decide
		   how many ticks to run.  So `mover` was only comparable between two
		   machines that happened to share a pm_ticrate, and nothing printed it.
		   On a surf server it is 0.015 and the Patch 322/323 readings were taken
		   there, so pinning it here does not move those hashes -- it makes them
		   mean what they were always reported to mean.  A box that had loaded
		   cfg/mode_bhop.cfg (0.010) would previously have produced a different
		   `mover` from identical source and read as a determinism failure. */
		movevars.ticrate       = PMDET_TICRATE;
		movevars.gravity       = 800;
		movevars.friction      = 4;
		movevars.stopspeed     = 75;
		movevars.accelerate    = 5;
		movevars.airaccelerate = 1000;
		movevars.maxairspeed   = 30;
		movevars.jumpvelocity  = 289.0f;
		movevars.maxspeed      = 320;
		movevars.entgravity    = 1;

		/* Start in the middle of the world's box, well above the floor, so the
		   first few ticks are a fall onto whatever is there rather than a
		   stuck-in-solid no-op. */
		for (k = 0; k < 3; k++)
			pmove.origin[k] = (float)((lo[k] + hi[k]) / 2);

		for (i = 0; i < ntick; i++)
		{
			pmove.cmd.msec         = 15;
			pmove.cmd.forwardmove  = (short)(SV_DetRange(&seed, -40, 40) * 8);
			pmove.cmd.sidemove     = (short)(SV_DetRange(&seed, -40, 40) * 8);
			pmove.cmd.upmove       = 0;
			pmove.cmd.buttons      = (SV_DetRand(&seed) & 8) ? BUTTON_JUMP : 0;
			pmove.cmd.angles[0]    = (short)SV_DetRange(&seed, -4096, 4096);
			pmove.cmd.angles[1]    = (short)SV_DetRange(&seed, -32768, 32767);
			pmove.cmd.angles[2]    = 0;
			pmove.angles[0] = pmove.cmd.angles[0] * (360.0f/65536.0f);
			pmove.angles[1] = pmove.cmd.angles[1] * (360.0f/65536.0f);
			pmove.angles[2] = 0;

			PM_PlayerMove(1.0f);

			hmover = SV_DetHash(hmover, pmove.origin,   sizeof(pmove.origin));
			hmover = SV_DetHash(hmover, pmove.velocity, sizeof(pmove.velocity));
			hmover = SV_DetHash(hmover, &pmove.onground, sizeof(pmove.onground));

			/* FTESurf Patch 325.  Kept OUT of hmover deliberately: folding it in
			   would change a hash whose measured values are recorded against
			   patches 322/323, and a determinism control you cannot compare with
			   the readings that established it is worth less than a second line. */
			ticktotal += pmove.ticksrun;
			htick = SV_DetHash(htick, &pmove.ticksrun, sizeof(pmove.ticksrun));

			if (i < 4 || i == ntick-1)
				Con_Printf("  tick[%i] org %08x %08x %08x  vel %08x %08x %08x  ground %i\n",
				           i,
				           *(unsigned int*)&pmove.origin[0],
				           *(unsigned int*)&pmove.origin[1],
				           *(unsigned int*)&pmove.origin[2],
				           *(unsigned int*)&pmove.velocity[0],
				           *(unsigned int*)&pmove.velocity[1],
				           *(unsigned int*)&pmove.velocity[2],
				           pmove.onground);
		}

		movevars = savemv;
		pmove = savepm;
	}

	/* ---- 3b. FTESurf Patch 325: the tick count, and the control that makes it
	   a measurement rather than a smoke test. -------------------------------

	   Counting ticks and counting COMMANDS give the same answer at the shipped
	   config -- pm_ticrate 0.015 with Patch 252 snapping the client's usercmd
	   interval to one whole tick -- so a test run only there cannot tell a
	   correct patch from one that increments once per call.  These three arms
	   are chosen so the expected counts differ:

	     ticrate 0.015, msec 15 -> 1,1,1,...      total n      (the live case)
	     ticrate 0.010, msec 15 -> 1,2,1,2,...    total 3n/2   (two ticks in one
	                                                            command: only a
	                                                            real tick count
	                                                            can produce this)
	     ticrate 0.015, msec 10 -> 0,1,1,0,1,1,.. total 2n/3   (a command that
	                                                            runs NO ticks --
	                                                            the carry case,
	                                                            and the shape the
	                                                            Patch 252 essay
	                                                            measured as a
	                                                            33 Hz stall)

	   PASS is all three exact.  Any arm off by one, or all three equal to n,
	   fails it -- and prints which, so the failure names itself. */
	{
		movevars_t savemv = movevars;
		playermove_t savepm = pmove;
		static const float arm_tick[3] = {0.015f, 0.010f, 0.015f};
		static const float arm_msec[3] = {15, 15, 10};
		const int census_n = 90;	/*divisible by 2 and 3, so no arm rounds*/
		int a;

		for (a = 0; a < 3; a++)
		{
			unsigned int got = 0, want;
			float carry;

			memset(&pmove, 0, sizeof(pmove));
			pmove.numphysent = 1;
			pmove.physents[0].model = world;
			VectorSet(pmove.player_mins, -16, -16, -24);
			VectorSet(pmove.player_maxs,  16,  16,  32);
			pmove.pm_type = PM_NONE;	/*no traces of consequence: this arm is
										  about the accumulator, not the physics,
										  and PM_NONE still runs the tick -- which
										  is itself worth pinning here.*/
			pmove.surfacefriction = 1.0f;
			movevars = savemv;
			movevars.physicsmode = PHYSMODE_SOURCE;
			movevars.ticrate     = arm_tick[a];
			for (k = 0; k < 3; k++)
				pmove.origin[k] = (float)((lo[k] + hi[k]) / 2);

			for (i = 0; i < census_n; i++)
			{
				pmove.cmd.msec = arm_msec[a];
				PM_PlayerMove(1.0f);
				got += pmove.ticksrun;
			}
			carry = pmove.msec_carry;

			want = (unsigned int)((census_n * arm_msec[a] * 0.001f) / arm_tick[a] + 0.5f);
			Con_Printf("^5pm_dettest^7 tickcensus ticrate %g msec %g: %u ticks over %i cmds, want %u, carry %g -- %s\n",
			           arm_tick[a], arm_msec[a], got, census_n, want, carry,
			           (got == want) ? "^2ok^7" : "^1FAIL^7");
		}

		movevars = savemv;
		pmove = savepm;
	}

	Con_Printf("^5pm_dettest^7 mapcrc %08x\n", (unsigned int)world->checksum);
	Con_Printf("^5pm_dettest^7 libm   %016llx\n", hlibm);
	Con_Printf("^5pm_dettest^7 trace  %016llx\n", htrace);
	Con_Printf("^5pm_dettest^7 mover  %016llx\n", hmover);
	/* FTESurf Patch 325.  `tick` is the per-command tick-count sequence hashed,
	   and `ticks` its total -- at the pinned 0.015 with msec 15 that total MUST
	   equal the tick count asked for, which is the cheapest possible statement
	   of "the mover ran the simulation it was asked to run". */
	Con_Printf("^5pm_dettest^7 tick   %016llx  ticks %u/%i\n", htick, ticktotal, ntick);
}

/*
================================================================================
  pm_recsim -- FTESurf Patch 327.  The anti-cheat plan's experiment E3.

  THE QUESTION.  Phase 3 wants a headless verifier that RE-COMPUTES a submitted
  run's time by replaying its usercmd stream, so the board never has to trust a
  claimed number.  QC build 82 put that stream into the .rec for the first time
  -- one `in` record per SIMULATED MOVE.  E3 asks the only question that can
  honestly be asked before a verifier is written: handed back to the mover that
  produced it, does the stream reproduce the trajectory the same file records?

  THIS IS AN EXPERIMENT AND NOT A VERIFIER, and the difference is the point.  It
  re-simulates and MEASURES.  It does not test zones, does not recompute the
  run's tick, and refuses nothing.  What it exists to produce is the list of
  things a verifier would still be missing -- while the format is young enough
  to change and the board is empty enough that changing it costs nothing.

  THREE ARMS, and the first needs no geometry at all.  That ordering is
  deliberate: if the timing model is wrong then every trajectory number below is
  noise about the wrong question.

    1 TICK ARITHMETIC.  The file stores no frametime.  Build 82 refused the
      column because the duration is exactly
          (next mt - mt)*rate + (next carry - carry)
      i.e. a third copy of a two-copy fact.  That is arithmetic on paper.  Arm 1
      hands the mover the derived duration and asks how many ticks it ran, then
      compares against the file's own movetick delta.  It tests the paper
      against pm_source.c -- and it tests one thing the paper cannot: <carry> is
      written at %.5f, so the reconstruction is fed a ROUNDED number while a
      tick boundary sits 0.01 away.  If that rounding ever costs a tick, this is
      where it shows, and a tick is a rank.

    2 PINNED LOOP.  Run forward continuously, but snap origin and velocity back
      to the recorded sample at every packet boundary.  Errors therefore cannot
      compound, so what this measures is ONE packet's worth of divergence, which
      is the honest way to report a seed that is itself rounded to 0.01 of a
      unit.  Note what is deliberately NOT re-seeded: the mover's carried state
      (pmsourcestate_t -- ducktime, ducked, oldbuttons, groundnormal, stamina,
      surfing, the ladder pair).  None of it is in the .rec and none of it is
      reconstructible from a sample, so re-seeding it would mean inventing it.
      Letting it run on is the only choice that does not fabricate evidence.

    3 OPEN LOOP.  Seed once, never correct.  This is what a verifier actually
      does.  The number that matters here is not the final error -- on a chaotic
      surf path that is unbounded by construction and says nothing -- but WHERE
      it stops being small.

  WHAT IT ASSUMES, LISTED HERE BECAUSE EVERY ONE IS A HOLE IN THE FORMAT AND NOT
  A SHORTCUT IN THE HARNESS.  A .rec pins the map (build 73, `mapcrc`) and the
  zone table (build 81, `zonesrc`/`zonecrc`/`zonerule`).  It does NOT pin:

    - the movement parameters.  gravity, the two accelerates, maxairspeed,
      friction, stopspeed, maxspeed, jumpvelocity.  Patch 313's `*ruleset` key
      publishes a BREACH COUNTER, not the values, so a verifier is told whether
      the ruleset held and never what it was.
    - the player hull.  FTESurf's is Source-shaped (origin at the feet) and is a
      QC constant; pm_dettest's Quake hull would put every trace in the wrong
      place.
    - the physent list.  World only here, as in pm_dettest.  A map with movers
      would need them and their state.
    - the pm_type.  A run that entered noclip is indistinguishable in the file
      from one that did not, which matters because the only recording a config
      can drive to a FINISH is a noclip flight.

  This command takes the first three from the RUNNING SERVER, which is legitimate
  for an experiment against the same build on the same map, and is exactly what a
  real verifier could not do.  Each is a finding, not a caveat.
================================================================================
*/
typedef struct
{
	int    pk;			/* <pk>    packet ordinal, stamped per row */
	int    mt;			/* <mt>    mover tick BEFORE this move */
	float  carry;		/* <carry> sub-tick remainder before it */
	float  mv[3];		/* forward side up, as handed to the mover */
	vec3_t ang;			/* pitch yaw roll */
	int    bt;			/* the three bits pm_source reads: 1 jump 2 duck 4 speed */
	int    nsam;		/* samples that preceded this row in the file */
	int    fl;			/* v9: 1 onground as the engine read it, 2 teleport_time, 4 not WALK; -1 absent */
} recsim_in_t;

typedef struct
{
	float  t;
	vec3_t org;
	vec3_t vel;
	int    fl;			/* SV_RecFlags: 1 onground, 2 ducked, 4 jump, 8 attack, 16 ramp */
	char   txt[6][20];	/* Patch 347: the file's own %.2f text of org and vel */
} recsim_sam_t;

/*
  FTESurf Patch 328 -- the `warp` record, QC build 83's answer to what E3 found.

  A warp is state imposed on the player from OUTSIDE the mover: a teleport, a
  setspeed pad, a push.  Patch 327 measured their absence -- ten of the fifteen
  packets the open loop could not reproduce were trigger_teleport, ~250 u of
  position with velocity carried through exactly, and the recording held the
  CONSEQUENCE with no statement of the EVENT.  So arm 3 died at the first one
  and everything after it was noise.

  APPLYING ONE IS NOT THE SAME AS PINNING.  Arm 2 already snaps to the sample at
  every packet boundary, which papers over a teleport by construction; that is
  why its numbers were good and arm 3's were not.  A warp is applied in BOTH
  arms because it is evidence the file states rather than a correction the
  harness makes -- which is exactly the distinction that makes arm 3 worth
  running at all.
*/
typedef struct
{
	int    pk;
	int    mt;			/* the mover tick the imposition happened at */
	vec3_t org;			/* POST-event state, which is what a verifier re-seeds from */
	vec3_t vel;
	char   kind[16];	/* tele telerel bhop speed push -- and whatever comes next */
	int    row;			/* Patch 347, v9: the `in` row whose move it followed; -1 = v8, bind by <mt> */
	int    fl;			/* v9: 1 = FL_ONGROUND after it */
	int    preseed;		/* Patch 367: written before session N's first row: imposed on its seed */
	qboolean post;		/* Patch 369: written after its row's packet sample -- between packets (`!r`) */
} recsim_warp_t;
/* Patch 369: a `restart` (SV_TimerRestartSeg) -- the zone latches reset.  Bound
   like a warp: the row before it, and whether it came after that packet's sample. */
typedef struct
{
	int      row;
	qboolean post;
} recsim_restart_t;
/* Patch 373: a `ghost` edge.  A client command, so always between packets: it
   acts after its row's packet scan, like a post restart.  While a window is open
   the live timer runs no zone scan and restamps its sweep origin every packet
   (SV_TimerFrame's ghost branch). */
typedef struct
{
	int      row;
	int      on, ticks;
} recsim_ghost_t;
#define RECSIM_GHOST_LAG 1.0f	/* s: an honest client empties its usercmd one RTT after `ghost 1` */

/* Patch 344: the basevelocity carrier (QC build 85, FTESURF-REC 8).  Replayed in
   SV_BaseVelocityFrame's order, before the move it precedes in the file: `pay`
   adds (1 + tickrate*0.5)*bv to velocity, `arm` sets pmove.basevelocity from
   this move on.  Keyed to the `in` row it precedes, not to <mt>: a move that
   runs zero ticks leaves two rows sharing one <mt>. */
typedef struct
{
	int    pk;
	int    mt;			/* pre-move, must equal the following row's <mt> */
	int    row;			/* index of the `in` row this record precedes */
	qboolean pay;
	vec3_t bv;
} recsim_ride_t;

/* Patch 345: the `in` row prints SHORT2ANGLE(wire short) at %.4f.  ANGLE2SHORT
   truncates into a short, so ~46% of rows came back one step (0.0055 deg) low;
   rounding recovers every one.  Wrapped explicitly: a server-set .v_angle can
   sit outside [-180,180). */
/* Patch 347: v9's state records.  `pm` is APPLIED from its row on; `pe` and
   `portal` are CHECKED against what the replay builds and does. */
typedef struct
{
	int    row;
	float  pin[SV_PMPIN_COUNT];
} recsim_pm_t;
typedef struct
{
	int          row;
	unsigned int crc;
} recsim_pe_t;
typedef struct
{
	int    row, n;
	vec3_t org, vel;
} recsim_portal_t;
/* Patch 367: a v10 session and the Multi-Session pause that closed the one before
   it.  That pause's <mt> <carry> are the closing horizon, as `inend`; a retry/load
   pause states the last row's PRE-move counter instead, so it cannot be replayed. */
typedef struct
{
	int    row;			/* its first `in` row */
	int    pmt, pticks;	/* the pause */
	float  pcarry;
	int    n, mt, ticks;	/* `session` */
	float  carry;
	qboolean seeded, stateok;
	vec3_t org, vel;
	int    ground;
	pmsourcestate_t st;
} recsim_sess_t;
#define RECSIM_SESS_TOL 0.0001f	/* the save state's %.4f (sv_saveloc.qc SV_SaveWriteState) */

static short SV_RecSim_AngleShort (float a)
{
	int s = (int)floor(a * (65536/360.0) + 0.5);
	return (short)(unsigned short)(s & 0xffff);
}

static int SV_RecSim_CmpF (const void *a, const void *b)
{	/*3-way on purpose.  See Patch 323: a comparator that can only say "greater"
	  is not an ordering, and this file has paid for that once already.*/
	float x = *(const float*)a, y = *(const float*)b;
	return (x > y) - (x < y);
}

static char *SV_RecSim_Line (char **pp, char *end, char *out, size_t outsz)
{
	char *p = *pp, *o = out;
	if (p >= end)
		return NULL;
	while (p < end && *p != '\n')
	{
		if (*p != '\r' && (size_t)(o - out) < outsz-1)
			*o++ = *p;
		p++;
	}
	if (p < end)
		p++;
	*o = 0;
	*pp = p;
	return out;
}

extern vec3_t pmove_mins, pmove_maxs;
extern cvar_t sv_maxvelocity, pm_trisoup_bevels, pm_rotatedboxhulls, pm_portalcsg_scanall;

//Patch 358: pin slots 10-12.  The trace code reads .ival (com_bih.c, pmovetst.c);
//.value is what the next verify's mismatch check reads.  Not Cvar_ForceSet: these are
//serverinfo, and a pm_verify on a live lobby would push the change to clients.
static cvar_t *const recsim_tracecv[3] = {&pm_trisoup_bevels, &pm_rotatedboxhulls, &pm_portalcsg_scanall};
static void SV_RecSim_TraceCvars (const float *pin)
{
	int i;
	for (i = 0; i < 3; i++)
	{
		recsim_tracecv[i]->value = pin[10+i];
		recsim_tracecv[i]->ival = (int)pin[10+i];
	}
}

/* Patch 349: the verifier's QC hooks (QC build 88), called by name. */
static int SV_RecSim_Step (func_t f, const vec3_t lastp, const vec3_t p, const vec3_t pmaxs)
{
	globalvars_t *pr_globals = PR_globals(svprogfuncs, PR_CURRENT);
	VectorCopy(lastp, G_VECTOR(OFS_PARM0));
	VectorCopy(p, G_VECTOR(OFS_PARM1));
	VectorCopy(pmaxs, G_VECTOR(OFS_PARM2));
	PR_ExecuteProgram(svprogfuncs, f);
	return (int)G_FLOAT(OFS_RETURN);
}

/* Patch 354: every early exit still owes the sweeper a verdict line. */
#define RECSIM_REFUSE(why) do { if (verify) Con_Printf("VERIFY %s REFUSE %s\n", fname, why); } while (0)

static void SV_RecSim_Run (const char *fname, int stopat, qboolean verify)
{
	model_t      *world = sv.state?sv.world.worldmodel:NULL;
	char         *buf, *p, *end, *ln;
	size_t        fsz = 0;
	char          line[4096];		/* Patch 347: a v9 `pmpin`/`pm` line is ~1.3 KB */
	char          mapname[64];
	unsigned int  filecrc = 0;
	qboolean      havecrc = false, inbody;
	float         rate = 0;
	int           instart_mt = -1, instart_run = -1;
	int           nin = 0, nsam = 0, nwarp = 0, nride = 0, i, pass;
	recsim_in_t  *ins = NULL;
	recsim_sam_t *sam = NULL;
	recsim_warp_t*wrp = NULL;
	recsim_ride_t*rid = NULL;
	float         sjrule = -1, sjoff[3] = {0,0,0};
	int           filever = 0;
	float         hdrtick = 0;					/* header `tickrate`: the cash-out's TICK_INTERVAL */
	int           inend_mt = -1;				/* Patch 344: v8 closing horizon */
	float         inend_carry = 0;
	/* Patch 347: v9's exact state */
	float         hdrpin[SV_PMPIN_COUNT];
	int           pinfound = 0, npm = 0, npe = 0, nportal = 0, nlong = 0;
	qboolean      haveseed = false, seedstate_ok = false;
	vec3_t        seedorg = {0,0,0}, seedvel = {0,0,0};
	int           seedground = 0;
	pmsourcestate_t seedstate;
	recsim_pm_t  *pms = NULL;
	recsim_pe_t  *pes = NULL;
	recsim_portal_t *prt = NULL;
	qboolean      exact;
	/* Patch 349: pm_verify */
	int           zs_ev = -1, zs_az = -1, zs_track = 0, zs_startseg = 0, zs_stagerun = 0, zs_twarp = 0;
	qboolean      havezseed = false, haveend = false;
	int           endticks = -1, nresume = 0, nghost = 0, nrestart = 0;
	char          hdrzsrc[32] = "", hdrzcrc[32] = "", hdrzrule[32] = "";
	func_t        vf_step = 0;
	/* Patch 367: v10 sessions */
	recsim_sess_t *ses = NULL;
	recsim_restart_t *rst = NULL;	/* Patch 369 */
	recsim_ghost_t *gho = NULL;		/* Patch 373 */
	const char   *ghostbad = NULL;
	func_t        vf_restart = 0;
	int           nses = 0, npause = 0;
	const char   *sesbad = NULL;		/* a structure this replay cannot follow */
	char          pausewhys[64] = "";

	if (!*fname)
	{
		Con_Printf("pm_recsim <file.rec> [stop after N packets]\n");
		return;
	}
	if (!world || world->loadstate != MLS_LOADED)
	{
		RECSIM_REFUSE("no map loaded");
		Con_Printf(CON_ERROR "pm_recsim: no map loaded.  Re-simulation needs the"
		                     " collision geometry the run was made against --"
		                     " load the recording's own map first.\n");
		return;
	}

	buf = FS_LoadMallocFile(fname, &fsz);
	if (!buf)
	{
		RECSIM_REFUSE("cannot read the file");
		Con_Printf(CON_ERROR "pm_recsim: cannot read \"%s\"\n", fname);
		return;
	}
	mapname[0] = 0;
	memset(hdrpin, 0, sizeof(hdrpin));
	memset(&seedstate, 0, sizeof(seedstate));

	/* Two passes: count, then fill.  Parsed FROM THE GRAMMAR in sv_timer.qc's
	   block comment, not from the writer -- the same rule reccheck.py is written
	   under, and the reason a disagreement between the two would be a finding
	   rather than a typo. */
	for (pass = 0; pass < 2; pass++)
	{
		int cin = 0, csam = 0, cwarp = 0, cride = 0, cpm = 0, cpe = 0, cportal = 0;
		/* Patch 367: from a `pause` to the next `in` row, state records are the
		   next session's floor: they apply from its first row, whatever <row> says. */
		int cses = 0, ppmt = 0, pptk = 0, crst = 0, cgho = 0, gopen = 0;
		float ppc = 0;
		qboolean floorwin = false, openpause = false;
		p = buf; end = buf + fsz; inbody = false;
		while ((ln = SV_RecSim_Line(&p, end, line, sizeof(line))) != NULL)
		{
			if (!*ln)
				continue;
			if (pass == 0 && strlen(ln) >= sizeof(line)-1)
				nlong++;	/* Patch 347: truncated, and said so below */
			if (!inbody)
			{
				if (!strcmp(ln, "begin"))
					{ inbody = true; continue; }
				if (pass == 0)
				{
					if (!strncmp(ln, "map ", 4))
						Q_strncpyz(mapname, ln+4, sizeof(mapname));
					else if (!strncmp(ln, "movetickrate ", 13))
						rate = atof(ln+13);
					else if (!strncmp(ln, "tickrate ", 9))
						hdrtick = atof(ln+9);
					else if (!strncmp(ln, "mapcrc ", 7))
						{ filecrc = (unsigned int)strtoul(ln+7, NULL, 16); havecrc = true; }
					else if (!strncmp(ln, "instart ", 8))
						sscanf(ln+8, "%i %i", &instart_mt, &instart_run);
					else if (!strncmp(ln, "FTESURF-REC ", 12))
						filever = atoi(ln+12);
					else if (!strncmp(ln, "pmpin ", 6))	/* Patch 347 */
						pinfound = SV_PMPinParse(ln+6, hdrpin);
					else if (!strncmp(ln, "zonesrc ", 8))	/* Patch 349 */
						Q_strncpyz(hdrzsrc, ln+8, sizeof(hdrzsrc));
					else if (!strncmp(ln, "zonecrc ", 8))
						Q_strncpyz(hdrzcrc, ln+8, sizeof(hdrzcrc));
					else if (!strncmp(ln, "zonerule ", 9))
						Q_strncpyz(hdrzrule, ln+9, sizeof(hdrzrule));
					/* Patch 328: the randomized start.  Read and REPORTED and
					   never applied -- the recording's first sample is already
					   post-displacement, because SV_TimerStart moves the player
					   before SV_RecOpen and SV_TimerRecFrame samples after both.
					   So the seed carries it for free and the key's job here is
					   to EXPLAIN the offset between the last padding sample and
					   the first run sample, which would otherwise read as an
					   unexplained 2-unit physics step. */
					else if (!strncmp(ln, "startjit ", 9))
						sscanf(ln+9, "%f %f %f %f", &sjrule,
						       &sjoff[0], &sjoff[1], &sjoff[2]);
				}
				continue;
			}
			/* FS_IsSample: a body line starting '-' or a digit is a sample.  So
			   "in " can never be one, which is why the trace needed no escape. */
			if (*ln == '-' || (*ln >= '0' && *ln <= '9'))
			{
				if (pass == 1 && csam < nsam)
				{
					recsim_sam_t *s = &sam[csam];
					float d[9];
					if (sscanf(ln, "%f %f %f %f %f %f %f %f %f %i",
					           &s->t, &d[0], &d[1], &d[2], &d[3], &d[4], &d[5],
					           &d[6], &d[7], &s->fl) == 10)
					{
						VectorSet(s->org, d[0], d[1], d[2]);
						VectorSet(s->vel, d[3], d[4], d[5]);
					}
					else
						s->fl = -1;
					{	/* Patch 347: keep the %.2f text of tokens 1..6 for ARM 4 */
						const char *q = ln;
						int k;
						for (k = 0; k < 7; k++)
						{
							const char *e2 = q;
							while (*e2 && *e2 != ' ') e2++;
							if (k && e2 - q < (int)sizeof(s->txt[0]))
								{ memcpy(s->txt[k-1], q, e2 - q); s->txt[k-1][e2 - q] = 0; }
							q = *e2 ? e2 + 1 : e2;
						}
					}
				}
				csam++;
			}
			else if (!strncmp(ln, "in ", 3))
			{
				if (pass == 1 && cin < nin)
				{
					recsim_in_t *r = &ins[cin];
					int nf = sscanf(ln+3, "%i %i %f %f %f %f %f %f %f %i %i",
					           &r->pk, &r->mt, &r->carry,
					           &r->mv[0], &r->mv[1], &r->mv[2],
					           &r->ang[0], &r->ang[1], &r->ang[2], &r->bt, &r->fl);
					if (nf < 10)
						r->mt = -1;
					if (nf < 11)
						r->fl = -1;	/* v6-v8: no <fl> column */
					r->nsam = csam;
				}
				cin++;
				floorwin = false;
			}
			/* Patch 328.  Before this, an unknown record was not "skipped with a
			   count" -- it was INVISIBLE: the two-pass allocator counted only
			   samples and `in` rows, so nothing in the output would have revealed
			   that the file carried records this harness ignored.  The tool that
			   found the teleport problem could not see the record written to fix
			   it until this branch existed. */
			else if (!strncmp(ln, "warp ", 5))
			{
				if (pass == 1 && cwarp < nwarp)
				{
					recsim_warp_t *w = &wrp[cwarp];
					float d[6];
					char  kb[32];
					w->row = -1;
					w->fl = -1;
					if (filever >= 9)
					{	/* Patch 347: v9, bound by <row> */
						if (sscanf(ln+5, "%i %i %i %31s %f %f %f %f %f %f %i",
						           &w->pk, &w->mt, &w->row, kb, &d[0], &d[1], &d[2],
						           &d[3], &d[4], &d[5], &w->fl) != 11)
							w->mt = -1;
					}
					else if (sscanf(ln+5, "%i %i %31s %f %f %f %f %f %f",
					           &w->pk, &w->mt, kb, &d[0], &d[1], &d[2],
					           &d[3], &d[4], &d[5]) != 9)
						w->mt = -1;
					if (w->mt >= 0)
					{
						VectorSet(w->org, d[0], d[1], d[2]);
						VectorSet(w->vel, d[3], d[4], d[5]);
						Q_strncpyz(w->kind, kb, sizeof(w->kind));
					}
					w->preseed = 0;
					w->post = !floorwin && cin > 0 && csam > ins[cin-1].nsam;
					if (floorwin && w->mt >= 0)
						{ w->row = cin; w->preseed = openpause ? cses + 1 : cses; }
				}
				cwarp++;
			}
			else if (!strncmp(ln, "ride ", 5))
			{
				if (pass == 1 && cride < nride)
				{
					recsim_ride_t *d = &rid[cride];
					char kb[16];
					d->row = cin;
					if (sscanf(ln+5, "%i %i %15s %f %f %f", &d->pk, &d->mt, kb,
					           &d->bv[0], &d->bv[1], &d->bv[2]) == 6
					    && (!strcmp(kb, "arm") || !strcmp(kb, "pay")))
						d->pay = !strcmp(kb, "pay");
					else
						d->mt = -1;
				}
				cride++;
			}
			else if (!strncmp(ln, "inend ", 6) && pass == 0)
			{
				if (sscanf(ln+6, "%i %f", &inend_mt, &inend_carry) != 2)
					inend_mt = -1;
			}
			/* Patch 349: the timer latches the start packet left (QC build 88). */
			else if (!strncmp(ln, "zseed ", 6) && pass == 0)
			{
				const char *q = ln + 6;
				havezseed = true;
				while (*q)
				{
					int v;
					char name[32];
					if (sscanf(q, "%31[^=]=%i", name, &v) == 2)
					{
						if (!strcmp(name, "evzone"))		zs_ev = v;
						else if (!strcmp(name, "azone"))	zs_az = v;
						else if (!strcmp(name, "track"))	zs_track = v;
						else if (!strcmp(name, "startseg"))	zs_startseg = v;
						else if (!strcmp(name, "stagerun"))	zs_stagerun = v;
						else if (!strcmp(name, "twarp"))	zs_twarp = v;
					}
					while (*q && *q != ' ') q++;
					while (*q == ' ') q++;
				}
			}
			else if (!strncmp(ln, "end ", 4) && pass == 0)
				{ haveend = true; endticks = atoi(ln+4); }
			else if ((!strncmp(ln, "resume ", 7) || !strncmp(ln, "retry ", 6)) && pass == 0)
				nresume++;
			else if (!strncmp(ln, "ghost ", 6))
			{
				int on = -1, tk = -1;
				if (sscanf(ln+6, "%i %i", &on, &tk) != 2 || (on != 0 && on != 1))
				{
					if (pass == 0 && !ghostbad)
						ghostbad = "a malformed `ghost`";
				}
				else if (pass == 0 && on == gopen && !ghostbad)
					ghostbad = on ? "`ghost 1` inside an open window" : "`ghost 0` with no window open";
				else if (pass == 1 && cgho < nghost)
				{
					gho[cgho].row = cin - 1;
					gho[cgho].on = on;
					gho[cgho].ticks = tk;
				}
				if (on == 0 || on == 1)
					gopen = on;
				cgho++;
			}
			else if (!strncmp(ln, "restart ", 8))
			{
				if (pass == 1 && crst < nrestart)
				{
					rst[crst].row = cin - 1;
					rst[crst].post = cin > 0 && csam > ins[cin-1].nsam;
				}
				crst++;
			}
			/* Patch 347: v9's exact seed and its three state tracks.  Patch 367: the
			   first is the run's; each later one belongs to the session before it. */
			else if (!strncmp(ln, "seed ", 5) && (cses ? pass == 1 : pass == 0))
			{
				const char *q = ln + 5;
				vec3_t so, sv;
				int k, sg;
				if (sscanf(q, "%f %f %f %f %f %f %i", &so[0], &so[1], &so[2],
				           &sv[0], &sv[1], &sv[2], &sg) == 7)
				{
					for (k = 0; k < 7 && *q; k++)
					{
						while (*q && *q != ' ') q++;
						while (*q == ' ') q++;
					}
					if (!cses && !haveseed)
					{
						haveseed = true;
						VectorCopy(so, seedorg);
						VectorCopy(sv, seedvel);
						seedground = sg;
						seedstate_ok = SV_PMStateParse(q, &seedstate);
					}
					else if (cses && cses <= nses && !ses[cses-1].seeded)
					{
						recsim_sess_t *s = &ses[cses-1];
						s->seeded = true;
						VectorCopy(so, s->org);
						VectorCopy(sv, s->vel);
						s->ground = sg;
						s->stateok = SV_PMStateParse(q, &s->st);
					}
				}
			}
			else if (!strncmp(ln, "pause ", 6))
			{
				char why[16];
				floorwin = true;
				if (openpause && pass == 0 && !sesbad)
					sesbad = "a second `pause` before its `session`";
				if (gopen && pass == 0 && !ghostbad)
					ghostbad = "a ghost window open across a `pause` (a park un-ghosts first)";
				openpause = true;
				if (sscanf(ln+6, "%i %f %i %15s", &ppmt, &ppc, &pptk, why) != 4)
				{
					if (pass == 0 && !sesbad)
						sesbad = "a malformed `pause`";
				}
				else if (pass == 0)
				{
					npause++;
					if (strlen(pausewhys) + strlen(why) + 2 < sizeof(pausewhys))
					{
						if (*pausewhys)
							Q_strncatz(pausewhys, " ", sizeof(pausewhys));
						Q_strncatz(pausewhys, why, sizeof(pausewhys));
					}
					if (!sesbad && (!strcmp(why, "retry") || !strcmp(why, "load")))
						sesbad = "a cold rewind (`pause retry|load`): the move before it has no stated duration";
					else if (!sesbad && strcmp(why, "drop") && strcmp(why, "rotate") && strcmp(why, "server"))
						sesbad = "a `pause` reason this replay does not know";
				}
			}
			else if (!strncmp(ln, "session ", 8))
			{
				int sn, smt, stk;
				float scy;
				if (sscanf(ln+8, "%i %i %f %i", &sn, &smt, &scy, &stk) != 4)
				{
					if (pass == 0 && !sesbad)
						sesbad = "a malformed `session`";
				}
				else if (!openpause)
				{
					if (pass == 0 && !sesbad)
						sesbad = "a `session` with no `pause` before it";
				}
				else
				{
					if (pass == 1 && cses < nses)
					{
						recsim_sess_t *s = &ses[cses];
						s->row = cin;
						s->pmt = ppmt; s->pcarry = ppc; s->pticks = pptk;
						s->n = sn; s->mt = smt; s->carry = scy; s->ticks = stk;
					}
					cses++;
				}
				openpause = false;
			}
			else if (!strncmp(ln, "pm ", 3))
			{
				if (pass == 1 && cpm < npm)
				{
					recsim_pm_t *m = &pms[cpm];
					const char *q = ln + 3;
					int k, pk;
					memcpy(m->pin, cpm ? pms[cpm-1].pin : hdrpin, sizeof(m->pin));
					if (sscanf(q, "%i %i", &pk, &m->row) != 2)
						m->row = -1;
					else if (floorwin)
						m->row = cin;
					for (k = 0; k < 2 && *q; k++)
					{
						while (*q && *q != ' ') q++;
						while (*q == ' ') q++;
					}
					SV_PMPinParse(q, m->pin);
				}
				cpm++;
			}
			else if (!strncmp(ln, "pe ", 3))
			{
				if (pass == 1 && cpe < npe)
				{
					int pk;
					if (sscanf(ln+3, "%i %i %u", &pk, &pes[cpe].row, &pes[cpe].crc) != 3)
						pes[cpe].row = -1;
					else if (floorwin)
						pes[cpe].row = cin;
				}
				cpe++;
			}
			else if (!strncmp(ln, "portal ", 7))
			{
				if (pass == 1 && cportal < nportal)
				{
					recsim_portal_t *o = &prt[cportal];
					int pk;
					if (sscanf(ln+7, "%i %i %i %f %f %f %f %f %f", &pk, &o->row, &o->n,
					           &o->org[0], &o->org[1], &o->org[2],
					           &o->vel[0], &o->vel[1], &o->vel[2]) != 9)
						o->row = -1;
					else if (floorwin)
						o->row = cin;
				}
				cportal++;
			}
		}
		if (pass == 0)
		{
			nin = cin; nsam = csam; nwarp = cwarp; nride = cride;
			npm = cpm; npe = cpe; nportal = cportal;
			nses = cses;
			nrestart = crst;
			nghost = cgho;
			if (gopen && !ghostbad)
				ghostbad = "a ghost window open at the finish";
			if (openpause && !sesbad)
				sesbad = "a `pause` no `session` answers (the run is still parked)";
			if (!nin)
			{
				RECSIM_REFUSE("no input trace (recorder before QC build 82, or a lifted stage)");
				Con_Printf(CON_ERROR "pm_recsim: \"%s\" carries no `in` records."
				                     "  It predates QC build 82, or it is a lifted"
				                     " stage (SV_StageLine drops them on purpose --"
				                     " a slice rebases and <mt> is per MAP).\n", fname);
				FS_FreeFile(buf);
				return;
			}
			ins = Z_Malloc(sizeof(*ins) * nin);
			sam = Z_Malloc(sizeof(*sam) * (nsam?nsam:1));
			wrp = Z_Malloc(sizeof(*wrp) * (nwarp?nwarp:1));
			rid = Z_Malloc(sizeof(*rid) * (nride?nride:1));
			pms = Z_Malloc(sizeof(*pms) * (npm?npm:1));
			pes = Z_Malloc(sizeof(*pes) * (npe?npe:1));
			prt = Z_Malloc(sizeof(*prt) * (nportal?nportal:1));
			ses = Z_Malloc(sizeof(*ses) * (nses?nses:1));
			rst = Z_Malloc(sizeof(*rst) * (nrestart?nrestart:1));
			gho = Z_Malloc(sizeof(*gho) * (nghost?nghost:1));
		}
	}
	FS_FreeFile(buf);

	if (rate <= 0)
	{
		RECSIM_REFUSE("no movetickrate in the header");
		Con_Printf(CON_ERROR "pm_recsim: no `movetickrate` in the header, so the"
		                     " duration of a move cannot be reconstructed.\n");
		Z_Free(ins); Z_Free(sam); Z_Free(wrp); Z_Free(rid);
		Z_Free(pms); Z_Free(pes); Z_Free(prt); Z_Free(ses); Z_Free(rst); Z_Free(gho);
		return;
	}
	/* Patch 367: v10 is read -- each session is reseeded from its own `seed` on
	   its restarted counter.  Anything this replay cannot follow is refused. */
	if (filever > 10)
		sesbad = "a newer format (this verifier reads FTESURF-REC 10)";
	else if (!sesbad && npause && filever < 10)
		sesbad = "a `pause` under a header below FTESURF-REC 10";
	else if (!sesbad && !npause && filever == 10)
		sesbad = "FTESURF-REC 10 with no `pause` (the header is 10 exactly when one is written)";
	for (i = 0; !sesbad && i < nses; i++)
		if (ses[i].row >= nin)
			sesbad = "a session with no `in` row after it";
	if (sesbad)
	{
		RECSIM_REFUSE(sesbad);
		Con_Printf(CON_ERROR "pm_recsim: \"%s\" is FTESURF-REC %i: %s.\n", fname, filever, sesbad);
		Z_Free(ins); Z_Free(sam); Z_Free(wrp); Z_Free(rid);
		Z_Free(pms); Z_Free(pes); Z_Free(prt); Z_Free(ses); Z_Free(rst); Z_Free(gho);
		return;
	}
	if (hdrtick <= 0)
		hdrtick = rate;

	Con_Printf("^5pm_recsim^7  %s\n", fname);
	Con_Printf("  header    map \"%s\"  movetickrate %g  instart %i %i\n",
	           mapname, rate, instart_mt, instart_run);
	if (havecrc)
		Con_Printf("  map pin   file %08x  world %08x  -- %s\n", filecrc,
		           (unsigned int)world->checksum,
		           filecrc == (unsigned int)world->checksum ? "^2MATCH^7"
		           : "^1DIFFERENT MAP -- every number below is meaningless^7");
	else
		Con_Printf("  map pin   ^3none in the header^7 (predates build 73)\n");
	Con_Printf("  body      %i in rows, %i samples, %i packets, %i warps, %i rides\n",
	           nin, nsam, nin?(ins[nin-1].pk - ins[0].pk + 1):0, nwarp, nride);
	if (inend_mt >= 0)
		Con_Printf("  inend     %i %.5f -- the final move's duration is stated\n",
		           inend_mt, inend_carry);

	/* Patch 347: v9.  EXACT means the file states the seed, the carried mover
	   state and every pinned input -- then the replay is the mover, not a model
	   of it, and any sample it disagrees with is the file's fault. */
	exact = haveseed && seedstate_ok && pinfound == SV_PMPIN_COUNT;
	for (i = 0; exact && i < nses; i++)	/* Patch 367: every session must restate its state */
		if (!ses[i].seeded || !ses[i].stateok)
			exact = false;
	if (nses)
		Con_Printf("  sessions  %i after the first (pauses: %s) -- each reseeded from its own `seed`\n",
		           nses, pausewhys);
	if (filever >= 9 || pinfound || haveseed)
		Con_Printf("  v9 state  pin %i/%i names, seed %s, %i pm, %i pe, %i portal -- %s\n",
		           pinfound, SV_PMPIN_COUNT,
		           haveseed ? (seedstate_ok ? "exact" : "^3carried state incomplete^7") : "^3absent^7",
		           npm, npe, nportal,
		           exact ? "^2EXACT REPLAY^7" : "^3approximate (the file lacks state)^7");
	if (pinfound && hdrpin[0] != PMSRC_VERSION)
		Con_Printf("  ^1pmsrcver %g in the file, %i in this engine -- a different mover;"
		           " an exact replay cannot vouch for this file^7\n", hdrpin[0], PMSRC_VERSION);
	if (nlong)
		Con_Printf("  ^1%i line(s) over %i bytes were truncated^7\n", nlong, (int)sizeof(line)-1);

	/* Patch 328.  The two v7 facts, printed whether or not they are there --
	   because "this file predates the record" and "nothing imposed any state on
	   this run" are the same bytes without a version, and that distinction is
	   the whole reason build 83 bumped one. */
	if (sjrule >= 0)
		Con_Printf("  startjit  rule %g u, applied %.4f %.4f %.4f%s\n",
		           sjrule, sjoff[0], sjoff[1], sjoff[2],
		           (sjoff[0]==0 && sjoff[1]==0)
		           ? "  ^3(blocked -- nothing was applied)^7" : "");
	else
		Con_Printf("  startjit  ^3no key^7 -- the rule was off, or this file"
		           " predates build 83\n");
	/* Patch 344: E5 measured the v7 form false (surf_trance: zero warps, ten
	   boosters).  v7 can only vouch for what it can express; v8 adds the carrier.
	   Neither expresses a linked_portal_door crossing, which the mover commits. */
	if (!nwarp)
	{
		if (filever >= 8)
			Con_Printf("  warps     none, and the file is v%i: no map entity wrote"
			           " origin or velocity on this run.\n", filever);
		else if (filever == 7)
			Con_Printf("  warps     none, and the file is v7: nothing THIS FORMAT"
			           " CAN EXPRESS imposed state.\n"
			           "            v7 has no record for a basevelocity carrier,"
			           " so a booster here is invisible.\n");
		else
			Con_Printf("  warps     none, and the file is v%i -- which says"
			           " NOTHING.  The writer did not exist.\n"
			           "            An open loop across a teleport here will"
			           " diverge and that is the format, not the mover.\n",
			           filever);
	}
	if (filever >= 8)
	{
		int npay = 0;
		for (i = 0; i < nride; i++)
			npay += rid[i].pay;
		Con_Printf("  rides     %i arm, %i pay; cash-out scale 1 + %g*0.5 (header"
		           " tickrate -- not yet pinned as the carrier's own)\n",
		           nride - npay, npay, hdrtick);
	}
	else
		Con_Printf("  rides     ^3v%i cannot carry them^7 -- a booster in this file"
		           " is unexplained displacement\n", filever);

	/* Patch 349: what v1 of the verifier will vouch for, and what it will not.
	   REFUSE is "cannot say", never a judgement on the run. */
	if (verify)
	{
		const char *refuse = NULL;
		char zbuf[128], zhere[128];
		/* Patch 356: a newer format is refused above (Patch 364/367), before any
		   unknown record could be skipped into a PASS. */
		if (!exact)
			refuse = "not exact: no seed or no full pin (recorder before QC build 87, or engine before Patch 346)";
		else if (!haveend || inend_mt < 0)
			refuse = "unfinished: no `inend`/`end`";
		else if (nresume)
			refuse = "a save-state resume or retry";
		else if (ghostbad)
			refuse = ghostbad;
		else if (nrestart && !(svprogfuncs && (vf_restart = PR_FindFunction(svprogfuncs, "SV_VerifyRestart", PR_ANY))))
			refuse = "a stage restart (these progs have no SV_VerifyRestart, Patch 369)";
		else if (!havecrc || filecrc != (unsigned int)world->checksum)
			refuse = "a different map";
		else if (hdrpin[0] != PMSRC_VERSION)
			refuse = "a different mover (pmsrcver)";
		else if (!havezseed)
			refuse = "no `zseed` (recorder before QC build 88)";
		else if (nlong)
			refuse = "a truncated line";
		else if (instart_mt != instart_run)
			refuse = "the start tick is not the trace's horizon";
		else if (!svprogfuncs || !(vf_step = PR_FindFunction(svprogfuncs, "SV_VerifyStep", PR_ANY)))
			refuse = "these progs have no verifier hooks (QC build 88)";
		else
		{
			func_t fpin = PR_FindFunction(svprogfuncs, "SV_VerifyZonePin", PR_ANY);
			func_t fbeg = PR_FindFunction(svprogfuncs, "SV_VerifyBegin", PR_ANY);
			globalvars_t *pr_globals = PR_globals(svprogfuncs, PR_CURRENT);
			*zhere = 0;
			if (fpin)
			{
				PR_ExecuteProgram(svprogfuncs, fpin);
				Q_strncpyz(zhere, PR_GetString(svprogfuncs, G_INT(OFS_RETURN)), sizeof(zhere));
			}
			Q_snprintfz(zbuf, sizeof(zbuf), "%s %s %s", hdrzsrc, hdrzcrc, hdrzrule);
			if (!fpin || !fbeg)
				refuse = "these progs have no verifier hooks (QC build 88)";
			else if (strcmp(zbuf, zhere))
			{
				Con_Printf("  zone pin  file \"%s\"  here \"%s\"\n", zbuf, zhere);
				refuse = "a different zone table or rule";
			}
			else
			{
				pr_globals = PR_globals(svprogfuncs, PR_CURRENT);
				G_FLOAT(OFS_PARM0) = zs_ev;
				G_FLOAT(OFS_PARM1) = zs_az;
				G_FLOAT(OFS_PARM2) = zs_track;
				G_FLOAT(OFS_PARM3) = zs_stagerun;
				G_FLOAT(OFS_PARM4) = zs_startseg;
				PR_ExecuteProgram(svprogfuncs, fbeg);
			}
		}
		if (refuse)
		{
			Con_Printf("VERIFY %s REFUSE %s\n", fname, refuse);
			Z_Free(ins); Z_Free(sam); Z_Free(wrp); Z_Free(rid);
			Z_Free(pms); Z_Free(pes); Z_Free(prt); Z_Free(ses); Z_Free(rst); Z_Free(gho);
			return;
		}
	}

	/* ---- the arms -------------------------------------------------------- */
	{
		movevars_t   savemv = movevars;
		playermove_t savepm = pmove;
		float  savetv[3];	/* Patch 358: this server's trace cvars */
		int    saveti[3];
		float *eo = Z_Malloc(sizeof(float) * (nin+1));
		float *ev = Z_Malloc(sizeof(float) * (nin+1));
		int    neo = 0;
		int    tick_exact = 0, tick_off = 0, tick_worst = 0, ndur = 0;
		int    open_first_1u = -1, open_first_01u = -1;
		float  open_last = 0;
		int    seeded = -1, mode, nbad = 0;
		int    wcur = 0, wapplied = 0;		/* Patch 328 */
		int    rcur = 0, rapplied = 0, rskew = 0;	/* Patch 344 */
		vec3_t carrier;
		/* Patch 347 */
		float  gamespeed = 1.0f, svmaxvel = sv_maxvelocity.value;
		wedict_t *proxy = NULL;
		qboolean scratchproxy = false;
		int    pmcur = 0, pecur = 0, portcur = 0, k;
		unsigned int pecrc = 0;
		qboolean pehave = false;
		int    pe_ok = 0, pe_bad = 0, pe_first = -1;
		unsigned int pe_file = 0, pe_ours = 0;
		int    port_ok = 0, port_bad = 0, port_first = -1;
		int    x_ok = 0, x_bad = 0, x_shown = 0, x_first = -1;
		/* Patch 349 */
		vec3_t vlastp;
		int    vfin_row = -1, vfin_ticks = -1, vcancel_row = -1, vrearm_row = -1;
		int    band[4] = {0,0,0,0};		/* <=0.02 u, <=0.1, <=1, worse */
		/* Patch 367: the session being replayed, and what its boundaries found */
		int    scur = 0, sbase_mt = instart_run, sbase_ticks = 0;
		int    rscur = 0, nrsapplied = 0;	/* Patch 369: restarts */
		/* Patch 373: ghost windows */
		int    gcur = 0, nghskip = 0, gwin_mt = 0;
		qboolean ghosting = false, gempty = false;
		int    gtick_row = -1, gtick_file = 0, gtick_trace = 0;
		int    ginput_row = -1, ginput_late = 0;
		int    sclk_n = -1, sclk_trace = 0, sclk_pause = 0, sclk_sess = 0;
		int    sjmp_n = -1, sjmp_row = -1;
		float  sjmp_o = 0, sjmp_v = 0;
		/* The mover's carried state at the top of a run: every field at its
		   natural initial value.  A verifier gets this for free at the START of a
		   recording and can never recover it anywhere else, which is the whole
		   reason ARM 2 may not re-seed it mid-run. */
		pmsourcestate_t zerostate;
		memset(&zerostate, 0, sizeof(zerostate));
		for (k = 0; k < 3; k++)
		{
			savetv[k] = recsim_tracecv[k]->value;
			saveti[k] = recsim_tracecv[k]->ival;
		}

		memset(&pmove, 0, sizeof(pmove));
		pmove.numphysent = 1;
		pmove.physents[0].model = world;
		pmove.skipent = -1;
		pmove.pm_type = PM_NORMAL;
		pmove.surfacefriction = 1.0f;
		/* FTESurf's hull, which is Source-shaped: the origin is at the FEET.
		   pm_dettest's Quake hull (-24/+32) would place every trace wrong. */
		VectorSet(pmove.player_mins, -16, -16,  0);
		VectorSet(pmove.player_maxs,  16,  16, 62);

		movevars.physicsmode = PHYSMODE_SOURCE;
		movevars.ticrate     = rate;		/* the FILE's, not the server's */
		if (exact)
		{	/* Patch 347: the physics the file says ran, not this server's. */
			SV_PMPinApply(hdrpin);
			SV_RecSim_TraceCvars(hdrpin);
			pmove.player_mins[2] = 0;
			pmove.player_maxs[2] = movevars.standheight;
			gamespeed = hdrpin[3];
			svmaxvel = hdrpin[9];
			Con_Printf("  movevars  ^2from the file's pin^7: grav %g fric %g acc %g airacc %g"
			           " maxair %g maxspd %g jump %g tic %g\n",
			           movevars.gravity, movevars.friction, movevars.accelerate,
			           movevars.airaccelerate, movevars.maxairspeed, movevars.maxspeed,
			           movevars.jumpvelocity, movevars.ticrate);
			if (hdrpin[10] != savetv[0] || hdrpin[11] != savetv[1] || hdrpin[12] != savetv[2])
				Con_Printf("  ^3trace cvars: the file ran %g %g %g, this server %g %g %g --"
				           " replaying with the file's^7\n",
				           hdrpin[10], hdrpin[11], hdrpin[12], savetv[0], savetv[1], savetv[2]);
			for (i = 0; i < sv.allocated_client_slots; i++)
				if (svs.clients[i].state >= cs_spawned && svs.clients[i].edict)
					{ proxy = (wedict_t*)svs.clients[i].edict; break; }
			/* Patch 349: a verifier process has no clients.  A scratch edict --
			   unlinked, non-solid, a player's default dimension bits -- stands in
			   for the recorded player; with no client there is nobody to leave out. */
			if (!proxy)
			{
				proxy = (wedict_t*)ED_Alloc(svprogfuncs, false, 0);
				if (proxy)
				{
					proxy->v->solid = SOLID_NOT;
					proxy->xv->dimension_hit = proxy->xv->dimension_solid = pr_global_struct->dimension_default;
					scratchproxy = true;
				}
			}
			Con_Printf("  physents  %s\n", !proxy ? "^3world only -- no edict to build them around^7"
			           : scratchproxy ? "built as the server builds them, around a scratch player edict"
			           : "built as the server builds them, around a spawned player who is left out");
		}
		else
		{
			Con_Printf("  hull      %g %g %g .. %g %g %g\n",
			           pmove.player_mins[0], pmove.player_mins[1], pmove.player_mins[2],
			           pmove.player_maxs[0], pmove.player_maxs[1], pmove.player_maxs[2]);
			Con_Printf("  movevars  ^3taken from the running server -- the .rec does not"
			           " carry them^7\n");
			Con_Printf("            grav %g fric %g stop %g acc %g airacc %g maxair %g"
			           " maxspd %g jump %g\n",
			           movevars.gravity, movevars.friction, movevars.stopspeed,
			           movevars.accelerate, movevars.airaccelerate,
			           movevars.maxairspeed, movevars.maxspeed, movevars.jumpvelocity);
		}

		Con_Printf("\n^5ARM 1^7  tick arithmetic -- no geometry, no seed.\n");
		Con_Printf("^5ARM 2^7  pinned loop -- org/vel snapped to the sample each packet.\n");
		Con_Printf("^5ARM 3^7  open loop -- seeded once, never corrected.\n\n");

		/* TWO PASSES, and they must be two.  The pinned loop and the open loop
		   are different simulations of the same input: one of them re-seeds and
		   the other does not, so a single pass that merely READ the error at each
		   boundary without snapping would be one simulation reported twice under
		   two names.  (It was, in the first cut of this command.)
		   mode 0 = open, mode 1 = pinned. */
		for (mode = 0; mode < (verify ? 1 : 2); mode++)	/* Patch 349: the verifier is the open loop */
		{
			/* Seed from the sample immediately before the first `in` row.  NOT
			   from `instart`, which is a HORIZON and not a state: it says which
			   mover tick the recording opened on and carries no position. */
			PMSrc_LoadState(&zerostate);
			pmove.onground = false;
			VectorClear(pmove.origin);
			VectorClear(pmove.velocity);
			pmcur = pecur = portcur = 0;
			pehave = false;
			if (exact)
			{	/* Patch 347: the file's own exact seed and carried state. */
				SV_PMPinApply(hdrpin);
				SV_RecSim_TraceCvars(hdrpin);
				pmove.player_mins[2] = 0;
				pmove.player_maxs[2] = movevars.standheight;
				gamespeed = hdrpin[3];
				svmaxvel = hdrpin[9];
				PMSrc_LoadState(&seedstate);
				VectorCopy(seedorg, pmove.origin);
				VectorCopy(seedvel, pmove.velocity);
				pmove.onground = (seedground & 1) != 0;
				seeded = ins[0].nsam - 1;
				if (!mode)
					Con_Printf("  seed      ^2exact^7 org %.9g %.9g %.9g  vel %.9g %.9g %.9g"
					           "  ducked %i  carry %.9g\n",
					           seedorg[0], seedorg[1], seedorg[2], seedvel[0], seedvel[1],
					           seedvel[2], seedstate.ducked, seedstate.msec_carry);
			}
			else if (ins[0].nsam > 0)
			{
				recsim_sam_t *s = &sam[ins[0].nsam - 1];
				VectorCopy(s->org, pmove.origin);
				VectorCopy(s->vel, pmove.velocity);
				pmove.onground = (s->fl & 1) != 0;
				seeded = ins[0].nsam - 1;
				if (!mode)
					Con_Printf("  seed      sample %i  t %.4f  org %.2f %.2f %.2f"
					           "  vel %.2f %.2f %.2f\n",
					           seeded, s->t, s->org[0], s->org[1], s->org[2],
					           s->vel[0], s->vel[1], s->vel[2]);
			}
			else if (!mode)
				Con_Printf("  seed      ^1no sample precedes the first `in` row^7\n");
			if (!exact)
				pmove.msec_carry = ins[0].carry;
			wcur = 0;			/* Patch 328: each pass replays the warps */
			/* Patch 347: a v9 warp at row -1 came between open and the first
			   command, so it is imposed on the seed. */
			for (; filever >= 9 && wcur < nwarp && wrp[wcur].mt >= 0 && wrp[wcur].row < 0; wcur++)
			{
				VectorCopy(wrp[wcur].org, pmove.origin);
				VectorCopy(wrp[wcur].vel, pmove.velocity);
				if (wrp[wcur].fl >= 0)
					pmove.onground = (wrp[wcur].fl & 1) != 0;
			}
			VectorCopy(pmove.origin, vlastp);	/* Patch 349: SV_TimerFrame's run_t_lastorg */
			rcur = 0;			/* Patch 344: and the carrier */
			VectorClear(carrier);
			scur = 0;			/* Patch 367: and the session */
			rscur = 0;			/* Patch 369: and the restarts */
			ghosting = false;	/* Patch 373: a window opened before the first command */
			for (gcur = 0; gcur < nghost && gho[gcur].row < 0; gcur++)
				ghosting = gho[gcur].on;
			gwin_mt = ins[0].mt;
			gempty = false;
			sbase_mt = instart_run;
			sbase_ticks = 0;

			for (i = 0; i < nin; i++)
			{
				recsim_in_t *r = &ins[i];
				float dt;
				int   wantticks, next_mt;
				float next_carry;

				if (stopat > 0 && r->pk - ins[0].pk >= stopat)
					break;

				/* Patch 367: a session starts on this row.  Its pause parked the
				   clock and the state the last one ended in; the resume must take
				   up both, to the save state's %.4f.  Then reseed, as at `begin`. */
				while (scur < nses && ses[scur].row == i)
				{
					recsim_sess_t *s = &ses[scur];
					int   clk = sbase_ticks + (s->pmt - sbase_mt);
					float dorg = 0, dvel = 0;
					for (k = 0; k < 3; k++)
					{
						if (fabs(pmove.origin[k] - s->org[k]) > dorg)
							dorg = fabs(pmove.origin[k] - s->org[k]);
						if (fabs(pmove.velocity[k] - s->vel[k]) > dvel)
							dvel = fabs(pmove.velocity[k] - s->vel[k]);
					}
					if (!mode)
					{
						Con_Printf("  session %i  row %i: parked at clock %i (pause %i, session %i)",
						           s->n, i, clk, s->pticks, s->ticks);
						if (s->seeded)
							Con_Printf(", resumed %.6g u / %.6g u/s from the replay's state\n", dorg, dvel);
						else
							Con_Printf(", ^3no seed^7\n");
						if (sclk_n < 0 && (clk != s->pticks || s->ticks != s->pticks))
							{ sclk_n = s->n; sclk_trace = clk; sclk_pause = s->pticks; sclk_sess = s->ticks; }
						if (sjmp_n < 0 && s->seeded && (dorg > RECSIM_SESS_TOL || dvel > RECSIM_SESS_TOL))
							{ sjmp_n = s->n; sjmp_row = i; sjmp_o = dorg; sjmp_v = dvel; }
					}
					if (s->seeded)
					{
						if (exact)
							PMSrc_LoadState(&s->st);
						VectorCopy(s->org, pmove.origin);
						VectorCopy(s->vel, pmove.velocity);
						pmove.onground = (s->ground & 1) != 0;
					}
					else if (r->nsam > 0)
					{
						recsim_sam_t *sm = &sam[r->nsam - 1];
						VectorCopy(sm->org, pmove.origin);
						VectorCopy(sm->vel, pmove.velocity);
						pmove.onground = (sm->fl & 1) != 0;
					}
					if (!exact)
						pmove.msec_carry = s->carry;
					VectorClear(carrier);
					sbase_mt = s->mt;
					sbase_ticks = s->ticks;
					scur++;
					for (; wcur < nwarp && wrp[wcur].preseed == scur; wcur++)
						if (wrp[wcur].mt >= 0)
						{
							VectorCopy(wrp[wcur].org, pmove.origin);
							VectorCopy(wrp[wcur].vel, pmove.velocity);
							if (wrp[wcur].fl >= 0)
								pmove.onground = (wrp[wcur].fl & 1) != 0;
							if (!mode)
								wapplied++;
						}
					VectorCopy(pmove.origin, vlastp);	/* SV_TimerFrame restamps run_t_lastorg every frame */
				}

				if (exact)
				{
					/* Patch 347: a changed pin is in force from its row on. */
					for (; pmcur < npm && pms[pmcur].row <= i; pmcur++)
						if (pms[pmcur].row >= 0)
						{
							SV_PMPinApply(pms[pmcur].pin);
							SV_RecSim_TraceCvars(pms[pmcur].pin);
							pmove.player_mins[2] = 0;
							pmove.player_maxs[2] = movevars.standheight;
							gamespeed = pms[pmcur].pin[3];
							svmaxvel = pms[pmcur].pin[9];
						}
					/* WPhys_CheckVelocity, which SV_RunCmd runs BEFORE PreThink:
					   NaN to zero, then each axis to sv_maxvelocity (Source mode). */
					for (k = 0; k < 3; k++)
					{
						if (IS_NAN(pmove.velocity[k]))
							pmove.velocity[k] = 0;
						if (pmove.velocity[k] > svmaxvel)
							pmove.velocity[k] = svmaxvel;
						else if (pmove.velocity[k] < -svmaxvel)
							pmove.velocity[k] = -svmaxvel;
					}
				}

				/* PATCH 344: the carrier, in SV_BaseVelocityFrame's order -- the
				   cash-out, then the hand-over -- before this row's move. */
				for (; rcur < nride && rid[rcur].row <= i; rcur++)
				{
					recsim_ride_t *d = &rid[rcur];
					if (d->mt < 0)
						continue;
					if (!mode)
					{
						rapplied++;
						if (d->mt != r->mt)
							rskew++;
					}
					if (d->pay)
						VectorMA(pmove.velocity, 1 + hdrtick*0.5f, d->bv, pmove.velocity);
					else
						VectorCopy(d->bv, carrier);
				}
				VectorCopy(carrier, pmove.basevelocity);

				if (r->mt < 0)
					continue;

				/* Patch 347: the ground flag the engine read (v9 <fl>). */
				if (exact && r->fl >= 0)
					pmove.onground = (r->fl & 1) != 0;

				/* THE DURATION, DERIVED from the successor row -- or, for the
				   last row, from `inend` (Patch 344; v8).  Without it the final
				   move, the one the finish is latched on, cannot be run. */
				if (scur < nses && ses[scur].row == i+1)	/* Patch 367: the pause closes this session */
					{ next_mt = ses[scur].pmt; next_carry = ses[scur].pcarry; }
				else if (i+1 < nin && ins[i+1].mt >= 0)	/* Patch 352: a malformed successor is no successor */
					{ next_mt = ins[i+1].mt; next_carry = ins[i+1].carry; }
				else if (inend_mt >= 0)
					{ next_mt = inend_mt; next_carry = inend_carry; }
				else
					break;
				dt = (next_mt - r->mt) * rate + (next_carry - r->carry);
				wantticks = next_mt - r->mt;
				if (!mode)
					ndur++;

				pmove.cmd.msec        = dt * 1000.0f;
				pmove.cmd.forwardmove = (int)r->mv[0];	/* Patch 345: the wire field is int, not short */
				pmove.cmd.sidemove    = (int)r->mv[1];
				pmove.cmd.upmove      = (int)r->mv[2];
				pmove.cmd.buttons     = ((r->bt & 1) ? BUTTON_JUMP  : 0) |
				                        ((r->bt & 2) ? BUTTON_DUCK  : 0) |
				                        ((r->bt & 4) ? BUTTON_SPEED : 0);
				pmove.cmd.angles[0]   = SV_RecSim_AngleShort(r->ang[0]);
				pmove.cmd.angles[1]   = SV_RecSim_AngleShort(r->ang[1]);
				pmove.cmd.angles[2]   = SV_RecSim_AngleShort(r->ang[2]);
				VectorCopy(r->ang, pmove.angles);

				if (exact && proxy)
				{	/* Patch 347: physents as SV_RunCmd builds them, then CHECKED
					   against the digest the server published for this row. */
					int oldpf = proxy->xv->pmove_flags;
					unsigned int crc;
					pmove.numphysent = 1;
					pmove.physents[0].model = world;
					for (k = 0; k < 3; k++)
					{
						pmove_mins[k] = pmove.origin[k] - 256;
						pmove_maxs[k] = pmove.origin[k] + 256;
					}
					proxy->xv->pmove_flags = oldpf & ~PMF_LADDER;
					AddAllLinksToPmove(&sv.world, proxy);
					pmove.onladder = ((int)proxy->xv->pmove_flags & PMF_LADDER) != 0;
					proxy->xv->pmove_flags = oldpf;
					pmove.world = &sv.world;
					crc = SV_PhysentDigest();
					for (; pecur < npe && pes[pecur].row <= i; pecur++)
						if (pes[pecur].row >= 0)
							{ pecrc = pes[pecur].crc; pehave = true; }
					if (!mode && pehave)
					{
						if (crc == pecrc)
							pe_ok++;
						else if (pe_bad++ == 0)
							{ pe_first = i; pe_file = pecrc; pe_ours = crc; }
					}
				}

				PM_PlayerMove(exact ? gamespeed : 1.0f);

				if (exact)
				{	/* Patch 347: crossings the file says this move made, CHECKED. */
					int want = 0;
					qboolean same = true;
					for (; portcur < nportal && prt[portcur].row <= i; portcur++)
						if (prt[portcur].row == i)
						{
							want += prt[portcur].n;
							same = same && VectorCompare(prt[portcur].org, pmove.origin)
							            && VectorCompare(prt[portcur].vel, pmove.velocity);
						}
					if (!mode && (want || pmove.portalcrossings))
					{
						if (want == pmove.portalcrossings && same)
							port_ok++;
						else if (port_bad++ == 0)
						{
							port_first = i;
							Con_Printf("  portal DISAGREES  row %i: file %i crossing(s), replay %i\n",
							           i, want, pmove.portalcrossings);
						}
					}
				}

				/*
				  PATCH 328: APPLY ANY STATE THE FILE SAYS WAS IMPOSED HERE.

				  A warp is stamped with the mover tick as it stood AFTER the
				  engine advanced it for the command whose touch fired -- while an
				  `in` row carries the counter BEFORE its own move.  So a warp
				  belongs after the row whose post-move tick reaches it, which is
				  the row whose successor states that tick.  Walked with a cursor
				  rather than searched, because both sequences are monotone and a
				  search would invite an off-by-one of exactly the kind this
				  command has already paid for once.

				  IN BOTH ARMS, and that is the point.  Arm 2 snaps to the sample
				  at every packet boundary and so papered over teleports by
				  construction; arm 3 never corrects and died at the first one.
				  Applying the record is neither -- it is reading what the file
				  says happened, which is the difference between a harness that
				  hides a gap and a format that closes it.
				*/
				while (wcur < nwarp)
				{
					if (wrp[wcur].mt < 0)
						{ wcur++; continue; }	/* a malformed row, already noted */
					if (filever >= 9)
					{	/* Patch 347: v9 binds by <row> */
						if (wrp[wcur].row > i)
							break;
						if (wrp[wcur].post)
							break;	/* Patch 369: after this packet's scan and sample, below */
					}
					else if (wrp[wcur].mt > next_mt)
						break;
					VectorCopy(wrp[wcur].org, pmove.origin);
					VectorCopy(wrp[wcur].vel, pmove.velocity);
					if (exact && wrp[wcur].fl >= 0)
						pmove.onground = (wrp[wcur].fl & 1) != 0;
					/* Patch 349: SV_TimerWarped moves the sweep origin for these,
					   gated on run_teleport_warp exactly as the handlers are. */
					if (zs_twarp && (!strcmp(wrp[wcur].kind, "tele") || !strcmp(wrp[wcur].kind, "telerel")
					                 || !strcmp(wrp[wcur].kind, "bhop") || !strcmp(wrp[wcur].kind, "zone")))	/* Patch 369: zone, per the grammar */
						VectorCopy(wrp[wcur].org, vlastp);
					if (!mode)
					{
						wapplied++;
						if (wapplied <= 6)
							Con_Printf("  warp  row %i  mt %i  %s  -> org %.2f"
							           " %.2f %.2f\n", i, wrp[wcur].mt,
							           wrp[wcur].kind, wrp[wcur].org[0],
							           wrp[wcur].org[1], wrp[wcur].org[2]);
					}
					wcur++;
				}

				/* Patch 369: a restart inside the move (a teleport back to the stage's
				   start) resets the zone latches before this packet's scan. */
				for (; rscur < nrestart && rst[rscur].row <= i && !rst[rscur].post; rscur++)
					if (verify && !mode && vf_restart)
						{ PR_ExecuteProgram(svprogfuncs, vf_restart); nrsapplied++; }

				/* Patch 373: inside a window the body is flown by nobody.  Once the
				   client has emptied its usercmd it stays empty until `ghost 0`. */
				if (ghosting && !mode)
				{
					qboolean empty = !r->mv[0] && !r->mv[1] && !r->mv[2] && !(r->bt & 7);
					if (empty)
						gempty = true;
					else if (ginput_row < 0 && (gempty || (r->mt - gwin_mt) * rate >= RECSIM_GHOST_LAG))
						{ ginput_row = i; ginput_late = r->mt - gwin_mt; }
				}

				/* Patch 349: the live timer runs once per PACKET, in PostThink,
				   after the packet's last move and its touches.  So does this. */
				if (verify && !mode && ghosting && (i+1 >= nin || ins[i+1].pk != r->pk))
				{	/* Patch 373: no zone scan; the sweep origin follows the body */
					nghskip++;
					VectorCopy(pmove.origin, vlastp);
				}
				else if (verify && !mode && (i+1 >= nin || ins[i+1].pk != r->pk))
				{
					int code = SV_RecSim_Step(vf_step, vlastp, pmove.origin, pmove.player_maxs);
					if (code == 1 && vfin_row < 0)
						{ vfin_row = i; vfin_ticks = sbase_ticks + (next_mt - sbase_mt); }
					else if (code == 2 && vcancel_row < 0)
						vcancel_row = i;
					else if (code == 3 && vrearm_row < 0)
						vrearm_row = i;
					VectorCopy(pmove.origin, vlastp);
				}

				/* ARM 1.  The mover's own tick count against the file's delta.
				   Counted on the open pass only -- it is a property of the
				   arithmetic, not of the trajectory, so it is the same on both
				   and counting twice would double every number. */
				if (!mode)
				{
					if ((int)pmove.ticksrun == wantticks)
						tick_exact++;
					else
					{
						tick_off++;
						if (abs((int)pmove.ticksrun - wantticks) > tick_worst)
							tick_worst = abs((int)pmove.ticksrun - wantticks);
						if (tick_off <= 4)
							Con_Printf("  arm1 row %i pk %i: mover ran %u ticks,"
							           " file says %i  (msec %.4f carry %.5f)\n",
							           i, r->pk, pmove.ticksrun, wantticks,
							           dt*1000.0f, r->carry);
					}
				}

				/* End of this packet?  Then a sample states where the run was.
				   INDEXING, because this was wrong once and the wrongness was
				   legible: a row's `nsam` is how many samples PRECEDED it, so the
				   group {nsam == k} is bracketed by sample[k-1] before and
				   sample[k] after.  Comparing against [k-1] -- the seed -- scored
				   the simulation against the state it started from, and the tell
				   was a velocity error whose median was EXACTLY 8.0000 u/s, which
				   is one tick of gravity at 800 u/s^2 and 0.01 s.  A median that
				   lands on a round physical constant is a systematic offset, not
				   a distribution.  The last group has no sample after it (the run
				   ended inside that packet) and is skipped by `nsam < nsam`. */
				if ((i+1 >= nin || ins[i+1].nsam != r->nsam)
				    && r->nsam >= 0 && r->nsam < nsam)
				{
					recsim_sam_t *s = &sam[r->nsam];
					vec3_t d;
					float  derr, verr;

					VectorSubtract(pmove.origin, s->org, d);
					derr = VectorLength(d);
					VectorSubtract(pmove.velocity, s->vel, d);
					verr = VectorLength(d);

					if (exact && !mode)
					{	/* Patch 347, ARM 4: the replay printed the way the writer
						   prints it must BE the file's text, all six numbers. */
						char tb[32];
						int bad = -1;
						for (k = 0; k < 6 && bad < 0; k++)
						{
							Q_snprintfz(tb, sizeof(tb), "%.2f", k < 3 ? pmove.origin[k] : pmove.velocity[k-3]);
							if (strcmp(tb, s->txt[k]))
								bad = k;
						}
						if (bad < 0)
							x_ok++;
						else
						{
							if (!x_bad)
								x_first = i;
							x_bad++;
							if (x_shown++ < 6)
								Con_Printf("  arm4 MISMATCH  row %i  packet %i  t %.4f  %s%c: file %s, replay %s\n",
								           i, r->pk, s->t, bad < 3 ? "org" : "vel", "xyz"[bad%3],
								           s->txt[bad], tb);
						}
					}

					if (mode)
					{
						/* ARM 2: record, THEN snap.  The carried mover state
						   (pmsourcestate_t) is deliberately NOT snapped -- none
						   of it is in the .rec and a sample cannot imply it, so
						   re-seeding it would be inventing evidence. */
						if (neo < nin)
						{
							eo[neo] = derr;
							ev[neo] = verr;
							neo++;
						}
						/* THE TAIL IS THE FINDING, not the median.  A median at
						   the file's own printing floor says the mover reproduces
						   the run; it says nothing about the handful of packets
						   that do not, and those are what a verifier would have
						   to refuse or explain.  Banded and located rather than
						   summarised into a maximum. */
						if      (derr <= 0.02f) band[0]++;
						else if (derr <= 0.1f)  band[1]++;
						else if (derr <= 1.0f)  band[2]++;
						else
						{
							band[3]++;
							if (nbad < 8)
								Con_Printf("  arm2 DIVERGED  row %i  packet %i  "
								           "t %.4f  org err %.2f u  vel err %.2f u/s\n",
								           i, r->pk, s->t, derr, verr);
							nbad++;
						}
						VectorCopy(s->org, pmove.origin);
						VectorCopy(s->vel, pmove.velocity);
						pmove.onground = (s->fl & 1) != 0;
					}
					else
					{
						/* ARM 3: never corrected. */
						open_last = derr;
						if (open_first_01u < 0 && derr > 0.1f) open_first_01u = i;
						if (open_first_1u  < 0 && derr > 1.0f) open_first_1u  = i;
					}
				}

				/* Patch 369: between packets (`!r`, SV_ZoneMove) -- written after this
				   packet's sample, so after its scan and its sample check here too. */
				for (; filever >= 9 && wcur < nwarp && wrp[wcur].post && wrp[wcur].row <= i; wcur++)
				{
					if (wrp[wcur].mt < 0)
						continue;
					VectorCopy(wrp[wcur].org, pmove.origin);
					VectorCopy(wrp[wcur].vel, pmove.velocity);
					if (exact && wrp[wcur].fl >= 0)
						pmove.onground = (wrp[wcur].fl & 1) != 0;
					if (zs_twarp && (!strcmp(wrp[wcur].kind, "tele") || !strcmp(wrp[wcur].kind, "telerel")
					                 || !strcmp(wrp[wcur].kind, "bhop") || !strcmp(wrp[wcur].kind, "zone")))
						VectorCopy(wrp[wcur].org, vlastp);
					if (!mode)
						wapplied++;
				}
				for (; rscur < nrestart && rst[rscur].row <= i; rscur++)
					if (verify && !mode && vf_restart)
						{ PR_ExecuteProgram(svprogfuncs, vf_restart); nrsapplied++; }
				/* Patch 373: a ghost edge after this packet.  Its <ticks> is
				   SV_TimerTicks there: the counter after this move. */
				for (; gcur < nghost && gho[gcur].row <= i; gcur++)
				{
					int tk = sbase_ticks + (next_mt - sbase_mt);
					if (!mode && gtick_row < 0 && gho[gcur].ticks != tk)
						{ gtick_row = i; gtick_file = gho[gcur].ticks; gtick_trace = tk; }
					ghosting = gho[gcur].on;
					if (ghosting)
						{ gwin_mt = next_mt; gempty = false; }
					else
						VectorCopy(pmove.origin, vlastp);	/* SV_GhostSet -> SV_TimerWarped */
				}
			}
		}

		if (nrestart && verify)
			Con_Printf("  restarts  %i of %i applied to the zone latches (SV_VerifyRestart)\n", nrsapplied, nrestart);
		if (nghost && verify)
			Con_Printf("  ghost     %i edge(s), %i packet scan(s) skipped while detached\n", nghost, nghskip);

		/* ARM 1 ------------------------------------------------------------- */
		Con_Printf("^5ARM 1^7  %i moves: %i exact, %i off (worst by %i tick%s)\n",
		           ndur, tick_exact, tick_off, tick_worst, tick_worst==1?"":"s");
		if (!tick_off && ndur)
			Con_Printf("        so the derived duration IS the duration: build 82's"
			           " refusal to store a frametime column holds, and %%.5f on"
			           " <carry> is enough precision to survive the round trip.\n");

		/* ARM 2 / 3 --------------------------------------------------------- */
		if (neo)
		{
			float *so = Z_Malloc(sizeof(float)*neo);
			memcpy(so, eo, sizeof(float)*neo);
			qsort(so, neo, sizeof(float), SV_RecSim_CmpF);
			Con_Printf("^5ARM 2^7  %i packets: origin error median %.4f  p90 %.4f  max %.4f u\n",
			           neo, so[neo/2], so[(neo*9)/10], so[neo-1]);
			memcpy(so, ev, sizeof(float)*neo);
			qsort(so, neo, sizeof(float), SV_RecSim_CmpF);
			Con_Printf("        velocity error median %.4f  p90 %.4f  max %.4f u/s\n",
			           so[neo/2], so[(neo*9)/10], so[neo-1]);
			Con_Printf("        (the sample itself is written at %%.2f, so +-0.005 u"
			           " and +-0.005 u/s is the floor this can possibly reach)\n");
			Con_Printf("        bands: %i at the printing floor (<=0.02 u), %i <=0.1,"
			           " %i <=1, ^3%i diverged^7\n",
			           band[0], band[1], band[2], band[3]);
			Z_Free(so);

			Con_Printf("^5ARM 3^7  open loop: first packet past 0.1 u at row %i,"
			           " past 1 u at row %i, last %.4f u\n",
			           open_first_01u, open_first_1u, open_last);

			/* PATCH 328: ARM 3 IS THE ONE THAT ANSWERS THE FORMAT QUESTION.
			   Arm 2 is pinned and would look fine over a hole; only an open loop
			   can say whether the file contains enough to cross one.  -1 means
			   it never diverged at all. */
			if (nwarp)
				Con_Printf("        %i warp record%s applied.  If arm 3 now runs"
				           " to the end, the format carries\n        what E3 said"
				           " it did not -- the EVENT and not only its"
				           " consequence.\n",
				           wapplied, wapplied==1?"":"s");
			else if (filever < 7)
				Con_Printf("        ^3and there were no warp records to apply."
				           "  On a map with teleports arm 3 CANNOT\n        run to"
				           " the end from a v%i file, whatever the mover does."
				           "^7\n", filever);
			/* Patch 344.  rskew counts records whose <mt> disagrees with the row
			   they precede -- the grammar says they must match, so nonzero is a
			   writer or ordering fault, not a physics result. */
			if (nride)
				Con_Printf("        %i ride record%s applied (%i off their row's"
				           " <mt>).\n", rapplied, rapplied==1?"":"s", rskew);
		}
		else if (!verify)	/* Patch 349: the verifier runs the open loop only */
			Con_Printf("^5ARM 2/3^7  no packet boundary carried a sample to compare against.\n");
		if (exact)
		{	/* Patch 347 */
			Con_Printf("^5ARM 4^7  exact: %i of %i packets reproduce the file's own"
			           " %%.2f text in all six numbers%s\n", x_ok, x_ok + x_bad,
			           x_bad ? "" : " -- ^2the replay IS the recording^7");
			Con_Printf("        physents: %i rows match the recorded digest, %i do not",
			           pe_ok, pe_bad);
			if (pe_bad)
				Con_Printf(" (first row %i: file %06x, replay %06x)", pe_first, pe_file, pe_ours);
			Con_Printf("%s\n", proxy ? "" : "  ^3(not built)^7");
			Con_Printf("        portals: %i crossing move(s) agree, %i disagree",
			           port_ok, port_bad);
			if (port_bad)
				Con_Printf(" (first row %i)", port_first);
			Con_Printf("\n");
		}

		if (inend_mt < 0)
			Con_Printf("\n  NOT MEASURED, because the file cannot say: the duration of"
			           " the FINAL move.\n  %i rows give %i durations -- the last row"
			           " has no successor to difference\n  against, and it is the move"
			           " the finish is latched on.\n", nin, nin-1);

		/* Patch 349: the verdict.  PASS: the replay IS the file, and the live
		   timer's own zone scan finishes it on its last packet at its stated
		   tick.  HOLD: something disagrees -- a reason for a human, never an
		   accusation. */
		if (verify)
		{
			char why[160];
			*why = 0;
			/* Patch 367: a session boundary is named before the divergence it causes;
			   the clock check is counter arithmetic and needs no trajectory. */
			if (sclk_n >= 0)
				Q_snprintfz(why, sizeof(why), "session %i: the trace parks the clock at %i, the pause says %i, the session %i",
				            sclk_n, sclk_trace, sclk_pause, sclk_sess);
			else if (sjmp_n >= 0 && (!x_bad || x_first >= sjmp_row))
				Q_snprintfz(why, sizeof(why), "session %i resumes %.4g u / %.4g u/s from where the last one parked",
				            sjmp_n, sjmp_o, sjmp_v);
			/* Patch 373: counter arithmetic and file content, like the session checks */
			else if (gtick_row >= 0)
				Q_snprintfz(why, sizeof(why), "ghost at row %i: the file says tick %i, the trace %i", gtick_row, gtick_file, gtick_trace);
			else if (ginput_row >= 0)
				Q_snprintfz(why, sizeof(why), "ghost: input at row %i, %i ticks into a detached window", ginput_row, ginput_late);
			else if (x_bad)
				Q_snprintfz(why, sizeof(why), "state: %i packet(s) differ, first at row %i", x_bad, x_first);
			else if (pe_bad)
				Q_snprintfz(why, sizeof(why), "physents: %i row(s) differ, first at row %i", pe_bad, pe_first);
			else if (port_bad)
				Q_snprintfz(why, sizeof(why), "portals: %i move(s) disagree, first at row %i", port_bad, port_first);
			else if (vcancel_row >= 0)
				Q_snprintfz(why, sizeof(why), "a cancel zone is crossed at row %i", vcancel_row);
			else if (vfin_row < 0)
				Q_snprintfz(why, sizeof(why), "no finish: the zones never end this run");
			else if (vfin_row != nin-1)
				Q_snprintfz(why, sizeof(why), "finish at row %i but the file runs to row %i", vfin_row, nin-1);
			else if (vfin_ticks != endticks)
				Q_snprintfz(why, sizeof(why), "ticks: the zones say %i, the file says %i", vfin_ticks, endticks);
			if (vrearm_row >= 0)
				Con_Printf("  note      the replay re-enters this track's START at row %i\n", vrearm_row);
			if (*why)
				Con_Printf("VERIFY %s HOLD %s\n", fname, why);
			else
				Con_Printf("VERIFY %s PASS ticks %i rows %i\n", fname, vfin_ticks, nin);
		}

		Z_Free(eo); Z_Free(ev);
		if (scratchproxy)
			ED_Free(svprogfuncs, (edict_t*)proxy);
		movevars = savemv;
		pmove = savepm;
		for (k = 0; k < 3; k++)
		{
			recsim_tracecv[k]->value = savetv[k];
			recsim_tracecv[k]->ival = saveti[k];
		}
	}

	Z_Free(ins);
	Z_Free(sam);
	Z_Free(wrp);
	Z_Free(rid);
	Z_Free(pms);
	Z_Free(pes);
	Z_Free(prt);
	Z_Free(ses);
	Z_Free(rst);
	Z_Free(gho);
}

static void SV_RecSim_f (void)
{
	SV_RecSim_Run(Cmd_Argv(1), atoi(Cmd_Argv(2)), false);
}

//FTESurf Patch 349: the verifier -- pm_recsim's exact open loop plus the live
//timer's zone scan, ending in one VERIFY line.
static void SV_RecVerify_f (void)
{
	SV_RecSim_Run(Cmd_Argv(1), 0, true);
}

/* FTESurf Patch 346: print what each client's mover was handed on its last move --
   the text QC copies into a recording -- plus the epoch, physent digest and portal
   count.  `pm_pin [slot]`. */
static void SV_PMPin_f (void)
{
	char text[2048];
	int i, only = Cmd_Argc() > 1 ? atoi(Cmd_Argv(1)) : -1;
	for (i = 0; i < sv.allocated_client_slots; i++)
	{
		client_t *cl = &svs.clients[i];
		if (cl->state < cs_spawned || (only >= 0 && i != only))
			continue;
		Con_Printf("pm_pin %i \"%s\"  epoch %u  physcrc %06x  portalx %u\n",
		           i, cl->name, cl->pmepoch, cl->physcrc, cl->portalx);
		if (!cl->pmepoch)
			continue;
		if (SV_PMPinText(cl->pmpin, text, sizeof(text)))
			Con_Printf("  pin   %s\n", text);
		if (SV_PMStateText(&cl->pmsrc, text, sizeof(text)))
			Con_Printf("  state %s\n", text);
		if (cl->portalx)
			Con_Printf("  last crossing: org %.9g %.9g %.9g  vel %.9g %.9g %.9g\n",
			           cl->portalorg[0], cl->portalorg[1], cl->portalorg[2],
			           cl->portalvel[0], cl->portalvel[1], cl->portalvel[2]);
	}
}

/*
==================
SV_InitOperatorCommands
==================
*/
void SV_InitOperatorCommands (void)
{
#ifndef SERVERONLY
	if (isDedicated)
#endif
	{
		Cmd_AddCommandD ("quit", SV_Quit_f, "Exits the engine back to desktop.");
		Cmd_AddCommandD ("say", SV_ConSay_f, "Send a chat message to everyone on the server.");
		Cmd_AddCommand ("sayone", SV_ConSayOne_f);
		Cmd_AddCommand ("tell", SV_ConSayOne_f);
		Cmd_AddCommand ("serverinfo", SV_Serverinfo_f);	//commands that conflict with client commands.
		Cmd_AddCommand ("serverinfoblob", SV_Serverinfo_f);	//commands that conflict with client commands.
		Cmd_AddCommand ("user", SV_User_f);

		Cmd_AddCommandD ("god", SV_God_f, "Makes you immune to damage.");
#ifdef QUAKESTATS
		Cmd_AddCommand ("give", SV_Give_f);
#endif
		Cmd_AddCommandD ("noclip", SV_Noclip_f, "Disables clipping, allowing you to fly through the level.");

		Cmd_AddCommand ("download", SV_Download_f);
	}

#ifdef SUBSERVERS
	Cvar_Register(&sv_autooffload, "server control variables");
#endif
	Cvar_Register(&sv_cheats, "Server Permissions");
	Cvar_Register(&sv_mapcompletion, "server control variables");	//FTESurf Patch 215
	if (COM_CheckParm ("-cheats"))
	{
		Cvar_Set(&sv_cheats, "1");
	}

	Cmd_AddCommand ("fraglogfile", SV_Fraglogfile_f);

	//ask clients to take a remote screenshot
	Cmd_AddCommand ("snap", SV_Snap_f);
	Cmd_AddCommand ("snapall", SV_SnapAll_f);

	//various punishments
	Cmd_AddCommandD ("kick", SV_Kick_f, "Removes a player from the server, provide the name or IP of the desired player.");
	Cmd_AddCommand ("clientkick", SV_KickSlot_f);
	Cmd_AddCommand ("renameclient", SV_ForceName_f);
	Cmd_AddCommandD ("mute", SV_Mute_f, "Mutes the player (no voice or chat), shaming them.");
	Cmd_AddCommandD ("stealthmute", SV_StealthMute_f, "Mutes the player, without telling them, while pretending that their messages are still being broadcast. For use against people that would escalate on expiry or externally.");
	Cmd_AddCommandD ("cuff", SV_Cuff_f, "Slap handcuffs on the player, preventing them from being able to attack.");
	Cmd_AddCommandD ("cripple", SV_CripplePlayer_f, "Block the player's ability to move.");
	Cmd_AddCommandD ("ban", SV_BanClientIP_f, "Block the player's IP, preventing them from connecting. Also kicks them.");
	Cmd_AddCommandD ("banname", SV_BanClientIP_f, "Legacy compat, please use ban.");	//legacy dupe-name cruft

	Cmd_AddCommandD ("banlist", SV_BanList_f, "Displays a list of every banned player on the server.");	//shows only bans, not other penalties
	Cmd_AddCommandD ("unban", SV_Unfilter_f, "Unbans or removes an IP Address from the penality list, alias to removeip.");	//merely renamed.

	Cmd_AddCommand ("addip", SV_FilterIP_f);
	Cmd_AddCommandD ("removeip", SV_Unfilter_f, "Removes an IP Address from the penality list.");
	Cmd_AddCommandD ("listip", SV_FilterList_f, "Displays a list of ever player the server has penalties for.");	//shows all penalties
	Cmd_AddCommand ("writeip", SV_WriteIP_f);

	Cmd_AddCommand ("floodprot", SV_Floodprot_f);

	Cmd_AddCommandD ("status", SV_Status_f, "Prints info about the current server.");

	Cmd_AddCommand ("sv", SV_SendGameCommand_f);
	Cmd_AddCommand ("mod", SV_SendGameCommand_f);

#ifdef SUBSERVERS
	Cmd_AddCommand ("ssv", MSV_SubServerCommand_f);
	Cmd_AddCommand ("ssv_all", MSV_SubServerCommand_f);
	Cmd_AddCommandAD ("mapcluster", MSV_MapCluster_f, SV_Map_c, "Sets this server up as a cluster-server gateway. Additional processes will be used to host individual maps. If an argument is given then that will be the name of the map that new clients will initially be directed to. This can also be used for single-player to off-load nearly all server functions - use the 'ssv' command to direct each subserver.");
#endif
	Cmd_AddCommand ("killserver", SV_KillServer_f);
	Cmd_AddCommandD ("precaches", SV_PrecacheList_f, "Displays a list of current server precaches.");
	Cmd_AddCommandAD ("map", SV_Map_f, SV_Map_c, "Begins a new game on the specified map.");
	Cmd_AddCommandD ("mapfrom", SV_MapFrom_f, "nettest (P26 Part 2): mapfrom <game> <mapname> — mount that game then load ITS copy of a same-named map. <game> = css|cs|hl|hl2|cod|cod2 (or a full \"steam:Game/dir\" spec). e.g. mapfrom css de_dust2. The css/hl/cod... aliases wrap this.");
	Cmd_AddCommandAD ("mapedit", SV_Map_f, SV_Map_c, "Loads the named map without any gamecode active.");
#ifdef Q3SERVER
	Cmd_AddCommandAD ("spmap", SV_Map_f, SV_Map_c, "Loads a map in single-player mode, for Quake III compat.");
	Cmd_AddCommandAD ("spdevmap", SV_Map_f, SV_Map_c, "Loads a map in single-player developer mode (sv_cheats 1), for Quake III compat.");
#endif
	Cmd_AddCommandAD ("devmap", SV_Map_f, SV_Map_c, "Loads a map in developer mode (sv_cheats 1), for Quake III compat.");
	Cmd_AddCommandAD ("gamemap", SV_Map_f, SV_Map_c, NULL);
	Cmd_AddCommandAD ("changelevel", SV_Map_f, SV_Map_c, "Continues the game on a different map. The current map can be reentered later when a starting position for the new map is specified as a second argument.");
	Cmd_AddCommandD ("map_restart", SV_Map_f, "Restarts the server and reloads the map while flushing level cache, for general use and Quake III compat.");	//from q3.
	Cmd_AddCommandD ("listmaps", SV_MapList_f, "Displays a list of installed maps.");
	Cmd_AddCommandD ("maplist", SV_MapList_f, "Displays a list of installed maps.");
	Cmd_AddCommandD ("maps", SV_MapList_f, "Displays a list of installed maps.");
#if defined(HAVE_LEGACY) && defined(HAVE_SERVER)
	Cmd_AddCommandD ("check_maps", SV_redundantcommand_f, "Obsolete, specific to ktpro. Modern mods should use search_begin instead.");
	Cmd_AddCommandD ("sys_select_timeout", SV_redundantcommand_f, "Redundant - server will throttle according to tick rates instead.");
	Cmd_AddCommandD ("sv_downloadchunksperframe", SV_redundantcommand_f, "Flawed - downloads instead proceed at the client's drate (or rate) setting instead of ignoring it entirely.");
	Cmd_AddCommandD ("sv_speedcheck", SV_redundantcommand_f, "Obsolete - movetime is instead metered over time, instead of randomly kicking everyone due to dodgy timer hardware on the server.");
	Cmd_AddCommandD ("sv_enableprofile", SV_redundantcommand_f, "Debug setting that is not implemented.");
	Cmd_AddCommandD ("sv_progsname", SV_redundantcommand_f, "Use sv_progs instead.");
	Cmd_AddCommandD ("download_map_url", SV_redundantcommand_f, "Redundant - individual maps will probably download faster than the user can open a browser at the given url.");
	Cmd_AddCommandD ("sv_progtype", SV_redundantcommand_f, "Use sv_progs instead. Using to block .dll loading is insufficient with buggy clients around.");
#endif

	Cmd_AddCommandD ("heartbeat", SV_Heartbeat_f, "Sends an update or ping to the master server so the current server can remain listed.");

	Cmd_AddCommand ("localinfo", SV_Localinfo_f);
	Cmd_AddCommandAD ("gamedir", SV_Gamedir_f, SV_Gamedir_c, "Change the current gamedir.");
	Cmd_AddCommandAD ("sv_gamedir", SV_Gamedir, SV_Gamedir_c, "Change the gamedir reported to clients, without changing any actual paths on the server.");
	Cmd_AddCommand ("sv_settimer", SV_SetTimer_f);
	Cmd_AddCommand ("stuffcmd", SV_StuffToClient_f);

	Cmd_AddCommand ("pin_save", SV_Pin_Save_f);
	Cmd_AddCommand ("pin_reload", SV_Pin_Reload_f);
	Cmd_AddCommand ("pin_delete", SV_Pin_Delete_f);
	Cmd_AddCommand ("pin_add", SV_Pin_Add_f);

	Cmd_AddCommand("sv_meminfo", SV_MemInfo_f);

	//FTESurf Patch 322 -- the anti-cheat plan's E4.  Needs a map loaded.
	Cmd_AddCommandD("pm_dettest", SV_DetTest_f,
	                "FTESurf: cross-build determinism falsifier (plan E4).  "
	                "pm_dettest [traces] [ticks].  Runs a fixed, integer-derived "
	                "input set through the collision tree, libm and the mover, and "
	                "prints three separate hashes so that a difference between two "
	                "builds says WHICH of the three moved.  Run it on two "
	                "architectures with the same map and diff the three lines.");

	//FTESurf Patch 327 -- the anti-cheat plan's E3.  Needs the run's own map.
	Cmd_AddCommandD("pm_recsim", SV_RecSim_f,
	                "FTESurf: re-simulate a recording's input trace (plan E3).  "
	                "pm_recsim <file.rec> [stop after N packets].  Feeds the `in` "
	                "records of a FTESURF-REC 6+ file back through the mover that "
	                "produced them, applying its warp/ride/inend records, and "
	                "measures whether the trajectory comes back. "
	                "Measures only -- it tests no zone and refuses no run.");

	//FTESurf Patch 349
	Cmd_AddCommandD("pm_verify", SV_RecVerify_f,
	                "FTESurf: pm_verify <file.rec>.  Replays a finished FTESURF-REC 9 or 10 "
	                "recording exactly on this map and asks the timer's own zone "
	                "scan where it finishes.  Prints VERIFY <file> PASS|HOLD|REFUSE "
	                "<reason>.");

	//FTESurf Patch 346
	Cmd_AddCommandD("pm_pin", SV_PMPin_f,
	                "FTESurf: pm_pin [slot].  Prints what each client's mover was "
	                "handed on its last move (the pin a recording states), its "
	                "epoch, the physent digest and the portal-crossing count.");

//	Cmd_AddCommand ("reallyevilhack", SV_ReallyEvilHack_f);
}

#endif
