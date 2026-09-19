//Generic input code.
//mostly mouse support, but can also handle a few keyboard events.

//Issues:

//VirtualBox mouse integration is bugged. X11 code can handle tablets, but VirtualBox sends mouse clicks on the ps/2 device instead.
//  you should be able to fix this with 'in_deviceids * 0 *', remapping both tablet+ps/2 to the same device id. 

//Android touchscreen inputs suck. should have some onscreen buttons, but they're still a bit poo or something. blame mods for not using csqc to do things themselves.

#include "quakedef.h"

extern qboolean mouse_active;

static cvar_t m_filter = CVARF("m_filter", "0", CVAR_ARCHIVE);
static cvar_t m_forcewheel = CVARD("m_forcewheel", "1", "0: ignore mousewheels in apis where it is abiguous.\n1: Use mousewheel when it is treated as a third axis. Motion above a threshold is ignored, to avoid issues with an unknown threshold.\n2: Like 1, but excess motion is retained. The threshold specifies exact z-axis distance per notice.");
static cvar_t m_forcewheel_threshold = CVARD("m_forcewheel_threshold", "32", "Mousewheel graduations smaller than this will not trigger mousewheel deltas.");
static cvar_t m_touchstrafe = CVARAFD("m_touchstrafe", "0", "m_strafeonright", CVAR_ARCHIVE, "0: entire screen changes angles only.\n1: right hand side controls strafing.\n2:left hand side strafes.");
//static cvar_t m_fatpressthreshold = CVARFD("m_fatpressthreshold", "0.2", CVAR_ARCHIVE, "How fat your thumb has to be to register a fat press (touchscreens).");
static cvar_t m_longpressthreshold = CVARFD("m_longpressthreshold", "1", CVAR_ARCHIVE, "How long to press for it to register as a long press (touchscreens).");
static cvar_t m_touchmajoraxis = CVARFD("m_touchmajoraxis", "1", CVAR_ARCHIVE, "When using a touchscreen, use only the major axis for strafing.");
static cvar_t m_slidethreshold = CVARFD("m_slidethreshold", "10", CVAR_ARCHIVE, "How far your finger needs to move to be considered a slide event (touchscreens).");

static cvar_t m_accel			= CVARAFD("m_accel",		"0",	"cl_mouseAccel", CVAR_ARCHIVE, "Values >0 will amplify mouse movement proportional to velocity. Small values have great effect. A lot of good Quake Live players use around the 0.1-0.2 mark, but this depends on your mouse CPI and polling rate.");
static cvar_t m_accel_style		= CVARAD("m_accel_style",	"1",	"cl_mouseAccelStyle",	"1 = Quake Live mouse acceleration, 0 = Old style accelertion.");
static cvar_t m_accel_power		= CVARAD("m_accel_power",	"2",	"cl_mouseAccelPower",	"Used when m_accel_style is 1.\nValues 1 or below are dumb. 2 is linear and the default. 99% of accel users use this. Above 2 begins to amplify exponentially and you will get more acceleration at higher velocities. Great if you want low accel for slow movements, and high accel for fast movements. Good in combination with a sensitivity cap.");
static cvar_t m_accel_offset	= CVARAD("m_accel_offset",	"0",	"cl_mouseAccelOffset",	"Used when m_accel_style is 1.\nAcceleration will not be active until the mouse movement exceeds this speed (counts per millisecond). Negative values are supported, which has the effect of causing higher rates of acceleration to happen at lower velocities.");
static cvar_t m_accel_senscap	= CVARAD("m_accel_senscap",	"0",	"cl_mouseSensCap",		"Used when m_accel_style is 1.\nSets an upper limit on the amplified mouse movement. Great for tuning acceleration around lower velocities while still remaining in control of fast motion such as flicking.");

/*FTESurf Patch 202: the raw input journal.  The cvar lives up here with the rest of
  this file's cvars; the machinery it belongs to is a long way down, just above
  IN_Commands, because that is where it hooks in.*/
/*FTESurf Patch 295: 8192 -> 262144.  The cap is a backstop now, not a budget.

  A truncated journal is a THIRD state for the submission rule to carry: "no
  .hid, no rank" is one condition, and a file that is present but stops early is
  neither present nor absent.  Every consumer -- the submission gate, hidcheck,
  the auditor reading it -- would need a branch for it, and the first time it
  fired would be on someone's long run, i.e. exactly when it costs the most.
  Cheaper to make it not happen: at ~1.15 MB/min of events plus the Patch 293 'v'
  records, 256 MB is about 85 minutes, which no real run reaches.

  It costs nothing to raise.  in_jrn_cap starts at 65536 and DOUBLES on demand
  (IN_Journal_Raw), bounded by in_jrn_max -- so the allocation tracks what is
  actually written and a short run still holds 64 KB.  What does scale with it is
  the synchronous COM_WriteFile at run end (see the essay below): 8192 held that
  to about 40 ms, so a journal that genuinely reached this cap would hitch for
  something over a second.  That only happens on a run long enough that the
  truncation would have been worse.*/
static cvar_t in_journal_maxkb	= CVARD("in_journal_maxkb", "262144", "Memory cap on a raw input journal, in kilobytes. About 1.15 MB per minute at a 1000 Hz polling rate, so the default is roughly 85 minutes; past that the file records a 'truncated' marker and stops. Sized so a real run never truncates -- a truncated journal is a state the run-submission rule would otherwise have to carry.");
static void IN_JournalBegin_f(void);
static void IN_JournalEnd_f(void);
static void IN_JournalNote_f(void);
static void IN_JournalSynth_f(void);

/*
FTESurf Patch 301 -- WHAT THE INPUT STACK ACTUALLY GAVE US, not what was asked for.

The .hid header has always recorded `rawinput` and `rawkbd` from the CVARS.  A cvar is
a request.  It is granted by INS_RawInput_Init enumerating devices and binding them, and
that can fail while the cvar still reads 1: no enumerable mouse, an RDP session (every
device is the Terminal Services virtual one, excluded unless in_rawinput_rdp), a
user32 export missing, or a RegisterRawInputDevices that simply refused.  In every one of
those cases the engine falls back to INS_Accumulate's GetCursorPos/SetCursorPos recentre
-- ONE already-OS-summed, OS-ACCELERATED delta per call, about twice a frame -- and the
header said `rawinput 1` over the top of it.

That is a false statement in an evidence file, and it is the worst kind: it is false
exactly in the sessions where the data is least trustworthy, so it reads as a clean run
recorded under the strict profile.  A reader checking Patch 293's yaw identity against
such a file is checking arithmetic against OS-accelerated deltas and cannot tell.

So the two numbers below carry the GRANT beside the request.  Both keys are kept --
`rawinput` is still the cvar, `rawmice` is what it got -- because they answer different
questions and a reader that only has one of them is missing half the picture: cvar 0 with
a live count is impossible, cvar 1 with a count of 0 is the RDP case, and cvar 0 with a
count of 0 is simply a player who turned it off.

THREE STATES, AND -1 IS NOT ZERO.  -1 means this platform's backend does not report the
figure -- in_generic.c is the cross-platform file and in_win.c is one of several backends,
so an SDL or X11 build never touches these and must not be described as having found no
mouse.  Zero is a measurement ("raw input ran and bound nothing"); -1 is the absence of
one.  Collapsing them would put a fault on every non-Windows journal, and a reader that
treats -1 as 0 would conclude the strictest thing about the least evidence.

Defined HERE rather than in in_win.c because in_generic.o is in CLIENT_OBJS (every client
build has it) while in_win.o is Windows-only; a backend that never assigns them leaves the
honest -1 standing.  in_win.c maintains them at three sites, all marked Patch 301.
*/
int in_rawmice_live = -1;
int in_rawkbd_live = -1;

/*
FTESurf Patch 326 -- THE GRANT, WHERE QC CAN READ IT.

Patch 301 put these two numbers in the .hid header, which answers the question AFTER the
fact, for a human auditor holding the file.  Nothing could ask them DURING a run, and the
ranked input profile -- the userinfo key the server latches TF_NOPROFILE from -- therefore
published `in_rawinput`, the request, with no way to publish the grant beside it.

THAT IS A LIVE BYPASS AND IT WAS MEASURED, not inferred.  in_win.c reads in_rawinput_mice
ONLY inside INS_RawInput_Init, so the cvar and the enumeration can be made to disagree and
then left that way:

    in_rawinput 0 ; in_restart ; in_rawinput 1

re-enumerates with raw input off, then sets the cvar back without re-enumerating.  The cvar
reads 1, rawmicecount is 0, INS_Accumulate's GetCursorPos/SetCursorPos fallback is live, and
injected SendInput motion lands in full.  Measured over four arms in one process at a strafe
optimiser's own magnitude: with raw input genuinely live, 16,800 injected counts moved the
view 0.022 degrees and every event was counted as rejected; after the sequence above, the
same injection moved it 232 degrees and the server still reported the run rankable.

THE POINT IS THAT IT NEEDS NO LIE.  The client reports its cvar honestly; the fact that
decides whether the evidence means anything simply was not on the wire.  Everything else in
that key is a claim a patched client could forge, and this was not even that.  So the fix is
not a new check, it is publishing a number the engine has had all along.

A CVAR RATHER THAN A NEW CHANNEL, because CSQC already reads in_rawinput and m_accel through
cvar_type()/cvar() and this is the same kind of fact about the same input stack.  CVAR_NOSET
so a console cannot assign it -- Cvar_ForceSetValue is the only writer, on cl_servername's
precedent -- and CVAR_NOSAVE because it is derived state that must never come back from a
config file describing a previous session's hardware.

PUBLISHED FROM IN_Commands, WHICH IS THE CROSS-PLATFORM PER-FRAME PUMP, AND DELIBERATELY NOT
FROM in_win.c's THREE ASSIGNMENT SITES.  Mirroring at the assignment sites is the cheaper
edit and it is the one that rots: the mirror would be correct until somebody adds a fourth
site or a second backend starts reporting, and it would then go stale IN THE DIRECTION THAT
RANKS A RUN -- a cvar still reading the last good count while the grant had gone to zero.
Reading the variable once a frame from the file that DECLARES it cannot miss a writer.

THREE STATES, AND -1 IS STILL NOT ZERO.  The default is "-1", so a backend that never
assigns in_rawmice_live publishes the honest unknown rather than a measurement of nothing --
which is the whole reason these variables live in in_generic.c and not in in_win.c.  QC
reads 0 as a fault and -1 as unknown, and unknown stays rankable.
*/
static cvar_t in_rawmice = CVARFD("in_rawmice", "-1", CVAR_NOSET|CVAR_NOSAVE, "Read-only: how many mice raw input actually enumerated and bound, as against in_rawinput, which is only the request. -1 means this platform's input backend does not report the figure; 0 means raw input ran and bound nothing, so mouse motion is taking the OS-summed, OS-accelerated legacy path.");
static cvar_t in_rawkbds = CVARFD("in_rawkbds", "-1", CVAR_NOSET|CVAR_NOSAVE, "Read-only: how many keyboards raw input actually enumerated and bound, as against in_rawinput_keyboard. -1 means this backend does not report the figure; 0 means none are bound and keystrokes are taking the legacy WM_KEYDOWN path.");

/*FTESurf Patch 306: THE REPORTS RAW INPUT THREW AWAY.

INS_RawInput_MouseRead matches raw->header.hDevice against the enumerated device table
and RETURNS if it is not there (in_win.c), and INS_RawInput_KeyboardRead does the same.
That drop is the defence -- it is why an injected SendInput motion never reaches the
view -- and it is completely SILENT.  So an audit of a .hid cannot today tell

    nobody injected anything

apart from

    somebody injected forty thousand events and the engine discarded every one.

Those two sessions produce byte-identical journals.  The defence working and the attack
never happening look the same, which means the ONE piece of evidence a defeated attempt
leaves behind is being deleted at the moment it is generated.  Counting it costs two ints
and one branch on a path that was already returning.

TWO COUNTERS, BECAUSE THEY ARE TWO FINDINGS AND COLLAPSING THEM WOULD ACCUSE.

  in_raw_injected  a report with NO DEVICE HANDLE AT ALL (hDevice == NULL).  That is
                   what the OS hands us for synthesized input -- SendInput and the
                   journal-playback hooks -- and there is no innocent reading of it.

  in_raw_unenum    a report carrying a REAL handle that is not in our table.  A mouse
                   plugged in after INS_RawInput_Init enumerated, or a Terminal Services
                   device excluded because in_rawinput_rdp is 0.  Entirely innocent, and
                   ALSO a live usability bug: that device's motion is going nowhere.

A reader given one number could not separate "you were attacked" from "you hot-plugged a
mouse", and the first of those is an accusation about a person.  They stay apart.

WHAT THIS DOES NOT PROVE, and the limits are larger than the counter.

  - It says the injection was REJECTED, not that it was attempted and succeeded.  These
    events did not reach the view; that is the whole reason they are countable.  A
    non-zero count is evidence of an attempt that FAILED.
  - It is blind to the two holes that are actually open.  Injected mouse BUTTONS still
    arrive as legacy WM_*BUTTONDOWN, which in_win.c keeps deliberately (the comment at
    the RIDEV_NOLEGACY line says click-state tracking needs them), and with
    in_rawinput_keyboard 0 every keystroke is legacy.  Neither path passes through the
    handle check, so neither is counted here.  This counter watches the door that is
    shut, not the two that are open.
  - A hardware replay device (Arduino/Pico/KMBox) enumerates as a genuine mouse and is
    counted as neither.  It is indistinguishable here by construction.

So this is a tripwire, honestly sized: it catches the unmodified public injector, and it
turns a successful defence into a recorded one.  It is not a detector and nothing may
auto-accuse from it.

THREE STATES, AND -1 IS NOT ZERO -- the same rule as in_rawmice_live above, for the same
reason.  -1 means this backend does not count; 0 is a measurement.  Defined here because
in_generic.o is in every client build while in_win.o is Windows-only, so a backend that
never assigns them leaves the honest -1 standing.  in_win.c lifts them to 0 when raw
input actually binds, and they are monotone from there -- never reset, because the
journal reports DELTAS against a baseline it took at begin, and a counter that restarts
would hand it a negative one.*/
int in_raw_injected = -1;
int in_raw_unenum = -1;

/*FTESurf Patch 307: THE BUTTON THAT GOT THROUGH.

Patch 306 counts reports raw input REJECTED.  This counts one that it ACCEPTED without
being able to corroborate, and that difference is the whole point: nothing 306 counts
reached the game, while every one of these became a real K_MOUSE1..5 and was acted on.

THE HOLE, and it is in shipped code rather than hypothetical.  in_win.c registers mouse
raw input with dwFlags 0 -- RIDEV_NOLEGACY is written out and deliberately not used --
so legacy WM_*BUTTONDOWN still flows alongside WM_INPUT.  An injected click's WM_INPUT
is dropped at the hDevice check, which sits BEFORE the button stamping, so
rawbuttondown[] is never set and rawbuttontime[] stays stale; INS_MouseEvent's dedupe
then sees a legacy press that raw did not account for and passes it through as genuine.
That is enough for scroll-bhop, for jump timing, and for the gate the sample cheat uses.

WHY THE LEGACY PATH CANNOT SIMPLY BE CLOSED, measured and written down in this tree
before this patch existed (see the essay on the dedupe in INS_MouseEvent): a Windows
precision touchpad is a HID DIGITIZER, not a RIM_TYPEMOUSE, so it produces no
RI_MOUSE_BUTTON_* flags at all and the legacy message is the ONLY press it has.  Closing
the path costs those users every mouse button, for taps and for press-and-hold alike, and
it presents as a dead hit-test rather than as missing input -- which is what made it hard
to find the first time.  So the block is real but it is a TRADE, and it ships as an
opt-in cvar rather than as a new default.

WHICH MAKES COUNTING THE PRIMARY DEFENCE HERE, not the consolation prize.  The count is
only ambiguous on a machine that has a non-RIM_TYPEMOUSE pointer.  The Patch 303 device
table says whether one exists, so a reader holding both can separate the two cases: N
uncorroborated presses on a session that enumerated two ordinary mice and nothing else is
not a touchpad, and on the overwhelming majority of ranked machines this number is 0 for
honest play by construction.

WHAT IT STILL CANNOT SAY.  It cannot name the cause -- an on-screen keyboard, a remote
desktop session, accessibility software and some vendor mouse utilities all click through
the legacy path legitimately.  It is a NOTE and a reason to look, never an accusation,
and hidcheck must treat it that way.

-1 is "this backend does not count", as for the two above.*/
int in_raw_legacybtn = -1;

/*FTESurf Patch 307: the EFFECTIVE suppression state, beside the cvar in the header, for
  exactly the reason Patch 301 put in_rawmice_live beside in_rawinput: the cvar is a
  request and this is what happened.  in_rawinput_nolegacy 1 does nothing at all while
  the mouse is ungrabbed (the setting is grab-scoped on purpose -- see in_win.c), and it
  does nothing if the re-registration is refused.  A header that printed only the cvar
  would describe a run as protected when it was not, which is the same false statement in
  the same file that 301 exists to prevent.
    -1  no registration, or a backend that does not report
     0  legacy mouse messages are being delivered -- the injected-click path is OPEN
     1  suppressed*/
int in_raw_nolegacy_live = -1;

/*FTESurf Patch 310: THE SETTINGS THAT CHANGE WHAT THE PLAYER COULD SEE.

Patches 293/301/303 put the input pipeline's constants in this file because a record of
mouse counts proves nothing without the numbers that turn them into an angle.  The same
argument applies to the renderer and had not been made: a run recorded with the fog
switched off, or the world drawn fullbright, is a different run from the one the .hid
currently describes, and nothing in the evidence said so.

r_fog_progless 0 is the concrete case.  It restores the pre-Patch-266 behaviour in which
program-less surfaces took no distance fog -- which on a fogged map means seeing through
the fog on a large share of the world.  It is CVAR_ARCHIVE and NOT CVAR_CHEAT, so it
survives a restart and no server gate touches it.  Worse, r_fullbright and r_drawflat
carry NO FLAGS AT ALL.

WHY RECORD RATHER THAN BLOCK.  Blocking needs a client-cvar enforcement channel this game
does not have yet, and a block with no record is unfalsifiable after the fact -- an
auditor still could not tell what a run was played with.  Recording is the half that can
ship today, it composes with a gate later, and it is the same architecture as every other
column here: the file states what happened and the validator decides what that means.

BOTH THE VALUE AND THE DEFAULT, on one line, for the P293 reason: a reader that knows only
the value needs its own table of what is normal, that table drifts from the engine's, and
the check silently starts measuring the wrong thing.  Writing `defaultstr` beside the
value makes "differs from default" answerable with no model in the reader at all, and it
stays correct when a default changes.

CHANGES TOO, not just a snapshot -- the lesson Patch 307 paid for.  These are ordinary
cvars a console can set mid-run.  Tracked by modifiedcount rather than by comparing
values, which is both cheaper and STRICTER: it catches a set-and-set-back that a value
comparison would report as nothing having happened.

`-` where the cvar does not exist in this build.  Not 0, which is a real value.*/
static const char *in_jrn_rendercvars[] =
{
	"r_fog_progless",			/*0 = program-less surfaces take no distance fog*/
	"r_fog_linear", "r_fog_exp2",	/*change the fog curve, so how far you see*/
	"r_fullbright",				/*no flags at all -- removes lighting entirely*/
	"r_drawflat",				/*no flags -- flat colours, no texture detail*/
	"r_drawentities",			/*no flags*/
	"r_novis",					/*ARCHIVE -- draws leaves the PVS excluded*/
	"r_lightmap_saturation",
	"r_shadow_realtime_world",	/*a different lighting model altogether*/
	"r_wireframe", "r_showbboxes",	/*CVAR_CHEAT, so recorded to CONFIRM they were off*/
	NULL
};
#define MAX_JRN_RENDERCVARS 16

/*FTESurf Patch 312: THE SETTINGS THAT CHANGE WHAT THE COUNTS BECOME.

in_xflip is the one this patch had to have.  in_generic.c does `if(in_xflip.value) mx *= -1`
one line after the counts leave the ring and long before either journal tap, and the cvar
appeared NOWHERE in the .hid.  A player with it set therefore produced v.dx == -sum(m.dx)
on every single frame -- a 100% failure of this patch's own check, from a cvar that is
neither CVAR_CHEAT nor even CVAR_ARCHIVE.  MEASURED, arm B of the falsifier: identical
synthetic input gave v.dx -157.5 against the control's +157.5.  Accusing that player is
the Patch 305 failure mode exactly, and 305 cost 10,091 frames of a clean PB to learn.

THE REST OF THE TABLE CLOSES A HOLE IN PATCH 293, not in this one, and it is here because
it is the same two lines of machinery.  293 records sensitivity, m_yaw, m_filter and the
m_accel family in the HEADER -- a snapshot taken at in_journal_begin.  They are ordinary
cvars a console can set mid-run, and if one moves, 293's identity stops closing with
nothing in the file to say why.  A reader would see the flagship check fail and have no
way to tell a cheat from a player who nudged their sensitivity between attempts.  These
stay in the header too, because six shipped recordings and hidcheck already read them
there; the line here adds the DEFAULT beside the value and, more importantly, enrols them
in the modifiedcount tracking that emits a 'c' record when one moves.

That is the same lesson for the sixth time -- 301 cvar vs grant, 306 not-counted vs
counted-zero, 307 true-at-the-start vs true-throughout, 310 snapshot vs change, 312's own
'a'-record span problem, and now this.  A snapshot cannot describe something that changes.*/
static const char *in_jrn_inputcvars[] =
{
	"in_xflip",					/*mx *= -1, upstream of both journal taps*/
	"sensitivity", "m_yaw", "m_pitch",	/*the P293 identity's own constants...*/
	"m_filter", "m_accel",				/*...and the two that make it inexact*/
	"m_accel_style", "m_accel_power", "m_accel_offset", "m_accel_senscap",
	"m_forcewheel", "m_forcewheel_threshold",	/*turn wheel motion into keypresses*/
	/*Found by the red-team pass over this patch, all three invisible until now:*/
	"leftisright",				/*r_xflip.  renderer.c declares it `cvar_t r_xflip =
							  CVAR("leftisright", ...)`, so the C identifier and the
							  console name differ -- the same alias trap Patch 310
							  hit with r_wireframe/r_showtris.  It inverts the
							  Patch 293 identity's SIGN downstream of both taps,
							  and nothing in the file said so.*/
	"cl_threadedphysics",		/*runs this whole pipeline on a second thread with
							  the drain outside the lock: the one setting that can
							  reorder the two ends of the window being compared*/
	"dpcompat_csqcinputeventtypes",	/*gates CSQC_MouseMove, i.e. whether the mod's
							  own progs may swallow the delta.  Defaults 999999 =
							  always on, and this game ships CSQC panels.*/
	NULL
};
#define MAX_JRN_TRACKEDCVARS 48

void QDECL joyaxiscallback(cvar_t *var, char *oldvalue)
{
	int sign;
	char *end;
	sign = strtol(var->string, &end, 0);
	if (!*end)
	{
		//okay, its missing or an actual number.
		if (sign >= -6  &&  sign <= 6) {
			var->ival = sign;
		}
		return;
	}

	end = var->string;
	if (*end == '-')
	{
		end++;
		sign = -1;
	}
	else if (*end == '+')
	{
		end++;
		sign = 1;
	}
	else
		sign = 1;
	if (!Q_strcasecmp(end, "forward") || !Q_strcasecmp(end, "moveforward"))
		var->ival = 1*sign;
	else if (!Q_strcasecmp(end, "back") || !Q_strcasecmp(end, "moveback"))
		var->ival = 1*sign*-1;
	else if (!Q_strcasecmp(end, "lookup") || !Q_strcasecmp(end, "pitchup"))
		var->ival = 2*sign;
	else if (!Q_strcasecmp(end, "lookdown") || !Q_strcasecmp(end, "pitchdown"))
		var->ival = 2*sign*-1;
	else if (!Q_strcasecmp(end, "moveright"))
		var->ival = 3*sign;
	else if (!Q_strcasecmp(end, "moveleft"))
		var->ival = 3*sign*-1;
	else if (!Q_strcasecmp(end, "right") || !Q_strcasecmp(end, "turnright"))
		var->ival = 4*sign;
	else if (!Q_strcasecmp(end, "left") || !Q_strcasecmp(end, "turnleft"))
		var->ival = 4*sign*-1;
	else if (!Q_strcasecmp(end, "up") || !Q_strcasecmp(end, "moveup"))
		var->ival = 5*sign;
	else if (!Q_strcasecmp(end, "down") || !Q_strcasecmp(end, "movedown"))
		var->ival = 5*sign*-1;
	else if (!Q_strcasecmp(end, "rollright"))
		var->ival = 6*sign;
	else if (!Q_strcasecmp(end, "rollleft"))
		var->ival = 6*sign*-1;
}

static cvar_t	joy_advaxis[6] =
{
#define ADVAXISDESC (const char *)"Provides a way to remap each joystick/controller axis.\nShould be set to one of: moveforward, moveback, lookup, lookdown, turnleft, turnright, moveleft, moveright, moveup, movedown, rollleft, rollright"
	CVARCD("joyadvaxisx", "moveright", joyaxiscallback, ADVAXISDESC),	//left rightwards axis
	CVARCD("joyadvaxisy", "moveback", joyaxiscallback, ADVAXISDESC),	//left downwards axis
	CVARCD("joyadvaxisz", "", joyaxiscallback, ADVAXISDESC),			//typically left trigger (use it as a button)
	CVARCD("joyadvaxisr", "turnright", joyaxiscallback, ADVAXISDESC),	//right rightwards axis
	CVARCD("joyadvaxisu", "lookup", joyaxiscallback, ADVAXISDESC),		//right downwards axis
	CVARCD("joyadvaxisv", "", joyaxiscallback, ADVAXISDESC)				//typically right trigger
};
static cvar_t	joy_advaxisscale[6] =
{
#define ADVAXISSCALEDESC "Because joyadvaxisx etc can be added together, this provides a way to rescale or invert an individual axis without affecting another with the same action."
	CVARD("joyadvaxisx_scale", "1.0", ADVAXISSCALEDESC),
	CVARD("joyadvaxisy_scale", "1.0", ADVAXISSCALEDESC),
	CVARD("joyadvaxisz_scale", "1.0", ADVAXISSCALEDESC),
	CVARD("joyadvaxisr_scale", "1.0", ADVAXISSCALEDESC),
	CVARD("joyadvaxisu_scale", "1.0", ADVAXISSCALEDESC),
	CVARD("joyadvaxisv_scale", "1.0", ADVAXISSCALEDESC)
};
static cvar_t	joy_anglesens[3] =
{
#define ANGLESENSDESC "Scaler value for the controller when it is at its most extreme value"
	CVARD("joypitchsensitivity", "0.5", ANGLESENSDESC),
	CVARD("joyyawsensitivity", "1.0", ANGLESENSDESC),
	CVARD("joyrollsensitivity", "1.0", ANGLESENSDESC)
};
static cvar_t	joy_movesens[3] =
{
	CVAR("joyforwardsensitivity", "1.0"),
	CVAR("joysidesensitivity", "1.0"),
	CVAR("joyupsensitivity", "1.0")
};
//comments on threshholds comes from microsoft's xinput docs.
static cvar_t	joy_anglethreshold[3] =
{
#define ANGLETHRESHOLDDESC "Values reported near the center of the analog joystick/controller are often erroneous and undesired.\nThe joystick threshholds are how much of the total values to ignore."
	CVARD("joypitchthreshold", "0.19", ANGLETHRESHOLDDESC),	//8689/32767 (right thumb)
	CVARD("joyyawthreshold", "0.19", ANGLETHRESHOLDDESC),	//8689/32767 (right thumb)
	CVARD("joyrollthreshold", "0.118", ANGLETHRESHOLDDESC),	//30/255	 (trigger)
};
static cvar_t	joy_movethreshold[3] =
{
	CVAR("joyforwardthreshold", "0.17"),//7849/32767 (left thumb)
	CVAR("joysidethreshold", "0.17"),	//7849/32767 (left thumb)
	CVAR("joyupthreshold", "0.118"),	//30/255	 (trigger)
};

static cvar_t joy_exponent = CVARD("joyexponent", "1", "Scales joystick/controller sensitivity non-linearly to increase precision in the center.\nA value of 1 is linear.");

#if defined(__linux__) && defined(FTE_SDL)
#ifdef FTE_SDL3
#include <SDL3/SDL.h>
#else
#include <SDL.h>
#endif
void joy_radialdeadzone_cb(cvar_t *var, char *oldvalue)
{
	if (!*var->string)
	{
#if SDL_VERSION_ATLEAST(3,0,0)
		if (SDL_GetHintBoolean(SDL_HINT_JOYSTICK_LINUX_DEADZONES, true))
#else
		if (SDL_GetHintBoolean(SDL_HINT_LINUX_JOYSTICK_DEADZONES, true))
#endif
			var->ival = 2;	//sdl2 provides its own deadzones on linux, by default.
		else
			var->ival = 1;
	}
}
#else
void joy_radialdeadzone_cb(cvar_t *var, char *oldvalue)
{
	if (!*var->string)
		var->ival = 1;
}
#endif
static cvar_t joy_radialdeadzone = CVARCD("joyradialdeadzone", "", joy_radialdeadzone_cb, "Treat controller dead zones as a pair, rather than per-axis.\n0: treat joystick axis independantly (square).\n1: treat the axis together, radially.\n2: do not handle deadzones (prefiltered).");

cvar_t in_skipplayerone = CVARD("in_skipplayerone", "1", "Do not auto-assign joysticks/game-controllers to the first player. Requires in_restart to take effect.");	//FIXME: this needs to be able to change deviceids when changed. until then menus will need to in_restart.


#define EVENTQUEUELENGTH 1024
static struct eventlist_s
{
	enum
	{
		IEV_KEYDOWN,
		IEV_KEYRELEASE,
		IEV_MOUSEABS,
		IEV_MOUSEDELTA,
		IEV_JOYAXIS,
		IEV_ACCELEROMETER,
		IEV_GYROSCOPE,
	} type;
	unsigned int devid;

	/*FTESurf Patch 202: when in_newevent() handed this slot out.  See the journal
	  block below for what it does and does not mean -- on Windows this is the time
	  the ENGINE saw the report, not the time the mouse produced it.*/
	double time;

	union
	{
		struct
		{
			float x, y, z;
			float tsize;	//the size of the touch
		} mouse;
		struct
		{
			int scancode, unicode;
		} keyboard;
		struct
		{
			int axis;
			float value;
		} joy;
		struct
		{	//metres per second, ish.
			float x, y, z;
		} accel;
		struct
		{	//these are in radians, not degrees.
			float pitch, yaw, roll;
		} gyro;
	};
} eventlist[EVENTQUEUELENGTH];
static volatile int events_avail; /*volatile to make sure the cc doesn't try leaving these cached in a register*/
static volatile int events_used;

/*FTESurf Patch 202: the ring has always dropped events silently when it filled, and
  nothing in this engine has ever reported that it happened.  A journal that cannot
  say "I lost some" is a journal that lies, so the count is kept whether or not one
  is open.  Same volatile discipline as events_avail, for the same reason.*/
static volatile unsigned int in_jrn_dropped;

static struct eventlist_s *in_newevent(void)
{
	if (events_avail >= events_used + EVENTQUEUELENGTH)
	{
		in_jrn_dropped++;
		return NULL;
	}

	/*FTESurf Patch 202.  Stamped HERE and not in the five producers because this is
	  the one choke point every event passes through -- key, mouse, joystick,
	  accelerometer, gyro, every platform backend -- so it is one line that a new
	  caller cannot forget.

	  Stamped UNCONDITIONALLY rather than only while a journal is open.  The gate
	  would save one QPC on a path that goes on to call Key_Event, and would buy a
	  bug: events already sitting in the ring when in_journal_begin runs would carry
	  stale stamps and be journalled as truth.

	  On the volatile comment above -- this is a store into the same struct the
	  producer already fills before it bumps events_avail, so it is exactly as
	  synchronised as ev->mouse.x already is, and no more.  On Windows every producer
	  is main-thread anyway: the wndproc, INS_Accumulate, and the low-level keyboard
	  hook (delivered on the installing thread's queue).  CL_IndepPhysicsThread calls
	  only CL_SendCmd and never touches this ring.*/
	eventlist[events_avail & (EVENTQUEUELENGTH-1)].time = Sys_DoubleTime();
	return &eventlist[events_avail & (EVENTQUEUELENGTH-1)];
}

static void in_finishevent(void)
{
	events_avail++;
}

#define MAXPOINTERS 8
static struct mouse_s
{
	enum
	{
		M_INVALID,
		M_MOUSE,	//using deltas
		M_TOUCH		//using absolutes
		//the only functional difference between touch and tablets is that tablets should draw the cursor when hovering too.
	} type;
	unsigned int qdeviceid;	//so we can just use pointers.
	vec2_t oldpos;		//last-known-cursor-position
	vec2_t heldpos;		//position the cursor was at when the button was held (touch-start pos)
	float moveddist;	//how far it has moved while held. this provides us with our emulated mouse1 when they release the press
	vec2_t delta;		//how far its moved recently
	vec2_t rawpend;		//FTESurf Patch 376: what the ring added to delta since IN_MoveMouse last read it
	vec2_t old_delta;	//how far its moved previously, for mouse smoothing
	float wheeldelta;
	double touchtime;	//0 when not touching, otherwise start time of touch.
	unsigned int touchkey;	//which other key we generated (so it gets released again.
	unsigned int updates;	//tracks updates per second
	qboolean updated;
} ptr[MAXPOINTERS];
static int touchcursor = -1;	//the cursor follows whichever finger was most recently pressed in preference to any mouse also on the same system

#define MAXJOYAXIS 6
#define MAXJOYSTICKS 8
static struct joy_s
{
	unsigned int qdeviceid;
	float axis[MAXJOYAXIS];
} joy[MAXJOYSTICKS];

void IN_Shutdown(void)
{
	INS_Shutdown();
}

void IN_ReInit(void)
{
	int i;

	for (i = 0; i < MAXPOINTERS; i++)
	{
		memset(&ptr[i], 0, sizeof(ptr[i]));
		ptr[i].type = M_INVALID;
		ptr[i].qdeviceid = i;
	}

	for (i = 0; i < MAXJOYSTICKS; i++)
	{
		memset(&joy[i], 0, sizeof(joy[i]));
		joy[i].qdeviceid = i;
	}

	INS_ReInit();
}

struct remapctx
{
	char *type;
	char *devicename;
	unsigned int newdevid;
	unsigned int found;
	unsigned int failed;
};
static void IN_DeviceIDs_DoRemap(void *vctx, const char *type, const char *devicename, unsigned int *qdevid)
{
	struct remapctx *ctx = vctx;

	if (!strcmp(ctx->type, type) || !strcmp(ctx->type, "*"))
		if (!strcmp(ctx->devicename, devicename) || !strcmp(ctx->devicename, "*"))
		{
			if (qdevid)
				*qdevid = ctx->newdevid;
			else
				ctx->failed = true;
			ctx->found++;
		}
}
void IN_DeviceIDs_Enumerate(void *ctx, const char *type, const char *devicename, unsigned int *qdevid)
{
	char buf[8192];
	devicename = COM_QuotedString(devicename, buf, sizeof(buf), false);
	if (!qdevid)
		Con_Printf("%s\t%s\t%s\n", type, "N/A", devicename);
	else if (*qdevid == DEVID_UNSET)
		Con_Printf("%s\t%s\t%s\n", type, "Unset", devicename);
	else
		Con_Printf("%s\t%u\t%s\n", type, *qdevid, devicename);
}

void IN_DeviceIDs_f(void)
{
	struct remapctx ctx;

	if (Cmd_Argc() > 3)
	{
		ctx.failed = false;
		ctx.found = 0;
		ctx.type = Cmd_Argv(1);
		ctx.newdevid = strtoul(Cmd_Argv(2), NULL, 0);
		ctx.devicename = Cmd_Argv(3);
		INS_EnumerateDevices(&ctx, IN_DeviceIDs_DoRemap);

		if (ctx.failed)
			Con_Printf("device cannot be remapped\n");
		else if (!ctx.found)
			Con_Printf("%s \"%s\" not known\n", ctx.type, ctx.devicename);
		else if (!cl_warncmd.ival)
			Con_Printf("device remapped\n");
	}
	else if (Cmd_Argc() > 1)
	{
		Con_Printf("%s TYPE NEWID DEVICENAME\n", Cmd_Argv(0));
	}
	else
	{
		Con_Printf("Type\tMapping\tName\n");
		INS_EnumerateDevices(NULL, IN_DeviceIDs_Enumerate);
	}
}

float IN_DetermineMouseRate(void)
{
	double time = Sys_DoubleTime();
	static double timer;
	static float last;
	float interval = time - timer;
	if (fabs(interval) >= 1)
	{
		timer = time;
		last = ptr[0].updates/interval;
		ptr[0].updates = 0;
	}
	return last;
}

void IN_Init(void)
{
	int i;
	events_avail = 0;
	events_used = 0;

	Cvar_Register (&m_filter, "input controls");
	Cvar_Register (&m_forcewheel, "Input Controls");
	Cvar_Register (&m_forcewheel_threshold, "Input Controls");
	Cvar_Register (&m_touchstrafe, "input controls");
	Cvar_Register (&m_longpressthreshold, "input controls");
	Cvar_Register (&m_slidethreshold, "input controls");
	Cvar_Register (&m_touchmajoraxis, "input controls");
	Cvar_Register (&m_accel, "input controls");
	Cvar_Register (&m_accel_style, "input controls");
	Cvar_Register (&m_accel_power, "input controls");
	Cvar_Register (&m_accel_offset, "input controls");
	Cvar_Register (&m_accel_senscap, "input controls");

	for (i = 0; i < 6; i++)
	{
		Cvar_Register (&joy_advaxis[i], "input controls");
		Cvar_Register (&joy_advaxisscale[i], "input controls");

		Cvar_ForceCallback(&joy_advaxis[i]);
	}
	for (i = 0; i < 3; i++)
	{
		Cvar_Register (&joy_anglesens[i], "input controls");
		Cvar_Register (&joy_movesens[i], "input controls");
		Cvar_Register (&joy_anglethreshold[i], "input controls");
		Cvar_Register (&joy_movethreshold[i], "input controls");
	}
	Cvar_Register (&joy_exponent, "input controls");
	Cvar_Register (&joy_radialdeadzone, "input controls");
	Cvar_Register (&in_skipplayerone, "input controls");

	Cmd_AddCommand ("in_deviceids", IN_DeviceIDs_f);

	/*FTESurf Patch 202.  Commands rather than a cvar, and the reason is concrete:
	  a cvar callback fires on registration too (the Cvar_ForceCallback idiom just
	  above), so an archived value would write a file at startup and every exec or
	  server "stuffcmd set" would write another.  A cvar also cannot express
	  "discard" distinctly from "write to an empty path".*/
	Cvar_Register (&in_journal_maxkb, "input controls");

	/*FTESurf Patch 326: the grant, beside the request.  Registered here rather than
	  in in_win.c for the same reason the variables behind them are declared in this
	  file: every client build has in_generic.o, so a backend that reports nothing
	  still offers the cvar reading -1, and a QC reader can tell "this platform does
	  not say" apart from "there is no such cvar on this engine".*/
	Cvar_Register (&in_rawmice, "input controls");
	Cvar_Register (&in_rawkbds, "input controls");
	Cmd_AddCommandD ("in_journal_begin", IN_JournalBegin_f, "Start an in-memory journal of raw input events.  Discards any journal already open.");
	Cmd_AddCommandD ("in_journal_end", IN_JournalEnd_f, "in_journal_end [path] -- write the journal under data/ and close it.  With no path, discard it.");
	Cmd_AddCommandD ("in_journal_note", IN_JournalNote_f, "Append a comment line to the open input journal.");
	Cmd_AddCommandD ("in_journal_synth", IN_JournalSynth_f, "in_journal_synth <n> -- inject n synthetic input events, for testing.  Marks the journal as synthetic, which makes it inadmissible.");

	INS_Init();
}

//there was no ui to click on at least...
//translates touch press events into ones that are actually bound, according to touchstrafe and position.
int IN_Touch_Fallback(unsigned int devid)
{
	int ret;
	if (devid >= countof(ptr))
		ret = 0;
	else switch(m_touchstrafe.ival)	//translate touch to mouse2 if its on the strafing side of the screen.
	{
	case 2:
		ret = ptr[devid].heldpos[0] < vid.pixelwidth/2;
		break;
	default:
		ret = ptr[devid].heldpos[0] > vid.pixelwidth/2;
		break;
	case 0:
		ret = false;
		break;
	}
	ret = ret?K_MOUSE2:K_MOUSE1;

	return ret;
}
void IN_Touch_BlockGestures(unsigned int devid)
{	//called via K_TOUCH, blocks K_TOUCHTAP etc gestures
	if (devid < countof(ptr))
		ptr[devid].touchkey = 0;	//block it all.
}
qboolean IN_Touch_MouseIsAbs(unsigned int devid)
{	//lets the caller know if a mouse1 down event was abs
	if (devid < countof(ptr))
		return ptr[devid].type == M_TOUCH;
	return false;
}

/*
==============================================================================

FTESurf Patch 202: the raw input journal (.hid)

WHAT IT IS FOR.  FTESurf records every timed run twice already -- a server-side
.rec at ~66/s and a client-side .view at render rate -- and both are derived
data: by the time either sees the mouse, IN_MoveMouse has consumed one SUMMED
delta per frame (see ptr[].delta below) and CL_AccumlateInput has weighted-
averaged the move axes across frames.  The individual reports are gone.

This journal keeps them.  It is an ANTI-CHEAT instrument and deliberately not a
playback one: the .rec positions are authoritative and the .view angle stream at
300 Hz is already finer than 66.7 Hz physics can express, so there is no fidelity
to gain.  What there is to gain is evidence -- the shape of the delta sequence
(a script's is uniform and unjittered), the effective report rate, and three
independent recordings of one run that a forgery has to falsify consistently.

WHAT THE TIMESTAMPS ARE, AND ARE NOT.  On Windows WM_INPUT arrives in the thread
message queue and Sys_SendKeyEvents drains a frame's worth in one PeekMessage
loop, microseconds apart; RAWMOUSE carries no hardware timestamp.  So a stamp
taken anywhere in this engine is WHEN THE ENGINE SAW THE REPORT, not when the
mouse produced it.  Per-frame timing is real.  Inter-event spacing INSIDE one
frame is a pump artifact and no reader may treat it as anything else.
(The upgrade, if that ever becomes the limiting factor, is GetMessageTime() in
the wndproc -- 1 ms resolution, which at 1000 Hz polling is one report per tick.
Nothing in this engine calls it today.)

AND IT DEPENDS ON in_rawinput.  The fallback path is INS_Accumulate's
GetCursorPos/SetCursorPos recentre -- ONE already-OS-summed delta per call, about
twice a frame.  On that path this file is a slightly finer .view and nothing more.

CORRECTED, PATCH 301: this paragraph used to say "that cvar defaults to 0
(in_win.c), as does in_dinput", and then spent a paragraph explaining that both
games on this tree set it to 1 from their configs so the engine default described
"a bare engine, not either game here".  `in_rawinput` now defaults to **1** --
in_win.c declares it CVARAFD(... "1" ..., CVAR_ARCHIVE) and the same change
removed cfg/default.cfg's `set in_rawinput 1`, because an archived cvar whose
default.cfg also asserts a value can never be turned off by the player.  The old
text was a fact about a config that has since moved into the engine, left standing
in a comment that reads as a fact about this code.  `in_rawinput_keyboard` is
still 0, and that half of the old warning stands.

So the header records the mode -- and since Patch 301, records it TWICE, because
a cvar is a request and not a grant:
  `rawinput` / `rawkbd`   the cvars: what this client ASKED for.
  `rawmice`  / `rawkbds`  what INS_RawInput_Init actually bound (-1 if the
                          platform backend does not report it).
The pair that matters is `rawinput 1` with `rawmice 0`: the request was granted by
the config and refused by the hardware, and every delta in the file is an
OS-summed, OS-accelerated one.  See the declaration of in_rawmice_live below.

`rawkbd` being 0 leaves keys on the legacy WM_KEYDOWN path -- that one AUTO-REPEATS,
and a reader pairing downs with ups needs to know.  A tool drawing a timing
conclusion without reading all four keys is drawing it from the wrong data.

PRIVACY.  data/ is readable by any CSQC, and every server a player joins runs
CSQC.  A journal written with the console open would contain the scancodes of an
rcon_password.  Two rules, both here rather than "later", because a mitigation
deferred is a mitigation that never lands:
  - the unicode is NEVER journalled.  The scancode is the input; the unicode is
    the plaintext, and it adds nothing to an audit.
  - when anything above the game has focus, the scancode is replaced by an 'x'
    line.  Timing and count survive -- a macro bound to a key and fired with the
    console open still shows as a burst -- only the identity is dropped.  Those
    keystrokes never reach +forward and carry no audit value.  The count is in
    the trailer, so the suppression is itself auditable.

THE FORMAT.  Header keys one per line, unknown keys skipped, exactly the additive
rule FTESURF-REC 3 and FTESURF-VIEW already follow.  Then:

	f <dt> <abs> <seq>      a drain of IN_Commands began.  abs = seconds since
	                        'base', seq = cl.movesequence
	m <dt> <dev> <dx> <dy>  IEV_MOUSEDELTA, in device units
	a <dt> <dev> <x> <y>    IEV_MOUSEABS.  An absolute position identical to the
	                        previous one from the same device is NOT recorded --
	                        the same non-event IN_MouseMove already drops for a
	                        zero delta.  See the case body for the measurement
	                        that made it necessary rather than tidy.
	+ <dt> <dev> <key>      key down, FTE K_* scancode
	- <dt> <dev> <key>      key up
	x <dt> <dev>            a key event whose scancode was suppressed
	j <dt> <dev> <ax> <v>   joystick axis
	d <dt> <rawdx> <rawdy>  Patch 312.  The counts the RING delivered this frame,
	                        written ONLY when they differ from what reached the
	                        view on the 'v' line that follows.  Its absence is
	                        therefore the positive statement "the pipeline passed
	                        the counts through untouched", which is the case on
	                        45,384 of 45,384 measured frames of honest play.
	# <dt> <text>           a note from the gamecode (save/load marks)
	! <dt> <n>              n events were lost by the ring BEFORE this point
	truncated <dt>          the cap was hit; nothing after this exists
	end <dt> <abs> <events> <frames> <dropped> <hidden>

<dt> is integer MICROSECONDS since the previous line, whatever kind it was.
Every line carries one, and only 'f' also carries an absolute -- so a reader can
resync after a corrupt region, AND the running sum of dt must equal each f's abs.
That is a self-check written from two independent counters, which is the same
discipline reccheck.py already applies to the .rec's end record.

The sum is EXACT, not approximate, because IN_Journal_Line carries the sub-
microsecond remainder forward rather than discarding it on each line.  Without
that the two disagree by about 1.8 ms per 5500 lines and the check would need a
tolerance proportional to file length, which is barely a check at all.

'+'/'-' for down/up is deliberate: grep '^+' is the whole keyboard.

WHY IN MEMORY.  The run's tag (pb/last/shadow) is part of the filename and is not
known until the run ends, so streaming would need a .part and a rename -- the
exact failure SV_RecClose was rewritten to eliminate, where a crash between the
remove and the rename loses the previous recording as well as this one.  Discard
on a voided run is then free, and the cap is one number.  What it costs, and this
is a real cost rather than a tidy one: a crash loses the evidence, and the write
is one synchronous COM_WriteFile at the instant the run ends.  in_journal_maxkb
8192 holds that to about 40 ms.  If crash-resistance ever matters more than the
rename hazard, stream to .part and accept the other failure mode.

==============================================================================
*/
/*QC_FixFileName lives in common/pr_bgcmd.c and no header declares it; the local
  extern with a source note is the idiom cl_input.c:1815 already uses.*/
qboolean QC_FixFileName(const char *name, const char **result, const char **fallbackread);

static char			*in_jrn_buf;
static size_t		in_jrn_len, in_jrn_cap, in_jrn_max;
static double		in_jrn_base, in_jrn_last;
static unsigned int	in_jrn_dropreported, in_jrn_dropbase, in_jrn_events, in_jrn_frames, in_jrn_hidden;
static int			in_jrn_injbase, in_jrn_unenumbase;			/*Patch 306: taken at begin, so the journal reports only its own window*/
static int			in_jrn_injreported, in_jrn_unenumreported;	/*...and how far the per-frame records have caught up*/
static int			in_jrn_lgbbase, in_jrn_lgbreported;			/*Patch 307: the same pair for the uncorroborated legacy button*/
static int			in_jrn_nolegacyreported;					/*Patch 307: the last effective suppression state written*/
/*Patch 310: the render-integrity cvars, resolved ONCE at begin.  Caching the
  pointers is what makes the per-frame poll free -- a Cvar_FindVar per cvar per
  frame would be a string hash lookup 800 times a second for nothing.*/
static cvar_t		*in_jrn_rcv[MAX_JRN_TRACKEDCVARS];	/*Patch 312: render AND input*/
static int			in_jrn_rcvmod[MAX_JRN_TRACKEDCVARS];
static int			in_jrn_rcvcount;
static qboolean		in_jrn_full, in_jrn_synth;
static float		in_jrn_lastabs[MAXPOINTERS][2];
static qboolean		in_jrn_haveabs[MAXPOINTERS];

/*FTESurf Patch 293: the per-frame view record.  IN_MoveMouse adds each pointer's
  final delta in here, IN_Journal_View emits one line once the angle it produced
  is known.  See the essay above IN_Journal_View.*/
static float		in_jrn_vdx, in_jrn_vdy;
static float		in_jrn_vkpitch, in_jrn_vkyaw;	/*Patch 305: the non-mouse half*/
/*Patch 312: the counts AS THE RING DELIVERED THEM, before any of the pipeline
  touched them.  See the essay above IN_Journal_ViewRaw.*/
static float		in_jrn_vrawx, in_jrn_vrawy;
static int			in_jrn_vflags;
static float		in_jrn_vlast[2];
static qboolean		in_jrn_vhavelast;

/*Raw append.  Grows by doubling; the caller has already decided the line fits.*/
static void IN_Journal_Raw(const char *s)
{
	size_t l = strlen(s);

	if (!in_jrn_buf)
		return;

	if (in_jrn_len + l > in_jrn_cap)
	{
		size_t want = in_jrn_cap*2;
		while (want < in_jrn_len + l)
			want *= 2;
		if (want > in_jrn_max + 1024)
			want = in_jrn_max + 1024;	/*headroom for the truncated and end lines*/
		if (in_jrn_len + l > want)
			return;
		in_jrn_buf = BZ_Realloc(in_jrn_buf, want);
		in_jrn_cap = want;
	}

	memcpy(in_jrn_buf+in_jrn_len, s, l);
	in_jrn_len += l;
}

/*Every line goes through here so nothing can forget the delta.  Clamped at 0:
  notes and the trailer are stamped from the Cbuf, which runs after the drain, so
  they are monotone in practice -- but a clamp is cheaper than a negative dt that
  a reader would have to decide what to do with.

  The cap is enforced HERE rather than in the raw append, because only this
  function knows `when` -- and a truncation marker whose own timestamp is
  guesswork would be the one line in the file that cannot be checked.*/
static void IN_Journal_Line(double when, const char *kind, const char *tail)
{
	char line[256];
	double dt;
	int us;

	if (!in_jrn_buf || in_jrn_full)
		return;

	dt = when - in_jrn_last;
	if (dt < 0)
		dt = 0;

	/*THE TRUNCATION REMAINDER IS CARRIED, not discarded, and that is what makes
	  the file's self-check exact instead of approximate.

	  Advancing in_jrn_last to `when` throws away up to 1 us on every line.  At
	  ~5500 lines in 1.6 s -- measured, not guessed -- that is 1.8 ms of drift
	  between the running sum of dt and the absolute on the next frame marker, and
	  it grows without bound.  A self-check that needs a tolerance proportional to
	  the file length is barely a self-check.

	  Advancing by the dt actually EMITTED instead means the sum of dt is the
	  elapsed time by construction: the residual is carried into the next line's
	  dt and comes back.  The error is then bounded below 1 us for the whole file,
	  however long it is, and hidcheck.py can demand exactness.*/
	us = (int)(dt*1000000);
	Q_snprintfz(line, sizeof(line), "%s %i%s%s\n", kind, us, *tail?" ":"", tail);

	if (in_jrn_len + strlen(line) > in_jrn_max)
	{	/*one honest marker, then nothing.  A truncated journal is useless as an
		  audit, but it must never be mistakable for a complete one.*/
		in_jrn_full = true;
		Q_snprintfz(line, sizeof(line), "truncated %i %.6f\n", us, when - in_jrn_base);
		in_jrn_last += us*0.000001;
		IN_Journal_Raw(line);
		return;
	}

	in_jrn_last += us*0.000001;
	IN_Journal_Raw(line);
}

void IN_Journal_Note(const char *text)
{
	char clean[96];
	size_t i;

	if (!in_jrn_buf)
		return;

	/*the gamecode writes this, so it is sanitised rather than trusted: anything
	  that could introduce a newline would let a note forge a record line.*/
	for (i = 0; i < sizeof(clean)-1 && text[i]; i++)
	{
		char c = text[i];
		clean[i] = ((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c==' '||c=='_'||c=='.'||c=='-') ? c : '?';
	}
	clean[i] = 0;

	/*STAMPED AT THE STREAM POSITION, NOT AT THE WALL CLOCK, and that is a
	  correctness fix rather than a shortcut.

	  A note arrives from the command buffer, which Cbuf_Execute runs AFTER
	  IN_Commands has already drained that frame -- but in_journal_note is issued
	  by the same Cbuf pass that in_journal_synth (or, in the real case, a save
	  keypress) put events into the ring with EARLIER stamps.  Those events are
	  drained on the next frame, so a note stamped from the wall clock lands 600 us
	  ahead of the frame marker that follows it.  IN_Journal_Line's dt clamp then
	  emits a 0 there and the running sum of dt runs ahead of the absolutes, which
	  breaks the one identity this format is built on.  Measured: +-609 us,
	  twice, in the first build of this.

	  A note is an annotation and not an event.  Its useful content is WHERE in
	  the stream it sits -- which frame it fell between -- and that is exactly
	  what this preserves, at the cost of up to one frame of precision on a
	  timestamp nobody reads.*/
	IN_Journal_Line(in_jrn_last, "#", clean);
}

void IN_Journal_Drop(void)
{
	if (in_jrn_buf)
		BZ_Free(in_jrn_buf);
	in_jrn_buf = NULL;
	in_jrn_len = in_jrn_cap = 0;
	in_jrn_full = in_jrn_synth = false;
	in_jrn_events = in_jrn_frames = in_jrn_hidden = 0;
	memset(in_jrn_haveabs, 0, sizeof(in_jrn_haveabs));
}

static void IN_Journal_Event(struct eventlist_s *ev)
{
	char tail[128];

	if (!in_jrn_buf || in_jrn_full)
		return;

	in_jrn_events++;

	switch(ev->type)
	{
	case IEV_KEYDOWN:
	case IEV_KEYRELEASE:
		/*never the unicode -- see the privacy note above.  scancode -1 is the
		  "release everything" pseudo-event and 0 is the unicode-only dead-key
		  path (in_win.c); neither is a key anyone pressed, so neither is
		  journalled as one.*/
		if (Key_Dest_Has_Higher(kdm_game))
		{
			in_jrn_hidden++;
			Q_snprintfz(tail, sizeof(tail), "%u", ev->devid);
			IN_Journal_Line(ev->time, "x", tail);
			break;
		}
		/*FTESurf Patch 309: WAS THIS KEY ALREADY DOWN.

		  MEASURED, on Lex's clean bhop_eazy PB (0004976_pb).  326 key-down
		  records against 130 releases -- 196 downs, 60% of every press in the
		  file, with no matching release.  They are OS auto-repeat, and nothing
		  said so.  On the two turn keys alone (e and CapsLock, which is where
		  this game's +left/+right live) it is 144 downs against 15 releases:
		  129 of 144, EIGHTY-NINE POINT SIX PERCENT of the turn-key press
		  records in an honest run, describing presses that never happened.

		  A reader asking "how many times did the player press the turn key"
		  got 144 for 15.  That is not a rounding error, it is a different
		  story about the run -- and the shape it invents (a rapid, perfectly
		  even press train) is precisely the shape a scripted turn would have.
		  The record was manufacturing the signature it exists to look for.

		  READ FROM keydown[] RATHER THAN TRACKED HERE, and that is deliberate.
		  Key_Event computes the identical value one line into its own body
		  (`wasdown`, keys.c) to block auto-repeat binds -- so the fact already
		  exists and a second copy would be a second thing to get wrong.  The
		  ordering is what makes it correct: IN_Commands calls this function
		  BEFORE the switch that dispatches Key_Event, so keydown[] still holds
		  the state from before this event -- exactly what Windows' lParam bit
		  30 means, but derived rather than threaded, so it holds on every
		  backend and on the raw path too instead of only on the Windows legacy
		  one.  Key_Event also owns the scancode -1 'release everything' sweep,
		  so a focus loss cannot leave this stale.

		  ON THE RELEASE TOO, because the same bit answers a second question
		  there: a '-' whose key was NOT down is an orphan -- a release with no
		  press, which means a press was lost or a release was invented.  One
		  field, one meaning ("the key was already down"), two findings.

		  -1 where the question does not apply: scancode -1 is the release-
		  everything pseudo-event and 0 is the unicode-only dead-key path, and
		  neither is a key anyone pressed.  NOT 0, which is a real answer.*/
		{
			int prev = -1;
			if (ev->keyboard.scancode > 0 && ev->keyboard.scancode < K_MAX &&
				ev->devid < 32)
				prev = !!(keydown[ev->keyboard.scancode] & (1u<<ev->devid));
			Q_snprintfz(tail, sizeof(tail), "%u %i %i",
				ev->devid, ev->keyboard.scancode, prev);
		}
		IN_Journal_Line(ev->time, (ev->type==IEV_KEYDOWN)?"+":"-", tail);
		break;
	case IEV_MOUSEDELTA:
		Q_snprintfz(tail, sizeof(tail), "%u %g %g", ev->devid, ev->mouse.x, ev->mouse.y);
		IN_Journal_Line(ev->time, "m", tail);
		break;
	case IEV_MOUSEABS:
		/*AN UNCHANGED ABSOLUTE POSITION IS NOT AN EVENT, and dropping it is the
		  same call IN_MouseMove already makes one screen down, where a delta of
		  (0,0,0) never becomes an event at all.

		  It is not a nicety.  IN_MouseMove does NOT apply that early-out to
		  absolute events, so while a menu or the console is up -- which is when
		  the cursor is free and the engine reports it -- a motionless mouse
		  produces two identical lines EVERY frame.  Measured on a headless run:
		  3706 of 5559 lines, all reading the same "-1718 793", for 1.6 seconds
		  of a config doing nothing.  At 1100 fps that is a megabyte a minute of
		  a cursor sitting still, and it would reach in_journal_maxkb and
		  truncate a real journal with pure noise.

		  Nothing is lost that an audit could use: an identical position carries
		  no information by definition, and absolute events only happen while
		  something above the game has focus -- which is exactly the state where
		  the key identities are being suppressed anyway.*/
		if (ev->devid < MAXPOINTERS)
		{
			if (in_jrn_haveabs[ev->devid] &&
			    in_jrn_lastabs[ev->devid][0] == ev->mouse.x &&
			    in_jrn_lastabs[ev->devid][1] == ev->mouse.y)
			{
				in_jrn_events--;
				break;
			}
			in_jrn_haveabs[ev->devid] = true;
			in_jrn_lastabs[ev->devid][0] = ev->mouse.x;
			in_jrn_lastabs[ev->devid][1] = ev->mouse.y;
		}
		Q_snprintfz(tail, sizeof(tail), "%u %g %g", ev->devid, ev->mouse.x, ev->mouse.y);
		IN_Journal_Line(ev->time, "a", tail);
		break;
	case IEV_JOYAXIS:
		Q_snprintfz(tail, sizeof(tail), "%u %i %g", ev->devid, ev->joy.axis, ev->joy.value);
		IN_Journal_Line(ev->time, "j", tail);
		break;
	default:
		in_jrn_events--;	/*accelerometer/gyro: not journalled, not counted*/
		break;
	}
}

/*FTESurf Patch 306: emit the rejected-report counters when they move.

Same shape and same honesty as the '!' drop record below: the COUNT is known, the TIME
IS NOT -- the backend discarded the report without stamping it -- so this says "n arrived
somewhere before this line" and claims nothing finer.

CALLED FROM TWO PLACES, AND THE SECOND IS THE ONE THAT MATTERS.  IN_Journal_Frame runs
only on a NON-EMPTY drain, and a rejected report by definition produces no event to
drain.  So an injection with no genuine input beside it -- which is exactly the session
this record exists for -- would never reach a frame marker and would never be written at
all.  IN_Journal_View runs once per accumulate frame regardless of the ring, so it closes
that gap.  The delta test makes the second call free on every frame where nothing
happened, and makes the order of the two irrelevant.*/
static void IN_Journal_Rejected(double when)
{
	char tail[64];
	int inj, une;

	if (!in_jrn_buf || in_jrn_full)
		return;

	if (in_raw_injected < 0 && in_raw_unenum < 0)
		return;	/*this backend does not count -- stay silent rather than write a zero
				  it cannot support.  See the essay at the declarations.*/

	/*clamped independently: a backend that came to count only one of the two would
	  otherwise have the other's -1 subtracted from a 0 baseline and print a delta of -1.*/
	inj = (in_raw_injected < 0) ? 0 : in_raw_injected;
	une = (in_raw_unenum  < 0) ? 0 : in_raw_unenum;

	if (inj == in_jrn_injreported && une == in_jrn_unenumreported)
		return;

	Q_snprintfz(tail, sizeof(tail), "%i %i", inj - in_jrn_injreported, une - in_jrn_unenumreported);
	in_jrn_injreported = inj;
	in_jrn_unenumreported = une;
	IN_Journal_Line(when, "i", tail);
}

/*FTESurf Patch 307: the uncorroborated legacy button, on its OWN record rather than as a
  third field on 'i'.

  They are not the same kind of fact and a reader must not be able to read them alike.
  'i' counts input that was REJECTED -- it never reached the game, and a non-zero count
  there is an attempt that failed.  'b' counts input that was ACCEPTED: every one of these
  became a K_MOUSE1..5 and was acted on.  Sharing a line would invite exactly the
  collapse that the two counters inside 'i' are already separated to prevent.*/
/*FTESurf Patch 307: the effective legacy-suppression state CHANGES DURING A RUN, and a
  header field cannot describe something that changes.

  MEASURED, on this patch's own first falsifier run.  in_rawinput_nolegacy is grab-scoped
  -- it has to be, because RIDEV_NOLEGACY also kills the non-client messages a windowed
  player needs to move their own window -- so every grab transition flips it.  The test
  harness stealing focus flipped it FOUR TIMES in three seconds, and the header, being a
  snapshot taken at in_journal_begin, recorded `nolegacylive 0` for an arm in which the
  block demonstrably worked (0 injected clicks through, against 20 and 16 on either
  side).  The header was not wrong; it was answering a question the file could not ask.

  So the transitions are recorded.  An auditor can then say WHICH SPANS of a run had the
  legacy click path open, instead of inferring it from one value at one instant -- and in
  ordinary play, where alt-tabbing is the only thing that flips it, the record is a couple
  of lines long.

  This is the third patch in a row to land on the same lesson: Patch 301 separated the
  cvar from the grant, Patch 306 separated "not counted" from "counted zero", and this
  separates "true at the start" from "true throughout".*/
static void IN_Journal_Legacy(double when)
{
	char tail[32];

	if (!in_jrn_buf || in_jrn_full)
		return;
	if (in_raw_nolegacy_live == in_jrn_nolegacyreported)
		return;

	Q_snprintfz(tail, sizeof(tail), "%i", in_raw_nolegacy_live);
	in_jrn_nolegacyreported = in_raw_nolegacy_live;
	IN_Journal_Line(when, "g", tail);
}

/*FTESurf Patch 310: emit a line when a render-integrity cvar is touched mid-journal.

  modifiedcount rather than the value: a set-and-set-back leaves the value identical and
  the count two higher, and "it was briefly something else" is exactly the fact a
  snapshot-based reader would miss.  The header carries the opening values, so these are
  changes FROM that, and a run where nobody touched anything writes none of them.*/
static void IN_Journal_RenderCvars(double when)
{
	char tail[192];
	char quoted[128];
	int i;

	if (!in_jrn_buf || in_jrn_full)
		return;

	for (i = 0; i < in_jrn_rcvcount; i++)
	{
		if (!in_jrn_rcv[i] || in_jrn_rcv[i]->modifiedcount == in_jrn_rcvmod[i])
			continue;
		in_jrn_rcvmod[i] = in_jrn_rcv[i]->modifiedcount;
		Q_snprintfz(tail, sizeof(tail), "%s %s", in_jrn_rcv[i]->name,
			COM_QuotedString(in_jrn_rcv[i]->string, quoted, sizeof(quoted), false));
		IN_Journal_Line(when, "c", tail);
	}
}

static void IN_Journal_Bypassed(double when)
{
	char tail[64];

	if (!in_jrn_buf || in_jrn_full)
		return;
	if (in_raw_legacybtn < 0)
		return;
	if (in_raw_legacybtn == in_jrn_lgbreported)
		return;

	Q_snprintfz(tail, sizeof(tail), "%i", in_raw_legacybtn - in_jrn_lgbreported);
	in_jrn_lgbreported = in_raw_legacybtn;
	IN_Journal_Line(when, "b", tail);
}

static void IN_Journal_Frame(void)
{
	char tail[64];
	double first;

	if (!in_jrn_buf || in_jrn_full)
		return;

	/*The f line's own stamp is the FIRST event of this drain, not the drain time:
	  the events were stamped on arrival, i.e. before now, so anchoring to the
	  drain would make every dt that follows negative.*/
	first = eventlist[events_used & (EVENTQUEUELENGTH-1)].time;
	in_jrn_frames++;

	Q_snprintfz(tail, sizeof(tail), "%.6f %i", first - in_jrn_base, cl.movesequence);
	IN_Journal_Line(first, "f", tail);

	if (in_jrn_dropped != in_jrn_dropreported)
	{	/*The drop TIME is genuinely unknown -- the ring threw the events away
		  without recording when -- so this says "n were lost somewhere before
		  this frame" and does not pretend to more.  A .hid carrying one of these
		  is not admissible across that gap.*/
		Q_snprintfz(tail, sizeof(tail), "%u", in_jrn_dropped - in_jrn_dropreported);
		in_jrn_dropreported = in_jrn_dropped;
		IN_Journal_Line(first, "!", tail);
	}

	IN_Journal_Rejected(first);	/*Patch 306*/
	IN_Journal_Legacy(first);	/*Patch 307*/
	IN_Journal_RenderCvars(first);	/*Patch 310*/
	IN_Journal_Bypassed(first);	/*Patch 307*/
}

/*
==============================================================================

FTESurf Patch 293: the per-frame view record ('v'), and why the journal had to
carry BOTH halves of the identity itself.

WHAT IT IS FOR.  With m_filter 0 and m_accel 0 the engine's mouse path is one
exact line (IN_MoveMouse, below):

    viewanglechange[YAW] -= m_yaw * (sensitivity * in_sensitivityscale) * mx

so for an unmodified client the yaw a frame produces is a fixed multiple of the
integer device counts that frame drained.  That is an ALGEBRAIC INVARIANT, not a
heuristic, and it is the one test that separates "the player moved the mouse"
from "something between the mouse and the angle changed the number".  A hook
that mutates the accumulated delta -- which is where this class of cheat lives,
because it is the only place the number is both final and still a mouse delta --
breaks it on every frame it touches, at any strength.

WHY NOT JUST CROSS-CHECK THE .hid AGAINST THE .view.  Because the two files
cannot be aligned, and that is by design rather than by accident:

  - the 'f' marker is emitted only on a NON-EMPTY drain (IN_Commands, below), so
    .hid frames are a deliberate SUBSET of rendered frames;
  - the only shared key is the command frame, and at 800 fps against cl_netfps
    66 roughly fifteen .view rows carry the same one.

Measured on this tree's own recordings before this patch existed:
data/runs/surf_beginner/stage_2/0000279_pb has 3298 'f' records against 4177
.view rows (246 vs 279 distinct command frames); the best index alignment agrees
on 35.7% of frames, and surf_boreas/main/0002930_pb manages 8.5%.  The residual
|dyaw - k*dx| then sits at 0.039 and 0.054 degrees -- around eight times .view's
own 0.005 degree half-step, i.e. swamped by misalignment long before precision
becomes the limit.  Raising .view's %.2f would not have fixed it.

So this line carries the counts and the angle they produced TOGETHER, and the
check needs no join at all.

WHAT IS AND IS NOT ON THE LINE.  dx/dy are the counts as they enter the
sensitivity pipeline: after in_xflip, after the Key_MouseShouldBeFree zeroing,
after the touch and CSQC_MouseMove consumers, summed over every pointer -- i.e.
exactly what m_filter/m_accel/sensitivity are about to be applied to, and
nothing that was already discarded upstream.  pitch/yaw are the FINAL angles for
the frame, read after CL_ClampPitch, because that is the only point at which
viewanglechange has been folded in and clamped.

flags exists because the identity does not hold on every frame and a reader must
be told which: bit 1 says the counts went to sidemove instead of yaw (+strafe),
bit 2 says the same for forwardmove/pitch, bit 4 says the cursor was free (a
menu or the console had it) and the counts reached neither.

THE EMIT RULE, and it is the whole of the size budget.  A line per rendered
frame at 800 fps is about 1.9 MB a minute, which would roughly triple this file
against the ~1.15 MB/min it already costs and would reach in_journal_maxkb
inside a long run.  A frame with no counts AND no angle change says nothing an
audit can use, so it is not written -- the same argument IN_MouseMove already
makes for a zero delta, and IN_Journal_Event makes for an unchanged absolute.

Note the second half of that test.  Dropping frames on "no counts" ALONE would
throw away precisely the evidence this patch exists to collect: an angle that
moved without counts behind it is the signature, not the noise.  So the rule is
"no counts and the angle did not move", and either half alone keeps the line.

==============================================================================
*/
void IN_Journal_View(const float *viewangles)
{
	char tail[128];

	if (!in_jrn_buf || in_jrn_full)
		return;

	/*Patch 306, and it goes ABOVE the emit rule deliberately.  A rejected report
	  produces no counts and moves no angle, so every frame of a motion-injection
	  attempt takes the quiet-frame return below -- the one case where the record
	  must NOT be quiet.*/
	IN_Journal_Rejected(Sys_DoubleTime());
	IN_Journal_Legacy(Sys_DoubleTime());	/*Patch 307*/
	IN_Journal_RenderCvars(Sys_DoubleTime());	/*Patch 310*/
	IN_Journal_Bypassed(Sys_DoubleTime());	/*Patch 307*/

	if (!in_jrn_vdx && !in_jrn_vdy &&
	    !in_jrn_vkpitch && !in_jrn_vkyaw &&
	    !in_jrn_vrawx && !in_jrn_vrawy &&
	    in_jrn_vhavelast &&
	    in_jrn_vlast[0] == viewangles[PITCH] &&
	    in_jrn_vlast[1] == viewangles[YAW])
	{	/*nothing moved and nothing turned -- see THE EMIT RULE above.
		  Patch 305 adds the keyboard terms to this test: a turn that was exactly
		  cancelled, or clamped away by CL_ClampPitch, leaves the angle equal and
		  would otherwise drop the line that says a key was doing something.

		  Patch 312 adds the RAW terms for the same reason, and it is the half the
		  emit rule was missing.  That rule already argues that an angle which
		  moved with no counts behind it is the signature rather than the noise;
		  counts that arrived and moved no angle are the identical fact from the
		  other side, and they were being dropped.  Without this a player who
		  opens the console mid-run leaves 'm' records with no 'v' to carry them,
		  they are orphaned into the NEXT window, and the check reports a break
		  against someone who did nothing wrong.  MEASURED: arm D of this patch's
		  falsifier orphaned exactly one 29-count batch on one console press.*/
		in_jrn_vflags = 0;
		return;
	}

	/*Patch 312: only when the pipeline changed the counts -- see the essay above
	  IN_Journal_ViewRaw.  Written BEFORE the 'v' it describes so a reader that
	  processes records in order already holds it when the 'v' arrives, the same
	  order the '!' and 'i' records use against the 'f' they qualify.*/
	if (in_jrn_vrawx != in_jrn_vdx || in_jrn_vrawy != in_jrn_vdy)
	{
		char rawtail[64];
		Q_snprintfz(rawtail, sizeof(rawtail), "%g %g", in_jrn_vrawx, in_jrn_vrawy);
		IN_Journal_Line(Sys_DoubleTime(), "d", rawtail);
	}

	/*Patch 305 appends kpitch/kyaw AFTER the existing five, so a pre-305 reader
	  that splits on whitespace and takes fields 0..4 is unaffected and an old
	  file simply has two fewer -- the same additive rule the header keys follow.*/
	Q_snprintfz(tail, sizeof(tail), "%g %g %i %.6f %.6f %.6f %.6f",
		in_jrn_vdx, in_jrn_vdy, in_jrn_vflags,
		viewangles[PITCH], viewangles[YAW],
		in_jrn_vkpitch, in_jrn_vkyaw);
	IN_Journal_Line(Sys_DoubleTime(), "v", tail);

	in_jrn_vlast[0] = viewangles[PITCH];
	in_jrn_vlast[1] = viewangles[YAW];
	in_jrn_vhavelast = true;
	in_jrn_vdx = in_jrn_vdy = 0;
	in_jrn_vkpitch = in_jrn_vkyaw = 0;
	in_jrn_vrawx = in_jrn_vrawy = 0;	/*Patch 312*/
	in_jrn_vflags = 0;
}

/*Called by IN_MoveMouse once per pointer, with the delta as it enters the
  sensitivity pipeline.  Summed rather than assigned: two mice both turn the one
  view, and the invariant is about the total.*/
void IN_Journal_ViewDelta(float dx, float dy, int flags)
{
	if (!in_jrn_buf || in_jrn_full)
		return;
	in_jrn_vdx += dx;
	in_jrn_vdy += dy;
	in_jrn_vflags |= flags;
}

/*FTESurf Patch 312 (plan item P294b): THE OTHER END OF THE SAME WINDOW.

WHAT PATCH 293 CANNOT DO, stated plainly because it is the reason this exists.
293 put the counts and the angle they produced on one line and the identity
    dyaw == -m_yaw*sensitivity*scale*dx
holds exactly.  But BOTH of its sides are computed from the engine's own mx, so
it tests the engine's multiplication and not the player.  A hook that rewrites
the accumulated delta UPSTREAM of mx -- which is where this entire class of cheat
lives, and is exactly what momentum.dll does to CInput::ApplyMouse -- gets a
conforming 293 record for free.  293's own essay says so.

The 'm' records are written by IN_Commands straight off the event ring.  mx is
read out of mouse->delta[] in IN_MoveMouse.  Those are the two ends of precisely
the window such a hook occupies, so A DISAGREEMENT BETWEEN THEM IS THAT HOOK.

MEASURED BEFORE ANY OF THIS WAS WRITTEN, because the plan required it: assuming a
frame correspondence is the mistake that made the .hid/.view cross-check
impossible.  Across every journal in the tree -- 45,447 'v' records including one
real 4976-tick PB -- the sum of the 'm' records in a window equals that window's
'v' delta on 45,384 of 45,384 covered frames, 100.000000%.  So the join already
worked and 'v' needed no sequence number.

WHY A RECORD AT ALL, THEN.  Because the same measurement found three ways an
HONEST player breaks it, and a check that accuses honest players is worse than no
check -- that is the Patch 305 lesson, where the flagship identity called this
game's core mechanic a cheat on 10,091 frames of a clean PB:

  - in_xflip (:2071 below) does `mx *= -1` upstream of the tap.  A player with it
    set produces v.dx == -sum(m.dx) on EVERY frame: a 100% break from a cvar that
    is neither CVAR_CHEAT nor archived.  MEASURED: arm B of this patch's
    falsifier, sum(m) 90 -> v.dx -157.5 against +157.5 in the control.
  - Key_MouseShouldBeFree() (:2076) zeroes mx before the tap, so the counts reach
    no angle, the quiet-frame return drops the 'v' line entirely, and the 'm'
    records are ORPHANED into the next window.  MEASURED: arm D, one console
    press orphaned exactly one 29-count batch.
  - the M_TOUCH branch applies a hardcoded `mx *= 1.75` (:2143, "boost
    sensitivity so that the default works okay").  MEASURED at exactly 1.75x.

THE FIX IS NOT A TABLE OF EXEMPTIONS.  It was tempting to have the reader know
about xflip and touch mode and the free cursor -- and that is precisely the
mistake Patch 293 already diagnosed: a reader that needs its own model of the
engine's internals drifts from the engine and silently starts measuring the wrong
thing.  Worse, the obvious span marker does not work.  The 'a' record would say
"absolute mode", but Patch 202 DEDUPES unchanged absolute positions for size, so
a run spent entirely ungrabbed carries ONE 'a' record for a state that lasted the
whole file.  A sparse record cannot mark a span -- the fifth time this tree has
landed on that lesson, after 301, 306, 307 and 310.

So the counts go in the file at the point they are still the ring's, and the
reader compares two numbers it was given.  No transform table, no exemptions, no
engine model.  Captured BEFORE the in_xflip flip deliberately: this is "what the
ring delivered", and anything the pipeline then does to it -- including a legal
sign flip -- is a difference the file states rather than one the reader assumes.

EMITTED ONLY WHEN IT DIFFERS, which is the whole size budget.  On honest grabbed
play raw == final on 100.000000% of measured frames, so the common case costs
nothing and the record's ABSENCE is the positive claim.  The same rule the '!',
'i', 'g', 'c' and 'b' records already follow.

ONE TAP RATHER THAN A FLAG AT EVERY DISCARD, and a red-team pass over this patch
is what settled it.  It found SEVEN more transforms between the 'm' record and
Patch 293's tap, six of them silent on an honest client: the Key_Dest_Has
(~kdm_game) zeroing, whose destinations are NOT all covered by flags bit 4
(key_dest_absolutemouse omits kdm_message, so opening chat discards counts with
flags reading 0); CSQC_MouseMove, which lets the mod's OWN progs swallow the
delta frame by frame and is on by default; the two menu mousemove consumers; the
weapon wheel, which returns true on the key alone regardless of its threshold;
and the M_TOUCH 1.75 boost.  The obvious design -- a flag bit set at each discard
site -- would have needed six correct edits, would have to be revisited by
everyone who ever adds a seventh site, and fails SILENTLY when someone does not.
Every one of those sites is DOWNSTREAM of this single line, so one tap covers all
six and covers the seventh nobody has written yet.  That is the whole argument
for putting it here instead.

WHAT THIS DOES NOT COVER, stated plainly so the patch is not oversold.  The check
spans the ring read and the delta read, so:
  - a hook that rewrites ptr[].delta between the drain and IN_MoveMouse is CAUGHT,
    and the residual is exactly the injected bias.  That is the bullet this patch
    is for, and it is the FTE shape of what momentum.dll already does.
  - a hook that edits the EVENT RING before the drain is NOT caught: the 'm'
    record is transcribed from the ring slot, so the edit is recorded as truth and
    both ends agree.  Same for the _GRID GetRawInputData pointer in in_win.c,
    which is a single static that can be overwritten with a data write.
  - anyone who can hook IN_Journal_Raw authors the whole file, 'm' records
    included, and no in-band check survives that.  That is the T4 ceiling and it
    is the reason this tree keeps three independent recordings rather than one.
  - the plugin API cannot mutate, only inject, so the join HOLDS there by
    construction and Patch 311's synth mark is the defence instead.

REJECTED, and recorded so it is not re-proposed: the first 'v' of a file is short
because in_journal_begin does not clear the pending ptr[].delta, and clearing it
would make the join exact from line one.  It would also THROW AWAY COUNTS THE
PLAYER ALREADY MADE.  The engine must not alter a player's aim, even by one
frame, to make its own evidence tidier; a reader exempting one frame per file
costs nothing and costs it in the right place.*/
void IN_Journal_ViewRaw(float dx, float dy)
{
	if (!in_jrn_buf || in_jrn_full)
		return;
	in_jrn_vrawx += dx;
	in_jrn_vrawy += dy;
}

/*FTESurf Patch 305: THE NON-MOUSE HALF OF THE ANGLE.

  MEASURED ON THE FIRST REAL RUN, and it is the reason this exists.  On a clean
  PB (bhop_eazy 0004976_pb, 42,973 'v' records) the Patch 293 identity
      dyaw == -m_yaw*sensitivity*scale*dx
  failed on 57.67% of frames.  Not noise: the residual is PERFECTLY BIMODAL with
  a three-decade empty gap.  With no turn key held, 30,929 of 30,934 frames sit
  under 1e-4 deg (median 2.0e-6).  With +left or +right held, 12,021 of 12,038
  sit in 1e-2..0.25 and NOT ONE is below 1e-4.  Dividing the residual by
  cl_yawspeed*elapsed gives a median of 1.0014, and the sign matches
  CL_AdjustAngles' convention 8,433 times out of 8,437 under +left.

  The angle was never wrong.  The RECORD was incomplete: 'v' logged the mouse
  counts and the resulting yaw, while CL_AdjustAngles had already added a
  keyboard turn to the same accumulator two lines earlier and nothing wrote it
  down.  Pitch proves it -- with cl_pitchspeed 0 the pitch residual's median is
  0.000000000 over the same 42,972 pairs, so the mouse path itself is exact to
  float precision and only the yaw term was missing.

  THIS IS NOT AN EXOTIC CASE.  +left/+right are SHIPPED DEFAULT BINDS
  (cfg/default.cfg:794,805) and the tree's own prestrafe calibration
  (default.cfg:409-425) is DERIVED from turning with them while holding a strafe
  key.  Keyboard turn is how a bhop run starts.  Without this column the
  flagship input check would have failed the opening second of essentially every
  honest run on this game -- a false accusation on 10,093 frames of this one.

  WHY THE DELTA AND NOT THE INGREDIENTS.  The obvious alternative is to log
  cl_yawspeed and let the reader re-derive the term.  That is wrong, and
  expensively so: a reader would have to re-implement CL_KeyState's 0/0.25/0.5/
  0.75/1.0 sub-frame fractions, cl_anglespeedkey, the in_strafe gate, in_rotate,
  r_xflip, the FPD_LIMIT_YAW/ruleset_allow_frj +-900 clamp, and the engine's
  frametime -- which is NOT the journal's own wall-clock delta.  Every one of
  those is a place for the reader and the engine to disagree, and it would break
  again the moment any of them changed.  Logging the realised change instead
  makes the identity exact arithmetic with no model in the reader at all:

      dyaw  ==  -m_yaw*sensitivity*scale*dx  +  kyaw

  BOTH AXES, though only yaw is broken today.  cl_pitchspeed defaults 0 here, so
  the pitch term is always 0 on this configuration -- but it is a cvar, and a
  reader must not have to know its value to trust the pitch column.  A field that
  is always zero costs two bytes and removes a silent dependency.

  IT IS NOT A TRUST BOUNDARY.  Like the P293 identity itself, both sides are
  computed by the same engine, so this closes the arithmetic rather than
  attesting the input.  What it buys is that a residual now MEANS something: with
  the keyboard term accounted for, an unexplained yaw is once again the signature
  the identity was written to surface, instead of being drowned by legitimate
  play.*/
void IN_Journal_ViewKeyboard(float dpitch, float dyaw)
{
	if (!in_jrn_buf || in_jrn_full)
		return;
	in_jrn_vkpitch += dpitch;
	in_jrn_vkyaw += dyaw;
}

/*FTESurf Patch 303: DEVICE PROVENANCE.

  Until now the journal recorded FTE's own devid and nothing else, so every device
  in the file was an anonymous small integer.  "devid 0 moved 4000 counts" is not
  evidence of anything: the reader cannot tell a real mouse from a virtual one,
  and that distinction is the whole point of keeping the file.

  Written through INS_EnumerateDevices rather than by reaching into in_win.c's
  rawmice[]/rawkbd[] arrays.  Those are static to the backend, and this file is the
  cross-platform one -- in_generic.o is in every client build while in_win.o is
  Windows-only.  The enumeration is a declared backend API (input.h) that every
  backend already implements, so this costs nothing on the platforms that have a
  real answer and degrades to whatever they do know on the ones that do not.

  THE CALLBACK MUST NOT WRITE THROUGH qdevid.  It is handed a pointer, and the
  other consumer of this API in this file -- IN_DeviceIDs_DoRemap -- assigns
  through it; a callback copied from that one would silently remap the player's
  devices as a side effect of opening a journal.

  THREE STATES, AND THEY ARE NOT TWO.  A NULL qdevid means the device cannot carry
  one at all (the "system" pseudo-devices); DEVID_UNSET means it can but has not
  been given one yet.  Neither is 0, and 0 is a real devid belonging to a real
  device -- so they print as `-` and `unset`.  Collapsing either into 0 would
  attribute one device's motion to another, which is the exact shape of error this
  file exists to make impossible.

  THE TABLE IS WRITTEN TWICE, AND THAT IS DELIBERATE.  Devids are allocated LAZILY
  at first use (in_win.c's Mouse_AllocateDevID, reached only once a device actually
  reports motion or a button), so at journal-begin almost every device still reads
  `unset` and the begin table cannot answer "which device produced devid 0".  What
  it does answer is what hardware was PRESENT when the run started, which is the
  anti-tamper half -- an injector's virtual device is in that list from the first
  line.  The `devmap` table at the end carries the resolved mapping.  A device that
  appears in one table and not the other was plugged or unplugged mid-run, and a
  reader should treat that as a fact about the run rather than as a parse error.

  PRIVACY, STATED PLAINLY BECAUSE IT IS NOT NOTHING.  On Windows the name is the
  device interface path -- VID, PID and an instance path that is stable for that
  device on that machine.  That is a pseudonymous hardware identifier.  It is
  recorded because it is precisely what makes provenance checkable, and it is
  another reason this file is consent-gated and uploaded only on demand rather
  than streamed.

  WHAT IT DOES NOT PROVE.  A device name is self-reported by the device.  An
  Arduino, Pico or KMBox presents whatever VID/PID string it likes and can clone a
  real mouse's exactly, so a name that looks legitimate is not attestation that the
  hardware is.  This raises the cost of the cheap end -- SendInput injection has no
  enumerated device at all, and a virtual driver has a name that says so -- and it
  leaves the hardware-replay ceiling exactly where it was.*/
static void IN_Journal_DeviceLine(void *vctx, const char *type, const char *devicename, unsigned int *qdevid)
{
	const char *tag = vctx;
	char quoted[2048];
	char line[2200];
	char id[16];

	if (!qdevid)
		Q_strncpyz(id, "-", sizeof(id));
	else if (*qdevid == DEVID_UNSET)
		Q_strncpyz(id, "unset", sizeof(id));
	else
		Q_snprintfz(id, sizeof(id), "%u", *qdevid);

	/*quoted because a Windows device path carries backslashes, braces and #, and
	  an unquoted one would need the reader to guess where the field ended.*/
	Q_snprintfz(line, sizeof(line), "%s %s %s %s\n", tag, type, id,
		COM_QuotedString(devicename, quoted, sizeof(quoted), false));
	IN_Journal_Raw(line);
}

static void IN_JournalBegin_f(void)
{
	/*Read by name rather than linked: both live in the platform backend
	  (in_win.c) as statics, and this file is the cross-platform one.  A missing
	  cvar reports as 0, which is the truthful answer on a platform that has no
	  raw input at all.*/
	cvar_t *raw = Cvar_FindVar("in_rawinput");
	cvar_t *rawkbd = Cvar_FindVar("in_rawinput_keyboard");
	cvar_t *nolegacy = Cvar_FindVar("in_rawinput_nolegacy");	/*Patch 307*/
	char head[1024];

	IN_Journal_Drop();

	in_jrn_max = in_journal_maxkb.value * 1024;
	if (in_jrn_max < 4096)
		in_jrn_max = 4096;
	in_jrn_cap = 65536;
	if (in_jrn_cap > in_jrn_max + 1024)
		in_jrn_cap = in_jrn_max + 1024;
	in_jrn_buf = BZ_Malloc(in_jrn_cap);
	in_jrn_len = 0;
	in_jrn_base = in_jrn_last = Sys_DoubleTime();
	in_jrn_dropreported = in_jrn_dropbase = in_jrn_dropped;
	/*Patch 306: clamped, so a backend that never counts leaves a 0 baseline and the
	  emitter's own -1 test is the single place that decides to stay silent.*/
	in_jrn_injreported = in_jrn_injbase = (in_raw_injected < 0) ? 0 : in_raw_injected;
	in_jrn_unenumreported = in_jrn_unenumbase = (in_raw_unenum < 0) ? 0 : in_raw_unenum;
	in_jrn_lgbreported = in_jrn_lgbbase = (in_raw_legacybtn < 0) ? 0 : in_raw_legacybtn;	/*Patch 307*/
	/*Patch 307: the header carries the state at begin, so only CHANGES from it are
	  worth a line.  Seeding from the live value means a run that never alt-tabs
	  writes no 'g' record at all.*/
	in_jrn_nolegacyreported = in_raw_nolegacy_live;
	in_jrn_vdx = in_jrn_vdy = 0;
	in_jrn_vkpitch = in_jrn_vkyaw = 0;	/*Patch 305*/
	in_jrn_vrawx = in_jrn_vrawy = 0;	/*Patch 312*/
	in_jrn_vflags = 0;
	in_jrn_vhavelast = false;

	/*Patch 293: the scale terms are recorded because WITHOUT THEM THE 'v' LINE
	  PROVES NOTHING.  The invariant is dyaw == -m_yaw*sensitivity*scale*dx, and a
	  reader that does not know the three constants can only check that the ratio
	  is SOME constant -- which a cheat holding a fixed multiplier would also pass.
	  They go after `synth 0` so IN_JournalSynth_f's strstr for "\nsynth 0\n" still
	  finds it; that rewrite is a fixed-width in-place poke and must not move.

	  in_sensitivityscale is a plain float rather than a cvar and the plugin API can
	  write it mid-run (plugin.h SetSensitivityScale), so this is its value AT THE
	  START and a reader must treat a run whose ratio steps as suspect rather than
	  as a measurement error.*/
	Q_snprintfz(head, sizeof(head),
		"FTESURF-HID 1\n"
		"map %s\n"
		"base %.6f\n"
		"rawinput %i\n"
		"rawkbd %i\n"
		"rawmice %i\n"
		"rawkbds %i\n"
		"nolegacy %i\n"
		"nolegacylive %i\n"
		"synth 0\n"
		"sensitivity %.9g\n"
		"sensitivityscale %.9g\n"
		"m_yaw %.9g\n"
		"m_pitch %.9g\n"
		"m_filter %.9g\n"
		"m_accel %.9g\n"
		"m_accel_style %i\n"
		"m_accel_power %.9g\n"
		"m_accel_offset %.9g\n"
		"m_accel_senscap %.9g\n",
		InfoBuf_ValueForKey(&cl.serverinfo, "map"),
		in_jrn_base,
		raw?raw->ival:0,
		rawkbd?rawkbd->ival:0,
		in_rawmice_live,	/*Patch 301: the GRANT, beside the request above*/
		in_rawkbd_live,
		nolegacy?nolegacy->ival:0,	/*Patch 307: the request...*/
		in_raw_nolegacy_live,		/*...and what it actually got*/
		sensitivity.value,
		in_sensitivityscale,
		m_yaw.value,
		m_pitch.value,
		m_filter.value,
		m_accel.value,
		m_accel_style.ival,
		m_accel_power.value,
		m_accel_offset.value,
		m_accel_senscap.value);
	IN_Journal_Raw(head);

	/*Patch 303: after the fixed block and before `begin`, so the header stays one
	  run of key/value lines and a reader that stops at `begin` still sees them.
	  `begin` therefore moves out of the snprintf above -- it cannot stay there,
	  because these lines have to land in front of it and the device list is
	  unbounded while that buffer is 1024 bytes.*/
	INS_EnumerateDevices((void*)"dev", IN_Journal_DeviceLine);

	/*Patch 310: the render-integrity table, beside the device table and before
	  `begin` for the same reason -- a reader that stops at `begin` has seen the
	  whole of what this run was played with.  Written as repeating `render`
	  lines rather than as header keys because a key/value header is a dict to
	  every reader that parses one, and a dict keeps the LAST of a repeated key:
	  eleven cvars would arrive as one.  The `dev` table learned this first.*/
	/*Patch 312 makes this two tables through one loop.  They are written under
	  DIFFERENT KEYS -- `render` and `input` -- because they answer different
	  questions (what the player could see, versus what their counts became) and a
	  reader must be able to tell them apart without a table of its own.  They
	  share the tracking arrays because 'c' is already generic over name+value, so
	  a mid-run change to either needs no new record kind at all.*/
	{
		static const char *const tables[2] = {"render", "input"};
		const char **names;
		char line[256], qval[64], qdef[64];
		int t, i;
		in_jrn_rcvcount = 0;
		for (t = 0; t < 2; t++)
		{
			names = t ? in_jrn_inputcvars : in_jrn_rendercvars;
			for (i = 0; names[i] && in_jrn_rcvcount < MAX_JRN_TRACKEDCVARS; i++)
			{
				cvar_t *v = Cvar_FindVar(names[i]);
				in_jrn_rcv[in_jrn_rcvcount] = v;
				in_jrn_rcvmod[in_jrn_rcvcount] = v?v->modifiedcount:0;
				in_jrn_rcvcount++;
				if (!v)
				{	/*the cvar does not exist in this build -- say so rather than
					  write a 0, which is a real value and would read as a
					  measurement.  Same rule as -1 on the counters above.*/
					Q_snprintfz(line, sizeof(line), "%s %s - -\n",
						tables[t], names[i]);
				}
				else
					Q_snprintfz(line, sizeof(line), "%s %s %s %s\n",
						tables[t], v->name,
						COM_QuotedString(v->string, qval, sizeof(qval), false),
						COM_QuotedString(v->defaultstr?v->defaultstr:v->enginevalue,
							qdef, sizeof(qdef), false));
				IN_Journal_Raw(line);
			}
		}
	}

	IN_Journal_Raw("begin\n");
}

static void IN_JournalEnd_f(void)
{
	const char *name, *fallback;
	char tail[128];
	double now;

	if (!in_jrn_buf)
	{
		if (Cmd_Argc() > 1)
			Con_Printf("in_journal_end: no journal open\n");
		return;
	}

	if (Cmd_Argc() < 2 || !*Cmd_Argv(1))
	{	/*no path means discard, and the discard branch is not optional -- without
		  it an abandoned run's buffer stays resident until the next begin.

		  It prints, because this is the branch a non-PB run takes and a headless
		  test otherwise has no way at all to see that the gamecode's end edge
		  fired: a discard leaves no file to inspect.*/
		Con_DPrintf("in_journal_end: discarded, %u events / %u frames\n",
			in_jrn_events, in_jrn_frames);
		IN_Journal_Drop();
		return;
	}

	/*ONE clock read, used for both halves.  Two calls here put the absolute in
	  the trailer and the dt that leads to it a couple of microseconds apart, so
	  the file disagreed with itself by exactly the gap between two adjacent QPC
	  reads -- which is a silly way to fail an exactness check that everything
	  else in the format works to make exact.*/
	now = Sys_DoubleTime();

	/*Patch 306: catch up the per-frame record before the totals below are written, so
	  the two agree.  Before the cap lift, for the same reason the devmap table is --
	  on a file that hit in_journal_maxkb this is dropped and the trailer's totals then
	  exceed the sum of the 'i' records.  That disagreement is not a fault: such a file
	  already says `truncated`, and a reader must not demand the cross-check on one.*/
	IN_Journal_Rejected(now);
	IN_Journal_Legacy(now);		/*Patch 307*/
	IN_Journal_RenderCvars(now);	/*Patch 310*/
	IN_Journal_Bypassed(now);	/*Patch 307*/

	/*The trailer carries the WHOLE-RUN totals beside the per-frame deltas on purpose:
	  two independent statements of the same quantity, so a hand-edited journal has to
	  be edited consistently in two places.  -1 is preserved rather than clamped -- on a
	  backend that does not count, "no measurement" is the true answer and a 0 here would
	  read as "none seen", which is the strictest conclusion drawn from the least
	  evidence.  Appended AFTER the existing five fields, so a pre-306 reader that takes
	  fields 0..4 is unaffected -- the same additive rule the 'v' line follows.*/
	Q_snprintfz(tail, sizeof(tail), "%.6f %u %u %u %u %i %i %i",
		now - in_jrn_base, in_jrn_events, in_jrn_frames,
		in_jrn_dropped - in_jrn_dropbase, in_jrn_hidden,
		(in_raw_injected  < 0) ? -1 : in_raw_injected  - in_jrn_injbase,
		(in_raw_unenum   < 0) ? -1 : in_raw_unenum    - in_jrn_unenumbase,
		(in_raw_legacybtn < 0) ? -1 : in_raw_legacybtn - in_jrn_lgbbase);	/*Patch 307*/
	/*Patch 303: the resolved devid->device mapping, which the header could not
	  carry because devids are handed out lazily at first use -- see the essay on
	  IN_Journal_DeviceLine.  DELIBERATELY BEFORE THE CAP IS LIFTED BELOW: the 1024
	  bytes of headroom that lift buys exist for the `truncated` and `end` lines,
	  and those outrank this table.  On a journal that hit the cap these lines are
	  dropped by IN_Journal_Raw's own growth check rather than competing for it,
	  and such a file already says `truncated`, so the loss is announced.*/
	INS_EnumerateDevices((void*)"devmap", IN_Journal_DeviceLine);

	/*the trailer is written whether or not the cap was hit -- a truncated file
	  still has to say how much it was missing.  in_jrn_max is lifted rather than
	  in_jrn_full cleared, so a second truncated marker cannot appear.*/
	in_jrn_full = false;
	in_jrn_max = in_jrn_cap;
	IN_Journal_Line(now, "end", tail);

	/*Deliberately NOT Cmd_IsInsecure()-guarded, unlike condump: CSQC has to be able
	  to call this and localcmd is RESTRICT_INSECURE by construction, so an exec-level
	  test would block the only caller there is.  The safety is in the path instead --
	  QC_FixFileName is the same sandbox PF_fopen uses, AND a data/ prefix is required
	  on top of it, because QC_FixFileName alone also accepts cfg/ and a journal must
	  not be able to overwrite a config.*/
	if (!QC_FixFileName(Cmd_Argv(1), &name, &fallback) || strncmp(name, "data/", 5))
	{
		Con_Printf("in_journal_end: refused \"%s\" -- journals go under data/\n", Cmd_Argv(1));
		IN_Journal_Drop();
		return;
	}

	COM_WriteFile(name, FS_GAMEONLY, in_jrn_buf, in_jrn_len);
	Con_DPrintf("in_journal_end: %s, %u events / %u frames / %u dropped / %u hidden, %uk\n",
		name, in_jrn_events, in_jrn_frames, in_jrn_dropped - in_jrn_dropbase, in_jrn_hidden, (unsigned)(in_jrn_len/1024));
	IN_Journal_Drop();
}

static void IN_JournalNote_f(void)
{
	if (Cmd_Argc() > 1)
		IN_Journal_Note(Cmd_Args());
}

/*A console +forward is a Cbuf command and never passes through in_newevent, so
  without this there is no way at all to exercise the key path, the m format or the
  ring overflow from a scripted config -- and this project does not ship what it
  cannot measure.  What makes it safe to ship is that it MARKS THE FILE: a journal
  containing injected events says synth 1 in its header and is inadmissible as
  evidence.  It does not need a cheat gate because it cannot forge a clean file.*/
/*FTESurf Patch 311: THE POISON, MOVED TO THE INJECTION POINT.

WHAT WAS WRONG.  in_jrn_synth was set by IN_JournalSynth_f because THE COMMAND set it,
not because the INJECTION did.  So the flag described one known caller rather than the
property it is named for, and every other way into IN_KeyEvent / IN_MouseMove produced a
journal that says `synth 0` -- a clean evidence file -- over forged input.

That is not hypothetical.  plugins/plugin.h:356-360 hands a plugin raw pointers to
IN_KeyEvent, IN_MouseMove, IN_JoystickAxisEvent, IN_Accelerometer and IN_Gyroscope
(common/plugin.c's input function table).  A plugin is an ordinary native DLL the player
can drop in; it needs no cheat gate, no memory patching and no hook.  It calls the same
function the Windows backend calls, the event lands in the same ring, and until now the
journal could not tell the difference.

SO THE MARK BELONGS WHERE THE EVENT ENTERS, and this function is that mark.  Anything
that is not a platform backend calls it first.  It is idempotent and costs one branch.

WHY NOT DETECT IT INSIDE IN_KeyEvent INSTEAD.  Because the honest test -- "did this come
from a backend?" -- is not answerable there: the backend and the plugin call the identical
function with identical arguments.  Any in-band answer would need every backend on every
platform to set a flag first, which is a correctness burden spread across code this patch
cannot test, and one missed backend silently poisons every honest journal on that
platform.  Marking at the few non-backend entries instead is a smaller, checkable set, and
it fails in the SAFE direction: a new injection route added later is un-marked (a gap to
close) rather than every honest run being accused (an accusation that cannot be undone).

WHAT IT STILL DOES NOT CATCH, and this is the ceiling rather than an oversight.  A native
DLL that detours IN_MouseMove itself, or patches the ring, runs BELOW this mark and is not
touched by it.  Neither is a hardware replay device.  This closes the SANCTIONED injection
route -- the one that needs no skill -- and does not pretend to close the others.*/
void IN_Journal_MarkSynth(const char *source)
{
	char note[64];

	if (!in_jrn_buf || in_jrn_synth)
		return;		/*no journal open, or already poisoned -- idempotent*/

	/*rewrite the header's synth key in place -- it is a fixed-width "0" by
	  construction, so this cannot move anything after it.*/
	{
		char *at = strstr(in_jrn_buf, "\nsynth 0\n");
		if (at)
			at[7] = '1';
	}
	in_jrn_synth = true;

	/*NAME THE SOURCE.  `synth 1` alone says the file is inadmissible but not why,
	  and "a developer ran in_journal_synth to test the format" and "a plugin
	  forged input" are the same flag with very different meanings.  Written as a
	  note rather than a new header key because the header is already closed by
	  the time this can fire.*/
	/*space-separated, not `source=plugin`: IN_Journal_Note's sanitiser allows
	  only alphanumerics, space, _ . and -, so an '=' arrives as '?'.  Measured on
	  the first run of this patch's own falsifier.*/
	Q_snprintfz(note, sizeof(note), "SYNTH source %s", source?source:"unknown");
	IN_Journal_Note(note);
}

static void IN_JournalSynth_f(void)
{
	int n = (Cmd_Argc() > 1) ? atoi(Cmd_Argv(1)) : 1;
	int i;

	if (n < 1)
		n = 1;
	if (n > 100000)
		n = 100000;

	IN_Journal_MarkSynth("in_journal_synth");

	for (i = 0; i < n; i++)
	{
		/*x is never zero: IN_MouseMove early-returns when every axis is 0
		  (in_generic.c, the !abs && !x && !y && !z test), so a delta pair that
		  could be (0,0) would make the count printed below a lie.*/
		IN_MouseMove(0, false, (i%7)+1, (i%5)-2, 0, 0);
		if (!(i % 16))
			IN_KeyEvent(0, (i/16)&1, K_SPACE, 0);
	}
	Con_Printf("in_journal_synth: injected %i mouse and %i key events\n", n, (n+15)/16);
}

/*a 'pointer' is either a multitouch pointer, or a separate device
note that mice use the keyboard button api, but separate devices*/
void IN_Commands(void)
{
	struct eventlist_s *ev;

	INS_Commands();

	/*FTESurf Patch 326: republish the grant, AFTER INS_Commands so a backend that
	  re-enumerated this frame is already reflected and before the journal's frame
	  marker below, so the .hid and the cvar can never disagree within a frame.

	  Guarded on inequality because Cvar_ForceSetValue rebuilds the string and runs
	  the callbacks: this path runs every frame at several hundred fps and the value
	  changes about twice per session.*/
	if (in_rawmice.ival != in_rawmice_live)
		Cvar_ForceSetValue(&in_rawmice, in_rawmice_live);
	if (in_rawkbds.ival != in_rawkbd_live)
		Cvar_ForceSetValue(&in_rawkbds, in_rawkbd_live);

	/*Patch 202: the frame marker, and the per-event append below, both sit ABOVE
	  the IEV_MOUSEDELTA case's ptr[].delta summation -- which is the whole point.
	  By the time that runs the individual reports have been added together and
	  are unrecoverable.  Emitted only on a non-empty drain, so an idle frame
	  costs nothing and the .view already carries one line per rendered frame.*/
	if (in_jrn_buf && events_used != events_avail)
		IN_Journal_Frame();

	while (events_used != events_avail)
	{
		ev = &eventlist[events_used & (EVENTQUEUELENGTH-1)];

		if (in_jrn_buf)
			IN_Journal_Event(ev);

		switch(ev->type)
		{
		case IEV_KEYRELEASE:
//Con_Printf("IEV_KEYDOWN %i: %i '%c'\n", ev->devid, ev->keyboard.scancode, ev->keyboard.unicode?ev->keyboard.unicode:' ');
			if (ev->keyboard.scancode == -1)
			{
				int i;
				for (i = 0; i < K_MAX; i++)
					Key_Event(ev->devid, i, 0, false);
				break;
			}
			if ((ev->keyboard.scancode == K_MOUSE1||ev->keyboard.scancode==K_TOUCH) && ev->devid < MAXPOINTERS && (ptr[ev->devid].type == M_TOUCH))
			{	//touch (or abs clicks)
				struct mouse_s *m = &ptr[ev->devid];
				if (touchcursor == ev->devid)
					touchcursor = -1;	//revert it to the mouse, or whatever device was 0.

				if (ev->keyboard.scancode==K_TOUCH && m->touchtime && m->touchkey==K_TOUCH)
				{	//convert to tap/longpress if it wasn't already a gesture. don't ever convert mouse.
					if (Sys_DoubleTime()-m->touchtime > m_longpressthreshold.value)
						m->touchkey = K_TOUCHLONG;
					else
						m->touchkey = K_TOUCHTAP;
					Key_Event(ev->devid, m->touchkey, 0, true);
				}
				if (m->touchkey && m->touchkey!=ev->keyboard.scancode)
					Key_Event(ev->devid, m->touchkey, 0, false);	//and release...

				//reset it.
				m->touchkey = 0;
				m->touchtime = 0;
				m->moveddist = 0;
			}
			Key_Event(ev->devid, ev->keyboard.scancode, ev->keyboard.unicode, false);
			break;
		case IEV_KEYDOWN:
//Con_Printf("IEV_KEYDOWN %i: %i '%c'\n", ev->devid, ev->keyboard.scancode, ev->keyboard.unicode?ev->keyboard.unicode:' ');
			if ((ev->keyboard.scancode == K_MOUSE1||ev->keyboard.scancode==K_TOUCH) && ev->devid < MAXPOINTERS && (ptr[ev->devid].type == M_TOUCH))
			{	//touch (or abs clicks)
				struct mouse_s *m = &ptr[ev->devid];
				float fl;
				touchcursor = ev->devid;
				fl = m->oldpos[0] * vid.width / vid.pixelwidth;		mousecursor_x = bound(0, fl, vid.width-1);
				fl = m->oldpos[1] * vid.height / vid.pixelheight;	mousecursor_y = bound(0, fl, vid.height-1);

				m->touchtime = Sys_DoubleTime();
				m->moveddist = 0;
				if (ev->keyboard.scancode==K_TOUCH)
					m->touchkey = K_TOUCH;
				else
					m->touchkey = 0;
			}
			Key_Event(ev->devid, ev->keyboard.scancode, ev->keyboard.unicode, true);
			break;
		case IEV_JOYAXIS:
			if (ev->devid < MAXJOYSTICKS && ev->joy.axis>=0 && ev->joy.axis<MAXJOYAXIS)
			{
				if (topmenu && topmenu->joyaxis && topmenu->joyaxis(topmenu, ev->devid, ev->joy.axis, ev->joy.value))
					joy[ev->devid].axis[ev->joy.axis] = 0;
#ifdef CSQC_DAT
				else if (CSQC_JoystickAxis(ev->joy.axis, ev->joy.value, ev->devid))
					joy[ev->devid].axis[ev->joy.axis] = 0;
#endif
				else
					joy[ev->devid].axis[ev->joy.axis] = ev->joy.value;
			}
			break;
		case IEV_MOUSEDELTA:
//Con_Printf("IEV_MOUSEDELTA %i: %f %f\n", ev->devid, ev->mouse.x, ev->mouse.y);
			if (ev->devid < MAXPOINTERS)
			{
				if (ev->mouse.x || ev->mouse.y)
					ptr[ev->devid].updated = true;
				if (ptr[ev->devid].type != M_MOUSE)
				{
					ptr[ev->devid].type = M_MOUSE;
				}
				ptr[ev->devid].delta[0] += ev->mouse.x;
				ptr[ev->devid].delta[1] += ev->mouse.y;
				ptr[ev->devid].rawpend[0] += ev->mouse.x;	//Patch 376
				ptr[ev->devid].rawpend[1] += ev->mouse.y;

				//if we're emulating a cursor, make sure that's updated too.
				if (touchcursor < 0 && !vrui.enabled && Key_MouseShouldBeFree())
				{
					mousecursor_x += ev->mouse.x;
					mousecursor_y += ev->mouse.y;
					mousecursor_x = bound(0, mousecursor_x, vid.pixelwidth-1);
					mousecursor_y = bound(0, mousecursor_y, vid.pixelheight-1);
				}
				ptr[ev->devid].oldpos[0] = mousecursor_x;
				ptr[ev->devid].oldpos[1] = mousecursor_y;

				if (m_forcewheel.value >= 2)
					ptr[ev->devid].wheeldelta -= ev->mouse.z;
				else if (m_forcewheel.value)
				{
					int mfwt = (int)m_forcewheel_threshold.value;

					if (ev->mouse.z > mfwt)
						ptr[ev->devid].wheeldelta -= mfwt;
					else if (ev->mouse.z < -mfwt)
						ptr[ev->devid].wheeldelta += mfwt;
				}

				if (ev->mouse.x || ev->mouse.y)
					ptr[ev->devid].updates++;
			}
			break;
		case IEV_MOUSEABS:
//Con_Printf("IEV_MOUSEABS %i: %f %f\n", ev->devid, ev->mouse.x, ev->mouse.y);
			/*mouse cursors only really work with one pointer*/
			if (!vrui.enabled)
			if (ev->devid == touchcursor || (touchcursor < 0 && ev->devid < MAXPOINTERS && (ptr[ev->devid].oldpos[0] != ev->mouse.x || ptr[ev->devid].oldpos[1] != ev->mouse.y)))
			{
				float fl;
				fl = ev->mouse.x * vid.width / vid.pixelwidth;
				mousecursor_x = bound(0, fl, vid.width-1);
				fl = ev->mouse.y * vid.height / vid.pixelheight;
				mousecursor_y = bound(0, fl, vid.height-1);
			}

			if (ev->devid < MAXPOINTERS)
			{
				struct mouse_s *m = &ptr[ev->devid];
				if (m->type != M_TOUCH)
				{
					//if its now become an absolute device, clear stuff so we don't get confused.
					m->type = M_TOUCH;
					m->touchtime = 0;
					m->touchkey = 0;
					m->moveddist = 0;
					m->oldpos[0] = ev->mouse.x;
					m->oldpos[1] = ev->mouse.y;
				}

				if (m->touchtime)
				{	//only do this when its actually held in some form...
					m->delta[0] += ev->mouse.x - m->oldpos[0];
					m->delta[1] += ev->mouse.y - m->oldpos[1];
					m->rawpend[0] += ev->mouse.x - m->oldpos[0];	//Patch 376
					m->rawpend[1] += ev->mouse.y - m->oldpos[1];
		
					m->moveddist += fabs(ev->mouse.x - m->oldpos[0]) + fabs(ev->mouse.y - m->oldpos[1]);
				}

				if (ev->mouse.x != m->oldpos[0] ||
					ev->mouse.y != m->oldpos[1])
				{
					m->updates++;
					m->updated = true;
				}

				m->oldpos[0] = ev->mouse.x;
				m->oldpos[1] = ev->mouse.y;

				if (m->touchtime && m->touchkey == K_TOUCH)
				{
					if (Sys_DoubleTime()-m->touchtime > 1)
						m->touchkey = K_TOUCHLONG;	//held for long enough...
					else if (m->moveddist >= m_slidethreshold.value)
						m->touchkey = K_TOUCHSLIDE;	//moved far enough to consitute a slide
					else
						break;
					Key_Event(ev->devid, m->touchkey, 0, true);
				}
			}
			break;

		case IEV_ACCELEROMETER:
			//down: x= +9.8
			//left: y= -9.8
			//up:   z= +9.8
#ifdef CSQC_DAT
			CSQC_Accelerometer(ev->accel.x, ev->accel.y, ev->accel.z);
#endif
			break;
		case IEV_GYROSCOPE:
#ifdef CSQC_DAT
			CSQC_Gyroscope(ev->gyro.pitch * 180.0/M_PI, ev->gyro.yaw * 180.0/M_PI, ev->gyro.roll * 180.0/M_PI);
#endif
			break;
		}
		events_used++;
	}
}

/*FTESurf Patch 376: per seat, since the process began: what the ring delivered, what
  IN_MoveMouse read, and the reports raw input rejected (Patch 306).  cl_input.c
  sends them in-band, so the Patch 312 counts join runs on the server's own file.*/
static double in_cntring[MAX_SPLITS][2], in_cntread[MAX_SPLITS][2];
void IN_CountsGet(int pnum, double *ring, double *read, int *rejected)
{
	ring[0] = in_cntring[pnum][0];
	ring[1] = in_cntring[pnum][1];
	read[0] = in_cntread[pnum][0];
	read[1] = in_cntread[pnum][1];
	*rejected = (in_raw_injected > 0 ? in_raw_injected : 0) + (in_raw_unenum > 0 ? in_raw_unenum : 0);
}

void IN_MoveMouse(struct mouse_s *mouse, float *movements, int pnum, float frametime)
{
	float mx, my;
	double mouse_x, mouse_y, mouse_deltadist;
	int mfwt;
	qboolean strafe_x, strafe_y;
	int wpnum;
#ifdef CSQC_DAT
#ifdef MULTITHREAD
	extern qboolean runningindepphys;
#else
	const qboolean runningindepphys = false;
#endif
#endif

	//small performance boost
	if (mouse->type == M_INVALID)
		return;

	/*each device will be processed when its player comes to be processed*/
	wpnum = cl.splitclients;
	if (wpnum < 1)
		wpnum = 1;
	if (cl_forceseat.ival)
		wpnum = (cl_forceseat.ival-1) % wpnum;
	else
		wpnum = mouse->qdeviceid % wpnum;
	if (wpnum != pnum)
		return;

	if (m_forcewheel.value)
	{
		mfwt = m_forcewheel_threshold.ival;
		if (mfwt)
		{
			while(mouse->wheeldelta <= -mfwt)
			{
				Key_Event (mouse->qdeviceid, K_MWHEELUP, 0, true);
				Key_Event (mouse->qdeviceid, K_MWHEELUP, 0, false);
				mouse->wheeldelta += mfwt;
			}

			while(mouse->wheeldelta >= mfwt)
			{
				Key_Event (mouse->qdeviceid, K_MWHEELDOWN, 0, true);
				Key_Event (mouse->qdeviceid, K_MWHEELDOWN, 0, false);
				mouse->wheeldelta -= mfwt;
			}
		}

		if (m_forcewheel.value < 2)
			mouse->wheeldelta = 0;
	}

	mx = mouse->delta[0];
	mouse->delta[0]=0;
	my = mouse->delta[1];
	mouse->delta[1]=0;

	/*FTESurf Patch 312.  HERE, one line after the read and BEFORE the in_xflip
	  flip below, because this is the last instant mx is still nothing but the sum
	  of the reports the ring delivered.  Patch 293's tap is 150 lines down, at the
	  other end of every transform; the gap between the two is exactly the window a
	  delta-mutating hook occupies, and recording both ends is the whole patch.
	  See the essay above IN_Journal_ViewRaw.

	  Not moved above the two early-returns at the top of this function: a pointer
	  this seat does not consume contributes nothing to this view, and counting it
	  here would manufacture a disagreement on splitscreen that means nothing.*/
	IN_Journal_ViewRaw(mx, my);

	/*FTESurf Patch 376: the same two ends, summed for the server.  An honest client
	  reads exactly what the ring delivered, so the sums are equal; a hook that
	  rewrites delta between the drain and this read is their difference.*/
	in_cntring[pnum][0] += mouse->rawpend[0];
	in_cntring[pnum][1] += mouse->rawpend[1];
	mouse->rawpend[0] = mouse->rawpend[1] = 0;
	in_cntread[pnum][0] += mx;
	in_cntread[pnum][1] += my;

	if(in_xflip.value) mx *= -1;

	mousemove_x += mx;
	mousemove_y += my;

	if (!vrui.enabled && Key_MouseShouldBeFree())
		mx=my=0;

	if (mouse->type == M_TOUCH)
	{
		qboolean strafing = false;

		if (mouse->touchtime && mouse->touchkey==K_TOUCH)
		{	//convert to tap/longpress if it wasn't already a gesture. don't ever convert mouse.
			if (Sys_DoubleTime()-mouse->touchtime > m_longpressthreshold.value)
			{	//might as well trigger this here...
				mouse->touchkey = K_TOUCHLONG;
				Key_Event(mouse-ptr, mouse->touchkey, 0, true);
			}
		}


		switch(m_touchstrafe.ival)
		{
		case 2:
			strafing = mouse->heldpos[0] < vid.pixelwidth/2;
			break;
		default:
			strafing = mouse->heldpos[0] > vid.pixelwidth/2;
			break;
		case 0:
			strafing = false;
			break;
		}
		if (strafing && movements != NULL && !Key_Dest_Has(~kdm_game))
		{
			//if they're strafing, calculate the speed to move at based upon their displacement
			if (mouse->touchtime)
			{
				if (m_touchstrafe.ival == 2)	//left side
					mx = mouse->oldpos[0] - (vid.pixelwidth*1)/4.0;
				else	//right side
					mx = mouse->oldpos[0] - (vid.pixelwidth*3)/4.0;
				my = mouse->oldpos[1] - (vid.pixelheight*3)/4.0;

				//mx = (mouse->oldpos[0] - mouse->heldpos[0])*0.1;
				//my = (mouse->oldpos[1] - mouse->heldpos[1])*0.1;
			}
			else
			{
				mx = 0;
				my = 0;
			}

			if (m_touchmajoraxis.ival)
			{
				//major axis only
				if (fabs(mx) > fabs(my))
					my = 0;
				else
					mx = 0;
			}

			strafe_x = true;
			strafe_y = true;
		}
		else
		{
			strafe_x = false;
			strafe_y = false;

			//boost sensitivity so that the default works okay.
			mx *= 1.75;
			my *= 1.75;

#ifdef QUAKESTATS
			if (IN_WeaponWheelAccumulate(pnum, mx, my, 0))
				mx = my = 0;
#endif
		}
	}
	else
	{
		strafe_x = (in_strafe.state[pnum] & 1) || (lookstrafe.value && (in_mlook.state[pnum] & 1) );
		strafe_y = !((in_mlook.state[pnum] & 1) && !(in_strafe.state[pnum] & 1));
	}

	if (mouse->type == M_TOUCH || Key_MouseShouldBeFree())
	{
		if (mouse->updated)
		{	//many mods assume only a single mouse device.
			//when we have multiple active abs devices, we need to avoid sending all of them, because that just confuses everyone. such mods will see only devices that are actually moving, so uni-cursor mods will see only the one that moved most recently.
			mouse->updated = false;
			if (!runningindepphys)
			{
				if ((promptmenu && promptmenu->mousemove && promptmenu->mousemove(topmenu, true, mouse->qdeviceid, mouse->oldpos[0], mouse->oldpos[1])) ||
					(topmenu && topmenu->mousemove && topmenu->mousemove(topmenu, true, mouse->qdeviceid, mouse->oldpos[0], mouse->oldpos[1])))
				{
					mx = 0;
					my = 0;
				}
#ifdef CSQC_DAT
				if (!runningindepphys && CSQC_MousePosition(mouse->oldpos[0], mouse->oldpos[1], mouse->qdeviceid))
				{
					mx = 0;
					my = 0;
				}
#endif
			}
		}
	}
	else
	{
		if (Key_Dest_Has(kdm_menu))
		if (mx || my)
		if (!runningindepphys && topmenu && topmenu->mousemove && topmenu->mousemove(topmenu, false, mouse->qdeviceid, mx, my))
		{
			mx = 0;
			my = 0;
		}

#ifdef CSQC_DAT
		if (mx || my)
		if (!runningindepphys && CSQC_MouseMove(mx, my, mouse->qdeviceid))
		{
			mx = 0;
			my = 0;
		}
#endif

		//if game is not focused, kill any mouse look
		if (
#ifdef QUAKESTATS
			IN_WeaponWheelAccumulate(pnum, mx, my, 0) ||
#endif
			Key_Dest_Has(~kdm_game))
		{
			mx = 0;
			my = 0;
		}
	}

	/*FTESurf Patch 293.  HERE, and not at the `mx = mouse->delta[0]` grab far
	  above, because between the two there are four places that legitimately
	  discard the delta -- the Key_MouseShouldBeFree zeroing, the touch major-axis
	  and weapon-wheel branches, the menu mousemove consumers, and CSQC_MouseMove --
	  and counts logged before them would be counts the angle never saw, which reads
	  as a broken invariant on a clean client.  This is the last point at which
	  mx/my are still integer device counts and the first at which they are final.*/
	IN_Journal_ViewDelta(mx, my,
		(strafe_x?1:0) | (strafe_y?2:0) | (Key_MouseShouldBeFree()?4:0));

	if (m_filter.value)
	{
		double fraction = bound(0, m_filter.value, 2) * 0.5;
		mouse_x = (mx*(1-fraction) + mouse->old_delta[0]*fraction);
		mouse_y = (my*(1-fraction) + mouse->old_delta[1]*fraction);
	}
	else
	{
		mouse_x = mx;
		mouse_y = my;
	}

	mouse->old_delta[0] = mx;
	mouse->old_delta[1] = my;

	if (m_accel.value)
	{
		if (m_accel_style.ival && frametime)
		{
			float accelsens = sensitivity.value*in_sensitivityscale;
			float mousespeed = (sqrt (mx * mx + my * my)) / (1000.0f * (float) frametime);
			mousespeed -= m_accel_offset.value;
			if (mousespeed > 0)
			{
				mousespeed *= m_accel.value;
				if (m_accel_power.value > 1)
					accelsens += exp((m_accel_power.value - 1) * log(mousespeed));
				else
					accelsens = 1;
			}
			if (m_accel_senscap.value > 0 && accelsens > m_accel_senscap.value)
				accelsens = m_accel_senscap.value;
			mouse_x *= accelsens;
			mouse_y *= accelsens;
		}
		else
		{
			mouse_deltadist = sqrt(mx*mx + my*my);
			mouse_x *= (mouse_deltadist*m_accel.value + sensitivity.value*in_sensitivityscale);
			mouse_y *= (mouse_deltadist*m_accel.value + sensitivity.value*in_sensitivityscale);
		}
	}
	else
	{
		mouse_x *= sensitivity.value*in_sensitivityscale;
		mouse_y *= sensitivity.value*in_sensitivityscale;
	}

/*
#ifdef QUAKESTATS
	if (cl.playerview[pnum].statsf[STAT_VIEWZOOM])
	{
		mouse_x *= cl.playerview[pnum].statsf[STAT_VIEWZOOM]/STAT_VIEWZOOM_SCALE;
		mouse_y *= cl.playerview[pnum].statsf[STAT_VIEWZOOM]/STAT_VIEWZOOM_SCALE;
	}
#endif
*/

	if (!movements || cl.disablemouse)
	{
		return;
	}

	if (r_xflip.ival)
		mouse_x *= -1;

// add mouse X/Y movement to cmd
	if (strafe_x)
		movements[1] += m_side.value * mouse_x;
	else
	{
//		if ((int)((cl.viewangles[pnum][PITCH]+89.99)/180) & 1)
//			mouse_x *= -1;
		cl.playerview[pnum].viewanglechange[YAW] -= m_yaw.value * mouse_x;
	}

	if (in_mlook.state[pnum] & 1)
		V_StopPitchDrift (&cl.playerview[pnum]);

	if (!strafe_y)
	{
		cl.playerview[pnum].viewanglechange[PITCH] += m_pitch.value * mouse_y;
	}
	else
	{
		if ((in_strafe.state[pnum] & 1) && noclip_anglehack)
			movements[2] -= m_forward.value * mouse_y;
		else
			movements[0] -= m_forward.value * mouse_y;
	}
}

//rescales threshold-1 down 0-1
static float joydeadzone(float mag, float deadzone)
{
	if (joy_radialdeadzone.ival == 2)	//hacky overload to disable dead zones where the system provides it instead.
		deadzone = 0;

	if (mag > 1)	//erg?
		mag = 1;
	if (mag > deadzone)
	{
		mag -= deadzone;
		mag = mag / (1.f-deadzone);

		mag = pow(mag, joy_exponent.value);
	}
	else
		mag = 0;
	return mag;
}

void IN_MoveJoystick(struct joy_s *joy, float *movements, int pnum, float frametime)
{
	float mag;
	vec3_t jlook, jstrafe;

	int wpnum, i;
	for (i = 0; i < MAXJOYAXIS; i++)
		if (joy->axis[i])
			break;
	if (i == MAXJOYAXIS)
		return;

	/*each device will be processed when its player comes to be processed*/
	wpnum = cl.splitclients;
	if (wpnum < 1)
		wpnum = 1;
	if (cl_forceseat.ival)
		wpnum = (cl_forceseat.ival-1) % wpnum;
	else
		wpnum = joy->qdeviceid % wpnum;
	if (wpnum != pnum)
		return;

	memset(jstrafe, 0, sizeof(jstrafe));
	memset(jlook, 0, sizeof(jlook));

	for (i = 0; i < 6; i++)
	{
		int ax = joy_advaxis[i].ival;
		switch(ax)
		{
		default:
		case 0:	//dead axis
			break;
		case 1:
		case 3:
		case 5:
			jstrafe[(ax-1)/2] += joy->axis[i] * joy_advaxisscale[i].value;
			break;
		case -1:
		case -3:
		case -5:
			jstrafe[(-ax-1)/2] -= joy->axis[i] * joy_advaxisscale[i].value;
			break;

		case 2:
		case 4:
		case 6:
			jlook[(ax-2)/2] += joy->axis[i] * joy_advaxisscale[i].value;
			break;
		case -2:
		case -4:
		case -6:
			jlook[(-ax-2)/2] -= joy->axis[i] * joy_advaxisscale[i].value;
			break;
		}
	}

	if (joy_radialdeadzone.ival)
	{
		//uses a radial deadzone for x+y axis, and separate out the z axis, just because most controllers are 2d affairs with any 3rd axis being a separate knob.
		//deadzone values are stolen from microsoft's xinput documentation. they seem quite large to me - I guess that means that xbox controllers are just dodgy imprecise crap with excessive amounts of friction and finger grease.
		float basemag = sqrt(jlook[0]*jlook[0] + jlook[1]*jlook[1]);
		if (basemag)
		{
			mag = joydeadzone(basemag, sqrt(joy_anglethreshold[0].value*joy_anglethreshold[0].value + joy_anglethreshold[1].value*joy_anglethreshold[1].value));
			jlook[0] = (jlook[0]/basemag) * mag;
			jlook[1] = (jlook[1]/basemag) * mag;
		}
		else
			jlook[0] = jlook[1] = 0;
		mag = joydeadzone(fabs(jlook[2]), joy_anglethreshold[2].value);
		jlook[2] = mag;

		basemag = sqrt(jstrafe[0]*jstrafe[0] + jstrafe[1]*jstrafe[1]);
		if (basemag)
		{
			mag = joydeadzone(basemag, sqrt(joy_movethreshold[0].value*joy_movethreshold[0].value + joy_movethreshold[1].value*joy_movethreshold[1].value));
			jstrafe[0] = (jstrafe[0]/basemag) * mag;
			jstrafe[1] = (jstrafe[1]/basemag) * mag;
		}
		else
			jstrafe[0] = jstrafe[1] = 0;
		mag = joydeadzone(fabs(jstrafe[2]), joy_movethreshold[2].value);
		jstrafe[2] = mag;
	}
	else
	{
		for (i = 0; i < 3; i++)
		{
			mag = joydeadzone(fabs(jlook[i]), joy_anglethreshold[i].value);
			jlook[i] = ((jlook[i]<0)?-1:1)*mag;

			mag = joydeadzone(fabs(jstrafe[i]), joy_movethreshold[i].value);
			jstrafe[i] = ((jstrafe[i]<0)?-1:1)*mag;
		}
	}

#ifdef QUAKESTATS
	if (IN_WeaponWheelAccumulate(joy->qdeviceid, jstrafe[1]*50, -jstrafe[0]*50, 20))
		jstrafe[0] = jstrafe[1] = 0;
	if (IN_WeaponWheelAccumulate(joy->qdeviceid, jlook[1]*50, jlook[0]*50, 20))
		jlook[0] = jlook[1] = 0;
#endif

	if (Key_Dest_Has(~kdm_game))
	{
		VectorClear(jlook);
		VectorClear(jstrafe);
	}
	if (r_xflip.ival)
		jlook[0] *= -1, jstrafe[0] *= -1;

	if (in_speed.state[pnum] & 1)
	{
		VectorScale(jlook, 360*cl_movespeedkey.value, jlook);
		VectorScale(jstrafe, 360*cl_movespeedkey.value, jstrafe);
	}
	VectorScale(jlook, 360*frametime*in_sensitivityscale, jlook);

	if (!movements)	//if this is null, gamecode should still get inputs, just no camera looking or anything.
		return;

	//angle changes
	cl.playerview[pnum].viewanglechange[PITCH] += joy_anglesens[0].value * jlook[0] * in_sensitivityscale;
	cl.playerview[pnum].viewanglechange[YAW] -= joy_anglesens[1].value * jlook[1] * in_sensitivityscale;
	cl.playerview[pnum].viewanglechange[ROLL] += joy_anglesens[2].value * jlook[2] * in_sensitivityscale;

	if (in_mlook.state[pnum] & 1)
		V_StopPitchDrift (&cl.playerview[pnum]);

	//movement
	mag = 1;
	if ((in_speed.state[pnum] & 1) ^ cl_run.ival)
		mag *= cl_movespeedkey.value;
	movements[0] += joy_movesens[0].value * mag*cl_forwardspeed.value * jstrafe[0];
	movements[1] += joy_movesens[1].value * mag*cl_sidespeed.value * jstrafe[1];
	movements[2] += joy_movesens[2].value * mag*cl_upspeed.value * jstrafe[2];
}

void IN_Move (float *nudgemovements, float *absmovements, int pnum, float frametime)
{
	int i;
	for (i = 0; i < MAXPOINTERS; i++)
		IN_MoveMouse(&ptr[i], nudgemovements, pnum, frametime);

	for (i = 0; i < MAXJOYSTICKS; i++)
		IN_MoveJoystick(&joy[i], absmovements, pnum, frametime);
}

void IN_JoystickAxisEvent(unsigned int devid, int axis, float value)
{
	struct eventlist_s *ev = in_newevent();
	if (!ev)	
		return;
	ev->type = IEV_JOYAXIS;
	ev->devid = devid;
	ev->joy.axis = axis;
	ev->joy.value = value;
	in_finishevent();
}

void IN_KeyEvent(unsigned int devid, int down, int keycode, int unicode)
{
	struct eventlist_s *ev = in_newevent();
	if (!ev)
		return;
	ev->type = down?IEV_KEYDOWN:IEV_KEYRELEASE;
	ev->devid = devid;
	ev->keyboard.scancode = keycode;
	ev->keyboard.unicode = unicode;
	in_finishevent();
}

/*
devid is the mouse device id. generally idependant from keyboards.
for multitouch, devid might be the touch identifier, which will persist until released.
x is horizontal, y is vertical.
z is height... generally its used as a mousewheel instead, but there are some '3d' mice out there, so its provided in this api.
*/
void IN_MouseMove(unsigned int devid, int abs, float x, float y, float z, float size)
{
	struct eventlist_s *ev;
	if (!abs && !x && !y && !z)
		return;	//ignore non-movements
	ev = in_newevent();
	if (!ev)
		return;
	ev->devid = devid;
	ev->type = abs?IEV_MOUSEABS:IEV_MOUSEDELTA;
	ev->mouse.x = x;
	ev->mouse.y = y;
	ev->mouse.z = z;
	ev->mouse.tsize = size;
	in_finishevent();
}

void IN_Accelerometer(unsigned int devid, float x, float y, float z)
{
	struct eventlist_s *ev = in_newevent();
	if (!ev)
		return;
	ev->devid = devid;
	ev->type = IEV_ACCELEROMETER;
	ev->accel.x = x;
	ev->accel.y = y;
	ev->accel.z = z;
	in_finishevent();
}
void IN_Gyroscope(unsigned int devid, float pitch, float yaw, float roll)
{
	struct eventlist_s *ev = in_newevent();
	if (!ev)
		return;
	ev->devid = devid;
	ev->type = IEV_GYROSCOPE;
	ev->gyro.pitch = pitch;
	ev->gyro.yaw = yaw;
	ev->gyro.roll = roll;
	in_finishevent();
}


extern usercmd_t cl_pendingcmd[MAX_SPLITS];
qboolean IN_SetHandPosition(const char *devname, vec3_t org, vec3_t ang, vec3_t vel, vec3_t avel)
{
	int dtype;
	int seat;
	struct vrdevinfo_s *dev;
	if (!Q_strncasecmp(devname, "left", 4))
	{
		seat = atoi(devname+4);
		dtype = VRDEV_LEFT;
	}
	else if (!Q_strncasecmp(devname, "right", 5))
	{
		seat = atoi(devname+5);
		dtype = VRDEV_RIGHT;
	}
	else if (!Q_strncasecmp(devname, "head", 4))
	{
		seat = atoi(devname+4);
		dtype = VRDEV_HEAD;
	}
	else
		return false;	//no idea what you're talking about.
	if (seat < 0 || seat >= MAX_SPLITS)
		return false;	//duuuude!
	dev = &cl.playerview[seat].vrdev[dtype];

	if (org)
		VectorCopy(org, dev->origin);
	else
		VectorClear(dev->origin);
	if (ang)
	{
		dev->angles[0] = ANGLE2SHORT(ang[0]),
		dev->angles[1] = ANGLE2SHORT(ang[1]),
		dev->angles[2] = ANGLE2SHORT(ang[2]);
	}
	else
		VectorClear(dev->angles);
	if (vel)
		VectorCopy(vel, dev->velocity);
	else
		VectorClear(dev->velocity);
	if (avel)
		dev->avelocity[0] = ANGLE2SHORT(avel[0]),
		dev->avelocity[1] = ANGLE2SHORT(avel[1]),
		dev->avelocity[2] = ANGLE2SHORT(avel[2]);
	else
		VectorClear(dev->avelocity);

	dev->status =
			(org ?VRSTATUS_ORG:0)|
			(ang ?VRSTATUS_ANG:0)|
			(vel ?VRSTATUS_VEL:0)|
			(avel?VRSTATUS_AVEL:0);
	return true;
}
