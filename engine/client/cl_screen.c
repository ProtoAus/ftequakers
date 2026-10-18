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

// cl_screen.c -- master for refresh, status bar, console, chat, notify, etc

#include "quakedef.h"
#ifdef GLQUAKE
#include "glquake.h"//would prefer not to have this
#endif
#include "shader.h"
#include "gl_draw.h"
#include "fs.h"

//name of the current backdrop for the loading screen
char levelshotname[MAX_QPATH];
extern cvar_t con_textsize;


//nettest: r_speeds_dump -- print the r_speeds table to the console instead of only drawing it.
//Reading a 40-row table of microsecond figures off a downscaled screenshot is lossy and slow;
//with -condebug this lands in qconsole.log as exact text, which is what the reconciliation
//arithmetic actually needs.  One-shot: set the flag, next RSpeedShow prints and clears it.
static int rspeed_dumpreq;

/*
FTESurf Patch 273 -- "Framerate (refresh only)" MEANT IT, AND NOBODY READ IT.

RSPEED_TOTALREFRESH is closed around SCR_UpdateScreen alone (cl_main.c:7761).
The listen server gets its OWN bracket around SV_Frame, three of them
(cl_main.c:7457, :7592, :7663), and SV_Frame is called once per Host_Frame
BESIDE the refresh, not inside it.  So the headline framerate this table has
always printed is 1/refresh, and on a listen server -- which is every single
-player session this game has -- it omits the server entirely.

MEASURED on surf_tensor2 at the spawn, post-Patch-271, uncapped.  The refresh
sub-buckets reconcile against their parent, which is how we know Server is the
one sitting outside:

    CSQC Drawing 2590.94 + 2d 256.78 + Present 178.27 + Protocol 3.76
      + overhead 58.92 + setup 0.55 + composite 0.11 + GL reset 3.40
      + Audio 0.58                                    = 3093.31
    Total refresh (reported)                            3094.03   <- closed
    Server                                              1790.68   <- OUTSIDE

    "Framerate (refresh only)"   323 fps
    what the frame actually is   1/(3094+1791) = 205 fps

The user reported "200-300" on this map and the counter said 323.  The counter
was not wrong, it was answering a different question, and the 1790us of server
-- 37% of the frame, the single largest item -- was invisible to every A/B this
project has ever run.  That is why this prints BOTH: the old number stays so
old measurements remain comparable, and the honest one sits under it in caps.

Deliberately just refresh+server rather than a sum over every bucket: those two
are the only top-level brackets in the host frame, and summing the array would
double-count every nested child.  If a third top-level bracket is ever added,
add it here or this line quietly starts lying the same way.
*/
#define RSPEED_REALFRAME(a) ((a)[RSPEED_TOTALREFRESH] + (a)[RSPEED_SERVER] + (a)[RSPEED_SERVERIDLE])	//Patch 782 split the between-frames server out

static void RSpeed_Dump_f(void)
{
	if (r_speeds.ival <= 1)
	{
		Con_Printf("r_speeds_dump: needs r_speeds 2 or higher (the sampler only runs then)\n");
		return;
	}
	rspeed_dumpreq = 1;
}

/*
==================================================
r_budget					FTESurf Patch 782

Where a frame goes, as bars: the CPU's work in groups, what is left of the frame interval as
idle, and the GPU's time per pass from timestamp queries (GLBudget_*, gl_backend.c).  It reads
the rspeeds[] buckets r_speeds 2 reads, as SELF times -- a bucket less the buckets nested in it,
budget_parent below -- so every microsecond lands in exactly one group.  ENGINE_PATCHES.md,
Patch 782, has the tree and what it cannot see.
==================================================
*/
int r_speeds_timing;
double rspeeds_self[RSPEED_MAX];	//us: each bucket's own time: its brackets less the brackets that closed inside them
int rspeed_depth;			//how many brackets are open; reset at the top of each host frame
#define RSPEED_DEPTH 32
static double rspeed_child[RSPEED_DEPTH];	//time of the brackets that closed inside the open one at each depth

static int rspeed_unclosed, rspeed_offthread;	//r_budget_dump's check on the nesting
double RSpeed_Begin(int *depth)
{
	int d;
	if (!r_speeds_timing)
	{
		*depth = -1;
		return 0;
	}
	if (!Sys_IsMainThread())
	{	//a worker's bracket: its time still counts for r_speeds, but it is no part of this thread's nesting
		*depth = -2;
		rspeed_offthread++;
		return Sys_DoubleTime() * 1000000;
	}
	d = rspeed_depth++;
	if (d < RSPEED_DEPTH)
		rspeed_child[d] = 0;
	*depth = d;
	return Sys_DoubleTime() * 1000000;
}
void RSpeed_Finish(int spt, double start, int d)
{
	double elapsed;
	if (d == -2)
	{
		rspeeds[spt] += Sys_DoubleTime() * 1000000 - start;
		return;
	}
	if (d < 0)
		return;	//opened while timing was off
	if (rspeed_depth != d + 1)
		rspeed_unclosed++;
#ifdef GLQUAKE
	if (r_speeds.ival > 2 && qrenderer == QR_OPENGL && qglFinish)
		qglFinish();	//r_speeds 3: each bucket waits for its GPU work too
#endif
	elapsed = Sys_DoubleTime() * 1000000 - start;
	rspeeds[spt] += elapsed;
	if (d < RSPEED_DEPTH)
		rspeeds_self[spt] += max(0, elapsed - rspeed_child[d]);
	rspeed_depth = d;	//also closes whatever opened inside and never closed (an early return)
	if (d > 0 && d-1 < RSPEED_DEPTH)
		rspeed_child[d-1] += elapsed;
}

cvar_t r_budget = CVARFD("r_budget", "0", 0, "FTESurf Patch 782. Shows where each frame's time goes: CPU work by group, idle, and GPU time by pass (GL timer queries). 1 = compact: two bars and the three biggest items under each, 2 = a row per group, 3 = every bucket in each group. r_budget_dump prints it and copies it to the clipboard.");
cvar_t r_budget_window = CVARFD("r_budget_window", "0.5", 0, "FTESurf Patch 782. r_budget averages over this many seconds.");
static void R_FrameTime_AfterDump(void);	//FTESurf Patch 801
cvar_t r_budget_csqc = CVARFD("r_budget_csqc", "0", 0, "FTESurf Patch 800. With r_budget on, also breaks CSQC down by QC source file (and at r_budget 3 its three busiest functions). Runs the QC profiler while on, which slows QC; off, it costs nothing.");

typedef struct
{
	int b;
	const char *name;
} budgetmember_t;
typedef struct
{
	const char *name;
	float col[3];
	budgetmember_t m[10];
} budgetgroup_t;
//every bucket in exactly one group, as its self time; the CPU's groups sum to its work
#define BUDGET_PREP 3	//the PrepareFrame group, named by map format below
static const budgetgroup_t budget_groups[] =
{
	{"Server: game logic, physics",	{0.35,0.55,1.00},	{{RSPEED_SERVER, "server frames"}, {RSPEED_SERVERIDLE, "between frames"}, {-1}}},
	{"Game client code (CSQC)",		{0.85,0.45,0.85},	{{RSPEED_CSQC_QCVIEW, "QC UpdateView, HUD"}, {RSPEED_CSQC_PREDICT, "prediction"}, {RSPEED_CSQCPHYSICS, "QC physics"}, {RSPEED_CSQCREDRAW, "other"}, {-1}}},
	{"World: walk, entities, lightmaps",{0.35,0.85,0.35},	{{RSPEED_WORLDNODE, "world walking"}, {RSPEED_LINKENTITIES, "entity setup"}, {RSPEED_DYNAMIC, "lightmap updates"}, {-1}}},
	{"Map vis + props (HL2 plugin)",	{1.00,0.60,0.20},	{{RSPEED_WORLDPREPARE, "PrepareFrame"}, {-1}}},
	{"Draw calls (CPU side)",		{0.95,0.85,0.30},	{{RSPEED_OPAQUE, "opaque"}, {RSPEED_TRANSPARENTS, "transparent"}, {RSPEED_RTLIGHTS, "rt lights"}, {RSPEED_SUBMIT, "gl_finish"}, {-1}}},
	{"Shadows",						{0.55,0.40,0.85},	{{RSPEED_FAKESHADOWS, "generate"}, {RSPEED_SHADOW_CLASSIFY, "classify"}, {RSPEED_SHADOW_ENTDRAW, "entity draw"}, {-1}}},
	{"Particles",					{1.00,0.40,0.40},	{{RSPEED_PARTICLES, "script particles"}, {RSPEED_PARTICLESDRAW, "classic"}, {-1}}},
	{"Post-processing",				{0.30,0.85,0.85},	{{RSPEED_POSTPROC, "resolve, effects, bloom"}, {-1}}},
	{"HUD, 2D, this overlay",		{0.85,0.85,0.85},	{{RSPEED_2D, "2d elements"}, {RSPEED_PALETTEFLASHES, "palette flashes"}, {RSPEED_RSPEEDSHOW, "r_budget / r_speeds"}, {-1}}},
	{"Display: swap/vsync, pacing",	{0.55,0.55,0.60},	{{RSPEED_PRESENT, "swap/present"}, {RSPEED_SCR_PACING, "frame pacing"}, {RSPEED_SCR_SETUP, "screen setup"}, {RSPEED_SCR_COMPOSITE, "composite"}, {RSPEED_SCR_BRIGHTEN, "brighten/capture"}, {RSPEED_SCR_RESET, "reset check"}, {RSPEED_TOTALREFRESH, "other refresh"}, {RSPEED_SETUP, "acquire wait"}, {RSPEED_ACQUIRE, "acquire"}, {-1}}},
	{"Input, network, audio",		{0.30,0.70,0.60},	{{RSPEED_INPUT, "input, console"}, {RSPEED_PROTOCOL, "network"}, {RSPEED_AUDIO, "audio"}, {-1}}},
	{"Loading (main thread)",		{0.90,0.30,0.30},	{{RSPEED_MAINTHREAD, "COM_MainThreadWork"}, {-1}}},
	{"Other host work",				{0.40,0.40,0.40},	{{RSPEED_HOSTWORK, "untimed"}, {-1}}},
};
#define BUDGET_GROUPS (sizeof(budget_groups)/sizeof(budget_groups[0]))
//r_budget 1's legend, in budget_groups' order
static const char *budget_groupshort[] = {"server", "csqc", "world", "hl2 props", "draws", "shadows", "particles", "post", "hud", "display", "input/net", "loading", "other"};
static const char *budget_gpushort[GPUB_MAX] = {"other", "cpu wait", "world", "props", "sky", "3d sky", "portals", "shadows", "lights", "transparent", "post", "hud"};
static const char *budget_gpunames[GPUB_MAX] =
{
	"Other: clears, setup",	"Waiting on the CPU",	"World",	"Models & props",	"Sky",	"3D sky (skyroom)",	"Portals & reflections",
	"Shadows",	"RT lights",	"Transparent & effects",	"Post-processing",	"HUD & 2D, with waits on its QC"
};
static const float budget_gpucols[GPUB_MAX][3] =
{
	{0.40,0.40,0.40},	{0.25,0.25,0.30},	{0.35,0.85,0.35},	{1.00,0.60,0.20},	{0.50,0.75,1.00},	{0.40,0.60,0.90},	{0.75,0.60,0.95},
	{0.55,0.40,0.85},	{1.00,1.00,0.70},	{0.95,0.85,0.30},	{0.30,0.85,0.85},	{0.85,0.85,0.85}
};
//the PrepareFrame group is the hl2 plugin only on a Source map; elsewhere it is the engine's own vis and world walk
static const char *R_Budget_GroupName(int i, qboolean shortname)
{
	if (i == BUDGET_PREP && !(cl.worldmodel && (cl.worldmodel->engineflags & MDLF_SOURCEBSP)))
		return shortname ? "map vis" : "Map vis + walk (PrepareFrame)";
	return shortname ? budget_groupshort[i] : budget_groups[i].name;
}

#define BUDGET_QCFILES 16	//published, and in the dump
#define BUDGET_QCROWS 10	//on the panel, functions under the first BUDGET_QCFUNCROWS: r_budget 3 fits 1200 px
#define BUDGET_QCFUNCROWS 4
#define BUDGET_QCACC 64
typedef struct
{
	char file[32];
	float ms;
	struct {char name[40]; float ms;} top[3];
} budgetqc_t;
//one window's QC profile, summed per source file (us); the overflow goes to the last slot
static struct {budgetqc_t f[BUDGET_QCACC]; int n;} budget_qcacc;
static qboolean budget_qcprof;	//the CSQC VM is profiling for us; outside `budget`, which is wiped when r_budget comes on

static struct
{
	qboolean on;
	double winstart, lastframe;
	int frames, intervals;
	double interval;			//window sums: seconds between frames
	double cpu[RSPEED_MAX];		//...self microseconds
	double quant[RQUANT_MAX];
	unsigned int prevq[RQUANT_MAX];
	//published averages, per frame
	qboolean valid;
	float frame;				//ms
	float self[RSPEED_MAX];		//us, self time
	float gpu[GPUB_MAX], gputotal, gpubusy;	//ms; busy is the total less "Waiting on the CPU"
	int gpuframes;				//finished frames the GPU figures came from; 0 = none
	float quantavg[RQUANT_MAX];
	qboolean qcfull;			//the window now open started with the profiler on
	int nqc;					//published r_budget_csqc files, busiest first; 0 = none
	float qctotal;				//ms
	budgetqc_t qc[BUDGET_QCFILES];
} budget;

static void R_Budget_QCFunc(void *ctx, const char *func, const char *file, double selfus, double totalus)
{
	char b[sizeof(budget_qcacc.f[0].file)];
	budgetqc_t *f;
	int i, k;
	Q_strncpyz(b, COM_SkipPath(file), sizeof(b));
	COM_StripExtension(b, b, sizeof(b));
	for (i = 0; i < budget_qcacc.n; i++)
		if (!strcmp(budget_qcacc.f[i].file, b))
			break;
	if (i == budget_qcacc.n)
	{
		if (i == BUDGET_QCACC-1)
			Q_strncpyz(b, "(other files)", sizeof(b));
		if (i < BUDGET_QCACC)
		{
			memset(&budget_qcacc.f[i], 0, sizeof(budget_qcacc.f[i]));
			Q_strncpyz(budget_qcacc.f[i].file, b, sizeof(budget_qcacc.f[i].file));
			budget_qcacc.n++;
		}
		else
			i = BUDGET_QCACC-1;
	}
	f = &budget_qcacc.f[i];
	f->ms += selfus;
	for (k = 0; k < 3; k++)
		if (selfus > f->top[k].ms)
		{
			memmove(&f->top[k+1], &f->top[k], sizeof(f->top[0])*(2-k));
			Q_strncpyz(f->top[k].name, func, sizeof(f->top[k].name));
			f->top[k].ms = selfus;
			break;
		}
}
static int QDECL R_Budget_QCSort(const void *a, const void *b)
{
	float d = ((const budgetqc_t*)b)->ms - ((const budgetqc_t*)a)->ms;
	return (d > 0) - (d < 0);
}
//the window's QC profile as per-frame ms; the first window after switching on is partial, so not shown
static void R_Budget_QCPublish(void)
{
	int i, k;
	budget.nqc = 0;
	budget.qctotal = 0;
	if (!budget_qcprof)
	{
		budget.qcfull = false;
		return;
	}
	budget_qcacc.n = 0;
#ifdef CSQC_DAT
	CSQC_EnumProfile(R_Budget_QCFunc, NULL);
#endif
	if (budget.qcfull)
	{
		qsort(budget_qcacc.f, budget_qcacc.n, sizeof(budget_qcacc.f[0]), R_Budget_QCSort);
		for (i = 0; i < budget_qcacc.n; i++)
		{
			for (k = 0; k < 3; k++)
				budget_qcacc.f[i].top[k].ms /= budget.frames * 1000.0;
			budget_qcacc.f[i].ms /= budget.frames * 1000.0;
			budget.qctotal += budget_qcacc.f[i].ms;
			if (i < BUDGET_QCFILES)
				budget.qc[budget.nqc++] = budget_qcacc.f[i];
		}
	}
	budget.qcfull = true;
}
//between QC calls each frame: the profiler runs only while r_budget_csqc shows it
static void R_Budget_QCProfiler(qboolean want)
{
	if (want == budget_qcprof && !want)
		return;
#ifdef CSQC_DAT
	if (CSQC_SetProfiling(want))
	{
		if (want && !budget_qcprof)
			budget.qcfull = false;	//the window now open began unprofiled
		budget_qcprof = want;
	}
	else if (!CSQC_Inited())
		budget_qcprof = false;	//no VM to profile (a map change reloads it with the profiler off)
#else
	budget_qcprof = false;
#endif
}

static qboolean R_Budget_GPUTimers(void)
{
#ifdef GLQUAKE
	return qrenderer == QR_OPENGL && qglQueryCounter;
#else
	return false;
#endif
}

static void R_Budget_Publish(double now)
{
	int i;
	double gsum[GPUB_MAX], gtotal = 0;
	int gn = 0;

	if (budget.frames <= 0)
		return;
	for (i = 0; i < RSPEED_MAX; i++)
		budget.self[i] = budget.cpu[i] / budget.frames;
	for (i = 0; i < RQUANT_MAX; i++)
		budget.quantavg[i] = budget.quant[i] / budget.frames;
	budget.frame = budget.intervals ? budget.interval * 1000 / budget.intervals : 0;

#ifdef GLQUAKE
	if (qrenderer == QR_OPENGL)
		gn = R_BudgetGPUCollect(gsum, &gtotal);
#endif
	budget.gpuframes = gn;
	for (i = 0; i < GPUB_MAX; i++)
		budget.gpu[i] = gn ? gsum[i] / gn : 0;
	budget.gputotal = gn ? gtotal / gn : 0;
	budget.gpubusy = max(0, budget.gputotal - budget.gpu[GPUB_WAIT]);
	R_Budget_QCPublish();
	budget.valid = true;

	budget.winstart = now;
	budget.frames = budget.intervals = 0;
	budget.interval = 0;
	memset(budget.cpu, 0, sizeof(budget.cpu));
	memset(budget.quant, 0, sizeof(budget.quant));
}

//once a frame, from RSpeedShow: this frame's buckets into the window
static void R_Budget_Sample(void)
{
	int i;
	double now = Sys_DoubleTime();

	R_Budget_QCProfiler(r_budget.ival && r_budget_csqc.ival);
	if (!r_budget.ival)
	{
		budget.on = budget.valid = false;
		return;
	}
	if (!budget.on)
	{	//switched on: start clean, the counters' history is not this window's
		double discard[GPUB_MAX], dt;
		memset(&budget, 0, sizeof(budget));
		budget.on = true;
		budget.winstart = budget.lastframe = now;
		memset(rspeeds_self, 0, sizeof(rspeeds_self));
		for (i = 0; i < RQUANT_MAX; i++)
			budget.prevq[i] = rquant[i];
#ifdef GLQUAKE
		if (qrenderer == QR_OPENGL)
			R_BudgetGPUCollect(discard, &dt);
#endif
		return;
	}
	for (i = 0; i < RSPEED_MAX; i++)
		budget.cpu[i] += rspeeds_self[i];
	memset(rspeeds_self, 0, sizeof(rspeeds_self));	//ours alone: r_speeds reads the inclusive rspeeds[]
	//unsigned deltas: r_speeds' own sampler may have zeroed rquant since, and it wraps
	for (i = 0; i < RQUANT_MAX; i++)
		budget.quant[i] += (unsigned int)rquant[i] >= budget.prevq[i] ? (unsigned int)rquant[i] - budget.prevq[i] : (unsigned int)rquant[i];
	budget.interval += now - budget.lastframe;
	budget.intervals++;
	budget.lastframe = now;
	budget.frames++;
	if (now - budget.winstart >= bound(0.1, r_budget_window.value, 10))
		R_Budget_Publish(now);
}

//after RSpeedShow is done with them: remember where rquant stands, and keep the counters from wrapping when nothing else resets them
static void R_Budget_Rebase(void)
{
	int i;
	if (!budget.on)
		return;
	if (!r_speeds.ival)
	{
		memset(rspeeds, 0, sizeof(rspeeds));
		memset(rquant, 0, sizeof(rquant));
	}
	for (i = 0; i < RQUANT_MAX; i++)
		budget.prevq[i] = rquant[i];
}

static float R_Budget_Group(const budgetgroup_t *g)
{
	int j;
	float us = 0;
	for (j = 0; j < countof(g->m) && g->m[j].b >= 0; j++)
		us += budget.self[g->m[j].b];
	return us / 1000;
}
static float R_Budget_Work(void)
{
	size_t i;
	float ms = 0;
	for (i = 0; i < BUDGET_GROUPS; i++)
		ms += R_Budget_Group(&budget_groups[i]);
	return ms;
}
//the swap and frame pacing are inside the work brackets but are waits: vsync, or the GPU catching up
static float R_Budget_Busy(float work)
{
	float busy = work - (budget.self[RSPEED_PRESENT] + budget.self[RSPEED_SCR_PACING]) / 1000;
	return (busy > 0) ? busy : 0;
}
static const char *R_Budget_Verdict(float work)
{
	float busy = R_Budget_Busy(work);
	if (budget.frame <= 0)
		return "";
	if (budget.gpuframes && budget.gpubusy > busy && budget.gpubusy > budget.frame * 0.85)
		return "GPU-bound: the GPU is busy nearly the whole frame";
	if (busy > budget.frame * 0.85)
		return "CPU-bound: the CPU is busy nearly the whole frame";
	return "headroom: the frame waits (fps cap, vsync or pacing)";
}

//one row: a label, its milliseconds and share of the frame, and a bar on one scale for all rows
#define BUDGET_SHOWN(ms) ((ms) >= 0.005f)	//rows under this are not drawn: they cost glyphs and say nothing

//text untinted: the 2D colour is also the text's tint, and the last bar's would colour it
static void R_Budget_Text(float x, float y, const char *s, float w, qboolean right)
{
	R2D_ImageColours(1, 1, 1, 1);
	Draw_FunStringWidthFont(font_console, x, y, s, w, right, false);
}

//one row: a label, its milliseconds and share of the frame, and a bar on one scale for all rows
#define BUDGET_LABEL 32		//characters: the longest group name
#define BUDGET_COLS (BUDGET_LABEL + 12)	//label, "%6.2f %3.0f%%", a space
static float budget_adv, budget_w;	//per draw: the console font's advance (its glyphs are narrower than con_textsize, the line height), the panel width
static void R_Budget_Row(float x, float y, float cw, float ms, float scale, const float *col, const char *label, qboolean indent)
{
	float barx = x + BUDGET_COLS*budget_adv, barw = budget_w - BUDGET_COLS*budget_adv, w;
	//one string per row, columns by padding: the console font is fixed-width, and glyph runs are this overlay's whole cost
	R_Budget_Text(x, y, va("^7%s%-*.*s%6.2f %3.0f%%", indent ? "  " : "", indent ? BUDGET_LABEL-2 : BUDGET_LABEL, indent ? BUDGET_LABEL-2 : BUDGET_LABEL,
		label, ms, scale > 0 ? ms*100/scale : 0), BUDGET_COLS*budget_adv, false);
	w = scale > 0 ? barw * ms / scale : 0;
	if (w > barw)
		w = barw;
	if (w > 0)
	{
		R2D_ImageColours(col[0], col[1], col[2], indent ? 0.6 : 0.95);
		R2D_FillBlock(barx, y + 1, max(w, 1), cw - 2);
	}
}

//r_budget 1: the three biggest of a bar's items, each a colour chip, its name and milliseconds
static void R_Budget_Legend(float x, float y, float cw, const char *what, const float *val, const char **name, const float **col, int n)
{
	qboolean used[32];
	float bx = x + 4*cw;
	int k, i, best;

	memset(used, 0, sizeof(used));
	R_Budget_Text(x, y, va("^7%s", what), 4*cw, false);
	for (k = 0; k < 3; k++)
	{
		best = -1;
		for (i = 0; i < n && i < countof(used); i++)
			if (!used[i] && BUDGET_SHOWN(val[i]) && (best < 0 || val[i] > val[best]))
				best = i;
		if (best < 0)
			break;
		used[best] = true;
		R2D_ImageColours(col[best][0], col[best][1], col[best][2], 0.95);
		R2D_FillBlock(bx, y + 2, cw - 4, cw - 4);
		R_Budget_Text(bx + cw, y, va("^7%s %.2f", name[best], val[best]), 18*cw, false);
		bx += (strlen(name[best]) + 8) * cw;
	}
}

static const float budget_qccols[][3] =
{
	{0.85,0.45,0.85},	{0.55,0.75,1.00},	{0.95,0.85,0.30},	{0.35,0.85,0.35},
	{1.00,0.60,0.20},	{0.30,0.85,0.85},	{1.00,0.40,0.40},	{0.70,0.70,0.75}
};
static void R_Budget_Draw(void)
{
	float cw, x = 8, y = 8, w, scale, work, idle, bx, seg, ms, lines;
	int i, j;
	const budgetgroup_t *g;
	static const float idlecol[3] = {0.15,0.15,0.18};

	if (!budget.valid)
		return;
	if (con_textsize.value < 0)
		cw = (-con_textsize.value * vid.height) / vid.pixelheight;
	else
		cw = con_textsize.value;
	if (!cw)
		cw = 8;

	work = R_Budget_Work();
	idle = budget.frame - work;
	if (idle < 0)
		idle = 0;
	scale = max(budget.frame, max(work, budget.gputotal));
	{
		int px, py;
		Font_BeginString(font_console, 0, 0, &px, &py);
		budget_adv = Font_CharWidth(CON_WHITEMASK, '0') * (float)vid.width / vid.rotpixelwidth;
		Font_EndString(font_console);
		if (budget_adv <= 0)
			budget_adv = cw;
	}
	w = budget_w = max(59*cw, BUDGET_COLS*budget_adv + 16*cw);

	//count what will be drawn, so the backdrop fits it
	if (r_budget.ival == 1)
		lines = 2.5 + 2 + 2 + (budget.nqc ? 1 : 0);	//compact: header, the two bars, a legend line for each, and qc's
	else
	{
		lines = 0;
		if (budget.nqc)
		{	//heading, files, then the total and its gap
			lines += 1 + 1.5;
			for (i = 0; i < budget.nqc && i < BUDGET_QCROWS; i++)
				if (BUDGET_SHOWN(budget.qc[i].ms))
				{
					lines++;
					if (r_budget.ival >= 3 && i < BUDGET_QCFUNCROWS)
						for (j = 0; j < 3; j++)
							if (BUDGET_SHOWN(budget.qc[i].top[j].ms))
								lines++;
				}
		}
		lines += 2.5 + 2.5 + 1 + 1 + 1.5 + 1 + 0.5 + 1;	//header, bars, cpu heading, idle, gpu heading, footer
		for (i = 0; i < BUDGET_GROUPS; i++)
		{
			g = &budget_groups[i];
			if (!BUDGET_SHOWN(R_Budget_Group(g)))
				continue;
			lines++;
			if (r_budget.ival >= 3)
				for (j = 0; j < countof(g->m) && g->m[j].b >= 0; j++)
					if (BUDGET_SHOWN(budget.self[g->m[j].b]/1000))
						lines++;
		}
		for (i = 0; i < GPUB_MAX; i++)
			if (budget.gpuframes && BUDGET_SHOWN(budget.gpu[i]))
				lines++;
	}
	R2D_ImageColours(0, 0, 0, 0.6);
	R2D_FillBlock(x - 4, y - 4, w + 8, lines*cw + 8);

	R_Budget_Text(x, y, va("^3frame %.2f ms  %.0f fps^7   cpu busy %.2f   gpu %s   idle %.2f   ^5panel %.2f",
		budget.frame, budget.frame > 0 ? 1000/budget.frame : 0, R_Budget_Busy(work),
		budget.gpuframes ? va("%.2f", budget.gpubusy) : R_Budget_GPUTimers() ? "..." : "n/a", idle,
		budget.self[RSPEED_RSPEEDSHOW]/1000), w, false);
	y += cw;
	R_Budget_Text(x, y, va("^7%s", R_Budget_Verdict(work)), w, false);
	y += cw*1.5;

	//the frame as two stacked bars on one scale: the CPU's groups then idle, and the GPU's passes
	R_Budget_Text(x, y, "^7cpu", 4*cw, false);
	bx = x + 4*cw;
	for (i = 0; i < BUDGET_GROUPS; i++)
	{
		seg = (w - 4*cw) * R_Budget_Group(&budget_groups[i]) / scale;
		if (seg <= 0)
			continue;
		R2D_ImageColours(budget_groups[i].col[0], budget_groups[i].col[1], budget_groups[i].col[2], 0.95);
		R2D_FillBlock(bx, y + 1, seg, cw - 2);
		bx += seg;
	}
	seg = (w - 4*cw) * idle / scale;
	if (seg > 0)
	{
		R2D_ImageColours(idlecol[0]*2, idlecol[1]*2, idlecol[2]*2, 0.95);
		R2D_FillBlock(bx, y + 1, seg, cw - 2);
	}
	y += cw;
	R_Budget_Text(x, y, "^7gpu", 4*cw, false);
	bx = x + 4*cw;
	for (i = 0; i < GPUB_MAX && budget.gpuframes; i++)
	{
		seg = (w - 4*cw) * budget.gpu[i] / scale;
		if (seg <= 0)
			continue;
		R2D_ImageColours(budget_gpucols[i][0], budget_gpucols[i][1], budget_gpucols[i][2], 0.95);
		R2D_FillBlock(bx, y + 1, seg, cw - 2);
		bx += seg;
	}
	if (r_budget.ival == 1)
	{	//compact: what the colours are, the three biggest items under each bar
		float vals[BUDGET_GROUPS];
		const float *cols[GPUB_MAX > BUDGET_GROUPS ? GPUB_MAX : BUDGET_GROUPS];
		const char *names[BUDGET_GROUPS];
		y += cw;
		for (i = 0; i < BUDGET_GROUPS; i++)
		{
			vals[i] = R_Budget_Group(&budget_groups[i]);
			cols[i] = budget_groups[i].col;
			names[i] = R_Budget_GroupName(i, true);
		}
		R_Budget_Legend(x, y, cw, "cpu", vals, names, cols, BUDGET_GROUPS);
		y += cw;
		if (budget.gpuframes)
		{
			for (i = 0; i < GPUB_MAX; i++)
				cols[i] = budget_gpucols[i];
			R_Budget_Legend(x, y, cw, "gpu", budget.gpu, budget_gpushort, cols, GPUB_MAX);
		}
		if (budget.nqc)
		{
			float qv[BUDGET_QCFILES];
			const char *qn[BUDGET_QCFILES];
			const float *qcol[BUDGET_QCFILES];
			y += cw;
			for (i = 0; i < budget.nqc; i++)
			{
				qv[i] = budget.qc[i].ms;
				qn[i] = budget.qc[i].file;
				qcol[i] = budget_qccols[i % countof(budget_qccols)];
			}
			R_Budget_Legend(x, y, cw, "qc", qv, qn, qcol, budget.nqc);
		}
		R2D_ImageColours(1, 1, 1, 1);
		return;
	}
	y += cw*1.5;

	R_Budget_Text(x, y, va("^3%-*s    ms frame", BUDGET_LABEL, "CPU work by group"), BUDGET_COLS*budget_adv, false);
	y += cw;
	for (i = 0; i < BUDGET_GROUPS; i++)
	{
		g = &budget_groups[i];
		ms = R_Budget_Group(g);
		if (!BUDGET_SHOWN(ms))
			continue;
		R_Budget_Row(x, y, cw, ms, scale, g->col, R_Budget_GroupName(i, false), false);
		y += cw;
		if (r_budget.ival >= 3)
			for (j = 0; j < countof(g->m) && g->m[j].b >= 0; j++)
			{
				ms = budget.self[g->m[j].b]/1000;
				if (!BUDGET_SHOWN(ms))
					continue;
				R_Budget_Row(x, y, cw, ms, scale, g->col, g->m[j].name, true);
				y += cw;
			}
	}
	R_Budget_Row(x, y, cw, idle, scale, idlecol, "Idle (waiting for next frame)", false);
	y += cw*1.5;

	if (budget.nqc)
	{	//on the QC total's scale, so the split shows. Not CSQC's row: QC run from entity setup is under World there
		R_Budget_Text(x, y, va("^3%-*s    ms   qc", BUDGET_LABEL, "CSQC by QC file (profiler on)"), BUDGET_COLS*budget_adv, false);
		y += cw;
		for (i = 0; i < budget.nqc && i < BUDGET_QCROWS; i++)
		{
			const float *col = budget_qccols[i % countof(budget_qccols)];
			if (!BUDGET_SHOWN(budget.qc[i].ms))
				continue;
			R_Budget_Row(x, y, cw, budget.qc[i].ms, budget.qctotal, col, budget.qc[i].file, false);
			y += cw;
			if (r_budget.ival >= 3 && i < BUDGET_QCFUNCROWS)
				for (j = 0; j < 3; j++)
					if (BUDGET_SHOWN(budget.qc[i].top[j].ms))
					{
						R_Budget_Row(x, y, cw, budget.qc[i].top[j].ms, budget.qctotal, col, budget.qc[i].top[j].name, true);
						y += cw;
					}
		}
		R_Budget_Text(x, y, va("^7%-*s%6.2f  (profiled: QC runs slower)", BUDGET_LABEL, "all QC", budget.qctotal), w, false);
		y += cw*1.5;
	}

	if (budget.gpuframes)
	{
		R_Budget_Text(x, y, va("^3%-*s    ms frame", BUDGET_LABEL, "GPU time by pass"), BUDGET_COLS*budget_adv, false);
		y += cw;
		for (i = 0; i < GPUB_MAX; i++)
		{
			if (!BUDGET_SHOWN(budget.gpu[i]))
				continue;
			R_Budget_Row(x, y, cw, budget.gpu[i], scale, budget_gpucols[i], budget_gpunames[i], false);
			y += cw;
		}
	}
	else
	{
		R_Budget_Text(x, y, R_Budget_GPUTimers() ? "^7GPU: waiting for timer results" : "^7GPU: no timer queries on this renderer (GL 3.3 needed)", w, false);
		y += cw;
	}
	y += cw*0.5;
	R_Budget_Text(x, y, va("^7draws %.0f   triangles %.2fM   batches: world %.0f, entity %.0f",
		budget.quantavg[RQUANT_DRAWS], budget.quantavg[RQUANT_PRIMITIVEINDICIES]/3000000, budget.quantavg[RQUANT_WORLDBATCHES], budget.quantavg[RQUANT_ENTBATCHES]), w, false);
	R2D_ImageColours(1, 1, 1, 1);
}

//the same table as text: printed, and put on the clipboard to paste into a report
static void R_Budget_Dump_f(void)
{
	char out[16384], bar[33];
	float work, idle, ms, scale;
	int i, j, n;
	const budgetgroup_t *g;

	if (!budget.valid)
	{
		Con_Printf("r_budget_dump: turn r_budget on and give it %g seconds first\n", r_budget_window.value);
		return;
	}
	work = R_Budget_Work();
	idle = max(0, budget.frame - work);
	scale = max(budget.frame, max(work, budget.gputotal));
#define BUDGET_BAR(v) (n = scale > 0 ? (int)(32*(v)/scale + 0.5) : 0, n = bound(0, n, 32), memset(bar, '#', n), bar[n] = 0, bar)
	*out = 0;
	Q_strncatz(out, va("frame %.2f ms (%.0f fps)  cpu busy %.2f  gpu %s  idle %.2f  panel %.2f  -- %s\n", budget.frame, budget.frame > 0 ? 1000/budget.frame : 0,
		R_Budget_Busy(work), budget.gpuframes ? va("%.2f", budget.gpubusy) : "n/a", idle, budget.self[RSPEED_RSPEEDSHOW]/1000, R_Budget_Verdict(work)), sizeof(out));
	Q_strncatz(out, "CPU work by group                    ms  frame\n", sizeof(out));
	for (i = 0; i < BUDGET_GROUPS; i++)
	{
		g = &budget_groups[i];
		ms = R_Budget_Group(g);
		Q_strncatz(out, va("  %-32s %6.2f %4.0f%% %s\n", R_Budget_GroupName(i, false), ms, scale > 0 ? ms*100/scale : 0, BUDGET_BAR(ms)), sizeof(out));
		for (j = 0; j < countof(g->m) && g->m[j].b >= 0; j++)
			if (budget.self[g->m[j].b] > 0)
				Q_strncatz(out, va("      %-28s %6.2f\n", g->m[j].name, budget.self[g->m[j].b]/1000), sizeof(out));
	}
	Q_strncatz(out, va("  %-32s %6.2f %4.0f%% %s\n", "Idle (waiting for next frame)", idle, scale > 0 ? idle*100/scale : 0, BUDGET_BAR(idle)), sizeof(out));
	if (budget.gpuframes)
	{
		Q_strncatz(out, "GPU time by pass\n", sizeof(out));
		for (i = 0; i < GPUB_MAX; i++)
			Q_strncatz(out, va("  %-32s %6.2f %4.0f%% %s\n", budget_gpunames[i], budget.gpu[i], scale > 0 ? budget.gpu[i]*100/scale : 0, BUDGET_BAR(budget.gpu[i])), sizeof(out));
	}
	else
		Q_strncatz(out, "GPU: no timer results\n", sizeof(out));
	if (budget.nqc)
	{
		Q_strncatz(out, va("CSQC by QC file (profiled, QC runs slower)   ms   qc  -- all QC %.2f ms\n", budget.qctotal), sizeof(out));
		for (i = 0; i < budget.nqc; i++)
		{
			ms = budget.qc[i].ms;
			Q_strncatz(out, va("  %-32s %6.2f %4.0f%%\n", budget.qc[i].file, ms, budget.qctotal > 0 ? ms*100/budget.qctotal : 0), sizeof(out));
			for (j = 0; j < 3; j++)
				if (budget.qc[i].top[j].ms > 0)
					Q_strncatz(out, va("      %-28s %6.3f\n", budget.qc[i].top[j].name, budget.qc[i].top[j].ms), sizeof(out));
		}
	}
	Q_strncatz(out, va("draws %.0f  triangles %.2fM  batches world %.0f entity %.0f  (%s, %s)\n", budget.quantavg[RQUANT_DRAWS],
		budget.quantavg[RQUANT_PRIMITIVEINDICIES]/3000000, budget.quantavg[RQUANT_WORLDBATCHES], budget.quantavg[RQUANT_ENTBATCHES],
		cl.worldmodel ? cl.worldmodel->name : "no map", version_string()), sizeof(out));
	Q_strncatz(out, va("timers since the last dump: %i closed with others still open inside, %i off the main thread\n", rspeed_unclosed, rspeed_offthread), sizeof(out));
	rspeed_unclosed = rspeed_offthread = 0;
#undef BUDGET_BAR
	{	//a line at a time: one Con_Printf of the whole table is cut at its print buffer
		char *line = out, *nl;
		while ((nl = strchr(line, '\n')))
		{
			Con_Printf("%.*s\n", (int)(nl - line), line);
			line = nl + 1;
		}
	}
	Sys_SaveClipboard(CBT_CLIPBOARD, out);
	Con_Printf("r_budget_dump: copied to the clipboard\n");
	budget.lastframe = Sys_DoubleTime();
	R_FrameTime_AfterDump();
}

//FTESurf Patch 801: r_frametime. One sample per rendered frame, from RSpeedShow. The graph keeps the
//slowest frame of each column's slice, so a spike is never averaged away; the lows come from a
//histogram updated as frames arrive and expire, so computing them never costs a frame of its own.
cvar_t r_frametime = CVARFD("r_frametime", "0", 0, "FTESurf Patch 801. A frame-time graph at the top right: the last r_frametime_seconds, each column the slowest frame in its slice, green, yellow over 1.5x the median and red over 2.5x (a stutter). Above it the average fps and the 1% and 0.1% lows (the average of the slowest 1% and 0.1% of frames) over r_frametime_window. r_frametime_dump lists the worst frames and where you were; r_frametime_reset starts the window again.");
cvar_t r_frametime_seconds = CVARFD("r_frametime_seconds", "10", 0, "FTESurf Patch 801. Seconds the r_frametime graph spans.");
cvar_t r_frametime_window = CVARFD("r_frametime_window", "30", 0, "FTESurf Patch 801. Seconds r_frametime's lows and stutter count cover (1 to 120).");

#define FT_RING 65536		//frames the window can hold: 120 s at 546 fps; past that the oldest go early
#define FT_BINMS 0.05f
#define FT_BINS 4000		//0.05 ms bins to 200 ms; the last also takes every slower frame, its exact times summed
#define FT_COLS 300
#define FT_WORST 8			//listed by the dump
#define FT_KEEP 64			//the window's slowest, kept exactly (with where the view was, when known)
#define FT_STUTTER 2.5f		//times the median
#define FT_EYE 64			//PM_VIEW_OFS standing: setpos takes the origin, the view is above it
typedef struct {float ms, at; vec3_t org, ang; qboolean known;} ftbad_t;
static struct
{
	qboolean on;
	double base, last;			//realtime when switched on, and of the previous frame
	float ms[FT_RING], at[FT_RING];	//each frame in the window, and when (s since base)
	int head, count;
	int bins[FT_BINS];
	double sum, oversum;		//ms: the window's, and the last bin's
	float colmax[FT_COLS];
	int colcount[FT_COLS];
	int col;
	double colstart;
	double nextstats;
	float avg, median, low1, low01, worst;	//ms
	int stutters;
	ftbad_t bad[FT_KEEP];		//slowest first
} ft;

static int R_FrameTime_Bin(float ms)
{
	int b = ms / FT_BINMS;
	return b < FT_BINS-1 ? b : FT_BINS-1;
}
static void R_FrameTime_Drop(void)
{
	int t = (ft.head - ft.count) & (FT_RING-1), b = R_FrameTime_Bin(ft.ms[t]);
	ft.bins[b]--;
	ft.sum -= ft.ms[t];
	if (b == FT_BINS-1)
		ft.oversum -= ft.ms[t];
	ft.count--;
}
//when one of the kept slowest leaves the window, the next may be in the ring but not the list: take
//them from the ring again (rare: only on an expiry). Where the view was survives for frames kept then.
static void R_FrameTime_Rebuild(void)
{
	static ftbad_t old[FT_KEEP];
	int i, j, k;
	float ms;
	memcpy(old, ft.bad, sizeof(old));
	memset(ft.bad, 0, sizeof(ft.bad));
	for (i = 0; i < ft.count; i++)
	{
		k = (ft.head - ft.count + i) & (FT_RING-1);
		ms = ft.ms[k];
		if (ms <= ft.bad[FT_KEEP-1].ms)
			continue;
		for (j = 0; ms <= ft.bad[j].ms; j++)
			;
		memmove(&ft.bad[j+1], &ft.bad[j], sizeof(ft.bad[0]) * (FT_KEEP-1 - j));
		memset(&ft.bad[j], 0, sizeof(ft.bad[j]));
		ft.bad[j].ms = ms;
		ft.bad[j].at = ft.at[k];
	}
	for (j = 0; j < FT_KEEP && ft.bad[j].ms; j++)
		for (i = 0; i < FT_KEEP && old[i].ms; i++)
			if (old[i].known && old[i].at == ft.bad[j].at && old[i].ms == ft.bad[j].ms)
			{
				ft.bad[j] = old[i];
				break;
			}
}
//the last bin's frames, slowest first: exact from bad[] (they are the window's slowest), then its mean
static float R_FrameTime_Over(int fromtop)
{
	int nb, n = ft.bins[FT_BINS-1];
	double sb = 0;
	for (nb = 0; nb < FT_KEEP && ft.bad[nb].ms >= (FT_BINS-1) * FT_BINMS; nb++)
		sb += ft.bad[nb].ms;
	nb = min(nb, n);
	if (fromtop < nb)
		return ft.bad[fromtop].ms;
	return n > nb ? (ft.oversum - sb) / (n - nb) : 0;
}
//the mean of the slowest n frames, walking the histogram down (a bin's frames count at its middle)
static float R_FrameTime_Slowest(int n)
{
	int b, c, k, left = n;
	double s = 0;
	for (b = FT_BINS-1; b >= 0 && left > 0; b--)
	{
		if (!ft.bins[b])
			continue;
		c = min(ft.bins[b], left);
		if (b == FT_BINS-1)
			for (k = 0; k < c; k++)
				s += R_FrameTime_Over(k);
		else
			s += c * (b + 0.5) * FT_BINMS;
		left -= c;
	}
	return n > left ? s / (n - left) : 0;
}
static void R_FrameTime_Stats(void)
{
	int b, c = 0, half = ft.count / 2, over;
	float stut;
	ft.avg = ft.count ? ft.sum / ft.count : 0;
	for (b = 0; b < FT_BINS; b++)
		if ((c += ft.bins[b]) > half)
			break;
	ft.median = (min(b, FT_BINS-1) + 0.5) * FT_BINMS;
	ft.low1 = R_FrameTime_Slowest(max(1, ft.count / 100));
	ft.low01 = R_FrameTime_Slowest(max(1, ft.count / 1000));
	ft.worst = ft.bad[0].ms;
	stut = ft.median * FT_STUTTER;
	for (over = 0, b = R_FrameTime_Bin(stut) + 1; b < FT_BINS; b++)
		over += ft.bins[b];
	ft.stutters = over;
}
static void R_FrameTime_Sample(void)
{
	double now = Sys_DoubleTime(), window, span;
	float ms, at;
	int b, i;

	if (!r_frametime.ival)
	{
		ft.on = false;
		return;
	}
	if (!ft.on)
	{
		memset(&ft, 0, sizeof(ft));
		ft.on = true;
		ft.base = ft.last = ft.colstart = now;
		return;
	}
	if (!vid.activeapp)
	{	//tabbed out: cl_idlefps throttles these, and nobody is watching
		ft.last = now;
		return;
	}
	ms = (now - ft.last) * 1000;
	ft.last = now;
	if (ms <= 0 || ms > 1000)
		return;	//minimised, or a load: not a frame anyone watched
	at = now - ft.base;

	window = bound(1, r_frametime_window.value, 120);
	while (ft.count && at - ft.at[(ft.head - ft.count) & (FT_RING-1)] > window)
		R_FrameTime_Drop();
	if (ft.count == FT_RING)
		R_FrameTime_Drop();
	ft.ms[ft.head] = ms;
	ft.at[ft.head] = at;
	ft.head = (ft.head + 1) & (FT_RING-1);
	ft.count++;
	b = R_FrameTime_Bin(ms);
	ft.bins[b]++;
	ft.sum += ms;
	if (b == FT_BINS-1)
		ft.oversum += ms;

	//the window's slowest, and where the view was for each
	for (i = 0; i < FT_KEEP && ft.bad[i].ms; i++)
		if (at - ft.bad[i].at > window)
		{
			R_FrameTime_Rebuild();
			break;
		}
	if (ms > ft.bad[FT_KEEP-1].ms)
	{
		for (i = 0; ms <= ft.bad[i].ms; i++)
			;
		memmove(&ft.bad[i+1], &ft.bad[i], sizeof(ft.bad[0]) * (FT_KEEP-1 - i));
		ft.bad[i].ms = ms;
		ft.bad[i].at = at;
		VectorCopy(r_refdef.vieworg, ft.bad[i].org);
		ft.bad[i].org[2] -= FT_EYE;
		VectorCopy(r_refdef.viewangles, ft.bad[i].ang);
		ft.bad[i].known = true;
	}

	//the graph's columns: a new one each slice; a gap leaves the ones it spans empty
	span = bound(1, r_frametime_seconds.value, 120) / FT_COLS;
	for (i = 0; now - ft.colstart >= span && i < FT_COLS; i++)
	{
		ft.col = (ft.col + 1) % FT_COLS;
		ft.colmax[ft.col] = 0;
		ft.colcount[ft.col] = 0;
		ft.colstart += span;
	}
	if (now - ft.colstart >= span)
		ft.colstart = now;
	ft.colmax[ft.col] = max(ft.colmax[ft.col], ms);
	ft.colcount[ft.col]++;

	if (now >= ft.nextstats)
	{
		R_FrameTime_Stats();
		ft.nextstats = now + 0.25;
	}
}
static void R_FrameTime_Draw(void)
{
	float cw, gw, colw, gh = 120, x, y, scale, v, h;
	static const float nice[] = {2, 5, 10, 15, 20, 30, 50, 100, 200, 500, 1000};
	int i, c;

	if (!ft.on || !ft.count)
		return;
	cw = (con_textsize.value < 0) ? (-con_textsize.value * vid.height) / vid.pixelheight : con_textsize.value;
	if (!cw)
		cw = 8;
	gw = bound(FT_COLS, vid.width - 24, 2*FT_COLS);
	colw = gw / FT_COLS;
	x = vid.width - gw - 12;
	y = 130;
	if (r_budget.ival && 8 + budget_w + 12 > x)
		y = vid.height - gh - 3*cw - 72;	//r_budget's panel is under the top right: go to the bottom
	for (i = 0, scale = 0; i < countof(nice) && !scale; i++)
		if (nice[i] >= ft.median * 3)
			scale = nice[i];
	if (!scale)
		scale = nice[countof(nice)-1];

	R2D_ImageColours(0, 0, 0, 0.6);
	R2D_FillBlock(x - 4, y - 4, gw + 8, gh + 3*cw + 8);
	R2D_ImageColours(1, 1, 1, 1);
	Draw_FunStringWidthFont(font_console, x, y, va("^3frametime^7  avg %.0f fps   ^31%% low %.0f   0.1%% low %.0f", ft.avg > 0 ? 1000/ft.avg : 0, ft.low1 > 0 ? 1000/ft.low1 : 0, ft.low01 > 0 ? 1000/ft.low01 : 0), gw, false, false);
	R2D_ImageColours(1, 1, 1, 1);
	Draw_FunStringWidthFont(font_console, x, y + cw, va("^7median %.2f ms  worst %.1f  ^1stutters %i^7/%gs  top %g", ft.median, ft.worst, ft.stutters, bound(1, r_frametime_window.value, 120), scale), gw, false, false);
	y += 2*cw + 4;

	//oldest on the left; each column the slowest frame of its slice
	for (i = 0; i < FT_COLS; i++)
	{
		c = (ft.col + 1 + i) % FT_COLS;
		if (!ft.colcount[c])
			continue;
		v = ft.colmax[c];
		h = min(v / scale, 1) * gh;
		if (v > ft.median * FT_STUTTER)
			R2D_ImageColours(1.0, 0.25, 0.25, 0.95);
		else if (v > ft.median * 1.5)
			R2D_ImageColours(0.95, 0.85, 0.3, 0.95);
		else
			R2D_ImageColours(0.35, 0.85, 0.35, 0.9);
		R2D_FillBlock(x + i*colw, y + gh - h, colw, max(h, 1));
	}
	//the median and the stutter line
	R2D_ImageColours(1, 1, 1, 0.5);
	R2D_FillBlock(x, y + gh - min(ft.median / scale, 1) * gh, gw, 1);
	R2D_ImageColours(1, 0.25, 0.25, 0.5);
	R2D_FillBlock(x, y + gh - min(ft.median * FT_STUTTER / scale, 1) * gh, gw, 1);
	R2D_ImageColours(1, 1, 1, 1);
}
//a dump stalled its own frame by 0.3 s in the P801 arm: asking must not add a stutter
static void R_FrameTime_AfterDump(void)
{
	ft.last = Sys_DoubleTime();
}
static void R_FrameTime_Reset_f(void)
{
	ft.on = false;	//the next frame starts clean
}
static void R_FrameTime_Dump_f(void)
{
	char out[4096];
	int i, b, c, n, p;
	static const float pct[] = {50, 90, 99, 99.9f};
	double now;
	if (!ft.on || !ft.count)
	{
		Con_Printf("r_frametime_dump: turn r_frametime on first\n");
		return;
	}
	R_FrameTime_Stats();
	now = Sys_DoubleTime() - ft.base;
	*out = 0;
	Q_strncatz(out, va("frametime over %.1f s, %i frames (%s, %s)\n", ft.at[(ft.head-1) & (FT_RING-1)] - ft.at[(ft.head - ft.count) & (FT_RING-1)], ft.count,
		cl.worldmodel ? cl.worldmodel->name : "no map", version_string()), sizeof(out));
	Q_strncatz(out, va("  avg %.2f ms (%.0f fps)  1%% low %.2f ms (%.0f fps)  0.1%% low %.2f ms (%.0f fps)\n", ft.avg, 1000/ft.avg,
		ft.low1, 1000/ft.low1, ft.low01, 1000/ft.low01), sizeof(out));
	for (p = 0; p < countof(pct); p++)
	{
		n = ft.count * pct[p] / 100;
		for (b = 0, c = 0; b < FT_BINS && (c += ft.bins[b]) <= n; b++)
			;
		Q_strncatz(out, va("  p%-4g %7.2f ms\n", pct[p], b < FT_BINS-1 ? (b + 0.5) * FT_BINMS : R_FrameTime_Over(ft.count - 1 - n)), sizeof(out));
	}
	Q_strncatz(out, va("  stutters (over %.1fx the median, %.2f ms): %i\n", FT_STUTTER, ft.median * FT_STUTTER, ft.stutters), sizeof(out));
	Q_strncatz(out, "  slowest frames: ms, seconds ago, where you stood (standing height) and looked\n", sizeof(out));
	for (i = 0; i < FT_WORST && ft.bad[i].ms; i++)
		Q_strncatz(out, ft.bad[i].known
			? va("    %7.2f  %6.1f s ago  setpos %.0f %.0f %.0f %.0f %.0f 0\n", ft.bad[i].ms, now - ft.bad[i].at,
				ft.bad[i].org[0], ft.bad[i].org[1], ft.bad[i].org[2], ft.bad[i].ang[0], ft.bad[i].ang[1])
			: va("    %7.2f  %6.1f s ago  (where was not kept)\n", ft.bad[i].ms, now - ft.bad[i].at), sizeof(out));
	{
		char *line = out, *nl;
		while ((nl = strchr(line, '\n')))
		{
			Con_Printf("%.*s\n", (int)(nl - line), line);
			line = nl + 1;
		}
	}
	Sys_SaveClipboard(CBT_CLIPBOARD, out);
	R_FrameTime_AfterDump();
}

void RSpeedShow(void)
{
	int i;
	static int samplerspeeds[RSPEED_MAX];
	static int samplerquant[RQUANT_MAX];
	int savedsamplerquant[RQUANT_MAX];	//so we don't count the r_speeds debug spam in draw counts.
	char *RSpNames[RSPEED_MAX];
	char *RQntNames[RQUANT_MAX];
	char *s;
	static int framecount;
	int frameinterval = 100;
	int tsize;

	R_Budget_Sample();	//FTESurf Patch 782
	if (r_budget.ival)
		R_Budget_Draw();
	R_FrameTime_Sample();	//FTESurf Patch 801
	if (r_frametime.ival)
		R_FrameTime_Draw();
	if (!r_speeds.ival)
	{
		R_Budget_Rebase();
		return;
	}

	if (con_textsize.value < 0)
		tsize = (-con_textsize.value * vid.height) / vid.pixelheight;	//size defined in physical pixels
	else
		tsize = con_textsize.value;	//size defined in virtual pixels.
	if (!tsize)
		tsize = 8;

	memset(RSpNames, 0, sizeof(RSpNames));

	RSpNames[RSPEED_TOTALREFRESH]	= "Total refresh";
	RSpNames[RSPEED_CSQCPHYSICS]	= " CSQC Physics";
	RSpNames[RSPEED_CSQCREDRAW]		= " CSQC Drawing";
	RSpNames[RSPEED_LINKENTITIES]	= "  Entity setup";
	RSpNames[RSPEED_WORLDNODE]		= "  World walking";
	RSpNames[RSPEED_DYNAMIC]		= "  Lightmap updates";
	RSpNames[RSPEED_FAKESHADOWS]	= "  Shadow generation";
	RSpNames[RSPEED_SHADOW_CLASSIFY]= "   Shadow classify";
	RSpNames[RSPEED_SHADOW_ENTDRAW]	= "   Shadow entdraw";
	RSpNames[RSPEED_CSQC_PREDICT]	= "  Prediction";
	RSpNames[RSPEED_CSQC_QCVIEW]	= "  QC UpdateView";
	RSpNames[RSPEED_POSTPROC]		= "  Postproc/resolve";
	RSpNames[RSPEED_RSPEEDSHOW]		= " r_speeds overhead";
	RSpNames[RSPEED_SCR_SETUP]		= " Screen setup";
	RSpNames[RSPEED_SCR_COMPOSITE]	= " Screen composite";
	RSpNames[RSPEED_SCR_BRIGHTEN]	= " Brighten/capture";
	RSpNames[RSPEED_SCR_PACING]		= " Frame pacing";
	RSpNames[RSPEED_SCR_RESET]		= " GL reset check";
	RSpNames[RSPEED_OPAQUE]			= "  Opaque Batches";
	RSpNames[RSPEED_RTLIGHTS]		= "  RT Lights";
	RSpNames[RSPEED_TRANSPARENTS]	= "  Transparent Batches";
	RSpNames[RSPEED_PARTICLES]		= "  Particle phys/sort";
	RSpNames[RSPEED_PARTICLESDRAW]	= "  Particle drawing";
	RSpNames[RSPEED_2D]				= " 2d Elements";
	RSpNames[RSPEED_PALETTEFLASHES]	= " Palette flashes";

	RSpNames[RSPEED_SETUP]			= "Acquire Wait";
	RSpNames[RSPEED_SUBMIT]			= "submit/finish";
	RSpNames[RSPEED_PRESENT]		= "Present";
	RSpNames[RSPEED_ACQUIRE]		= "Acquire Request";

	RSpNames[RSPEED_PROTOCOL]		= "Client Protocol";
	RSpNames[RSPEED_SERVER]			= "Server";
	RSpNames[RSPEED_AUDIO]			= "Audio";
	//FTESurf Patch 782; the table draws every bucket's name, so a new one without one prints garbage
	RSpNames[RSPEED_HOSTWORK]		= "Host frame work";
	RSpNames[RSPEED_MAINTHREAD]		= " Main-thread work";
	RSpNames[RSPEED_INPUT]			= " Input/console";
	RSpNames[RSPEED_SERVERIDLE]		= "Server (between frames)";
	RSpNames[RSPEED_WORLDPREPARE]	= "   Map PrepareFrame";

	memset(RQntNames, 0, sizeof(RQntNames));
	RQntNames[RQUANT_MSECS]					= "Microseconds";
	RQntNames[RQUANT_PRIMITIVEINDICIES]		= "Draw Indicies";
	RQntNames[RQUANT_DRAWS]					= "Draw Calls";
	RQntNames[RQUANT_2DBATCHES]				= "2d Batches";
	RQntNames[RQUANT_WORLDBATCHES]			= "World Batches";
	RQntNames[RQUANT_ENTBATCHES]			= "Ent Batches";
	RQntNames[RQUANT_SHADOWINDICIES]		= "Shadow Indicies";
	RQntNames[RQUANT_SHADOWEDGES]			= "Shadow Edges";
	RQntNames[RQUANT_SHADOWSIDES]			= "Shadowmap Sides";
	RQntNames[RQUANT_LITFACES]				= "Lit faces";

	RQntNames[RQUANT_RTLIGHT_DRAWN]			= "Lights Drawn";
	RQntNames[RQUANT_RTLIGHT_CULL_FRUSTUM]	= "Lights offscreen";
	RQntNames[RQUANT_RTLIGHT_CULL_PVS]		= "Lights PVS Culled";
	RQntNames[RQUANT_RTLIGHT_CULL_SCISSOR]	= "Lights Scissored";
	RQntNames[RQUANT_MODELLIGHTSAMPLE]		= "ModelLight Samples";

	memcpy(savedsamplerquant, rquant, sizeof(savedsamplerquant));
	if (r_speeds.ival > 1)
	{
		for (i = 0; i < RSPEED_MAX; i++)
		{
			s = va("%g %-24s", samplerspeeds[i]/(float)frameinterval, RSpNames[i]);
			Draw_FunStringWidthFont(font_console, 0, i*tsize, s, vid.width, true, false);
		}
	}
	for (i = 0; i < RQUANT_MAX; i++)
	{
		s = va("%u.%.3u %-24s", samplerquant[i]/frameinterval, (samplerquant[i]%100), RQntNames[i]);
		Draw_FunStringWidthFont(font_console, 0, (i+RSPEED_MAX)*tsize, s, vid.width, true, false);
	}
	if (r_speeds.ival > 1)
	{
		s = va("%f %-24s", (frameinterval*1000*1000.0f)/samplerspeeds[RSPEED_TOTALREFRESH], "Framerate (refresh only)");
		Draw_FunStringWidthFont(font_console, 0, (i+RSPEED_MAX)*tsize, s, vid.width, true, false);
		s = va("%f %-24s", (frameinterval*1000*1000.0f)/RSPEED_REALFRAME(samplerspeeds), "Framerate (INCL. SERVER)");
		Draw_FunStringWidthFont(font_console, 0, (i+RSPEED_MAX+1)*tsize, s, vid.width, true, false);
	}
	memcpy(rquant, savedsamplerquant, sizeof(rquant));

	if (rspeed_dumpreq)
	{	//nettest: same snapshot the table above is drawn from, as parseable text.
		rspeed_dumpreq = 0;
		Con_Printf("---- r_speeds_dump (us/frame, %i-frame average) ----\n", frameinterval);
		for (i = 0; i < RSPEED_MAX; i++)
			if (RSpNames[i])
				Con_Printf("%12.2f %s\n", samplerspeeds[i]/(float)frameinterval, RSpNames[i]);
		for (i = 0; i < RQUANT_MAX; i++)
			if (RQntNames[i])
				Con_Printf("%12u %s\n", samplerquant[i]/frameinterval, RQntNames[i]);
		if (samplerspeeds[RSPEED_TOTALREFRESH])
			Con_Printf("%12.2f %s\n", (frameinterval*1000*1000.0f)/samplerspeeds[RSPEED_TOTALREFRESH], "Framerate (refresh only)");
		if (RSPEED_REALFRAME(samplerspeeds))
		{
			Con_Printf("%12.2f %s\n", RSPEED_REALFRAME(samplerspeeds)/(float)frameinterval, "REAL frame (refresh+server)");
			Con_Printf("%12.2f %s\n", (frameinterval*1000*1000.0f)/RSPEED_REALFRAME(samplerspeeds), "Framerate (INCL. SERVER)");
		}
		Con_Printf("---- end r_speeds_dump ----\n");
	}

	if (++framecount>=frameinterval)
	{
		for (i = 0; i < RSPEED_MAX; i++)
		{
			samplerspeeds[i] = rspeeds[i];
			rspeeds[i] = 0;
		}
		for (i = 0; i < RQUANT_MAX; i++)
		{
			samplerquant[i] = rquant[i];
			rquant[i] = 0;
		}
		framecount=0;
	}
	R_Budget_Rebase();	//FTESurf Patch 782: after the sampler above may have zeroed them
}


/*

background clear
rendering
turtle/net/ram icons
sbar
centerprint / slow centerprint
notify lines
intermission / finale overlay
loading plaque
console
menu

required background clears
required update regions


syncronous draw mode or async
One off screen buffer, with updates either copied or xblited
Need to double buffer?


async draw will require the refresh area to be cleared, because it will be
xblited, but sync draw can just ignore it.

sync
draw

CenterPrint ()
SlowPrint ()
Screen_Update ();
Con_Printf ();

net
turn off messages option

the refresh is always rendered, unless the console is full screen


console is:
	notify lines
	half
	full


*/





float mousecursor_x, mousecursor_y;
float mousemove_x, mousemove_y;

float multicursor_x[8], multicursor_y[8];
qboolean multicursor_active[8];

float           scr_con_current;	//current console lines shown
float           scr_con_target;		//the target number of lines (not a local, because it helps to know if we're at the target yet, etc)

qboolean		scr_con_forcedraw;

extern cvar_t			scr_viewsize;
extern cvar_t			scr_fov;
extern cvar_t			scr_conspeed;
extern cvar_t			scr_centertime;
extern cvar_t			scr_logcenterprint;
extern cvar_t			scr_showturtle;
extern cvar_t			scr_turtlefps;
extern cvar_t			scr_showpause;
extern cvar_t			scr_printspeed;
extern cvar_t			scr_allowsnap;
extern cvar_t			scr_sshot_type;
extern cvar_t			scr_sshot_prefix;
extern cvar_t			scr_sshot_mapname;	//FTESurf Patch 211
extern cvar_t			crosshair;
extern cvar_t			scr_consize;
cvar_t			scr_neticontimeout = CVAR("scr_neticontimeout", "0.3");
cvar_t			scr_diskicontimeout = CVAR("scr_diskicontimeout", "0.3");

qboolean        scr_initialized;                // ready to draw

mpic_t          *scr_net;
mpic_t          *scr_turtle;

int                     clearconsole;
int                     clearnotify;

viddef_t        vid;                            // global video state

vrect_t         scr_vrect;

qboolean        scr_disabled_for_loading;
qboolean        scr_drawloading;
float           scr_disabled_time;

cvar_t	con_stayhidden = CVARFD("con_stayhidden", "1", CVAR_NOTFROMSERVER, "0: allow console to pounce on the user\n1: console stays hidden unless explicitly invoked\n2:toggleconsole command no longer works\n3: shift+escape key no longer works");
cvar_t	show_fps	= CVARAFD("show_fps"/*qw*/, "0", "scr_showfps"/*qs*/, CVAR_ARCHIVE, "Displays the current framerate on-screen.\n0: Off.\n1: framerate average over a second.\n2: Show a frametimes graph (with additional timing info).\n-1: Normalized graph that focuses on the variation ignoring base times.");
cvar_t	show_fps_x	= CVAR("show_fps_x", "-1");
cvar_t	show_fps_y	= CVAR("show_fps_y", "-1");
cvar_t	show_clock	= CVAR("cl_clock", "0");
cvar_t	show_clock_x	= CVAR("cl_clock_x", "0");
cvar_t	show_clock_y	= CVAR("cl_clock_y", "-1");
cvar_t	show_gameclock	= CVAR("cl_gameclock", "0");
cvar_t	show_gameclock_x	= CVAR("cl_gameclock_x", "0");
cvar_t	show_gameclock_y	= CVAR("cl_gameclock_y", "-1");
cvar_t	show_speed	= CVAR("show_speed", "0");
cvar_t	show_speed_x	= CVAR("show_speed_x", "-1");
cvar_t	show_speed_y	= CVAR("show_speed_y", "-9");
cvar_t	scr_showdisk	= CVAR("scr_showdisk", "0");
cvar_t	scr_showdisk_x	= CVAR("scr_showdisk_x", "-24");
cvar_t	scr_showdisk_y	= CVAR("scr_showdisk_y", "0");
cvar_t	scr_showobituaries = CVAR("scr_showobituaries", "0");

cvar_t	scr_loadingrefresh = CVARD("scr_loadingrefresh", "0", "Force redrawing of the loading screen, in order to display EVERY resource that is loaded");
cvar_t	scr_showloading	= CVAR("scr_showloading", "1");
//things to configure the legacy loading screen
cvar_t	scr_loadingscreen_picture = CVAR("scr_loadingscreen_picture", "gfx/loading");
cvar_t	scr_loadingscreen_aspect = CVARD("scr_loadingscreen_aspect", "0", "Controls the aspect of levelshot images.\n0: Use source image's aspect.\n1: Force 4:3 aspect (ignore image's aspect), for best q3 compat.\n2: Ignore aspect considerations and just smear it over the entire screen.\n-1: Disable levelshot use.");
cvar_t	scr_loadingscreen_scale = CVAR("scr_loadingscreen_scale", "1");
cvar_t	scr_loadingscreen_scale_limit = CVAR("scr_loadingscreen_scale_limit", "2");

void *scr_curcursor;


static void SCR_CPrint_f(void)
{
	int seat = CL_TargettedSplit(false);
	if (Cmd_Argc() == 2)
		SCR_CenterPrint(seat, Cmd_Argv(1), true);
	else
		SCR_CenterPrint(seat, Cmd_Args(), true);
}

extern char cl_screengroup[];
void CLSCR_Init(void)
{
	int i;
	Cmd_AddCommand("cprint", SCR_CPrint_f);

	Cvar_Register(&con_stayhidden, cl_screengroup);
	Cvar_Register(&scr_loadingrefresh, cl_screengroup);
	Cvar_Register(&scr_showloading, cl_screengroup);
	Cvar_Register(&scr_loadingscreen_picture, cl_screengroup);
	Cvar_Register(&scr_loadingscreen_scale, cl_screengroup);
	Cvar_Register(&scr_loadingscreen_scale_limit, cl_screengroup);
	Cvar_Register(&scr_loadingscreen_aspect, cl_screengroup);
	Cvar_Register(&show_fps, cl_screengroup);
	Cvar_Register(&show_fps_x, cl_screengroup);
	Cvar_Register(&show_fps_y, cl_screengroup);
	Cvar_Register(&show_clock, cl_screengroup);
	Cvar_Register(&show_clock_x, cl_screengroup);
	Cvar_Register(&show_clock_y, cl_screengroup);
	Cvar_Register(&show_gameclock, cl_screengroup);
	Cvar_Register(&show_gameclock_x, cl_screengroup);
	Cvar_Register(&show_gameclock_y, cl_screengroup);
	Cvar_Register(&show_speed, cl_screengroup);
	Cvar_Register(&show_speed_x, cl_screengroup);
	Cvar_Register(&show_speed_y, cl_screengroup);
	Cvar_Register(&scr_neticontimeout, cl_screengroup);
	Cvar_Register(&scr_showdisk, cl_screengroup);
	Cvar_Register(&scr_showdisk_x, cl_screengroup);
	Cvar_Register(&scr_showdisk_y, cl_screengroup);
	Cvar_Register(&scr_diskicontimeout, cl_screengroup);
	Cvar_Register(&scr_showobituaries, cl_screengroup);


	memset(&key_customcursor, 0, sizeof(key_customcursor));
	for (i = 0; i < kc_max; i++)
		key_customcursor[i].dirty = true;
	scr_curcursor = NULL;
	if (rf && rf->VID_SetCursor)
		rf->VID_SetCursor(scr_curcursor);
}

/*
===============================================================================

CENTER PRINTING

===============================================================================
*/

typedef struct cprint_s {
	unsigned int	flags;

	conchar_t		*string;
	conchar_t		*cursorchar;	//pointer into string
	size_t			stringbytes;
	size_t			charcount;
	char			titleimage[MAX_QPATH];
	float			time_start;   // for slow victory printing
	float			time_off;
	int				erase_lines;
	int				erase_center;

	int				oldmousex, oldmousey;	//so the cursorchar can be changed by keyboard without constantly getting stomped on.

	struct cprint_s *queue;	//switch to the next on timeout.
} cprint_t;

static cprint_t *scr_centerprint[MAX_SPLITS];

// SCR_StringToRGB: takes in "<index>" or "<r> <g> <b>" and converts to an RGB vector
void SCR_StringToRGB (char *rgbstring, float *rgb, float rgbinputscale)
{
	char *t;

	//hex values
	if (!strncmp(rgbstring, "0x", 2))
	{
		char *end;
		unsigned int val = strtoul(rgbstring+2, &end, 16);
		if (end == rgbstring + 5)
		{
			rgb[0] = ((val&0xf00)>>8)/15.0;
			rgb[1] = ((val&0x0f0)>>4)/15.0;
			rgb[2] = ((val&0x00f)>>0)/15.0;
		}
		else if (end == rgbstring + 8)
		{
			rgb[0] = ((val&0xff0000)>>12)/255.0;
			rgb[1] = ((val&0x00ff00)>> 8)/255.0;
			rgb[2] = ((val&0x0000ff)>> 0)/255.0;
		}
		else
			rgb[0] = rgb[1] = rgb[2] = 1;

		return ;
	}

	t = strstr(rgbstring, " ");

	if (!t) // palette index
	{
		qbyte *pal;
		int i = atoi(rgbstring);
		i = bound(0, i, 255);

		pal = host_basepal;

		pal += (i * 3);
		// convert r8g8b8 to rgb floats
		rgb[0] = (float)(pal[0]);
		rgb[1] = (float)(pal[1]);
		rgb[2] = (float)(pal[2]);

		VectorScale(rgb, 1/255.0, rgb);
	}
	else // use RGB coloring (input is scaled from 0-rgbinputscale)
	{
		t++;
		rgb[0] = atof(rgbstring);
		rgb[1] = atof(t);
		t = strstr(t, " "); // find last value
		if (t)
			rgb[2] = atof(t+1);
		else
			rgb[2] = 0.0;

		rgbinputscale = 1/rgbinputscale;
		VectorScale(rgb, rgbinputscale, rgb);
	} // i contains the crosshair color
}

static qboolean SCR_CenterPrintPop(int pnum)
{
	cprint_t *p = scr_centerprint[pnum];
	if (!p)
		return false;
	scr_centerprint[pnum] = p->queue;
	Z_Free(p->string);
	Z_Free(p);

	//timers start NOW (not when its first queued, obviously)
	p = scr_centerprint[pnum];
	if (p)
		p->time_start = cl.time;
	return true;
}

/*
==============
SCR_CenterPrint

Called for important messages that should stay in the center of the screen
for a few moments
==============
*/
void SCR_CenterPrint (int pnum, const char *str, qboolean skipgamecode)
{
	unsigned int pfl = 0;
	size_t i;
	cprint_t *p, **l;
	qboolean doqueue = false;
	qboolean donotify = ((scr_logcenterprint.ival && !cl.deathmatch) || scr_logcenterprint.ival == 2);
#ifdef HAVE_LEGACY
	if (scr_usekfont.ival)
		pfl |= PFS_FORCEUTF8;
#endif
	if (!str)
	{
		if (cl.intermissionmode == IM_NONE)
			while(SCR_CenterPrintPop(pnum))
				;
		return;
	}
	if (!skipgamecode)
	{
#ifdef CSQC_DAT
		if (CSQC_CenterPrint(pnum, str))	//csqc nabbed it.
			return;
#endif
	}

	if (Cmd_AliasExist("f_centerprint", RESTRICT_LOCAL))
	{
		cvar_t *var;
		var = Cvar_FindVar ("scr_centerprinttext");
		if (!var)
			var = Cvar_Get("scr_centerprinttext", "", 0, "Script Notifications");
		if (var)
		{
			Cvar_Set(var, str);
			Cbuf_AddText("f_centerprint\n", RESTRICT_LOCAL);
		}
	}

	p = Z_Malloc(sizeof(*p));
	p->flags = 0;
	p->titleimage[0] = 0;
	p->cursorchar = NULL;
	p->time_off = scr_centertime.value;

	if (*str != '/')
	{
		if (cl.intermissionmode != IM_NONE)
		{
			p->flags |= CPRINT_TYPEWRITER | CPRINT_PERSIST | CPRINT_TALIGN;
			if (cl.intermissionmode != IM_NQCUTSCENE)
				Q_strncpyz(p->titleimage, "gfx/finale.lmp", sizeof(p->titleimage));
		}
	}

	while (*str == '/')
	{
		if (str[1] == '.')
		{
/* /. means text actually starts after, no more flags */
			str+=2;
			break;
		}
		else if (str[1] == 'P')
		{
			p->flags |= CPRINT_PERSIST | CPRINT_BACKGROUND;
			p->flags &= ~CPRINT_TALIGN;
		}
		else if (str[1] == 'C')
			p->flags |= CPRINT_CURSOR;	//this can be a little jarring if there's no links, so this forces consistent behaviour.
		else if (str[1] == 'W')	//wait between each char
			p->flags ^= CPRINT_TYPEWRITER;
		else if (str[1] == 'Q')	//queue
			doqueue = true;
		else if (str[1] == 'D')	//explicit delay
		{
			p->time_off = strtod(str+2, (char**)&str);
			continue;
		}
		else if (str[1] == 'S')	//Stay
			p->flags ^= CPRINT_PERSIST;
		else if (str[1] == 'M')	//'Mask' the background so that its readable.
			p->flags ^= CPRINT_BACKGROUND;
		else if (str[1] == 'N')	//'Notify' spam it to the console even in deathmatch.
		{
			donotify = strtol(str+2, (char**)&str, 10);
			continue;
		}
		else if (str[1] == 'O')	//Obituaries are shown at the bottom, ish.
			p->flags ^= CPRINT_OBITUARTY;
		else if (str[1] == 'B')
			p->flags ^= CPRINT_BALIGN;	//Note: you probably want to add some blank lines...
		else if (str[1] == 'T')
			p->flags ^= CPRINT_TALIGN;
		else if (str[1] == 'L')
			p->flags ^= CPRINT_LALIGN;
		else if (str[1] == 'R')
			p->flags ^= CPRINT_RALIGN;
		else if (str[1] == 'F')
		{	//'F' is reserved for special handling via svc_finale
			if (cl.intermissionmode == IM_NONE)
			{
				TP_ExecTrigger ("f_mapend", false);
				if (cl.playerview[pnum].spectator || cls.demoplayback)
					TP_ExecTrigger ("f_specmapend", true);
				else
					TP_ExecTrigger ("f_playmapend", true);
				cl.completed_time = cl.time;
			}
			str+=2;
			switch(*str++)
			{
			case 'R':	//remove intermission
				cl.intermissionmode = IM_NONE;
				break;
			case 'I':	//standard intermission
			case 'S':	//score board / map stats
				cl.intermissionmode = IM_NQSCORES;
				break;
			case 'F':	//finale
				cl.intermissionmode = IM_NQFINALE;
				break;
			case 'f':	//finale, but with the view offset properly
				cl.intermissionmode = IM_H2FINALE;
				break;
			case 0:
				str--;
				break;
			default:
				break;	//no idea. suck it up for compat.
			}
			continue;
		}
		else if (str[1] == 'I')
		{
			const char *e;
			int l;
			str+=2;
			e = strchr(str, ':');
			if (!e)
				e = strchr(str, ' ');	//probably an error
			if (!e)
				e = str + strlen(str)-1;	//error
			l = e - str;
			if (l >= sizeof(p->titleimage))
				l = sizeof(p->titleimage)-1;
			strncpy(p->titleimage, str, l);
			p->titleimage[l] = 0;
			str = e+1;
			continue;
		}
		else
			break;
		str += 2;
	}

	if (!doqueue)	//kill all prior ones if we're not queueing
		while (SCR_CenterPrintPop(pnum))
			;
	//add it to the end
	for (l = &scr_centerprint[pnum]; *l; l = &(*l)->queue)
		;
	*l = p;


	if (donotify && !(p->flags & CPRINT_PERSIST))
	{
		//don't spam too much.
		if (*str && strncmp(cl.lastcenterprint, str, sizeof(cl.lastcenterprint)-1))
		{
			Q_strncpyz(cl.lastcenterprint, str, sizeof(cl.lastcenterprint));
			Con_CenterPrint(str);
		}
	}

	for (;;)
	{
		p->charcount = COM_ParseFunString(CON_WHITEMASK, str, p->string, p->stringbytes, pfl) - p->string;

		if ((p->charcount+1)*sizeof(*p->string) < p->stringbytes)
			break;
		else
		{
			p->stringbytes=p->stringbytes*2+sizeof(*p->string);
			Z_Free(p->string);
			p->string = Z_Malloc(p->stringbytes);
		}
	}

	if (!(p->flags & CPRINT_CURSOR))
	{	//autodetect links
		for (i = 0; i < p->charcount; i++)
		{
			if (p->string[i] == CON_LINKSTART)
			{
				p->flags |= CPRINT_CURSOR;
				break;
			}
		}
	}

	p->time_start = cl.time;
	VRUI_SnapAngle();
}

void VARGS Stats_Message(char *msg, ...)
{
	va_list		argptr;
	char str[2048];
	cprint_t *p = scr_centerprint[0];
	if (!scr_showobituaries.ival)
		return;
	if (p)	//don't show if one is shown... FIXME: queue these too (some depth cutoff?)
		return;

	va_start (argptr, msg);
	vsnprintf (str,sizeof(str)-1, msg, argptr);
	va_end (argptr);

	p = scr_centerprint[0] = Z_Malloc(sizeof(*p));
	p->flags = CPRINT_OBITUARTY;
	p->titleimage[0] = 0;

	for (;;)
	{
		p->charcount = COM_ParseFunString(CON_WHITEMASK, str, p->string, p->stringbytes, false) - p->string;

		if ((p->charcount+1)*sizeof(*p->string) < p->stringbytes)
			break;
		else
		{
			p->stringbytes=p->stringbytes*2+sizeof(*p->string);
			Z_Free(p->string);
			p->string = Z_Malloc(p->stringbytes);
		}
	}

	p->time_off = scr_centertime.value;
	p->time_start = cl.time;
}

static char *SCR_CopyCenterPrint(cprint_t *p)	//reads the link under the mouse cursor. non-links are ignored.
{
	size_t maxlen, outlen;
	char *result;

	conchar_t *start = p->cursorchar, *end;

	if (!start)	//cursor isn't over anything.
		return NULL;

	//scan backwards to find any link enclosure
	for(end = start-1; end >= p->string; end--)
	{
		if (*end == CON_LINKSTART)
		{
			//found one
			start = end;
			break;
		}
		if (*end == CON_LINKEND)
		{
			//some other link ended here. don't use its start.
			break;
		}
	}
	//scan forwards to find the end of the selected link
	if (start < p->string+p->charcount && *start == CON_LINKSTART)
	{
		for(end = start; end < p->string + p->charcount; end++)
		{
			if (*end == CON_LINKEND)
			{
				end++;
				break;
			}
		}
	}
	else// if (onlyiflink)
		return NULL;

	maxlen = 1024*1024;
	result = Z_Malloc(maxlen+1);

	outlen = COM_DeFunString(start, end, result, maxlen, false, false) - result;

	result[outlen++] = 0;
	return result;
}

#define MAX_CPRINT_LINES 512
int SCR_DrawCenterString (vrect_t *playerrect, cprint_t *p, struct font_s *font)
{
	int				l;
	int				y, x;
	int				left;
	int				right;
	int				top;
	int				bottom;
	int				remaining;
	shader_t		*pic;
	int				ch;
	int mousex,mousey, mousemoved;

	conchar_t *line_start[MAX_CPRINT_LINES];
	conchar_t *line_end[MAX_CPRINT_LINES];
	int linecount;

	vrect_t rect = *playerrect;

// the finale prints the characters one at a time
	if (p->flags & CPRINT_TYPEWRITER)
		remaining = scr_printspeed.value * (cl.time - p->time_start);
	else
		remaining = 9999;

	p->erase_center = 0;

	if (*p->titleimage)
		pic = R2D_SafeCachePic (p->titleimage);
	else
		pic = NULL;

	if (p->flags & CPRINT_BACKGROUND)
	{	//hexen2 style plaque.
		int w = 320;
		if (rect.width > w)
		{
			rect.x = (rect.x + rect.width/2) - (w / 2);
			rect.width = w;
		}

		if (rect.width < 32)
			return 0;
		rect.x += 16;
		rect.width -= 32;
	}

	y = rect.y;

	if (pic)
	{
		//the pic is just a header
		if (!(p->flags & CPRINT_BACKGROUND))
		{
			int w, h;
			R_GetShaderSizes(pic, &w, &h, false);
			w *= 24.0/h;
			h = 24;
			y+= 16;
			R2D_ScalePic (rect.x + (rect.width-w)/2, y, w, h, pic);
			y+= h;
			y+= 8;
		}
	}

	Font_BeginString(font, mousecursor_x, mousecursor_y, &mousex, &mousey);
	Font_BeginString(font, rect.x, y, &left, &top);
	Font_BeginString(font, rect.x+rect.width, rect.y+rect.height, &right, &bottom);
	linecount = Font_LineBreaks(p->string, p->string + p->charcount, (p->flags & CPRINT_NOWRAP)?0x7fffffff:(right - left), MAX_CPRINT_LINES, line_start, line_end);

	mousemoved = mousex != p->oldmousex || mousey != p->oldmousey;
	p->oldmousex = mousex;
	p->oldmousey = mousey;

	ch = Font_CharHeight();

	if (p->flags & CPRINT_TALIGN)
		y = top;
	else if (p->flags & CPRINT_BALIGN)
		y = bottom - ch*linecount;
	else if (p->flags & CPRINT_OBITUARTY)
		//'obituary' messages appear at the bottom of the screen
		y = (bottom-top - ch*linecount) * 0.65 + top;
	else
	{
		if (linecount <= 5)
		{
			//small messages appear above and away from the crosshair
			y = (bottom-top - ch*linecount) * 0.35 + top;
		}
		else
		{
			//longer messages are fully centered
			y = (bottom-top - ch*linecount) * 0.5 + top;
		}
	}

	if (p->flags & CPRINT_BACKGROUND)
	{	//hexen2 style plaque.
		Font_EndString(font);

		if (*p->titleimage && pic)
		{
			int w, h;
			R_GetShaderSizes(pic, &w, &h, false);
			R2D_Letterbox(playerrect->x, playerrect->y, playerrect->width, playerrect->height, pic, w, h);
		}
		else
			Draw_ApproxTextBox(rect.x, (y * (float)vid.height) / (float)vid.pixelheight, rect.width, linecount*Font_CharVHeight(font));

		Font_BeginString(font, rect.x, y, &left, &top);
	}

	x = (left+right)/2;
	for (l = 0; l < linecount; l++, y += ch)
	{
		if (y >= bottom)
			break;
		if (p->flags & CPRINT_RALIGN)
			x = right - Font_LineWidth(line_start[l], line_end[l]);
		else if (p->flags & CPRINT_LALIGN)
			x = left;
		else
			x = left + (right - left - Font_LineWidth(line_start[l], line_end[l]))/2;

		if (mousemoved && mousey >= y && mousey < y+ch)
		{
			conchar_t *linkstart;
			linkstart = Font_CharAt(mousex - x, line_start[l], line_end[l]);

			//scan backwards to find any link enclosure
			if (linkstart)
				for(linkstart = linkstart-1; linkstart >= line_start[l]; linkstart--)
				{
					if (*linkstart == CON_LINKSTART)
					{
						//found one
						p->cursorchar = linkstart;
						break;
					}
					if (*linkstart == CON_LINKEND)
					{
						//some other link ended here. don't use its start.
						break;
					}
				}
		}

		remaining -= line_end[l]-line_start[l];
		if (remaining <= 0)
		{
			if (p->time_off && (p->flags&(CPRINT_TYPEWRITER|CPRINT_PERSIST))==CPRINT_TYPEWRITER)
				p->time_off = scr_centertime.value;	//reset the timeout while we're still truncating it.
			line_end[l] += remaining;
			if (line_end[l] <= line_start[l])
				line_end[l] = line_start[l];
		}

		if (p->cursorchar && p->cursorchar >= line_start[l] && p->cursorchar < line_end[l] && *p->cursorchar == CON_LINKSTART)
		{
			conchar_t *linkend;
			int s, e;
			for (linkend = p->cursorchar; linkend < line_end[l]; linkend++)
				if (*linkend == CON_LINKEND)
					break;

			s = x+Font_LineWidth(line_start[l], p->cursorchar);
			e = x+Font_LineWidth(line_start[l], linkend);

			//draw a 2-pixel underscore (behind the text).
			R2D_ImageColours(SRGBA(0.3,0.3,0.3, 1));	//mouseover.
			R2D_FillBlock((s*vid.width)/(float)vid.rotpixelwidth, ((y+Font_CharHeight()-2)*vid.height)/(float)vid.rotpixelheight, ((e - s)*vid.width)/(float)vid.rotpixelwidth, (2*vid.height)/(float)vid.rotpixelheight);
			R2D_Flush();
			R2D_ImageColours(SRGBA(1, 1, 1, 1));
		}

		Font_LineDraw(x, y, line_start[l], line_end[l]);

		if (remaining <= 0 || l == linecount-1)
		{
			if ((p->flags&(CPRINT_TYPEWRITER|CPRINT_PERSIST))==CPRINT_TYPEWRITER && l < linecount && ((int)(realtime*4)&1))
			{
				x = x+Font_LineWidth(line_start[l], line_end[l]);
				Font_DrawChar(x, y, CON_WHITEMASK, 0xe00b);
			}
			break;
		}
	}

	Font_EndString(font);

	return linecount;
}

static void Key_CenterPrintActivate(int pnum)
{
	char *link;
	cprint_t *p = scr_centerprint[pnum];
	link = SCR_CopyCenterPrint(p);
	if (link)
	{

		if (link[0] == '^' && link[1] == '[')
		{
			//looks like it might be a link!
			char *end = NULL;
			char *info;
			for (info = link + 2; *info; )
			{
				if (info[0] == '^' && info[1] == ']')
					break; //end of tag, with no actual info, apparently
				if (*info == '\\')
					break;
				else if (info[0] == '^' && info[1] == '^')
					info+=2;
				else
					info++;
			}
			for(end = info; *end; )
			{
				if (end[0] == '^' && end[1] == ']')
				{
					//okay, its a valid link that they clicked
					*end = 0;

#ifdef PLUGINS
					if (!Plug_ConsoleLink(link+2, info, ""))
#endif
#ifdef CSQC_DAT
					if (!CSQC_ConsoleLink(link+2, info))
#endif
						Key_DefaultLinkClicked(NULL, link+2, info);

					break;
				}
				if (end[0] == '^' && end[1] == '^')
					end+=2;
				else
					end++;
			}
		}
	}
}
qboolean Key_Centerprint(int key, int unicode, unsigned int devid)
{
	int pnum;
	cprint_t *p;

	if (key == K_MOUSE1)
	{
		//figure out which player has the cursor
		for (pnum = 0; pnum < cl.splitclients; pnum++)
		{
			if (cl.playerview[pnum].gamerectknown == cls.framecount)
				Key_CenterPrintActivate(pnum);
		}
		return true;	//handled
	}
	else if (key == K_MOUSE2)
	{
		for (pnum = 0; pnum < cl.splitclients; pnum++)
		{
			p = scr_centerprint[pnum];
			if (p)
				p->flags &= ~CPRINT_CURSOR;
		}
		return true;
	}
	else if ((key == K_ENTER ||
			  key == K_KP_ENTER ||
			  key == K_GP_DIAMOND_RIGHT) && devid < countof(scr_centerprint))
	{
		p = scr_centerprint[devid];
		if (p && p->cursorchar)
			Key_CenterPrintActivate(devid);
		return true;
	}
	else if ((key == K_UPARROW ||
			  key == K_LEFTARROW ||
			  key == K_KP_UPARROW ||
			  key == K_KP_LEFTARROW ||
			  key == K_GP_DPAD_UP ||
			  key == K_GP_DPAD_LEFT) && devid < countof(scr_centerprint))
	{
		p = scr_centerprint[devid];
		if (p)
		{
			if (!p->cursorchar)
				p->cursorchar = p->string + p->charcount;
			while (--p->cursorchar >= p->string)
			{
				if (*p->cursorchar == CON_LINKSTART)
					return true;	//found one
			}
			p->cursorchar = NULL;
		}
		return true;
	}
	else if ((key == K_DOWNARROW ||
			  key == K_RIGHTARROW ||
			  key == K_KP_DOWNARROW ||
			  key == K_KP_RIGHTARROW ||
			  key == K_GP_DPAD_DOWN ||
			  key == K_GP_DPAD_RIGHT) && devid < countof(scr_centerprint))
	{
		p = scr_centerprint[devid];
		if (p)
		{
			if (!p->cursorchar)
				p->cursorchar = p->string-1;
			while (++p->cursorchar < p->string + p->charcount)
			{
				if (*p->cursorchar == CON_LINKSTART)
					return true;	//found one
			}
			p->cursorchar = NULL;	//hit the end
		}
		return true;
	}

	return false;
}

void SCR_CheckDrawCenterString (void)
{
	int pnum;
	cprint_t *p;

	Key_Dest_Remove(kdm_centerprint);

	for (pnum = 0; pnum < cl.splitclients; pnum++)
	{
#ifdef QUAKESTATS
		if (IN_DrawWeaponWheel(pnum))
		{	//we won't draw the cprint, but it also won't fade while the wwheel is shown.
			continue;
		}
#endif

		p = scr_centerprint[pnum];
		if (!p)
			continue;
		if (p->time_off <= 0 && !(p->flags & CPRINT_PERSIST))	//'/P' prefix doesn't time out
		{	//this one is done. move to the next.
			if (SCR_CenterPrintPop(pnum))
				pnum++;
			continue;
		}

		p->time_off -= host_frametime;

		if (Key_Dest_Has(~(kdm_game|kdm_centerprint)))	//don't let progs guis/centerprints interfere with the game menu
			continue;					//should probably allow the console with a scissor region or something.

#ifdef QUAKEHUD
		if (cl.playerview[pnum].sb_showscores || cl.playerview[pnum].sb_showteamscores)	//this was annoying
			continue;
#endif

		if (cl.playerview[pnum].gamerectknown == cls.framecount)
		{
			SCR_DrawCenterString(&cl.playerview[pnum].gamerect, p, font_default);
			if (p->flags & CPRINT_CURSOR)
				Key_Dest_Add(kdm_centerprint);
		}
	}
}

int R_DrawTextField(int x, int y, int w, int h, const char *text, unsigned int defaultmask, unsigned int fieldflags, struct font_s *font, vec2_t fontscale)
{
	cprint_t p;
	vrect_t r;
	conchar_t buffer[16384];	//FIXME: make dynamic.
	int lines;

	p.cursorchar = NULL;
	p.string = buffer;
	p.stringbytes = sizeof(buffer);
	r.x = x;
	r.y = y;
	r.width = w;
	r.height = h;

	p.flags = fieldflags;
	p.charcount = COM_ParseFunString(defaultmask, text, p.string, p.stringbytes, false) - p.string;
	p.time_off = scr_centertime.value;
	p.time_start = cl.time;
	*p.titleimage = 0;


	lines = SCR_DrawCenterString(&r, &p, font);

//	SCR_CopyCenterPrint(&p);
	return lines;
}

qboolean SCR_HardwareCursorIsActive(void)
{
	if (Key_MouseShouldBeFree())
		return !!scr_curcursor;
	return false;
}
void SCR_DrawCursor(void)
{
	extern cvar_t cl_cursor, cl_cursorbiasx, cl_cursorbiasy, cl_cursorscale, cl_prydoncursor;
	mpic_t *p;
	char *newc;
	int prydoncursornum = 0;
	extern qboolean cursor_active;
	struct key_cursor_s *kcurs;
	void *oldcurs = NULL;

	if (cursor_active && cl_prydoncursor.ival > 0)
		prydoncursornum = cl_prydoncursor.ival;
	else if (!Key_MouseShouldBeFree())
		return;

	if ((key_dest_mask & key_dest_absolutemouse & kdm_prompt))
	{
		if (promptmenu&&promptmenu->cursor)
			kcurs = promptmenu->cursor;
		else
			kcurs = &key_customcursor[kc_console];
	}
	//choose the cursor based upon the module that has primary focus
	else if (key_dest_mask & key_dest_absolutemouse & (kdm_console|kdm_cwindows))
		kcurs = &key_customcursor[kc_console];
	else if ((key_dest_mask & key_dest_absolutemouse & kdm_menu))
	{
		if (topmenu&&topmenu->cursor)
			kcurs = topmenu->cursor;
		else
			kcurs = &key_customcursor[kc_console];
	}
	else// if (key_dest_mask & key_dest_absolutemouse)
		kcurs = &key_customcursor[prydoncursornum?kc_console:kc_game];

	if (kcurs == &key_customcursor[kc_console])
	{
		if (!*cl_cursor.string || prydoncursornum>1)
			newc = va("gfx/prydoncursor%03i.lmp", prydoncursornum);
		else
			newc = cl_cursor.string;
		if (strcmp(kcurs->name, newc) || kcurs->hotspot[0] != cl_cursorbiasx.value || kcurs->hotspot[1] != cl_cursorbiasy.value || kcurs->scale != cl_cursorscale.value)
		{
			kcurs->dirty = true;
			Q_strncpyz(kcurs->name, newc, sizeof(kcurs->name));
			kcurs->hotspot[0] = cl_cursorbiasx.value;
			kcurs->hotspot[1] = cl_cursorbiasy.value;
			kcurs->scale = cl_cursorscale.value;
		}
	}

	if (vrui.enabled && kcurs->handle)
		kcurs->dirty = true;

	if (kcurs->dirty)
	{
		if (kcurs->scale <= 0 || !*kcurs->name)
		{
			kcurs->hotspot[0] = cl_cursorbiasx.value;
			kcurs->hotspot[1] = cl_cursorbiasy.value;
			kcurs->scale = cl_cursorscale.value;
		}

		kcurs->dirty = false;
		oldcurs = kcurs->handle;
		if (rf->VID_CreateCursor && !vrui.enabled && strcmp(kcurs->name, "none"))
		{
			image_t dummytex;
			flocation_t loc;
			char bestname[MAX_QPATH];
			unsigned int bestflags;
			qbyte *rgbadata;
			uploadfmt_t format;
			void *filedata = NULL;
			int filelen = 0, width, height;

			kcurs->handle = NULL;

			memset(&dummytex, 0, sizeof(dummytex));
			dummytex.flags = IF_NOREPLACE;	//no dds files
			*bestname = 0;

			//first try the named image, if possible
			if (!filedata && *kcurs->name)
			{
				dummytex.ident = kcurs->name;
				if (Image_LocateHighResTexture(&dummytex, &loc, bestname, sizeof(bestname), &bestflags))
					filelen = FS_LoadFile(bestname, &filedata);
			}
			if (!filedata)
			{
				dummytex.ident = "gfx/cursor.lmp";
				if (Image_LocateHighResTexture(&dummytex, &loc, bestname, sizeof(bestname), &bestflags))
					filelen = FS_LoadFile(bestname, &filedata);
			}
			if (!filedata)
			{
				static qbyte lamecursor[] =
				{
#define W 0x8f,0x8f,0x8f,0xff,
#define B 0x00,0x00,0x00,0xff,
#define T 0x00,0x00,0x00,0x00,
					W T T T T T T T
					W W T T T T T T
					W B W T T T T T
					W B B W T T T T

					W B B B W T T T
					W B B B B W T T
					W B B B B B W T
					W B B B B B B W

					W B B B B W W W
					W B W B B W T T
					W W T W B B W T
					W T T W B B W T

					T T T T W B B W
					T T T T W B B W
					T T T T T W W T
#undef W
#undef B
				};
				kcurs->handle = rf->VID_CreateCursor(lamecursor, 8, 15, PTI_LLLA8, 0, 0, 1);	//try the fallback
			}
			else if (!filedata)
				FS_FreeFile(filedata);	//format not okay, just free it.
			else
			{	//raw file loaded.
				rgbadata = ReadRawImageFile(filedata, filelen, &width, &height, &format, true, bestname);
				FS_FreeFile(filedata);
				if (rgbadata)
				{	//image loaded properly, yay
					if ((format==PTI_BGRX8 || format==PTI_RGBX8 || format==PTI_LLLX8) && !strchr(bestname, ':'))
						Image_ReadExternalAlpha(rgbadata, width, height, bestname, &format);

					kcurs->handle = rf->VID_CreateCursor(rgbadata, width, height, format, kcurs->hotspot[0], kcurs->hotspot[1], kcurs->scale);	//try the fallback
					BZ_Free(rgbadata);
				}
			}
		}
		else
			kcurs->handle = NULL;
	}

	if (scr_curcursor != kcurs->handle)
	{
		scr_curcursor = kcurs->handle;
		rf->VID_SetCursor(scr_curcursor);
	}
	if (oldcurs)
		rf->VID_DestroyCursor(oldcurs);

	if (scr_curcursor)
		return;
	//system doesn't support a hardware cursor, so try to draw a software one.

	if (!strcmp(kcurs->name, "none"))
		return;

	p = R2D_SafeCachePic(kcurs->name);
	if (!p || !R_GetShaderSizes(p, NULL, NULL, false))
		p = R2D_SafeCachePic("gfx/cursor.lmp");
	if (p && R_GetShaderSizes(p, NULL, NULL, false))
	{
		R2D_ImageColours(1, 1, 1, 1);
		R2D_Image(mousecursor_x-kcurs->hotspot[0], mousecursor_y-kcurs->hotspot[1], p->width*cl_cursorscale.value, p->height*cl_cursorscale.value, 0, 0, 1, 1, p);
	}
	else
	{
		float x, y;
		Font_BeginScaledString(font_default, mousecursor_x, mousecursor_y, 8, 8, &x, &y);
		x -= Font_CharScaleWidth(CON_WHITEMASK, '+' | 0xe000)/2;
		y -= Font_CharHeight()/2;
		Font_DrawScaleChar(x, y, CON_WHITEMASK, '+' | 0xe000);
		Font_EndString(font_default);
	}
}
static void SCR_DrawSimMTouchCursor(void)
{
	int i;
	float x, y;
	for (i = 0; i < 8; i++)
	{
		if (multicursor_active[i])
		{
			Font_BeginScaledString(font_default, multicursor_x[i], multicursor_y[i], 8, 8, &x, &y);
			x -= Font_CharScaleWidth(CON_WHITEMASK, '+' | 0xe000)/2;
			y -= Font_CharHeight()/2;
			Font_DrawScaleChar(x, y, CON_WHITEMASK, '+' | 0xe000);
			Font_EndString(font_default);
		}
	}
}

////////////////////////////////////////////////////////////////
//TEI_SHOWLMP2 (not 3)
//
typedef struct showpic_s {
	struct showpic_s *next;
	qbyte zone;
	qboolean persist;
	float fadedelay;
	short x, y, w, h;
	char *name;
	char *picname;
	char *tcommand;
} showpic_t;
showpic_t *showpics;
double showpics_touchtime;

static void SP_RecalcXY ( float *xx, float *yy, int origin )
{
	int midx, midy;
	float x,y;

	x = xx[0];
	y = yy[0];

	midy = vid.height * 0.5;// >>1
	midx = vid.width * 0.5;// >>1

	switch (origin)
	{
		//tei's original encoding
		case SL_ORG_NW:
			break;
		case SL_ORG_NE:
			x = vid.width - x;//Inv
			break;
		case SL_ORG_SW:
			y = vid.height - y;//Inv
			break;
		case SL_ORG_SE:
			y = vid.height - y;//inv
			x = vid.width - x;//Inv
			break;
		case SL_ORG_CC:
			y = midy + (y - 8000);//NegCoded
			x = midx + (x - 8000);//NegCoded
			break;
		case SL_ORG_CN:
			x = midx + (x - 8000);//NegCoded
			break;
		case SL_ORG_CS:
			x = midx + (x - 8000);//NegCoded
			y = vid.height - y;//Inverse
			break;
		case SL_ORG_CW:
			y = midy + (y - 8000);//NegCoded
			break;
		case SL_ORG_CE:
			y = midy + (y - 8000);//NegCoded
			x = vid.height - x; //Inverse
			break;

		//spike's attempt to provide sane origins that are a little more predictable.
		case SL_ORG_TL:
			break;
		case SL_ORG_TR:
			x += vid.width;
			break;
		case SL_ORG_BL:
			y += vid.height;
			break;
		case SL_ORG_BR:
			x += vid.width;
			y += vid.height;
			break;
		case SL_ORG_MM:
			x += midx;
			y += midy;
			break;
		case SL_ORG_TM:
			x += midx;
			break;
		case SL_ORG_BM:
			x += midx;
			y += vid.height;
			break;
		case SL_ORG_ML:
			y += midy;
			break;
		case SL_ORG_MR:
			x += vid.height;
			y += midy;
			break;

		default:
			break;
	}

	xx[0] = x;
	yy[0] = y;
}
void SCR_ShowPics_Draw(void)
{
	downloadlist_t *failed;
	float x, y, a = 1;
	showpic_t *sp;
	mpic_t *p;
	for (sp = showpics; sp; sp = sp->next)
	{
		x = sp->x;
		y = sp->y;
		SP_RecalcXY(&x, &y, sp->zone);
		if (!*sp->picname)
			continue;

		for (failed = cl.faileddownloads; failed; failed = failed->next)
		{	//don't try displaying ones that we know to have failed.
			if (!strcmp(failed->rname, sp->picname))
				break;
		}
		if (failed)
			continue;

		if (sp->fadedelay && showpics_touchtime+sp->fadedelay < realtime)
		{
			a = 1+(showpics_touchtime+sp->fadedelay-realtime);
			if (a > 1)
				a = 1;	//shouldn't really happen, w/e.
			else if (a <= 0)
				continue;
			R2D_ImageColours(1,1,1,a);
		}
		else if (a != 1)
			R2D_ImageColours(1,1,1,a=1);

		p = R2D_SafeCachePic(sp->picname);
		if (!p || !R_GetShaderSizes(p, NULL, NULL, false))
		{
			if (sp->h > 8)
				Draw_FunStringWidth(x-2, y + (sp->h-8)/2, sp->name, sp->w+4, 2, false);	//slightly wider, to try to somewhat deal with 16:10 resolutions...
		}
		else
			R2D_ScalePic(x, y, sp->w?sp->w:p->width, sp->h?sp->h:p->height, p);
	}

	if (a != 1)
		R2D_ImageColours(1,1,1,1);
}
const char *SCR_ShowPics_ClickCommand(float cx, float cy, qboolean istouch)
{
	downloadlist_t *failed;
	float x, y, w, h;
	showpic_t *sp;
	mpic_t *p;
	qboolean tryload = istouch && !showpics_touchtime;
	float bestdist = istouch?16:1;
	const char *best = NULL;
	showpics_touchtime = realtime;
	for (sp = showpics; sp; sp = sp->next)
	{
		if (!sp->tcommand || !*sp->tcommand)
			continue;

		tryload = false;

		x = sp->x;
		y = sp->y;
		w = sp->w;
		h = sp->h;
		SP_RecalcXY(&x, &y, sp->zone);

		if (!w || !h)
		{
			if (!*sp->picname)
				continue;
			for (failed = cl.faileddownloads; failed; failed = failed->next)
			{	//don't try displaying ones that we know to have failed.
				if (!strcmp(failed->rname, sp->picname))
					break;
			}
			if (failed)
				continue;
			p = R2D_SafeCachePic(sp->picname);
			if (!p)
				continue;
			w = w?w:sp->w;
			h = h?h:sp->h;
		}

		x = bound(x, cx, x+w)-cx;
		y = bound(y, cy, y+h)-cy;
		x = max(fabs(x),fabs(y));
		if (bestdist > x)
		{	//looks like this one is closer to the cursor
			bestdist = x;
			best = sp->tcommand;
			if (bestdist <= 0)	//use the first that's inside.
				break;
		}
	}

	if (tryload)
		Cbuf_AddText("exec touch.cfg\n", RESTRICT_LOCAL);
	return best;
}

//all=false clears only server pics, not ones from configs.
void SCR_ShowPic_ClearAll(qboolean persistflag)
{
	showpic_t **link, *sp;
	int pnum;

	for (pnum = 0; pnum < MAX_SPLITS; pnum++)
	{
		while (SCR_CenterPrintPop(pnum))
			;
	}

	if (!persistflag)
		showpics_touchtime = 0;	//map change. gamedir may have changed too.
	for (link = &showpics; (sp=*link); )
	{
		if (sp->persist != persistflag)
		{
			link = &sp->next;
			continue;
		}
		
		*link = sp->next;

		Z_Free(sp->name);
		Z_Free(sp->picname);
		Z_Free(sp->tcommand);
		Z_Free(sp);
	}
}

showpic_t *SCR_ShowPic_Find(char *name)
{
	showpic_t *sp, *last;
	for (sp = showpics; sp; sp = sp->next)
	{
		if (!strcmp(sp->name, name))
			return sp;
	}

	if (showpics)
	{
		for (last = showpics; last->next; last = last->next)
			;
	}
	else
		last = NULL;
	sp = Z_Malloc(sizeof(showpic_t));
	if (last)
	{
		last->next = sp;
		sp->next = NULL;
	}
	else
	{
		sp->next = showpics;
		showpics = sp;
	}
	sp->name = Z_Malloc(strlen(name)+1);
	strcpy(sp->name, name);
	sp->picname = Z_Malloc(1);
	sp->x = 0;
	sp->y = 0;
	sp->zone = 0;

	return sp;
}

void SCR_ShowPic_Create(void)
{
	int zone = MSG_ReadByte();
	showpic_t *sp;
	char *s;

	sp = SCR_ShowPic_Find(MSG_ReadString());

	s = MSG_ReadString();

	Z_Free(sp->picname);
	sp->picname = Z_Malloc(strlen(s)+1);
	strcpy(sp->picname, s);
	sp->zone = zone;
	sp->x = MSG_ReadShort();
	sp->y = MSG_ReadShort();

	CL_CheckOrEnqueDownloadFile(sp->picname, sp->picname, 0);
}

void SCR_ShowPic_Hide(void)
{
	showpic_t *sp, *prev;

	sp = SCR_ShowPic_Find(MSG_ReadString());

	if (sp == showpics)
		showpics = sp->next;
	else
	{
		for (prev = showpics; prev->next != sp; prev = prev->next)
			;
		prev->next = sp->next;
	}

	Z_Free(sp->name);
	Z_Free(sp->picname);
	Z_Free(sp);
}

void SCR_ShowPic_Move(void)
{
	int zone = MSG_ReadByte();
	showpic_t *sp;

	sp = SCR_ShowPic_Find(MSG_ReadString());

	sp->zone = zone;
	sp->x = MSG_ReadShort();
	sp->y = MSG_ReadShort();
}

void SCR_ShowPic_Update(void)
{
	showpic_t *sp;
	char *s;

	sp = SCR_ShowPic_Find(MSG_ReadString());

	s = MSG_ReadString();

	Z_Free(sp->picname);
	sp->picname = Z_Malloc(strlen(s)+1);
	strcpy(sp->picname, s);

	CL_CheckOrEnqueDownloadFile(sp->picname, sp->picname, 0);
}

static int SCR_ShowPic_MapZone(char *zone)
{
	//sane coding scheme
	if (!Q_strcasecmp(zone, "tr"))
		return SL_ORG_TR;
	if (!Q_strcasecmp(zone, "tl"))
		return SL_ORG_TL;
	if (!Q_strcasecmp(zone, "br"))
		return SL_ORG_BR;
	if (!Q_strcasecmp(zone, "bl"))
		return SL_ORG_BL;
	if (!Q_strcasecmp(zone, "mm"))
		return SL_ORG_MM;
	if (!Q_strcasecmp(zone, "tm"))
		return SL_ORG_TM;
	if (!Q_strcasecmp(zone, "bm"))
		return SL_ORG_BM;
	if (!Q_strcasecmp(zone, "mr"))
		return SL_ORG_MR;
	if (!Q_strcasecmp(zone, "ml"))
		return SL_ORG_MR;

	//compasy directions (but uses tei's coding scheme...)
	if (!Q_strcasecmp(zone, "nw"))
		return SL_ORG_NW;
	if (!Q_strcasecmp(zone, "ne"))
		return SL_ORG_NE;
	if (!Q_strcasecmp(zone, "sw"))
		return SL_ORG_SW;
	if (!Q_strcasecmp(zone, "sw"))
		return SL_ORG_SE;
	if (!Q_strcasecmp(zone, "cc"))
		return SL_ORG_CC;
	if (!Q_strcasecmp(zone, "cn"))
		return SL_ORG_CN;
	if (!Q_strcasecmp(zone, "cs"))
		return SL_ORG_CS;
	if (!Q_strcasecmp(zone, "cw"))
		return SL_ORG_CW;
	if (!Q_strcasecmp(zone, "ce"))
		return SL_ORG_CE;

	return atoi(zone);
}
void SCR_ShowPic_Script_f(void)
{
	char *imgname;
	char *name;
	char *tcommand;
	int x, y, w, h;
	int zone;
	showpic_t *sp;
	float fadedelay;

	imgname = Cmd_Argv(1);
	name = Cmd_Argv(2);
	x = atoi(Cmd_Argv(3));
	y = atoi(Cmd_Argv(4));
	zone = SCR_ShowPic_MapZone(Cmd_Argv(5));

	w = atoi(Cmd_Argv(6));
	h = atoi(Cmd_Argv(7));
	tcommand = Cmd_Argv(8);
	fadedelay = atof(Cmd_Argv(9));


	sp = SCR_ShowPic_Find(name);

	Z_Free(sp->picname);
	Z_Free(sp->tcommand);
	sp->picname = Z_StrDup(imgname);
	sp->tcommand = Z_StrDup(tcommand);

	sp->zone = zone;
	sp->x = x;
	sp->y = y;
	sp->w = w;
	sp->h = h;
	sp->fadedelay = fadedelay;

	if (!sp->persist)
		sp->persist = !Cmd_FromGamecode();

}

void SCR_ShowPic_Remove_f(void)
{
	SCR_ShowPic_ClearAll(!Cmd_FromGamecode());
}

//=============================================================================

void QDECL SCR_Fov_Callback (struct cvar_s *var, char *oldvalue)
{
	if (var->value < 10)
		Cvar_ForceSet(var, "10");
	//highs are capped elsewhere. this allows fisheye to use this cvar automatically.
}

void QDECL SCR_Viewsize_Callback (struct cvar_s *var, char *oldvalue)
{
	if (var->value < 30)
	{
		Cvar_ForceSet (var, "30");
		return;
	}
	if (var->value > 120)
	{
		Cvar_ForceSet (var, "120");
		return;
	}
}

void QDECL CL_Sbar_Callback(struct cvar_s *var, char *oldvalue)
{
}

void SCR_CrosshairPosition(playerview_t *pview, float *x, float *y)
{
	extern cvar_t cl_crossx, cl_crossy, crosshaircorrect, v_viewheight;

	vrect_t rect;
	rect = r_refdef.vrect;

	if (cl.worldmodel && crosshaircorrect.ival)
	{
		float adj;
		trace_t tr;
		vec3_t end;
		vec3_t start;
		vec3_t right, up, fwds;

		AngleVectors(pview->simangles, fwds, right, up);

		VectorCopy(pview->simorg, start);
		start[2]+=16;
		VectorMA(start, 100000, fwds, end);

		memset(&tr, 0, sizeof(tr));
		tr.fraction = 1;
		cl.worldmodel->funcs.NativeTrace(cl.worldmodel, 0, NULLFRAMESTATE, NULL, start, end, vec3_origin, vec3_origin, false, MASK_WORLDSOLID, &tr);
		start[2]-=16;
		if (tr.fraction != 1)
		{
			adj=pview->viewheight;
			if (v_viewheight.value < -7)
				adj+=-7;
			else if (v_viewheight.value > 4)
				adj+=4;
			else
				adj+=v_viewheight.value;

			start[2]+=adj;
			Matrix4x4_CM_Project(tr.endpos, end, pview->simangles, start, r_refdef.fov_x, r_refdef.fov_y);
			*x = rect.x+rect.width*end[0] + cl_crossx.value;
			*y = rect.y+rect.height*(1-end[1]) + cl_crossy.value;
			return;
		}
	}

	*x = rect.x + rect.width/2 + cl_crossx.value;
	*y = rect.y + rect.height/2 + cl_crossy.value;
}

/*
=================
SCR_SizeUp_f

Keybinding command
=================
*/
void SCR_SizeUp_f (void)
{
	if (Cmd_FromGamecode())
		Cvar_ForceSet(&scr_viewsize,va("%i", scr_viewsize.ival+10));
	else
		Cvar_SetValue (&scr_viewsize,scr_viewsize.value+10);
}


/*
=================
SCR_SizeDown_f

Keybinding command
=================
*/
void SCR_SizeDown_f (void)
{
	if (Cmd_FromGamecode())
		Cvar_ForceSet(&scr_viewsize,va("%i", scr_viewsize.ival-10));
	else
		Cvar_SetValue (&scr_viewsize,scr_viewsize.value-10);
}

//============================================================================

void SCR_StringXY(const char *str, float x, float y)
{
	const char *s2;
	int px, py;
	unsigned int codepoint;
	int error;
	//pick the largest of the two. this is to avoid awkwardness with fonts that lack a version <12 pixels high.
	struct font_s *font = (Font_CharVHeight(font_console)>Font_CharVHeight(font_default))?font_console:font_default;

	Font_BeginString(font, ((x<0)?vid.width:x), ((y<0)?vid.height - sb_lines:y), &px, &py);

	if (x < 0)
	{
		for (s2 = str; *s2; )
		{
			codepoint = unicode_decode(&error, s2, &s2, true);
			px -= Font_CharWidth(CON_WHITEMASK, codepoint);
		}
	}

	if (y < 0)
		py += y*Font_CharHeight();

	while (*str)
	{
		codepoint = unicode_decode(&error, str, &str, true);
		px = Font_DrawChar(px, py, CON_WHITEMASK, codepoint);
	}
	Font_EndString(font);
}

/*
==============
SCR_DrawTurtle
==============
*/
void SCR_DrawTurtle (void)
{
	static int      count;

	if (!scr_showturtle.ival || !scr_turtle)
		return;

	if (host_frametime <= 1.0/scr_turtlefps.value)
	{
		count = 0;
		return;
	}

	count++;
	if (count < 3)
		return;

	R2D_ScalePic (scr_vrect.x, scr_vrect.y, 64, 64, scr_turtle);
}

void SCR_DrawDisk (void)
{
	extern float fs_accessed_time;
	if ((float)realtime - fs_accessed_time > scr_diskicontimeout.value)
		return;
	if (!draw_disc || !scr_showdisk.ival)
		return;

	if (!R_GetShaderSizes(draw_disc, NULL, NULL, true))
		SCR_StringXY("FS", scr_showdisk_x.value, scr_showdisk_y.value);
	else
		R2D_ScalePic (scr_showdisk_x.value + (scr_showdisk_x.value<0?vid.width:0), scr_showdisk_y.value + (scr_showdisk_y.value<0?vid.height:0), 24, 24, draw_disc);
}

/*
==============
SCR_DrawNet
==============
*/
void SCR_DrawNet (void)
{
	if (realtime - cls.netchan.last_received < scr_neticontimeout.value)
		return;
	if (cls.demoplayback || !scr_net)
		return;

	R2D_ScalePic (scr_vrect.x+64, scr_vrect.y, 64, 64, scr_net);
}

void R_GetGPUUtilisation(float *gpu, float *mem)
{
#ifdef __linux__
	static qboolean tried;
	typedef void *nvmlDevice_t;
	struct nvmlUtilization_s
	{
		unsigned int gpu;
		unsigned int mem;
	} util = {~0u,~0u};
	static int (*nvmlDeviceGetUtilizationRates) (nvmlDevice_t device, struct nvmlUtilization_s *utilization);
	static nvmlDevice_t dev;
	if (!tried)
	{
		int (*nvmlInit_v2) (void);
		int (*nvmlDeviceGetHandleByIndex_v2) (unsigned int index, nvmlDevice_t *device);
		dllhandle_t *nvml;
		dllfunction_t funcs[] =
		{
			{(void**)&nvmlInit_v2, "nvmlInit_v2"},
			{(void**)&nvmlDeviceGetHandleByIndex_v2, "nvmlDeviceGetHandleByIndex_v2"},
			{(void**)&nvmlDeviceGetUtilizationRates, "nvmlDeviceGetUtilizationRates"},
			{NULL}
		};
		tried = true;

		nvml = Sys_LoadLibrary("libnvidia-ml.so.1", funcs);
		if (nvml)
		{
			if (!nvmlInit_v2())
				if (nvmlDeviceGetHandleByIndex_v2(0, &dev))
					dev = NULL;
		}
	}

	if (dev)
		nvmlDeviceGetUtilizationRates(dev, &util);
	*gpu = (util.gpu == ~0u)?-1:(util.gpu/100.0);
	*mem = (util.mem == ~0u)?-1:(util.mem/100.0);
#else
	*gpu = *mem = -1;
#endif
}

void SCR_DrawFPS (void)
{
	extern cvar_t show_fps;
	static double lastupdatetime;
	static double lastsystemtime;
	double t;
	extern int fps_count;
	static float lastfps;
	char str[80];
	extern cvar_t cl_fakeframes;

	float frametime;

	static float gpu=-1, gpumem=-1;

	if (!show_fps.ival)
		return;

	t = Sys_DoubleTime();
	if ((t - lastupdatetime) >= 1.0)
	{
		lastfps = fps_count/(t - lastupdatetime);
		fps_count = 0;
		lastupdatetime = t;

		if (developer.ival)
			R_GetGPUUtilisation(&gpu, &gpumem);	//not all that accurate, but oh well.
		else
			gpu=gpumem=-1;
	}
	frametime = t - lastsystemtime;
	lastsystemtime = t;

	if (show_fps.value < 0)
		R_FrameTimeGraph(frametime, 0);
	else if (show_fps.value > 1)
		R_FrameTimeGraph(frametime, show_fps.value-1);
	if (cl_fakeframes.ival!=0)
		sprintf(str, "%3.1f FPS(x%i)", lastfps, max(0,cl_fakeframes.ival)+1);
	else
		sprintf(str, "%3.1f FPS", lastfps);
	SCR_StringXY(str, show_fps_x.value, show_fps_y.value);

	if (gpu>=0)
	{
		sprintf(str, "%.0f%% GPU", gpu*100);
		SCR_StringXY(str, show_fps_x.value, (show_fps_y.value>=0)?(show_fps_y.value+8):(show_fps_y.value-1));
	}
/*	if (gpumem>=0)
	{
		sprintf(str, "%.0f%% VRAM Bus", gpumem*100);
		SCR_StringXY(str, show_fps_x.value, (show_fps_y.value>=0)?(show_fps_y.value+16):(show_fps_y.value-2));
	}*/
}

void SCR_DrawClock(void)
{
	struct tm *newtime;
	time_t long_time;
	char str[16];

	if (!show_clock.ival)
		return;

	time( &long_time );
	newtime = localtime( &long_time );
	if (show_clock.ival == 2)
		strftime( str, sizeof(str)-1, "%I:%M %p   ", newtime);
	else
		strftime( str, sizeof(str)-1, "%H:%M    ", newtime);

	SCR_StringXY(str, show_clock_x.value, show_clock_y.value);
}

void SCR_DrawGameClock(void)
{
#ifdef QUAKESTATS
	float showtime;
	int minutes;
	int seconds;
	char str[16];
	int flags;
	float timelimit;

	if (!show_gameclock.ival)
		return;

	flags = (show_gameclock.value-1);
	if (flags & 1)
		timelimit = 60 * atof(InfoBuf_ValueForKey(&cl.serverinfo, "timelimit"));
	else
		timelimit = 0;

	if (cl.matchstate == MATCH_STANDBY)
		showtime = cl.servertime;
	else if (cl.playerview[0].statsf[STAT_MATCHSTARTTIME])
		showtime = timelimit - (cl.servertime - cl.playerview[0].statsf[STAT_MATCHSTARTTIME]/1000);
	else 
		showtime = timelimit - (cl.servertime - cl.matchgametimestart);

	if (showtime < 0)
		showtime *= -1;

	minutes = showtime/60;
	seconds = (int)showtime - (minutes*60);
	sprintf(str, "%02i:%02i", minutes, seconds);

	SCR_StringXY(str, show_gameclock_x.value, show_gameclock_y.value);
#endif
}

/*
==============
DrawPause
==============
*/
void SCR_DrawPause (void)
{
	mpic_t  *pic;

	if (!scr_showpause.ival)               // turn off for screenshots
		return;

	if (!cl.paused)
		return;

#ifndef CLIENTONLY
	if (sv.active && sv.paused == PAUSE_AUTO)
		return;
#endif

	if (Key_Dest_Has(kdm_menu))
		return;

	pic = R2D_SafeCachePic ("gfx/pause.lmp");
	if (pic)
	{
		R2D_ScalePic ( (vid.width - pic->width)/2,
			(vid.height - 48 - pic->height)/2, pic->width, pic->height, pic);
	}
	else
		Draw_FunString((vid.width-strlen("Paused")*8)/2, (vid.height-8)/2, "Paused");
}



/*
==============
SCR_DrawLoading
==============
*/

int			total_loading_size, current_loading_size, loading_stage;
char		*loadingfile;
int CL_DownloadRate(void);

int SCR_GetLoadingStage(void)
{
	return loading_stage;
}
void SCR_SetLoadingStage(int stage)
{
	switch(stage)
	{
	case LS_NONE:
		if (loadingfile)
			Z_Free(loadingfile);
		loadingfile = NULL;
		scr_disabled_for_loading = scr_drawloading = false;
		break;
	case LS_CONNECTION:
		SCR_SetLoadingFile("waiting for connection...");
		break;
	case LS_SERVER:
		SCR_SetLoadingFile("starting server...");
		break;
	case LS_CLIENT:
		SCR_SetLoadingFile("receiving map info");
		break;
	}
	loading_stage = stage;
}
void SCR_SetLoadingFile(char *str)
{
	if (loadingfile && !strcmp(loadingfile, str))
		return;

	if (loadingfile)
		Z_Free(loadingfile);
	loadingfile = Z_Malloc(strlen(str)+1);
	strcpy(loadingfile, str);

	if (scr_loadingrefresh.ival)
	{
		qboolean oldwarndraw = r_refdef.warndraw;
		r_refdef.warndraw = false;
		SCR_UpdateScreen();
		r_refdef.warndraw = oldwarndraw;
	}
}

void SCR_DrawLoading (qboolean opaque)
{
	int sizex, x, y, w, h;
	mpic_t  *pic;
	char *s;

	if (CSQC_UseGamecodeLoadingScreen())
		return;	//will be drawn as part of the regular screen updates
#ifdef MENU_DAT
	if (MP_UsingGamecodeLoadingScreen())
	{
		if (opaque)
			MP_Draw();
		return;	//menuqc should have just drawn whatever overlays it wanted.
	}
#endif
#ifdef MENU_NATIVECODE
	if (mn_entry && mn_entry->DrawLoading)
	{
		mn_entry->DrawLoading(host_frametime);
		return;
	}
#endif

	//int mtype = M_GameType(); //unused variable
	y = vid.height/2;

	if (R2D_DrawLevelshot())
		;
	else if (opaque)
	{
		R2D_ImageColours(0, 0, 0, 1);
		R2D_FillBlock(0, 0, vid.width, vid.height);
		R2D_ImageColours(1, 1, 1, 1);
		R2D_ConsoleBackground (0, vid.height, true);
	}

	if (scr_showloading.ival)
	{
		char *qname = scr_loadingscreen_picture.string;

#ifdef HEXEN2
		int qdepth = COM_FDepthFile(qname, true);
		int h2depth = COM_FDepthFile("gfx/menu/loading.lmp", true);

		if (qdepth < h2depth && h2depth != FDEPTH_MISSING)
		{	//hexen2 files.
			//hexen2 has some fancy sliders built into its graphics in specific places. so this is messy.
			pic = R2D_SafeCachePic ("gfx/menu/loading.lmp");
			if (R_GetShaderSizes(pic, &w, &h, true))
			{
				int		size, count, offset;

				if (!scr_drawloading && loading_stage == 0)
					return;

				offset = (vid.width - w)/2;
				R2D_ScalePic (offset, 0, w, h, pic);

				if (loading_stage == LS_NONE)
					return;

				if (total_loading_size)
					size = current_loading_size * 106 / total_loading_size;
				else
					size = 0;

				if (loading_stage == LS_CLIENT)
					count = size;
				else
					count = 106;

				R2D_ImagePaletteColour (136, 1.0);
				R2D_FillBlock (offset+42, 87, count, 1);
				R2D_FillBlock (offset+42, 87+5, count, 1);
				R2D_ImagePaletteColour (138, 1.0);
				R2D_FillBlock (offset+42, 87+1, count, 4);

				if (loading_stage == LS_SERVER)
					count = size;
				else
					count = 0;

				R2D_ImagePaletteColour(168, 1.0);
				R2D_FillBlock (offset+42, 97, count, 1);
				R2D_FillBlock (offset+42, 97+5, count, 1);
				R2D_ImagePaletteColour(170, 1.0);
				R2D_FillBlock (offset+42, 97+1, count, 4);

				y = 104;
			}
		}
		else
#endif
		{	//quake files
			pic = R2D_SafeCachePic (qname);
			if (R_GetShaderSizes(pic, &w, &h, true))
			{
				float f = 1;
				w *= scr_loadingscreen_scale.value;
				h *= scr_loadingscreen_scale.value;
				switch(scr_loadingscreen_scale_limit.ival)
				{
				default:
					break;
				case 1:
					f = max(w/vid.width,h/vid.height);
					break;
				case 2:
					f = min(w/vid.width,h/vid.height);
					break;
				case 3:
					f = w/vid.width;
					break;
				case 4:
					f = h/vid.height;
					break;
				}
				if (f > 1)
				{
					w /= f;
					h /= f;
				}
				x = ((int)vid.width - w)/2;
				y = ((int)vid.height - 48 - h)/2;
				R2D_ScalePic (x, y, w, h, pic);
				x = (vid.width/2) - 96;
				y += h + 8;

				//make sure our loading crap will be displayed.
				if (y > vid.height-32)
					y = vid.height-32;
			}
			else
			{
				x = (vid.width/2) - 96;
				y = (vid.height/2) - 8;

				Draw_FunString((vid.width-7*8)/2, y-16, "Loading");
			}

			if (!total_loading_size)
				total_loading_size = 1;
			if (loading_stage > LS_CONNECTION)
			{
				sizex = current_loading_size * 192 / total_loading_size;
				if (loading_stage == LS_SERVER)
				{
					R2D_ImageColours(1.0, 0.0, 0.0, 1.0);
					R2D_FillBlock(x, y, sizex, 16);
					R2D_ImageColours(0.0, 0.0, 0.0, 1.0);
					R2D_FillBlock(x+sizex, y, 192-sizex, 16);
				}
				else
				{
					R2D_ImageColours(1.0, 1.0, 0.0, 1.0);
					R2D_FillBlock(x, y, sizex, 16);
					R2D_ImageColours(1.0, 0.0, 0.0, 1.0);
					R2D_FillBlock(x+sizex, y, 192-sizex, 16);
				}

				R2D_ImageColours(1, 1, 1, 1);
				Draw_FunString(x+8, y+4, va("Loading %s... %i%%",
					(loading_stage == LS_SERVER) ? "server" : "client",
					current_loading_size * 100 / total_loading_size));
			}
			y += 16;

			if (loadingfile)
			{
				Draw_FunString(x+8, y+4, loadingfile);
				y+=16;
			}
		}
	}
	R2D_ImageColours(1, 1, 1, 1);

	if (cl.downloadlist || cls.download)
	{
		unsigned int fcount;
		qofs_t tsize;
		qboolean sizeextra;

		x = vid.width/2 - 160;

		CL_GetDownloadSizes(&fcount, &tsize, &sizeextra);
		//downloading files?
		if (cls.download)
			Draw_FunString(x+8, y+4, va("Downloading %s... %i%%",
				cls.download->localname,
				(int)cls.download->percent));

		if (tsize > 1024*1024*16)
		{
			Draw_FunString(x+8, y+8+4, va("%5ukbps %8umb%s remaining (%i files)",
				(unsigned int)(CL_DownloadRate()/1000.0f),
				(unsigned int)(tsize/(1024*1024)),
				sizeextra?"+":"",
				fcount));
		}
		else
		{
			Draw_FunString(x+8, y+8+4, va("%5ukbps %8ukb%s remaining (%i files)",
				(unsigned int)(CL_DownloadRate()/1000.0f),
				(unsigned int)(tsize/1024),
				sizeextra?"+":"",
				fcount));
		}

		y+= 16+8;
	}
	else if ((s=CL_TryingToConnect()))
	{
		char *nl;
		if (!strchr(s, '\n'))
			s = va("^bConnecting to: %s...", s);

		for (;s; s = nl)
		{
			nl = strchr(s, '\n');
			if (nl)
				*nl++ = 0;
			Draw_FunStringWidth(0, y+4, s, vid.width, 2, false);
			y += 8;
		}
	}
}

void SCR_BeginLoadingPlaque (void)
{
	if (cls.state != ca_active && cls.protocol != CP_QUAKE3)
		return;

	if (!scr_initialized)
		return;

//	if (key_dest == key_console) //not really appropriate if client is to show it on a remote server.
//		return;

// redraw with no console and the loading plaque
	if (!scr_disabled_for_loading)
	{
		qboolean oldwarndraw = r_refdef.warndraw;
		Sbar_Changed ();
		r_refdef.warndraw = false;
		scr_drawloading = true;
		SCR_UpdateScreen ();
		scr_drawloading = false;
		scr_disabled_for_loading = true;
		r_refdef.warndraw = oldwarndraw;
	}

	scr_disabled_time = Sys_DoubleTime();	//realtime tends to change... Hmmm....
}

void SCR_EndLoadingPlaque (void)
{
//	if (!scr_initialized)
//		return;

	scr_disabled_for_loading = false;
	*levelshotname = '\0';
	SCR_SetLoadingStage(0);
	scr_drawloading = false;
}

void SCR_ImageName (const char *mapname)
{
	//assume levelshots/foo
	strcpy(levelshotname, "levelshots/");
	COM_FileBase(mapname, levelshotname + strlen(levelshotname), sizeof(levelshotname)-strlen(levelshotname));

	if (qrenderer && scr_loadingscreen_aspect.ival >= 0 && *mapname)
	{
		R_LoadHiResTexture(levelshotname, NULL, IF_NOWORKER|IF_UIPIC|IF_NOPICMIP|IF_NOMIPMAP|IF_CLAMP);

		if (!R_GetShaderSizes(R2D_SafeCachePic (levelshotname), NULL, NULL, true))
		{
			//try maps/foo
			strcpy(levelshotname, "maps/");
			COM_FileBase(mapname, levelshotname + strlen(levelshotname), sizeof(levelshotname)-strlen(levelshotname));
			if (!R_GetShaderSizes(R2D_SafeCachePic (levelshotname), NULL, NULL, true))
			{
				*levelshotname = '\0';
				if (scr_disabled_for_loading)
					return;
			}
		}
	}
	else
	{
		*levelshotname = '\0';
		if (scr_disabled_for_loading)
			return;
	}

	scr_drawloading = true;
	if (qrenderer != QR_NONE)
	{
		qboolean oldwarndraw = r_refdef.warndraw;
		r_refdef.warndraw = false;
		Sbar_Changed ();
		SCR_UpdateScreen ();
		r_refdef.warndraw = oldwarndraw;
	}

	scr_disabled_time = Sys_DoubleTime();	//realtime tends to change... Hmmm....
	scr_disabled_for_loading = true;
}


//=============================================================================


/*
==================
SCR_SetUpToDrawConsole
==================
*/
void SCR_SetUpToDrawConsole (void)
{
	extern int startuppending;	//true if we're downloading media or something and have not yet triggered the startup action (read: main menu or cinematic)
	float fullscreenpercent = 1;
#ifdef ANDROID
	//android has an onscreen imm that we don't want to obscure
	fullscreenpercent = scr_consize.value;
#endif
	if (!con_stayhidden.ival && (!Key_Dest_Has(~(kdm_console|kdm_game))) && (!cl.sendprespawn && cl.worldmodel && cl.worldmodel->loadstate != MLS_LOADED))
	{
		//force console to fullscreen if we're loading stuff (but don't necessarily force focus)
//		Key_Dest_Add(kdm_console);
		scr_con_target = scr_con_current = vid.height * fullscreenpercent;
	}
	else if (!startuppending && !Key_Dest_Has(kdm_menu) && (!Key_Dest_Has(~((!con_stayhidden.ival?kdm_console:0)|kdm_prompt|kdm_game))) && SCR_GetLoadingStage() == LS_NONE && cls.state < ca_active && !CSQC_UnconnectedOkay(false))
	{
		//go fullscreen if we're not doing anything
		if (con_curwindow && !cls.state && !scr_drawloading && !Key_Dest_Has(kdm_console))
		{
			Key_Dest_Add(kdm_cwindows);
			scr_con_target = 0; // not looking at an normal console
		}
#ifdef VM_UI
		else if (q3 && q3->ui.OpenMenu())
			scr_con_current = scr_con_target = 0;	//force instantly hidden.
#endif
		else
		{
			qboolean legacyfullscreen = false;
			if (cls.state < ca_demostart)
			{
				if (con_stayhidden.ival)
				{	//go to the menu instead of the console.
					extern int startuppending;
					if (!scr_drawloading && SCR_GetLoadingStage() == LS_NONE)
					{
						if (CL_TryingToConnect())	//if we're trying to connect, make sure there's a loading/connecting screen showing instead of forcing the menu visible
							SCR_SetLoadingStage(LS_CONNECTION);
						else if (!Key_Dest_Has(kdm_menu) && !Key_Dest_Has(kdm_prompt) && !PM_IsApplying() && !startuppending)	//don't force anything until the startup stuff has been done
							M_ToggleMenu_f();
					}
				}
				else
				{	//nothing happening, make sure the console is visible or something.
					if (!scr_drawloading)
					{
						if (SCR_GetLoadingStage() == LS_NONE && CL_TryingToConnect())	//if we're trying to connect, make sure there's a loading/connecting screen showing instead of forcing the menu visible
							SCR_SetLoadingStage(LS_CONNECTION);
						else
							Key_Dest_Add(kdm_console);
					}
					legacyfullscreen = true;
				}
			}

			if (startuppending)
				scr_con_target = 0;	//not made any decisions yet
			else if (Key_Dest_Has(kdm_console) || legacyfullscreen)
				scr_con_current = scr_con_target = vid.height * fullscreenpercent; // force instantly to fullscreen
			else
				scr_con_target = 0;
		}
	}
	else if (Key_Dest_Has(kdm_console))
	{
		//go half-screen if we're meant to have the console visible
		scr_con_target = vid.height*scr_consize.value;    // half screen
		if (scr_con_target < 32)
			scr_con_target = 32;	//prevent total loss of console.
		else if (scr_con_target>vid.height)
			scr_con_target = vid.height;
	}
	else
		scr_con_target = 0;                               // scroll to nothing

	if (scr_con_target < scr_con_current)
	{
		scr_con_current -= scr_conspeed.value*host_frametime * (vid.height/320.0f);
		if (scr_con_target > scr_con_current)
			scr_con_current = scr_con_target;

	}
	else if (scr_con_target > scr_con_current)
	{
		scr_con_current += scr_conspeed.value*host_frametime * (vid.height/320.0f);
		if (scr_con_target < scr_con_current)
			scr_con_current = scr_con_target;
	}

	if (scr_con_current>vid.height)
		scr_con_current = vid.height;

	if (clearconsole++ < vid.numpages)
	{
		Sbar_Changed ();
	}
	else if (clearnotify++ < vid.numpages)
	{
	}
}

/*
==================
SCR_DrawConsole
==================
*/
void SCR_DrawConsole (qboolean noback)
{
	if (!scr_con_current)
	{
		if (!Key_Dest_Has(kdm_console|kdm_menu))
			Con_DrawNotify ();      // only draw notify in game
	}
	if (vrui.enabled)	//make the console all-or-nothing when its a vr ui.
		Con_DrawConsole (scr_con_target?vid.height:0, noback);
	else
		Con_DrawConsole (scr_con_current, noback);
	if (scr_con_current || Key_Dest_Has(kdm_cwindows))
		clearconsole = 0;
}


/*
==============================================================================

						SCREEN SHOTS

==============================================================================
*/

/*
==================
SCR_ScreenShot_f
==================
*/
static void SCR_ScreenShot_f (void)
{
	char			displayname[1024];
	char            pcxname[MAX_QPATH];
	int                     i;
	vfsfile_t *vfs;
	void *rgbbuffer;
	int stride, width, height;
	enum uploadfmt fmt;

	if (!VID_GetRGBInfo)
	{
		Con_Printf("Screenshots are not supported with the current renderer\n");
		return;
	}

	if (Cmd_Argc() == 2)
	{
		/*
		  FTESurf: a NAMED screenshot honours scr_sshot_prefix's directory, the
		  same way every sibling command already does.

		  This branch used to copy the argument straight into pcxname and add an
		  extension, ignoring the prefix entirely -- so `screenshot` with no
		  argument wrote screenshots/fte-<date>-0.png while `screenshot foo`
		  wrote foo.png into the gamedir ROOT.  Two commands, one name, two
		  destinations, and the odd one out is the one every script uses.

		  screenshot_mega, _stereo, _360 and _vr all resolve this by keeping the
		  prefix's path and replacing only its basename (COM_SkipPath), which is
		  what makes "screenshots/fte-" mean a folder AND a filename stem.  This
		  is that idiom, copied verbatim rather than reinvented.

		  The name check moved onto the ARGUMENT because pcxname now holds the
		  prefix by the time the name goes in -- checking the joined string would
		  test our own prefix rather than the user's input.
		*/
		char *mangle;
		if (strstr (Cmd_Argv(1), "..") || strchr(Cmd_Argv(1), ':') || *Cmd_Argv(1) == '.' || *Cmd_Argv(1) == '/')
		{
			Con_Printf("Screenshot name refused\n");
			return;
		}
		Q_strncpyz(pcxname, scr_sshot_prefix.string, sizeof(pcxname));
		mangle = COM_SkipPath(pcxname);
		Q_snprintfz(mangle, sizeof(pcxname) - (mangle - pcxname), "%s", Cmd_Argv(1));
		COM_DefaultExtension (pcxname, scr_sshot_type.string, sizeof(pcxname));
	}
	else
	{
		int stop = 1000;
		char stem[MAX_QPATH];
		/*
		FTESurf Patch 211: name an unnamed screenshot after the map, <map>_000.png,
		counting up per map.

		The COUNTER IS THE PROBE, not stored state.  The loop below already walks
		i upward until a name is free, so numbering per map falls out of changing
		the stem: nothing to persist, nothing to reset on a map change, it survives
		a restart, and deleting _001 refills 001 rather than leaving a hole.  That
		is why this is three lines and not a table of per-map counters.

		The prefix contributes its DIRECTORY only.  scr_sshot_prefix is a directory
		AND a filename stem ("screenshots/fte-"), and <map> is already the stem we
		want, so keeping "fte-" would produce screenshots/fte-surf_666_000.png.
		COM_SkipPath is the same call the named branch above uses to split the two
		halves, so both branches agree about what the cvar means.

		scr_sshot_mapname 0 restores the <date>-<n> form byte for byte.
		*/
		if (scr_sshot_mapname.ival && cl.worldmodel && *cl.worldmodel->name)
			COM_FileBase(cl.worldmodel->name, stem, sizeof(stem));	//maps/surf_666.bsp -> surf_666
		else if (scr_sshot_mapname.ival)
			Q_strncpyz(stem, "menu", sizeof(stem));	//no map loaded: cl.worldmodel is NULL at the menu
		else
		{
			time_t tm = time(NULL);
			strftime(stem, sizeof(stem), "%Y%m%d%H%M%S", localtime(&tm));
		}
	//
	// find a file name to save it to
	//
		for (i=0 ; i<stop ; i++)
		{
			if (scr_sshot_mapname.ival)
			{
				char dir[MAX_QPATH];
				Q_strncpyz(dir, scr_sshot_prefix.string, sizeof(dir));
				*COM_SkipPath(dir) = 0;	//"screenshots/fte-" -> "screenshots/"
				Q_snprintfz(pcxname, sizeof(pcxname), "%s%s_%03i.%s", dir, stem, i, scr_sshot_type.string);
			}
			else
				Q_snprintfz(pcxname, sizeof(pcxname), "%s%s-%i.%s", scr_sshot_prefix.string, stem, i, scr_sshot_type.string);

			if (!(vfs = FS_OpenVFS(pcxname, "rb", FS_GAMEONLY)))
				break;  // file doesn't exist
			VFS_CLOSE(vfs);
		}
		if (i==stop)
		{
			Con_Printf ("SCR_ScreenShot_f: Couldn't create sequentially named file\n");
			return;
		}
	}

	FS_DisplayPath(pcxname, FS_GAMEONLY, displayname, sizeof(displayname));

	rgbbuffer = VID_GetRGBInfo(&stride, &width, &height, &fmt);
	if (rgbbuffer)
	{
		//regarding metadata - we don't really know what's on the screen, so don't write something that may be wrong (eg: if there's only a console, don't claim that its a 360 image)
		if (SCR_ScreenShot(pcxname, FS_GAMEONLY, &rgbbuffer, 1, stride, width, height, fmt, false))
		{
			Con_Printf ("Wrote %s\n", displayname);
			BZ_Free(rgbbuffer);
			return;
		}
		BZ_Free(rgbbuffer);
		Con_Printf (CON_ERROR "Couldn't write %s\n", displayname);
	}
	else
		Con_Printf (CON_ERROR "Couldn't get colour buffer for screenshot\n");
}

void *SCR_ScreenShot_Capture(int fbwidth, int fbheight, int *stride, enum uploadfmt *fmt, qboolean no2d, qboolean hdr)
{
	int width, height;
	void *buf;
	qboolean okay = false;
	qboolean usefbo;
	qboolean oldwarndraw = r_refdef.warndraw;

	if (fbwidth == vid.fbpwidth && fbheight == vid.fbpheight && qrenderer != QR_VULKAN)
		usefbo = false;
#ifdef GLQUAKE
	else if (qrenderer == QR_OPENGL && gl_config.ext_framebuffer_objects)
		usefbo = true;
#endif
	else
		return NULL;

	if (usefbo)
	{
		r_refdef.warndraw = true;
		Q_strncpyz(r_refdef.rt_destcolour[0].texname, "megascreeny", sizeof(r_refdef.rt_destcolour[0].texname));
		/*vid.framebuffer =*/R2D_RT_Configure(r_refdef.rt_destcolour[0].texname, fbwidth, fbheight, (hdr&&sh_config.texfmt[PTI_RGBA16F])?PTI_RGBA16F:PTI_RGBA8, RT_IMAGEFLAGS);
		BE_RenderToTextureUpdate2d(true);
	}
	else
		r_refdef.warndraw = false;

	R2D_FillBlock(0, 0, vid.fbvwidth, vid.fbvheight);

#ifdef VM_CG
	if (!okay && q3 && q3->cg.Redraw(cl.time))
		okay = true;
#endif
#ifdef CSQC_DAT
	if (!okay && CSQC_DrawView())
		okay = true;
	if (usefbo)// && !*r_refdef.rt_destcolour[0].texname)
	{	//csqc protects its own. lazily.
		Q_strncpyz(r_refdef.rt_destcolour[0].texname, "megascreeny", sizeof(r_refdef.rt_destcolour[0].texname));
		BE_RenderToTextureUpdate2d(true);
	}
#endif
	if (!okay && r_worldentity.model)
	{
		V_RenderView (no2d);
		okay = true;
	}
	if (R2D_Flush)
		R2D_Flush();

	//fixme: add a way to get+save the depth values too
	if (!okay)
	{
		buf = NULL;
		*stride = width = height = 0;
		*fmt = TF_INVALID;
	}
	else
		buf = VID_GetRGBInfo(stride, &width, &height, fmt);

	if (usefbo)
	{
		R2D_RT_Configure(r_refdef.rt_destcolour[0].texname, 0, 0, 0, RT_IMAGEFLAGS);
		Q_strncpyz(r_refdef.rt_destcolour[0].texname, "", sizeof(r_refdef.rt_destcolour[0].texname));
		BE_RenderToTextureUpdate2d(true);
	}

	r_refdef.warndraw = oldwarndraw;

	if (!buf || width != fbwidth || height != fbheight)
	{
		*fmt = TF_INVALID;
		BZ_Free(buf);
		return NULL;
	}
	return buf;
}

static void SCR_ScreenShot_Mega_f(void)
{
	extern cvar_t r_projection;
	int stride[2];
	int width[2];
	int height[2];
	void *buffers[2];
	enum uploadfmt fmt[2];
	int numbuffers = 0;
	int buf;
	char filename[MAX_QPATH];
	stereomethod_t osm = r_refdef.stereomethod;
	int projection = r_projection.ival;

	//massage the rendering code to redraw the screen with an fbo forced.
	//this allows us to generate screenshots which are not otherwise possible to actually draw.
	//this includes larger resolutions (although the lack of stiching leaves us subject to gpu limits of about 16k)

	char *screenyname = Cmd_Argv(1);
	unsigned int fbwidth = strtoul(Cmd_Argv(2), NULL, 0);
	unsigned int fbheight = strtoul(Cmd_Argv(3), NULL, 0);
	char ext[8];

	if (Cmd_IsInsecure())
		return;

	if (qrenderer <= QR_HEADLESS)
	{
		Con_Printf("No renderer active\n");
		return;
	}

	if (strcmp(Cmd_Argv(0), "screenshot_mega"))
	{	//screenshot_360, screenshot_stereo etc probably don't want to capture mega screenshots
		//so default to something more realistic instead.
		if (!fbwidth)
			fbwidth = vid.pixelwidth;
		if (!fbheight)
			fbheight = vid.pixelheight;
	}

	if (!fbwidth)
		fbwidth = sh_config.texture2d_maxsize;
	fbwidth = bound(1, fbwidth, sh_config.texture2d_maxsize);
	if (!fbheight)
		fbheight = (fbwidth * 3)/4;
	fbheight = bound(1, fbheight, sh_config.texture2d_maxsize);

	if (strstr (screenyname, "..") || strchr(screenyname, ':') || *screenyname == '.' || *screenyname == '/')
		screenyname = "";
	if (!*screenyname)
	{
		screenyname = "megascreeny";
		Q_snprintfz(filename, sizeof(filename), "%s%s", scr_sshot_prefix.string, screenyname);
	}
	else
	{
		char *mangle;
		Q_snprintfz(filename, sizeof(filename), "%s", scr_sshot_prefix.string);
		mangle = COM_SkipPath(filename);
		Q_snprintfz(mangle, sizeof(filename) - (mangle-filename), "%s", screenyname);
	}

	if (!strcmp(Cmd_Argv(0), "screenshot_360"))
		r_projection.ival = PROJ_EQUIRECTANGULAR;

	if (!strcmp(Cmd_Argv(0), "screenshot_stereo"))
		COM_DefaultExtension (filename, "png", sizeof(filename));	//png/pns is the only format that can really cope with this right now.
	else
		COM_DefaultExtension (filename, scr_sshot_type.string, sizeof(filename));

	COM_FileExtension(filename, ext, sizeof(ext));
	if (!strcmp(Cmd_Argv(0), "screenshot_stereo") || !strcmp(ext, "pns") || osm == STEREO_QUAD)
		numbuffers = 2;	//stereo png
	else
		numbuffers = 1;

	if (numbuffers == 2 && Q_strcasecmp(ext, "png") && Q_strcasecmp(ext, "pns"))
	{
		Con_Printf("Only png format is supported for stereo screenshots\n");
		return;
	}

	for(buf = 0; buf < numbuffers; buf++)
	{
		if (numbuffers == 2)
		{
			if (buf)
				r_refdef.stereomethod = STEREO_RIGHTONLY;
			else
				r_refdef.stereomethod = STEREO_LEFTONLY;
		}

		buffers[buf] = SCR_ScreenShot_Capture(fbwidth, fbheight, &stride[buf], &fmt[buf], false, false);
		width[buf] = fbwidth;
		height[buf] = fbheight;

		if (width[buf] != width[0] || height[buf] != height[0] || fmt[buf] != fmt[0])
		{	//invalid is better than unmatched.
			BZ_Free(buffers[buf]);
			buffers[buf] = NULL;
		}
	}

	if (numbuffers == 2 && !buffers[1])
		numbuffers = 1;

	//okay, we drew something, we're good to save a screeny.
	if (buffers[0])
	{
		if (SCR_ScreenShot(filename, FS_GAMEONLY, buffers, numbuffers, stride[0], width[0], height[0], fmt[0], true))
		{
			char			displayname[1024];
			FS_DisplayPath(filename, FS_GAMEONLY, displayname, sizeof(displayname));
			Con_Printf ("Wrote %s\n", displayname);
		}
	}
	else
		Con_Printf("Unable to capture screenshot\n");

	for(buf = 0; buf < numbuffers; buf++)
		BZ_Free(buffers[buf]);

	r_refdef.stereomethod = osm;
	r_projection.ival = projection;
}

static void SCR_ScreenShot_VR_f(void)
{
	char *screenyname = Cmd_Argv(1);
	const char *ext;
	int width = atoi(Cmd_Argv(2));
	int stride=0;
	//we spin the camera around, taking slices from equirectangular screenshots
	char filename[MAX_QPATH];
	int height;	//equirectangular 360 * 180 gives a nice clean ratio
	int px = 4;
	int step = atof(Cmd_Argv(3));
	void *buffer[2], *buf;
	enum uploadfmt fmt;
	int lx, rx, x, y;
	vec3_t baseang;
	float ang;
	qboolean fail = false;
	extern cvar_t r_projection, r_stereo_separation, r_stereo_convergence;
	qboolean stereo;
	VectorCopy(r_refdef.viewangles, baseang);

	if (width <= 2)
		width = 2048;
	if (width > sh_config.texture2d_maxsize)
		width = sh_config.texture2d_maxsize;
	height = width/2;
	if (step <= 0)
		step = 5;

	buffer[0] = BZF_Malloc (width*height*2*px);
	buffer[1] = (qbyte*)buffer[0] + width*height*px;

	if (strstr (screenyname, "..") || strchr(screenyname, ':') || *screenyname == '.' || *screenyname == '/')
		screenyname = "";
	if (!*screenyname)
	{
		screenyname = "vr";
		Q_snprintfz(filename, sizeof(filename), "%s%s", scr_sshot_prefix.string, screenyname);
	}
	else
	{
		char *mangle;
		Q_snprintfz(filename, sizeof(filename), "%s", scr_sshot_prefix.string);
		mangle = COM_SkipPath(filename);
		Q_snprintfz(mangle, sizeof(filename) - (mangle-filename), "%s", screenyname);
	}
	COM_DefaultExtension (filename, scr_sshot_type.string, sizeof(filename));

	ext = COM_GetFileExtension(filename, NULL);
	stereo = !Q_strcasecmp(ext, ".pns") || !Q_strcasecmp(ext, ".jns");



	if (!buffer[0])
	{
		Con_Printf("out of memory\n");
		return;
	}

	r_projection.ival = PROJ_EQUIRECTANGULAR;
	for (lx = 0; lx < width; lx = rx)
	{
		rx = lx + step;
		if (rx > width)
			rx = width;

		r_refdef.stereomethod = STEREO_OFF;

		cl.playerview->simangles[0] = 0;	//pitch is BAD
		cl.playerview->simangles[1] = baseang[1] + r_stereo_convergence.value*0.5;
		cl.playerview->simangles[2] = 0; //roll is BAD
		VectorCopy(cl.playerview->simangles, cl.playerview->viewangles);

		//FIXME: it should be possible to do this more inteligently, and get both strips with a single render.
		//FIXME: we should render to a PBO instead, so that the gpu+cpu don't need to sync until the very end.
		//FIXME: we should be using scissoring to avoid redrawing the entire screen (also tweak cull planes)

		ang = M_PI*2*(baseang[1]/360.0 + (lx+0.5*(rx-lx))/width);
		r_refdef.eyeoffset[0] = sin(ang) * r_stereo_separation.value * 0.5;
		r_refdef.eyeoffset[1] = cos(ang) * r_stereo_separation.value * 0.5;
		r_refdef.eyeoffset[2] = 0;
		buf = SCR_ScreenShot_Capture(width, height, &stride, &fmt, true, false);
		switch(fmt)
		{
		case TF_BGRA32:
			for (y = 0; y < height; y++)
			for (x = lx; x < rx; x++)
				((unsigned int*)buffer[0])[y*width + x] = ((unsigned int*)buf)[y*width + x];
			break;
		case TF_RGB24:
			for (y = 0; y < height; y++)
			for (x = lx; x < rx; x++)
			{
				((qbyte*)buffer[0])[(y*width + x)*4+0] = ((qbyte*)buf)[(y*width + x)*3+2];
				((qbyte*)buffer[0])[(y*width + x)*4+1] = ((qbyte*)buf)[(y*width + x)*3+1];
				((qbyte*)buffer[0])[(y*width + x)*4+2] = ((qbyte*)buf)[(y*width + x)*3+0];
				((qbyte*)buffer[0])[(y*width + x)*4+3] = 255;
			}
			break;
		default:
			fail = true;
			break;
		}
		BZ_Free(buf);


		cl.playerview->simangles[0] = 0;	//pitch is BAD
		cl.playerview->simangles[1] = baseang[1] - r_stereo_convergence.value*0.5;
		cl.playerview->simangles[2] = 0; //roll is BAD
		VectorCopy(cl.playerview->simangles, cl.playerview->viewangles);


		r_refdef.eyeoffset[0] *= -1;
		r_refdef.eyeoffset[1] *= -1;
		r_refdef.eyeoffset[2] = 0;
		buf = SCR_ScreenShot_Capture(width, height, &stride, &fmt, true, false);
		switch(fmt)
		{
		case TF_BGRA32:
			for (y = 0; y < height; y++)
			for (x = lx; x < rx; x++)
				((unsigned int*)buffer)[y*width + x] = ((unsigned int*)buf)[y*width + x];
			break;
		case TF_RGB24:
			for (y = 0; y < height; y++)
			for (x = lx; x < rx; x++)
			{
				((qbyte*)buffer[1])[(y*width + x)*4+0] = ((qbyte*)buf)[(y*width + x)*3+2];
				((qbyte*)buffer[1])[(y*width + x)*4+1] = ((qbyte*)buf)[(y*width + x)*3+1];
				((qbyte*)buffer[1])[(y*width + x)*4+2] = ((qbyte*)buf)[(y*width + x)*3+0];
				((qbyte*)buffer[1])[(y*width + x)*4+3] = 255;
			}
			break;
		default:
			fail = true;
			break;
		}
		BZ_Free(buf);
	}

	if (fail)
		Con_Printf ("Unable to capture suitable screen image\n");
	else if (SCR_ScreenShot(filename, FS_GAMEONLY, buffer, (stereo?2:1), stride, width, height*(stereo?1:2), TF_BGRX32, true))
	{
		char			displayname[1024];
		FS_DisplayPath(filename, FS_GAMEONLY, displayname, sizeof(displayname));
		Con_Printf ("Wrote %s\n", displayname);
	}

	BZ_Free(buffer[0]);

	r_projection.ival = r_projection.value;
	VectorCopy(baseang, r_refdef.viewangles);
	VectorClear(r_refdef.eyeoffset);
}

static void SCR_ScreenShot_Cubemap_Internal(vec3_t origin, int size, const char *fname, const char *fnamefmt, qboolean envmap)
{
	void *buffer;
	int stride, fbwidth, fbheight;
	uploadfmt_t fmt;
	char filename[MAX_QPATH];
	char displayname[1024];
	int i, firstside;
	char olddrawviewmodel[64];	//hack, so we can set r_drawviewmodel to 0 so that it doesn't appear in screenshots even if the csqc is generating new data.
	vec3_t oldangles;
	void *facedata;
	struct pendingtextureinfo mips;
	static const struct
	{
		vec3_t angle;
		const char *postfix;
		qboolean verticalflip;
		qboolean horizontalflip;
	} sides[] =
	{
		//standard cubemap (flipping is done on save)
		{{0, 0, 90}, "_px", true},
		{{0, 180, -90}, "_nx", true},
		{{0, 90, 0}, "_py", true},	//upside down
		{{0, 270, 0}, "_ny", false, true},
		{{-90, 0, 90}, "_pz", true},
		{{90, 0, 90}, "_nz", true},

		//annoying envmap (requires processing to flip/etc the images before they can be loaded into a texture)
		{{0, 270, 0}, "_ft"},
		{{0, 90, 0}, "_bk"},
		{{0, 0, 0}, "_rt"},
		{{0, 180, 0}, "_lf"},
		{{90, 0, 0}, "_dn"},
		{{-90, 0, 0}, "_up"}
	};
	const char *ext;
	unsigned int bb, bw, bh, bd;
	refdef_t oldrefdef;

	if (!cls.state || !cl.worldmodel || cl.worldmodel->loadstate != MLS_LOADED)
	{
		Con_Printf("Please start a map first\n");
		return;
	}

	firstside = envmap?6:0;

	oldrefdef = r_refdef;

	r_refdef.stereomethod = STEREO_OFF;
	Q_strncpyz(olddrawviewmodel, r_drawviewmodel.string, sizeof(olddrawviewmodel));
	Cvar_Set(&r_drawviewmodel, "0");

	if (origin)
	{
		r_refdef.externalview = true;
		r_refdef.forcevis = false;
		r_refdef.flags |= RDF_DISABLEPARTICLES;
		r_refdef.forcedvis = NULL;
		r_refdef.areabitsknown = false;
		r_refdef.sceneareas = NULL;
		r_refdef.fixedview = true;
		VectorCopy(origin, r_refdef.fixedvieworg);
		// r_refdef.fixedviewangles is set below for each screenshot
	}
	else
		origin = r_refdef.vieworg;

	VectorCopy(cl.playerview->viewangles, oldangles);

	fbheight = size&~1;
	if (fbheight < 1)
		fbheight = 512;
	fbwidth = fbheight;

	ext = COM_GetFileExtension(fname, NULL);
	if (!*fname || ext == fname)
	{	//generate a default filename if none exists yet.
		char base[MAX_QPATH];
		COM_FileBase(cl.worldmodel->name, base, sizeof(base));
		fname = va(fnamefmt, base, (int)origin[0], (int)origin[1], (int)origin[2]);
	}
	if (!strcmp(ext, ".ktx") || !strcmp(ext, ".dds"))
	{
		qboolean fail = false;
		mips.type = PTI_CUBE;
		mips.encoding = PTI_INVALID;
		mips.extrafree = NULL;
		mips.mipcount = 1;

		bb=0;
		for (i = 0; i < 6; i++)
		{
			if (r_refdef.fixedview)
				VectorCopy(sides[i].angle, r_refdef.fixedviewangles);
			else
			{
				VectorCopy(sides[i].angle, cl.playerview->aimangles);
				VectorCopy(cl.playerview->aimangles, cl.playerview->viewangles);
			}

			//don't use hdr when saving dds files. it generally means dx10 dds files and most tools suck too much and then I get blamed for writing 'corrupt' dds files.
			facedata = SCR_ScreenShot_Capture(fbwidth, fbheight, &stride, &fmt, true, !!strcmp(ext, ".dds"));
			if (!facedata)
				break;
			if (!bb)
			{
				Image_BlockSizeForEncoding(fmt, &bb, &bw, &bh, &bd);
				if (!bb || bw != 1 || bh != 1 || bd != 1 || fbwidth != fbheight)
				{	//erk, no block compression here...
					BZ_Free(facedata);
					break;	//zomgwtfbbq
				}
				mips.mip[0].datasize = bb*((fbwidth+bw-1)/bw)*((fbheight+bh-1)/bh);
				mips.mip[0].width = fbwidth;
				mips.mip[0].height = fbheight;
				mips.mip[0].depth = 6;
				mips.mip[0].datasize *= mips.mip[0].depth;
				mips.mip[0].data = BZ_Malloc(mips.mip[0].datasize);
				mips.mip[0].needfree = true;

				mips.encoding = fmt;
			}
			else if (fmt != mips.encoding || fbwidth != mips.mip[0].width || fbheight != mips.mip[0].height)
			{
				BZ_Free(facedata);
				break;	//zomgwtfbbq
			}

			Image_FlipImage(facedata, (qbyte*)mips.mip[0].data + i*(mips.mip[0].datasize/6), &fbwidth, &fbheight, bb, sides[i].horizontalflip, sides[i].verticalflip^(stride<0), false);
			BZ_Free(facedata);
		}
		if (i == 6)
		{
			if (mips.encoding == PTI_RGB32F || mips.encoding == PTI_RGBA16F || mips.encoding == PTI_RGBA32F)
			{	//convert to a more memory-efficient hdr format.
				qboolean pixelformats[PTI_MAX] = {0};
				pixelformats[PTI_E5BGR9] = true;
				Image_ChangeFormat(&mips, pixelformats, mips.encoding, fname);
			}
			else if ((mips.encoding == PTI_RGBX8 || mips.encoding == PTI_BGRX8) && !strcmp(ext, ".dds"))
			{	//gimp-dds plugin is buggy. convert to a less problematic format, so that I don't get invalid complaints.
				qboolean pixelformats[PTI_MAX] = {0};
				pixelformats[PTI_BGR8] = true;
				Image_ChangeFormat(&mips, pixelformats, mips.encoding, fname);
			}

			Q_snprintfz(filename, sizeof(filename), "textures/%s", fname);
			COM_DefaultExtension (filename, ext, sizeof(filename));

			ext = COM_GetFileExtension(filename, NULL);
			if (fail)
				Con_Printf("Unable to generate cubemap data\n");
#ifdef IMAGEFMT_DDS
			else if (!strcmp(ext, ".dds"))
			{
				if (Image_WriteDDSFile(filename, FS_GAMEONLY, &mips))
				{
					FS_DisplayPath(filename, FS_GAMEONLY, displayname, sizeof(displayname));
					Con_Printf ("Wrote %s\n", displayname);
				}
			}
#endif
#ifdef IMAGEFMT_KTX
			else if (!strcmp(ext, ".ktx"))
			{
				if (Image_WriteKTXFile(filename, FS_GAMEONLY, &mips))
				{
					FS_DisplayPath(filename, FS_GAMEONLY, displayname, sizeof(displayname));
					Con_Printf ("Wrote %s\n", displayname);
				}
			}
#endif
			else
				Con_Printf ("Unknown format %s\n", filename);
		}
		if (mips.mip[0].needfree)
			BZ_Free(mips.mip[0].data);
	}
	else
	{
		for (i = firstside; i < firstside+6; i++)
		{
			if (r_refdef.fixedview)
				VectorCopy(sides[i].angle, r_refdef.fixedviewangles);
			else
			{
				VectorCopy(sides[i].angle, cl.playerview->aimangles);
				VectorCopy(cl.playerview->aimangles, cl.playerview->viewangles);
			}

			buffer = SCR_ScreenShot_Capture(fbwidth, fbheight, &stride, &fmt, true, false);
			if (buffer)
			{
				Image_BlockSizeForEncoding(fmt, &bb, &bw, &bh, &bd);
				if (sides[i].horizontalflip)
				{
					int y, x, p;
					char *bad = buffer;
					char *in = buffer, *out;
					buffer = out = BZ_Malloc(fbwidth*fbheight*bb);
					for (y = 0; y < fbheight; y++, in += abs(stride), out += fbwidth*bb)
					{
						for (x = 0; x < fbwidth*bb; x+=bb)
						{
							for (p = 0; p < bb; p++)
								out[x+p] = in[(fbwidth-1)*bb-x+p];
						}
					}
					BZ_Free(bad);
					if (stride < 0)
						stride = -fbwidth*bb;
					else
						stride = fbwidth*bb;
				}
				if (sides[i].verticalflip)
					stride = -stride;

				Q_snprintfz(filename, sizeof(filename), "textures/%s%s", fname, sides[i].postfix);
				COM_DefaultExtension (filename, scr_sshot_type.string, sizeof(filename));

				if (SCR_ScreenShot(filename, FS_GAMEONLY, &buffer, 1, stride, fbwidth, fbheight, fmt, false))
				{
					FS_DisplayPath(filename, FS_GAMEONLY, displayname, sizeof(displayname));
					Con_Printf ("Wrote %s\n", displayname);
				}
				else
				{
					FS_DisplayPath(filename, FS_GAMEONLY, displayname, sizeof(displayname));
					Con_Printf ("Failed to write %s\n", displayname);
				}
				BZ_Free(buffer);
			}
		}
	}

	r_refdef = oldrefdef;

	Cvar_Set(&r_drawviewmodel, olddrawviewmodel);

	VectorCopy(oldangles, cl.playerview->viewangles);
}

void SCR_ScreenShot_Cubemap(vec3_t origin, int size)
{
	SCR_ScreenShot_Cubemap_Internal(origin, size, "", "env/%s_%i_%i_%i", false);
}

static void SCR_ScreenShot_Cubemap_f(void)
{
	SCR_ScreenShot_Cubemap_Internal(NULL, atoi(Cmd_Argv(2)), Cmd_Argv(1), "%s/%i_%i_%i", false);
}

static void SCR_ScreenShot_Envmap_f(void)
{
	SCR_ScreenShot_Cubemap_Internal(NULL, atoi(Cmd_Argv(2)), Cmd_Argv(1), "%s/%i_%i_%i", true);
}

#ifdef IMAGEFMT_PCX
// from gl_draw.c
qbyte		*draw_chars;				// 8*8 graphic characters

static void SCR_DrawCharToSnap (int num, qbyte *dest, int width)
{
	int		row, col;
	qbyte	*source;
	int		drawline;
	int		x;

	if (!draw_chars)
	{
		size_t lumpsize;
		qbyte lumptype;
		draw_chars = W_GetLumpName("conchars", &lumpsize, &lumptype);
//		if (lumptype != )
//			draw_chars = NULL;
		if (!draw_chars || lumpsize != 128*128)
			return;
	}

	row = num>>4;
	col = num&15;
	source = draw_chars + (row<<10) + (col<<3);

	drawline = 8;

	while (drawline--)
	{
		for (x=0 ; x<8 ; x++)
			if (source[x] && source[x]!=255)
				dest[x] = source[x];
		source += 128;
		dest -= width;
	}

}

static void SCR_DrawStringToSnap (const char *s, qbyte *buf, int x, int y, int width)
{
	qbyte *dest;
	const unsigned char *p;

	dest = buf + ((y * width) + x);

	p = (const unsigned char *)s;
	while (*p) {
		SCR_DrawCharToSnap(*p++, dest, width);
		dest += 8;
	}
}


/*
==================
SCR_RSShot
==================
*/
int MipColor(int r, int g, int b);
qboolean SCR_RSShot (void)
{
	int stride;
	int truewidth;
	int trueheight;

	int     x, y;
	unsigned char		*src, *dest;
	unsigned char		*newbuf;
	int w, h;
	int dx, dy, dex, dey, nx;
	int r, b, g;
	int count;
	float fracw, frach;
	char st[80];
	time_t now;
	enum uploadfmt fmt;
	int src_size;
	int src_red;
	int src_green;
	int src_blue;

	if (!scr_allowsnap.ival)
		return false;

	if (CL_IsUploading())
		return false; // already one pending

	if (cls.state < ca_onserver)
		return false; // gotta be connected

	if (!VID_GetRGBInfo || !scr_initialized)
	{
		return false;
	}

	Con_Printf("Remote screen shot requested.\n");

//
// save the pcx file
//
	newbuf = VID_GetRGBInfo(&truewidth, &trueheight, &stride, &fmt);

	if (fmt == TF_INVALID)
		return false;
	switch(fmt)
	{
	case TF_RGB24:
	case TF_RGBA32:
		src_size = (fmt==TF_RGB24)?3:4;
		src_red = 0;
		src_green = 1;
		src_blue = 2;
		break;
	case TF_BGR24:
	case TF_BGRA32:
		src_size =  (fmt==TF_BGR24)?3:4;
		src_red = 2;
		src_green = 1;
		src_blue = 0;
		break;
	default:
		BZ_Free(newbuf);
		return false;
	}
	//FIXME: bgra

	w = RSSHOT_WIDTH;
	h = RSSHOT_HEIGHT;

	fracw = (float)truewidth / (float)w;
	frach = (float)trueheight / (float)h;

	//scale down first.
	for (y = 0; y < h; y++) {
		dest = newbuf + (w*src_size * y);

		for (x = 0; x < w; x++) {
			r = g = b = 0;

			dx = x * fracw;
			dex = (x + 1) * fracw;
			if (dex == dx) dex++; // at least one
			dy = y * frach;
			dey = (y + 1) * frach;
			if (dey == dy) dey++; // at least one

			count = 0;
			for (/* */; dy < dey; dy++) {
				src = newbuf + (truewidth * src_size * dy) + dx * src_size;
				for (nx = dx; nx < dex; nx++) {
					r += src[src_red];
					g += src[src_green];
					b += src[src_blue];
					src += src_size;
					count++;
				}
			}
			r /= count;
			g /= count;
			b /= count;
			*dest++ = r;
			*dest++ = g;
			*dest++ = b;
		}
	}

	// convert to eight bit
	for (y = 0; y < h; y++) {
		src = newbuf + (w * src_size * y);
		dest = newbuf + (w * y);

		for (x = 0; x < w; x++) {
			*dest++ = MipColor(src[0], src[1], src[2]);
			src += src_size;
		}
	}

	time(&now);
	strcpy(st, ctime(&now));
	st[strlen(st) - 1] = 0;
	SCR_DrawStringToSnap (st, newbuf, w - strlen(st)*8, h - 1, w);

	Q_strncpyz(st, cls.servername, sizeof(st));
	SCR_DrawStringToSnap (st, newbuf, w - strlen(st)*8, h - 11, w);

	Q_strncpyz(st, name.string, sizeof(st));
	SCR_DrawStringToSnap (st, newbuf, w - strlen(st)*8, h - 21, w);

	WritePCXfile ("snap.pcx", FS_GAMEONLY, newbuf, w, h, w, host_basepal, true);

	BZ_Free(newbuf);

	return true;
}
#endif

//=============================================================================


//=============================================================================

/*
===============
SCR_BringDownConsole

Brings the console down and fades the palettes back to normal
================
*/
void SCR_BringDownConsole (void)
{
	int pnum;

	for (pnum = 0; pnum < cl.splitclients; pnum++)
	{
		while (SCR_CenterPrintPop(pnum))
			;
		cl.playerview[pnum].cshifts[CSHIFT_CONTENTS].percent = 0;              // no area contents palette on next frame
	}
}

void SCR_TileClear (int skipbottom)
{
	if (r_refdef.vrect.width < r_refdef.grect.width)
	{
		float w;
		// left
		R2D_TileClear (r_refdef.grect.x, r_refdef.grect.y, r_refdef.vrect.x-r_refdef.grect.x, r_refdef.grect.height - skipbottom);
		// right
		w = (r_refdef.grect.x+r_refdef.grect.width) - (r_refdef.vrect.x+r_refdef.vrect.width);
		R2D_TileClear ((r_refdef.grect.x+r_refdef.grect.width) - (w), r_refdef.grect.y, w, r_refdef.grect.height - skipbottom);
	}
	if (r_refdef.vrect.height < r_refdef.grect.height)
	{
		// top
		R2D_TileClear (r_refdef.vrect.x, r_refdef.grect.y,
			r_refdef.vrect.width,
			r_refdef.vrect.y - r_refdef.grect.y);
		// bottom
		R2D_TileClear (r_refdef.vrect.x,
			r_refdef.vrect.y + r_refdef.vrect.height,
			r_refdef.vrect.width,
			(r_refdef.grect.y+r_refdef.grect.height) - skipbottom - (r_refdef.vrect.y + r_refdef.vrect.height));
	}
}



// The 2d refresh stuff.
void SCR_DrawTwoDimensional(qboolean nohud)
{
	qboolean consolefocused = !!Key_Dest_Has(kdm_console|kdm_cwindows);
	RSpeedMark();

	R2D_ImageColours(1, 1, 1, 1);

	//
	// draw any areas not covered by the refresh
	//
	if (r_netgraph.value)
		R_NetGraph ();

	if (scr_drawloading || loading_stage)
	{
		SCR_DrawLoading(false);

		SCR_ShowPics_Draw();

		if (!scr_disabled_for_loading)
			consolefocused = false;
	}
	else if (nohud)
	{
		SCR_DrawDisk();
		SCR_DrawFPS ();
		SCR_CheckDrawCenterString ();
	}
	else if (cl.intermissionmode == IM_NQFINALE || cl.intermissionmode == IM_NQCUTSCENE || cl.intermissionmode == IM_H2FINALE)
	{
		SCR_CheckDrawCenterString ();
	}
	else if (cl.intermissionmode != IM_NONE)
	{
		Sbar_IntermissionOverlay (r_refdef.playerview);
	}
	else
	{
		SCR_DrawNet ();
		SCR_DrawDisk();
		SCR_DrawFPS ();
		SCR_DrawClock();
		SCR_DrawGameClock();
		SCR_DrawTurtle ();
		SCR_DrawPause ();
		SCR_ShowPics_Draw();
		SCR_CheckDrawCenterString ();
	}

//#ifdef TEXTEDITOR
//	if (editoractive)
//		Editor_Draw();
//#endif

	//if the console is not focused, show it scrolling back up behind the menu
	if (!consolefocused)
		SCR_DrawConsole (false);

	Menu_Draw();

	//but if the console IS focused, then always show it infront.
	if (consolefocused)
		SCR_DrawConsole (false);

	Prompts_Draw();

#ifdef RTLIGHTS
	//nettest: r_shadows_propshadows_showatlas debug view (no-op unless set).  Drawn LAST so it stays on
	//top of menus/console -- it's a debug instrument, occluding it defeats the point.
	Sh_DrawFakeShadowAtlasOverlay();
#endif

	SCR_DrawCursor();
	SCR_DrawSimMTouchCursor();
	{	/*FTESurf Patch 667: over the cursor and the overlay above, which draw over prompts (pr_clcmd.c)*/
		void CL_Paste_DrawLast(void);
		CL_Paste_DrawLast();
	}

	if (R2D_Flush)
		R2D_Flush();

	RSpeedEnd(RSPEED_2D);
}






/*
==================
SCR_Init
==================
*/
void SCR_Init (void)
{
//
// register our commands
//
	Cmd_AddCommandD ("screenshot_mega",SCR_ScreenShot_Mega_f, "screenshot_mega <name> [width] [height]\nTakes a screenshot with explicit sizes that are not tied to the size of your monitor, allowing for true monstrosities.");
	Cmd_AddCommandD ("screenshot_stereo",SCR_ScreenShot_Mega_f, "screenshot_stereo <name> [width] [height]\nTakes a simple stereo screenshot.");
	Cmd_AddCommandD ("screenshot_360",SCR_ScreenShot_Mega_f, "screenshot_360 <name> [width] [height]\nTakes an equirectangular screenshot.");
	Cmd_AddCommandD ("screenshot_vr",SCR_ScreenShot_VR_f, "screenshot_vr <name> [width]\nTakes a spherical stereoscopic panorama image, for viewing with VR displays.");
	Cmd_AddCommandD ("screenshot_cubemap",SCR_ScreenShot_Cubemap_f, "screenshot_cubemap <name> [size]\nTakes 6 screenshots forming a single cubemap.");
	Cmd_AddCommandD ("envmap",SCR_ScreenShot_Envmap_f, "Legacy name for the screenshot_cubemap command.");	//legacy 
	Cmd_AddCommand ("screenshot",SCR_ScreenShot_f);
	Cvar_Register (&r_budget, "Screen options");	//FTESurf Patch 782
	Cvar_Register (&r_budget_window, "Screen options");
	Cvar_Register (&r_budget_csqc, "Screen options");	//FTESurf Patch 800
	Cvar_Register (&r_frametime, "Screen options");	//FTESurf Patch 801
	Cvar_Register (&r_frametime_seconds, "Screen options");
	Cvar_Register (&r_frametime_window, "Screen options");
	Cmd_AddCommandD ("r_frametime_dump", R_FrameTime_Dump_f, "FTESurf Patch 801. Prints r_frametime's lows, percentiles and slowest frames (with a setpos for each) and copies them to the clipboard.");
	Cmd_AddCommandD ("r_frametime_reset", R_FrameTime_Reset_f, "FTESurf Patch 801. Starts r_frametime's window again.");
	Cmd_AddCommandD ("r_budget_dump", R_Budget_Dump_f, "FTESurf Patch 782. Prints r_budget's breakdown as text and copies it to the clipboard.");
	Cmd_AddCommandD ("r_speeds_dump", RSpeed_Dump_f, "Prints the current r_speeds 2 sample table to the console as text, so it can be logged with -condebug and reconciled arithmetically instead of read off a screenshot.");

	scr_net = R2D_SafePicFromWad ("net");
	scr_turtle = R2D_SafePicFromWad ("turtle");

	scr_initialized = true;
}

void SCR_DeInit (void)
{
	int i;
	if (scr_curcursor)
	{
		rf->VID_SetCursor(scr_curcursor);
		scr_curcursor = NULL;
	}
	for (i = 0; i < countof(key_customcursor); i++)
	{
		if (key_customcursor[i].handle)
		{
			rf->VID_DestroyCursor(key_customcursor[i].handle);
			key_customcursor[i].handle = NULL;
		}
		key_customcursor[i].dirty = true;
	}
	for (i = 0; i < countof(scr_centerprint); i++)
	{
		while (SCR_CenterPrintPop(i))
			;
	}
	if (scr_initialized)
	{
		scr_initialized = false;

		Cmd_RemoveCommand ("screenshot");
		Cmd_RemoveCommand ("screenshot_mega");
		Cmd_RemoveCommand ("screenshot_stereo");
		Cmd_RemoveCommand ("screenshot_vr");
		Cmd_RemoveCommand ("screenshot_360");
		Cmd_RemoveCommand ("screenshot_cubemap");
		Cmd_RemoveCommand ("envmap");
	}
}
