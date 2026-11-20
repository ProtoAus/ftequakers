/*
FTESurf Patch 1691: Discord Rich Presence over the Discord client's local IPC (no SDK).

Frames are a little-endian u32 opcode, a u32 length and JSON: 0 handshake {"v":1,"client_id":..}, 1 a command or its
reply, 2 close, 3 ping (answer 4 with the same body). Linux/macOS: a unix socket discord-ipc-0..9 under
$XDG_RUNTIME_DIR, $TMPDIR, /tmp (and the Flatpak/Snap subfolders); Windows: \\.\pipe\discord-ipc-0..9.

Everything shown comes from the engine: the map from the world model, its picture from the map table the menu keeps
(data/mapmeta*.txt, a thumbnail id), and the run from the timer stats (STAT_FS_TIMER*, sh_defs.qc). A server picks only
the map and those numbers: a map is named only when the local map table or the GoldSrc catalogue knows it (otherwise
"a custom map"), and the numbers are clamped. Discord takes about one activity per 4 s; ours changes
only on a state edge.
*/
#include "quakedef.h"
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#endif

static cvar_t cl_discord = CVARFD("cl_discord", "1", CVAR_ARCHIVE|CVAR_NOTFROMSERVER,
	"FTESurf Patch 1691. Show what you are playing on your Discord profile (map, stage, run clock, the map's picture). 0 hides it.");
static cvar_t cl_discord_appid = CVARFD("cl_discord_appid", "1573424786774696097", CVAR_NOTFROMSERVER,
	"FTESurf Patch 1691. The Discord application the presence belongs to (World of Surf's; its name is the game Discord shows). Empty: off.");
static cvar_t cl_discord_ipcdir = CVARFD("cl_discord_ipcdir", "", CVAR_NOTFROMSERVER,
	"FTESurf Patch 1691, tests: the folder holding discord-ipc-N instead of the usual ones (not on Windows).");

#define DC_STAT_STATE			52	// STAT_FS_TIMERSTATE: TS_IDLE 0, TS_ARMED 1, TS_RUNNING 2, TS_FINISHED 3
#define DC_STAT_TICKS			53
#define DC_STAT_SEG				54	// 0-based segment: stage seg+1
#define DC_STAT_SEGS			55	// 1 = linear
#define DC_STAT_TRACK			56	// 0 main, N bonus N
#define DC_STAT_OWNER			102	// STAT_FS_STATOWNER: whose stats these are (LT_StatsAreMine); 0 = an older server
#define DC_THUMBS "https://play.proto.bar/ftesurf/media/thumbs/"
#define DC_LOGO "https://proto.bar/ftesurf/worldofsurf-512.png"	// the brand kit's square export (src/brand), by URL: no art asset to upload

#ifdef _WIN32
static HANDLE dc_pipe = INVALID_HANDLE_VALUE;
#define DC_OPEN (dc_pipe != INVALID_HANDLE_VALUE)
#else
static int dc_fd = -1;
#define DC_OPEN (dc_fd >= 0)
#endif
static int dc_state;			// 0 closed, 1 handshake sent, 2 ready
static double dc_nexttry, dc_nextsend, dc_nextbuild;
static char dc_sent[2048], dc_want[2048];
static char dc_inbuf[16384];
static size_t dc_inlen;
static unsigned int dc_nonce;
static qboolean dc_said;
static char dc_map[MAX_QPATH], dc_thumb[128];
static time_t dc_start, dc_runstart;
static float dc_lastticks;
static double dc_tickmoved;
static qboolean dc_known;		// the map is in the local map table or the GoldSrc catalogue: its name may show
static char dc_appid[32];		// the id the open connection was made with
static int dc_closes;

static void DC_Close(double retry)
{
#ifdef _WIN32
	if (dc_pipe != INVALID_HANDLE_VALUE)
		CloseHandle(dc_pipe);
	dc_pipe = INVALID_HANDLE_VALUE;
#else
	if (dc_fd >= 0)
		close(dc_fd);
	dc_fd = -1;
#endif
	dc_state = 0;
	dc_inlen = 0;
	*dc_sent = 0;
	dc_nexttry = realtime + retry;
}

static qboolean DC_Connect(void)
{
	int i;
#ifdef _WIN32
	for (i = 0; i < 10; i++)
	{
		char name[64];
		Q_snprintfz(name, sizeof(name), "\\\\.\\pipe\\discord-ipc-%i", i);
		dc_pipe = CreateFileA(name, GENERIC_READ|GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
		if (dc_pipe != INVALID_HANDLE_VALUE)
		{
			DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;	// a full pipe fails the write instead of freezing the frame
			SetNamedPipeHandleState(dc_pipe, &mode, NULL, NULL);
			return true;
		}
	}
	return false;
#else
	const char *base[4], *sub[] = {"", "app/com.discordapp.Discord/", "snap.discord/"};
	int nb = 0, b, s;
	if (*cl_discord_ipcdir.string)
		base[nb++] = cl_discord_ipcdir.string;
	else
	{
		const char *e;
		if ((e = getenv("XDG_RUNTIME_DIR")) && *e) base[nb++] = e;
		if ((e = getenv("TMPDIR")) && *e) base[nb++] = e;
		base[nb++] = "/tmp";
	}
	for (b = 0; b < nb; b++)
		for (s = 0; s < (*cl_discord_ipcdir.string ? 1 : 3); s++)
			for (i = 0; i < 10; i++)
			{
				struct sockaddr_un a;
				int fd;
				memset(&a, 0, sizeof(a));
				a.sun_family = AF_UNIX;
				if (Q_snprintfz(a.sun_path, sizeof(a.sun_path), "%s/%sdiscord-ipc-%i", base[b], sub[s], i))
					continue;	// truncated
				fd = socket(AF_UNIX, SOCK_STREAM, 0);
				if (fd < 0)
					return false;
				if (connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0)
				{
					fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
					{ int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)); }
#endif
					dc_fd = fd;
					return true;
				}
				close(fd);
			}
	return false;
#endif
}

// One frame; false (and closed) when the pipe will not take it whole.
static qboolean DC_Send(unsigned int op, const char *json)
{
	unsigned char buf[sizeof(dc_sent) + 128];
	size_t len = strlen(json), done = 0;
	if (len + 8 > sizeof(buf))
		return false;
	buf[0] = op; buf[1] = op>>8; buf[2] = op>>16; buf[3] = op>>24;
	buf[4] = len; buf[5] = len>>8; buf[6] = len>>16; buf[7] = len>>24;
	memcpy(buf + 8, json, len);
	len += 8;
	while (done < len)
	{
#ifdef _WIN32
		DWORD w = 0;
		if (!WriteFile(dc_pipe, buf + done, (DWORD)(len - done), &w, NULL) || !w)
			break;
		done += w;
#else
#ifdef MSG_NOSIGNAL
		ssize_t w = send(dc_fd, buf + done, len - done, MSG_NOSIGNAL);
#else
		ssize_t w = send(dc_fd, buf + done, len - done, 0);
#endif
		if (w <= 0)
			break;		// a full buffer is a stalled reader: start again rather than block the frame
		done += w;
#endif
	}
	if (done == len)
		return true;
	Con_DPrintf("discord: write failed, closing\n");
	DC_Close(10);
	return false;
}

// Is the frame's "evt" this event? Tolerates JSON whitespace around the colon.
static qboolean DC_Event(const char *body, const char *evt)
{
	const char *p = strstr(body, "\"evt\"");
	size_t n = strlen(evt);
	if (!p)
		return false;
	for (p += 5; *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':'; p++)
		;
	return *p == '"' && !strncmp(p + 1, evt, n) && p[1 + n] == '"';
}

static void DC_Read(void)
{
	for (;;)
	{
		size_t room = sizeof(dc_inbuf) - 1 - dc_inlen;
#ifdef _WIN32
		DWORD avail = 0, got = 0;
		if (!PeekNamedPipe(dc_pipe, NULL, 0, NULL, &avail, NULL))
		{
			DC_Close(10);
			return;
		}
		if (!avail || !room)
			break;
		if (!ReadFile(dc_pipe, dc_inbuf + dc_inlen, (DWORD)min(avail, room), &got, NULL) || !got)
		{
			DC_Close(10);
			return;
		}
#else
		ssize_t got;
		if (!room)
			break;
		got = recv(dc_fd, dc_inbuf + dc_inlen, room, 0);
		if (got == 0 || (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
		{
			Con_DPrintf("discord: connection closed\n");
			DC_Close(10);
			return;
		}
		if (got < 0)
			break;
#endif
		dc_inlen += got;
	}
	while (dc_inlen >= 8)
	{
		unsigned char *h = (unsigned char *)dc_inbuf;
		unsigned int op = h[0] | (h[1]<<8) | (h[2]<<16) | ((unsigned)h[3]<<24);
		unsigned int len = h[4] | (h[5]<<8) | (h[6]<<16) | ((unsigned)h[7]<<24);
		char body[sizeof(dc_inbuf)];
		if (len > sizeof(dc_inbuf) - 9)
		{
			DC_Close(30);	// not Discord, or a frame we will never hold
			return;
		}
		if (dc_inlen < 8 + len)
			break;
		memcpy(body, dc_inbuf + 8, len);
		body[len] = 0;
		dc_inlen -= 8 + len;
		memmove(dc_inbuf, dc_inbuf + 8 + len, dc_inlen);
		if (op == 3)
			DC_Send(4, body);
		else if (op == 2)
		{
			if (!dc_closes++)
				Con_Printf("discord: closed by Discord: %.200s\n", body);	// a wrong cl_discord_appid, say
			else
				Con_DPrintf("discord: closed by Discord: %.200s\n", body);
			DC_Close(dc_closes > 2 ? 600 : 60);
			return;
		}
		else if (op == 1 && DC_Event(body, "READY"))
		{
			dc_state = 2;
			dc_closes = 0;
			Con_DPrintf("discord: ready\n");
		}
		else if (op == 1 && DC_Event(body, "ERROR"))
			Con_DPrintf("discord: %.300s\n", body);
		else if (op == 1)
			Con_DPrintf("discord: reply %.400s\n", body);	// what Discord kept of an activity (developer 1)
		if (!DC_OPEN)
			return;
	}
}

// Appends s to out as JSON string content.
static void DC_Esc(char *out, size_t outsize, const char *s)
{
	size_t n = strlen(out);
	for (; *s && n + 7 < outsize; s++)
	{
		unsigned char c = *s;
		if (c == '"' || c == '\\')
		{
			out[n++] = '\\';
			out[n++] = c;
		}
		else if (c < 32 || c >= 127)
			continue;	// map names are ASCII; drop the rest rather than encode it
		else
			out[n++] = c;
	}
	out[n] = 0;
}

// The map's thumbnail id from the menu's map table (P1099): "meta <map> n n n n n n <uuid> ...", the newer stamp's file.
static void DC_FindThumb(const char *map)
{
	const char *files[] = {"data/mapmeta.net.txt", "data/mapmeta.txt"};
	char best[37] = "", beststamp[32] = "", found[37], stamp[32], line[1024];
	size_t ml = strlen(map);
	int f, field;
	*dc_thumb = 0;
	for (f = 0; f < 2; f++)
	{
		vfsfile_t *vf = FS_OpenVFS(files[f], "rb", FS_GAME);
		if (!vf)
			continue;
		*found = *stamp = 0;
		while (VFS_GETS(vf, line, sizeof(line)))
		{
			if (!strncmp(line, "# stamp ", 8))
				Q_strncpyz(stamp, line + 8, sizeof(stamp));
			else if (!strncmp(line, "meta ", 5) && !strncmp(line + 5, map, ml) && line[5 + ml] == ' ')
			{
				const char *p = line + 5;
				for (field = 0; field < 7 && p; field++)	// map, six numbers, then the id
					if ((p = strchr(p, ' ')))
						p++;
				if (p && strspn(p, "0123456789abcdef-") == 36 && (unsigned char)p[36] <= ' ')
					Q_strncpyz(found, p, sizeof(found));
			}
		}
		VFS_CLOSE(vf);
		if (*found && (!*best || strcmp(stamp, beststamp) > 0))
		{
			Q_strncpyz(best, found, sizeof(best));
			Q_strncpyz(beststamp, stamp, sizeof(beststamp));
		}
	}
	if (*best)
		Q_snprintfz(dc_thumb, sizeof(dc_thumb), DC_THUMBS"%s.jpg", best);
	{
		goldsrcrow_t gr;	//filled in place: never NULL
		dc_known = *best || FS_GoldSrc_Row(map, &gr);
	}
}

static float DC_Stat(const playerview_t *pv, int i, float lo, float hi)
{
	float v = pv->statsf[i];
	return isfinite(v) ? bound(lo, v, hi) : lo;
}

static void DC_Build(char *out, size_t outsize)
{
	char details[160], state[96], start[48] = "";
	qboolean local = false, mine;
	playerview_t *pv = &cl.playerview[0];
	const char *image;
	int owner;

	if (cls.state != ca_active || !cl.worldmodel)
	{
		*dc_map = 0;
		Q_snprintfz(out, outsize, "{\"details\":\"In the menu\",\"assets\":{\"large_image\":\"" DC_LOGO "\",\"large_text\":\"World of Surf\"}}");
		return;
	}
	{
		char map[MAX_QPATH];
		COM_FileBase(cl.worldmodel->name, map, sizeof(map));
		if (strcmp(map, dc_map))
		{
			Q_strncpyz(dc_map, map, sizeof(dc_map));
			DC_FindThumb(dc_map);
			dc_start = time(NULL);
			dc_runstart = 0;
		}
	}
#ifdef HAVE_SERVER
	local = sv.state != ss_dead;
#endif
	*details = 0;
	if (dc_known)
		DC_Esc(details, sizeof(details), dc_map);
	else
		Q_strncpyz(details, "a custom map", sizeof(details));
	owner = DC_Stat(pv, DC_STAT_OWNER, 0, MAX_CLIENTS);
	mine = owner > 0 ? owner == pv->playernum + 1 : pv->viewentity == pv->playernum + 1;
	if (cls.demoplayback || !mine)
	{
		Q_strncpyz(state, cls.demoplayback ? "Watching a replay" : "Spectating", sizeof(state));
		dc_runstart = 0;
	}
	else
	{
		int track = DC_Stat(pv, DC_STAT_TRACK, 0, 99);
		if (track > 0)
			Q_strncatz(details, va(" (bonus %i)", track), sizeof(details));
		switch ((int)DC_Stat(pv, DC_STAT_STATE, 0, 3))
		{
		case 2:
			{
				int segs = DC_Stat(pv, DC_STAT_SEGS, 1, 999), seg = DC_Stat(pv, DC_STAT_SEG, 0, 998);
				float ticks = DC_Stat(pv, DC_STAT_TICKS, 0, 1e8);
				float tick = movevars.ticrate > 0 ? movevars.ticrate : 0.015;
				time_t at = time(NULL) - (time_t)(ticks * tick);
				if (segs > 1)
					Q_snprintfz(state, sizeof(state), "Stage %i/%i", bound(1, seg + 1, segs), segs);
				else
					Q_strncpyz(state, "Linear", sizeof(state));
				if (!dc_runstart || ticks != dc_lastticks)
				{	// a new run starts unpaused (review 2)
					dc_lastticks = ticks;
					dc_tickmoved = realtime;
				}
				// one start per run: a second's rounding must not resend, a restart or rewind (> 2 s off) must
				if (!dc_runstart || at > dc_runstart + 2 || at < dc_runstart - 2)
					dc_runstart = at;
				if (realtime - dc_tickmoved > 2)
					Q_strncatz(state, " (paused)", sizeof(state));	// the clock stopped (a local game's menu)
				else
					Q_snprintfz(start, sizeof(start), ",\"timestamps\":{\"start\":%lld}", (long long)dc_runstart);
			}
			break;
		case 3:
			Q_strncpyz(state, "Finished a run", sizeof(state));
			break;
		case 1:
			Q_strncpyz(state, "In the start zone", sizeof(state));
			break;
		default:
			Q_strncpyz(state, local ? "Local game" : "Online", sizeof(state));
			break;
		}
		if (DC_Stat(pv, DC_STAT_STATE, 0, 3) != 2)
			dc_runstart = 0;
	}
	if (!*start && !strstr(state, "(paused)"))
		Q_snprintfz(start, sizeof(start), ",\"timestamps\":{\"start\":%lld}", (long long)dc_start);
	image = *dc_thumb ? dc_thumb : DC_LOGO;
	Q_snprintfz(out, outsize, "{\"details\":\"%s\",\"state\":\"%s\"%s,\"assets\":{\"large_image\":\"%s\",\"large_text\":\"%s\","
		"\"small_image\":\"" DC_LOGO "\",\"small_text\":\"%s\"}}", details, state, start, image, details, local ? "Local game" : "Online");
}

void CL_Discord_Frame(void)
{
	size_t idlen = strlen(cl_discord_appid.string);
	qboolean want = cl_discord.ival && idlen && idlen < 21 && strspn(cl_discord_appid.string, "0123456789") == idlen;
	char frame[sizeof(dc_sent) + 160];

	if (!want || (DC_OPEN && strcmp(dc_appid, cl_discord_appid.string)))
	{
		if (DC_OPEN)
			DC_Close(1);	// closing the pipe is what clears the presence (and a new id needs a new handshake)
		return;
	}
	if (!DC_OPEN)
	{
		if (strcmp(dc_appid, cl_discord_appid.string))
		{	// a corrected id tries at once, not after a wrong one's backoff (review 2)
			Q_strncpyz(dc_appid, cl_discord_appid.string, sizeof(dc_appid));
			dc_nexttry = 0;
			dc_closes = 0;
		}
		if (realtime < dc_nexttry)
			return;
		if (!DC_Connect())
		{
			dc_nexttry = realtime + 15;	// Discord not running
			return;
		}
		Q_strncpyz(dc_appid, cl_discord_appid.string, sizeof(dc_appid));
		Q_snprintfz(frame, sizeof(frame), "{\"v\":1,\"client_id\":\"%s\"}", dc_appid);
		if (!DC_Send(0, frame))
			return;
		dc_state = 1;
		dc_nextsend = realtime;
		Con_DPrintf("discord: connected, handshake sent\n");
	}
	DC_Read();
	if (dc_state != 2)
		return;
	if (!dc_said)
	{
		dc_said = true;
		Con_Printf("Discord: your profile shows what you are playing (cl_discord 0 to hide it).\n");
	}
	if (realtime < dc_nextbuild)
		return;
	dc_nextbuild = realtime + 1;
	DC_Build(dc_want, sizeof(dc_want));
	if (!strcmp(dc_want, dc_sent) || realtime < dc_nextsend)
		return;
	Q_snprintfz(frame, sizeof(frame), "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%u,\"activity\":%s},\"nonce\":\"%u\"}",
#ifdef _WIN32
		(unsigned)GetCurrentProcessId(),
#else
		(unsigned)getpid(),
#endif
		dc_want, ++dc_nonce);
	if (DC_Send(1, frame))
	{
		Q_strncpyz(dc_sent, dc_want, sizeof(dc_sent));
		dc_nextsend = realtime + 4;
	}
}

void CL_Discord_Shutdown(void)
{
	if (DC_OPEN)
		DC_Close(0);
}

void CL_Discord_Init(void)
{
	Cvar_Register(&cl_discord, "FTESurf");
	Cvar_Register(&cl_discord_appid, "FTESurf");
	Cvar_Register(&cl_discord_ipcdir, "FTESurf");
}
