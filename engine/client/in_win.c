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
// in_win.c -- windows 95 mouse and joystick code
// 02/21/97 JCB Added extended DirectInput code to support external controllers.

#include "quakedef.h"
#include "winquake.h"
//#include "dosisms.h"
#ifndef WINRT
#define USINGRAWINPUT

#ifdef USINGRAWINPUT
#include "in_raw.h"
#endif

void INS_Accumulate (void);

#define AVAIL_XINPUT
#ifdef AVAIL_XINPUT
typedef struct _XINPUT_GAMEPAD {
	WORD  wButtons;
	BYTE  bLeftTrigger;
	BYTE  bRightTrigger;
	SHORT sThumbLX;
	SHORT sThumbLY;
	SHORT sThumbRX;
	SHORT sThumbRY;
} XINPUT_GAMEPAD, *PXINPUT_GAMEPAD;
typedef struct _XINPUT_STATE {
	DWORD          dwPacketNumber;
	XINPUT_GAMEPAD Gamepad;
} XINPUT_STATE, *PXINPUT_STATE;
typedef struct _XINPUT_VIBRATION {
  WORD wLeftMotorSpeed;
  WORD wRightMotorSpeed;
} XINPUT_VIBRATION, *PXINPUT_VIBRATION;
DWORD (WINAPI *pXInputGetState)(DWORD dwUserIndex, XINPUT_STATE *pState);
DWORD (WINAPI *pXInputSetState)(DWORD dwUserIndex, XINPUT_VIBRATION *pState);
DWORD (WINAPI *pXInputGetDSoundAudioDeviceGuids)(DWORD dwUserIndex, GUID* pDSoundRenderGuid, GUID* pDSoundCaptureGuid);	//xi 1.3
DWORD (WINAPI *pXInputGetAudioDeviceIds)(DWORD dwUserIndex, LPWSTR pRenderDeviceId, UINT *pRenderCount, LPWSTR pCaptureDeviceId, UINT *pCaptureCount); //xi 1.4

enum
{
	XINPUT_GAMEPAD_DPAD_UP			= 0x0001,
	XINPUT_GAMEPAD_DPAD_DOWN		= 0x0002,
	XINPUT_GAMEPAD_DPAD_LEFT		= 0x0004,
	XINPUT_GAMEPAD_DPAD_RIGHT		= 0x0008,
	XINPUT_GAMEPAD_START			= 0x0010,
	XINPUT_GAMEPAD_BACK				= 0x0020,
	XINPUT_GAMEPAD_LEFT_THUMB		= 0x0040,
	XINPUT_GAMEPAD_RIGHT_THUMB		= 0x0080,
	XINPUT_GAMEPAD_LEFT_SHOULDER	= 0x0100,
	XINPUT_GAMEPAD_RIGHT_SHOULDER	= 0x0200,
	XINPUT_GAMEPAD_A				= 0x1000,
	XINPUT_GAMEPAD_B				= 0x2000,
	XINPUT_GAMEPAD_X				= 0x4000,
	XINPUT_GAMEPAD_Y				= 0x8000
};
static qboolean xinput_useaudio;
#endif


#ifdef AVAIL_DINPUT

#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0700
#endif

#include <dinput.h>

#define DINPUT_BUFFERSIZE           16
#define iDirectInputCreate(a,b,c,d)	pDirectInputCreate(a,b,c,d)

HRESULT (WINAPI *pDirectInputCreate)(HINSTANCE hinst, DWORD dwVersion,
	LPDIRECTINPUT * lplpDirectInput, LPUNKNOWN punkOuter);

#endif

#define DINPUT_VERSION_DX3 0x0300
#define DINPUT_VERSION_DX7 0x0700

// mouse variables
static cvar_t	in_dinput = CVARFD("in_dinput","0", CVAR_ARCHIVE|CVAR_NOTFROMSERVER, "Enables the use of dinput for mouse movements. Runs with it on do not rank (Patch 1576).");
static cvar_t	in_xinput = CVARFD("in_xinput","1", CVAR_ARCHIVE, "Enables the use of xinput for controllers.\nNote that if you have a headset plugged in, that headset will be used for audio playback if no specific audio device is configured.");
static cvar_t	in_builtinkeymap = CVARF("in_builtinkeymap", "0", CVAR_ARCHIVE);
static cvar_t in_simulatemultitouch = CVAR("in_simulatemultitouch", "0");
/*
FTESurf Patch 268: whether "is this window active" means FOREGROUND or merely
"owns its own thread's keyboard focus".

Not static -- gl_vidnt.c's per-frame reconciliation reads it too.
*/
cvar_t	in_focusgrab = CVARFD("in_focusgrab", "1", CVAR_ARCHIVE, "Whether the mouse grab follows the foreground window.\n0: Pre-268 behaviour - vid.activeapp is set from WM_SETFOCUS/WM_KILLFOCUS alone, so a window that owns its thread's focus without being foreground grabs the mouse anyway.\n1: The window must actually be the foreground window. A background instance leaves your mouse alone.");
static cvar_t	in_nonstandarddeadkeys = CVARD("in_nonstandarddeadkeys", "1", "Discard input events that result in multiple keys. Only the last key will be used. This results in behaviour that differs from eg notepad. To use a dead key, press it twice instead of the dead key followed by space.");

static cvar_t	xinput_leftvibrator = CVARF("xinput_leftvibrator","0", CVAR_ARCHIVE);
static cvar_t	xinput_rightvibrator = CVARF("xinput_rightvibrator","0", CVAR_ARCHIVE);

static cvar_t	m_accel_noforce = CVAR("m_accel_noforce", "0");
static cvar_t  m_threshold_noforce = CVAR("m_threshold_noforce", "0");

static cvar_t	cl_keypad = CVAR("cl_keypad", "1");

extern float multicursor_x[8], multicursor_y[8];
extern qboolean multicursor_active[8];

POINT		current_mouse_pos;


HWND mainwindow;
int window_center_x, window_center_y;
RECT		window_rect;


typedef struct {
	union {
		HANDLE rawinputhandle;
	} handles;
	char sysname[MAX_OSPATH];

	int qdeviceid;
} keyboard_t;

typedef struct {
	union {
		HANDLE rawinputhandle; // raw input
	} handles;
	char sysname[MAX_OSPATH];

	int numbuttons;
	int oldbuttons;
	int qdeviceid;	/*the device id controls which player slot it controls, if splitscreen splits it that way*/
} mouse_t;

static mouse_t sysmouse;

static qboolean	restore_spi;
static int		originalmouseparms[3], newmouseparms[3] = {0, 0, 0};
qboolean		mouseinitialized;
static qboolean	mouseparmsvalid, mouseactivatetoggle;
qboolean	mouseshowtoggle = 1;
unsigned int uiWheelMessage;

qboolean	mouseactive;

// joystick defines and variables
// where should defines be moved?
#define JOY_ABSOLUTE_AXIS	0x00000000		// control like a joystick
#define JOY_RELATIVE_AXIS	0x00000010		// control like a mouse, spinner, trackball
#define	JOY_MAX_AXES		6				// X, Y, Z, R, U, V


// none of these cvars are saved over a session
// this means that advanced controller configuration needs to be executed
// each time.  this avoids any problems with getting back to a default usage
// or when changing from one controller to another.  this way at least something
// works.
static cvar_t	in_joystick				= CVARAF("joystick","0", "in_joystick", CVAR_ARCHIVE);
static qboolean	joy_advancedinit;

static DWORD		joy_flags = JOY_RETURNALL|JOY_RETURNCENTERED;
#define MAX_JOYSTICKS 8
static struct wjoy_s {
	qboolean			isxinput;	//xinput device
	enum
	{
		DS_UNKNOWN,
		DS_PRESENT,
		DS_NOTPRESENT
	} devstate;
	unsigned int		id;		//windows id. device id is the index.
	unsigned int		devid;	//quake id (generally player index)
	DWORD	numbuttons;
	qboolean haspov;
	DWORD	povstate;
	DWORD	oldpovstate;
	DWORD	buttonstate;
	DWORD	oldbuttonstate;
	soundcardinfo_t *audiodev;
} wjoy[MAX_JOYSTICKS];
static int joy_count;

#ifdef AVAIL_DINPUT
static const GUID fGUID_XAxis	= {0xA36D02E0, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
static const GUID fGUID_YAxis	= {0xA36D02E1, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
static const GUID fGUID_ZAxis	= {0xA36D02E2, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
static const GUID fGUID_SysMouse	= {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

static const GUID fIID_IDirectInputDevice7A	= {0x57d7c6bc, 0x2356, 0x11d3, {0x8e, 0x9d, 0x00, 0xC0, 0x4f, 0x68, 0x44, 0xae}};
static const GUID fIID_IDirectInput7A		= {0x9a4cb684, 0x236d, 0x11d3, {0x8e, 0x9d, 0x00, 0xc0, 0x4f, 0x68, 0x44, 0xae}};

// devices
static LPDIRECTINPUT		g_pdi;
static LPDIRECTINPUTDEVICE	g_pMouse;
static qboolean	dinput_acquired;

static HINSTANCE hInstDI;

// current DirectInput version in use, 0 means using no DirectInput
static int dinput;

typedef struct MYDATA {
	LONG  lX;                   // X axis goes here
	LONG  lY;                   // Y axis goes here
	LONG  lZ;                   // Z axis goes here
	BYTE  bButtonA;             // One button goes here
	BYTE  bButtonB;             // Another button goes here
	BYTE  bButtonC;             // Another button goes here
	BYTE  bButtonD;             // Another button goes here
#if (DIRECTINPUT_VERSION >= DINPUT_VERSION_DX7)
	BYTE  bButtonE;             // DX7 buttons
	BYTE  bButtonF;
	BYTE  bButtonG;
	BYTE  bButtonH;
#endif
} MYDATA;

static DIOBJECTDATAFORMAT rgodf[] = {
  { &fGUID_XAxis,    FIELD_OFFSET(MYDATA, lX),       DIDFT_AXIS | DIDFT_ANYINSTANCE,   0,},
  { &fGUID_YAxis,    FIELD_OFFSET(MYDATA, lY),       DIDFT_AXIS | DIDFT_ANYINSTANCE,   0,},
  { &fGUID_ZAxis,    FIELD_OFFSET(MYDATA, lZ),       0x80000000 | DIDFT_AXIS | DIDFT_ANYINSTANCE,   0,},
  { 0,              FIELD_OFFSET(MYDATA, bButtonA), DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0,},
  { 0,              FIELD_OFFSET(MYDATA, bButtonB), DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0,},
  { 0,              FIELD_OFFSET(MYDATA, bButtonC), 0x80000000 | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0,},
  { 0,              FIELD_OFFSET(MYDATA, bButtonD), 0x80000000 | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0,},
#if (DIRECTINPUT_VERSION >= DINPUT_VERSION_DX7)
  { 0,              FIELD_OFFSET(MYDATA, bButtonE), 0x80000000 | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0,},
  { 0,              FIELD_OFFSET(MYDATA, bButtonF), 0x80000000 | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0,},
  { 0,              FIELD_OFFSET(MYDATA, bButtonG), 0x80000000 | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0,},
  { 0,              FIELD_OFFSET(MYDATA, bButtonH), 0x80000000 | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0,},
#endif
};

#define NUM_OBJECTS (sizeof(rgodf) / sizeof(rgodf[0]))

static DIDATAFORMAT	df = {
	sizeof(DIDATAFORMAT),       // this structure
	sizeof(DIOBJECTDATAFORMAT), // size of object data format
	DIDF_RELAXIS,               // absolute axis coordinates
	sizeof(MYDATA),             // device data size
	NUM_OBJECTS,                // number of objects
	rgodf,                      // and here they are
};

#if (DIRECTINPUT_VERSION >= DINPUT_VERSION_DX7)
// DX7 devices
static LPDIRECTINPUT7		g_pdi7;
static LPDIRECTINPUTDEVICE7	g_pMouse7;

// DX7 specific calls
#define iDirectInputCreateEx(a,b,c,d,e)	pDirectInputCreateEx(a,b,c,d,e)

static HRESULT (WINAPI *pDirectInputCreateEx)(HINSTANCE hinst,
		DWORD dwVersion, REFIID riidltf, LPVOID *ppvOut, LPUNKNOWN punkOuter);
#endif

#else
#define dinput 0
#endif

static JOYINFOEX	ji;

// raw input specific defines
#ifdef USINGRAWINPUT
// defines
#define MAX_RI_DEVICE_SIZE 128
#define INIT_RIBUFFER_SIZE (sizeof(RAWINPUTHEADER)+sizeof(RAWMOUSE))

#define RI_RAWBUTTON_MASK 0x000003E0
#define RI_INVALID_POS    0x80000000

// raw input dynamic functions
typedef INT (WINAPI *pGetRawInputDeviceList)(OUT PRAWINPUTDEVICELIST pRawInputDeviceList, IN OUT PINT puiNumDevices, IN UINT cbSize);
typedef INT(WINAPI *pGetRawInputData)(IN HRAWINPUT hRawInput, IN UINT uiCommand, OUT LPVOID pData, IN OUT PINT pcbSize, IN UINT cbSizeHeader);
typedef INT(WINAPI *pGetRawInputDeviceInfoA)(IN HANDLE hDevice, IN UINT uiCommand, OUT LPVOID pData, IN OUT PINT pcbSize);
typedef BOOL (WINAPI *pRegisterRawInputDevices)(IN PCRAWINPUTDEVICE pRawInputDevices, IN UINT uiNumDevices, IN UINT cbSize);

static pGetRawInputDeviceList _GRIDL;
static pGetRawInputData _GRID;
static pGetRawInputDeviceInfoA _GRIDIA;
static pRegisterRawInputDevices _RRID;

static keyboard_t *rawkbd;
static mouse_t *rawmice;
static int rawmicecount;
static int rawkbdcount;
static RAWINPUT *raw;
static int ribuffersize;
/*FTESurf Patch 468: a Windows precision touchpad.  Its RIM_TYPEMOUSE collection is
  enumerated and never reports; the OS synthesises the cursor motion from the
  digitizer collection (usage page 0x0D, usage 0x05) and delivers it as RIM_TYPEMOUSE
  with hDevice NULL -- 2137 of 2137 motion reports in a 3-minute probe on the owner's
  laptop (Pixart 093a:0255) -- which is exactly what Patch 306 counts as injected and
  drops.  So with raw input on the pad could not turn the view at all, and every
  honest swipe ticked the injected counter.  Fix: register for the digitizer
  collection too, stamp the time of its reports, and accept a handle-less mouse
  report only inside RAWPAD_WINDOW after one.  Measured gap from a digitizer report
  to the motion it produced: 0.2 ms median, 15 ms p90, 268 ms worst (a tap's
  synthesised button-up, delivered after lift-off).  Accepted motion lands on
  padmouse, a device of its own, so the journal attributes it to a `touchpad` entry
  and never to a mouse.  The gate is a presence test, not a motion test: every
  digitizer report re-arms it, a resting finger included, so a synthesised report timed
  inside it passes at whatever size and rate its sender chooses and becomes counts on
  the touchpad device, which the journal names.  Reviewed 2026-10-02 (three lenses,
  three rounds): the devid allocator must see padmouse; a release for a button the pad
  pressed is accepted outside the window so an accepted press cannot latch, and such a
  report is stripped to those release bits (round 3: whole-report acceptance let one
  touch seed a hands-off chain of UP|DOWN|motion reports); the wheel flag is dropped
  (the legacy WM_MOUSEWHEEL still carries the pad's scroll and gl_vidnt.c has no raw
  gate on it); the digitizer reports and the releases are counted so the journal shows
  when the window was armed and bounds the releases by the pad's presses.*/
static HANDLE rawpad[4];
static int rawpadcount;
static double rawpad_lasttime = -1;	/*-1 until a digitizer report: 0 is a real time on a process-relative clock*/
static qboolean rawpad_registered;
static mouse_t padmouse;
static qboolean padbuttondown[5];	/*buttons an ACCEPTED pad report pressed: only their releases pass outside the window*/
static const unsigned short padbtn_down[5] = {RI_MOUSE_BUTTON_1_DOWN, RI_MOUSE_BUTTON_2_DOWN, RI_MOUSE_BUTTON_3_DOWN, RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_5_DOWN};
static const unsigned short padbtn_up[5] = {RI_MOUSE_BUTTON_1_UP, RI_MOUSE_BUTTON_2_UP, RI_MOUSE_BUTTON_3_UP, RI_MOUSE_BUTTON_4_UP, RI_MOUSE_BUTTON_5_UP};
#define RAWPAD_WINDOW 0.5
/*FTESurf Patch 1424: observe-only raw keyboard (see in_raw_legacykey in in_generic.c).  A
  legacy press is corroborated by a real device's raw DOWN TRANSITION of the same scancode
  within RAWKEY_WINDOW, in either order, one for one.  rawkey_time is an unconsumed raw
  down, rawkey_pending a legacy press still waiting for one, counted once it is older than
  the window (INS_RawKeyExpire).  Repeats are judged by each path's own held state, never
  by a flag in the message: rawheld skips typematic makes, legheld skips legacy repeats;
  INS_ClearStates resets both on focus changes.  -1 = none (0 is a real time on this
  clock).  The key events, the raw reads and the expiry all run on the thread that owns the
  window (the window thread under WTHREAD), including registration transitions and
  INS_ClearStates via the bounded keyboard message. The extended bit is not compared:
  the L/R twins sharing a byte can vouch for each other, which costs less than a quirk
  counting an honest key.*/
#define RAWKEY_WINDOW 0.1
static qboolean rawrdp_bound;	//FTESurf Patch 1576: a Remote Desktop device was bound (sticks for the session)
static qboolean rawkbd_observe;
static qboolean rawkbd_nolegacy;
// Owner-only gate: the caller may mutate the entire cohort only between QUIESCE/RESUME.
static qboolean rawinput_quiesced;
static int rawinput_readdepth;
static double rawkey_time[256];
static double rawkey_pending[256];
static double rawkey_nohandletime[256];	//FTESurf Patch 1576: last handle-less raw report per scancode
static double rawkey_legup[256];	//FTESurf Patch 1576: last legacy release per scancode
static qboolean rawkey_ready;	//the arrays have been reset once (zero is a real time)
static void INS_RawKeyUncorroborated(int sc);
static int MapKey (int vkey);
int in_raw_injectedkey;	//FTESurf Patch 1576: uncorroborated presses SendInput's raw report sat beside (a lapse)
static qboolean rawkey_rawheld[256], rawkey_legheld[256];
static int rawkey_npending;

static void INS_RawKeyReset(void)
{
	int i;

	for (i = 0; i < (int)countof(rawkey_time); i++)
	{
		if (rawkey_ready && rawkey_pending[i] >= 0)
		{	//FTESurf Patch 1576: a re-registration decides a pending press, it does not swallow it
			INS_RawKeyUncorroborated(i);
			if (in_raw_legacykey >= 0)
				in_raw_legacykey++;
		}
		rawkey_time[i] = rawkey_pending[i] = rawkey_nohandletime[i] = rawkey_legup[i] = -1;
		rawkey_rawheld[i] = rawkey_legheld[i] = false;
	}
	rawkey_npending = 0;
	rawkey_ready = true;
}

/*FTESurf Patch 1576: an uncorroborated legacy press reached the game.  Windows makes some from real
  keyboards (AltGr's Ctrl, Shift around NumLock keypad keys) with no raw report at all; SendInput from a
  UIAccess program, which Windows reports as hardware, leaves a handle-less raw report for its key.
  Only the second is a guard lapse.*/
static void INS_RawKeyUncorroborated(int sc)
{
	if (rawkey_nohandletime[sc] >= 0 && fabs(rawkey_pending[sc] - rawkey_nohandletime[sc]) <= 2*RAWKEY_WINDOW)
		in_raw_injectedkey++;
}

static void INS_RawKeyExpire(double now)
{
	int i;

	if (rawinput_quiesced || !raw || !rawkbd_observe || !rawkey_npending)
		return;
	for (i = 0; i < (int)countof(rawkey_pending); i++)
	{
		if (rawkey_pending[i] >= 0 && now - rawkey_pending[i] > RAWKEY_WINDOW)
		{
			INS_RawKeyUncorroborated(i);
			rawkey_pending[i] = -1;
			rawkey_npending--;
			in_raw_legacykey++;
		}
	}
}
//nettest: what raw input has accounted for, per button, for the five buttons it has
//usButtonFlags for.  Read only by INS_MouseEvent, to tell a genuine duplicate legacy
//message from a device raw input never enumerated -- see the comment there.
//  rawbuttontime -- Sys_DoubleTime() of the last raw transition, catching the DOWN and UP
//                   edges, where the synthesised legacy message follows within microseconds.
//  rawbuttondown -- whether raw currently holds it down.  REQUIRED as well as the
//                   timestamp: raw reports transitions, not held state, so during a long
//                   press there is no new stamp, and WM_MOUSEMOVE carries live button
//                   flags into INS_MouseEvent.  On a timestamp alone a held button would
//                   look un-accounted-for 100ms in and fire a second press on the first
//                   mouse movement.
static double rawbuttontime[5];
static qboolean rawbuttondown[5];
//FTESurf Patch 307: whether the CURRENT mouse registration carries RIDEV_NOLEGACY.
//Declared here rather than beside its setter because INS_RawInput_DeInit and
//INS_RawInput_MouseRegister both clear it and both come earlier in the file.
static qboolean rawmouse_nolegacy;

/*FTESurf Patch 204: default 1, and answers to Source's name for it.

  The default was 0, which meant the live path was INS_Accumulate's
  GetCursorPos/SetCursorPos recentre -- ONE already-OS-summed delta per call,
  roughly twice a frame, with the OS's own pointer acceleration and its
  screen-pixel quantisation baked in before the engine ever sees it.  That is
  the wrong input for a movement game and it is strictly the wrong input for
  Patch 202's journal, whose entire premise is one record per report.  Both
  games on this tree were already setting it to 1 from their configs, so this
  moves the good case out of a config line and into the engine, where a fresh
  install and a wiped config also get it.

  "m_rawinput" is the alias because that is what this cvar is called in the
  Source engine, and this game's players arrive from there with the name in
  their fingers.  name2 hashes to the same cvar, so the two are one setting
  with two spellings rather than two settings that can disagree -- which is the
  only reason an alias is safe to add at all.

  CVAR_ARCHIVE, so turning it off sticks.  cfg/default.cfg's `set in_rawinput 1`
  is removed in the same patch: an archived cvar whose default.cfg also asserts
  a value can never be turned off by the player, because default.cfg execs
  before the archived config and the player's own choice would be the one
  getting overwritten -- backwards.*/
static cvar_t in_rawinput_mice = CVARAFD("in_rawinput", "1", "m_rawinput", CVAR_ARCHIVE, "Enables rawinput support for mice in XP onwards. Rawinput permits independant device identification (ie: splitscreen clients can each have their own mouse), and reports mouse motion as the device sends it rather than as an OS-summed and OS-accelerated cursor delta.");
static cvar_t in_rawinput_keyboard = CVARD("in_rawinput_keyboard", "0", "Enables rawinput support for keyboards in XP onwards as well as just mice.");
static cvar_t in_rawinput_rdp = CVARFD("in_rawinput_rdp", "0", CVAR_NOTFROMSERVER, "Activate Remote Desktop Protocol devices too. Runs with it on do not rank (Patch 1576).");
/*FTESurf Patch 307, defaults since Patch 1576: on, and touchpads off (the owner's 16 Nov
  ranked rule; Patch 468 gave touchpad clicks a raw path).  NOTFROMSERVER: a map's
  worldspawn `cvar_` key or a server stuffcmd must not hold these apart from what the
  player set (Patch 1573's open case).  Applied only while the mouse is grabbed -- see
  INS_RawInput_MouseSetLegacy.*/
static cvar_t in_rawinput_nolegacy = CVARFD("in_rawinput_nolegacy", "1", CVAR_ARCHIVE|CVAR_NOTFROMSERVER, "Suppresses legacy mouse messages while the mouse is grabbed, so a synthesized (injected) click cannot reach the game through the legacy path. A precision touchpad's clicks arrive through raw input while a finger is on the pad (Patch 468). On by default; 0 lets injected clicks through and runs do not rank.");
static cvar_t in_rawinput_touchpad = CVARFD("in_rawinput_touchpad", "0", CVAR_ARCHIVE|CVAR_NOTFROMSERVER, "Accept raw mouse reports that carry no device handle while a Windows precision touchpad has reported contact within the last half second. The OS synthesises a touchpad's cursor motion with no handle, which is otherwise rejected as injected input (Patch 306) and never turns the view. Off by default (Patch 1576); runs with it on do not rank.");	//FTESurf Patch 468

void INS_RawInput_MouseDeRegister(void);
int INS_RawInput_MouseRegister(void);
void INS_RawInput_KeyboardDeRegister(void);
int INS_RawInput_KeyboardRegister(void);
static void INS_RawInput_KeyObserveRegister(void);
static int INS_RawInput_KeyboardRequest(rawkbdop_t op);
void INS_RawInput_DeInit(void);

#endif

#ifdef AVAIL_XINPUT
static const int xinputjbuttons[] =
{	//xinput->fte button mapping table
	K_GP_DPAD_UP,
	K_GP_DPAD_DOWN,
	K_GP_DPAD_LEFT,
	K_GP_DPAD_RIGHT,
	K_GP_START,
	K_GP_BACK,
	K_GP_LEFT_STICK,
	K_GP_RIGHT_STICK,

	K_GP_LEFT_SHOULDER,
	K_GP_RIGHT_SHOULDER,
	K_GP_GUIDE,		//officially, this is 'reserved'
	K_GP_UNKNOWN,	//reserved
	K_GP_A,
	K_GP_B,
	K_GP_X,
	K_GP_Y,

	//not part of xinput specs, but used to treat the various axis as buttons.
	K_GP_LEFT_THUMB_UP,
	K_GP_LEFT_THUMB_DOWN,
	K_GP_LEFT_THUMB_LEFT,
	K_GP_LEFT_THUMB_RIGHT,
	K_GP_RIGHT_THUMB_UP,
	K_GP_RIGHT_THUMB_DOWN,
	K_GP_RIGHT_THUMB_LEFT,
	K_GP_RIGHT_THUMB_RIGHT,

	K_GP_LEFT_TRIGGER,
	K_GP_RIGHT_TRIGGER,
};
#endif
static const int mmjbuttons[32] =
{	//winmm->fte joystick button mapping table
	K_JOY1,
	K_JOY2,
	K_JOY3,
	K_JOY4,
	//yes, aux1-4 skipped for compat with other quake engines.
	K_JOY9,
	K_JOY10,
	K_JOY11,
	K_JOY12,
	K_JOY13,
	K_JOY14,
	K_JOY15,
	K_JOY16,
	K_JOY17,
	K_JOY18,
	K_JOY19,
	K_JOY20,
	K_JOY21,
	K_JOY22,
	K_JOY23,
	K_JOY24,
	K_JOY25,
	K_JOY26,
	K_JOY27,
	K_JOY28,
	K_JOY29,
	K_JOY30,
	K_JOY31,
	K_JOY32,
	//29-32 used for the pov stuff, so lets switch back to aux1-4 to avoid wastage
	K_JOY5,
	K_JOY6,
	K_JOY7,
	K_JOY8
};


static HANDLE	powercontext = INVALID_HANDLE_VALUE;
static qboolean	powersaveblocked = false;
static HANDLE	(WINAPI *pPowerCreateRequest)	(REASON_CONTEXT *Context);
static BOOL		(WINAPI *pPowerSetRequest)		(HANDLE Context,	POWER_REQUEST_TYPE RequestType);
static BOOL		(WINAPI *pPowerClearRequest)	(HANDLE Context,	POWER_REQUEST_TYPE RequestType);
static void INS_ScreenSaver_Init(void)
{
	dllfunction_t functable[] =
	{
		{(void*)&pPowerCreateRequest,	"PowerCreateRequest"},
		{(void*)&pPowerSetRequest,		"PowerSetRequest"},
		{(void*)&pPowerClearRequest,	"PowerClearRequest"},
		{NULL}
	};

	if (Sys_LoadLibrary("kernel32.dll", functable))
	{	//we don't do microsoft's interpretation of localisation, so this is lame.
		REASON_CONTEXT reason = {POWER_REQUEST_CONTEXT_VERSION, POWER_REQUEST_CONTEXT_SIMPLE_STRING};
		reason.Reason.SimpleReasonString = L"Demo Playback";
		powercontext = pPowerCreateRequest(&reason);

		//FIXME: be prepared to regenerate the reason when our lang changes...
	}
}
static void INS_ScreenSaver_UpdateBlock(qboolean block)
{
	if (powercontext != INVALID_HANDLE_VALUE && block != powersaveblocked)
	{
		powersaveblocked = block;
		if (block)
		{
			pPowerSetRequest(powercontext, PowerRequestDisplayRequired);	//keep the screen on...
			pPowerSetRequest(powercontext, PowerRequestSystemRequired);		//and don't go to sleep mid-video, too...
		}
		else
		{
			pPowerClearRequest(powercontext, PowerRequestSystemRequired);
			pPowerClearRequest(powercontext, PowerRequestDisplayRequired);
		}
	}
}



// forward-referenced functions
void INS_StartupJoystick (void);
void INS_JoyMove (void);

/*
===========
Force_CenterView_f
===========
*/
void Force_CenterView_f (void)
{
	cl.playerview[0].viewangles[PITCH] = 0;
}

/*
===========
INS_UpdateClipCursor
===========
*/
void INS_UpdateClipCursor (void)
{
	if (mouseinitialized && mouseactive && !dinput)
	{
		ClipCursor (&window_rect);
	}
}


/*
===========
INS_ShowMouse
===========
*/
static void INS_ShowMouse (void)
{
	if (!mouseshowtoggle)
	{	//FIXME: we should be sending this via the window thread.
		ShowCursor (TRUE);
		mouseshowtoggle = 1;
	}
}


/*
===========
INS_HideMouse
===========
*/
static void INS_HideMouse (void)
{
	if (mouseshowtoggle)
	{	//I'm told that nvidia's null-named 'Geforce Experience' (which is malware in my book) fucks with cursor visibility.
		//So lets try to ensure that it gets hidden properly in case we've got race conditions and code getting injected into our process.
		//in modern windows, this is per-thread, so blame code injection if this ever prints.
		//FIXME: we should be sending this via the window thread.
		while (ShowCursor (FALSE) >= 0)
			Con_Printf(CON_WARNING "Force-hiding mouse cursor...\n");
		mouseshowtoggle = 0;
	}
}

//scan for an unused device id for joysticks (now that something was pressed).
static int Joy_AllocateDevID(void)
{
	extern cvar_t cl_splitscreen;
	extern cvar_t in_skipplayerone;
	int id = (in_skipplayerone.ival?1:0), j;
	for ( ; ; id++)
	{
		for (j = 0; j < joy_count; j++)
		{
			if (wjoy[j].devid == id)
				break;
		}
		if (j == joy_count)
		{
			if (id > cl_splitscreen.ival && !*cl_splitscreen.string)
				cl_splitscreen.ival = id;
			return id;
		}
	}
}

enum controllertype_e INS_GetControllerType(int id)
{
	int j;
	for (j = 0; j < joy_count; j++)
	{
		if (wjoy[j].devid == id)
		{
			if (wjoy[j].devstate == DS_NOTPRESENT)
				return CONTROLLER_NONE;
			if (wjoy[j].isxinput)
				return CONTROLLER_XBOX;	//we don't really know, but we're using it through an xbox-specific api, so...
			return CONTROLLER_UNKNOWN;	//the legacy joy API.
		}
	}
	return CONTROLLER_NONE;	//no idea what you're talking about.
}

void INS_Rumble(int id, quint16_t amp_low, quint16_t amp_high, quint32_t duration)
{
	//Con_DPrintf(CON_WARNING "Rumble is unavailable on this platform\n");
}

void INS_RumbleTriggers(int id, quint16_t left, quint16_t right, quint32_t duration)
{
	//Con_DPrintf(CON_WARNING "Trigger rumble is unavailable on this platform\n");
}

void INS_SetLEDColor(int id, vec3_t color)
{
	//Con_DPrintf(CON_WARNING "Game-Pad LED colors are unavailable on this platform\n");
}

void INS_SetTriggerFX(int id, const void *data, size_t size)
{
	//Con_DPrintf(CON_WARNING "Trigger FX are unavailable on this platform\n");
}

#ifdef USINGRAWINPUT
static int Mouse_AllocateDevID(void)
{
	extern cvar_t cl_splitscreen;
	int id = 0, j;
	for (id = 0; ; id++)
	{
		for (j = 0; j < rawmicecount; j++)
		{
			if (rawmice[j].qdeviceid == id)
				break;
		}
		if (j == rawmicecount && !(rawpadcount > 0 && padmouse.qdeviceid == id))	//FTESurf Patch 468: padmouse is not in the table
		{
			if (id > cl_splitscreen.ival && !*cl_splitscreen.string)
				cl_splitscreen.ival = id;
			return id;
		}
	}
}
static int Keyboard_AllocateDevID(void)
{
	extern cvar_t cl_splitscreen;
	int id = 0, j;
	for (id = 0; ; id++)
	{
		for (j = 0; j < rawkbdcount; j++)
		{
			if (rawkbd[j].qdeviceid == id)
				break;
		}
		if (j == rawkbdcount)
		{
			if (id > cl_splitscreen.ival && !*cl_splitscreen.string)
				cl_splitscreen.ival = id;
			return id;
		}
	}
}
#endif

/*
===========
INS_ActivateMouse
===========
*/
static void INS_ActivateMouse (void)
{
	mouseactivatetoggle = true;

	if (mouseinitialized && !mouseactive)
	{
#ifdef AVAIL_DINPUT
#if (DIRECTINPUT_VERSION >= DINPUT_VERSION_DX7)
		if (dinput >= DINPUT_VERSION_DX7)
		{
			if (g_pMouse7)
			{
				if (!dinput_acquired)
				{
					IDirectInputDevice7_Acquire(g_pMouse7);
					dinput_acquired = true;
				}
			}
			else
			{
				return;
			}
		}
		else
#endif
		if (dinput)
		{
			if (g_pMouse)
			{
				if (!dinput_acquired)
				{
					IDirectInputDevice_Acquire(g_pMouse);
					dinput_acquired = true;
				}
			}
			else
			{
				return;
			}
		}
		else
#endif
		{
#ifdef USINGRAWINPUT
			if (rawkbdcount > 0 && in_rawinput_keyboard.ival)
			{
				if (INS_RawInput_KeyboardRegister())
				{
					Con_SafePrintf("Raw input: unable to register raw input for keyboard\n");
					INS_RawInput_KeyObserveRegister();
				}
			}
#endif

			if (mouseparmsvalid)
			{
				SystemParametersInfo (SPI_SETMOUSE, 0, newmouseparms, 0);
				restore_spi = true;
			}

			SetCursorPos (window_center_x, window_center_y);
			SetCapture (mainwindow);
			ClipCursor (&window_rect);
		}

		mouseactive = true;
	}
}


/*
===========
INS_DeactivateMouse
===========
*/
static void INS_DeactivateMouse (void)
{
	mouseactivatetoggle = false;

	if (mouseinitialized && mouseactive)
	{
#ifdef AVAIL_DINPUT
#if (DIRECTINPUT_VERSION >= DINPUT_VERSION_DX7)
		if (dinput >= DINPUT_VERSION_DX7)
		{
			if (g_pMouse7)
			{
				if (dinput_acquired)
				{
					IDirectInputDevice_Unacquire(g_pMouse7);
					dinput_acquired = false;
				}
			}
			else
			{
				return;
			}
		}
		else
#endif
		if (dinput)
		{
			if (g_pMouse)
			{
				if (dinput_acquired)
				{
					IDirectInputDevice_Unacquire(g_pMouse);
					dinput_acquired = false;
				}
			}
		}
		else
#endif
		{
#ifdef USINGRAWINPUT
//			if (rawmicecount > 0)
//				INS_RawInput_MouseDeRegister();
#endif

			if (restore_spi)
				SystemParametersInfo (SPI_SETMOUSE, 0, originalmouseparms, 0);

			ClipCursor (NULL);
			ReleaseCapture ();
			if (!vid.forcecursor)
			{
				vid.forcecursor = true;
				vid.forcecursorpos[0] = mousecursor_x;
				vid.forcecursorpos[1] = mousecursor_y;
			}
		}

		mouseactive = false;
	}
}

/*
===========
INS_SetQuakeMouseState
===========
*/
void INS_SetQuakeMouseState (void)
{
	if (mouseactivatetoggle)
		INS_ActivateMouse ();
	else
		INS_DeactivateMouse();
}

/*
===========
INS_RestoreOriginalMouseState
===========
*/
void INS_RestoreOriginalMouseState (void)
{
	if (mouseactivatetoggle)
	{
		INS_DeactivateMouse ();
		mouseactivatetoggle = true;
	}

// try to redraw the cursor so it gets reinitialized, because sometimes it
// has garbage after the mode switch
	ShowCursor (TRUE);
	ShowCursor (FALSE);
}




void INS_UpdateGrabs(int fullscreen, int activeapp)
{
	int grabmouse;

	INS_ScreenSaver_UpdateBlock(cls.demoplayback && activeapp);

	if (!activeapp)
		grabmouse = false;
	else if (fullscreen || in_simulatemultitouch.ival || in_windowed_mouse.value)
	{
		if (vrui.enabled || !Key_MouseShouldBeFree())
			grabmouse = true;
		else
			grabmouse = false;
	}
	else
		grabmouse = false;

	if (activeapp)
	{
		if (
#ifdef TEXTEDITOR
			!editormodal &&
#endif
			!SCR_HardwareCursorIsActive() && (fullscreen || in_simulatemultitouch.ival || in_windowed_mouse.value) && (fullscreen || (current_mouse_pos.x >= window_rect.left && current_mouse_pos.y >= window_rect.top && current_mouse_pos.x <= window_rect.right && current_mouse_pos.y <= window_rect.bottom)))
		{
			INS_HideMouse();
		}
		else 
		{
			INS_ShowMouse();
			grabmouse = false;
		}
	}
	else
	{
		INS_ShowMouse();
		grabmouse = false;
	}

#ifdef HLCLIENT
	//halflife gamecode does its own mouse control... yes this is vile.
	if (grabmouse)
	{
		if (CLHL_GamecodeDoesMouse())
			grabmouse = 2;
	}

	if (grabmouse == 2)
	{
		INS_DeactivateMouse();
		CLHL_SetMouseActive(true);
		return;
	}

	CLHL_SetMouseActive(false);
#endif

	if (grabmouse)
		INS_ActivateMouse();
	else
		INS_DeactivateMouse();


	if (vid.forcecursor && !mouseactive)
	{
		vid.forcecursor = false;
		if (activeapp)
			SetCursorPos(window_rect.left+vid.forcecursorpos[0], window_rect.top+vid.forcecursorpos[1]);
	}
}






#ifdef AVAIL_DINPUT
BOOL CALLBACK INS_EnumerateDI7Devices(LPCDIDEVICEINSTANCE inst, LPVOID parm)
{
	Con_DPrintf("EnumerateDevices found: %s\n", inst->tszProductName);

	return DIENUM_CONTINUE;
}
/*
===========
INS_InitDInput
===========
*/
int INS_InitDInput (void)
{
	HRESULT		hr;
	DIPROPDWORD	dipdw = {
		{
			sizeof(DIPROPDWORD),        // diph.dwSize
			sizeof(DIPROPHEADER),       // diph.dwHeaderSize
			0,                          // diph.dwObj
			DIPH_DEVICE,                // diph.dwHow
		},
		DINPUT_BUFFERSIZE,              // dwData
	};

	if (!hInstDI)
	{
		hInstDI = Sys_LoadLibrary("dinput.dll", NULL);

		if (hInstDI == NULL)
		{
			Con_SafePrintf ("Couldn't load dinput.dll\n");
			return 0;
		}
	}

#if (DIRECTINPUT_VERSION >= DINPUT_VERSION_DX7)
	if (!pDirectInputCreateEx)
		pDirectInputCreateEx = (void *)GetProcAddress(hInstDI,"DirectInputCreateEx");

	if (pDirectInputCreateEx) // use DirectInput 7
	{
		// register with DirectInput and get an IDirectInput to play with.
		hr = iDirectInputCreateEx(global_hInstance, DINPUT_VERSION_DX7, &fIID_IDirectInput7A, (void**)&g_pdi7, NULL);

		if (FAILED(hr))
			return 0;

		IDirectInput7_EnumDevices(g_pdi7, 0, &INS_EnumerateDI7Devices, NULL, DIEDFL_ATTACHEDONLY);

		// obtain an interface to the system mouse device.
		hr = IDirectInput7_CreateDeviceEx(g_pdi7, &fGUID_SysMouse, &fIID_IDirectInputDevice7A, (void**)&g_pMouse7, NULL);

		if (FAILED(hr)) {
			Con_SafePrintf ("Couldn't open DI7 mouse device\n");
			return 0;
		}

		// set the data format to "mouse format".
		hr = IDirectInputDevice7_SetDataFormat(g_pMouse7, &df);

		if (FAILED(hr)) {
			Con_SafePrintf ("Couldn't set DI7 mouse format\n");
			return 0;
		}

		// set the cooperativity level.
		hr = IDirectInputDevice7_SetCooperativeLevel(g_pMouse7, mainwindow,
			DISCL_EXCLUSIVE | DISCL_FOREGROUND);

		if (FAILED(hr)) {
			Con_SafePrintf ("Couldn't set DI7 coop level\n");
			return 0;
		}

		// set the buffer size to DINPUT_BUFFERSIZE elements.
		// the buffer size is a DWORD property associated with the device
		hr = IDirectInputDevice7_SetProperty(g_pMouse7, DIPROP_BUFFERSIZE, &dipdw.diph);

		if (FAILED(hr)) {
			Con_SafePrintf ("Couldn't set DI7 buffersize\n");
			return 0;
		}

		return DINPUT_VERSION_DX7;
	}
#endif

	if (!pDirectInputCreate)
	{
		pDirectInputCreate = (void *)GetProcAddress(hInstDI,"DirectInputCreateA");

		if (!pDirectInputCreate)
		{
			Con_SafePrintf ("Couldn't get DI3 proc addr\n");
			return 0;
		}
	}

// register with DirectInput and get an IDirectInput to play with.
	hr = iDirectInputCreate(global_hInstance, DINPUT_VERSION_DX3, &g_pdi, NULL);

	if (FAILED(hr))
	{
		return 0;
	}
	IDirectInput_EnumDevices(g_pdi, 0, &INS_EnumerateDI7Devices, NULL, DIEDFL_ATTACHEDONLY);

// obtain an interface to the system mouse device.
	hr = IDirectInput_CreateDevice(g_pdi, &fGUID_SysMouse, &g_pMouse, NULL);

	if (FAILED(hr))
	{
		Con_SafePrintf ("Couldn't open DI3 mouse device\n");
		return 0;
	}

// set the data format to "mouse format".
	hr = IDirectInputDevice_SetDataFormat(g_pMouse, &df);

	if (FAILED(hr))
	{
		Con_SafePrintf ("Couldn't set DI3 mouse format\n");
		return 0;
	}

// set the cooperativity level.
	hr = IDirectInputDevice_SetCooperativeLevel(g_pMouse, mainwindow,
			DISCL_EXCLUSIVE | DISCL_FOREGROUND);

	if (FAILED(hr))
	{
		Con_SafePrintf ("Couldn't set DI3 coop level\n");
		return 0;
	}


// set the buffer size to DINPUT_BUFFERSIZE elements.
// the buffer size is a DWORD property associated with the device
	hr = IDirectInputDevice_SetProperty(g_pMouse, DIPROP_BUFFERSIZE, &dipdw.diph);

	if (FAILED(hr))
	{
		Con_SafePrintf ("Couldn't set DI3 buffersize\n");
		return 0;
	}

	return DINPUT_VERSION_DX3;
}

void INS_CloseDInput (void)
{
#if (DIRECTINPUT_VERSION >= DINPUT_VERSION_DX7)
	if (g_pMouse7)
	{
		IDirectInputDevice7_Release(g_pMouse7);
		g_pMouse7 = NULL;
	}
	if (g_pdi7)
	{
		IDirectInput7_Release(g_pdi7);
		g_pdi7 = NULL;
	}
	pDirectInputCreateEx = NULL;
#endif
	if (g_pMouse)
	{
		IDirectInputDevice_Release(g_pMouse);
		g_pMouse = NULL;
	}
	if (g_pdi)
	{
		IDirectInput_Release(g_pdi);
		g_pdi = NULL;
	}
	if (hInstDI)
	{
		FreeLibrary(hInstDI);
		hInstDI = NULL;
		pDirectInputCreate = NULL;
	}

}
#endif

#ifdef USINGRAWINPUT
void INS_RawInput_MouseDeRegister(void)
{
	RAWINPUTDEVICE Rid;

	// deregister raw input
	Rid.usUsagePage = 0x01;
	Rid.usUsage = 0x02;
	Rid.dwFlags = RIDEV_REMOVE;
	Rid.hwndTarget = NULL;

	(*_RRID)(&Rid, 1, sizeof(Rid));
}

void INS_RawInput_KeyboardDeRegister(void)
{
	INS_RawInput_KeyboardRequest(RAWKBD_REMOVE);
}

/*FTESurf Patch 468: the precision touchpad's digitizer collection.  Registered beside
  the mouse (same focus-following NULL target), never with NOLEGACY: the OS must keep
  synthesising the motion, since that synthesised report is what gets accepted.*/
static void INS_RawInput_PadRegister(void)
{
	RAWINPUTDEVICE Rid;

	Rid.usUsagePage = 0x0D;
	Rid.usUsage = 0x05;
	Rid.dwFlags = 0;
	Rid.hwndTarget = NULL;
	rawpad_registered = (*_RRID)(&Rid, 1, sizeof(Rid)) != 0;
	rawpad_lasttime = -1;
	in_rawpads_live = rawpad_registered ? 1 : 0;	/*the header says bound, not enumerated; one padmouse, one table line, however many collections*/
	memset(padbuttondown, 0, sizeof(padbuttondown));
	Con_DPrintf("Raw input: precision touchpad %s -- handle-less motion accepted within %g s of its reports\n",
		rawpad_registered ? "registered" : "registration FAILED", RAWPAD_WINDOW);
}

static void INS_RawInput_PadDeRegister(void)
{
	RAWINPUTDEVICE Rid;

	if (!rawpad_registered)
		return;
	Rid.usUsagePage = 0x0D;
	Rid.usUsage = 0x05;
	Rid.dwFlags = RIDEV_REMOVE;
	Rid.hwndTarget = NULL;
	(*_RRID)(&Rid, 1, sizeof(Rid));
	rawpad_registered = false;
}

static void INS_RawInput_DeInitQuiesced(void)
{
	/*Patch 1452: Windows retains a refused binding. Abort the destructive
	  transaction before dropping its dispatch table/buffer (or mouse state).
	  ReInit retries removal before allocating any replacement resources.*/
	if (INS_RawInput_KeyboardRequest(RAWKBD_REMOVE) && (rawkbd_nolegacy || rawkbd_observe))
		return;
	if (rawmicecount > 0)
		INS_RawInput_MouseDeRegister();
	INS_RawInput_PadDeRegister();	//FTESurf Patch 468
	rawpadcount = 0;
	in_rawpads_live = 0;
	padmouse.qdeviceid = DEVID_UNSET;
	memset(padbuttondown, 0, sizeof(padbuttondown));
	rawmicecount = 0;
	rawkbdcount = 0;
	in_rawmice_live = 0;	//FTESurf Patch 301: kept in step with the two above
	in_rawkbd_live = 0;
	//nettest: forget what raw was accounting for. A device unplugged mid-press never sends
	//its UP, and a latched rawbuttondown would suppress that button's legacy messages for
	//good -- reinstating the very bug this tracking exists to fix.
	memset(rawbuttondown, 0, sizeof(rawbuttondown));
	memset(rawbuttontime, 0, sizeof(rawbuttontime));
	rawmouse_nolegacy = false;	//FTESurf Patch 307: the registration is gone with it
	in_raw_nolegacy_live = -1;	//...and with no registration there is no state to report
	Z_Free(rawmice);
	rawmice = NULL;
	Z_Free(rawkbd);
	rawkbd = NULL;
	Z_Free(raw);
	raw = NULL;
	ribuffersize = 0;
	memset(multicursor_active, 0, sizeof(multicursor_active));
}

void INS_RawInput_DeInit(void)
{
	if (INS_RawInput_KeyboardRequest(RAWKBD_QUIESCE))
		return;
	INS_RawInput_DeInitQuiesced();
	INS_RawInput_KeyboardRequest(RAWKBD_RESUME);
}
#endif

#ifdef USINGRAWINPUT
// raw input registration functions
/*FTESurf Patch 307: registration flags are now a parameter, because the legacy
  path has to be closable WITHOUT being closed by default.  Everything below is
  unchanged for flags == 0, which is what every existing caller passes.*/
static int INS_RawInput_MouseRegisterFlags(DWORD flags)
{
	// This function registers to receive the WM_INPUT messages
	RAWINPUTDEVICE Rid; // Register only for mouse messages from wm_input.

	//register to get wm_input messages
	Rid.usUsagePage = 0x01;
	Rid.usUsage = 0x02;
	//note: we don't exclude legacy events any more. while we don't really want them, we also don't want to get confused about click states. this way we can track the states properly without breaking.
	Rid.dwFlags = flags;//RIDEV_NOLEGACY adds HID mouse and also ignores legacy mouse messages
	Rid.hwndTarget = NULL;

	// Register to receive the WM_INPUT message for any change in mouse (buttons, wheel, and movement will all generate the same message)
	if (!(*_RRID)(&Rid, 1, sizeof(Rid)))
		return 1;

	return 0;
}

int INS_RawInput_MouseRegister(void)
{
	/*Patch 307: this is the flags-0 registration, so the latch must come back with
	  it.  Every caller of this is a fresh start (init, or a re-register after a
	  failure); leaving the latch set would make INS_RawInput_MouseSetLegacy believe
	  NOLEGACY was still in force and skip re-applying it.*/
	rawmouse_nolegacy = false;
	in_raw_nolegacy_live = 0;
	if (INS_RawInput_MouseRegisterFlags(0))
		return 1;
	if (rawpadcount > 0)
		INS_RawInput_PadRegister();	//FTESurf Patch 468: with the mouse, never without it
	return 0;
}

/*FTESurf Patch 307: THE OPTIONAL BLOCK, and it is GRAB-SCOPED for two separate
  reasons, only one of which is about cheating.

  RIDEV_NOLEGACY stops the synthesized WM_*BUTTONDOWN that an injected click
  relies on -- INS_MouseEvent's dedupe cannot be fooled by a message that is
  never delivered.  That closes the hole outright rather than merely recording
  it.

  IT IS NOT FREE, AND THE COST IS ALREADY MEASURED IN THIS FILE.  A Windows
  precision touchpad is a HID digitizer rather than a RIM_TYPEMOUSE: it emits no
  RI_MOUSE_BUTTON_* flags whatever, so the legacy message is the ONLY press it
  has and this setting takes every mouse button away from those users -- taps and
  press-and-hold alike.  See the essay in INS_MouseEvent, which was written when
  that bug was found the hard way.  So the default is 0 and this is a profile
  decision, not a fix to apply blindly.

  SCOPED TO THE GRAB because NOLEGACY also suppresses NON-CLIENT legacy messages,
  which is how a windowed player drags, resizes and closes the window.  Applying
  it while the mouse is free would make the title bar dead -- a far worse bug
  than the one being fixed, and one the player could not escape without editing a
  config.  While the mouse is grabbed there is no title bar to click, so the
  window cost is zero and only the touchpad cost remains.

  The state is latched so a re-register only happens on an actual transition;
  RegisterRawInputDevices on every activate would be a syscall per alt-tab for
  no reason, and on every frame would be worse.*/
static void INS_RawInput_MouseSetLegacy(qboolean wantnolegacy)
{
	if (rawmicecount <= 0)
		return;			//nothing registered to change
	if (!!wantnolegacy == !!rawmouse_nolegacy)
		return;			//already in that state

	if (INS_RawInput_MouseRegisterFlags(wantnolegacy ? RIDEV_NOLEGACY : 0))
	{	/*a refused re-register leaves the PREVIOUS registration standing, so the
		  latch must not move -- claiming the new state while the old one is live
		  would make the journal's account of this session wrong.*/
		Con_DPrintf("Raw input: could not %s legacy mouse messages\n",
			wantnolegacy ? "suppress" : "restore");
		return;
	}
	rawmouse_nolegacy = !!wantnolegacy;
	in_raw_nolegacy_live = rawmouse_nolegacy;
	/*Printed rather than silent.  This setting's failure mode is that it quietly
	  does NOT apply -- it is grab-scoped, so anything leaving the mouse ungrabbed
	  leaves the hole open -- and a test with no way to see whether it took would
	  read that as the block itself not working.*/
	Con_DPrintf("Raw input: legacy mouse messages %s\n",
		rawmouse_nolegacy ? "SUPPRESSED (injected clicks blocked)" : "allowed");
}

// Binding mode survives refused removal; availability excludes lifecycle blackouts.
static void INS_RawInput_KeyObserveAvailability(void)
{
	qboolean available = !rawinput_quiesced && raw && _GRID && rawkbd_observe;

	if (in_rawkbd_observe != available)
		in_rawkbd_observechanges++;
	in_rawkbd_observe = available;
}

/*Patch 1452: runs on the window/input thread. Refusal retains the Windows binding.
  No callbacks or pointers cross the registration handoff.*/
int INS_RawInput_KeyboardControl(rawkbdop_t op)
{
	RAWINPUTDEVICE Rid;
	qboolean observe;

	if (op == RAWKBD_QUIESCE)
	{
		// A reentrant SendMessage must not acknowledge a read still on this stack.
		if (rawinput_readdepth || rawinput_quiesced)
			return 1;
		rawinput_quiesced = true;
		INS_RawKeyReset();	// Unmatched work cannot survive discarded corroboration.
		INS_RawInput_KeyObserveAvailability();
		return 0;
	}
	if (op == RAWKBD_RESUME)
	{
		rawinput_quiesced = false;
		INS_RawInput_KeyObserveAvailability();
		return 0;
	}
	if (op == RAWKBD_CLEARHELD)
	{
		memset(rawkey_rawheld, 0, sizeof(rawkey_rawheld));
		memset(rawkey_legheld, 0, sizeof(rawkey_legheld));
		return 0;
	}
	if (op != RAWKBD_REMOVE && op != RAWKBD_OPTIN && op != RAWKBD_OBSERVE)
		return 1;
	if (!_RRID)
		return 1;
	if (op == RAWKBD_OBSERVE && (rawkbd_observe || rawkbd_nolegacy))
		return 0;
	if (op == RAWKBD_OBSERVE && !raw)
		return 1;

	Rid.usUsagePage = 0x01;
	Rid.usUsage = 0x06;
	Rid.dwFlags = op == RAWKBD_REMOVE ? RIDEV_REMOVE : op == RAWKBD_OPTIN ?
		RIDEV_NOLEGACY | RIDEV_APPKEYS | RIDEV_NOHOTKEYS : 0;
	Rid.hwndTarget = NULL;
	if (!(*_RRID)(&Rid, 1, sizeof(Rid)))
		return 1;

	observe = op == RAWKBD_OBSERVE;
	if (observe)
	{
		INS_RawKeyReset();
		if (in_raw_legacykey < 0)
			in_raw_legacykey = 0;
		if (in_raw_kbdnohandle < 0)
			in_raw_kbdnohandle = 0;
	}
	rawkbd_observe = observe;
	rawkbd_nolegacy = op == RAWKBD_OPTIN;
	INS_RawInput_KeyObserveAvailability();
	return 0;
}

static int INS_RawInput_KeyboardRequest(rawkbdop_t op)
{
	DWORD thread, process;

	/*No window means no input owner: cleanup and its gate can run locally.
	  Startup registration is retried by ReInit once the window exists.*/
	if (!mainwindow)
		return op == RAWKBD_REMOVE || op == RAWKBD_QUIESCE || op == RAWKBD_RESUME ? INS_RawInput_KeyboardControl(op) : 1;
	thread = GetWindowThreadProcessId(mainwindow, &process);
	if (!thread || process != GetCurrentProcessId())
		return 1;
	if (thread == GetCurrentThreadId())
		return INS_RawInput_KeyboardControl(op);
	return SendMessage(mainwindow, WM_USER_RAWKBD, op, 0) != 0;
}

int INS_RawInput_KeyboardRegister(void)
{
	return INS_RawInput_KeyboardRequest(RAWKBD_OPTIN);
}

/*Flags 0 observation never replaces a successfully acquired opt-in binding.*/
static void INS_RawInput_KeyObserveRegister(void)
{
	if (INS_RawInput_KeyboardRequest(RAWKBD_OBSERVE))
		Con_DPrintf("Raw input: could not observe the keyboard\n");
}

//FTESurf Patch 1424: a raw keyboard report while observing.
static void INS_RawInput_KeyObserve(void)
{
	int sc;
	double now;

	if (!raw->header.hDevice)
	{	//SendInput's: its legacy copy is what reaches the game, and nothing vouches for it
		in_raw_kbdnohandle++;
		sc = raw->data.keyboard.MakeCode & 0xff;
		now = Sys_DoubleTime();
		if (!(raw->data.keyboard.Flags & RI_KEY_BREAK))
			rawkey_nohandletime[sc] = now;
		else if (keydown[MapKey((sc | ((raw->data.keyboard.Flags & RI_KEY_E0) ? 0x100 : 0)) << 16)] ||
			(rawkey_legup[sc] >= 0 && now - rawkey_legup[sc] <= 2*RAWKEY_WINDOW))
			in_raw_injectedkey++;	//FTESurf Patch 1576: SendInput released a key the game held (UIAccess or not)
		return;
	}
	sc = raw->data.keyboard.MakeCode & 0xff;
	if (raw->data.keyboard.Flags & RI_KEY_BREAK)
	{
		rawkey_rawheld[sc] = false;
		return;
	}
	if (rawkey_rawheld[sc])
		return;	//typematic repeat: vouches for nothing
	rawkey_rawheld[sc] = true;
	now = Sys_DoubleTime();
	INS_RawKeyExpire(now);
	if (rawkey_pending[sc] >= 0)
	{	//the legacy press arrived first, inside the window
		rawkey_pending[sc] = -1;
		rawkey_npending--;
		return;
	}
	rawkey_time[sc] = now;
}

//FTESurf Patch 1424: a legacy key message while observing.
static void INS_LegacyKeyObserve(LPARAM lParam, qboolean down)
{
	int sc = (lParam >> 16) & 0xff;
	double now;

	if (rawinput_quiesced || !raw || !rawkbd_observe)
		return;
	now = Sys_DoubleTime();
	INS_RawKeyExpire(now);
	if (!down)
	{
		rawkey_legheld[sc] = false;
		return;
	}
	if (rawkey_legheld[sc])
		return;	//a repeat by this path's own state; lParam bit 30 is the sender's word
	rawkey_legheld[sc] = true;
	if (rawkey_pending[sc] >= 0)
	{	//pressed again before a raw down vouched for the last press
		INS_RawKeyUncorroborated(sc);
		rawkey_pending[sc] = -1;
		rawkey_npending--;
		in_raw_legacykey++;
	}
	if (rawkey_time[sc] >= 0 && now - rawkey_time[sc] <= RAWKEY_WINDOW)
		rawkey_time[sc] = -1;	//consumed: one raw down vouches for one press
	else
	{
		rawkey_pending[sc] = now;
		rawkey_npending++;
	}
}

int INS_RawInput_Register(void)
{
	if (INS_RawInput_MouseRegister())
		return !in_rawinput_keyboard.ival || INS_RawInput_KeyboardRegister();
	return 0;
}

int INS_RawInput_IsRDPDevice(char *cDeviceString)
{
	// mouse is \\?\Root#RDP_MOU#, keyboard is \\?\Root#RDP_KBD#"
	char cRDPString[] = "\\\\?\\Root#RDP_";
	int i;

	if (strlen(cDeviceString) < strlen(cRDPString)) {
		return 0;
	}

	for (i = strlen(cRDPString) - 1; i >= 0; i--)
	{
		if (cDeviceString[i] != cRDPString[i])
			return 0;
	}

	return 1;
}

static void INS_RawInput_InitQuiesced(void)
{
	  // "0" to exclude, "1" to include
	PRAWINPUTDEVICELIST pRawInputDeviceList;
	int inputdevices, i, j, mtemp, ktemp;
	char dname[MAX_RI_DEVICE_SIZE];

	/*A refused teardown still owns these resources and services its binding.*/
	if (raw)
		return;

	/*FTESurf Patch 301: publish what raw input ACTUALLY got, for the .hid header.
	  Zeroed here rather than at the counters below, because every `return` between
	  this line and there is a path on which raw input is unavailable -- a missing
	  user32 export is as much "no raw input" as finding no devices, and leaving the
	  mirrors at their -1 "backend does not report" seed would describe a failure as
	  an unknown.  See in_generic.c's declaration for why they exist at all.*/
	in_rawmice_live = 0;
	in_rawkbd_live = 0;
	in_rawpads_live = 0;	//FTESurf Patch 468: same rule

	// Return 0 if rawinput is not available
	HMODULE user32 = LoadLibrary("user32.dll");
	if (!user32)
	{
		Con_SafePrintf("Raw input: unable to load user32.dll\n");
		return;
	}
	_RRID = (pRegisterRawInputDevices)GetProcAddress(user32,"RegisterRawInputDevices");
	if (!_RRID)
	{
		Con_SafePrintf("Raw input: function RegisterRawInputDevices is not available\n");
		return;
	}
	_GRIDL = (pGetRawInputDeviceList)GetProcAddress(user32,"GetRawInputDeviceList");
	if (!_GRIDL)
	{
		Con_SafePrintf("Raw input: function GetRawInputDeviceList is not available\n");
		return;
	}
	_GRIDIA = (pGetRawInputDeviceInfoA)GetProcAddress(user32,"GetRawInputDeviceInfoA");
	if (!_GRIDIA)
	{
		Con_SafePrintf("Raw input: function GetRawInputDeviceInfoA is not available\n");
		return;
	}
	_GRID = (pGetRawInputData)GetProcAddress(user32,"GetRawInputData");
	if (!_GRID)
	{
		Con_SafePrintf("Raw input: function GetRawInputData is not available\n");
		return;
	}

	rawmicecount = 0;
	rawpadcount = 0;	//FTESurf Patch 468
	rawmice = NULL;
	raw = NULL;
	ribuffersize = 0;
	memset(rawbuttondown, 0, sizeof(rawbuttondown));	//nettest: see INS_RawInput_DeInit
	memset(rawbuttontime, 0, sizeof(rawbuttontime));

	// 1st call to GetRawInputDeviceList: Pass NULL to get the number of devices.
	if ((*_GRIDL)(NULL, &inputdevices, sizeof(RAWINPUTDEVICELIST)) != 0)
	{
		Con_SafePrintf("Raw input: unable to count raw input devices\n");
		return;
	}

	// Allocate the array to hold the DeviceList
	pRawInputDeviceList = Z_Malloc(sizeof(RAWINPUTDEVICELIST) * inputdevices);

	// 2nd call to GetRawInputDeviceList: Pass the pointer to our DeviceList and GetRawInputDeviceList() will fill the array
	if ((*_GRIDL)(pRawInputDeviceList, &inputdevices, sizeof(RAWINPUTDEVICELIST)) == -1)
	{
		Con_SafePrintf("Raw input: unable to get raw input device list\n");
		return;
	}

	// Loop through all devices and count the mice
	for (i = 0, mtemp = 0, ktemp = 0; i < inputdevices; i++)
	{
		j = MAX_RI_DEVICE_SIZE;

		// Get the device name and use it to determine if it's the RDP Terminal Services virtual device.
		if ((*_GRIDIA)(pRawInputDeviceList[i].hDevice, RIDI_DEVICENAME, dname, &j) < 0)
			dname[0] = 0;

		if (!(in_rawinput_rdp.value) && INS_RawInput_IsRDPDevice(dname)) // use rdp (cvar)
			continue;
		if (INS_RawInput_IsRDPDevice(dname))
			rawrdp_bound = true;	//FTESurf Patch 1576: INS_GuardPublish

		switch (pRawInputDeviceList[i].dwType)
		{
		case RIM_TYPEMOUSE:
			if (!in_rawinput_mice.ival)
				continue;

			mtemp++;
			break;
		case RIM_TYPEKEYBOARD:
			if (!in_rawinput_keyboard.ival)
				continue;

			ktemp++;
			break;
		default: // (RIM_TYPEHID) support joysticks?
			/*FTESurf Patch 468: a precision touchpad's digitizer collection.  Only the
			  handle is kept -- its reports are timed, never parsed (see rawpad).*/
			if (pRawInputDeviceList[i].dwType == RIM_TYPEHID && in_rawinput_mice.ival && rawpadcount < countof(rawpad))
			{
				RID_DEVICE_INFO info;
				int isz = sizeof(info);
				memset(&info, 0, sizeof(info));
				info.cbSize = sizeof(info);
				if ((*_GRIDIA)(pRawInputDeviceList[i].hDevice, RIDI_DEVICEINFO, &info, &isz) > 0
					&& info.dwType == RIM_TYPEHID && info.hid.usUsagePage == 0x0D && info.hid.usUsage == 0x05)
				{
					if (!rawpadcount)
					{
						memset(&padmouse, 0, sizeof(padmouse));
						Q_strncpyz(padmouse.sysname, dname, sizeof(padmouse.sysname));
						padmouse.numbuttons = 16;
						padmouse.qdeviceid = DEVID_UNSET;
					}
					rawpad[rawpadcount++] = pRawInputDeviceList[i].hDevice;
					Con_DPrintf("Raw input: precision touchpad [%i] %s\n", i, dname);
				}
			}
			break;
		}
	}

	// exit out if no devices found
	if (!mtemp && !ktemp)
	{
		Con_SafePrintf("Raw input: no usable device found\n");
		rawpadcount = 0;	//FTESurf Patch 468: nothing gets registered on this path
		return;
	}

	// Loop again and bind devices
	rawmice = (mouse_t *)Z_Malloc(sizeof(mouse_t) * mtemp);
	rawkbd = (keyboard_t *)Z_Malloc(sizeof(keyboard_t) * ktemp);
	for (i = 0; i < inputdevices; i++)
	{
		j = MAX_RI_DEVICE_SIZE;

		// Get the device name and use it to determine if it's the RDP Terminal Services virtual device.
		if ((*_GRIDIA)(pRawInputDeviceList[i].hDevice, RIDI_DEVICENAME, dname, &j) < 0)
			dname[0] = 0;

		if (!(in_rawinput_rdp.value) && INS_RawInput_IsRDPDevice(dname)) // use rdp (cvar)
			continue;
		if (INS_RawInput_IsRDPDevice(dname))
			rawrdp_bound = true;	//FTESurf Patch 1576: INS_GuardPublish

		switch (pRawInputDeviceList[i].dwType)
		{
		case RIM_TYPEMOUSE:
			if (!in_rawinput_mice.ival)
				continue;

			// set handle
			rawmice[rawmicecount].handles.rawinputhandle = pRawInputDeviceList[i].hDevice;
			Q_strncpyz(rawmice[rawmicecount].sysname, dname, sizeof(rawmice[rawmicecount].sysname));
			rawmice[rawmicecount].numbuttons = 16;
			rawmice[rawmicecount].oldbuttons = 0;
			rawmice[rawmicecount].qdeviceid = DEVID_UNSET;
			rawmicecount++;
			break;
		case RIM_TYPEKEYBOARD:
			if (!in_rawinput_keyboard.ival)
				continue;

			rawkbd[rawkbdcount].handles.rawinputhandle = pRawInputDeviceList[i].hDevice;
			Q_strncpyz(rawkbd[rawkbdcount].sysname, dname, sizeof(rawkbd[rawkbdcount].sysname));
			rawkbd[rawkbdcount].qdeviceid = DEVID_UNSET;
			rawkbdcount++;
			break;
		default:
			continue;
		}

		// print pretty message about device
		dname[MAX_RI_DEVICE_SIZE - 1] = 0;
		for (mtemp = strlen(dname); mtemp >= 0; mtemp--)
		{
			if (dname[mtemp] == '#')
			{
				dname[mtemp + 1] = 0;
				break;
			}
		}
		Con_DPrintf("Raw input type %i: [%i] %s\n", (int)pRawInputDeviceList[i].dwType, i, dname);
	}


	// free the RAWINPUTDEVICELIST
	Z_Free(pRawInputDeviceList);

	// alloc raw input buffer
	raw = BZ_Malloc(INIT_RIBUFFER_SIZE);
	ribuffersize = INIT_RIBUFFER_SIZE;

	Con_DPrintf("Raw input: initialized with %i mice, %i keyboards and %i touchpads\n", rawmicecount, rawkbdcount, rawpadcount);	//FTESurf Patch 468: the third

	//FTESurf Patch 301: the only place these become non-zero.  This is the success
	//return, so anything that took an earlier one keeps the 0 set at entry.
	in_rawmice_live = rawmicecount;
	in_rawkbd_live = rawkbdcount;

	//FTESurf Patch 306: lift the rejected-report counters out of "this backend does
	//not count" and into "counted, none seen yet".  Conditional, so a vid_restart
	//re-entering this function cannot reset a running total -- the journal reports
	//deltas against a baseline and a counter that went backwards would emit a
	//negative one.  Only ever raised from -1, never cleared, not even at DeInit:
	//a journal open across an input restart must still see what happened.
	if (in_raw_injected < 0)
		in_raw_injected = 0;
	if (in_raw_unenum < 0)
		in_raw_unenum = 0;
	if (in_raw_legacybtn < 0)
		in_raw_legacybtn = 0;	//FTESurf Patch 307, same rule
	if (in_raw_touchpad < 0)
		in_raw_touchpad = 0;	//FTESurf Patch 468, same rule
	if (in_raw_padreports < 0)
		in_raw_padreports = 0;
	if (in_raw_padrelease < 0)
		in_raw_padrelease = 0;
	//in_rawpads_live is published by INS_RawInput_PadRegister: bound, not merely enumerated

	return; // success
}

void INS_RawInput_Init(void)
{
	if (INS_RawInput_KeyboardRequest(RAWKBD_QUIESCE))
		return;
	INS_RawInput_InitQuiesced();
	INS_RawInput_KeyboardRequest(RAWKBD_RESUME);
}
#endif

/*
===========
INS_StartupMouse
===========
*/
void INS_StartupMouse (void)
{
	if ( COM_CheckParm ("-nomouse") )
		return;

	mouseinitialized = true;

	//make sure it can't get stuck
	INS_DeactivateMouse ();

#ifdef AVAIL_DINPUT
	if (in_dinput.value)
	{
		dinput = INS_InitDInput ();

		if (dinput)
		{
			Con_SafePrintf ("DirectInput initialized, version %i\n", (dinput >> 8 & 0xFF));
		}
		else
		{
			Con_SafePrintf ("DirectInput not initialized\n");
		}
	}
	else
		dinput = 0;

	if (!dinput)
#endif
	{
		if (!mouseparmsvalid)
			mouseparmsvalid = SystemParametersInfo (SPI_GETMOUSE, 0, originalmouseparms, 0);

		if (mouseparmsvalid)
		{
			if ( m_accel_noforce.value )
				newmouseparms[2] = originalmouseparms[2];

			if ( m_threshold_noforce.value )
			{
				newmouseparms[0] = originalmouseparms[0];
				newmouseparms[1] = originalmouseparms[1];
			}
		}
	}

	sysmouse.numbuttons = 10;

// if a fullscreen video mode was set before the mouse was initialized,
// set the mouse state appropriately
	if (mouseactivatetoggle)
		INS_ActivateMouse ();
}


/*
===========
INS_Init
===========
*/
void INS_ReInit (void)
{
#ifdef USINGRAWINPUT
	if (INS_RawInput_KeyboardRequest(RAWKBD_QUIESCE))
		return;
	/*Retry an aborted teardown, including explicit opt-out with both settings 0.
	  Never overwrite/reallocate the last usable retained binding resources.*/
	if (raw)
		INS_RawInput_DeInitQuiesced();
	if (!raw && (in_rawinput_mice.ival || in_rawinput_keyboard.ival))
	{
		INS_RawInput_InitQuiesced();
	}
#endif

	INS_StartupMouse ();
	INS_StartupJoystick ();
//	INS_ActivateMouse();

#ifdef USINGRAWINPUT
	//mouse rawinput is always enabled, because its too messy otherwise.
	if (rawmicecount > 0)
	{
		if (INS_RawInput_MouseRegister())
		{
			Con_SafePrintf("Raw input: unable to register raw input for mice, deinitializing\n");
			INS_RawInput_MouseDeRegister();
		}
	}
	INS_RawInput_KeyObserveRegister();	//FTESurf Patch 1424
	INS_RawInput_KeyboardRequest(RAWKBD_RESUME);
#endif
}

/*FTESurf Patch 1576: input other programs make is dropped.  Windows 8+ says where the input
  message being dispatched came from (GetCurrentInputMessageSource); only IMO_HARDWARE passes, so
  SendInput from ordinary programs (IMO_INJECTED), system-injected input (IMO_SYSTEM), posted
  messages (IMO_UNAVAILABLE) and messages sent from another thread are dropped.  Windows reports
  SendInput from a UIAccess program (signed and installed under Program Files, such as the
  on-screen keyboard) as hardware; P1424's observer still counts those.  An in-process hook or a
  device that presents itself as a keyboard or mouse is not seen here.  Each check runs on the
  window thread inside the message's own dispatch, the only place the answer is current.

  The window thread enforces guard_applied, which only INS_GuardPublish (main thread, once a
  frame) changes: a guard switched on applies at once, one switched off keeps applying for
  GUARD_WEAKEN_DELAY after in_guard_live has said so, so a setting turned off and back on between
  two reports, or just before a finish, still reaches the server first.*/
static cvar_t in_keyguard = CVARFD("in_keyguard", "1", CVAR_ARCHIVE|CVAR_NOTFROMSERVER, "Drop keyboard messages that Windows does not report as coming from a keyboard: keys sent by other programs, such as macro tools, remappers and remote-play software. 0 accepts them; runs then do not rank.");
static cvar_t in_guard_live = CVARFD("in_guard_live", "0", CVAR_NOSET|CVAR_NOSAVE|CVAR_NOTFROMSERVER, "Set by the engine: which input guards apply. 1 clicks and wheel from other programs dropped, 2 touchpad and touch input off, 4 keys from other programs dropped, 8 the key guard cannot run here.");
static cvar_t in_guard_rev = CVARFD("in_guard_rev", "0", CVAR_ARCHIVE|CVAR_NOTFROMSERVER, "Which one-time move of the input guard settings this config has had.");
#define GUARD_CLICKS 1
#define GUARD_TOUCH  2
#define GUARD_KEYS   4
#define GUARD_KEYSNA 8
#define GUARD_WEAKEN_DELAY 1.0
#define GUARD_TAINT_TIME 1.5	//an injected release let through keeps bit 4 off this long, so a run reads `changed`
#define KEYGUARD_SUSPEND_AFTER 8	//drops with no hardware key yet: every key is coming from another program
static cvar_t in_guard_lapses = CVARFD("in_guard_lapses", "0", CVAR_NOSET|CVAR_NOSAVE|CVAR_NOTFROMSERVER, "Set by the engine: counts every moment an input guard was not in force or other software reached the game anyway (a guard switched off, a key no keyboard vouched for, a held key's release let through, a console command from another program). The ranked check reads a change during a run as `changed`.");
int in_extcommands;	//console commands from another program (stdin, WM_COPYDATA): each is a lapse
int in_qcdebugged;	//the -plugin pipe drove the QC debugger (it can write game-code globals): cannot run
typedef struct {int devicetype, originid;} fs_inputsource_t;	//INPUT_MESSAGE_SOURCE
#define FS_IMO_HARDWARE 1
static int guard_lastlive;	//main thread
static BOOL (WINAPI *pGetCurrentInputMessageSource)(fs_inputsource_t *src);
static volatile LONG guard_applied = GUARD_CLICKS|GUARD_TOUCH|GUARD_KEYS;
//window thread writes, INS_GuardPublish reads
static volatile LONG keyguard_blocked, keyguard_heldrelease, keyguard_hwseen, keyguard_apifail;
static volatile LONG pointer_blocked, pad_rejected, keyguard_lastorigin = -1;
static qboolean keyguard_dropheld[256];	//window thread: a press dropped and not yet released, so repeats count once
extern int in_raw_keydropped;	//in_generic.c: the .hid's 'kd' record

//0 a hardware device's, 1 another program's, -1 cannot tell (passed, and the guard reports itself off).
static int INS_MessageSource(void)
{
	fs_inputsource_t src;
	if (InSendMessage())
		src.originid = -2;
	else if (!pGetCurrentInputMessageSource(&src))
	{
		InterlockedExchange(&keyguard_apifail, 1);
		return -1;
	}
	else if (src.originid == FS_IMO_HARDWARE)
		return 0;
	keyguard_lastorigin = src.originid;
	return 1;
}

static qboolean INS_KeyGuardBlocks(qboolean down, int qcode, LPARAM lParam, qboolean journal)
{
	int from, sc = (lParam >> 16) & 0xff;
	if (!(guard_applied & GUARD_KEYS) || !pGetCurrentInputMessageSource)
		return false;
	from = INS_MessageSource();
	if (from == 0)
		InterlockedExchange(&keyguard_hwseen, 1);
	if (from != 1)
		return false;
	if (!down && qcode > 0 && qcode < K_MAX && keydown[qcode])
	{	//never strand a held key: the release passes, and the run reads `changed`
		InterlockedIncrement(&keyguard_heldrelease);
		return false;
	}
	InterlockedIncrement(&keyguard_blocked);
	if (!journal)
		return true;
#ifdef USINGRAWINPUT
	if (down && rawkey_pending[sc] >= 0)
	{	//P1424's observer counted it as reaching the game; it did not ('y' is what reached it)
		rawkey_pending[sc] = -1;
		rawkey_npending--;
	}
#endif
	if (!down)
		keyguard_dropheld[sc] = false;
	else if (!keyguard_dropheld[sc])
	{
		keyguard_dropheld[sc] = true;
		in_raw_keydropped++;
	}
	return true;
}

//The low-level hook's own keys (sys_win.c: Print Screen, and the Windows/menu keys under
//sys_disableWinKeys) never become window messages; the hook's flags say who sent them.
qboolean INS_HookKeyBlocks(unsigned int llflags, qboolean down, int qkey)
{
	if (!(guard_applied & GUARD_KEYS) || !(llflags & (LLKHF_INJECTED|LLKHF_LOWER_IL_INJECTED)))
		return false;
	if (!down && qkey > 0 && qkey < K_MAX && keydown[qkey])
	{
		InterlockedIncrement(&keyguard_heldrelease);
		return false;
	}
	InterlockedIncrement(&keyguard_blocked);
	return true;
}

//The window procedures' clicks, wheel and app commands.
qboolean INS_PointerGuardBlocks(int changed)
{
	if (!(guard_applied & GUARD_CLICKS) || !pGetCurrentInputMessageSource || INS_MessageSource() != 1)
		return false;
	if (changed)	//a move Windows made itself (SetCursorPos, activation) is not a click
		InterlockedIncrement(&pointer_blocked);
	return true;
}
qboolean INS_TouchAllowed(void)
{
	return !(guard_applied & GUARD_TOUCH);
}

/*Once per config, after a server connection (the config has run by then): the P1573 move for
  configs written before it, which pin the old values.  The game code's copy cannot set these
  any more (NOTFROMSERVER refuses localcmd).  A value changed back after this stays.*/
static void INS_GuardMigrate(void)
{
	if (in_guard_rev.ival >= 1573)
		return;
#ifdef USINGRAWINPUT
	if (strcmp(in_rawinput_nolegacy.string, "1"))
	{
		Cvar_Set(&in_rawinput_nolegacy, "1");
		Con_Printf("Input: clicks no mouse sent are blocked in game now (in_rawinput_nolegacy 1)\n");
	}
	if (strcmp(in_rawinput_touchpad.string, "0"))
	{
		Cvar_Set(&in_rawinput_touchpad, "0");
		Con_Printf("Input: touchpads no longer move the view or click in game, and runs on one do not rank (in_rawinput_touchpad 1 brings it back, unranked)\n");
	}
#endif
	Cvar_Set(&in_guard_rev, "1576");
}

//Main thread, once a frame: what the settings ask for, what applies, what in_guard_live says.
static void INS_GuardPublish(void)
{
	static qboolean first = true, migrated, suspended, padsaid;
	static LONG blockreported, heldreported, pointerreported, lapsereported;
	static int extreported, legkeyreported, lapses;
	static double offsince[3] = {-1, -1, -1}, taintuntil;
	double now = Sys_DoubleTime();
	int want = 0, cannot = 0, live, applied, b, n;
	char livestr[16];

	if (!migrated && cls.state >= ca_connected)
	{
		migrated = true;
		INS_GuardMigrate();
	}
	if (!suspended && !keyguard_hwseen && keyguard_blocked >= KEYGUARD_SUSPEND_AFTER)
	{
		suspended = true;
		Con_Printf("Input: every key so far came from another program (remote play?), so keys are no longer dropped this session. Runs do not rank until the game is restarted with a keyboard.\n");
	}
#ifdef USINGRAWINPUT
	if (in_rawinput_nolegacy.ival)
		want |= GUARD_CLICKS;
	if (!in_rawinput_touchpad.ival)
		want |= GUARD_TOUCH;
	//the key guard is the source check plus P1424's observer (or the raw opt-in path's handle test)
	if (in_keyguard.ival && (rawkbd_observe || rawkbd_nolegacy) && !suspended)
		want |= GUARD_KEYS;	//suspended: stop dropping, or a remote-play player has no keyboard
#endif
	//what defeats them is read from what is running, not from settings that apply at in_restart
	if (!pGetCurrentInputMessageSource || keyguard_apifail || suspended || in_qcdebugged || GetSystemMetrics(SM_REMOTESESSION))
		cannot = GUARD_CLICKS|GUARD_KEYS;
#ifdef AVAIL_DINPUT
	if (dinput)
		cannot |= GUARD_CLICKS;	//DirectInput reads the mouse with no source to check
#endif
#ifdef USINGRAWINPUT
	if (rawrdp_bound)
		cannot |= GUARD_CLICKS|GUARD_KEYS;	//a remote session's devices pass as hardware
#endif

	applied = first ? want : guard_applied;
	for (b = 0; b < 3; b++)
	{
		if (want & (1<<b))
		{
			applied |= 1<<b;
			offsince[b] = -1;
		}
		else if (applied & (1<<b))
		{
			if (offsince[b] < 0)
				offsince[b] = now;
			else if (now - offsince[b] >= GUARD_WEAKEN_DELAY)
				applied &= ~(1<<b);
		}
	}
	InterlockedExchange(&guard_applied, applied);
	first = false;

	//Lapses: a published bit leaving, a held release let through, a key no keyboard vouched for
	//(Windows passes a UIAccess program's SendInput as hardware; P1424 still sees no device behind it),
	//a console command from another program. CSQC steps `bad` on any change.
	live = want & ~cannot;
	if (in_keyguard.ival && ((cannot & GUARD_KEYS) || !(want & GUARD_KEYS)))
		live |= GUARD_KEYSNA;	//asked for and not running: no setting the player can change helps
	for (b = 0; b < 3; b++)
		if ((guard_lastlive & (1<<b)) && !(live & (1<<b)))
			lapses++;
	guard_lastlive = live;
	n = keyguard_heldrelease;	//once: two threads increment it
	if (n != lapsereported)
	{
		lapses += n - lapsereported;
		lapsereported = n;
	}
	n = in_raw_injectedkey;
	if (n != legkeyreported)
	{
		if (n > legkeyreported)
			lapses += n - legkeyreported;
		legkeyreported = n;
	}
	n = in_extcommands;
	if (n != extreported)
	{
		lapses += n - extreported;
		extreported = n;
	}
	Q_snprintfz(livestr, sizeof(livestr), "%i", lapses);
	if (strcmp(in_guard_lapses.string, livestr))
		Cvar_ForceSet(&in_guard_lapses, livestr);
	if (keyguard_heldrelease != heldreported)
	{
		heldreported = keyguard_heldrelease;
		taintuntil = now + GUARD_TAINT_TIME;
	}
	if (now < taintuntil)
		live &= ~GUARD_KEYS;
	Q_snprintfz(livestr, sizeof(livestr), "%i", live);
	if (strcmp(in_guard_live.string, livestr))	//by string: a launch +set can leave an odd one behind
		Cvar_ForceSet(&in_guard_live, livestr);

	if (keyguard_blocked != blockreported)
	{
		if (!blockreported)
			Con_Printf("Input: dropped a key another program sent (in_keyguard 1). Remappers, macro tools and remote-play software send keys this way; in_keyguard 0 accepts them, and runs then do not rank.\n");
		Con_DPrintf("in_keyguard: %ld dropped, last origin %ld\n", (long)keyguard_blocked, (long)keyguard_lastorigin);
		blockreported = keyguard_blocked;
	}
	if (pointer_blocked != pointerreported)
	{
		if (!pointerreported)
			Con_Printf("Input: dropped a click or wheel turn another program sent (in_rawinput_nolegacy 1).\n");
		pointerreported = pointer_blocked;
	}
	if (pad_rejected && !padsaid)
	{
		padsaid = true;
		Con_Printf("Input: the touchpad is off in game (in_rawinput_touchpad 0). in_rawinput_touchpad 1 turns it on; runs on it do not rank.\n");
	}
}

void INS_Init (void)
{
	//keyboard variables
	Cvar_Register (&cl_keypad, "Input Controls");

#ifdef AVAIL_DINPUT
	Cvar_Register (&in_dinput, "Input Controls");
#endif
#ifdef AVAIL_XINPUT
	Cvar_Register (&in_xinput, "Input Controls");
	Cvar_Register (&xinput_leftvibrator, "Input Controls");
	Cvar_Register (&xinput_rightvibrator, "Input Controls");
#endif
	Cvar_Register (&in_builtinkeymap, "Input Controls");
	Cvar_Register (&in_nonstandarddeadkeys, "Input Controls");
	Cvar_Register (&in_simulatemultitouch, "Input Controls");
	Cvar_Register (&in_focusgrab, "Input Controls");	//FTESurf Patch 268

	Cvar_Register (&m_accel_noforce, "Input Controls");
	Cvar_Register (&m_threshold_noforce, "Input Controls");

	// this looks strange but quake cmdline definitions
	// and MS documentation don't agree with each other
	if (COM_CheckParm ("-noforcemspd"))
		Cvar_Set(&m_accel_noforce, "1");

	if (COM_CheckParm ("-noforcemaccel"))
		Cvar_Set(&m_threshold_noforce, "1");

	if (COM_CheckParm ("-noforcemparms"))
	{
		Cvar_Set(&m_accel_noforce, "1");
		Cvar_Set(&m_threshold_noforce, "1");
	}

	if (COM_CheckParm ("-dinput"))
		Cvar_Set(&in_dinput, "1");

	// joystick variables
	Cvar_Register (&in_joystick, "Joystick variables");

	Cmd_AddCommand ("force_centerview", Force_CenterView_f);

	uiWheelMessage = RegisterWindowMessageA ( "MSWHEEL_ROLLMSG" );

#ifdef USINGRAWINPUT
	Cvar_Register (&in_rawinput_mice, "Input Controls");
	Cvar_Register (&in_rawinput_keyboard, "Input Controls");
	Cvar_Register (&in_rawinput_rdp, "Input Controls");
	Cvar_Register (&in_rawinput_nolegacy, "Input Controls");	//FTESurf Patch 307
	Cvar_Register (&in_rawinput_touchpad, "Input Controls");	//FTESurf Patch 468
#endif
	Cvar_Register (&in_keyguard, "Input Controls");	//FTESurf Patch 1576
	Cvar_Register (&in_guard_live, "Input Controls");
	Cvar_ForceSet(&in_guard_live, "0");	//a launch +set made before this is not the engine's word
	Cvar_Register (&in_guard_rev, "Input Controls");
	Cvar_Register (&in_guard_lapses, "Input Controls");
	Cvar_ForceSet(&in_guard_lapses, "0");
	pGetCurrentInputMessageSource = (void*)GetProcAddress(GetModuleHandleA("user32.dll"), "GetCurrentInputMessageSource");
	if (pGetCurrentInputMessageSource && in_raw_keydropped < 0)
		in_raw_keydropped = 0;	//before any message can be dropped

	INS_ScreenSaver_Init();
}

/*
===========
INS_Shutdown
===========
*/
void INS_Shutdown (void)
{
	INS_DeactivateMouse ();
	INS_ShowMouse ();

	mouseinitialized = false;
	mouseparmsvalid = false;

#ifdef AVAIL_DINPUT
	INS_CloseDInput();
#endif
#ifdef USINGRAWINPUT
	INS_RawInput_DeInit();
#endif
}


/*FTESurf Patch 1410: legacy button bits (1 L, 2 R, 4 M, 8/16 X) of a press that activated the window.
  gl_vidnt.c eats its WM_*BUTTONDOWN and raw input (no RIDEV_INPUTSINK) never gives a background window
  the down, so a WM_MOUSEMOVE carrying its MK_ bit while it is still held (any motion, or the grab's
  SetCursorPos recentre) read as a new press. One mask per path: legacy clears on a message showing it up, raw on
  its up or a new down -- whichever path sees the release first must not unmask the other.*/
static int legacyactivateclick, rawactivateclick;
void INS_MouseActivateClick (int buttons)
{
	legacyactivateclick |= buttons;
	rawactivateclick |= buttons;
	Con_DPrintf("[focus] activating click: mouse buttons %#x are not game keys until released\n", buttons);
}

/*
===========
INS_MouseEvent
===========
a mouse button was pressed/released, mstate is the current set of buttons pressed.
*/
void INS_MouseEvent (int mstate)
{
	int		i;

	if (INS_PointerGuardBlocks(mstate != sysmouse.oldbuttons))
	{	//FTESurf Patch 1576; and a button it pressed stays up until released (P1410's latch)
		legacyactivateclick |= mstate & ~sysmouse.oldbuttons;
		return;
	}

	legacyactivateclick &= mstate;	//FTESurf Patch 1410: held at its old state until released
	mstate = (mstate & ~legacyactivateclick) | (sysmouse.oldbuttons & legacyactivateclick);

	if (dinput && mouseactive)
		return;

#ifdef HLCLIENT
	if (CLHL_MouseEvent(mstate))
		return;
#endif

	if (1)//mouseactive || (key_dest != key_game))
	{
	// perform button actions
		for (i=0 ; i<sysmouse.numbuttons ; i++)
		{
			if ( (mstate & (1<<i)) &&
				!(sysmouse.oldbuttons & (1<<i)) )
			{
				//nettest: this used to be `if (!rawmicecount) ... else mstate &= ~(1<<i);`,
				//which discarded the legacy press whenever ANY mouse was raw-enumerated.
				//rawmicecount is a GLOBAL count and a legacy WM_*BUTTON* does not say which
				//device sent it, so a device raw input never enumerated lost its buttons
				//because some *other* device was covered.  A Windows precision touchpad is
				//a HID digitizer, not a RIM_TYPEMOUSE, so it emits no RI_MOUSE_BUTTON_*
				//flags and the legacy message was the only press it had -- dropping it
				//produced no K_MOUSE1 at all, for taps AND for press-and-hold.  Motion was
				//unaffected (the free-cursor path reads GetCursorPos directly), so the
				//cursor still tracked and menu hover still highlighted: it presented as a
				//dead hit-test rather than as missing input, which is what made it hard to
				//find.  Worked around downstream with in_rawinput 0.
				//
				//WM_INPUT for a physical click is delivered before the synthesised legacy
				//message, so a raw transition on this button in the last 100ms identifies
				//the real duplicate.  Buttons past the five raw reports usButtonFlags for
				//keep the old blanket behaviour, since those arrive via ulRawButtons below
				//and would otherwise double up.
				//
				//The rawbuttondown test is not redundant with the timestamp.  WM_MOUSEMOVE
				//falls into the same window-proc case as the button messages and calls us
				//with the live MK_* flags, and a suppressed press never reaches
				//sysmouse.oldbuttons -- so on the timestamp alone, holding a raw mouse
				//button and moving re-entered this branch 100ms later with the bit set and
				//oldbuttons clear, and fired a spurious second press on every drag.  Raw
				//reports transitions, not held state, so the hold has to be tracked.
				if (!rawmicecount ||
					(i < (int)countof(rawbuttontime) && !rawbuttondown[i] &&
					 Sys_DoubleTime() - rawbuttontime[i] > 0.1))
				{
					/*FTESurf Patch 307: this press is being accepted although raw
					  input -- which IS live, that is what rawmicecount says -- could
					  not corroborate it.  Two causes, and the engine cannot tell them
					  apart here: a pointer that is not a RIM_TYPEMOUSE (a precision
					  touchpad emits no RI_MOUSE_BUTTON_* at all, see above), or an
					  injected click, whose WM_INPUT was dropped at the hDevice test
					  BEFORE the stamping below and so left no trace to match against.
					  Counted rather than blocked: blocking is the in_rawinput_nolegacy
					  trade, and it costs touchpad users every button they have.
					  Bounded to the five buttons raw reports flags for -- past those
					  the old blanket behaviour applies and a miss is not evidence.*/
					/*FTESurf Patch 468: a precision touchpad's own taps now arrive through raw
					  input (accepted inside its window, see padmouse) and are deduped above
					  like any raw press, so on such a build this count no longer includes them.*/
					if (rawmicecount && i < (int)countof(rawbuttontime) &&
						in_raw_legacybtn >= 0)
						in_raw_legacybtn++;
					IN_KeyEvent (sysmouse.qdeviceid, true, K_MOUSE1 + i, 0);
				}
				else
					mstate &= ~(1<<i);
			}

			if ( !(mstate & (1<<i)) &&
				(sysmouse.oldbuttons & (1<<i)) )
			{
				IN_KeyEvent (sysmouse.qdeviceid, false, K_MOUSE1 + i, 0);
			}
		}

		sysmouse.oldbuttons = mstate;
	}
}

/*
===========
INS_MouseMove
===========
*/
void INS_MouseMove (void)
{
#ifdef AVAIL_DINPUT
	if (dinput && mouseactive)
	{
		DIDEVICEOBJECTDATA	od;
		DWORD				dwElements;
		HRESULT				hr;
		int xd = 0, yd = 0, zd = 0;

		for (;;)
		{
			dwElements = 1;

#if (DIRECTINPUT_VERSION >= DINPUT_VERSION_DX7)
			if (dinput >= DINPUT_VERSION_DX7)
			{
				hr = IDirectInputDevice7_GetDeviceData(g_pMouse7,
						sizeof(DIDEVICEOBJECTDATA), &od, &dwElements, 0);

				if ((hr == DIERR_INPUTLOST) || (hr == DIERR_NOTACQUIRED))
				{
					dinput_acquired = true;
					IDirectInputDevice7_Acquire(g_pMouse7);
					break;
				}
			}
			else
#endif
			{
				hr = IDirectInputDevice_GetDeviceData(g_pMouse,
						sizeof(DIDEVICEOBJECTDATA), &od, &dwElements, 0);

				if ((hr == DIERR_INPUTLOST) || (hr == DIERR_NOTACQUIRED))
				{
					dinput_acquired = true;
					IDirectInputDevice_Acquire(g_pMouse);
					break;
				}
			}

			/* Unable to read data or no data available */
			if (FAILED(hr) || dwElements == 0)
			{
				break;
			}

			/* Look at the element to see what happened */

			switch (od.dwOfs)
			{
				case DIMOFS_X:
					xd += od.dwData;
					break;

				case DIMOFS_Y:
					yd += od.dwData;
					break;

				case DIMOFS_Z:
					zd += od.dwData;
					break;

				case DIMOFS_BUTTON0:
				case DIMOFS_BUTTON1:
				case DIMOFS_BUTTON2:
				case DIMOFS_BUTTON3:
#if (DIRECTINPUT_VERSION >= DINPUT_VERSION_DX7)
				case DIMOFS_BUTTON4:
				case DIMOFS_BUTTON5:
				case DIMOFS_BUTTON6:
				case DIMOFS_BUTTON7:
#endif
					IN_KeyEvent(sysmouse.qdeviceid, (od.dwData & 0x80)?true:false, K_MOUSE1 + ((od.dwOfs - DIMOFS_BUTTON0) / (DIMOFS_BUTTON1-DIMOFS_BUTTON0)), 0);
					break;
			}
		}
		if (xd || yd || zd)
			IN_MouseMove(sysmouse.qdeviceid, false, xd, yd, zd, 0);
	}
	else
#endif
	{
		INS_Accumulate();
	}
}


/*
===========
INS_Move
===========
*/
void INS_Move (void)
{
#ifdef USINGRAWINPUT
	/*FTESurf Patch 307.  ONE call site rather than one in each of ActivateMouse and
	  DeactivateMouse, because those two would cover the grab transitions and miss the
	  third case -- the player changing the cvar mid-session, which would then not take
	  effect until the next alt-tab and would read as the setting not working.  Driving
	  it from the live state instead makes all three the same event.  The latch inside
	  makes this two compares on a frame where nothing changed.*/
	INS_RawInput_MouseSetLegacy(mouseactive && (guard_applied & GUARD_CLICKS));	//Patch 1576: the applied guard, see INS_GuardPublish
#endif
	INS_GuardPublish();	//FTESurf Patch 1576

	if (vid.activeapp && !Minimized)
	{
		INS_MouseMove ();
		INS_JoyMove ();
	}
	else
		INS_Accumulate();
}


/*
===========
INS_Accumulate
===========
potentially called multiple times per frame.
*/
void INS_Accumulate (void)
{
	if (mouseactive && !dinput)
	{
		//if you have two programs side by side that both think that they own the mouse cursor then there are certain race conditions when switching between them
		//when alt+tabbing, windows won't wait for a response, so we may have already lost focus by the time we get here, and none of our internal state will know about it.
		//fte won't grab the mouse until its actually inside the window, but other programs don't have similar delays.
		//so when switching to other quake ports, expect fte to detect the oter engine's setcursorpos as a really big mouse move.

		RECT cliprect;
		if (GetClipCursor (&cliprect) && !(
			cliprect.left >= window_rect.left && 
			cliprect.right <= window_rect.right && 
			cliprect.top >= window_rect.top && 
			cliprect.bottom <= window_rect.bottom
			))
			;	//cliprect now covers some area outside of where we asked for.
		else
#ifdef USINGRAWINPUT
		//raw input disables the system mouse, to avoid dupes
		if (!rawmicecount)
#endif
		{
			GetCursorPos (&current_mouse_pos);
			SetCursorPos (window_center_x, window_center_y);

			if (current_mouse_pos.x - window_center_x || current_mouse_pos.y - window_center_y)
				IN_MouseMove(sysmouse.qdeviceid, false, current_mouse_pos.x - window_center_x, current_mouse_pos.y - window_center_y, 0, 0);
		}
		else
		{
			// force the mouse to the center, so there's room to move (rawinput ignore this apparently)
			SetCursorPos (window_center_x, window_center_y);
		}
	}

	if (!mouseactive)
	{
		GetCursorPos (&current_mouse_pos);

		/*
		FTESurf Patch 268: an unfocused instance stops driving the cursor.

		This branch runs unconditionally today -- INS_Move calls INS_Accumulate on
		exactly the frames where we are NOT active, and snd_dma.c calls it again at
		its own rate -- so a backgrounded copy of the game keeps feeding itself
		absolute mouse positions.  That does not move the view (this is the absolute
		path, so it drives mousecursor_x/y), but it does drag the menu cursor around
		and it retypes the device as M_TOUCH in in_generic.c, which changes how that
		mouse is interpreted for the rest of the session.

		The GetCursorPos above deliberately stays OUTSIDE the test: current_mouse_pos
		is written nowhere else, and INS_UpdateGrabs uses it for the windowed
		"is the pointer inside our window" check.  Let it go stale and the grab can
		fail to reinstate when you come back.
		*/
		if (in_focusgrab.ival && !vid.activeapp)
			return;

		IN_MouseMove(sysmouse.qdeviceid, true, current_mouse_pos.x-window_rect.left, current_mouse_pos.y-window_rect.top, 0, 0);
		return;
	}
}

#ifdef USINGRAWINPUT
/*FTESurf Patch 468: a handle-less report that RELEASES a button an accepted pad report
  pressed is accepted outside the window too.  A tap's synthesised up arrives 0.2-0.27 s
  after lift-off here; on a slower timer it would miss the window, leave rawbuttondown[]
  set, and INS_MouseEvent's dedupe would then swallow every later legacy press on that
  button -- the latch INS_RawInput_DeInit's comment names.  Only buttons the pad itself
  pressed qualify, so a synthesised release cannot lift a real mouse's held button, and
  the admitted report is stripped to those release bits before it is applied (see the
  accept branch), so it carries no motion and presses nothing.  Not gated on the cvar:
  turning in_rawinput_touchpad off while a pad button is held must still release it.*/
static qboolean INS_RawInput_PadReleases(void)
{
	int b;
	for (b = 0; b < (int)countof(padbuttondown); b++)
	{
		if ((raw->data.mouse.usButtonFlags & padbtn_up[b]) && padbuttondown[b])
			return true;
	}
	return false;
}

void INS_RawInput_MouseRead(void)
{
	int i, tbuttons, j;
	mouse_t *mouse;

	// find mouse in our mouse list
	for (i = 0; i < rawmicecount; i++)
	{
		if (rawmice[i].handles.rawinputhandle == raw->header.hDevice)
			break;
	}

	mouse = (i < rawmicecount) ? &rawmice[i] : NULL;
	if (!mouse) // we're not tracking this device
	{
		/*FTESurf Patch 306: count what we are about to throw away.  The dwType test
		  is load-bearing, not defensive -- INS_RawInput_Read calls this for EVERY
		  WM_INPUT including keyboard ones, and every keyboard report misses the mouse
		  table, so without it the counter would tick on ordinary typing.*/
		if (raw->header.dwType == RIM_TYPEMOUSE)
		{
			if (!raw->header.hDevice)
			{
				/*FTESurf Patch 468: no handle, but a precision touchpad reported contact
				  inside the window -- the OS synthesised this report from that contact.
				  Accepted onto padmouse and counted apart from the rejections.*/
				double now = Sys_DoubleTime();
				qboolean inwindow = rawpadcount > 0 && !(guard_applied & GUARD_TOUCH) && rawpad_lasttime >= 0
					&& now - rawpad_lasttime <= RAWPAD_WINDOW;
				qboolean release = !inwindow && rawpadcount > 0 && INS_RawInput_PadReleases();
				if (inwindow || release)
				{
					int b;
					if (!in_raw_touchpad)
						Con_DPrintf("Raw input: first handle-less report accepted as touchpad motion, %.1f ms after the digitizer\n", (now - rawpad_lasttime) * 1000);
					if (in_raw_touchpad >= 0)
						in_raw_touchpad++;
					mouse = &padmouse;
					if (release)
					{
						/*Admitted for the release alone (round 3): everything else in the
						  report is stripped, so a report that can only be a late tap-up
						  carries no motion and presses no other button, which would re-arm
						  this rule with no finger on the pad.  Only the UP bits of buttons
						  the pad pressed survive; the extra-button word is left unchanged.*/
						unsigned short keep = 0;
						for (b = 0; b < (int)countof(padbuttondown); b++)
							if (padbuttondown[b])
								keep |= padbtn_up[b];
						raw->data.mouse.usButtonFlags &= keep;
						raw->data.mouse.usFlags = 0;
						raw->data.mouse.lLastX = raw->data.mouse.lLastY = 0;
						raw->data.mouse.ulRawButtons = padmouse.oldbuttons & RI_RAWBUTTON_MASK;
						if (in_raw_padrelease >= 0)
							in_raw_padrelease++;
					}
					for (b = 0; b < (int)countof(padbuttondown); b++)
					{
						if (raw->data.mouse.usButtonFlags & padbtn_down[b])
							padbuttondown[b] = true;
						if (raw->data.mouse.usButtonFlags & padbtn_up[b])
							padbuttondown[b] = false;
					}
					/*the legacy WM_MOUSEWHEEL still delivers the pad's scroll exactly as before
					  this patch, and gl_vidnt.c fires it with no raw gate; the raw copy would be
					  a second notch.  Dropped here, so scrolling on the pad is unchanged.*/
					raw->data.mouse.usButtonFlags &= ~RI_MOUSE_WHEEL;
				}
				else
					in_raw_injected++;
			}
			else
				in_raw_unenum++;
		}
		if (!mouse)
			return;
	}

	if (mouse->qdeviceid == DEVID_UNSET)
	{
		if ((raw->data.mouse.usButtonFlags & (RI_MOUSE_BUTTON_1_DOWN|RI_MOUSE_BUTTON_2_DOWN|RI_MOUSE_BUTTON_3_DOWN|RI_MOUSE_BUTTON_4_DOWN|RI_MOUSE_BUTTON_5_DOWN|RI_MOUSE_WHEEL))
			|| (raw->data.mouse.ulRawButtons & RI_RAWBUTTON_MASK) || raw->data.mouse.lLastX || raw->data.mouse.lLastY)
		{
			mouse->qdeviceid = Mouse_AllocateDevID();
		}
		else
			return;
	}

	multicursor_active[mouse->qdeviceid&7] = 0;

	{	//nettest: note which buttons raw input actually accounted for, so INS_MouseEvent
		//can drop the matching legacy message and only that one.  Deliberately outside
		//the vid.activeapp test below: releases are dispatched even when unfocused, and
		//a release we failed to record would let the next legacy press through as a dupe.
		int b;
		static const unsigned short btndown[countof(rawbuttontime)] =
		{
			RI_MOUSE_BUTTON_1_DOWN, RI_MOUSE_BUTTON_2_DOWN, RI_MOUSE_BUTTON_3_DOWN,
			RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_5_DOWN
		};
		static const unsigned short btnup[countof(rawbuttontime)] =
		{
			RI_MOUSE_BUTTON_1_UP, RI_MOUSE_BUTTON_2_UP, RI_MOUSE_BUTTON_3_UP,
			RI_MOUSE_BUTTON_4_UP, RI_MOUSE_BUTTON_5_UP
		};
		for (b = 0; b < (int)countof(btndown); b++)
		{
			if (raw->data.mouse.usButtonFlags & (btndown[b]|btnup[b]))
				rawbuttontime[b] = Sys_DoubleTime();
			//DOWN first, then UP, so a click fast enough to report both edges in one
			//packet settles as released rather than latching down forever.
			if (raw->data.mouse.usButtonFlags & btndown[b])
				rawbuttondown[b] = true;
			if (raw->data.mouse.usButtonFlags & btnup[b])
				rawbuttondown[b] = false;
			//FTESurf Patch 1410: a down raw sees is a new press; an up with no down is the activating click's
			if (raw->data.mouse.usButtonFlags & btndown[b])
				rawactivateclick &= ~(1<<b);
			else if ((raw->data.mouse.usButtonFlags & btnup[b]) && (rawactivateclick & (1<<b)))
			{
				rawactivateclick &= ~(1<<b);
				raw->data.mouse.usButtonFlags &= ~btnup[b];
			}
		}
	}

	if (vid.activeapp)
	{
		// movement
		if (raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)
		{
			if (in_simulatemultitouch.ival)
			{
				multicursor_active[mouse->qdeviceid&7] = true;
				multicursor_x[mouse->qdeviceid&7] = raw->data.mouse.lLastX;
				multicursor_y[mouse->qdeviceid&7] = raw->data.mouse.lLastY;
			}
			IN_MouseMove(mouse->qdeviceid, true, raw->data.mouse.lLastX, raw->data.mouse.lLastY, 0, 0);
		}
		else if (mouseactive)// RELATIVE
		{
			if (in_simulatemultitouch.ival)
			{
				multicursor_active[mouse->qdeviceid&7] = true;
				multicursor_x[mouse->qdeviceid&7] += raw->data.mouse.lLastX;
				multicursor_y[mouse->qdeviceid&7] += raw->data.mouse.lLastY;
				multicursor_x[mouse->qdeviceid&7] = bound(0, multicursor_x[mouse->qdeviceid&7], vid.pixelwidth);
				multicursor_y[mouse->qdeviceid&7] = bound(0, multicursor_y[mouse->qdeviceid&7], vid.pixelheight);
				IN_MouseMove(mouse->qdeviceid, true, multicursor_x[mouse->qdeviceid&7], multicursor_y[mouse->qdeviceid&7], 0, 0);
			}
			else
				IN_MouseMove(mouse->qdeviceid, false, raw->data.mouse.lLastX, raw->data.mouse.lLastY, 0, 0);
		}

		// button presses
		if (raw->data.mouse.usButtonFlags & RI_MOUSE_BUTTON_1_DOWN)
			IN_KeyEvent(mouse->qdeviceid, true, K_MOUSE1, 0);
		if (raw->data.mouse.usButtonFlags & RI_MOUSE_BUTTON_2_DOWN)
			IN_KeyEvent(mouse->qdeviceid, true, K_MOUSE2, 0);
		if (raw->data.mouse.usButtonFlags & RI_MOUSE_BUTTON_3_DOWN)
			IN_KeyEvent(mouse->qdeviceid, true, K_MOUSE3, 0);
		if (raw->data.mouse.usButtonFlags & RI_MOUSE_BUTTON_4_DOWN)
			IN_KeyEvent(mouse->qdeviceid, true, K_MOUSE4, 0);
		if (raw->data.mouse.usButtonFlags & RI_MOUSE_BUTTON_5_DOWN)
			IN_KeyEvent(mouse->qdeviceid, true, K_MOUSE5, 0);


		// mouse wheel
		if (raw->data.mouse.usButtonFlags & RI_MOUSE_WHEEL)
		{      // If the current message has a mouse_wheel message
			if ((SHORT)raw->data.mouse.usButtonData > 0)
			{
				IN_KeyEvent(mouse->qdeviceid, true, K_MWHEELUP, 0);
				IN_KeyEvent(mouse->qdeviceid, false, K_MWHEELUP, 0);
			}
			if ((SHORT)raw->data.mouse.usButtonData < 0)
			{
				IN_KeyEvent(mouse->qdeviceid, true, K_MWHEELDOWN, 0);
				IN_KeyEvent(mouse->qdeviceid, false, K_MWHEELDOWN, 0);
			}
		}
	}
	//button releass
	if (raw->data.mouse.usButtonFlags & RI_MOUSE_BUTTON_1_UP)
		IN_KeyEvent(mouse->qdeviceid, false, K_MOUSE1, 0);
	if (raw->data.mouse.usButtonFlags & RI_MOUSE_BUTTON_2_UP)
		IN_KeyEvent(mouse->qdeviceid, false, K_MOUSE2, 0);
	if (raw->data.mouse.usButtonFlags & RI_MOUSE_BUTTON_3_UP)
		IN_KeyEvent(mouse->qdeviceid, false, K_MOUSE3, 0);
	if (raw->data.mouse.usButtonFlags & RI_MOUSE_BUTTON_4_UP)
		IN_KeyEvent(mouse->qdeviceid, false, K_MOUSE4, 0);
	if (raw->data.mouse.usButtonFlags & RI_MOUSE_BUTTON_5_UP)
		IN_KeyEvent(mouse->qdeviceid, false, K_MOUSE5, 0);

	// extra buttons
	tbuttons = raw->data.mouse.ulRawButtons & RI_RAWBUTTON_MASK;
	for (j=6 ; j<mouse->numbuttons ; j++)	//FTESurf Patch 468: mouse, not rawmice[i] -- padmouse is not in the table
	{
		if ( (tbuttons & (1<<j)) && !(mouse->oldbuttons & (1<<j)) )
		{
			if (vid.activeapp)
				IN_KeyEvent (mouse->qdeviceid, true, K_MOUSE1 + j, 0);
		}

		if ( !(tbuttons & (1<<j)) && (mouse->oldbuttons & (1<<j)) )
		{
			IN_KeyEvent (mouse->qdeviceid, false, K_MOUSE1 + j, 0);
		}
	}

	mouse->oldbuttons &= ~RI_RAWBUTTON_MASK;
	mouse->oldbuttons |= tbuttons;
}

/*FTESurf Patch 468: a report from the digitizer collection says a finger is on the pad.
  Its content is device-specific and is not parsed; the time is the whole fact.*/
static void INS_RawInput_PadRead(void)
{
	int i;

	if (raw->header.dwType != RIM_TYPEHID)
		return;
	for (i = 0; i < rawpadcount; i++)
	{
		if (rawpad[i] == raw->header.hDevice)
		{
			rawpad_lasttime = Sys_DoubleTime();
			if (guard_applied & GUARD_TOUCH)
				pad_rejected = 1;	//Patch 1576: INS_GuardPublish says so once
			if (in_raw_padreports >= 0)
				in_raw_padreports++;
			return;
		}
	}
}

void INS_RawInput_KeyboardRead(void)
{
	int i;
	qboolean down;
	WPARAM wParam;
	LPARAM lParam;

	if (rawkbd_observe)
	{	//FTESurf Patch 1424: nothing here becomes a key; the legacy message does
		if (raw->header.dwType == RIM_TYPEKEYBOARD)
			INS_RawInput_KeyObserve();
		return;
	}

	if (!rawkbd_nolegacy)
		return;

	for (i = 0; i < rawkbdcount; i++)
	{
		if (rawkbd[i].handles.rawinputhandle == raw->header.hDevice)
			break;
	}

	if (i == rawkbdcount) // not tracking this device
	{
		/*FTESurf Patch 306: the keyboard half.  Note what this CANNOT see: with
		  in_rawinput_keyboard 0 -- the shipped default -- rawkbdcount is 0, nothing
		  is registered, no keyboard WM_INPUT ever arrives, and an injected keystroke
		  takes the legacy WM_KEYDOWN path without passing through here at all.  This
		  counts injected keys only in sessions that opted into raw keyboard.*/
		if (raw->header.dwType == RIM_TYPEKEYBOARD)
		{
			if (!raw->header.hDevice)
				in_raw_injected++;
			else
				in_raw_unenum++;
		}
		return;
	}

	if (rawkbd[i].qdeviceid == DEVID_UNSET)
		rawkbd[i].qdeviceid = Keyboard_AllocateDevID();

	down = !(raw->data.keyboard.Flags & RI_KEY_BREAK);
	wParam = raw->data.keyboard.VKey ;//(-down) & 0xC0000000;
	lParam = (MapVirtualKey(raw->data.keyboard.VKey, 0)<<16) | ((!!(raw->data.keyboard.Flags & RI_KEY_E0))<<24);

 	INS_TranslateKeyEvent(wParam, lParam, down, rawkbd[i].qdeviceid, true);
}

void INS_RawInput_Read(HANDLE in_device_handle)
{
	int dwSize;

	if (rawinput_quiesced || !raw || rawinput_readdepth)
		return;
	rawinput_readdepth++;

	// get raw input
	if ((*_GRID)((HRAWINPUT)in_device_handle, RID_INPUT, NULL, &dwSize, sizeof(RAWINPUTHEADER)) == -1)
	{
		Con_Printf("Raw input: unable to add to get size of raw input header.\n");
		goto done;
	}

	if (dwSize > ribuffersize)
	{
		ribuffersize = dwSize;
		raw = (RAWINPUT *)BZ_Realloc(raw, dwSize);
	}

	if ((*_GRID)((HRAWINPUT)in_device_handle, RID_INPUT, raw, &dwSize, sizeof(RAWINPUTHEADER)) != dwSize ) {
		Con_Printf("Raw input: unable to add to get raw input header.\n");
		goto done;
	}

	/*FTESurf Patch 1424: decide pending presses here, on the window thread that owns the
	  rawkey state.  Every WM_INPUT -- a mouse move included -- is a prompt, so during any
	  activity a press is counted within the window.  A run that goes perfectly idle (mouse
	  dead still, no keys) defers its still-pending presses to the next event: an
	  undercount-safe floor, the same safe direction as Patch 307's click counter.*/
	if (rawkbd_observe && rawkey_npending)
		INS_RawKeyExpire(Sys_DoubleTime());
	INS_RawInput_PadRead();	//FTESurf Patch 468
	INS_RawInput_MouseRead();
	INS_RawInput_KeyboardRead();
done:
	rawinput_readdepth--;
}
#else
int INS_RawInput_KeyboardControl(rawkbdop_t op)
{
	return 1;
}
void INS_RawInput_Read(HANDLE in_device_handle)
{
}
#endif

//FTESurf Patch 898: is ALT down in this thread's key state? keys.c asks when ENTER arrives, because a
//window rebuild or a focus change clears keydown[] while ALT is still held.
qboolean INS_AltHeld (void)
{
	return GetKeyState(VK_MENU) < 0;
}

/*
===================
INS_ClearStates
===================
*/
void INS_ClearStates (void)
{
#ifdef USINGRAWINPUT
	//An up delivered to another window must not leave a key held here.
	INS_RawInput_KeyboardRequest(RAWKBD_CLEARHELD);
#endif

	if (mouseactive)
	{
		memset(&sysmouse, 0, sizeof(sysmouse));
		sysmouse.numbuttons = 10;
	}
}

/*
===============
INS_StartupJoystick
===============
*/
static void INS_StartupJoystickId(unsigned int id)
{
	JOYCAPS		jc;
	MMRESULT	mmr;
	struct wjoy_s *joy;
	if (joy_count == MAX_JOYSTICKS)
		return;
	joy = &wjoy[joy_count];
	memset(joy, 0, sizeof(*joy));
	joy->id = id;
	joy->devid = DEVID_UNSET;//1+joy_count;	//FIXME: this is a hack. make joysticks 1-based. this means mouse0 controls 1st player, joy0 controls 2nd player (controls wrap so joy1 controls 1st player too.

	// get the capabilities of the selected joystick
	// abort startup if command fails
	memset (&jc, 0, sizeof(jc));
	if ((mmr = joyGetDevCaps (joy->id, &jc, sizeof(jc))) != JOYERR_NOERROR)
	{
		Con_Printf ("joystick %03x not found -- invalid joystick capabilities (%x)\n", id, mmr);
		return;
	}

	// save the joystick's number of buttons and POV status
	joy->numbuttons = min(countof(mmjbuttons), jc.wNumButtons);
	joy->haspov = jc.wCaps & JOYCAPS_HASPOV;

	// mark the joystick as available and advanced initialization not completed
	// this is needed as cvars are not available during initialization

	joy_advancedinit = false;

	joy_count++;
}
static void IN_XInput_SetupAudio(struct wjoy_s *joy)
{
#ifdef AVAIL_XINPUT
	char audiodevicename[MAX_QPATH];
	wchar_t mssuck[MAX_QPATH];
	static GUID GUID_NULL;
	GUID gplayback = GUID_NULL;
	GUID gcapture = GUID_NULL;

	if (joy->audiodev)
	{
		S_ShutdownCard(joy->audiodev);
		joy->audiodev = NULL;
	}

	if (!xinput_useaudio)
		return;

	if (!joy->isxinput)
		return;
	if (joy->devid == DEVID_UNSET)
		return;

	if (pXInputGetDSoundAudioDeviceGuids)
	{
		if (pXInputGetDSoundAudioDeviceGuids(joy->id, &gplayback, &gcapture) != ERROR_SUCCESS)
			return;	//probably not plugged in

		if (!memcmp(&gplayback, &GUID_NULL, sizeof(gplayback)))
			return;	//we have a controller, but no headset.

		StringFromGUID2(&gplayback, mssuck, sizeof(mssuck)/sizeof(mssuck[0]));
		narrowen(audiodevicename, sizeof(audiodevicename), mssuck);
		Con_Printf("Controller %i uses directsound device %s\n", joy->id, audiodevicename);
		joy->audiodev = S_SetupDeviceSeat("DirectSound", audiodevicename, joy->devid);
	}
	else if (pXInputGetAudioDeviceIds)
	{
		UINT wccount = countof(mssuck);
		if (!FAILED(pXInputGetAudioDeviceIds(joy->id, mssuck, &wccount, NULL,NULL/*we don't have separate mics, sadly, which also makes config awkward*/)))
		{
	        narrowen(audiodevicename, sizeof(audiodevicename), mssuck);
			Con_Printf("Controller %i uses xaudio2 device %s\n", joy->id, audiodevicename);
			joy->audiodev = S_SetupDeviceSeat("XAudio2", audiodevicename, joy->devid);
		}
	}
#endif
}
void INS_SetupControllerAudioDevices(qboolean enabled)
{
#ifdef AVAIL_XINPUT
	int i;

	xinput_useaudio = enabled;
	for (i = 0; i < joy_count; i++)
		IN_XInput_SetupAudio(&wjoy[i]);
#endif
}
void INS_StartupJoystick (void)
{
	unsigned int	id, numdevs;
	MMRESULT	mmr;

 	// assume no joysticks
	joy_count = 0;

#ifdef AVAIL_XINPUT
	if (in_xinput.ival)
	{
		static dllhandle_t *xinput;
		static const char *dllnames[] =
		{
			"xinput1_4.dll",	//win8+ only. does xaudio2 instead of dsound.
			"xinput1_3.dll",	//dxsdk (vista+). does dsound stuff.
			"xinput9_1_0.dll"	//vista+. doesn't do audio stuff.
		};
		if (!xinput)
		{
			dllfunction_t funcs[] =
			{
				{(void**)&pXInputGetState, "XInputGetState"},
				{(void**)&pXInputSetState, "XInputSetState"},
				{NULL}
			};
			pXInputGetDSoundAudioDeviceGuids = NULL;
			pXInputGetAudioDeviceIds = NULL;
			for (id = 0; id < countof(dllnames); id++)
			{
				xinput = Sys_LoadLibrary(dllnames[id], funcs);
				if (xinput)
					break;
			}

			pXInputGetDSoundAudioDeviceGuids = xinput?Sys_GetAddressForName(xinput, "XInputGetDSoundAudioDeviceGuids"):NULL;
			pXInputGetAudioDeviceIds = xinput?Sys_GetAddressForName(xinput, "XInputGetAudioDeviceIds"):NULL;
		}
		if (pXInputGetState)
		{
			XINPUT_STATE xistate;
			numdevs = 0;
			for (id = 0; id < 4; id++)
			{
				if (joy_count == countof(wjoy))
					break;
				memset(&wjoy[id], 0, sizeof(wjoy[id]));
				wjoy[joy_count].isxinput = true;
				wjoy[joy_count].id = id;
				wjoy[joy_count].devid = DEVID_UNSET;//id;
				wjoy[joy_count].numbuttons = countof(xinputjbuttons);	//xinput supports 16 buttons, we emulate two more with the two triggers.
				joy_count++;

				if (ERROR_DEVICE_NOT_AVAILABLE != pXInputGetState(id, &xistate))
					numdevs++;
			}
			Con_DPrintf("XInput is enabled (%i controllers found)\n", numdevs);
		}
		else
			Con_Printf("XInput (%s) not installed\n", dllnames[1]);
	}
#endif

	// abort startup if user requests no joystick
	if (!in_joystick.ival )
		return;

	// verify joystick driver is present
	if ((numdevs = joyGetNumDevs ()) == 0)
	{
		Con_Printf ("joystick not found -- driver not present\n");
		return;
	}

	mmr = JOYERR_UNPLUGGED;

	// cycle through the joystick ids for the first valid one
	for (id=0 ; id<numdevs ; id++)
	{
		memset (&ji, 0, sizeof(ji));
		ji.dwSize = sizeof(ji);
		ji.dwFlags = JOY_RETURNCENTERED;

		if ((mmr = joyGetPosEx (id, &ji)) == JOYERR_NOERROR)
		{
			INS_StartupJoystickId(id);
		}
	}

	if (joy_count)
		Con_Printf ("found %i joysticks\n", joy_count);
	else
		Con_DPrintf ("found no joysticks\n");
}

/*
===========
INS_Commands
===========
*/
void INS_Commands (void)
{
	int		i;
	DWORD	buttonstate, povstate;
	unsigned int idx;
	struct wjoy_s *joy;

	for (idx = 0; idx < joy_count; idx++)
	{
		joy = &wjoy[idx];

		// loop through the joystick buttons
		// key a joystick event or auxillary event for higher number buttons for each state change
		buttonstate = joy->buttonstate;
		if (buttonstate != joy->oldbuttonstate)
		{
			if (joy->devid == DEVID_UNSET)
			{
				joy->devid = Joy_AllocateDevID();
				IN_XInput_SetupAudio(joy);
			}

			if (joy->isxinput)
			{
				for (i=0 ; i < joy->numbuttons ; i++)
				{
					if ( (buttonstate & (1<<i)) && !(joy->oldbuttonstate & (1<<i)) )
						Key_Event (joy->devid, xinputjbuttons[i], 0, true);

					if ( !(buttonstate & (1<<i)) && (joy->oldbuttonstate & (1<<i)) )
						Key_Event (joy->devid, xinputjbuttons[i], 0, false);
				}
			}
			else
			{
				for (i=0 ; i < joy->numbuttons ; i++)
				{
					if ( (buttonstate & (1<<i)) && !(joy->oldbuttonstate & (1<<i)) )
						Key_Event (joy->devid, mmjbuttons[i], 0, true);

					if ( !(buttonstate & (1<<i)) && (joy->oldbuttonstate & (1<<i)) )
						Key_Event (joy->devid, mmjbuttons[i], 0, false);
				}
			}
			joy->oldbuttonstate = buttonstate;
		}

		if (joy->haspov)
		{
			// convert POV information into 4 bits of state information
			// this avoids any potential problems related to moving from one
			// direction to another without going through the center position
			povstate = 0;
			if(joy->povstate != JOY_POVCENTERED)
			{
				if (joy->povstate == JOY_POVFORWARD)
					povstate |= 0x01;
				if (joy->povstate == JOY_POVRIGHT)
					povstate |= 0x02;
				if (joy->povstate == JOY_POVBACKWARD)
					povstate |= 0x04;
				if (joy->povstate == JOY_POVLEFT)
					povstate |= 0x08;
			}
			if (joy->oldpovstate != povstate)
			{
				if (joy->devid == DEVID_UNSET)
					joy->devid = Joy_AllocateDevID();

				// determine which bits have changed and key an auxillary event for each change
				for (i=0 ; i < 4 ; i++)
				{
					if ( (povstate & (1<<i)) && !(joy->oldpovstate & (1<<i)) )
					{
						Key_Event (joy->devid, K_AUX13 + i, 0, true);
					}

					if ( !(povstate & (1<<i)) && (joy->oldpovstate & (1<<i)) )
					{
						Key_Event (joy->devid, K_AUX13 + i, 0, false);
					}
				}
				joy->oldpovstate = povstate;
			}
		}
	}
}


void INS_DeviceChanged(void *ctx, void *data, size_t a ,size_t b)
{	//called on WM_DEVICECHANGE
	unsigned int idx;
	for (idx = 0; idx < joy_count; idx++)
	{
		wjoy[idx].devstate = DS_UNKNOWN;
	}
}
/*
===============
INS_ReadJoystick
===============
*/
static qboolean INS_ReadJoystick (struct wjoy_s *joy)
{
	if (joy->devstate != DS_NOTPRESENT)
	{	//xinput samples all basically say to poll, but that gives really shit performance.
		//instead we wait until our window thread gets a WM_DEVICECHANGE before we restart polling an inactive device.
		//this can safe us a couple ms each frame.

#ifdef AVAIL_XINPUT
		if (joy->isxinput)
		{
			XINPUT_STATE xistate;
			XINPUT_VIBRATION vibrator;
			HRESULT hr;
			memset(&xistate, 0, sizeof(xistate));
			hr = pXInputGetState(joy->id, &xistate);

			if (in_xinput.ival == 2)
			{
				Con_Printf("xi%i %s, b:%#x r:%u,%u l:%u,%u t:%u,%u\n", joy->id, (hr==ERROR_SUCCESS)?"success":va("%lx", hr), xistate.Gamepad.wButtons,
					xistate.Gamepad.sThumbRX, xistate.Gamepad.sThumbRY, 
					xistate.Gamepad.sThumbLX,xistate.Gamepad.sThumbLY,
					xistate.Gamepad.bLeftTrigger,xistate.Gamepad.bRightTrigger);
			}
			else if (in_xinput.ival == 3 && joy->id == 0)
			{
			//I don't have a controller to test this with, so we fake stuff.
				POINT p;
				GetCursorPos(&p);
				hr = ERROR_SUCCESS;
				xistate.Gamepad.wButtons = rand() & 0xfff0;
				xistate.Gamepad.sThumbRX = 0;//(p.x/1920.0)*0xffff - 0x8000;
				xistate.Gamepad.sThumbRY = 0;//(p.y/1080.0)*0xffff - 0x8000;
				xistate.Gamepad.sThumbLX = (p.x/1920.0)*0xffff - 0x8000;
				xistate.Gamepad.sThumbLY = (p.y/1080.0)*0xffff - 0x8000;
				xistate.Gamepad.bLeftTrigger = 0;
				xistate.Gamepad.bRightTrigger = 0;
			}

			if (hr == ERROR_SUCCESS)
			{	//ERROR_SUCCESS
				//do we care about the dwPacketNumber?
				joy->devstate = DS_PRESENT;
				joy->buttonstate = xistate.Gamepad.wButtons & 0xffff;

				if (xistate.Gamepad.sThumbLY < -16364)
					joy->buttonstate |= 0x00010000;
				if (xistate.Gamepad.sThumbLY > 16364)
					joy->buttonstate |= 0x00020000;
				if (xistate.Gamepad.sThumbLX < -16364)
					joy->buttonstate |= 0x00040000;
				if (xistate.Gamepad.sThumbLX > 16364)
					joy->buttonstate |= 0x00080000;

				if (xistate.Gamepad.sThumbRY < -16364)
					joy->buttonstate |= 0x00100000;
				if (xistate.Gamepad.sThumbRY > 16364)
					joy->buttonstate |= 0x00200000;
				if (xistate.Gamepad.sThumbRX < -16364)
					joy->buttonstate |= 0x00400000;
				if (xistate.Gamepad.sThumbRX > 16364)
					joy->buttonstate |= 0x00800000;

				if (xistate.Gamepad.bLeftTrigger >= 128)
					joy->buttonstate |= 0x01000000;
				if (xistate.Gamepad.bRightTrigger >= 128)
					joy->buttonstate |= 0x02000000;

				if (joy->devid != DEVID_UNSET)
				{
					IN_JoystickAxisEvent(joy->devid, GPAXIS_LT_RIGHT, xistate.Gamepad.sThumbLX / 32768.0);
					IN_JoystickAxisEvent(joy->devid, GPAXIS_LT_DOWN, -xistate.Gamepad.sThumbLY / 32768.0);
					IN_JoystickAxisEvent(joy->devid, GPAXIS_LT_TRIGGER, xistate.Gamepad.bLeftTrigger/255.0);
					IN_JoystickAxisEvent(joy->devid, GPAXIS_RT_RIGHT, xistate.Gamepad.sThumbRX / 32768.0);
					IN_JoystickAxisEvent(joy->devid, GPAXIS_RT_DOWN, -xistate.Gamepad.sThumbRY / 32768.0);
					IN_JoystickAxisEvent(joy->devid, GPAXIS_RT_TRIGGER, xistate.Gamepad.bRightTrigger/255.0);

					vibrator.wLeftMotorSpeed = xinput_leftvibrator.value * 0xffff;
					vibrator.wRightMotorSpeed = xinput_rightvibrator.value * 0xffff;
					pXInputSetState(joy->id, &vibrator);
				}
				return true;
			}
			else if (hr == ERROR_DEVICE_NOT_CONNECTED)
				joy->devstate = DS_NOTPRESENT;
		}
		else
#endif
		{
			memset (&ji, 0, sizeof(ji));
			ji.dwSize = sizeof(ji);
			ji.dwFlags = joy_flags;

			if (joyGetPosEx (joy->id, &ji) == JOYERR_NOERROR)
			{
				joy->devstate = DS_PRESENT;
				joy->povstate = ji.dwPOV;
				joy->buttonstate = ji.dwButtons;
				if (joy->devid != DEVID_UNSET)
				{
					IN_JoystickAxisEvent(joy->devid, GPAXIS_LT_RIGHT, (ji.dwXpos - 32768.0) / 32768);
					IN_JoystickAxisEvent(joy->devid, GPAXIS_LT_DOWN, (ji.dwYpos - 32768.0) / 32768);
					IN_JoystickAxisEvent(joy->devid, GPAXIS_LT_AUX, (ji.dwZpos - 32768.0) / 32768);
					IN_JoystickAxisEvent(joy->devid, GPAXIS_RT_RIGHT, (ji.dwRpos - 32768.0) / 32768);
					IN_JoystickAxisEvent(joy->devid, GPAXIS_RT_DOWN, (ji.dwUpos - 32768.0) / 32768);
					IN_JoystickAxisEvent(joy->devid, GPAXIS_RT_AUX, (ji.dwVpos - 32768.0) / 32768);
				}
				return true;
			}
			else
				joy->devstate = DS_NOTPRESENT;
		}
	}

	joy->povstate = 0;
	joy->buttonstate = 0;
	if (joy->devid != DEVID_UNSET)
	{
		IN_JoystickAxisEvent(joy->devid, 0, 0);
		IN_JoystickAxisEvent(joy->devid, 1, 0);
		IN_JoystickAxisEvent(joy->devid, 2, 0);
		IN_JoystickAxisEvent(joy->devid, 3, 0);
		IN_JoystickAxisEvent(joy->devid, 4, 0);
		IN_JoystickAxisEvent(joy->devid, 5, 0);
	}

	// read error occurred
	// turning off the joystick seems too harsh for 1 read error,
	// but what should be done?
	// Con_Printf ("INS_ReadJoystick: no response\n");
	// joy_avail = false;
	return false;
}

/*
===========
INS_JoyMove
===========
*/
void INS_JoyMove (void)
{
	unsigned int idx;
	for (idx = 0; idx < joy_count; idx++)
	{
		INS_ReadJoystick(&wjoy[idx]);
	}
}

void INS_EnumerateDevices(void *ctx, void(*callback)(void *ctx, const char *type, const char *devicename, unsigned int *qdevid))
{
	int idx;

	for (idx = 0; idx < rawkbdcount; idx++)
		callback(ctx, "keyboard", rawkbd[idx].sysname?rawkbd[idx].sysname:va("rawk%i", idx), &rawkbd[idx].qdeviceid);
	callback(ctx, "keyboard", "system", NULL);

	for (idx = 0; idx < rawmicecount; idx++)
		callback(ctx, "mouse", rawmice[idx].sysname?rawmice[idx].sysname:va("raw%i", idx), &rawmice[idx].qdeviceid);
	if (rawpadcount > 0)	//FTESurf Patch 468: the synthesised motion's own device, so a reader can tell it from a mouse
		callback(ctx, "touchpad", padmouse.sysname, &padmouse.qdeviceid);

#ifdef AVAIL_DINPUT
#if (DIRECTINPUT_VERSION >= DINPUT_VERSION_DX7)
	if (dinput >= DINPUT_VERSION_DX7 && g_pMouse7)
		callback(ctx, "mouse", "di7", NULL);
	else
#endif
		if (dinput && g_pMouse)
		callback(ctx, "mouse", "di", NULL);
#endif
	callback(ctx, "mouse", "system", NULL);

	for (idx = 0; idx < joy_count; idx++)
	{
		int odevid = wjoy[idx].devid;
		if (wjoy[idx].isxinput)
			callback(ctx, "joy", va("xi%i", wjoy[idx].id), &wjoy[idx].devid);
		else
			callback(ctx, "joy", va("mmj%i", wjoy[idx].id), &wjoy[idx].devid);
		if (odevid != wjoy[idx].devid)
			IN_XInput_SetupAudio(&wjoy[idx]);
	}
}

static unsigned short        scantokey[] =
{
//	0			1			2			3			4			5			6				7
//	8			9			A			B			C			D			E				F
	0,			27,			'1',		'2',		'3',		'4',		'5',			'6',		// 0
	'7',		'8',		'9',		'0',		'-',		'=',		K_BACKSPACE,	9,			// 0
	'q',		'w',		'e',		'r',		't',		'y',		'u',			'i',		// 1
	'o',		'p',		'[',		']',		K_ENTER,	K_LCTRL,	'a',			's',		// 1
	'd',		'f',		'g',		'h',		'j',		'k',		'l',			';',		// 2
	'\'',		'`',		K_LSHIFT,	'\\',		'z',		'x',		'c',			'v',		// 2
	'b',		'n',		'm',		',',		'.',		'/',		K_RSHIFT,		K_KP_STAR,	// 3
	K_LALT,		' ',		K_CAPSLOCK,	K_F1,		K_F2,		K_F3,		K_F4,			K_F5,		// 3
	K_F6,		K_F7,		K_F8,		K_F9,		K_F10,		K_PAUSE,	K_SCRLCK,		K_KP_HOME,		// 4
	K_KP_UPARROW,K_KP_PGUP,	K_KP_MINUS,	K_KP_LEFTARROW,K_KP_5,	K_KP_RIGHTARROW,K_KP_PLUS,	K_KP_END,		// 4
	K_KP_DOWNARROW,K_KP_PGDN,K_KP_INS,	K_KP_DEL,	0,			0,			0,				K_F11,		// 5
	K_F12,		0,			0,			0,			0,			0,			0,				0,			// 5
	0,			0,			0,			0,			0,			'\\',		0,				0,			// 6
	0,			0,			0,			0,			0,			0,			0,				0,			// 6
	0,			0,			0,			0,			0,			0,			0,				0,			// 7
	0,			0,			0,			0,			0,			0,			0,				0,			// 7
//	0			1			2			3			4			5			6				7
//	8			9			A			B			C			D			E				F
	0,			0,			0,			0,			0,			0,			0,				0,			// 8
	0,			0,			0,			0,			0,			0,			0,				0,			// 8
	0,			0,			0,			0,			0,			0,			0,				0,			// 9
	0,			0,			0,			0,			0,			0,			0,				0,			// 9
	0,			0,			0,			0,			0,			0,			0,				0,			// a
	0,			0,			0,			0,			0,			0,			0,				0,			// a
	0,			0,			0,			0,			0,			0,			0,				0,			// b
	0,			0,			0,			0,			0,			0,			0,				0,			// b
	0,			0,			0,			0,			0,			0,			0,				0,			// c
	0,			0,			0,			0,			0,			0,			0,				0,			// c
	0,			0,			0,			0,			0,			0,			0,				0,			// d
	0,			0,			0,			0,			0,			0,			0,				0,			// d
	0,			0,			0,			0,			0,			0,			0,				0,			// e
	0,			0,			0,			0,			0,			0,			0,				0,			// e
	0,			0,			0,			0,			0,			0,			0,				0,			// f
	0,			0,			0,			0,			0,			0,			0,				0,			// f
//	0			1			2			3			4			5			6				7
//	8			9			A			B			C			D			E				F
	0,			27,			'1',		'2',		'3',		'4',		'5',			'6',		// 0
	'7',		'8',		'9',		'0',		'-',		'=',		K_BACKSPACE,	9,			// 0
	'q',		'w',		'e',		'r',		't',		'y',		'u',			'i',		// 1
	'o',		'p',		'[',		']',		K_KP_ENTER,	K_RCTRL,	'a',			's',		// 1
	'd',		'f',		'g',		'h',		'j',		'k',		'l',			';',		// 2
	'\'',		'`',		K_SHIFT,	'\\',		'z',		'x',		'c',			'v',		// 2
	'b',		'n',		'm',		',',		'.',		K_KP_SLASH,	K_SHIFT,		K_PRINTSCREEN,// 3
	K_RALT,		' ',		K_CAPSLOCK,	K_F1,		K_F2,		K_F3,		K_F4,			K_F5,		// 3
	K_F6,		K_F7,		K_F8,		K_F9,		K_F10,		K_KP_NUMLOCK,K_SCRLCK,		K_HOME,		// 4
	K_UPARROW,	K_PGUP,		'-',		K_LEFTARROW,0,			K_RIGHTARROW,'+',			K_END,		// 4
	K_DOWNARROW,K_PGDN,		K_INS,		K_DEL,		0,			0,			0,				K_F11,		// 5
	K_F12,		0,			0,			0,			0,			0,			0,				0,			// 5
	0,			0,			0,			0,			0,			'\\',		0,				0,			// 6
	0,			0,			0,			0,			0,			0,			0,				0,			// 6
	0,			0,			0,			0,			0,			0,			0,				0,			// 7
	0,			0,			0,			0,			0,			0,			0,				0			// 7
//	0			1			2			3			4			5			6				7
//	8			9			A			B			C			D			E				F
};

/*
=======
MapKey

Map from windows to quake keynums
=======
*/
static int MapKey (int vkey)
{
	int key;
	key = (vkey>>16)&511;

	if (key < sizeof(scantokey) / sizeof(scantokey[0]))
		key = scantokey[key];
	else
		key = 0;
	if (!cl_keypad.ival)
	{
		switch(key)
		{
		case K_KP_HOME:			return '7';
		case K_KP_UPARROW:		return '8';
		case K_KP_PGUP:			return '9';
		case K_KP_LEFTARROW:	return '4';
		case K_KP_5:			return '5';
		case K_KP_RIGHTARROW:	return '6';
		case K_KP_END:			return '1';
		case K_KP_DOWNARROW:	return '2';
		case K_KP_PGDN:			return '3';
		case K_KP_ENTER:		return K_ENTER;
		case K_KP_INS:			return '0';
		case K_KP_DEL:			return '.';
		case K_KP_SLASH:		return '/';
		case K_KP_MINUS:		return '-';
		case K_KP_PLUS:			return '+';
		case K_KP_STAR:			return '*';
		case K_KP_EQUALS:		return '=';
		}
	}
	if (key == 0)
		Con_DPrintf("key 0x%02x has no translation\n", key);

	return key;
}

void INS_TranslateKeyEvent(WPARAM wParam, LPARAM lParam, qboolean down, int qdeviceid, qboolean genkeystate)
{
	extern cvar_t in_builtinkeymap;
	int qcode;
	int unicode;
	int chars;
	extern int		keyshift[K_MAX];
	extern int		shift_down;

#ifdef USINGRAWINPUT
	if (!genkeystate)
		INS_LegacyKeyObserve(lParam, down);	//FTESurf Patch 1424: only the legacy callers pass false
#endif
	qcode = MapKey(lParam);
	if (!genkeystate && INS_KeyGuardBlocks(down, qcode, lParam, true))
		return;	//FTESurf Patch 1576; the raw opt-in path already drops handle-less reports
#ifdef USINGRAWINPUT
	if (!genkeystate && !down)
		rawkey_legup[(lParam >> 16) & 0xff] = Sys_DoubleTime();	//a release the game took (INS_RawInput_KeyObserve)
#endif

	if (WinNT && !in_builtinkeymap.value)
	{
		BYTE	keystate[256];
		WCHAR	wchars[2];

		if (genkeystate)
		{
			extern qboolean	keydown[K_MAX];
			memset(keystate, 0, sizeof(keystate));
			//128 for held.
			//1 for toggled (ie: caps / num)
			keystate[VK_LSHIFT] = 128*!!keydown[K_LSHIFT];
			keystate[VK_RSHIFT] = 128*!!keydown[K_RSHIFT];
			keystate[VK_LCONTROL] = 128*!!keydown[K_LCTRL];
			keystate[VK_RCONTROL] = 128*!!keydown[K_RCTRL];
			keystate[VK_LMENU] = 128*!!keydown[K_LALT];
			keystate[VK_RMENU] = 128*!!keydown[K_RALT];

			//seems to matter
			keystate[VK_SHIFT] = keystate[VK_LSHIFT]|keystate[VK_RSHIFT];
			keystate[VK_CONTROL] = keystate[VK_LCONTROL]|keystate[VK_RCONTROL];
			keystate[VK_MENU] = keystate[VK_LMENU]|keystate[VK_RMENU];

			keystate[VK_NUMLOCK] = 1;	//doesn't seem to matter?
		}
		else
			GetKeyboardState(keystate);
		chars = ToUnicode(wParam, HIWORD(lParam), keystate, wchars, sizeof(wchars)/sizeof(wchars[0]), 0);
	
		if (chars > 0)
		{
			if (!in_nonstandarddeadkeys.ival)
			{
				for (unicode = 0; unicode < chars-1; unicode++)
					IN_KeyEvent(qdeviceid, down, 0, wchars[unicode]);
			}
			unicode = wchars[chars-1];
		}
		else unicode = 0;
	}
	else
	{
		unicode = (qcode < 128)?qcode:0;
		if (shift_down && unicode < K_MAX && keyshift[unicode])
			unicode = keyshift[unicode]; 
	}
	IN_KeyEvent(qdeviceid, down, qcode, unicode);
}

qboolean INS_KeyToLocalName(int qkey, char *buf, size_t bufsize)
{
	int i;
	*buf = 0;	//assume failure
	for (i = 0; i < countof(scantokey); i++)
	{
		if (!scantokey[i])
			continue;	//not a vkey that quake understands
		if (scantokey[i] == qkey)
		{
			wchar_t tmpbuf[64];
			if (GetKeyNameTextW(i<<16, tmpbuf, sizeof(tmpbuf)))
			{
				narrowen(buf, bufsize, tmpbuf);	//yay for utf-8
				return true;
			}
			break;
		}
	}
	return false;
}


#ifndef APPCOMMAND_BROWSER_BACKWARD			//added in win2k (but was probably used before that too)
#define APPCOMMAND_BROWSER_BACKWARD       1
#define APPCOMMAND_BROWSER_FORWARD        2
#define APPCOMMAND_BROWSER_REFRESH        3
#define APPCOMMAND_BROWSER_STOP           4
#define APPCOMMAND_BROWSER_SEARCH         5
#define APPCOMMAND_BROWSER_FAVORITES      6
#define APPCOMMAND_BROWSER_HOME           7
#define APPCOMMAND_VOLUME_MUTE            8
#define APPCOMMAND_VOLUME_DOWN            9
#define APPCOMMAND_VOLUME_UP              10
#define APPCOMMAND_MEDIA_NEXTTRACK        11
#define APPCOMMAND_MEDIA_PREVIOUSTRACK    12
#define APPCOMMAND_MEDIA_STOP             13
#define APPCOMMAND_MEDIA_PLAY_PAUSE       14
#define APPCOMMAND_LAUNCH_MAIL            15
#define APPCOMMAND_LAUNCH_MEDIA_SELECT    16
#define APPCOMMAND_LAUNCH_APP1            17
#define APPCOMMAND_LAUNCH_APP2            18
#define APPCOMMAND_BASS_DOWN              19
#define APPCOMMAND_BASS_BOOST             20
#define APPCOMMAND_BASS_UP                21
#define APPCOMMAND_TREBLE_DOWN            22
#define APPCOMMAND_TREBLE_UP              23
#endif
#ifndef APPCOMMAND_MICROPHONE_VOLUME_MUTE	//added in winxp
#define APPCOMMAND_MICROPHONE_VOLUME_MUTE 24
#define APPCOMMAND_MICROPHONE_VOLUME_DOWN 25
#define APPCOMMAND_MICROPHONE_VOLUME_UP   26
#define APPCOMMAND_HELP                   27
#define APPCOMMAND_FIND                   28
#define APPCOMMAND_NEW                    29
#define APPCOMMAND_OPEN                   30
#define APPCOMMAND_CLOSE                  31
#define APPCOMMAND_SAVE                   32
#define APPCOMMAND_PRINT                  33
#define APPCOMMAND_UNDO                   34
#define APPCOMMAND_REDO                   35
#define APPCOMMAND_COPY                   36
#define APPCOMMAND_CUT                    37
#define APPCOMMAND_PASTE                  38
#define APPCOMMAND_REPLY_TO_MAIL          39
#define APPCOMMAND_FORWARD_MAIL           40
#define APPCOMMAND_SEND_MAIL              41
#define APPCOMMAND_SPELL_CHECK            42
#define APPCOMMAND_DICTATE_OR_COMMAND_CONTROL_TOGGLE    43
#define APPCOMMAND_MIC_ON_OFF_TOGGLE      44
#define APPCOMMAND_CORRECTION_LIST        45
#define APPCOMMAND_MEDIA_PLAY             46
#define APPCOMMAND_MEDIA_PAUSE            47
#define APPCOMMAND_MEDIA_RECORD           48
#define APPCOMMAND_MEDIA_FAST_FORWARD     49
#define APPCOMMAND_MEDIA_REWIND           50
#define APPCOMMAND_MEDIA_CHANNEL_UP       51
#define APPCOMMAND_MEDIA_CHANNEL_DOWN     52
#endif

int INS_AppCommand(LPARAM lParam)
{
	const char *b;
	int qkey = 0;
	if (INS_KeyGuardBlocks(true, 0, 0, false))
		return true;	//FTESurf Patch 1576: posted or injected; swallowed
	switch(HIWORD(lParam)&0xfff)
	{
	case APPCOMMAND_BROWSER_BACKWARD:	qkey = K_MM_BROWSER_BACK;			break;
	case APPCOMMAND_BROWSER_FAVORITES:	qkey = K_MM_BROWSER_FAVORITES;		break;
	case APPCOMMAND_BROWSER_FORWARD:	qkey = K_MM_BROWSER_FORWARD;		break;
	case APPCOMMAND_BROWSER_HOME:		qkey = K_MM_BROWSER_HOME;			break;
	case APPCOMMAND_BROWSER_REFRESH:	qkey = K_MM_BROWSER_REFRESH;		break;
	case APPCOMMAND_BROWSER_SEARCH:		qkey = K_SEARCH;					break;
	case APPCOMMAND_BROWSER_STOP:		qkey = K_MM_BROWSER_STOP;			break;
	case APPCOMMAND_VOLUME_MUTE:		qkey = K_MM_VOLUME_MUTE;			break;
	case APPCOMMAND_VOLUME_UP:			qkey = K_VOLUP;						break;
	case APPCOMMAND_VOLUME_DOWN:		qkey = K_VOLDOWN;					break;
	case APPCOMMAND_MEDIA_NEXTTRACK:	qkey = K_MM_TRACK_NEXT;				break;
	case APPCOMMAND_MEDIA_PREVIOUSTRACK:qkey = K_MM_TRACK_PREV;				break;
	case APPCOMMAND_MEDIA_STOP:			qkey = K_MM_TRACK_STOP;				break;
	case APPCOMMAND_MEDIA_PLAY_PAUSE:	qkey = K_MM_TRACK_PLAYPAUSE;		break;
	default:
		return false;
	}
	b = Key_GetBinding(qkey, 0, 0);
	if (b && *b)
	{	//only take the key if its actually bound to something, otherwise let the system handle it normally.
		IN_KeyEvent(0, true,  qkey, 0);
		IN_KeyEvent(0, false, qkey, 0);
		return true;
	}
	return false;
}

/*FTESurf Patch 1577: DLL tripwire.  Calls back once per module loaded from outside the game's
  folders and the Windows folder, with its file name only (paths name the user) and where it came
  from: "program" (Program Files, ProgramData), "user" (the profile) or "other".  Overlays, capture
  tools and security software land here too; the journal records, the reader never judges.  Anyone
  can rename or unlist a module, so this only catches tools that do neither.*/
static qboolean INS_PathUnder(const wchar_t *path, const wchar_t *dir)
{
	size_t n;
	if (!dir || !*dir)
		return false;
	n = wcslen(dir);
	while (n && (dir[n-1] == L'\\' || dir[n-1] == L'/'))
		n--;
	return n && !_wcsnicmp(path, dir, n) && (path[n] == L'\\' || path[n] == L'/');
}
typedef struct
{
	wchar_t exe[MAX_PATH], base[MAX_PATH], sys[MAX_PATH], prog[3][MAX_PATH], user[MAX_PATH];
} insdirs_t;
static void INS_ModDirs(insdirs_t *d)
{
	HRESULT (WINAPI *pSHGetFolderPathW)(HWND, int, HANDLE, DWORD, LPWSTR);
	static const int progcsidl[3] = {0x26, 0x2a, 0x23};	//CSIDL_PROGRAM_FILES, _PROGRAM_FILESX86, _COMMON_APPDATA
	wchar_t *slash;
	HMODULE shell;
	int j;

	if (!GetModuleFileNameW(NULL, d->exe, countof(d->exe)))
		d->exe[0] = 0;
	if ((slash = wcsrchr(d->exe, L'\\')))
		*slash = 0;
	if (!MultiByteToWideChar(CP_UTF8, 0, com_gamepath, -1, d->base, countof(d->base)))
		d->base[0] = 0;
	//From Windows, not the environment, which the player sets (SystemRoot=... hides a folder).
	if (!GetSystemWindowsDirectoryW(d->sys, countof(d->sys)))
		d->sys[0] = 0;
	shell = LoadLibraryA("shell32.dll");
	pSHGetFolderPathW = shell ? (void*)GetProcAddress(shell, "SHGetFolderPathW") : NULL;
	for (j = 0; j < countof(d->prog); j++)
		if (!pSHGetFolderPathW || pSHGetFolderPathW(NULL, progcsidl[j], NULL, 0, d->prog[j]) != S_OK)
			d->prog[j][0] = 0;
	if (!pSHGetFolderPathW || pSHGetFolderPathW(NULL, 0x28 /*CSIDL_PROFILE*/, NULL, 0, d->user) != S_OK)
		d->user[0] = 0;
	if (shell)
		FreeLibrary(shell);
}
//"game", "windows", "program", "user" or "other"
static const char *INS_ModClass(const insdirs_t *d, const wchar_t *path)
{
	int j;
	if (INS_PathUnder(path, d->exe) || INS_PathUnder(path, d->base))
		return "game";
	if (INS_PathUnder(path, d->sys))
		return "windows";
	for (j = 0; j < countof(d->prog); j++)
		if (INS_PathUnder(path, d->prog[j]))
			return "program";
	return INS_PathUnder(path, d->user) ? "user" : "other";
}
static void INS_ModToken(const wchar_t *path, char *name, size_t size)
{	//the file name as one printable token
	const wchar_t *slash = wcsrchr(path, L'\\');
	size_t k;
	slash = slash ? slash + 1 : path;
	for (k = 0; slash[k] && k < size - 1; k++)
		name[k] = (slash[k] < 128 && slash[k] > ' ' && slash[k] != '"') ? (char)slash[k] : '_';
	name[k] = 0;
}
int INS_ForeignModules(void *ctx, void (*callback)(void *ctx, const char *cls, const char *name))
{
	BOOL (WINAPI *pEnum)(HANDLE, HMODULE *, DWORD, LPDWORD);
	HMODULE mods[1024];
	DWORD need = 0, n, i;
	wchar_t path[MAX_PATH];
	insdirs_t dirs;
	char name[64];
	int foreign = 0;
	const char *cls;

	pEnum = (void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32EnumProcessModules");
	if (!pEnum || !pEnum(GetCurrentProcess(), mods, sizeof(mods), &need))
		return -1;
	n = need / sizeof(HMODULE);
	if (n > countof(mods))
		n = countof(mods);
	INS_ModDirs(&dirs);
	for (i = 0; i < n; i++)
	{
		if (!GetModuleFileNameW(mods[i], path, countof(path)))
			continue;
		cls = INS_ModClass(&dirs, path);
		if (!strcmp(cls, "game") || !strcmp(cls, "windows"))
			continue;
		INS_ModToken(path, name, sizeof(name));
		callback(ctx, cls, name);
		foreign++;
	}
	return foreign;
}

/*FTESurf Patch 1586: hooks below the journal (ENGINE_PATCHES.md).  While a journal is open a worker
  thread scans this process 0.5-2.5 s after it opens and every 10-20 s: the exe's executable sections
  against its file; the first 32 bytes and export entries of the exports in ins_hookapis against their DLL
  files (each DLL must load from the Windows folder); the import slots of the exe and of those DLLs, in_win.c's
  API pointers and the window procedure against where they resolve; debug registers; executable memory no
  image backs and executable images the loader does not list.  A written journal gets the distinct findings
  of every scan it saw, most telling first, then `hookskip` lines and the `hooks` summary.  Anything inside
  the process can undo a patch around the scans or stop the worker: a tripwire for tools that do neither.*/
#if defined(_WIN64)
#include <tlhelp32.h>
#define INS_HOOKLINES 16
#define INS_HOOKLINELEN 224
#define INS_HOOKSKIPS 16
typedef struct
{
	char lines[INS_HOOKLINES][INS_HOOKLINELEN];	//"hook ..." without the newline, by INS_HookPrio
	int prio[INS_HOOKLINES], nlines;
	char skips[INS_HOOKSKIPS][64];				//"<what> <why>"
	int nskips;
	int found, unread, codebytes, funcs, slots, regions, kib, unlisted, dbg, scans, coderuns;
	double ms;	//the last scan's duration
} inshookset_t;
typedef struct
{
	inshookset_t r;
	insdirs_t dirs;
	struct {const char *dll, *name; const unsigned char *addr;} want[96];	//exports as the DLL files place them
	int nwant;
} inshook_t;
typedef struct
{
	HANDLE f;
	ULONGLONG imagebase;	//the file's preferred base
	IMAGE_DATA_DIRECTORY dir[IMAGE_NUMBEROF_DIRECTORY_ENTRIES];
	IMAGE_SECTION_HEADER sec[96];
	int nsec;
	unsigned char *reloc;
	DWORD reloclen;
} inspe_t;

//Committed, readable and not a guard page (reading one would disarm it), all of [p, p+n).
static qboolean INS_Readable(const void *p, size_t n)
{
	MEMORY_BASIC_INFORMATION mbi;
	const unsigned char *q = p, *end = (const unsigned char*)p + n;
	if (end < q)
		return false;
	while (q < end)
	{
		if (!VirtualQuery(q, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || !mbi.Protect || (mbi.Protect & (PAGE_GUARD|PAGE_NOACCESS)))
			return false;
		q = (const unsigned char*)mbi.BaseAddress + mbi.RegionSize;
	}
	return true;
}
static qboolean INS_ReadSelf(const void *p, void *out, size_t n)
{	//never faults (a page changed between the test and the copy only fails the copy)
	SIZE_T got = 0;
	return INS_Readable(p, n) && ReadProcessMemory(GetCurrentProcess(), p, out, n, &got) && got == n;
}
//The kernel32 calls the worker makes, resolved on the main thread: GetProcAddress reads export tables
//directly, which a tool may have guarded.
static struct
{
	BOOL (WINAPI *GetModuleHandleExW)(DWORD, LPCWSTR, HMODULE *);
	BOOL (WINAPI *IsWow64Process2)(HANDLE, USHORT *, USHORT *);
	HANDLE (WINAPI *OpenThread)(DWORD, BOOL, DWORD);
	BOOL (WINAPI *CheckRemoteDebuggerPresent)(HANDLE, PBOOL);
} insapi;
//GetProcAddress, only once mod's header and export directory are readable; *refused when they are not.
//(A forwarded export's target DLL is read unchecked: a known limit.)
static const void *INS_ProcAddress(HMODULE mod, const char *name, qboolean *refused)
{
	unsigned char hdr[4096];
	const IMAGE_NT_HEADERS64 *nt;
	const IMAGE_DATA_DIRECTORY *ed;
	LONG at;
	*refused = true;
	if (!INS_ReadSelf(mod, hdr, sizeof(hdr)))
		return NULL;
	at = ((const IMAGE_DOS_HEADER*)hdr)->e_lfanew;
	if (at < 0 || at > (LONG)(sizeof(hdr) - sizeof(*nt)))
		return NULL;
	nt = (const void*)(hdr + at);
	ed = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
	if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT || !ed->VirtualAddress || !ed->Size
		|| !INS_Readable((const unsigned char*)mod + ed->VirtualAddress, ed->Size))
		return NULL;
	*refused = false;
	return (const void*)GetProcAddress(mod, name);
}

//Where a jump idiom at p lands, and its length: jmp rel32/rel8, jmp [rip+x] (also REX.W), mov reg,imm64 +
//jmp reg or push reg;ret, push lo + mov [rsp+4],hi + ret; with calls, call rel32 too.  NULL for anything else.
static const unsigned char *INS_JumpTarget(const unsigned char *p, int *len, qboolean calls)
{
	unsigned char b[16];
	ULONGLONG q;
	DWORD lo, hi;
	int rel, r, l;
	const unsigned char *t = NULL;
	MEMORY_BASIC_INFORMATION mbi;
	size_t avail;
	if (!len)
		len = &l;
	avail = sizeof(b);
	if (!INS_ReadSelf(p, b, sizeof(b)))
	{	//a trampoline at the end of a region, before an unreadable one
		if (!VirtualQuery(p, &mbi, sizeof(mbi)) || (avail = (const unsigned char*)mbi.BaseAddress + mbi.RegionSize - p) < 2
			|| avail >= sizeof(b) || !INS_ReadSelf(p, b, avail))
			return NULL;
		memset(b + avail, 0, sizeof(b) - avail);
	}
#define INS_HAVE(n) if ((size_t)(n) > avail) return NULL	//no jump decoded from bytes that were not there
	if (b[0] == 0xe9 || (calls && b[0] == 0xe8))
	{
		INS_HAVE(5);
		memcpy(&rel, b+1, 4);
		*len = 5;
		return p + 5 + rel;
	}
	if (b[0] == 0xeb)
	{
		*len = 2;
		return p + 2 + (signed char)b[1];
	}
	if ((b[0] == 0xff && b[1] == 0x25) || (b[0] == 0x48 && b[1] == 0xff && b[2] == 0x25))
	{
		r = (b[0] == 0x48);
		INS_HAVE(6+r);
		memcpy(&rel, b+2+r, 4);
		*len = rel ? 6+r : 14+r;	//rel 0: the address follows inline
		return INS_ReadSelf(p + 6 + r + rel, &q, 8) ? (const unsigned char*)(uintptr_t)q : NULL;
	}
	if ((b[0] == 0x48 || b[0] == 0x49) && (b[1] & 0xf8) == 0xb8)
	{
		memcpy(&q, b+2, 8);
		r = b[1] & 7;
		if (b[0] == 0x48 && ((b[10] == 0xff && b[11] == 0xe0 + r) || (b[10] == 0x50 + r && b[11] == 0xc3)))
			t = (const unsigned char*)(uintptr_t)q, *len = 12;
		else if (b[0] == 0x49 && b[10] == 0x41 && ((b[11] == 0xff && b[12] == 0xe0 + r) || (b[11] == 0x50 + r && b[12] == 0xc3)))
			t = (const unsigned char*)(uintptr_t)q, *len = 13;
		if (t)
			INS_HAVE(*len);
		return t;
	}
	if (b[0] == 0x68 && b[5] == 0xc7 && b[6] == 0x44 && b[7] == 0x24 && b[8] == 0x04 && b[13] == 0xc3)
	{
		INS_HAVE(14);
		memcpy(&lo, b+1, 4);
		memcpy(&hi, b+9, 4);
		*len = 14;
		return (const unsigned char*)(uintptr_t)(((ULONGLONG)hi << 32) | lo);
	}
	return NULL;
#undef INS_HAVE
}
//first if a jump starts there, else the nearest start up to back bytes earlier whose jump covers first
//(a patch whose leading bytes match the original), else first.
static const unsigned char *INS_PatchStart(const unsigned char *first, int back)
{
	int k, len;
	if (INS_JumpTarget(first, &len, true))
		return first;
	for (k = 1; k <= back; k++)
		if (INS_JumpTarget(first - k, &len, true) && len > k)
			return first - k;
	return first;
}

static HMODULE INS_ModuleAt(const void *addr)
{
	HMODULE mod;
	//4|2: GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|_UNCHANGED_REFCOUNT
	return (insapi.GetModuleHandleExW && insapi.GetModuleHandleExW(4|2, (LPCWSTR)addr, &mod)) ? mod : NULL;
}
//A listed image's INS_ModClass, "image" for one the loader does not list, "unbacked" for private or
//mapped memory, "bad" for none.
static const char *INS_AddrOwner(inshook_t *h, const void *addr, char *name, size_t size)
{
	MEMORY_BASIC_INFORMATION mbi;
	HMODULE mod;
	wchar_t path[MAX_PATH];

	Q_strncpyz(name, "-", size);
	if (!VirtualQuery(addr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
		return "bad";
	if (mbi.Type != MEM_IMAGE)
		return "unbacked";
	if (!(mod = INS_ModuleAt(addr)) || !GetModuleFileNameW(mod, path, countof(path)))
		return "image";
	INS_ModToken(path, name, size);
	return INS_ModClass(&h->dirs, path);
}

static int INS_HookPrio(const char *kind)
{
	static const char *order[] = {"wndproc", "ptr", "dll", "iat", "eat", "code", "api", "draw", "noread"};
	int i;
	for (i = 0; i < countof(order); i++)
		if (!strcmp(kind, order[i]))
			return i;
	return countof(order);
}
//Into a set kept in priority order; one past the cap that ranks no higher is dropped (counted, not named).
//False for a duplicate.
static qboolean INS_HookInsert(inshookset_t *s, const char *text, int pr)
{
	int i, j;
	for (i = 0; i < s->nlines; i++)
		if (!strcmp(s->lines[i], text))
			return false;
	for (i = 0; i < s->nlines && s->prio[i] <= pr; i++)
		;
	if (i >= INS_HOOKLINES)
		return true;
	if (s->nlines < INS_HOOKLINES)
		s->nlines++;
	for (j = s->nlines - 1; j > i; j--)
	{
		memcpy(s->lines[j], s->lines[j-1], INS_HOOKLINELEN);
		s->prio[j] = s->prio[j-1];
	}
	Q_strncpyz(s->lines[i], text, INS_HOOKLINELEN);
	s->prio[i] = pr;
	return true;
}
static void INS_HookLine(inshook_t *h, const char *kind, const char *site, const char *cls, const char *owner, int hops)
{
	char text[INS_HOOKLINELEN];
	Q_snprintfz(text, sizeof(text), "hook %s %s %s %s %i", kind, site, cls, owner, hops);
	if (!INS_HookInsert(&h->r, text, INS_HookPrio(kind)))
		return;
	if (!strcmp(kind, "noread"))
		h->r.unread++;
	else
		h->r.found++;
}
static void INS_HookSkip(inshook_t *h, const char *what, const char *why)
{
	char text[64];
	int i;
	Q_snprintfz(text, sizeof(text), "%s %s", what, why);
	for (i = 0; i < h->r.nskips; i++)
		if (!strcmp(h->r.skips[i], text))
			return;
	if (h->r.nskips < INS_HOOKSKIPS)
		Q_strncpyz(h->r.skips[h->r.nskips++], text, sizeof(h->r.skips[0]));
}
//One finding.  at is patched code (decoded from there) or, with isptr, the address a slot holds; then
//through further jumps (trampolines, thunks) to the code that runs.
static void INS_Hook(inshook_t *h, const char *kind, const char *site, const unsigned char *at, qboolean isptr)
{
	const unsigned char *t = isptr ? at : INS_JumpTarget(at, NULL, true), *next;
	const char *cls = "-";
	char owner[64];
	unsigned char op;
	int hops = 0;

	Q_strncpyz(owner, "-", sizeof(owner));
	if (!t && !isptr && INS_ReadSelf(at, &op, 1) && op == 0xcc)
		cls = "int3";
	while (t && hops < 4)
	{
		hops++;
		cls = INS_AddrOwner(h, t, owner, sizeof(owner));
		if (!(next = INS_JumpTarget(t, NULL, false)))
			break;
		t = next;
	}
	INS_HookLine(h, kind, site, cls, owner, hops);
}

static qboolean INS_PERead(inspe_t *pe, DWORD ofs, void *out, DWORD len)
{
	OVERLAPPED ov;
	DWORD got = 0;
	memset(&ov, 0, sizeof(ov));
	ov.Offset = ofs;
	return ReadFile(pe->f, out, len, &got, &ov) && got == len;
}
//The file offset of [rva, rva+len) when one section's raw data holds all of it, else 0.
static DWORD INS_PEOffset(const inspe_t *pe, DWORD rva, DWORD len)
{
	int i;
	for (i = 0; i < pe->nsec; i++)
		if (rva >= pe->sec[i].VirtualAddress && (ULONGLONG)rva - pe->sec[i].VirtualAddress + len <= pe->sec[i].SizeOfRawData)
			return pe->sec[i].PointerToRawData + rva - pe->sec[i].VirtualAddress;
	return 0;
}
static void INS_PEClose(inspe_t *pe)
{
	if (pe->f && pe->f != INVALID_HANDLE_VALUE)
		CloseHandle(pe->f);
	free(pe->reloc);
	memset(pe, 0, sizeof(*pe));
}
//NULL when the file opened and its headers are the loaded image's; else why not.  "stale": the file is not
//the one mapped (an update waiting on a restart; Wine's placeholders).  "noheader": memory has no PE header.
static const char *INS_PEOpen(inspe_t *pe, const wchar_t *path, const unsigned char *loaded)
{
	unsigned char hdr[4096], mhdr[4096];
	const IMAGE_NT_HEADERS64 *nt, *mnt;
	const IMAGE_SECTION_HEADER *sh;
	LONG at, mat;
	DWORD len, ofs, ndir;

	memset(pe, 0, sizeof(*pe));
	if (!INS_ReadSelf(loaded, mhdr, sizeof(mhdr)))
		return "noheader";
	mat = ((const IMAGE_DOS_HEADER*)mhdr)->e_lfanew;
	mnt = (const void*)(mhdr + mat);
	if (((const IMAGE_DOS_HEADER*)mhdr)->e_magic != IMAGE_DOS_SIGNATURE || mat < 0 || mat > (LONG)(sizeof(mhdr) - sizeof(*mnt))
		|| mnt->Signature != IMAGE_NT_SIGNATURE)
		return "noheader";
	pe->f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
	if (pe->f == INVALID_HANDLE_VALUE || !INS_PERead(pe, 0, hdr, sizeof(hdr)))
		return "unreadable";
	at = ((const IMAGE_DOS_HEADER*)hdr)->e_lfanew;
	if (((const IMAGE_DOS_HEADER*)hdr)->e_magic != IMAGE_DOS_SIGNATURE || at < 0 || at > (LONG)(sizeof(hdr) - sizeof(*nt)))
		return "malformed";
	nt = (const void*)(hdr + at);
	if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
		return "malformed";
	pe->nsec = nt->FileHeader.NumberOfSections;
	sh = (const void*)((const unsigned char*)&nt->OptionalHeader + nt->FileHeader.SizeOfOptionalHeader);
	if (pe->nsec > (int)countof(pe->sec) || (const unsigned char*)(sh + pe->nsec) > hdr + sizeof(hdr))
		return "malformed";
	memcpy(pe->sec, sh, pe->nsec * sizeof(*sh));
	ndir = min(nt->OptionalHeader.NumberOfRvaAndSizes, (DWORD)countof(pe->dir));
	memcpy(pe->dir, nt->OptionalHeader.DataDirectory, ndir * sizeof(pe->dir[0]));
	pe->imagebase = nt->OptionalHeader.ImageBase;
	if (mnt->FileHeader.TimeDateStamp != nt->FileHeader.TimeDateStamp || mnt->OptionalHeader.SizeOfImage != nt->OptionalHeader.SizeOfImage
		|| mnt->OptionalHeader.CheckSum != nt->OptionalHeader.CheckSum)
		return "stale";
	len = pe->dir[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size;
	if (pe->dir[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress && len)
	{
		ofs = INS_PEOffset(pe, pe->dir[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress, len);
		if (!ofs || len > (8<<20) || !(pe->reloc = malloc(len)) || !INS_PERead(pe, ofs, pe->reloc, len))
			return "malformed";
		pe->reloclen = len;
	}
	return NULL;
}
//Adds the load delta to each base relocation lying wholly inside buf, which holds [rva, rva+len) as the file does.
static void INS_PERelocate(const inspe_t *pe, unsigned char *buf, DWORD rva, DWORD len, ULONGLONG delta)
{
	const unsigned char *p = pe->reloc, *end = pe->reloc + pe->reloclen;
	DWORD page, size, k, at, v4;
	ULONGLONG v8;
	WORD e;
	while (end - p >= 8)
	{
		memcpy(&page, p, 4);
		memcpy(&size, p+4, 4);
		if (size < 8 || size > (size_t)(end - p))
			break;
		if ((ULONGLONG)page + 0x1008 > rva && page < (ULONGLONG)rva + len)
			for (k = 8; k + 2 <= size; k += 2)
			{
				memcpy(&e, p+k, 2);
				at = page + (e & 0xfff);
				if ((e >> 12) == IMAGE_REL_BASED_DIR64 && at >= rva && (ULONGLONG)at + 8 <= (ULONGLONG)rva + len)
				{
					memcpy(&v8, buf + at - rva, 8);
					v8 += delta;
					memcpy(buf + at - rva, &v8, 8);
				}
				else if ((e >> 12) == IMAGE_REL_BASED_HIGHLOW && at >= rva && (ULONGLONG)at + 4 <= (ULONGLONG)rva + len)
				{
					memcpy(&v4, buf + at - rva, 4);
					v4 += (DWORD)delta;
					memcpy(buf + at - rva, &v4, 4);
				}
			}
		p += size;
	}
}

static void INS_HookRun(inshook_t *h, const unsigned char *self, DWORD va, DWORD s, DWORD e)
{
	char site[64];
	if (h->r.coderuns++ >= 64)
	{	//a debugger's breakpoints, or a rewritten exe: the count is the finding
		h->r.found++;
		return;
	}
	Q_snprintfz(site, sizeof(site), "exe+%x/%u", (unsigned)(va + s), (unsigned)(e - s + 1));
	INS_Hook(h, "code", site, INS_PatchStart(self + va + s, min(s, 15)), false);
}
//The exe's executable sections, byte for byte against the file.  Returns the bytes compared, -1 if it could not.
static int INS_HookExe(inshook_t *h, inspe_t *pe, const unsigned char *self)
{
	unsigned char *file = NULL, *mem = malloc(65536);
	char site[64];
	int i, bytes = 0;
	qboolean run;
	DWORD va, len, at, n, j, pg, pn, off, rs = 0, last = 0;

	if (!mem)
		return -1;
	for (i = 0; i < pe->nsec; i++)
	{
		if (!(pe->sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE))
			continue;
		va = pe->sec[i].VirtualAddress;
		len = min(pe->sec[i].Misc.VirtualSize, pe->sec[i].SizeOfRawData);
		if (len > (64<<20) || !(file = malloc(len ? len : 1)) || !INS_PERead(pe, pe->sec[i].PointerToRawData, file, len))
		{
			free(file);
			free(mem);
			return -1;
		}
		INS_PERelocate(pe, file, va, len, (uintptr_t)self - pe->imagebase);
		run = false;
		for (at = 0; at < len; at += n)
		{
			n = min(len - at, 65536);
			if (INS_ReadSelf(self + va + at, mem, n))
				bytes += n;
			else for (pg = 0; pg < n; pg += pn)
			{	//page by page, so one unreadable page costs only itself
				pn = min(n - pg, 4096);
				if (INS_ReadSelf(self + va + at + pg, mem + pg, pn))
					bytes += pn;
				else
				{
					memcpy(mem + pg, file + at + pg, pn);
					Q_snprintfz(site, sizeof(site), "exe+%x/%u", (unsigned)(va + at + pg), (unsigned)pn);
					INS_HookLine(h, "noread", site, "-", "-", 0);
				}
			}
			if (!memcmp(mem, file + at, n))
				continue;
			for (j = 0; j < n; j++)
			{
				off = at + j;
				if (mem[j] == file[off])
					continue;
				if (run && off <= last + 8)
				{	//one run: differences at most 8 apart, across chunks too
					last = off;
					continue;
				}
				if (run)
					INS_HookRun(h, self, va, rs, last);
				run = true;
				rs = last = off;
			}
		}
		if (run)
			INS_HookRun(h, self, va, rs, last);
		free(file);
		file = NULL;
	}
	free(mem);
	return bytes;
}

//rva of a named export as the file places it, and its index in AddressOfFunctions; 0 when absent or forwarded.
static DWORD INS_PEExport(const unsigned char *exp, DWORD exprva, DWORD explen, const char *name, WORD *index)
{
	const IMAGE_EXPORT_DIRECTORY *ed = (const void*)exp;
	size_t nl = strlen(name) + 1;
	DWORD i, nr, rva;
	WORD ord;
#define INS_INEXP(r,l) ((r) >= exprva && (ULONGLONG)(r) - exprva + (ULONGLONG)(l) <= explen)
	if (!INS_INEXP(ed->AddressOfNames, ed->NumberOfNames * 4ull) || !INS_INEXP(ed->AddressOfNameOrdinals, ed->NumberOfNames * 2ull)
		|| !INS_INEXP(ed->AddressOfFunctions, ed->NumberOfFunctions * 4ull))
		return 0;
	for (i = 0; i < ed->NumberOfNames; i++)
	{
		memcpy(&nr, exp + ed->AddressOfNames - exprva + i*4, 4);
		if (!INS_INEXP(nr, nl) || memcmp(exp + nr - exprva, name, nl))
			continue;
		memcpy(&ord, exp + ed->AddressOfNameOrdinals - exprva + i*2, 2);
		if (ord >= ed->NumberOfFunctions)
			return 0;
		memcpy(&rva, exp + ed->AddressOfFunctions - exprva + ord*4, 4);
		*index = ord;
		return (rva >= exprva && rva < exprva + explen) ? 0 : rva;
	}
	return 0;
#undef INS_INEXP
}

static const void *INS_Wanted(inshook_t *h, const char *dll, const char *name)
{
	int i;
	for (i = 0; i < h->nwant; i++)
		if (!strcmp(h->want[i].dll, dll) && !strcmp(h->want[i].name, name))
			return h->want[i].addr;
	return NULL;
}
//Each import slot of the image at base against where its export resolves.  Names come from base's file;
//the address from the exporting DLL's file for the exports in ins_hookapis (scanned before their importers),
//else from GetProcAddress.  importer: NULL for the exe.
static void INS_HookImports(inshook_t *h, const unsigned char *base, inspe_t *pe, const char *importer)
{
	const IMAGE_DATA_DIRECTORY *dd = &pe->dir[IMAGE_DIRECTORY_ENTRY_IMPORT];
	const IMAGE_SECTION_HEADER *s = NULL;
	IMAGE_IMPORT_DESCRIPTOR imp;
	unsigned char *buf;
	char dll[64], fn[128], site[INS_HOOKLINELEN - 96], *dot;
	const void *want;
	ULONGLONG ent, held;
	DWORD blen, d, j, k;
	HMODULE mod;
	qboolean refused;
	int i;

	for (i = 0; i < pe->nsec; i++)
		if (dd->VirtualAddress >= pe->sec[i].VirtualAddress && dd->VirtualAddress - pe->sec[i].VirtualAddress < pe->sec[i].SizeOfRawData)
			s = &pe->sec[i];
	if (!dd->VirtualAddress)
		return;
	if (!s || (blen = s->SizeOfRawData) > (16<<20) || !(buf = malloc(blen)))
	{
		INS_HookSkip(h, importer ? importer : "exe", "imports");
		return;
	}
	if (!INS_PERead(pe, s->PointerToRawData, buf, blen))
	{
		INS_HookSkip(h, importer ? importer : "exe", "imports");
		free(buf);
		return;
	}
#define INS_INSEC(r,l) ((r) >= s->VirtualAddress && (ULONGLONG)(r) - s->VirtualAddress + (l) <= blen)
#define INS_STR(r,out) do {for (k = 0; k < sizeof(out) - 1 && INS_INSEC((r)+k, 1) && buf[(r)+k-s->VirtualAddress]; k++) \
	{out[k] = (buf[(r)+k-s->VirtualAddress] > ' ' && buf[(r)+k-s->VirtualAddress] < 127 && buf[(r)+k-s->VirtualAddress] != '"') ? buf[(r)+k-s->VirtualAddress] : '_';} \
	out[k] = 0;} while(0)	//one printable token, as INS_ModToken
	for (d = 0; d < 1024 && INS_INSEC(dd->VirtualAddress + d*sizeof(imp), sizeof(imp)); d++)
	{
		memcpy(&imp, buf + dd->VirtualAddress + d*sizeof(imp) - s->VirtualAddress, sizeof(imp));
		if (!imp.Name)
			break;
		INS_STR(imp.Name, dll);
		if (!imp.OriginalFirstThunk || !imp.FirstThunk || !(mod = GetModuleHandleA(dll)))
			continue;
		for (k = 0; dll[k]; k++)
			dll[k] = (dll[k] >= 'A' && dll[k] <= 'Z') ? dll[k] + 32 : dll[k];
		if ((dot = strrchr(dll, '.')))
			*dot = 0;
		for (j = 0; j < 16384 && INS_INSEC(imp.OriginalFirstThunk + j*8, 8); j++)
		{
			memcpy(&ent, buf + imp.OriginalFirstThunk + j*8 - s->VirtualAddress, 8);
			if (!ent)
				break;
			if (IMAGE_SNAP_BY_ORDINAL64(ent))
			{
				Q_snprintfz(fn, sizeof(fn), "#%u", (unsigned)IMAGE_ORDINAL64(ent));
				want = INS_ProcAddress(mod, MAKEINTRESOURCEA(IMAGE_ORDINAL64(ent)), &refused);
			}
			else
			{
				INS_STR((DWORD)ent + 2, fn);
				if (!*fn)
					continue;
				refused = false;
				if (!(want = INS_Wanted(h, dll, fn)))
					want = INS_ProcAddress(mod, fn, &refused);
			}
			if (refused)
				INS_HookSkip(h, dll, "exports");	//its export directory is not readable
			if (!want)
				continue;
			if (importer)
				Q_snprintfz(site, sizeof(site), "%s/%s!%s", importer, dll, fn);
			else
				Q_snprintfz(site, sizeof(site), "%s!%s", dll, fn);
			if (!INS_ReadSelf(base + imp.FirstThunk + j*8, &held, sizeof(held)))
				INS_HookLine(h, "noread", site, "-", "-", 0);
			else
			{
				h->r.slots++;
				if ((const void*)(uintptr_t)held != want)	//SwapBuffers: the exe imports it, and overlays hook it
					INS_Hook(h, (!strcmp(fn, "SwapBuffers") || !strcmp(fn, "wglSwapBuffers")) ? "draw" : "iat",
						site, (const void*)(uintptr_t)held, true);
			}
		}
	}
#undef INS_STR
#undef INS_INSEC
	free(buf);
}

static const char *const ins_hookapis[] =
{	//per DLL: its name, then exports (input below the journal; what this scan calls), NULL.  An exporter
	//comes before the DLLs importing from it, so their slots are checked against its file.
	"ntdll", "NtReadVirtualMemory", "NtQueryVirtualMemory", "NtGetContextThread", "NtReadFile", NULL,
	"win32u", "NtUserGetRawInputData", "NtUserGetRawInputBuffer", "NtUserGetCurrentInputMessageSource",
		"NtUserPeekMessage", "NtUserGetMessage", "NtUserGetAsyncKeyState", "NtUserGetKeyState",
		"NtUserGetKeyboardState", "NtUserGetRawInputDeviceInfo", "NtUserSetWindowsHookEx", NULL,
	"kernelbase", "ReadProcessMemory", "VirtualQuery", "GetThreadContext", "ReadFile", "GetModuleHandleExW", NULL,
	"user32", "GetCurrentInputMessageSource", "InSendMessage", "InSendMessageEx", "GetRawInputData",
		"GetRawInputBuffer", "GetRawInputDeviceInfoA", "GetRawInputDeviceInfoW", "GetRawInputDeviceList",
		"RegisterRawInputDevices", "GetCursorPos", "GetPhysicalCursorPos", "SetCursorPos", "GetAsyncKeyState",
		"GetKeyState", "GetKeyboardState", "PeekMessageA", "PeekMessageW", "GetMessageA", "GetMessageW",
		"DispatchMessageA", "DispatchMessageW", "TranslateMessage", "CallWindowProcA", "CallWindowProcW",
		"CallNextHookEx", "SetWindowsHookExA", "SetWindowsHookExW", "GetMessageExtraInfo",
		"GetWindowLongPtrA", "GetWindowLongPtrW", NULL,
	"xinput1_4", "XInputGetState", NULL,
	"xinput1_3", "XInputGetState", NULL,
	"xinput9_1_0", "XInputGetState", NULL,
	"gdi32", "SwapBuffers", NULL,	//draw: overlays hook these two, which shows the check seeing a real hook
	"opengl32", "wglSwapBuffers", NULL,
	NULL
};
//One export's first 32 bytes, and its export entry in memory, against the file.  False when not compared
//(no room in its section, unreadable, an alias of one already compared).
static qboolean INS_HookAPI(inshook_t *h, inspe_t *pe, const IMAGE_EXPORT_DIRECTORY *ed, HMODULE mod,
	const char *dll, const char *name, DWORD rva, WORD index, qboolean draw)
{
	const unsigned char *at = (const unsigned char*)mod + rva;
	unsigned char file[48], mem[48];
	DWORD held;
	DWORD ofs = rva >= 8 ? INS_PEOffset(pe, rva - 8, sizeof(file)) : 0;
	char site[96];
	int i, k;
	qboolean alias = false;

	for (i = 0; i < h->nwant; i++)
		if (h->want[i].addr == at)
			alias = true;	//GetCursorPos and GetPhysicalCursorPos are one function on Windows 11
	if (h->nwant < countof(h->want))
	{
		h->want[h->nwant].dll = dll;
		h->want[h->nwant].name = name;
		h->want[h->nwant++].addr = at;
	}
	Q_snprintfz(site, sizeof(site), "%s!%s", dll, name);
	if (!INS_ReadSelf((const unsigned char*)mod + ed->AddressOfFunctions + index*4, &held, sizeof(held)))
		INS_HookLine(h, "noread", site, "-", "-", 0);
	else if (held != rva)	//GetProcAddress, and anything bound later, now resolves elsewhere
		INS_Hook(h, "eat", site, (const unsigned char*)mod + held, true);
	if (alias || !ofs || !INS_PERead(pe, ofs, file, sizeof(file)))
		return false;
	INS_PERelocate(pe, file, rva - 8, sizeof(file), (uintptr_t)mod - pe->imagebase);
	if (!INS_ReadSelf(at - 8, mem, sizeof(mem)))
	{
		INS_HookLine(h, "noread", site, "-", "-", 0);
		return false;
	}
	for (k = 8; k < 40 && mem[k] == file[k]; k++)
		;
	if (k < 40)
	{
		if (k > 8)
			Q_snprintfz(site, sizeof(site), "%s!%s+%i", dll, name, k - 8);
		INS_Hook(h, draw ? "draw" : "api", site, INS_PatchStart(at + k - 8, k - 8), false);
	}
	return true;
}
static void INS_HookAPIs(inshook_t *h)
{
	USHORT procm = 0, nativem = 0;
	qboolean emulated, draw;
	const char *const *a = ins_hookapis;
	const char *dll, *why, *cls;
	unsigned char *exp;
	char text[96], owner[64];
	wchar_t path[MAX_PATH];
	HMODULE mod;
	inspe_t pe;
	DWORD exprva, explen, ofs, rva;
	WORD index;

	//x64 under ARM64 Windows: system DLLs are ARM64X, rewritten for x64 callers as they load, so no file speaks for them
	emulated = insapi.IsWow64Process2 && insapi.IsWow64Process2(GetCurrentProcess(), &procm, &nativem) && nativem == 0xaa64;
	while ((dll = *a++))
	{
		Q_snprintfz(text, sizeof(text), "%s.dll", dll);
		draw = !strcmp(dll, "gdi32") || !strcmp(dll, "opengl32");
		exp = NULL;
		memset(&pe, 0, sizeof(pe));
		if (!(mod = GetModuleHandleA(text)) || !GetModuleFileNameW(mod, path, countof(path)))
			why = "-";	//not loaded, so not on the input path (no XInput yet, a D3D renderer)
		else if (strcmp((cls = INS_ModClass(&h->dirs, path)), "windows"))
		{	//loaded from elsewhere: a stand-in, which its own file would vouch for
			INS_ModToken(path, owner, sizeof(owner));
			INS_HookLine(h, "dll", dll, cls, owner, 0);
			why = "-";
		}
		else
			why = emulated ? "emulated" : INS_PEOpen(&pe, path, (const void*)mod);
		exprva = pe.dir[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
		explen = pe.dir[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
		if (!why)
		{
			ofs = INS_PEOffset(&pe, exprva, explen);
			if (!ofs || explen < sizeof(IMAGE_EXPORT_DIRECTORY) || explen > (4<<20) || !(exp = malloc(explen)) || !INS_PERead(&pe, ofs, exp, explen))
				why = "malformed";
		}
		if (!why)
		{
			unsigned char mh[4096];
			IMAGE_EXPORT_DIRECTORY med;
			const IMAGE_EXPORT_DIRECTORY *fed = (const void*)exp;
			LONG at;
			Q_snprintfz(text, sizeof(text), "%s!exports", dll);
			if (!INS_ReadSelf(mod, mh, sizeof(mh)) || (at = ((const IMAGE_DOS_HEADER*)mh)->e_lfanew) < 0 || at > (LONG)(sizeof(mh) - sizeof(IMAGE_NT_HEADERS64))
				|| !INS_ReadSelf((const unsigned char*)mod + exprva, &med, sizeof(med)))
				INS_HookLine(h, "noread", text, "-", "-", 0);
			else if (memcmp(&((const IMAGE_NT_HEADERS64*)(mh + at))->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT],
					&pe.dir[IMAGE_DIRECTORY_ENTRY_EXPORT], sizeof(IMAGE_DATA_DIRECTORY))
				|| med.AddressOfFunctions != fed->AddressOfFunctions || med.AddressOfNames != fed->AddressOfNames
				|| med.AddressOfNameOrdinals != fed->AddressOfNameOrdinals || med.NumberOfFunctions != fed->NumberOfFunctions
				|| med.NumberOfNames != fed->NumberOfNames)
				INS_Hook(h, "eat", text, (const unsigned char*)mod + med.AddressOfFunctions, true);
		}
		if (why && *why != '-')
			INS_HookSkip(h, dll, why);
		for (; *a; a++)
			if (!why && (rva = INS_PEExport(exp, exprva, explen, *a, &index))
				&& INS_HookAPI(h, &pe, (const void*)exp, mod, dll, *a, rva, index, draw) && !draw)
				h->r.funcs++;
		a++;
		if (!why)
			INS_HookImports(h, (const void*)mod, &pe, dll);
		free(exp);
		INS_PEClose(&pe);
	}
}

static void INS_HookPointer(inshook_t *h, const char *dll, const char *name, const void *p)
{
	const void *want = INS_Wanted(h, dll, name);
	char site[96];
	HMODULE mod;
	qboolean refused = false;
	if (!p)
		return;
	Q_snprintfz(site, sizeof(site), "%s.dll", dll);
	if (!want && (mod = GetModuleHandleA(site)))
		want = INS_ProcAddress(mod, name, &refused);
	if (refused)
		INS_HookSkip(h, dll, "exports");
	if (!want)
		return;
	h->r.slots++;
	if (p != want)
	{
		Q_snprintfz(site, sizeof(site), "%s!%s", dll, name);
		INS_Hook(h, "ptr", site, p, true);
	}
}
//Threads with a hardware breakpoint armed (a debugger, or a tool's exception handler, changes no bytes);
//-1 if some thread's could not be read.
static int INS_HookDebugRegs(void)
{
	THREADENTRY32 te;
	CONTEXT ctx;
	HANDLE snap, th;
	DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
	BOOL more;
	int armed = 0, missed = 0;

	if (!insapi.OpenThread || (snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)) == INVALID_HANDLE_VALUE)
		return -1;
	te.dwSize = sizeof(te);
	for (more = Thread32First(snap, &te); more; more = Thread32Next(snap, &te))
	{
		if (te.th32OwnerProcessID != pid)
			continue;
		th = (te.th32ThreadID == self) ? GetCurrentThread() : insapi.OpenThread(THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
		if (!th)
		{	//one that exited since the snapshot is no loss; one we may not open is
			missed += (GetLastError() == ERROR_ACCESS_DENIED);
			te.dwSize = sizeof(te);
			continue;
		}
		memset(&ctx, 0, sizeof(ctx));
		ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
		if (!GetThreadContext(th, &ctx))
		{	//access denied is also what a thread that is exiting answers
			DWORD code = 0;
			missed += (GetLastError() == ERROR_ACCESS_DENIED && GetExitCodeThread(th, &code) && code == STILL_ACTIVE);
		}
		else if ((ctx.Dr7 & 0xff) && (ctx.Dr0 || ctx.Dr1 || ctx.Dr2 || ctx.Dr3))
			armed++;
		if (te.th32ThreadID != self)
			CloseHandle(th);
		te.dwSize = sizeof(te);
	}
	CloseHandle(snap);
	return armed ? armed : missed ? -1 : 0;
}

static void INS_HookScan(inshook_t *h)
{
	MEMORY_BASIC_INFORMATION mbi;
	const unsigned char *self = (const void*)GetModuleHandleW(NULL), *p = NULL, *lastalloc = NULL;
	const void *wp;
	const char *cls, *why;
	ULONGLONG execbytes = 0;
	BOOL remote = FALSE;
	char owner[64];
	wchar_t path[MAX_PATH];
	HMODULE mod;
	HWND w = mainwindow;
	inspe_t pe;
	int dr;
	double t0 = Sys_DoubleTime();

	INS_HookAPIs(h);
	memset(&pe, 0, sizeof(pe));
	why = GetModuleFileNameW(NULL, path, countof(path)) ? INS_PEOpen(&pe, path, self) : "unreadable";
	if (why)
	{
		INS_HookSkip(h, "exe", why);
		h->r.codebytes = -1;
	}
	else
	{
		INS_HookImports(h, self, &pe, NULL);
		h->r.codebytes = INS_HookExe(h, &pe, self);
		if (h->r.codebytes < 0)
			INS_HookSkip(h, "exe", "unreadable");
	}
	INS_PEClose(&pe);
#ifdef USINGRAWINPUT
	INS_HookPointer(h, "user32", "GetRawInputData", (const void*)_GRID);
	INS_HookPointer(h, "user32", "GetRawInputDeviceList", (const void*)_GRIDL);
	INS_HookPointer(h, "user32", "GetRawInputDeviceInfoA", (const void*)_GRIDIA);
	INS_HookPointer(h, "user32", "RegisterRawInputDevices", (const void*)_RRID);
#endif
	INS_HookPointer(h, "user32", "GetCurrentInputMessageSource", (const void*)pGetCurrentInputMessageSource);
#ifdef AVAIL_XINPUT
	INS_HookPointer(h, GetModuleHandleA("xinput1_4.dll") ? "xinput1_4" : GetModuleHandleA("xinput1_3.dll") ? "xinput1_3" : "xinput9_1_0",
		"XInputGetState", (const void*)pXInputGetState);
#endif
	if (w && IsWindow(w))
	{	//a subclassed window sees every message first: only the exe itself or a Windows DLL may own the head
		//of the chain (opengl32.dll subclasses it under GL, measured).  Read once: the renderer may destroy it.
		wp = (const void*)(IsWindowUnicode(w) ? GetWindowLongPtrW(w, GWLP_WNDPROC) : GetWindowLongPtrA(w, GWLP_WNDPROC));
		if (wp && !strcmp(INS_AddrOwner(h, wp, owner, sizeof(owner)), "bad"))	//a CallWindowProc handle: the subclass used the other charset
			wp = (const void*)(IsWindowUnicode(w) ? GetWindowLongPtrA(w, GWLP_WNDPROC) : GetWindowLongPtrW(w, GWLP_WNDPROC));
		if (wp && IsWindow(w))
		{
			cls = INS_AddrOwner(h, wp, owner, sizeof(owner));
			mod = INS_ModuleAt(wp);
			h->r.slots++;
			if (!mod || (mod != (HMODULE)self && strcmp(cls, "windows")))
				INS_Hook(h, "wndproc", "window", wp, true);
		}
	}
	//Executable memory no image backs (JITs, overlay trampolines, manually mapped code), and executable
	//images the loader does not list.
	while (VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi))
	{
		if (mbi.State == MEM_COMMIT && (mbi.Protect & 0xf0) && !(mbi.Protect & (PAGE_GUARD|PAGE_NOACCESS)))
		{	//0xf0: PAGE_EXECUTE, _READ, _READWRITE, _WRITECOPY
			if (mbi.Type != MEM_IMAGE)
			{
				h->r.regions++;
				execbytes += mbi.RegionSize;
			}
			else if (mbi.AllocationBase != lastalloc)
			{
				lastalloc = mbi.AllocationBase;
				if (!INS_ModuleAt(mbi.AllocationBase))
					h->r.unlisted++;
			}
		}
		if ((const unsigned char*)mbi.BaseAddress + mbi.RegionSize <= p)
			break;
		p = (const unsigned char*)mbi.BaseAddress + mbi.RegionSize;
	}
	h->r.kib = (int)(execbytes >> 10);
	if (insapi.CheckRemoteDebuggerPresent && !insapi.CheckRemoteDebuggerPresent(GetCurrentProcess(), &remote))
		remote = FALSE;
	dr = INS_HookDebugRegs();
	h->r.dbg = ((IsDebuggerPresent() || remote) ? 1 : 0) | (dr > 0 ? 2 : 0) | (dr < 0 ? 4 : 0);
	h->r.scans = 1;
	h->r.ms = (Sys_DoubleTime() - t0) * 1000;	//no console here: the worker can outlive it at quit
}

static struct
{
	void *mutex, *thread;
	HANDLE wake;
	int gen, open, busy;	//busy: the generation a scan is under way for, else 0
	double next;
	unsigned int seed;
	insdirs_t dirs;		//INS_ModDirs, on the main thread
	char dirsfor[MAX_OSPATH];
	inshookset_t acc;
} inshookst;
static double INS_HookRand(double lo, double hi)
{	//under the mutex: when the next scan comes is not known in advance
	inshookst.seed = inshookst.seed * 1664525u + 1013904223u;
	return lo + (hi - lo) * ((inshookst.seed >> 8) / 16777216.0);
}
static void INS_HookMerge(const inshookset_t *r)
{
	inshookset_t *a = &inshookst.acc;
	int i, j, f = 0, u = 0;
	for (i = 0; i < r->nlines; i++)
		INS_HookInsert(a, r->lines[i], r->prio[i]);
	for (i = 0; i < r->nskips; i++)
	{
		for (j = 0; j < a->nskips && strcmp(a->skips[j], r->skips[i]); j++)
			;
		if (j == a->nskips && a->nskips < INS_HOOKSKIPS)
			memcpy(a->skips[a->nskips++], r->skips[i], sizeof(a->skips[0]));
	}
	for (i = 0; i < a->nlines; i++)
	{
		if (a->prio[i] == INS_HookPrio("noread"))
			u++;
		else
			f++;
	}
	a->found = max(max(a->found, r->found), f);	//distinct findings over the journal's scans, at least
	a->unread = max(max(a->unread, r->unread), u);
	a->codebytes = r->codebytes;
	a->funcs = r->funcs;
	a->slots = r->slots;
	a->regions = r->regions;
	a->kib = r->kib;
	a->unlisted = r->unlisted;
	a->dbg |= r->dbg;
	a->ms = r->ms;
	a->scans++;
}
#ifdef MULTITHREAD
static int INS_HookWorker(void *arg)
{
	inshook_t *h;
	qboolean due, open;
	int gen;
	double wait;
	//below the game's threads (Sys_CreateThread ignores its priority on Windows); not idle, which could
	//starve it while it holds a heap lock the game needs
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
	for (;;)
	{
		if (!(h = calloc(1, sizeof(*h))))
		{
			Sys_Sleep(1);
			continue;
		}
		Sys_LockMutex(inshookst.mutex);
		wait = bound(0, inshookst.next - Sys_DoubleTime(), 60);
		open = inshookst.open;
		due = open && wait <= 0;
		gen = inshookst.gen;
		if (due)
		{
			inshookst.next = Sys_DoubleTime() + INS_HookRand(10, 20);
			inshookst.busy = gen;
			h->dirs = inshookst.dirs;
		}
		Sys_UnlockMutex(inshookst.mutex);
		if (!due)
		{	//asleep until the next scan, or until a journal opens
			free(h);
			WaitForSingleObject(inshookst.wake, open ? (DWORD)(wait * 1000) + 1 : INFINITE);
			continue;
		}
		INS_HookScan(h);
		Sys_LockMutex(inshookst.mutex);
		inshookst.busy = 0;
		if (gen == inshookst.gen && inshookst.open)
			INS_HookMerge(&h->r);
		Sys_UnlockMutex(inshookst.mutex);
		free(h);
	}
	return 0;
}
#endif
//On the main thread: what the worker must not resolve itself.
static void INS_HookPrepare(void)
{
	HMODULE k32 = GetModuleHandleA("kernel32.dll");
	LARGE_INTEGER now;
	if (!insapi.GetModuleHandleExW && k32)
	{
		insapi.GetModuleHandleExW = (void*)GetProcAddress(k32, "GetModuleHandleExW");
		insapi.IsWow64Process2 = (void*)GetProcAddress(k32, "IsWow64Process2");
		insapi.OpenThread = (void*)GetProcAddress(k32, "OpenThread");
		insapi.CheckRemoteDebuggerPresent = (void*)GetProcAddress(k32, "CheckRemoteDebuggerPresent");
		QueryPerformanceCounter(&now);
		inshookst.seed = (unsigned int)now.QuadPart ^ GetCurrentProcessId();
	}
	if (strcmp(inshookst.dirsfor, com_gamepath))
	{	//once, and again if the game directory changes
		INS_ModDirs(&inshookst.dirs);
		Q_strncpyz(inshookst.dirsfor, com_gamepath, sizeof(inshookst.dirsfor));
	}
}
//As a journal opens: forget the last journal's scans; the first comes 0.5-2.5 s in, off the run's first
//frames, so a reset before then costs nothing.  True if a scan will be written.
qboolean INS_HookJournalBegin(void)
{
#ifdef MULTITHREAD
	if (!inshookst.mutex)
		inshookst.mutex = Sys_CreateMutex();
	if (!inshookst.wake)
		inshookst.wake = CreateEventA(NULL, FALSE, FALSE, NULL);
	if (inshookst.mutex && inshookst.wake)
	{
		Sys_LockMutex(inshookst.mutex);
		INS_HookPrepare();
		inshookst.gen++;
		inshookst.open = true;
		inshookst.next = Sys_DoubleTime() + INS_HookRand(0.5, 2.5);
		memset(&inshookst.acc, 0, sizeof(inshookst.acc));
		Sys_UnlockMutex(inshookst.mutex);
		if (!inshookst.thread)
			inshookst.thread = Sys_CreateThread("hookscan", INS_HookWorker, NULL, THREADP_IDLE, 0);
		SetEvent(inshookst.wake);
		return true;
	}
#endif
	INS_HookPrepare();
	memset(&inshookst.acc, 0, sizeof(inshookst.acc));
	inshookst.open = true;
	return true;
}
//As a journal closes: with line, the journal's scans as lines (a scan here if no worker ran one).
void INS_HookJournalEnd(void *ctx, void (*line)(void *ctx, const char *text))
{
	inshookset_t a;
	inshook_t *h;
	char text[INS_HOOKLINELEN + 2];
	int i, inflight;

	if (inshookst.mutex)
		Sys_LockMutex(inshookst.mutex);
	a = inshookst.acc;
	inflight = inshookst.busy && inshookst.busy == inshookst.gen;
	inshookst.open = false;
	if (inshookst.mutex)
		Sys_UnlockMutex(inshookst.mutex);
	if (!line)
		return;
	if ((!inshookst.mutex || !inshookst.thread) && !a.scans && (h = calloc(1, sizeof(*h))))
	{	//no worker: scan now, on this thread
		h->dirs = inshookst.dirs;
		INS_HookScan(h);
		inshookst.acc = a;
		INS_HookMerge(&h->r);
		a = inshookst.acc;
		free(h);
	}
	for (i = 0; i < a.nlines; i++)
	{
		Q_snprintfz(text, sizeof(text), "%s\n", a.lines[i]);
		line(ctx, text);
	}
	for (i = 0; i < a.nskips; i++)
	{
		Q_snprintfz(text, sizeof(text), "hookskip %s\n", a.skips[i]);
		line(ctx, text);
	}
	if (a.scans)
	{
		Con_DPrintf("hook scans: %i, the last %.1f ms\n", a.scans, a.ms);
		Q_snprintfz(text, sizeof(text), "hooks %i %i %i %i %i %i %i %i %i %i 0\n", a.found, a.codebytes, a.funcs, a.slots,
			a.regions, a.kib, a.dbg, a.scans, a.unread, a.unlisted);
	}
	else	//no scan finished while the journal was open; the last field: one was under way
		Q_snprintfz(text, sizeof(text), "hooks -1 -1 -1 -1 -1 -1 0 0 -1 -1 %i\n", inflight);
	line(ctx, text);
}
#else
qboolean INS_HookJournalBegin(void)
{
	return false;
}
void INS_HookJournalEnd(void *ctx, void (*line)(void *ctx, const char *text))
{
}
#endif
#endif
