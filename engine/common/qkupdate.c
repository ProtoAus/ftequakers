/*
 * Quakers in-game delta updater.
 *
 * Content is distributed as a content-addressed tree: a JSON manifest listing every file's
 * path/size/blake2b-256 hash, plus immutable blobs at objects/<hh>/<hash>. The Rust launcher
 * (C:\FTEQuake\launcher) installs and repairs that tree. What it cannot do is tell a player
 * who is already in the game that a new build exists -- they would have to quit and re-run it.
 * So the engine checks for itself when the main menu opens, and applies SMALL deltas in place.
 *
 * Deliberately NOT a general installer. First-time installs, full repairs and anything that
 * touches the engine binaries stay with the launcher; this refuses and says so. The caps
 * (update_max_files / update_max_mb) are the blast radius, not a performance tuning knob.
 *
 * Why not FTE's own package manager (client/m_download.c): it models *packages*, its hash keys
 * are hard-wired to sha1/sha512, and PM_SignatureOkay refuses arbitrary loose files. Adopting
 * it would mean repackaging 4,243 loose files into pk3s and losing per-file dedup. Its PARTS
 * are reused here instead -- FS_Hash_ValidateWrites (hoisted to fs.c), HTTP_CL_Get,
 * DL_CreateThread, VFSPIPE_Open, the Cmd_AddTimer bounce.
 *
 * THREADING. Three threads touch this file and the rules are not negotiable:
 *   - download workers        run notifycomplete. They may ONLY read the dl_download and
 *                             Cmd_AddTimer a copy of what they found. No cvars, no globals.
 *   - WG_LOADER               parses JSON and hashes files off disk. No cvars, no `va()`.
 *   - main (Cbuf_Execute)     owns every field of `qku` and every Cvar_ForceSet.
 * Cmd_AddTimer is the hop in both directions: it is FTE_Atomic_Insert'd so it is callable from
 * any thread, and it drains from Cbuf_Execute, which is a clean top-level point on both the
 * client and the dedicated server. notifycomplete, by contrast, fires from deep inside
 * HTTP_CL_Think with arbitrary engine state on the stack.
 */

#include "quakedef.h"
#include "netinc.h" // IWYU pragma: keep (portable OS file/lock declarations)
#include "fs.h"			//also pulls in qclib/hash.h, for the state-ledger lookup table
#include "qkupdate.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/file.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#if defined(WEBCLIENT) && defined(MULTITHREAD) && defined(HAVE_CLIENT)

//which manifest entries apply to this build. entries with no "platform" key are shared.
#if defined(_WIN32) && (defined(_WIN64) || defined(__x86_64__) || defined(_M_X64))
	#define QKU_PLATFORM "win64"
#elif defined(__linux__) && (defined(__x86_64__) || defined(__amd64__))
	#define QKU_PLATFORM "linux64"
#else
	#define QKU_PLATFORM ""		//unrecognised: every check refuses rather than guessing
#endif

static qboolean qku_ftesurf;
static qboolean qku_profileknown, qku_initialised;
static qboolean qku_menuaction;
#define QKU_STAGEDIR	(qku_ftesurf ? ".ftesurf-updater/ingame" : ".quakers-update")
#define QKU_STATEFILE	(qku_ftesurf ? ".ftesurf-updater/state.json" : "quakers-launcher.json")
#define QKU_FSKEY "18c7c332f5bbc913a0fceef38eab9494ce79cf60771fdda093a2d5eabef86cb6"
//Where the ledger lived until 2026-07-28: a directory holding exactly one file. The dot hides it
//on unix but not on Windows, so it was only clutter beside the game binaries. Read-only fallback
//so an install the new launcher has not touched yet still gets an accurate delta; whichever of
//the two runs first migrates it, and the launcher deletes the old directory when it saves.
#define QKU_STATEDIR_OLD	".quakers-launcher"
#define QKU_MAXMANIFEST	(32*1024*1024)			//a cloudflare error page is small; a real manifest is <1MB. this is only a sanity bound.
#define QKU_MAXATTEMPTS	3						//journal replays before we give up, so a stuck file can never boot-loop
#define QKU_HEXLEN		64						//blake2b-256, hex

enum qku_state_e
{
	QKUS_DISABLED,
	QKUS_IDLE,
	QKUS_CHECKING,
	QKUS_UPTODATE,
	QKUS_AVAILABLE,
	QKUS_DOWNLOADING,
	QKUS_APPLYING,
	QKUS_RESTART,
	QKUS_LAUNCHER,
	QKUS_ERROR
};
static const char *qku_statename[] =
{
	"disabled", "idle", "checking", "uptodate", "available",
	"downloading", "applying", "restart", "launcher", "error"
};

//per-file progress, main thread only once the plan is adopted
enum
{
	QKUF_PENDING,	//not started
	QKUF_ACTIVE,	//download in flight
	QKUF_STAGED,	//downloaded AND hash-verified: staged/<hash> exists
	QKUF_DONE,		//installed at its destination
	QKUF_FAILED
};
typedef struct
{
	char		*path;					//install-root-relative, eg "quakers/menu.dat"
	char		hash[QKU_HEXLEN+1];
	quint64_t	size;
	qboolean	exec;					//unix +x. mirrors the launcher; without it a downloaded engine won't start.
	qboolean	preserved;				//preflight decision against the unchanged install ledger
	int			status;
} qku_file_t;

//handed from WG_LOADER back to main. self-contained: main adopts the arrays wholesale.
typedef struct
{
	unsigned int	seq;
	int				verdict;			//QKUS_UPTODATE / AVAILABLE / LAUNCHER / ERROR
	char			version[64];
	char			objectbase[MAX_OSPATH];
	char			*envelope;
	char			reason[192];
	qku_file_t		*files;
	size_t			numfiles;
	quint64_t		totalbytes;
	char			**removed;		//install-relative paths the manifest retired
	size_t			numremoved;
} qku_plan_t;

static struct
{
	int				state;
	unsigned int	seq;				//bumped by cancel/shutdown; stale async results carry an old seq and are dropped
	char			version[64];		//the version we are moving TO
	char			objectbase[MAX_OSPATH];
	char			*envelope;
	char			error[192];

	qku_file_t		*files;
	size_t			numfiles;
	size_t			numdone;
	quint64_t		totalbytes;
	quint64_t		donebytes;			//bytes of files that fully landed

	char			**removed;			//paths to delete once the download half succeeds
	size_t			numremoved;

	int				active;				//downloads in flight
	qboolean		ticking;			//a progress timer is armed
	qboolean		autochecked;		//the one automatic check has been kicked

	double			ratetime;
	quint64_t		ratebytes;
	float			rate;
} qku;

// Startup can select its manifest before or after cvar registration.
static int qku_bootverdict = -1;
static char qku_bootreason[192];

static cvar_t update_enabled		= CVARFD("update_enabled",		"1",	CVAR_ARCHIVE|CVAR_NOTFROMSERVER, "Allow the engine to check for and apply content updates. 0 disables it entirely.");
static cvar_t update_autocheck		= CVARFD("update_autocheck",	"1",	CVAR_ARCHIVE|CVAR_NOTFROMSERVER, "Check once automatically when the main menu opens.");
static cvar_t update_manifest_url	= CVARFD("update_manifest_url",	"https://dl.proto.bar/manifests/alpha.json",	CVAR_ARCHIVE|CVAR_NOTFROMSERVER, "Where to look for the content manifest. NOTFROMSERVER: a server must never be able to repoint this.");
static cvar_t update_concurrency	= CVARFD("update_concurrency",	"3",	CVAR_ARCHIVE|CVAR_NOTFROMSERVER, "Parallel object downloads. Kept below MAXDOWNLOADTHREADS so connect-time map downloads are not starved.");
static cvar_t update_max_files		= CVARFD("update_max_files",	"400",	CVAR_ARCHIVE|CVAR_NOTFROMSERVER, "Refuse to apply a delta larger than this many files; tell the player to use the launcher instead.");
static cvar_t update_max_mb			= CVARFD("update_max_mb",		"512",	CVAR_ARCHIVE|CVAR_NOTFROMSERVER, "Refuse to apply a delta larger than this many megabytes.");
static cvar_t update_engine_policy	= CVARFD("update_engine_policy","0",	CVAR_ARCHIVE|CVAR_NOTFROMSERVER, "0: refuse any delta touching the engine binaries (exe/plugins/sqlite3/fmf) -- they must be replaced as a matched set or the plugin ABI corrupts memory.");
static cvar_t update_allow_insecure	= CVARFD("update_allow_insecure","0",	CVAR_ARCHIVE|CVAR_NOTFROMSERVER, "Permit a plain-http manifest url. For testing against a local mirror only.");
static cvar_t update_stage_only		= CVARFD("update_stage_only",	"0",	CVAR_ARCHIVE|CVAR_NOTFROMSERVER, "Download and verify into .quakers-update/staged but never install. Debugging aid.");

//read-only status, polled by menu.dat. NORESET is required, not cosmetic: Cvar_GamedirChange()
//wipes registered cvars back to their defaults during boot (see cl_main.c's cl_launchintogame).
#define QKU_ROFLAGS (CVAR_NOSET|CVAR_NOSAVE|CVAR_NORESET|CVAR_NOTFROMSERVER)
static cvar_t update_consent = CVARFD("update_consent", "-1", QKU_ROFLAGS, "Read-only shared launcher/in-game update permission: -1 undecided, 0 off, 1 on. Use update_permission.");
static cvar_t update_state			= CVARFD("update_state",		"idle",	QKU_ROFLAGS, "Read-only: disabled/idle/checking/uptodate/available/downloading/applying/restart/launcher/error.");
static cvar_t update_files			= CVARFD("update_files",		"0",	QKU_ROFLAGS, "Read-only: files in the pending update.");
static cvar_t update_files_done		= CVARFD("update_files_done",	"0",	QKU_ROFLAGS, "Read-only: files completed so far.");
static cvar_t update_bytes			= CVARFD("update_bytes",		"0",	QKU_ROFLAGS, "Read-only: total bytes to fetch.");
static cvar_t update_done_bytes		= CVARFD("update_done_bytes",	"0",	QKU_ROFLAGS, "Read-only: bytes fetched so far.");
static cvar_t update_rate			= CVARFD("update_rate",			"0",	QKU_ROFLAGS, "Read-only: current download rate, bytes/sec.");
static cvar_t update_current		= CVARFD("update_current",		"",		QKU_ROFLAGS, "Read-only: the file currently downloading.");
static cvar_t update_version		= CVARFD("update_version",		"",		QKU_ROFLAGS, "Read-only: the manifest version last seen.");
static cvar_t update_error			= CVARFD("update_error",		"",		QKU_ROFLAGS, "Read-only: why the last check or apply failed.");

static void QKU_StartDownloads(void);
static void QKU_ConfigureProfile(void);

// ---------------------------------------------------------------------------------------
// paths + validation
// ---------------------------------------------------------------------------------------

//com_gamepath is the install root and already carries its trailing separator ("c:/FTEQuake/").
//Everything here is an ABSOLUTE path used with FS_SYSTEM. Never FS_ROOT: that redirects into
//com_homepath, which would scatter half an update through Saved Games.
static void QKU_InstallPath(char *out, size_t outsize, const char *rel)
{
	Q_snprintfz(out, outsize, "%s%s", com_gamepath, rel);
}
static void QKU_StagePath(char *out, size_t outsize, const char *fmt, const char *leaf)
{
	char sub[MAX_OSPATH];
	Q_snprintfz(sub, sizeof(sub), fmt, leaf);
	Q_snprintfz(out, outsize, "%s%s/%s", com_gamepath, QKU_STAGEDIR, sub);
}

static qboolean QKU_ValidHash(const char *h)
{
	int i;
	if (!h)
		return false;
	for (i = 0; i < QKU_HEXLEN; i++)
		if (!((h[i] >= '0' && h[i] <= '9') || (h[i] >= 'a' && h[i] <= 'f')))
			return false;	//lowercase only: the hex IS the object's filename, so case matters
	return h[QKU_HEXLEN] == 0;
}

//`path` comes straight out of attacker-controllable JSON and is concatenated onto the install
//root, so this is the security boundary for the whole feature. Anything even slightly unusual
//is rejected and the WHOLE manifest is discarded -- a partially-trusted manifest is worthless.
static qboolean QKU_ValidPath(const char *p)
{
	const char *c, *seg;
	size_t len;

	if (!p)
		return false;
	len = strlen(p);
	if (!len || len >= MAX_OSPATH-128)
		return false;
	if (*p == '/')
		return false;					//absolute
	if (strchr(p, '\\') || strchr(p, ':'))
		return false;					//windows separators and drive letters / NTFS streams

	for (c = seg = p; ; c++)
	{
		if (*c == '/' || !*c)
		{
			size_t sl = c - seg;
			if (!sl)
				return false;			//"" or "a//b" or a trailing slash
			if (sl == 1 && seg[0] == '.')
				return false;			//"."
			if (sl == 2 && seg[0] == '.' && seg[1] == '.')
				return false;			//".."
			if (seg[sl-1] == '.' || seg[sl-1] == ' ')
				return false;			//win32 silently strips these, so "a.txt." would alias "a.txt"
			if (!*c)
				break;
			seg = c+1;
		}
		else if ((unsigned char)*c < 0x20 || *c == '"' || *c == '*' || *c == '<' || *c == '>' || *c == '?' || *c == '|')
			return false;
	}
	return true;
}

// FTESurf's disk policy is also enforced by updatefeed.py and the native launcher.
static qboolean QKU_FSManaged(const char *p)
{
	static const char *roots[] = {"default.fmf", "ftesurf.bat", "ftesurf.sh", "ftesurf_debug.bat", "LICENSE", "INSTALL.txt", "VERSION.txt", "SOURCE.txt", "THIRD-PARTY.txt", "ftesurf64.exe", "ftesurf64", "fteplug_hl2_x64.dll", "fteplug_ui_imgui_x64.dll", "fteplug_hl2_amd64.so", "ftesurf/csprogs.dat", "ftesurf/qwprogs.dat", "ftesurf/menu.dat", "ftesurf/fs_addons.default.txt", "ftesurf/fs_addons.txt", "ftesurf/data/mapmeta.txt", "ftesurf/data/mapdeps.txt", "ftesurf/data/mapmeta_override.txt", "ftesurf/data/mapparticles.txt", "ftesurf/data/mapwr.txt", "ftesurf/data/mapdl.txt", "ftesurf/data/mapmanifest.txt", "ftesurf/textures/no_texture.png"};
	static const char *prefixes[] = {"ftesurf/gfx/", "ftesurf/glsl/", "ftesurf/particles/", "ftesurf/scripts/", "ftesurf/models/", "ftesurf/sound/"};
	size_t i, n = strlen(p);
	const char *t;
	if (!QKU_ValidPath(p) || n >= 240) return false;
	for (i = 0; i < n; i++) if ((unsigned char)p[i] >= 127) return false;
	for (t = p; *t; )
	{
		const char *end = strchr(t, '/'), *dot = strchr(t, '.');
		size_t len = end ? (size_t)(end-t) : strlen(t);
		if (dot && (size_t)(dot-t) < len) len = dot-t;
		if ((len == 3 && (!Q_strncasecmp(t, "con", 3) || !Q_strncasecmp(t, "prn", 3) || !Q_strncasecmp(t, "aux", 3) || !Q_strncasecmp(t, "nul", 3))) || (len == 4 && (!Q_strncasecmp(t, "com", 3) || !Q_strncasecmp(t, "lpt", 3)) && t[3] >= '1' && t[3] <= '9')) return false;
		if (!end) break;
		t = end+1;
	}
	if (!strncmp(p, "notices/", 8)) return !strchr(p+8, '/') && n >= 4 && !strcmp(p+n-4, ".txt");
	for (i = 0; i < sizeof(roots)/sizeof(roots[0]); i++) if (!strcmp(p, roots[i])) return true;
	if (!strncmp(p, "ftesurf/maps/zones/local/", 25))
	{
		t = p + 25;
		return !strchr(t, '/') && ((n >= 5 && !strcmp(p+n-5, ".json")) || !strcmp(t, "manifest.txt"));
	}
	if (!strncmp(p, "ftesurf/cfg/", 12))
	{
		t = p+12;
		return n >= 4 && !strcmp(p+n-4, ".cfg") && strncmp(t, "test/", 5) && strncmp(t, "testrun/", 8) && strncmp(t, "build", 5) && strncmp(t, "fs_", 3) && !strstr(t, "lobby_local") && strcmp(t, "settings.cfg") && (!strchr(t, '/') || !strncmp(t, "maps/", 5) || !strncmp(t, "lobby/", 6));
	}
	for (i = 0; i < sizeof(prefixes)/sizeof(prefixes[0]); i++) if (!strncmp(p, prefixes[i], strlen(prefixes[i]))) return true;
	return false;
}

static qboolean QKU_FSInGame(const char *p)
{
	return !strncmp(p, "ftesurf/", 8) || !strncmp(p, "notices/", 8) || !strcmp(p, "LICENSE") || !strcmp(p, "INSTALL.txt") || !strcmp(p, "VERSION.txt") || !strcmp(p, "SOURCE.txt") || !strcmp(p, "THIRD-PARTY.txt");
}

static qboolean QKU_FSProtected(const char *p)
{
	return !strcmp(p, "ftesurf/fs_addons.txt") || !strncmp(p, "ftesurf/maps/zones/local/", 25);
}

static qboolean QKU_DecodeHex(const char *h, qbyte *out, size_t len)
{
	static const char hex[] = "0123456789abcdef";
	size_t i;
	if (strlen(h) != len*2) return false;
	for (i = 0; i < len; i++)
	{
		const char *a = strchr(hex, h[i*2]);
		const char *b = strchr(hex, h[i*2+1]);
		if (!a || !b) return false;
		out[i] = ((a-hex)<<4) | (b-hex);
	}
	return true;
}

static qboolean QKU_FSVersion(const char *v, unsigned parts[3])
{
	size_t i;
	for (i = 0; i < 3; i++)
	{
		unsigned n = 0;
		if (*v < '0' || *v > '9') return false;
		while (*v >= '0' && *v <= '9')
		{
			unsigned digit = *v++ - '0';
			if (n > (0xffffffffu-digit)/10u) return false;
			n = n*10u + digit;
		}
		parts[i] = n;
		if (i < 2 && *v++ != '.') return false;
	}
	return !*v;
}

static qboolean QKU_FSVersionOlder(const char *v, const char *old)
{
	unsigned n[3], o[3];
	if (!QKU_FSVersion(v, n) || !QKU_FSVersion(old, o)) return true;
	return n[0] < o[0] || (n[0] == o[0] && (n[1] < o[1] || (n[1] == o[1] && n[2] < o[2])));
}

// Check OS ancestors, not merely virtual mount names: a junction must never turn a
// game-owned relative path into a write to Steam, recordings or another install.
static qboolean QKU_FSValidateManifest(json_t *root)
{
	struct fsentry_s { char path[MAX_OSPATH], key[MAX_OSPATH+32], hash[65]; quint64_t size; bucket_t bypath, bynative, byhash; } *rows = NULL;
	hashtable_t paths, native, hashes;
	void *pm = NULL, *nm = NULL, *hm = NULL;
	json_t *list, *exec, *args;
	size_t i, j, count, buckets;
	quint64_t total = 0;
	unsigned ver[3], must = 0;
	qboolean ok = false;
	char buf[MAX_OSPATH];
	const char *exe = !strcmp(QKU_PLATFORM, "win64") ? "ftesurf64.exe" : "ftesurf64";
	JSON_GetString(root, "game", buf, sizeof(buf), ""); if (strcmp(buf, "ftesurf")) return false;
	JSON_GetString(root, "channel", buf, sizeof(buf), ""); if (strcmp(buf, "stable")) return false;
	JSON_GetString(root, "version", buf, sizeof(buf), ""); if (!QKU_FSVersion(buf, ver)) return false;
	list = JSON_FindChild(root, "removed"); if (!list || list->type != json_type_array || JSON_GetCount(list)) return false;
	list = JSON_FindChild(root, "files"); count = JSON_GetCount(list);
	if (!list || list->type != json_type_array || !count || count > 8192) return false;
	exec = JSON_FindChild(JSON_FindChild(root, "exec_by_platform"), QKU_PLATFORM);
	if (!exec) exec = JSON_FindChild(root, "exec");
	JSON_GetString(exec, "cmd", buf, sizeof(buf), ""); if (strcmp(buf, exe)) return false;
	args = JSON_FindChild(exec, "args"); if (!args || args->type != json_type_array || JSON_GetCount(args) != 2) return false;
	JSON_ReadBody(JSON_GetIndexed(args, 0), buf, sizeof(buf)); if (strcmp(buf, "-manifest")) return false;
	JSON_ReadBody(JSON_GetIndexed(args, 1), buf, sizeof(buf)); if (strcmp(buf, "default.fmf")) return false;
	for (buckets = 256; buckets < count; buckets <<= 1) ;
	pm = Z_Malloc(Hash_BytesForBuckets(buckets)); Hash_InitTable(&paths, buckets, pm);
	nm = Z_Malloc(Hash_BytesForBuckets(buckets)); Hash_InitTable(&native, buckets, nm);
	hm = Z_Malloc(Hash_BytesForBuckets(buckets)); Hash_InitTable(&hashes, buckets, hm);
	rows = Z_Malloc(sizeof(*rows)*count);
	for (i = 0; i < count; i++)
	{
		json_t *f = JSON_GetIndexed(list, i), *s = JSON_FindChild(f, "size");
		char platform[32];
		const char *c;
		struct fsentry_s *same;
		JSON_GetString(f, "path", rows[i].path, sizeof(rows[i].path), "");
		JSON_GetString(f, "hash", rows[i].hash, sizeof(rows[i].hash), "");
		JSON_GetString(f, "platform", platform, sizeof(platform), "");
		if (!QKU_FSManaged(rows[i].path) || !QKU_ValidHash(rows[i].hash) || !s || s->type != json_type_number || s->bodyend == s->bodystart) goto done;
		rows[i].size = 0;
		for (c = s->bodystart; c < s->bodyend; c++)
		{
			if (*c < '0' || *c > '9') goto done;
			rows[i].size = rows[i].size*10u + (*c-'0');
			if (rows[i].size > 32*1024*1024) goto done;
		}
		total += rows[i].size; if (total > 160*1024*1024) goto done;
		if (strcmp(platform, "all") && strcmp(platform, "win64") && strcmp(platform, "linux64")) goto done;
		if (!strcmp(platform, "all") || !strcmp(platform, QKU_PLATFORM))
		{
			if (!strcmp(rows[i].path, exe)) must |= 1u;
			if (!strcmp(rows[i].path, "default.fmf")) must |= 2u;
			if (!strcmp(rows[i].path, "ftesurf/menu.dat")) must |= 4u;
			if (!strcmp(rows[i].path, "ftesurf/csprogs.dat")) must |= 8u;
			if (!strcmp(rows[i].path, "ftesurf/qwprogs.dat")) must |= 16u;
			for (j = 0; rows[i].path[j]; j++) if (rows[i].path[j] >= 'A' && rows[i].path[j] <= 'Z') rows[i].path[j] += 'a'-'A';
			if (Hash_Get(&native, rows[i].path)) goto done;
			Hash_Add(&native, rows[i].path, &rows[i], &rows[i].bynative);
		}
		for (j = 0; rows[i].path[j]; j++) if (rows[i].path[j] >= 'A' && rows[i].path[j] <= 'Z') rows[i].path[j] += 'a'-'A';
		Q_snprintfz(rows[i].key, sizeof(rows[i].key), "%s:%s", platform, rows[i].path);
		if (Hash_Get(&paths, rows[i].key)) goto done;
		Hash_Add(&paths, rows[i].key, &rows[i], &rows[i].bypath);
		same = Hash_Get(&hashes, rows[i].hash);
		if (same) { if (same->size != rows[i].size) goto done; }
		else Hash_Add(&hashes, rows[i].hash, &rows[i], &rows[i].byhash);
	}
	ok = must == 31u;
 done:
	Z_Free(rows); Z_Free(pm); Z_Free(nm); Z_Free(hm);
	return ok;
}

static qboolean QKU_FSNoLinks(const char *rel)
{
	char path[MAX_OSPATH];
	size_t i, n;
	QKU_InstallPath(path, sizeof(path), rel);
	n = strlen(path);
	for (i = 0; i <= n; i++)
	{
		char saved = path[i];
		if (saved && saved != '/' && saved != '\\') continue;
		if (!i) continue;
		path[i] = 0;
#ifdef _WIN32
		{
			wchar_t wide[MAX_OSPATH];
			DWORD attr;
			if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, MAX_OSPATH)) return false;
			attr = GetFileAttributesW(wide);
			if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
			if (attr == INVALID_FILE_ATTRIBUTES && GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) return false;
		}
#else
		{
			struct stat st;
			if (!lstat(path, &st) && S_ISLNK(st.st_mode)) return false;
		}
#endif
		path[i] = saved;
	}
	return true;
}

static qboolean QKU_FSStageNoLinks(const char *fmt, const char *leaf)
{
	char sub[MAX_OSPATH], rel[MAX_OSPATH];
	if (!qku_ftesurf) return true;
	Q_snprintfz(sub, sizeof(sub), fmt, leaf);
	Q_snprintfz(rel, sizeof(rel), "%s/%s", QKU_STAGEDIR, sub);
	return QKU_FSNoLinks(rel);
}

#ifdef _WIN32
static HANDLE qku_fslock = INVALID_HANDLE_VALUE;
#else
static int qku_fslock = -1;
#endif

static void QKU_FSLock(void)
{
	char path[MAX_OSPATH];
#ifdef HAVE_SERVER
	if (isDedicated) return; //the client launcher does not manage dedicated installs
#endif
	if (!QKU_FSNoLinks(".ftesurf-updater/install.lock") || !QKU_FSNoLinks(".ftesurf-updater/pending.json") || !QKU_FSNoLinks(".ftesurf-updater/state.json") || !QKU_FSNoLinks(".ftesurf-updater/state.json.tmp") || !QKU_FSNoLinks(".ftesurf-updater/state.json.enginetmp") || !QKU_FSNoLinks(".ftesurf-updater/installed.json") || !QKU_FSNoLinks(".ftesurf-updater/installed.json.tmp")) Sys_Error("Linked updater metadata: refusing to start");
	QKU_InstallPath(path, sizeof(path), ".ftesurf-updater/install.lock");
	FS_CreatePath(path, FS_SYSTEM);
#ifdef _WIN32
	{
		wchar_t wide[MAX_OSPATH];
		OVERLAPPED ov = {0};
		MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, MAX_OSPATH);
		qku_fslock = CreateFileW(wide, GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (qku_fslock == INVALID_HANDLE_VALUE || !LockFileEx(qku_fslock, LOCKFILE_EXCLUSIVE_LOCK|LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) Sys_Error("FTESurf or its updater is running. Close it before starting another copy.");
	}
#else
	qku_fslock = open(path, O_RDWR|O_CREAT, 0600);
	if (qku_fslock < 0 || flock(qku_fslock, LOCK_EX|LOCK_NB)) Sys_Error("FTESurf or its updater is running. Close it before starting another copy.");
#endif
	QKU_InstallPath(path, sizeof(path), ".ftesurf-updater/pending.json");
	{
		vfsfile_t *pending = FS_OpenVFS(path, "rb", FS_SYSTEM);
		if (pending) { VFS_CLOSE(pending); Sys_Error("Unfinished launcher update. Run ftesurf-launcher to finish installing before starting the game."); }
	}
}

// ---------------------------------------------------------------------------------------
// main-thread state
// ---------------------------------------------------------------------------------------

static void QKU_PublishCounts(void)
{
	Cvar_ForceSet(&update_files,		va("%u", (unsigned)qku.numfiles));
	Cvar_ForceSet(&update_files_done,	va("%u", (unsigned)qku.numdone));
	Cvar_ForceSet(&update_bytes,		va("%.0f", (double)qku.totalbytes));
	Cvar_ForceSet(&update_done_bytes,	va("%.0f", (double)qku.donebytes));
}

static void QKU_SetState(int s)
{
	qku.state = s;
	Cvar_ForceSet(&update_state, qku_statename[s]);
}

static void QKU_Fail(int s, const char *fmt, ...)
{
	va_list argptr;
	va_start(argptr, fmt);
	Q_vsnprintfz(qku.error, sizeof(qku.error), fmt, argptr);
	va_end(argptr);

	Cvar_ForceSet(&update_error, qku.error);
	QKU_SetState(s);
	Con_DPrintf(CON_WARNING "update: %s\n", qku.error);
}

static void QKU_FreePlanFiles(qku_file_t *files, size_t n)
{
	size_t i;
	if (!files)
		return;
	for (i = 0; i < n; i++)
		Z_Free(files[i].path);
	Z_Free(files);
}

static void QKU_FreeStrings(char **v, size_t n)
{
	size_t i;
	if (!v)
		return;
	for (i = 0; i < n; i++)
		Z_Free(v[i]);
	Z_Free(v);
}

static void QKU_ClearPlan(void)
{
	if (qku.envelope) { Z_Free(qku.envelope); qku.envelope = NULL; }
	QKU_FreePlanFiles(qku.files, qku.numfiles);
	QKU_FreeStrings(qku.removed, qku.numremoved);
	qku.removed = NULL;
	qku.numremoved = 0;
	qku.files = NULL;
	qku.numfiles = qku.numdone = 0;
	qku.totalbytes = qku.donebytes = 0;
	Cvar_ForceSet(&update_current, "");
	Cvar_ForceSet(&update_rate, "0");
	QKU_PublishCounts();
}

// ---------------------------------------------------------------------------------------
// hashing a local file (WG_LOADER)
// ---------------------------------------------------------------------------------------

static qboolean QKU_HashFile(const char *abspath, quint64_t *outsize, char *outhex)
{
	vfsfile_t *f = FS_OpenVFS(abspath, "rb", FS_SYSTEM);
	qbyte digest[DIGEST_MAXSIZE];
	qbyte block[65536];
	void *ctx;
	int r;
	quint64_t sz = 0;

	if (!f)
		return false;

	ctx = Z_Malloc(hash_blake2b_256.contextsize);
	hash_blake2b_256.init(ctx);
	for (;;)
	{
		r = VFS_READ(f, block, sizeof(block));
		if (r <= 0)
			break;
		hash_blake2b_256.process(ctx, block, r);
		sz += r;
	}
	VFS_CLOSE(f);
	hash_blake2b_256.terminate(digest, ctx);
	Z_Free(ctx);

	outhex[Base16_EncodeBlock((const char*)digest, hash_blake2b_256.digestsize, (qbyte*)outhex, QKU_HEXLEN)] = 0;
	if (outsize)
		*outsize = sz;
	return r == 0;
}

static qboolean QKU_FSComplete(json_t *root)
{
	json_t *files = JSON_FindChild(root, "files");
	size_t i, count = JSON_GetCount(files);
	for (i = 0; i < count; i++)
	{
		json_t *f = JSON_GetIndexed(files, i);
		char platform[32], path[MAX_OSPATH], abs[MAX_OSPATH], hash[65], have[65];
		quint64_t size;
		JSON_GetString(f, "platform", platform, sizeof(platform), "");
		if (strcmp(platform, "all") && strcmp(platform, QKU_PLATFORM)) continue;
		JSON_GetString(f, "path", path, sizeof(path), "");
		JSON_GetString(f, "hash", hash, sizeof(hash), "");
		if (!QKU_FSNoLinks(path)) return false;
		QKU_InstallPath(abs, sizeof(abs), path);
		if (!QKU_HashFile(abs, &size, have)) return false;
		if (!QKU_FSProtected(path) && (size != (quint64_t)JSON_GetFloat(f, "size", -1) || strcmp(have, hash))) return false;
	}
	return true;
}

// ---------------------------------------------------------------------------------------
// reading a whole small file (WG_LOADER / boot)
// ---------------------------------------------------------------------------------------

static char *QKU_ReadTextFile(const char *abspath, size_t maxsize)
{
	vfsfile_t *f = FS_OpenVFS(abspath, "rb", FS_SYSTEM);
	qofs_t len;
	char *buf;

	if (!f)
		return NULL;
	len = VFS_GETLEN(f);
	if (len >= maxsize)
	{
		VFS_CLOSE(f);
		return NULL;
	}
	buf = BZ_Malloc(len+1);
	if (VFS_READ(f, buf, len) != (int)len)
	{
		BZ_Free(buf);
		VFS_CLOSE(f);
		return NULL;
	}
	buf[len] = 0;
	VFS_CLOSE(f);
	//a NUL inside would make every strlen-based reader disagree with the parser about where
	//the document ends, which is exactly the sort of ambiguity a smuggling attack needs.
	if (strlen(buf) != len)
	{
		BZ_Free(buf);
		return NULL;
	}
	return buf;
}

// The same signature reader is used for network checks and cold journal recovery.
static qboolean QKU_FSJSONNoNul(const char *start, const char *end)
{
	const char *c;
	for (c = start; c < end; c++)
		if (*c == '\\')
		{
			if (c+1 == end) return false;
			c++;
			if (*c == 'u' && end-c >= 5 && !strncmp(c+1, "0000", 4)) return false;
		}
	return true;
}

static json_t *QKU_FSParseSigned(const char *text, char **payloadout)
{
	json_t *envelope, *body, *payloadroot = NULL;
	qbyte pk[32], sig[64];
	char sighex[129], *payload = NULL;
	size_t n;
	*payloadout = NULL;
	if (strlen(text) > 2*1024*1024) return NULL;
	envelope = JSON_Parse(text);
	if (!envelope) goto done;
	body = JSON_FindChild(envelope, "payload");
	if (!body || body->type != json_type_string || !QKU_FSJSONNoNul(body->bodystart, body->bodyend)) goto done;
	n = JSON_ReadBody(body, NULL, 0); // escaped-length upper bound, NOT decoded length
	if (!n || n >= 2*1024*1024 || JSON_ReadBody(JSON_FindChild(envelope, "signature"), NULL, 0) != 128) goto done;
	payload = Z_Malloc(n+1);
	JSON_ReadBody(body, payload, n+1);
	if (!QKU_FSJSONNoNul(payload, payload+strlen(payload))) goto done;
	JSON_GetString(envelope, "signature", sighex, sizeof(sighex), "");
	if (!QKU_DecodeHex(QKU_FSKEY, pk, sizeof(pk)) || !QKU_DecodeHex(sighex, sig, sizeof(sig)) || !Ed25519_Verify(sig, (const qbyte *)payload, strlen(payload), pk)) goto done;
	payloadroot = JSON_Parse(payload);
	if (payloadroot) { *payloadout = payload; payload = NULL; }
 done:
	if (payload) Z_Free(payload);
	if (envelope) JSON_Destroy(envelope);
	return payloadroot;
}

static qboolean QKU_FSSaveEnvelope(const char *text, const char *rel)
{
	char path[MAX_OSPATH], tmp[MAX_OSPATH], tmprel[MAX_OSPATH];
	vfsfile_t *f;
	qboolean ok;
	if (!text) return false;
	Q_snprintfz(tmprel, sizeof(tmprel), "%s.tmp", rel);
	if (!QKU_FSNoLinks(rel) || !QKU_FSNoLinks(tmprel)) return false;
	QKU_InstallPath(path, sizeof(path), rel);
	QKU_InstallPath(tmp, sizeof(tmp), tmprel);
	FS_CreatePath(tmp, FS_SYSTEM);
	f = FS_OpenVFS(tmp, "wb", FS_SYSTEM);
	if (!f) return false;
	ok = VFS_WRITE(f, text, strlen(text)) == (int)strlen(text);
	if (!VFS_CLOSE(f)) ok = false;
	if (!ok) return false;
	FS_Remove(path, FS_SYSTEM);
	return FS_Rename(tmp, path, FS_SYSTEM);
}

static qboolean QKU_FSJournalEntry(json_t *signedroot, const char *path, const char *hash, quint64_t size)
{
	json_t *files = JSON_FindChild(signedroot, "files"), *f;
	char p[MAX_OSPATH], h[65], plat[32];
	size_t i, n = JSON_GetCount(files);
	if (!QKU_FSManaged(path) || !QKU_FSInGame(path) || !QKU_FSNoLinks(path) || n > 8192) return false;
	for (i = 0; i < n; i++)
	{
		f = JSON_GetIndexed(files, i);
		JSON_GetString(f, "path", p, sizeof(p), "");
		JSON_GetString(f, "platform", plat, sizeof(plat), "all");
		if (strcmp(p, path) || (strcmp(plat, "all") && strcmp(plat, QKU_PLATFORM))) continue;
		JSON_GetString(f, "hash", h, sizeof(h), "");
		return !strcmp(h, hash) && size == (quint64_t)JSON_GetFloat(f, "size", 0);
	}
	return false;
}

// ---------------------------------------------------------------------------------------
// the delta (WG_LOADER)
// ---------------------------------------------------------------------------------------

//The launcher's install ledger, shared on purpose. A second engine-owned copy would drift the
//moment either side wrote without the other, and the failure mode of drift here is a spurious
//multi-GB re-download. If it is missing or unreadable we do NOT fall back to hashing the whole
//6.3 GB tree -- we hand the player to the launcher.
typedef struct
{
	const char	*path;
	const char	*hash;		//slice into the document; NOT null-terminated
	size_t		hashlen;
} qku_stateent_t;

typedef struct
{
	json_t			*json;
	char			*text;
	char			version[64];
	qku_stateent_t	*ents;
	size_t			numents;
	hashtable_t		ht;
	void			*htmem;
	bucket_t		*buckets;
} qku_state_t;

static void QKU_FreeState(qku_state_t *st)
{
	if (st->json)
		JSON_Destroy(st->json);
	if (st->text)
		BZ_Free(st->text);
	Z_Free(st->ents);
	Z_Free(st->htmem);
	Z_Free(st->buckets);
	memset(st, 0, sizeof(*st));
}

static qboolean QKU_LoadState(qku_state_t *st)
{
	char path[MAX_OSPATH];
	json_t *files, *n;
	size_t i, buckets;

	memset(st, 0, sizeof(*st));
	Q_snprintfz(path, sizeof(path), "%s%s", com_gamepath, QKU_STATEFILE);
	st->text = QKU_ReadTextFile(path, 64*1024*1024);
	if (!st->text && !qku_ftesurf)
	{	//pre-2026-07-28 install the new launcher has not run on yet
		Q_snprintfz(path, sizeof(path), "%s%s/state.json", com_gamepath, QKU_STATEDIR_OLD);
		st->text = QKU_ReadTextFile(path, 64*1024*1024);
	}
	if (!st->text)
		return false;
	st->json = JSON_Parse(st->text);
	if (!st->json)
		return false;

	JSON_GetString(st->json, "version", st->version, sizeof(st->version), "");
	files = JSON_FindChild(st->json, "files");
	if (!files)
		return false;

	for (n = files->child, st->numents = 0; n; n = n->sibling)
		st->numents++;
	if (!st->numents)
		return false;

	st->ents = Z_Malloc(sizeof(*st->ents) * st->numents);
	st->buckets = Z_Malloc(sizeof(bucket_t) * st->numents);
	//power-of-two-ish bucket count, ~2 entries per bucket
	for (buckets = 256; buckets < st->numents; buckets <<= 1)
		;
	st->htmem = Z_Malloc(Hash_BytesForBuckets(buckets));
	Hash_InitTable(&st->ht, buckets, st->htmem);

	for (n = files->child, i = 0; n && i < st->numents; n = n->sibling, i++)
	{
		//json_t keeps the key in ->name and the value as a body slice of st->text, so both
		//pointers stay valid for as long as the tree does. No copying 4,243 strings.
		st->ents[i].path = n->name;
		st->ents[i].hash = n->bodystart;
		st->ents[i].hashlen = (size_t)(n->bodyend - n->bodystart);
		Hash_Add(&st->ht, st->ents[i].path, &st->ents[i], &st->buckets[i]);
	}
	return true;
}

//The recorded hash is a slice of the still-live document rather than a copy, so compare by
//length. Keeping it as a slice is the whole point of the hashtable: re-finding the value with
//JSON_FindChild would be a linear scan over 4,000+ children per lookup, which is exactly the
//~18M strncmp() this design exists to avoid.
static qboolean QKU_StateHashEquals(const qku_stateent_t *e, const char *hex)
{
	return e->hashlen == QKU_HEXLEN && !strncmp(e->hash, hex, QKU_HEXLEN);
}

enum qku_preserve_e { QKUP_INSTALL, QKUP_PRESERVE, QKUP_UNREADABLE };

// Planning and cold recovery must distinguish absence from an unreadable local edit.
static enum qku_preserve_e QKU_FSPreserveFile(qku_state_t *st, const char *path, const char *hash)
{
	char abs[MAX_OSPATH], have[QKU_HEXLEN+1];
	quint64_t size;
	qku_stateent_t *old;
	if (!QKU_FSProtected(path)) return QKUP_INSTALL;
	QKU_InstallPath(abs, sizeof(abs), path);
	if (!QKU_HashFile(abs, &size, have))
	{
#ifdef _WIN32
		wchar_t wide[MAX_OSPATH];
		DWORD error;
		if (!MultiByteToWideChar(CP_UTF8, 0, abs, -1, wide, MAX_OSPATH)) return QKUP_UNREADABLE;
		if (GetFileAttributesW(wide) != INVALID_FILE_ATTRIBUTES) return QKUP_UNREADABLE;
		error = GetLastError();
		if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return QKUP_INSTALL;
#else
		struct stat stbuf;
		if (lstat(abs, &stbuf) < 0 && errno == ENOENT) return QKUP_INSTALL;
#endif
		return QKUP_UNREADABLE;
	}
	old = Hash_Get(&st->ht, path);
	return (!strcmp(path, "ftesurf/fs_addons.txt") || (strcmp(have, hash) && (!old || !QKU_StateHashEquals(old, have)))) ? QKUP_PRESERVE : QKUP_INSTALL;
}

struct qku_checkargs_s
{
	char	*text;			//manifest body, BZ_Malloc'd, owned by the work function
	unsigned int seq;
	qboolean force;			//skip the version fast-path
	qboolean verifyonly;	//hash every file on disk and only report
};

static void QKU_Main_AdoptPlan(int iarg, void *data);

static void QKU_PostPlan(qku_plan_t *plan)
{
	Cmd_AddTimer(0, QKU_Main_AdoptPlan, 0, &plan, sizeof(qku_plan_t *));
}

static void QKU_Work_Check(void *ctx, void *data, size_t a, size_t b)
{
	struct qku_checkargs_s *args = data;
	qku_plan_t *plan = Z_Malloc(sizeof(*plan));
	qku_state_t st;
	json_t *root = NULL, *filelist, *ent;
	char *payload = NULL;
	char buf[MAX_OSPATH];
	size_t i, count, cand = 0;
	qku_file_t *files = NULL;
	qboolean haveengine = false;
	unsigned int maxfiles;
	quint64_t maxbytes;

	(void)ctx; (void)a; (void)b;
	plan->seq = args->seq;
	plan->verdict = QKUS_ERROR;
	memset(&st, 0, sizeof(st));

	maxfiles = (update_max_files.ival > 0) ? (unsigned)update_max_files.ival : 400;
	maxbytes = (quint64_t)((update_max_mb.value > 0) ? update_max_mb.value : 512) * 1024u * 1024u;

	root = JSON_Parse(args->text);
	if (!root)
	{	//JSON_Parse returns NULL for anything at all, including a cloudflare html error page
		Q_strncpyz(plan->reason, "manifest is not valid JSON (a proxy error page?)", sizeof(plan->reason));
		goto done;
	}

	if (qku_ftesurf)
	{
		JSON_Destroy(root);
		root = QKU_FSParseSigned(args->text, &payload);
		if (!root) { Q_strncpyz(plan->reason, "update manifest signature is invalid", sizeof(plan->reason)); goto done; }
		if (!QKU_FSValidateManifest(root)) { Q_strncpyz(plan->reason, "signed update format/policy is invalid", sizeof(plan->reason)); goto done; }
		if (maxfiles > 512) maxfiles = 512;
		if (maxbytes > 16*1024*1024) maxbytes = 16*1024*1024;
		JSON_GetString(root, "game", buf, sizeof(buf), "");
		if (strcmp(buf, "ftesurf") || JSON_GetCount(JSON_FindChild(root, "removed")))
		{ Q_strncpyz(plan->reason, "wrong game or unsupported file deletion", sizeof(plan->reason)); goto done; }
		JSON_GetString(root, "channel", buf, sizeof(buf), "");
		if (strcmp(buf, "stable")) { Q_strncpyz(plan->reason, "wrong update channel", sizeof(plan->reason)); goto done; }
		JSON_ReadBody(JSON_GetIndexed(JSON_FindChild(root, "mirrors"), 0), plan->objectbase, sizeof(plan->objectbase));
		if (strncmp(plan->objectbase, "https://", 8) && !(update_allow_insecure.ival && (!strncmp(plan->objectbase, "http://127.0.0.1:", 17) || !strncmp(plan->objectbase, "http://localhost:", 17))))
		{ Q_strncpyz(plan->reason, "update object mirror must use HTTPS", sizeof(plan->reason)); goto done; }
	}

	if (JSON_GetInteger(root, "schema", 0) != 2)
	{
		Q_snprintfz(plan->reason, sizeof(plan->reason), "manifest schema %i, expected 2", (int)JSON_GetInteger(root, "schema", 0));
		goto done;
	}
	JSON_GetString(root, "hash_algo", buf, sizeof(buf), "");
	if (strcmp(buf, "blake2b-256"))
	{	//object names ARE the hashes, so this can never be renegotiated at runtime
		Q_snprintfz(plan->reason, sizeof(plan->reason), "manifest hash_algo \"%s\", this engine only speaks blake2b-256", buf);
		goto done;
	}
	JSON_GetString(root, "object_layout", buf, sizeof(buf), "");
	if (strcmp(buf, "objects/{h0h1}/{hash}"))
	{	//refuse rather than implement a template engine over an untrusted string
		Q_snprintfz(plan->reason, sizeof(plan->reason), "unsupported object_layout \"%s\"", buf);
		goto done;
	}
	if (!*QKU_PLATFORM)
	{
		Q_strncpyz(plan->reason, "this platform has no manifest key", sizeof(plan->reason));
		plan->verdict = QKUS_LAUNCHER;
		goto done;
	}
	JSON_GetString(root, "version", plan->version, sizeof(plan->version), "");
	if (!*plan->version)
	{
		Q_strncpyz(plan->reason, "manifest has no version", sizeof(plan->reason));
		goto done;
	}

	filelist = JSON_FindChild(root, "files");
	count = filelist ? JSON_GetCount(filelist) : 0;
	if (!count)
	{
		Q_strncpyz(plan->reason, "manifest lists no files", sizeof(plan->reason));
		goto done;
	}

	if (!QKU_LoadState(&st))
	{
		Q_strncpyz(plan->reason, "no launcher install state -- run the launcher once", sizeof(plan->reason));
		plan->verdict = QKUS_LAUNCHER;
		goto done;
	}

	if (qku_ftesurf && *st.version && QKU_FSVersionOlder(plan->version, st.version))
	{ Q_strncpyz(plan->reason, "refusing an older signed release", sizeof(plan->reason)); goto done; }
	if (!qku_ftesurf && !args->force && !args->verifyonly && *st.version && !strcmp(st.version, plan->version))
	{	//the O(1) fast path, and the case that runs 99% of the time
		plan->verdict = QKUS_UPTODATE;
		goto done;
	}

	files = Z_Malloc(sizeof(*files) * count);
	for (i = 0; i < count; i++)
	{
		char path[MAX_OSPATH], hash[QKU_HEXLEN+16], platform[32], component[32];
		json_t *pathnode;
		quint64_t size;
		qku_stateent_t *known;

		ent = JSON_GetIndexed(filelist, i);
		if (!ent)
			continue;

		pathnode = JSON_FindChild(ent, "path");
		if (!pathnode || !JSON_ReadBody(pathnode, path, sizeof(path)))
		{
			Q_strncpyz(plan->reason, "manifest entry has no path", sizeof(plan->reason));
			plan->verdict = QKUS_ERROR;
			goto done;
		}
		JSON_GetString(ent, "hash", hash, sizeof(hash), "");
		size = (quint64_t)JSON_GetFloat(ent, "size", -1);

		//one bad entry poisons the whole manifest. a partially-trusted manifest is worthless:
		//we could not tell which half was authored by whom.
		if (!QKU_ValidPath(path) || !QKU_ValidHash(hash) || size > 0x80000000ull || (qku_ftesurf && (!QKU_FSManaged(path) || size > 32*1024*1024 || !QKU_FSNoLinks(path))))
		{
			Q_snprintfz(plan->reason, sizeof(plan->reason), "manifest rejected: bad entry \"%.64s\"", path);
			plan->verdict = QKUS_ERROR;
			goto done;
		}

		//"platform" is absent on the 4,231 shared entries; absent means "all".
		JSON_GetString(ent, "platform", platform, sizeof(platform), "");
		if (*platform && strcmp(platform, "all") && strcmp(platform, QKU_PLATFORM))
			continue;

		known = Hash_Get(&st.ht, path);
		if (!qku_ftesurf && known && QKU_StateHashEquals(known, hash))
			continue;	//unchanged: ZERO filesystem calls for the ~4,240 files in this branch

		JSON_GetString(ent, "component", component, sizeof(component), "");
		if (!strcmp(component, "engine"))
			haveengine = true;

		files[cand].path = Z_StrDup(path);
		memcpy(files[cand].hash, hash, QKU_HEXLEN+1);
		files[cand].size = size;
		files[cand].exec = JSON_GetInteger(ent, "exec", 0) ? true : false;
		files[cand].status = QKUF_PENDING;
		cand++;
	}

	// Refuse before compacting: the original candidate array still owns every path.
	if (qku_ftesurf)
		for (i = 0; i < cand; i++)
		{
			enum qku_preserve_e protect = QKU_FSPreserveFile(&st, files[i].path, files[i].hash);
			if (protect == QKUP_UNREADABLE)
			{
				Q_snprintfz(plan->reason, sizeof(plan->reason), "cannot read protected update file: %.128s", files[i].path);
				goto done;
			}
			files[i].preserved = protect == QKUP_PRESERVE;
		}

	//Only now, for the handful of candidates, touch the disk: the ledger can be behind reality
	//(a hand-copied file, a previous partial apply), and re-downloading something already
	//correct is pure waste.
	{
		size_t keep = 0;
		for (i = 0; i < cand; i++)
		{
			char abs[MAX_OSPATH], have[QKU_HEXLEN+1];
			quint64_t sz = 0;
			QKU_InstallPath(abs, sizeof(abs), files[i].path);
			if (qku_ftesurf && files[i].preserved)
			{ Z_Free(files[i].path); continue; }
			if (QKU_HashFile(abs, &sz, have) && sz == files[i].size && !strcmp(have, files[i].hash))
			{
				Z_Free(files[i].path);
				continue;
			}
			if (keep != i)
				files[keep] = files[i];
			keep++;
		}
		cand = keep;
	}

	if (args->verifyonly)
	{	//report-only: never downloads, never writes. the diagnostic for "does my install match?"
		Q_snprintfz(plan->reason, sizeof(plan->reason), "%u of %u manifest files differ on disk",
					(unsigned)cand, (unsigned)count);
		//deliberately does NOT hand the list over as an appliable plan -- a full repair is the
		//launcher's job, and offering "update now" here would silently exceed the caps.
		plan->verdict = QKUS_UPTODATE;
		Con_Printf("update_verify: %s\n", plan->reason);
		for (i = 0; i < cand; i++)
			Con_Printf("  differs: %s\n", files[i].path);
		goto done;
	}

	if (!cand)
	{
		plan->verdict = QKUS_UPTODATE;
		goto done;
	}

	if (qku_ftesurf)
	{
		haveengine = false;
		for (i = 0; i < cand; i++) if (!QKU_FSInGame(files[i].path)) haveengine = true;
	}
	if (haveengine && (qku_ftesurf || !update_engine_policy.ival))
	{
		//The exe and its four native plugins compile the same engine headers and are only ABI
		//compatible as a matched set; the plugin gate checks the function table but there is no
		//data-struct canary, so a mismatched pair loads happily and corrupts memory at map load.
		Q_strncpyz(plan->reason, "update includes engine binaries -- use the launcher", sizeof(plan->reason));
		plan->verdict = QKUS_LAUNCHER;
		goto done;
	}

	plan->totalbytes = 0;
	for (i = 0; i < cand; i++)
		plan->totalbytes += files[i].size;

	if (cand > maxfiles || plan->totalbytes > maxbytes)
	{
		Q_snprintfz(plan->reason, sizeof(plan->reason), "update is %u files / %.0f MB -- too big for in-game, use the launcher",
					(unsigned)cand, (double)plan->totalbytes / (1024.0*1024.0));
		plan->verdict = QKUS_LAUNCHER;
		goto done;
	}

	//Paths the manifest retired. Nothing else prunes -- both this and the launcher only ever add
	//or overwrite -- so without acting on these a file dropped from the manifest lingers forever.
	//Validated exactly like a file path: this list is attacker-controlled JSON aimed at unlink().
	//
	//Applied only alongside a real delta. A manifest whose ONLY change is removals leaves the
	//version fast-path reporting "uptodate", and the pruning waits for the launcher -- acceptable
	//because a removal-only release has nothing else to hand out either.
	{
		json_t *rem = JSON_FindChild(root, "removed");
		size_t nrem = rem ? JSON_GetCount(rem) : 0;
		if (nrem)
		{
			plan->removed = Z_Malloc(sizeof(*plan->removed) * nrem);
			for (i = 0; i < nrem; i++)
			{
				char rp[MAX_OSPATH];
				json_t *n = JSON_GetIndexed(rem, i);
				if (!n || !JSON_ReadBody(n, rp, sizeof(rp)))
					continue;
				if (!QKU_ValidPath(rp))
				{
					Con_DPrintf(CON_WARNING "update: ignoring unsafe \"removed\" entry\n");
					continue;
				}
				plan->removed[plan->numremoved++] = Z_StrDup(rp);
			}
		}
	}

	plan->files = files;
	plan->numfiles = cand;
	files = NULL;
	if (!args->verifyonly)
		plan->verdict = QKUS_AVAILABLE;

done:
	if (files)
		QKU_FreePlanFiles(files, cand);
	if (root)
		JSON_Destroy(root);
	if (payload) Z_Free(payload);
	QKU_FreeState(&st);
	if (qku_ftesurf && (plan->verdict == QKUS_AVAILABLE || plan->verdict == QKUS_UPTODATE)) plan->envelope = Z_StrDup(args->text);
	BZ_Free(args->text);
	Z_Free(args);
	QKU_PostPlan(plan);
}

// ---------------------------------------------------------------------------------------
// manifest fetch
// ---------------------------------------------------------------------------------------

struct qku_fetchresult_s
{
	unsigned int	seq;
	qboolean		force;
	qboolean		verifyonly;
	unsigned int	replycode;
	unsigned int	gotsize;		//what the transfer produced, for diagnosing a "200 but nothing"
	int				readsize;
	char			*text;			//NULL on failure
};

static void QKU_Main_ManifestGot(int iarg, void *data)
{
	struct qku_fetchresult_s *r = *(struct qku_fetchresult_s **)data;
	struct qku_checkargs_s *args;
	(void)iarg;

	if (r->seq != qku.seq || qku.state != QKUS_CHECKING)
	{	//cancelled, or superseded by a newer check
		if (r->text)
			BZ_Free(r->text);
		Z_Free(r);
		return;
	}

	if (!r->text)
	{
		QKU_Fail(QKUS_ERROR, "manifest fetch failed (http %u, %u bytes, read %i)",
					r->replycode, r->gotsize, r->readsize);
		Z_Free(r);
		return;
	}

	args = Z_Malloc(sizeof(*args));
	args->text = r->text;
	args->seq = r->seq;
	args->force = r->force;
	args->verifyonly = r->verifyonly;
	Z_Free(r);

	//JSON parse + 4,243-entry delta + a few file hashes: off the main thread so the menu does
	//not hitch. json.c uses plain malloc, so this is safe on the loader.
	COM_AddWork(WG_LOADER, QKU_Work_Check, NULL, args, 0, 0);
}

//DOWNLOAD WORKER THREAD. Read the download, copy what we need, hop to main. Nothing else.
static void QKU_ManifestNotify(struct dl_download *dl)
{
	struct qku_fetchresult_s *r = Z_Malloc(sizeof(*r));
	r->seq = dl->user_num;
	r->force = (dl->user_sequence & 1) ? true : false;
	r->verifyonly = (dl->user_sequence & 2) ? true : false;
	r->replycode = dl->replycode;

	if (dl->file && dl->status != DL_FAILED)
	{
		qofs_t sz = VFS_GETLEN(dl->file);
		r->gotsize = (unsigned)sz;
		if (sz > 0 && sz < QKU_MAXMANIFEST)
		{
			char *buf = BZ_Malloc(sz+1);
			int got = VFS_READ(dl->file, buf, sz);
			r->readsize = got;
			if (got == (int)sz)
			{
				//Terminate BEFORE measuring. Getting this backwards reads uninitialised heap,
				//which is a bug that hides: at boot the allocation lands on fresh zeroed pages
				//and the check passes, but the same code called later off a dirty heap rejects
				//a perfectly good manifest with a bare "http 200".
				buf[sz] = 0;
				if (strlen(buf) == sz)
					r->text = buf;
				else
					BZ_Free(buf);	//an embedded NUL: parser and strlen would disagree on where the document ends
			}
			else
				BZ_Free(buf);
		}
	}
	Cmd_AddTimer(0, QKU_Main_ManifestGot, 0, &r, sizeof(struct qku_fetchresult_s *));
}

static void QKU_BeginCheck(qboolean force, qboolean verifyonly)
{
	struct dl_download *dl;
	const char *url = update_manifest_url.string;

	if (!qku_profileknown) return;
	if (qku_ftesurf && Cmd_IsInsecure() && !qku_menuaction) return;
	if (qku_ftesurf && update_consent.ival != 1) { QKU_SetState(QKUS_DISABLED); return; }
	if (!update_enabled.ival)
	{
		QKU_SetState(QKUS_DISABLED);
		return;
	}
	if (qku.state == QKUS_CHECKING || qku.state == QKUS_DOWNLOADING || qku.state == QKUS_APPLYING)
		return;		//already busy
	if (qku.state == QKUS_RESTART)
		return;		//an update is already staged; checking again would only confuse the player

	if (!*QKU_PLATFORM)
	{
		QKU_Fail(QKUS_LAUNCHER, "this platform is not covered by the manifest");
		return;
	}
	if (Q_strncasecmp(url, "https://", 8) && !update_allow_insecure.ival)
	{
		QKU_Fail(QKUS_ERROR, "update_manifest_url must be https (or set update_allow_insecure 1)");
		return;
	}

	QKU_ClearPlan();
	qku.seq++;
	qku.error[0] = 0;
	Cvar_ForceSet(&update_error, "");
	QKU_SetState(QKUS_CHECKING);

	dl = HTTP_CL_Get(url, NULL, QKU_ManifestNotify);
	if (!dl)
	{
		QKU_Fail(QKUS_ERROR, "could not contact %s", url);
		return;
	}
	dl->user_num = qku.seq;
	dl->user_sequence = (force?1:0) | (verifyonly?2:0);
	dl->file = VFSPIPE_Open(1, false);
	dl->isquery = true;			//keeps it out of the ingame download progress bar
	dl->sizelimit = QKU_MAXMANIFEST;
	DL_CreateThread(dl, NULL, NULL);
}

// ---------------------------------------------------------------------------------------
// adopting a finished plan (main)
// ---------------------------------------------------------------------------------------

static void QKU_Main_AdoptPlan(int iarg, void *data)
{
	qku_plan_t *plan = *(qku_plan_t **)data;
	(void)iarg;

	if (plan->seq != qku.seq)
	{	//stale: a cancel or a newer check landed while we were parsing
		QKU_FreePlanFiles(plan->files, plan->numfiles);
		QKU_FreeStrings(plan->removed, plan->numremoved);
		if (plan->envelope) Z_Free(plan->envelope);
		Z_Free(plan);
		return;
	}

	QKU_ClearPlan();
	Q_strncpyz(qku.version, plan->version, sizeof(qku.version));
	Q_strncpyz(qku.objectbase, plan->objectbase, sizeof(qku.objectbase));
	if (*plan->version)
		Cvar_ForceSet(&update_version, plan->version);

	switch (plan->verdict)
	{
	case QKUS_UPTODATE:
		if (qku_ftesurf && !QKU_FSSaveEnvelope(plan->envelope, ".ftesurf-updater/installed.json"))
		{ QKU_Fail(QKUS_ERROR, "cannot save the verified offline update manifest"); break; }
		QKU_SetState(QKUS_UPTODATE);
		Con_DPrintf("update: up to date (%s)\n", plan->version);
		break;
	case QKUS_AVAILABLE:
		qku.envelope = plan->envelope;
		plan->envelope = NULL;
		qku.files = plan->files;
		qku.numfiles = plan->numfiles;
		qku.totalbytes = plan->totalbytes;
		qku.removed = plan->removed;
		qku.numremoved = plan->numremoved;
		plan->files = NULL;
		plan->removed = NULL;
		plan->numremoved = 0;
		QKU_PublishCounts();
		QKU_SetState(QKUS_AVAILABLE);
		Con_Printf("update: %u file%s (%.1f MB) available -> %s\n", (unsigned)qku.numfiles,
					qku.numfiles==1?"":"s", (double)qku.totalbytes/(1024.0*1024.0), plan->version);
		if (*plan->reason)
			Con_Printf("update: %s\n", plan->reason);
		break;
	case QKUS_LAUNCHER:
		QKU_Fail(QKUS_LAUNCHER, "%s", *plan->reason?plan->reason:"use the launcher");
		break;
	default:
		QKU_Fail(QKUS_ERROR, "%s", *plan->reason?plan->reason:"update check failed");
		break;
	}

	QKU_FreePlanFiles(plan->files, plan->numfiles);
	QKU_FreeStrings(plan->removed, plan->numremoved);
	if (plan->envelope) Z_Free(plan->envelope);
	Z_Free(plan);
}

// ---------------------------------------------------------------------------------------
// downloading objects
// ---------------------------------------------------------------------------------------

struct qku_objresult_s
{
	unsigned int	seq;
	size_t			index;
	qboolean		ok;
	unsigned int	replycode;
};

static void QKU_Main_ObjectGot(int iarg, void *data);

//DOWNLOAD WORKER THREAD.
static void QKU_ObjectNotify(struct dl_download *dl)
{
	struct qku_objresult_s *r = Z_Malloc(sizeof(*r));
	r->seq = dl->user_num;
	r->index = (size_t)dl->user_sequence;
	r->replycode = dl->replycode;
	r->ok = (dl->status == DL_FINISHED);

	//Closing it ourselves is what surfaces the verification result: dl->file is wrapped in
	//FS_Hash_ValidateWrites, whose Close fails on either a size or a digest mismatch. Clearing
	//the pointer stops the http layer closing it a second time.
	if (dl->file)
	{
		if (!VFS_CLOSE(dl->file))
			r->ok = false;
		dl->file = NULL;
	}
	else
		r->ok = false;

	Cmd_AddTimer(0, QKU_Main_ObjectGot, 0, &r, sizeof(struct qku_objresult_s *));
}

static void QKU_StartOne(size_t i)
{
	qku_file_t *fi = &qku.files[i];
	char part[MAX_OSPATH], url[MAX_OSPATH*2];
	const char *base = update_manifest_url.string;
	char root[MAX_OSPATH];
	char *slash;
	struct dl_download *dl;
	vfsfile_t *out;

	//"https://host/manifests/alpha.json" -> "https://host/". Derived from the manifest url
	//rather than a separate cvar so the two can never point at different hosts.
	Q_strncpyz(root, base, sizeof(root));
	slash = strstr(root, "://");
	slash = strchr(slash ? slash+3 : root, '/');
	if (slash)
		*slash = 0;
	Q_snprintfz(url, sizeof(url), "%s/objects/%c%c/%s", qku_ftesurf ? qku.objectbase : root, fi->hash[0], fi->hash[1], fi->hash);

	if (!QKU_FSStageNoLinks("staged/%s.part", fi->hash) || !QKU_FSStageNoLinks("staged/%s", fi->hash))
	{ fi->status = QKUF_FAILED; QKU_Fail(QKUS_ERROR, "linked update staging path; nothing installed"); return; }
	QKU_StagePath(part, sizeof(part), "staged/%s.part", fi->hash);
	FS_CreatePath(part, FS_SYSTEM);
	out = FS_OpenVFS(part, "wb", FS_SYSTEM);
	if (out)
		out = FS_Hash_ValidateWrites(out, fi->path, fi->size, &hash_blake2b_256, fi->hash);
	if (!out)
	{
		fi->status = QKUF_FAILED;
		QKU_Fail(QKUS_ERROR, "could not write %s", part);
		return;
	}

	dl = HTTP_CL_Get(url, NULL, QKU_ObjectNotify);
	if (!dl)
	{
		VFS_CLOSE(out);
		fi->status = QKUF_FAILED;
		QKU_Fail(QKUS_ERROR, "could not contact %s", root);
		return;
	}
	dl->user_num = qku.seq;
	dl->user_sequence = (int)i;
	dl->file = out;
	dl->isquery = true;
	dl->sizelimit = fi->size;

	fi->status = QKUF_ACTIVE;
	qku.active++;
	Cvar_ForceSet(&update_current, fi->path);
	DL_CreateThread(dl, NULL, NULL);
}

static void QKU_Apply(void);

static void QKU_StartDownloads(void)
{
	int cap = update_concurrency.ival;
	size_t i;

	if (qku.state != QKUS_DOWNLOADING)
		return;
	if (cap < 1) cap = 1;
	if (cap > 3) cap = 3;	//MAXDOWNLOADTHREADS is 4; leave one for connect-time map downloads

	for (i = 0; i < qku.numfiles && qku.active < cap; i++)
		if (qku.files[i].status == QKUF_PENDING)
		{
			size_t j;
			if (qku_ftesurf)
			{
				for (j = 0; j < i; j++)
					if (!strcmp(qku.files[j].hash, qku.files[i].hash)) break;
				if (j < i) continue; // the first entry owns this hash's transfer
			}
			QKU_StartOne(i);
			if (qku.state != QKUS_DOWNLOADING) return;
		}

	if (!qku.active)
	{	//nothing running and nothing startable: we are done one way or the other
		for (i = 0; i < qku.numfiles; i++)
			if (qku.files[i].status == QKUF_FAILED)
			{
				QKU_Fail(QKUS_ERROR, "download failed: %s", qku.files[i].path);
				return;
			}
		QKU_Apply();
	}
}

static void QKU_Main_ObjectGot(int iarg, void *data)
{
	struct qku_objresult_s *r = *(struct qku_objresult_s **)data;
	qku_file_t *fi;
	(void)iarg;

	if (r->seq != qku.seq || qku.state != QKUS_DOWNLOADING || r->index >= qku.numfiles)
	{
		Z_Free(r);
		return;
	}
	fi = &qku.files[r->index];
	qku.active--;

	if (r->ok && (!QKU_FSStageNoLinks("staged/%s.part", fi->hash) || !QKU_FSStageNoLinks("staged/%s", fi->hash))) r->ok = false;
	if (r->ok)
	{
		char part[MAX_OSPATH], full[MAX_OSPATH];
		QKU_StagePath(part, sizeof(part), "staged/%s.part", fi->hash);
		QKU_StagePath(full, sizeof(full), "staged/%s", fi->hash);
		//Sys_Rename is MoveFileW WITHOUT MOVEFILE_REPLACE_EXISTING, so it fails outright if the
		//destination exists. Always clear it first -- this is the single easiest way to get a
		//silent "the update did nothing".
		FS_Remove(full, FS_SYSTEM);
		if (FS_Rename(part, full, FS_SYSTEM))
		{
			//the filename is now the proof of verification: only a hash-checked body gets here
			fi->status = QKUF_STAGED;
			qku.numdone++;
			qku.donebytes += fi->size;
		}
		else
			fi->status = QKUF_FAILED;
	}
	else
	{
		char part[MAX_OSPATH];
		QKU_StagePath(part, sizeof(part), "staged/%s.part", fi->hash);
		if (QKU_FSStageNoLinks("staged/%s.part", fi->hash)) FS_Remove(part, FS_SYSTEM); //no HTTP resume
		fi->status = QKUF_FAILED;
		Con_Printf(CON_WARNING "update: %s failed (http %u)\n", fi->path, r->replycode);
	}
	if (qku_ftesurf)
	{
		size_t j;
		for (j = 0; j < qku.numfiles; j++)
			if (qku.files[j].status == QKUF_PENDING && !strcmp(qku.files[j].hash, fi->hash))
			{
				qku.files[j].status = qku.files[j].size == fi->size ? fi->status : QKUF_FAILED;
				if (qku.files[j].status == QKUF_STAGED)
				{ qku.numdone++; qku.donebytes += qku.files[j].size; }
			}
	}
	Z_Free(r);

	QKU_PublishCounts();
	QKU_StartDownloads();
}

// ---------------------------------------------------------------------------------------
// journal + apply
// ---------------------------------------------------------------------------------------

//Written before the first rename and rewritten as entries land, so a crash mid-apply is
//recoverable: at any instant a destination is the old file, absent for microseconds, or the new
//file -- and if absent, replay reinstalls it on the next boot.
static void QKU_WriteJournal(int attempts)
{
	char path[MAX_OSPATH], tmp[MAX_OSPATH];
	vfsfile_t *f;
	size_t i;
	qboolean first = true;

	if (!QKU_FSStageNoLinks("%s", "journal.json") || !QKU_FSStageNoLinks("%s", "journal.tmp")) return;
	QKU_StagePath(path, sizeof(path), "%s", "journal.json");
	QKU_StagePath(tmp, sizeof(tmp), "%s", "journal.tmp");
	FS_CreatePath(tmp, FS_SYSTEM);
	f = FS_OpenVFS(tmp, "wb", FS_SYSTEM);
	if (!f)
		return;

	VFS_PRINTF(f, "{\n\"version\":\"%s\",\n\"attempts\":%i,\n\"files\":[\n", qku.version, attempts);
	for (i = 0; i < qku.numfiles; i++)
	{
		if (qku.files[i].status != QKUF_STAGED)
			continue;	//already installed, or never staged: nothing to replay
		VFS_PRINTF(f, "%s{\"p\":\"%s\",\"h\":\"%s\",\"s\":%.0f,\"x\":%i}",
					first?"":",\n", qku.files[i].path, qku.files[i].hash,
					(double)qku.files[i].size, qku.files[i].exec?1:0);
		first = false;
	}
	VFS_PRINTF(f, "\n]\n}\n");
	if (!VFS_CLOSE(f))
		return;

	FS_Remove(path, FS_SYSTEM);
	FS_Rename(tmp, path, FS_SYSTEM);
}

static void QKU_FSPurgeObject(const char *hash, quint64_t size)
{
	char path[MAX_OSPATH], have[QKU_HEXLEN+1];
	quint64_t sz;
	if (!QKU_ValidHash(hash) || !QKU_FSStageNoLinks("staged/%s", hash)) return;
	QKU_StagePath(path, sizeof(path), "staged/%s", hash);
	if (QKU_HashFile(path, &sz, have) && sz == size && !strcmp(have, hash)) FS_Remove(path, FS_SYSTEM);
}

static void QKU_PurgeStaging(void)
{
	char path[MAX_OSPATH];
	size_t i;
	if (!QKU_FSStageNoLinks("%s", "journal.json")) return;
	QKU_StagePath(path, sizeof(path), "%s", "journal.json");
	FS_Remove(path, FS_SYSTEM);
	if (qku_ftesurf)
		for (i = 0; i < qku.numfiles; i++) QKU_FSPurgeObject(qku.files[i].hash, qku.files[i].size);
	// Only Quakers consumes objects by rename. Displaced originals remain until next boot.
}

//Delete the displaced originals from a previous session. Safe here and nowhere else: at boot
//nothing is mounted, so nothing holds them. Anything that somehow survives is retried next boot.
static int QDECL QKU_SweepOld_Enum(const char *fname, qofs_t fsize, time_t mtime, void *parm, searchpathfuncs_t *spath)
{
	char full[MAX_OSPATH];
	(void)fsize; (void)mtime; (void)spath;
	Q_snprintfz(full, sizeof(full), "%s%s", (const char*)parm, fname);
	FS_Remove(full, FS_SYSTEM);
	return true;
}
static void QKU_SweepOld(void)
{
	char dir[MAX_OSPATH];
	if (!QKU_FSStageNoLinks("%s", "old")) Sys_Error("Linked updater backup directory: refusing to start");
	Q_snprintfz(dir, sizeof(dir), "%s%s/old/", com_gamepath, QKU_STAGEDIR);
	Sys_EnumerateFiles(dir, "*.bin", QKU_SweepOld_Enum, dir, NULL);
}

// ---------------------------------------------------------------------------------------
// the install ledger
// ---------------------------------------------------------------------------------------

//Rewrite quakers-launcher.json so the launcher agrees with what the engine just did.
//Skipping this is not merely untidy: the launcher would still believe the OLD hashes and
//re-download every file we installed the next time a tester runs it. Worse, the two would then
//disagree about what is on disk, which is exactly the drift the shared ledger exists to prevent.
//
//Asymmetric on purpose: record every path that actually landed, but bump `version` ONLY when
//the whole plan landed. Worst case the launcher re-fetches something we already installed --
//wasted bytes. The other way round would leave a silently stale install claiming to be current.
struct qku_record_s
{
	const char *path;
	const char *hash;
};

static void QKU_SanitiseVersion(char *out, size_t outsize, const char *in)
{	//it came from JSON, and it is about to be written back into JSON unescaped
	size_t o = 0;
	for (; *in && o < outsize-1; in++)
		if ((*in >= '0' && *in <= '9') || (*in >= 'a' && *in <= 'z') ||
			(*in >= 'A' && *in <= 'Z') || *in == '.' || *in == '_' || *in == '-' || *in == '+')
			out[o++] = *in;
	out[o] = 0;
}

static void QKU_WriteState(const struct qku_record_s *recs, size_t n, const char *newversion)
{
	char path[MAX_OSPATH], tmp[MAX_OSPATH], tmprel[MAX_OSPATH], ver[64];
	char *text;
	json_t *root = NULL, *files = NULL, *c;
	vfsfile_t *f;
	qboolean *used;
	size_t i;
	qboolean first = true;

	if (!n && !newversion)
		return;

	Q_snprintfz(path, sizeof(path), "%s%s", com_gamepath, QKU_STATEFILE);
	//NOT ".json.tmp" -- that is the launcher's own temp name, and a launcher running at
	//the same moment would have both of us writing the same file.
	Q_snprintfz(tmprel, sizeof(tmprel), "%s.enginetmp", QKU_STATEFILE);
	QKU_InstallPath(tmp, sizeof(tmp), tmprel);
	if (qku_ftesurf && (!QKU_FSNoLinks(QKU_STATEFILE) || !QKU_FSNoLinks(tmprel)))
	{
		Con_Printf(CON_WARNING "update: linked install ledger metadata; refusing to write\n");
		return;
	}

	text = QKU_ReadTextFile(path, 64*1024*1024);
	if (!text && !qku_ftesurf)
	{	//pre-2026-07-28 layout: merge onto the old ledger, then write the new one. We do NOT
		//delete the old file -- the launcher owns that, and it only does so after a clean save.
		char old[MAX_OSPATH];
		Q_snprintfz(old, sizeof(old), "%s%s/state.json", com_gamepath, QKU_STATEDIR_OLD);
		text = QKU_ReadTextFile(old, 64*1024*1024);
	}
	if (text)
	{
		root = JSON_Parse(text);
		if (root)
			files = JSON_FindChild(root, "files");
	}

	if (newversion && *newversion)
		QKU_SanitiseVersion(ver, sizeof(ver), newversion);
	else if (root)
		JSON_GetString(root, "version", ver, sizeof(ver), "");
	else
		*ver = 0;

	used = Z_Malloc(sizeof(*used) * (n?n:1));

	f = NULL;
	if (!qku_ftesurf || (QKU_FSNoLinks(QKU_STATEFILE) && QKU_FSNoLinks(tmprel)))
	{
		FS_CreatePath(tmp, FS_SYSTEM);
		f = FS_OpenVFS(tmp, "wb", FS_SYSTEM);
	}
	if (!f)
	{
		Z_Free(used);
		if (root) JSON_Destroy(root);
		if (text) BZ_Free(text);
		Con_Printf(CON_WARNING "update: could not write the install ledger; the launcher will re-fetch these files\n");
		return;
	}

	//no escaping needed: QKU_ValidPath already rejected quotes, backslashes and control chars,
	//and every hash is 64 lowercase hex.
	VFS_PRINTF(f, "{\n  \"version\": \"%s\",\n  \"files\": {\n", ver);
	if (files)
	{
		for (c = files->child; c; c = c->sibling)
		{
			const char *hash = NULL;
			size_t k;
			qboolean retired = false;

			//A path we just deleted must leave the ledger too, or every future run believes it
			//is still installed and the entry outlives the file.
			for (k = 0; k < qku.numremoved; k++)
				if (!strcmp(qku.removed[k], c->name))
				{
					retired = true;
					break;
				}
			if (retired)
				continue;

			for (i = 0; i < n; i++)
				if (!strcmp(recs[i].path, c->name))
				{
					hash = recs[i].hash;
					used[i] = true;
					break;
				}
			if (hash)
				VFS_PRINTF(f, "%s    \"%s\": \"%s\"", first?"":",\n", c->name, hash);
			else
				VFS_PRINTF(f, "%s    \"%s\": \"%.*s\"", first?"":",\n", c->name,
							(int)(c->bodyend - c->bodystart), c->bodystart);
			first = false;
		}
	}
	for (i = 0; i < n; i++)
		if (!used[i])
		{
			VFS_PRINTF(f, "%s    \"%s\": \"%s\"", first?"":",\n", recs[i].path, recs[i].hash);
			first = false;
		}
	VFS_PRINTF(f, "\n  }\n}\n");

	Z_Free(used);
	if (root) JSON_Destroy(root);
	if (text) BZ_Free(text);

	if (!VFS_CLOSE(f))
	{
		Con_Printf(CON_WARNING "update: install ledger write failed\n");
		return;
	}
	//Sys_Rename will not overwrite, so clear the destination first
	FS_Remove(path, FS_SYSTEM);
	if (!FS_Rename(tmp, path, FS_SYSTEM))
		Con_Printf(CON_WARNING "update: could not replace the install ledger\n");
}

//Install one staged object over its destination.
//
//The Win32 asymmetry this relies on: a file held open by this process cannot be DELETED or
//overwritten, but it CAN be renamed -- only the directory entry moves, and the running code
//keeps reading the same inode. So a locked pk3 or dll is moved aside rather than fought with.
//Returns false only for a destination we could neither remove nor move (antivirus, another
//process); those stay in the journal and are retried next boot.
static qboolean QKU_InstallOne(const char *relpath, const char *hash, quint64_t size, qboolean exec, int *oldcounter)
{
	char staged[MAX_OSPATH], dest[MAX_OSPATH], old[MAX_OSPATH];

	QKU_StagePath(staged, sizeof(staged), "staged/%s", hash);
	QKU_InstallPath(dest, sizeof(dest), relpath);
	if (qku_ftesurf)
	{
		char copy[MAX_OSPATH];
		qbyte block[65536];
		int r = 0;
		qboolean ok = true;
		vfsfile_t *in, *out;
		if (!QKU_FSNoLinks(relpath) || !QKU_FSStageNoLinks("staged/%s", hash) || !QKU_FSStageNoLinks("staged/%s.install", hash)) return false;
		QKU_StagePath(copy, sizeof(copy), "staged/%s.install", hash);
		in = FS_OpenVFS(staged, "rb", FS_SYSTEM);
		if (!in) return false;
		out = FS_OpenVFS(copy, "wb", FS_SYSTEM);
		if (out) out = FS_Hash_ValidateWrites(out, relpath, size, &hash_blake2b_256, hash);
		if (!out) { VFS_CLOSE(in); return false; }
		while ((r = VFS_READ(in, block, sizeof(block))) > 0)
			if (VFS_WRITE(out, block, r) != r) { ok = false; break; }
		VFS_CLOSE(in);
		if (!VFS_CLOSE(out) || r < 0) ok = false;
		if (!ok) { FS_Remove(copy, FS_SYSTEM); return false; }
		// Keep the verified object until every destination/recovery entry has landed.
		Q_strncpyz(staged, copy, sizeof(staged));
	}
	FS_CreatePath(dest, FS_SYSTEM);

	if (!FS_Remove(dest, FS_SYSTEM))
	{	//Sys_remove returns false with ERROR_ACCESS_DENIED when a handle holds the file -- that
		//IS the lock test, so no extension-based guessing is needed.
		char n[32];
		Q_snprintfz(n, sizeof(n), "%i", (*oldcounter)++);
		if (!QKU_FSStageNoLinks("old/%s.bin", n)) { if (qku_ftesurf) FS_Remove(staged, FS_SYSTEM); return false; }
		QKU_StagePath(old, sizeof(old), "old/%s.bin", n);
		FS_CreatePath(old, FS_SYSTEM);
		if (!FS_Rename(dest, old, FS_SYSTEM))
		{
			//absent is fine (a brand new file); genuinely locked-and-unmovable is not
			vfsfile_t *probe = FS_OpenVFS(dest, "rb", FS_SYSTEM);
			if (probe)
			{
				VFS_CLOSE(probe);
				if (qku_ftesurf) FS_Remove(staged, FS_SYSTEM);
				return false;
			}
		}
	}

	if (!FS_Rename(staged, dest, FS_SYSTEM))
	{ if (qku_ftesurf) FS_Remove(staged, FS_SYSTEM); return false; }

#ifndef _WIN32
	if (exec)
	{	//without this the downloaded engine is byte-perfect and still won't start
		char sys[MAX_OSPATH];
		if (FS_SystemPath(dest, FS_SYSTEM, sys, sizeof(sys)))
			chmod(sys, 0755);
	}
#else
	(void)exec;
#endif
	return true;
}

static void QKU_Apply(void)
{
	size_t i;
	int oldcounter = 0;
	size_t deferred = 0, installed = 0;

	if (update_stage_only.ival)
	{
		Con_Printf("update: staged %u files, update_stage_only is set so nothing was installed\n", (unsigned)qku.numdone);
		QKU_SetState(QKUS_RESTART);
		return;
	}

#ifdef HAVE_CLIENT
	if (qku_ftesurf && cls.state != ca_disconnected)
	{ Con_Printf("update: downloaded; disconnect before installing\n"); QKU_SetState(QKUS_AVAILABLE); return; }
#endif
#ifdef HAVE_SERVER
	if (qku_ftesurf && sv.state != ss_dead)
	{ Con_Printf("update: stop the local server before installing\n"); QKU_SetState(QKUS_AVAILABLE); return; }
#endif
	QKU_SetState(QKUS_APPLYING);
	if (qku_ftesurf && !QKU_FSSaveEnvelope(qku.envelope, ".ftesurf-updater/ingame/manifest.json"))
	{ QKU_Fail(QKUS_ERROR, "cannot save the signed recovery manifest; nothing installed"); return; }
	QKU_WriteJournal(1);
	if (qku_ftesurf)
	{
		char path[MAX_OSPATH], *text;
		json_t *journal;
		QKU_InstallPath(path, sizeof(path), ".ftesurf-updater/ingame/journal.json");
		text = QKU_ReadTextFile(path, 8*1024*1024);
		journal = text ? JSON_Parse(text) : NULL;
		if (!journal || JSON_GetCount(JSON_FindChild(journal, "files")) != qku.numfiles)
		{ if (journal) JSON_Destroy(journal); if (text) BZ_Free(text); QKU_Fail(QKUS_ERROR, "cannot save the complete recovery journal; nothing installed"); return; }
		JSON_Destroy(journal); BZ_Free(text);
	}

	for (i = 0; i < qku.numfiles; i++)
	{
		if (qku.files[i].status != QKUF_STAGED)
			continue;
		if (QKU_InstallOne(qku.files[i].path, qku.files[i].hash, qku.files[i].size, qku.files[i].exec, &oldcounter))
		{
			qku.files[i].status = QKUF_DONE;
			installed++;
		}
		else
		{
			deferred++;
			Con_Printf(CON_WARNING "update: %s is locked, will finish on next launch\n", qku.files[i].path);
		}
	}

	//Delete what the manifest retired, but only now that the installs are done -- pruning while
	//downloads were still failing could leave the install missing both the old file and its
	//replacement. Every path was validated by QKU_ValidPath during the delta, so none of these
	//can reach outside the install root.
	for (i = 0; i < qku.numremoved; i++)
	{
		char abs[MAX_OSPATH];
		QKU_InstallPath(abs, sizeof(abs), qku.removed[i]);
		if (FS_Remove(abs, FS_SYSTEM))
			Con_DPrintf("update: removed %s\n", qku.removed[i]);
	}

	//Tell the filesystem its view is stale. NOT FS_ReloadPackFiles: fs.c flags that as
	//"potentially unsafe, needs lots of testing", and the rename dance already produced the
	//correct on-disk result. This is a flag set, not a rebuild.
	FS_FlushFSHashFull();

	if (qku_ftesurf && !deferred)
	{
		char *payload = NULL;
		json_t *signedroot = QKU_FSParseSigned(qku.envelope, &payload);
		qboolean complete = signedroot && QKU_FSComplete(signedroot);
		if (signedroot) JSON_Destroy(signedroot);
		if (payload) Z_Free(payload);
		if (!complete) { QKU_Fail(QKUS_LAUNCHER, "installed set is incomplete; run the launcher to repair"); return; }
	}
	if (installed)
	{	//hand the launcher an accurate ledger, or it re-downloads everything we just installed
		struct qku_record_s *recs = Z_Malloc(sizeof(*recs) * installed);
		size_t n = 0;
		for (i = 0; i < qku.numfiles; i++)
			if (qku.files[i].status == QKUF_DONE)
			{
				recs[n].path = qku.files[i].path;
				recs[n].hash = qku.files[i].hash;
				n++;
			}
		QKU_WriteState(recs, n, deferred ? NULL : qku.version);
		Z_Free(recs);
	}

	if (deferred)
	{
		QKU_WriteJournal(1);	//keep only what is still outstanding
		Con_Printf("update: installed %u of %u files; restart to finish\n", (unsigned)installed, (unsigned)(installed+deferred));
	}
	else
	{
		if (qku_ftesurf && !QKU_FSSaveEnvelope(qku.envelope, ".ftesurf-updater/installed.json"))
		{ QKU_Fail(QKUS_ERROR, "update installed but offline manifest could not be saved; restart to recover"); return; }
		QKU_PurgeStaging();
		Con_Printf("update: installed %u files\n", (unsigned)installed);
	}

	//ALWAYS "restart to finish", never "carry on playing". 14 pk3s stay open for the session and
	//assets are cached in RAM; the nastiest mixed state is a changed csprogs.dat whose CRC no
	//longer matches what a server expects, so the player silently cannot connect anywhere.
	QKU_SetState(QKUS_RESTART);
	Cvar_ForceSet(&update_current, "");
}

// ---------------------------------------------------------------------------------------
// crash recovery, from the tail of COM_InitFilesystem
// ---------------------------------------------------------------------------------------

void QKU_ReplayJournal(void)
{
	static qboolean alreadyran = false;
	char path[MAX_OSPATH];
	char jver[64];
	char *text, *signedtext = NULL, *signedpayload = NULL;
	json_t *root, *files, *ent, *signedroot = NULL;
	qboolean *preserved = NULL;
	size_t i, count, pending = 0, done = 0;
	int attempts, oldcounter = 1000;	//distinct range so replay cannot collide with a live apply

	//The call site fires on every gamedir change, but only the FIRST one is at a point where
	//nothing is mounted. Replaying later could try to replace a pk3 the session has open.
	if (alreadyran)
		return;
	if (!*com_gamepath || !fs_manifest)
		return;
	alreadyran = true;
	// Public gamedir is assigned only when paths mount, AFTER this recovery hook.
	qku_ftesurf = fs_manifest->installation && !Q_strcasecmp(fs_manifest->installation, "FTESurf");
	qku_profileknown = true;
	if (qku_ftesurf) QKU_FSLock();
	if (qku_initialised) QKU_ConfigureProfile();

	QKU_SweepOld();		//displaced originals from a previous session; nothing holds them now

	if (!QKU_FSStageNoLinks("%s", "journal.json") || !QKU_FSStageNoLinks("%s", "journal.tmp") || !QKU_FSStageNoLinks("%s", "manifest.json")) Sys_Error("Linked update recovery metadata: refusing to start");
	Q_snprintfz(path, sizeof(path), "%s%s/journal.json", com_gamepath, QKU_STAGEDIR);
	text = QKU_ReadTextFile(path, 8*1024*1024);
	if (!text)
		return;

	root = JSON_Parse(text);
	if (!root)
	{
		if (qku_ftesurf) Sys_Error("Damaged update journal. Run the launcher to repair; no mixed install was started.");
		BZ_Free(text);
		FS_Remove(path, FS_SYSTEM);
		return;
	}

	attempts = (int)JSON_GetInteger(root, "attempts", 1) + 1;
	JSON_GetString(root, "version", jver, sizeof(jver), "");	//read before the tree is destroyed
	files = JSON_FindChild(root, "files");
	count = files ? JSON_GetCount(files) : 0;
	if (qku_ftesurf)
	{
		char sigpath[MAX_OSPATH], version[64], game[32];
		qku_state_t state;
		qboolean older;
		QKU_InstallPath(sigpath, sizeof(sigpath), ".ftesurf-updater/ingame/manifest.json");
		signedtext = QKU_ReadTextFile(sigpath, 2*1024*1024);
		signedroot = signedtext ? QKU_FSParseSigned(signedtext, &signedpayload) : NULL;
		if (!signedroot || !QKU_FSValidateManifest(signedroot) || !count || count > 8192) Sys_Error("Update recovery lacks a valid signed manifest. Run the launcher to repair.");
		JSON_GetString(signedroot, "version", version, sizeof(version), "");
		JSON_GetString(signedroot, "game", game, sizeof(game), "");
		if (strcmp(version, jver) || strcmp(game, "ftesurf")) Sys_Error("Update journal does not match its signed manifest. Run the launcher to repair.");
		if (!QKU_LoadState(&state))
		{ QKU_FreeState(&state); Sys_Error("Cannot read the update recovery ledger. Run the launcher to repair; nothing installed."); }
		older = *state.version && QKU_FSVersionOlder(version, state.version);
		if (older) Sys_Error("Older signed recovery release. Run the launcher to repair; nothing installed.");
		preserved = Z_Malloc(sizeof(*preserved) * count);
		// Validate and classify ALL entries against the pre-recovery ledger before replacements.
		for (i = 0; i < count; i++)
		{
			char rel[MAX_OSPATH], hash[QKU_HEXLEN+16];
			json_t *f = JSON_GetIndexed(files, i);
			quint64_t size = (quint64_t)JSON_GetFloat(f, "s", -1);
			enum qku_preserve_e protect;
			JSON_GetString(f, "p", rel, sizeof(rel), ""); JSON_GetString(f, "h", hash, sizeof(hash), "");
			if (!QKU_ValidHash(hash) || !QKU_FSJournalEntry(signedroot, rel, hash, size) || !QKU_FSNoLinks(rel) || !QKU_FSStageNoLinks("staged/%s", hash) || !QKU_FSStageNoLinks("staged/%s.install", hash)) Sys_Error("Unverified or linked recovery entry. Run the launcher to repair; nothing installed.");
			protect = QKU_FSPreserveFile(&state, rel, hash);
			if (protect == QKUP_UNREADABLE) Sys_Error("Cannot read protected update file %s. Restore access or repair with the launcher; nothing installed.", rel);
			preserved[i] = protect == QKUP_PRESERVE;
		}
		QKU_FreeState(&state);
	}

	if (attempts > QKU_MAXATTEMPTS)
	{	//something is permanently holding a file. give up loudly rather than boot-loop.
		if (qku_ftesurf) Sys_Error("Update cannot finish while files are held open. Close other copies and repair with the launcher; journal retained.");
		qku_bootverdict = QKUS_LAUNCHER;
		Q_snprintfz(qku_bootreason, sizeof(qku_bootreason),
					"an update could not be finished after %i attempts -- run the launcher", QKU_MAXATTEMPTS);
		Con_Printf(CON_ERROR "update: %s\n", qku_bootreason);
		JSON_Destroy(root);
		BZ_Free(text);
		FS_Remove(path, FS_SYSTEM);
		return;
	}

	Con_Printf("update: finishing an interrupted update (%u files)\n", (unsigned)count);

	//Entries are re-emitted verbatim if they are still outstanding, so keep the parsed values
	//rather than re-reading the document we are about to overwrite.
	{
		struct qku_replay_s
		{
			char		rel[MAX_OSPATH];
			char		hash[QKU_HEXLEN+1];
			quint64_t	size;
			qboolean	exec;
			qboolean	installed;
			qboolean	valid;
		} *e = Z_Malloc(sizeof(*e) * count);
		struct qku_record_s *recs;
		size_t n = 0;

		for (i = 0; i < count; i++)
		{
			char raw[QKU_HEXLEN+16], abs[MAX_OSPATH], have[QKU_HEXLEN+1];
			quint64_t sz = 0;

			ent = JSON_GetIndexed(files, i);
			if (!ent)
				continue;
			JSON_GetString(ent, "p", e[i].rel, sizeof(e[i].rel), "");
			JSON_GetString(ent, "h", raw, sizeof(raw), "");
			e[i].size = (quint64_t)JSON_GetFloat(ent, "s", 0);
			e[i].exec = JSON_GetInteger(ent, "x", 0) ? true : false;

			if (qku_ftesurf && (!QKU_ValidHash(raw) || !QKU_FSJournalEntry(signedroot, e[i].rel, raw, e[i].size)))
				Sys_Error("Update journal contains an unverified file. Run the launcher to repair; journal retained.");
			if (!QKU_ValidPath(e[i].rel) || !QKU_ValidHash(raw))
				continue;	//left !valid: dropped from the rewritten journal rather than retried forever
			memcpy(e[i].hash, raw, QKU_HEXLEN+1);
			e[i].valid = true;
			if (preserved && preserved[i])
			{
				Con_Printf("update: preserving local file %s during recovery\n", e[i].rel);
				continue; //resolved without claiming the signed hash in the ledger
			}

			//This skip is what terminates the loop: a class-B install (locked destination
			//renamed aside) leaves the correct file in place, so the next boot drops the entry.
			QKU_InstallPath(abs, sizeof(abs), e[i].rel);
			if (QKU_HashFile(abs, &sz, have) && sz == e[i].size && !strcmp(have, e[i].hash))
			{
				e[i].installed = true;
				done++;
				continue;
			}

			{	//verify the staged object before trusting it -- it may itself be a torn write
				char staged[MAX_OSPATH];
				Q_snprintfz(staged, sizeof(staged), "%s%s/staged/%s", com_gamepath, QKU_STAGEDIR, e[i].hash);
				if (!QKU_HashFile(staged, &sz, have) || sz != e[i].size || strcmp(have, e[i].hash))
				{
					pending++;
					continue;
				}
			}

			if (QKU_InstallOne(e[i].rel, e[i].hash, e[i].size, e[i].exec, &oldcounter))
			{
				e[i].installed = true;
				done++;
			}
			else
				pending++;
		}

		JSON_Destroy(root);
		BZ_Free(text);
		root = NULL;
		text = NULL;

		if (qku_ftesurf && !pending && !QKU_FSComplete(signedroot)) Sys_Error("Incomplete signed installation after recovery. Run the launcher to repair; journal retained.");
		if (done || (qku_ftesurf && !pending))
		{	//A protected-only completion still commits the verified release version,
			//but no replacement hashes. Keep its rollback floor and cache consistent.
			recs = Z_Malloc(sizeof(*recs) * (done?done:1));
			for (i = 0; i < count; i++)
				if (e[i].installed && n < done)
				{
					recs[n].path = e[i].rel;
					recs[n].hash = e[i].hash;
					n++;
				}
			//only claim the new version once nothing is outstanding
			QKU_WriteState(recs, n, pending ? NULL : jver);
			Z_Free(recs);
		}

		if (pending)
		{
			//Rewrite the journal with ONLY what is still outstanding and a bumped attempt count.
			//Getting this wrong is how you build a boot loop: leaving the original in place means
			//`attempts` never rises and a permanently locked file is retried forever.
			char tmp[MAX_OSPATH];
			vfsfile_t *f;
			qboolean first = true, rewritten = false;
			Q_snprintfz(tmp, sizeof(tmp), "%s%s/journal.tmp", com_gamepath, QKU_STAGEDIR);
			FS_CreatePath(tmp, FS_SYSTEM);
			f = FS_OpenVFS(tmp, "wb", FS_SYSTEM);
			if (f)
			{
				if (qku_ftesurf) VFS_PRINTF(f, "{\n\"attempts\":%i,\n\"version\":\"%s\",\n\"files\":[\n", attempts, jver);
				else VFS_PRINTF(f, "{\n\"attempts\":%i,\n\"files\":[\n", attempts);
				for (i = 0; i < count; i++)
				{
					if (!e[i].valid || e[i].installed || (preserved && preserved[i]))
						continue;
					VFS_PRINTF(f, "%s{\"p\":\"%s\",\"h\":\"%s\",\"s\":%.0f,\"x\":%i}",
								first?"":",\n", e[i].rel, e[i].hash, (double)e[i].size, e[i].exec?1:0);
					first = false;
				}
				VFS_PRINTF(f, "\n]\n}\n");
				if (VFS_CLOSE(f))
				{
					FS_Remove(path, FS_SYSTEM);		//Sys_Rename will not overwrite
					rewritten = FS_Rename(tmp, path, FS_SYSTEM);
				}
			}
			if (qku_ftesurf)
			{
				if (!rewritten) Sys_Error("Cannot rewrite the update recovery journal. Run the launcher to repair; staged objects retained.");
				for (i = 0; i < count; i++)
					if (e[i].valid && (e[i].installed || preserved[i]))
					{
						size_t j;
						// A resolved row may share its object with an outstanding destination.
						for (j = 0; j < count; j++)
							if (e[j].valid && !e[j].installed && !preserved[j] && !strcmp(e[j].hash, e[i].hash)) break;
						if (j == count) QKU_FSPurgeObject(e[i].hash, e[i].size);
					}
			}
			Con_Printf(CON_WARNING "update: %u file(s) still pending, will retry next launch (attempt %i of %i)\n",
						(unsigned)pending, attempts, QKU_MAXATTEMPTS);
		}
		else
		{
			if (qku_ftesurf && !QKU_FSSaveEnvelope(signedtext, ".ftesurf-updater/installed.json")) Sys_Error("Cannot save the recovered update's offline manifest; journal retained.");
			FS_Remove(path, FS_SYSTEM);
			if (qku_ftesurf)
				for (i = 0; i < count; i++) if (e[i].valid && (e[i].installed || preserved[i])) QKU_FSPurgeObject(e[i].hash, e[i].size);
			Con_Printf("update: interrupted update completed (%u files)\n", (unsigned)done);
		}

		Z_Free(e);
	}
	Z_Free(preserved);
	if (signedroot) JSON_Destroy(signedroot);
	if (signedpayload) Z_Free(signedpayload);
	if (signedtext) BZ_Free(signedtext);
	if (qku_ftesurf && pending) Sys_Error("Unfinished content update. Close other copies and restart to recover; no mixed install was started.");
}

// ---------------------------------------------------------------------------------------
// commands
// ---------------------------------------------------------------------------------------

//Progress is tracked at FILE granularity, not byte-by-byte: the in-flight byte counters live in
//dl_download structs owned by the download threads, and reaching into those to make a number
//tick more smoothly is not worth a data race. A delta is small files, so this is fine -- and it
//is why the engine publishes update_rate rather than letting the menu derive one.
static void QKU_Main_Tick(int iarg, void *data)
{
	double now = Sys_DoubleTime();
	int dummy = 0;
	(void)iarg; (void)data;

	if (qku.state != QKUS_DOWNLOADING)
	{
		qku.ticking = false;
		Cvar_ForceSet(&update_rate, "0");
		return;
	}

	if (qku.ratetime)
	{
		double dt = now - qku.ratetime;
		if (dt > 0)
		{	//smoothed, because file-granular progress arrives in bursts
			double inst = (double)(qku.donebytes - qku.ratebytes) / dt;
			qku.rate = (float)(qku.rate*0.7 + inst*0.3);
			Cvar_ForceSet(&update_rate, va("%.0f", qku.rate));
		}
	}
	qku.ratetime = now;
	qku.ratebytes = qku.donebytes;

	Cmd_AddTimer(0.5, QKU_Main_Tick, 0, &dummy, sizeof(dummy));
}

static void QKU_ArmTick(void)
{
	int dummy = 0;
	if (qku.ticking)
		return;
	qku.ticking = true;
	qku.ratetime = 0;
	qku.ratebytes = qku.donebytes;
	qku.rate = 0;
	Cmd_AddTimer(0.5, QKU_Main_Tick, 0, &dummy, sizeof(dummy));
}

static void QKU_Check_f(void)
{
	const char *mode = Cmd_Argv(1);
	if (qku_ftesurf && Cmd_IsInsecure() && !qku_menuaction) return;
	if (!Q_strcasecmp(mode, "auto"))
	{	//what the main menu calls. Fires at most once per run, and only if the player wants it.
		if (!update_autocheck.ival || qku.autochecked)
			return;
		qku.autochecked = true;
		QKU_BeginCheck(false, false);
		return;
	}
	QKU_BeginCheck(!Q_strcasecmp(mode, "force"), false);
}

static void QKU_Verify_f(void)
{
	if (qku_ftesurf && Cmd_IsInsecure() && !qku_menuaction) return;
	Con_Printf("update: hashing the install against the manifest, this reads every file...\n");
	QKU_BeginCheck(true, true);
}

static void QKU_Apply_f(void)
{
	if (qku_ftesurf && Cmd_IsInsecure() && !qku_menuaction) return;
	if (qku_ftesurf && update_consent.ival != 1) return;
#ifdef HAVE_CLIENT
	if (qku_ftesurf && cls.state != ca_disconnected)
	{ Con_Printf("update: disconnect before installing an update\n"); return; }
#endif
	if (qku.state != QKUS_AVAILABLE || !qku.numfiles)
	{
		Con_Printf("update: nothing to apply (state %s)\n", qku_statename[qku.state]);
		return;
	}
	if (qku_ftesurf && qku.numdone == qku.numfiles)
	{ QKU_Apply(); return; }
	qku.numdone = 0;
	qku.donebytes = 0;
	QKU_SetState(QKUS_DOWNLOADING);
	QKU_PublishCounts();
	QKU_ArmTick();
	QKU_StartDownloads();
}

static void QKU_Cancel_f(void)
{
	if (qku_ftesurf && Cmd_IsInsecure() && !qku_menuaction) return;
	if (qku.state != QKUS_DOWNLOADING && qku.state != QKUS_CHECKING && qku.state != QKUS_AVAILABLE)
	{
		Con_Printf("update: nothing in progress\n");
		return;
	}
	//Deliberately NOT DL_Close: that does Sys_WaitOnThread and re-enters notifycomplete. Bump
	//the sequence instead -- in-flight downloads drain harmlessly and their results are dropped
	//as stale on arrival.
	qku.seq++;
	qku.active = 0;
	QKU_ClearPlan();
	QKU_SetState(QKUS_IDLE);
	Con_Printf("update: cancelled\n");
}

static void QKU_Info_f(void)
{
	size_t i, show;
	Con_Printf("update state : %s\n", qku_statename[qku.state]);
	Con_Printf("manifest     : %s\n", update_manifest_url.string);
	Con_Printf("platform     : %s\n", *QKU_PLATFORM?QKU_PLATFORM:"(unsupported)");
	if (*update_version.string)
		Con_Printf("version      : %s\n", update_version.string);
	if (*qku.error)
		Con_Printf("last error   : %s\n", qku.error);
	if (qku.numfiles)
	{
		Con_Printf("files        : %u (%u done), %.1f MB\n", (unsigned)qku.numfiles,
					(unsigned)qku.numdone, (double)qku.totalbytes/(1024.0*1024.0));
		show = qku.numfiles < 40 ? qku.numfiles : 40;
		for (i = 0; i < show; i++)
			Con_Printf("  %s (%.0f bytes)\n", qku.files[i].path, (double)qku.files[i].size);
		if (show < qku.numfiles)
			Con_Printf("  ...and %u more\n", (unsigned)(qku.numfiles-show));
	}
}

static void QKU_Permission(const char *v)
{
	char path[MAX_OSPATH], tmp[MAX_OSPATH];
	vfsfile_t *f;
	if ((Cmd_IsInsecure() && !qku_menuaction) || !qku_ftesurf || (strcmp(v, "0") && strcmp(v, "1"))) return;
	if (!QKU_FSNoLinks(".ftesurf-updater/consent") || !QKU_FSNoLinks(".ftesurf-updater/consent.tmp"))
	{ Con_Printf("update: linked permission path; permission was not changed\n"); return; }
	QKU_InstallPath(path, sizeof(path), ".ftesurf-updater/consent");
	QKU_InstallPath(tmp, sizeof(tmp), ".ftesurf-updater/consent.tmp");
	f = FS_OpenVFS(tmp, "wb", FS_SYSTEM);
	if (!f) { Con_Printf("update: could not save update permission\n"); return; }
	VFS_PRINTF(f, "%s\n", v);
	if (!VFS_CLOSE(f)) { Con_Printf("update: could not save update permission\n"); return; }
	FS_Remove(path, FS_SYSTEM);
	if (!FS_Rename(tmp, path, FS_SYSTEM)) { Con_Printf("update: could not save update permission\n"); return; }
	Cvar_ForceSet(&update_consent, v);
	if (!strcmp(v, "0")) { QKU_Cancel_f(); QKU_SetState(QKUS_DISABLED); }
	else { qku.autochecked = false; QKU_SetState(QKUS_IDLE); }
}

static void QKU_Permission_f(void) { QKU_Permission(Cmd_Argv(1)); }

// Fixed, local handoff: no URL/shell/arbitrary executable from the manifest or server.
static void QKU_Restart_f(void)
{
	char exe[MAX_OSPATH];
	const char *launcher;
	if ((Cmd_IsInsecure() && !qku_menuaction) || !qku_ftesurf || update_consent.ival != 1 || (qku.state != QKUS_RESTART && qku.state != QKUS_LAUNCHER)) return;
#ifdef HAVE_CLIENT
	if (cls.state != ca_disconnected) { QKU_Fail(qku.state, "Restart was not started: disconnect before restarting."); return; }
#endif
#ifdef _WIN32
	launcher = "ftesurf-launcher.exe";
#else
	launcher = "ftesurf-launcher";
#endif
	if (!QKU_FSNoLinks(launcher) || !QKU_FSNoLinks(".ftesurf-updater/install.lock"))
	{ QKU_Fail(qku.state, "linked launcher path; restart was not started. Use a regular local launcher beside FTESurf, then retry."); return; }
	QKU_InstallPath(exe, sizeof(exe), launcher);
#ifdef _WIN32
	{
		char cmd[MAX_OSPATH*3];
		wchar_t wide[MAX_OSPATH*3];
		STARTUPINFOW si = {0};
		PROCESS_INFORMATION pi = {0};
		Q_snprintfz(cmd, sizeof(cmd), "\"%s\" --dir \"%s.\" --wait-for-game --yes", exe, com_gamepath);
		if (!MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wide, sizeof(wide)/sizeof(wide[0])))
		{ QKU_Fail(qku.state, "Restart was not started: cannot encode the launcher path. Start the launcher manually."); return; }
		si.cb = sizeof(si);
		if (!CreateProcessW(NULL, wide, NULL, NULL, false, CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi))
		{ QKU_Fail(qku.state, "Restart was not started: launcher is missing or could not start. Get it from https://proto.bar/ftesurf/, put it beside FTESurf and retry."); return; }
		CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
	}
#else
	{
		int fds[2], err = 0;
		pid_t pid;
		ssize_t result;
		if (access(exe, X_OK))
		{ QKU_Fail(qku.state, "Restart was not started: launcher is missing or not executable. Get it from https://proto.bar/ftesurf/, put it beside FTESurf, enable execute permission and retry."); return; }
		if (pipe(fds)) { QKU_Fail(qku.state, "Restart was not started: cannot create the launcher pipe. Start the launcher manually."); return; }
		if (fcntl(fds[1], F_SETFD, FD_CLOEXEC) < 0)
		{ close(fds[0]); close(fds[1]); QKU_Fail(qku.state, "Restart was not started: cannot prepare the launcher pipe. Start the launcher manually."); return; }
		pid = fork();
		if (pid == 0)
		{
			close(fds[0]);
			if (qku_fslock >= 0) close(qku_fslock);
			execl(exe, exe, "--dir", com_gamepath, "--wait-for-game", "--yes", (char *)NULL);
			err = 1;
			do { result = write(fds[1], &err, sizeof(err)); } while (result < 0 && errno == EINTR);
			_exit(result == sizeof(err) ? 127 : 126);
		}
		close(fds[1]);
		result = -1;
		if (pid >= 0)
			do { result = read(fds[0], &err, sizeof(err)); } while (result < 0 && errno == EINTR);
		close(fds[0]);
		// Only close-on-exec EOF confirms success; a read error is not a handoff.
		if (result != 0) { QKU_Fail(qku.state, "Restart was not started: could not start the launcher. Start it manually or replace it from https://proto.bar/ftesurf/ and retry."); return; }
	}
#endif
	qku.error[0] = 0;
	Cvar_ForceSet(&update_error, "");
	Cbuf_AddText("quit\n", RESTRICT_LOCAL);
}

// Invoked ONLY by the menu VM's native builtin, never the command dispatcher or CSQC.
int QKU_MenuAction(int action)
{
	if (!qku_ftesurf || action < 0 || action > 6) return false;
	qku_menuaction = true;
	switch (action)
	{
	case 0: if (!qku.autochecked && update_autocheck.ival) { qku.autochecked = true; QKU_BeginCheck(false, false); } break;
	case 1: QKU_BeginCheck(true, false); break;
	case 2: QKU_Apply_f(); break;
	case 3: QKU_Cancel_f(); break;
	case 4: QKU_Permission("1"); break;
	case 5: QKU_Permission("0"); break;
	case 6: QKU_Restart_f(); break;
	}
	qku_menuaction = false;
	return true;
}

// ---------------------------------------------------------------------------------------
// init / shutdown
// ---------------------------------------------------------------------------------------

static void QKU_ConfigureProfile(void)
{
	if (qku_ftesurf)
	{
		cvar_t *vars[] = {&update_manifest_url, &update_max_files, &update_max_mb};
		char *values[] = {"https://proto.bar/ftesurf/update.json", "512", "16"};
		char path[MAX_OSPATH], *consent;
		size_t i;
		for (i = 0; i < countof(vars); i++)
		{
			qboolean wasdefault = !strcmp(vars[i]->string, vars[i]->enginevalue);
			// CVARFD uses static literals; Cvar_SetEngineDefault would free them.
			if (vars[i]->defaultstr == vars[i]->enginevalue) vars[i]->defaultstr = values[i];
			vars[i]->enginevalue = values[i];
			if (wasdefault) Cvar_ForceSet(vars[i], values[i]);
		}
		if (!QKU_FSNoLinks(".ftesurf-updater/consent")) Sys_Error("Linked update permission file: refusing to start");
		QKU_InstallPath(path, sizeof(path), ".ftesurf-updater/consent");
		consent = QKU_ReadTextFile(path, 8);
		Cvar_ForceSet(&update_consent, "-1");
		if (consent)
		{
			if (!strcmp(consent, "1") || !strcmp(consent, "1\n") || !strcmp(consent, "1\r\n")) Cvar_ForceSet(&update_consent, "1");
			else Cvar_ForceSet(&update_consent, "0");
			BZ_Free(consent);
		}
	}
	else Cvar_ForceSet(&update_consent, "1");
	QKU_SetState(update_enabled.ival && (!qku_ftesurf || update_consent.ival != 0) ? QKUS_IDLE : QKUS_DISABLED);
}

void QKU_Init(void)
{
	Cvar_Register(&update_enabled,			"Updates");
	Cvar_Register(&update_autocheck,		"Updates");
	Cvar_Register(&update_manifest_url,		"Updates");
	Cvar_Register(&update_concurrency,		"Updates");
	Cvar_Register(&update_max_files,		"Updates");
	Cvar_Register(&update_max_mb,			"Updates");
	Cvar_Register(&update_engine_policy,	"Updates");
	Cvar_Register(&update_allow_insecure,	"Updates");
	Cvar_Register(&update_stage_only,		"Updates");

	Cvar_Register(&update_consent, "Updates");
	Cvar_Register(&update_state,			"Updates");
	Cvar_Register(&update_files,			"Updates");
	Cvar_Register(&update_files_done,		"Updates");
	Cvar_Register(&update_bytes,			"Updates");
	Cvar_Register(&update_done_bytes,		"Updates");
	Cvar_Register(&update_rate,				"Updates");
	Cvar_Register(&update_current,			"Updates");
	Cvar_Register(&update_version,			"Updates");
	Cvar_Register(&update_error,			"Updates");

	//"update_info", not "update_status": a command and a cvar of the same name collide in
	//Cmd_ExecuteString, and the cvar would win.
	Cmd_AddCommandD("update_check",		QKU_Check_f,	"Check for a content update. \"force\" skips the version fast-path; \"auto\" is the once-per-run menu check and obeys update_autocheck.");
	Cmd_AddCommandD("update_apply",		QKU_Apply_f,	"Download and install the pending update.");
	Cmd_AddCommandD("update_restart", QKU_Restart_f, "Restart FTESurf through its local launcher to finish or install a complete update.");
	Cmd_AddCommandD("update_permission", QKU_Permission_f, "0 disables, 1 enables automatic updates in the game and launcher. No request occurs before consent.");
	Cmd_AddCommandD("update_cancel",	QKU_Cancel_f,	"Abandon the pending check or download.");
	Cmd_AddCommandD("update_info",		QKU_Info_f,		"Print what the updater currently knows.");
	Cmd_AddCommandD("update_verify",	QKU_Verify_f,	"Hash every manifest file on disk and report differences. Reports only, never downloads.");

	qku_initialised = true;
	if (qku_profileknown) QKU_ConfigureProfile();
	if (qku_bootverdict >= 0)
	{	//journal replay ran before any of these cvars existed
		Q_strncpyz(qku.error, qku_bootreason, sizeof(qku.error));
		Cvar_ForceSet(&update_error, qku.error);
		QKU_SetState(qku_bootverdict);
	}
	else
		QKU_SetState(update_enabled.ival && (!qku_ftesurf || update_consent.ival != 0) ? QKUS_IDLE : QKUS_DISABLED);
}

void QKU_Shutdown(void)
{
#ifdef _WIN32
	if (qku_fslock != INVALID_HANDLE_VALUE) { CloseHandle(qku_fslock); qku_fslock = INVALID_HANDLE_VALUE; }
#else
	if (qku_fslock >= 0) { close(qku_fslock); qku_fslock = -1; }
#endif
	//Stop caring about anything still in flight. HTTP_CL_Terminate (called immediately after
	//this) does the actual teardown; we just make sure no late callback finds live state.
	qku.seq++;
	qku.active = 0;
	if (qku.envelope) { Z_Free(qku.envelope); qku.envelope = NULL; }
	QKU_FreePlanFiles(qku.files, qku.numfiles);
	QKU_FreeStrings(qku.removed, qku.numremoved);
	qku.files = NULL;
	qku.numfiles = 0;
	qku.removed = NULL;
	qku.numremoved = 0;
}

#else	//!WEBCLIENT || !MULTITHREAD

void QKU_Init(void) {}
void QKU_Shutdown(void) {}
void QKU_ReplayJournal(void) {}
int QKU_MenuAction(int action) { return false; }

#endif
