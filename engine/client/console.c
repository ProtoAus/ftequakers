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
// console.c

#include "quakedef.h"
#include "shader.h"

console_t	*con_head;			// first console in the list
console_t	*con_curwindow;		// the (window) console that's currently got focus.
console_t	*con_current;		// points to whatever is the active console (the one that has focus ONLY when kdm_console)
console_t	*con_mouseover;		// points to whichever console's title is currently mouseovered, or null

console_t	*con_main;			// the default console that text will be thrown at. recreated as needed.
console_t	*con_chat;			// points to a chat console

#define Font_ScreenWidth() (vid.pixelwidth)

static int Con_DrawProgress(int left, int right, int y);
static int Con_DrawConsoleLines(console_t *con, conline_t *l, float displayscroll, int sx, int ex, int y, int top, int selactive, int selsx, int selex, int selsy, int seley, float lineagelimit);

#ifdef QTERM
#include <windows.h>
typedef struct qterm_s {
	console_t *console;
	qboolean running;
	HANDLE process;
	HANDLE pipein;
	HANDLE pipeout;

	HANDLE pipeinih;
	HANDLE pipeoutih;

	struct qterm_s *next;
} qterm_t;

qterm_t *qterms;
qterm_t *activeqterm;
#endif

//int 		con_linewidth;	// characters across screen
//int			con_totallines;		// total lines in console scrollback

static float		con_cursorspeed = 4;


static cvar_t		con_numnotifylines = CVAR("con_notifylines","4");		//max lines to show
static cvar_t		con_notifytime = CVAR("con_notifytime","3");		//seconds
static cvar_t		con_notifyfade = CVARD("con_notifyfade","1", "Seconds that closed-console notifications spend fading after con_notifytime expires.");
/*FTESurf Patch 242.  Two knobs the notify overlay never had.

  con_notifytime_error lets a failure stay on screen long enough to read while
  ordinary chatter still clears quickly.  0 means "same as con_notifytime", so
  the feature costs nothing to anyone who does not want it.

  con_notifystyle is the ORDER, which no notify surface in this engine has ever
  offered: every one of them draws oldest-first, so new lines arrive at the
  bottom and push older ones up and off.  1 puts the newest line at the top with
  older ones sliding down beneath it -- the newest is then always in the same
  place instead of moving as the block grows, which is what makes a burst of
  errors readable at a glance.  2 is the classic order anchored to the BOTTOM of
  the block, which is what CONF_NOTIFY_BOTTOM has always done for the chat
  console and the frag tracker and which nothing could reach from a config.*/
static cvar_t		con_notifytime_error = CVARD("con_notifytime_error","0", "Seconds an error or warning line stays on screen before fading, instead of con_notifytime. 0 uses con_notifytime.");
static cvar_t		con_notifystyle = CVARD("con_notifystyle","0", "Notification order for the main console. 0: oldest at the top, newest at the bottom (classic). 1: newest at the top, older below it. 2: newest at the bottom, older pushed up, anchored to the bottom of the block.");
static cvar_t		con_notify_x = CVAR("con_notify_x","0");
static cvar_t		con_notify_y = CVAR("con_notify_y","0");
static cvar_t		con_notify_w = CVAR("con_notify_w","1");
static cvar_t		con_centernotify = CVAR("con_centernotify", "0");
//FTESurf Patch 211: no longer static -- keys.c gates the dropdown's wheel on it.
//Same reason con_showcompletion was de-static'd below.
cvar_t				con_displaypossibilities = CVARD("con_displaypossibilities", "1", "Show the list of possible completions above the input line.\n0: never.\n1: after Tab, as one wrapped line of links.\n2: as you type, as a scrollable vertical dropdown.");
cvar_t				con_showcompletion = CVAR("con_showcompletion", "1");	//nettest: no longer static - keys.c gates right-arrow-accept on it
static cvar_t		con_maxlines = CVAR("con_maxlines", "1024");
cvar_t				cl_chatmode = CVARD("cl_chatmode", "2", "0(nq) - everything is assumed to be a console command. prefix with 'say', or just use a messagemode bind\n1(q3) - everything is assumed to be chat, unless its prefixed with a /\n2(qw) - anything explicitly recognised as a command will be used as a command, anything unrecognised will be a chat message.\n/ prefix is supported in all cases.\nctrl held when pressing enter always makes any implicit chat into team chat instead.");
static cvar_t		con_numnotifylines_chat = CVAR("con_numnotifylines_chat", "8");
static cvar_t		con_notifytime_chat = CVAR("con_notifytime_chat", "8");
cvar_t				con_separatechat = CVAR("con_separatechat", "0");
static cvar_t		con_timestamps = CVAR("con_timestamps", "0");
static cvar_t		con_timeformat = CVAR("con_timeformat", "(%H:%M:%S) ");
cvar_t				con_textsize = CVARD("con_textsize", "8", "Resize the console text to be a different height, scaled separately from the hud. The value is the height in (virtual) pixels.");
static cvar_t		con_savehistory = CVARD("con_savehistory", "1", "Write/update conhistory.txt");
//nettest: closing the console used to throw away where you were reading. It still has to DRAW the
//live tail while hidden (that surface doubles as the notify overlay under con_window 1), so the
//position is parked for the duration of the draw and restored immediately after.
static cvar_t		con_keepscroll = CVARD("con_keepscroll", "1", "Remember the console scrollback position when you close the console, so reopening it lands where you left off. 0 = always reopen at the live end.");

/*
FTESurf Patch 211: the windowed console's chrome.

Everything below replaces a bare 8, 16 or 24 that was typed into console.c AND
independently into keys.c -- the draw and the hit test.  They have to agree or
the title bar draws at one height and is grabbable at another, and the failure is
silent: a bar that simply ignores you.  So the two sizes get one speller each and
both files call it.  Same move FS_SaveSlotName, FS_RunPath and Zone_TrackSegs
were, each after a drift bug of exactly this shape.

CON_WNDBORDER stays 8 and stays a constant: it is the LEFT inset and the origin
mousecursor[] is measured from, and neither is a thing anyone asked to resize.

FTESurf Patch 213: the resize grips are no longer CON_WNDBORDER either. They are
con_gripsize, because "the bottom right corner is easier to grab" is a question
about a hit target and the inset is a question about layout, and folding the two
into one constant is what made the corner an 8x8 square in the first place.
*/
/*
EVERY DEFAULT BELOW IS THE PREVIOUS BEHAVIOUR, and that is not tidiness: this
engine is shared with quakers, and `con_window` defaults to 1 THERE TOO -- so
the windowed console is not FTESurf-only and a new default here would change a
game whose owner did not ask for it. FTESurf opts in from its own cfg/default.cfg.
*/
static void QDECL con_window_rect_cb(cvar_t *var, char *oldval);
static cvar_t		con_window_rect = CVARFCD("con_window_rect", "0 0 640 480", 0, con_window_rect_cb, "Where the console window sits: x y width height, in virtual pixels. Setting it moves the window immediately, so a value in a config applies on the launch it is read.");
static cvar_t		con_window_titlepad = CVARD("con_window_titlepad", "0", "Padding above and below the console window's title text, in virtual pixels. The title bar is the drag handle, so this is how much bigger than the text it is.");
static cvar_t		con_scrollwidth = CVARD("con_scrollwidth", "8", "Width of the console window's scrollbar, in virtual pixels.");
static cvar_t		con_colour_back = CVARD("con_colour_back", "0 0.05 0.1", "Console window background colour, \"r g b\" from 0 to 1. The default is the stock navy.");
static cvar_t		con_colour_accent = CVARD("con_colour_accent", "0.55 0.7 0.95", "Console window accent colour, \"r g b\" from 0 to 1: the scrollbar thumb, its track, and the resize-edge highlights are all derived from this. The default is the stock thumb colour.");
static cvar_t		con_completionrows = CVARD("con_completionrows", "12", "How many entries of the tab-completion dropdown are shown at once. The rest scroll.");
static cvar_t		con_completiondown = CVARD("con_completiondown", "0", "Where the tab-completion list is drawn.\n0: above the input line, a drop-UP (default).\n1: below it, inside the console. The console is drawn from the bottom up, so the list claims the bottom band and the input line moves up above it -- the field shifts as the match count changes.\n2: below it, as a floating popup drawn outside the console after everything else. The input line never moves. It flips above the field when there is no room below.");
static cvar_t		con_gripsize = CVARD("con_gripsize", "8", "Width of the console window's right resize grip and height of its bottom one, in virtual pixels. The bottom-right corner is both at once, so this squares: 16 makes that corner four times the target 8 does. The left grip is deliberately not affected.");

/*
FTESurf Patch 211: a pastel dark-mode text palette.

consolecolours[] (common/common.c) is the CGA RGBI table -- fully saturated
primaries designed for a black CRT.  `^4` blue is {0.33,0.33,1} and `^1` red is
{1,0.33,0.33}: on a dark ground they glare, and their dark halves (indices 0-7,
reached by ^& codes and by CON_HALFALPHA) are close to unreadable.

WHAT THIS TOUCHES, because it is wider than the console and must be said: the
table is read by gl_font.c for EVERY ^N glyph the engine draws -- the console,
chat, Con_Printf, Draw_FunString -- and by m_items.c for the built-in menus.  So
the timer's ^3practice^7 and every console line change with it.  That is what a
dark mode is; `con_palette 0` puts all of it back in one command, and nothing
drawn from an explicit colour vector (most of the FTESurf HUD) is affected at all.

Index 0 is deliberately left pure black: it is used as a BACKGROUND colour, not
as ink, and lifting it would put a grey box behind ^0 text.
*/
static const consolecolours_t con_palette_cga[MAXCONCOLOURS] =
{	//the stock table, kept here so con_palette 0 restores it exactly rather than
	//requiring a restart
	{0,    0,    0   }, {0,    0,    0.67}, {0,    0.67, 0   }, {0,    0.67, 0.67},
	{0.67, 0,    0   }, {0.67, 0,    0.67}, {0.67, 0.33, 0   }, {0.67, 0.67, 0.67},
	{0.33, 0.33, 0.33}, {0.33, 0.33, 1   }, {0.33, 1,    0.33}, {0.33, 1,    1   },
	{1,    0.33, 0.33}, {1,    0.33, 1   }, {1,    1,    0.33}, {1,    1,    1   }
};
static const consolecolours_t con_palette_pastel[MAXCONCOLOURS] =
{
	{0.00, 0.00, 0.00},	// 0  black -- a background, left alone
	{0.24, 0.31, 0.48},	// 1  dark blue
	{0.27, 0.42, 0.31},	// 2  dark green
	{0.25, 0.42, 0.45},	// 3  dark cyan
	{0.48, 0.28, 0.30},	// 4  dark red
	{0.42, 0.29, 0.47},	// 5  dark magenta
	{0.46, 0.37, 0.26},	// 6  brown
	{0.55, 0.57, 0.62},	// 7  grey        -- this is ^9
	{0.36, 0.38, 0.43},	// 8  dark grey
	{0.55, 0.70, 0.95},	// 9  blue        -- ^4
	{0.62, 0.84, 0.62},	// 10 green       -- ^2
	{0.58, 0.84, 0.87},	// 11 cyan        -- ^5
	{0.94, 0.60, 0.60},	// 12 red         -- ^1
	{0.87, 0.66, 0.92},	// 13 magenta     -- ^6
	{0.94, 0.85, 0.60},	// 14 yellow      -- ^3
	{0.88, 0.89, 0.92}	// 15 white       -- ^7, softened off pure white
};
static void QDECL con_palette_cb(cvar_t *var, char *oldval)
{
	memcpy(consolecolours, var->ival ? con_palette_pastel : con_palette_cga, sizeof(consolecolours));
}
static cvar_t		con_palette = CVARFCD("con_palette", "0", CVAR_ARCHIVE, con_palette_cb, "Colour table for ^-codes, everywhere the engine draws them.\n0: the stock CGA palette (default).\n1: a desaturated pastel set intended for a dark background.");

//CON_WNDBORDER and these two accessors are declared in common/console.h, because
//keys.c hit-tests against exactly the numbers this file draws with.

//FTESurf Patch 211: the title bar's height. Derived from the font it is drawn in
//(font_console -- con_textfont at con_textsize) rather than typed, so it stays
//right at any con_textsize. Font_CharVHeight takes its font explicitly, unlike
//Font_CharHeight, whose global is not bound this early in Con_DrawConsole.
int Con_WindowTitleHeight(void)
{
	int h = Font_CharVHeight(font_console) + 2*(int)con_window_titlepad.value;
	if (h < 8)
		h = 8;	//the stock height is the floor; below it the X is unclickable
	return h;
}

//FTESurf Patch 211: the scrollbar strip on the right, inside the resize grip.
int Con_WindowScrollWidth(void)
{
	int w = con_scrollwidth.ival;
	if (w < 4)
		w = 4;
	if (w > 64)
		w = 64;
	return w;
}

/*
FTESurf Patch 213: the right and bottom resize grips.

The scrollbar moves inward with this rather than being eaten by it. The two are
adjacent -- grip at the window's edge, scrollbar just inside it -- and today they
do not overlap only because both happen to be 8. Growing the grip on its own
would have taken half of the scrollbar's clickable width, which is the thing the
LAST build widened on request; so the scrollbar's x, the text width and the text
region's bottom are all derived from this, and the invariant "the grip is outside
everything else" is what is actually being kept.

The floor is 4 rather than 0: a zero-width grip is a window that cannot be
resized at all, with nothing on screen to say why.
*/
int Con_WindowGripSize(void)
{
	int g = con_gripsize.ival;
	if (g < 4)
		g = 4;
	if (g > 64)
		g = 64;
	return g;
}

/*
FTESurf Patch 211: the completion dropdown is a CHAIN of conline_t, one per row,
linked by ->older -- the same structure Con_Footerf builds for a multi-line
footer, and the one Con_DrawConsoleLines already knows how to walk.

Both helpers exist because the chain has two owners: the draw rebuilds it every
frame, and Con_Destroy/Con_Finit have to drop it. Before this it was a single
allocation and a bare Z_Free; freeing only the head now would leak the rest.
*/
extern int con_commandmatch;
int con_completionscroll;	//first entry shown in the dropdown

/*
FTESurf Patch 226: con_completiondown 2 -- the list as a FLOATING popup.

    "the real dream is to have it part of an actual drop down, separate from the
     console, so that the console text enter bar never has to shift"

Patch 215 could not do that and said why: Con_DrawInput is bottom-anchored, so the
only way to put the list BELOW the input line was to give it the bottom band and
move the input row up above it -- and a console window is scissored to its own
rect (Con_DrawConsole's BE_Scissor around Con_DrawOneConsole), so a list hanging
below the window would be clipped rather than drawn over the game.

Both halves of that are still true.  What was missed is that the scissor is
released again a few lines later, and the console has nothing left to draw by
then: a popup emitted AFTER the console's own draw is unclipped and on top, which
is what every real combobox does.  So the list stops being part of the console's
vertical budget entirely.  Con_DrawInput records where the input line ended up
and how many rows are waiting; Con_DrawCompletionPopup paints them.

The input row's y is then fixed at exactly the one place that has always set it
(y -= Font_CharHeight() below), and no longer depends on the match count.
*/
static console_t	*con_popupcon;		//whose list is pending this frame. NULL = none.
static int			con_popuprows;		//conline_t nodes in the chain, the ordinal row included
static int			con_popupl;			//the input line's left, right and BOTTOM, in physical
static int			con_popupr;			//font space -- the same space Con_DrawConsoleLines and
static int			con_popupb;			//Font_BeginString hand back.
/*The window scissor to put back afterwards. Con_DrawCompletionPopup is called
  from inside Con_DrawOneConsole -- see the essay on it for why it cannot simply
  be called at the end of Con_DrawConsole -- so it has to lift and restore the
  clip itself. BE_Scissor is a flat set with no stack, hence keeping a copy.*/
static srect_t		con_popupclip;
static qboolean		con_popupclipped;
/*
Where the popup actually painted, in VIRTUAL pixels, so the con_mouseover test
can route a click that lands on it.

Deliberately NOT cleared per frame with the rest: that test runs at the top of
Con_DrawConsole, before this frame's Con_DrawInput has recorded anything, so it
is always answering with the last frame that painted. A mouse does not teleport,
and the alternative is laying the popup out twice per frame to ask where it will
be. Cleared by Con_DrawCompletionPopup itself on the first frame it declines.
*/
static console_t	*con_popupshown;
static float		con_popuphit[4];	//x, y, w, h

static void Con_FreeCompletion(console_t *con)
{
	conline_t *l;
	qboolean dropsselection = false;

	/*These rows are rebuilt every frame. A drag-selection can point at one of
	  them, so invalidate that selection before its node is freed rather than
	  leaving Con_CopyConsole/the next draw with a dangling line pointer.*/
	for (l = con->completionline; l; l = l->older)
	{
		if (con->selstartline == l || con->selendline == l)
			dropsselection = true;
		if (con->userline == l)
			con->userline = NULL;
		if (con->highlightline == l)
			con->highlightline = NULL;
	}
	if (dropsselection)
	{
		con->selstartline = con->selendline = NULL;
		con->flags &= ~(CONF_KEEPSELECTION|CONF_BACKSELECTION);
	}

	while (con->completionline)
	{
		l = con->completionline;
		con->completionline = l->older;
		Z_Free(l);
	}
}

/*Make arbitrary cvar text safe to place inside a parsed console fun-string.
  Carets introduce native markup and &c can introduce legacy colour markup, so
  both are encoded without changing what is displayed. Controls and quoting are
  made visible so one value cannot manufacture extra rows or ambiguous quotes.*/
const char *Con_EscapeConsoleMarkup(const char *text, char *out, size_t outsize)
{
	char *dst = out;
	char *last;

	if (!outsize)
		return out;
	last = out + outsize - 1;
	while (*text && dst < last)
	{
		const char *escaped = NULL;
		char control[5];
		unsigned char ch = (unsigned char)*text++;

		switch (ch)
		{
		case '^':  escaped = "^^"; break;
		case '&':  escaped = "^U0026"; break;
		case '\\': escaped = "\\\\"; break;
		case '"':  escaped = "\\\""; break;
		case '\n': escaped = "\\n"; break;
		case '\r': escaped = "\\r"; break;
		case '\t': escaped = "\\t"; break;
		default:
			if (ch < 32 || ch == 127)
			{
				Q_snprintfz(control, sizeof(control), "\\x%02x", ch);
				escaped = control;
			}
			else
				*dst++ = ch;
			break;
		}

		if (escaped)
		{
			size_t escapedlen = strlen(escaped);
			/*Never truncate inside ^U0026: the fun-string parser quite reasonably
			  expects all six bytes once it sees ^U, and a partial escape at the
			  buffer edge would make it inspect beyond this string's terminator.*/
			if ((size_t)(last-dst) < escapedlen)
				break;
			memcpy(dst, escaped, escapedlen);
			dst += escapedlen;
		}
	}
	*dst = 0;
	return out;
}

//push a row onto the chain. The chain head is drawn at the BOTTOM, so the LAST
//row pushed is the one nearest the bottom -- see Con_BuildCompletion.
static void Con_PushCompletion(console_t *con, const char *text)
{
	conchar_t marked[512], *markedend;
	conline_t *nl;
	size_t len;

	/* Completion rows are already ephemeral console text.  Parse colour markup
	 * now instead of preserving it, otherwise strings such as "^9...^7" are
	 * rendered literally in the dropdown.  FORCEUTF8 retains the intended text
	 * decoding while still consuming markup and preserving clickable links. */
	markedend = COM_ParseFunString(COLOR_GREEN<<CON_FGSHIFT, text, marked, sizeof(marked), PFS_FORCEUTF8);
	len = markedend - marked;

	nl = Z_Malloc(sizeof(*nl) + len*sizeof(conchar_t));
	nl->length = len;
	nl->older = con->completionline;
	if (nl->older)
		nl->older->newer = nl;
	memcpy((conchar_t*)(nl+1), marked, len*sizeof(conchar_t));
	con->completionline = nl;
}

/*
FTESurf Patch 215: build the dropdown's rows.

Split out of Con_DrawInput's tail so that con_completiondown can draw the list
BEFORE the input line -- i.e. in the band the input line then sits above --
without a second copy of this loop drifting away from the first.

THE ROW ORDER WAS BACKWARDS, and that is the reported bug:

	"the drop up up/down arrow keys moves the selection highlight in the wrong
	 direction"

Con_DrawConsoleLines draws the line it is HANDED at the y it is given and then
walks ->older UPWARD (it does `y -= Font_CharHeight()` before each row), so the
chain HEAD is the BOTTOM row and the deepest link is the TOP one.  Con_PushCompletion
pushes onto the head.  Patch 211 therefore pushed backwards -- `for (i = last-1;
i >= first; i--)` -- under a comment claiming that made entry[first] "deepest,
i.e. at the top".  It did the exact opposite: entry[first] was pushed LAST, so it
became the head, so it was drawn at the BOTTOM, and the list read in DESCENDING
index order.

Up decrements con_commandmatch (Key_CompletionNav), so Up walked toward index 1,
which was at the bottom of the screen.  Up moved the highlight DOWN.  Pushing
FORWARDS is the whole fix, and it is right for both layouts.

`maxrows` is the caller's height budget, and it is new.  Patch 211 passed top=0
to Con_DrawConsoleLines, so con_completionrows 64 in a short window drew the list
straight out through the top of the console.  That was survivable while the list
sat above the input line; with con_completiondown it would also shove the input
line off the top, so the row count is now bounded by the space that actually
exists.

It is a budget in ENTRIES against a limit in screen rows, and those are not the
same thing: Con_DrawConsoleLines runs each conline_t through Font_LineBreaks and
a name too long for the console wraps onto several rows.  The budget is therefore
advisory, and the callers do not rely on it alone -- both pass a real `top` to
Con_DrawConsoleLines, which stops rather than drawing past it.

Returns the number of rows pushed; 0 means there is nothing to draw.
*/
static int Con_BuildCompletion(console_t *con, const char *text, int maxrows)
{
	cmd_completion_t *c;
	int cmdstart = (text[0] == '/')?1:0;
	int rows = con_completionrows.ival;
	int first, last, i, n = 0;
	size_t total;
	qboolean showstatus = (maxrows != 1);

	Con_FreeCompletion(con);

	c = Cmd_Complete(text+cmdstart, true);
	if (!c || !c->num)
		return 0;

	if (rows < 1)
		rows = 1;
	if (rows > 64)
		rows = 64;
	/*The ordinal is a real row too. Keep it inside the caller's height budget.*/
	if (maxrows > 1 && rows > maxrows-1)
		rows = maxrows-1;
	else if (maxrows == 1 && rows > 1)
		rows = 1;

	/*
	Scroll the window to the highlight, THEN bound the window to the list.

	Patch 211 did those two in the opposite order, and the order is the whole bug:
	the highlight clamp does not know how long the list is, so it could leave
	`first` past the end.  That needs con_commandmatch > c->num, which is reachable
	because the number is set against a slightly different string from the one the
	draw completes -- CompleteCommand and Key_UpdateCompletionDesc strip leading
	whitespace and a leading '\\' (keys.c), while the draw strips only a leading
	'/'.  So type a space and then a command, and with num 0 and match 13 you get
	first 1, last 0: the row loop runs zero times and `extra` comes out
	0 + 0 - (0-1) = 1, so the dropdown draws as a single phantom "1 more" with no
	rows and no highlight.  Stable, every frame, because both clamps re-fight it.

	Bounding to the list last makes 0 <= first <= max(0, num-rows) unconditionally,
	so last > first whenever there is anything to show.
	*/
	if (con_commandmatch)
	{
		if (con_commandmatch-1 < con_completionscroll)
			con_completionscroll = con_commandmatch-1;
		if (con_commandmatch-1 >= con_completionscroll+rows)
			con_completionscroll = con_commandmatch-rows;
	}
	if (con_completionscroll > (int)c->num - rows)
		con_completionscroll = (int)c->num - rows;
	if (con_completionscroll < 0)
		con_completionscroll = 0;

	first = con_completionscroll;
	last = first + rows;
	if (last > (int)c->num)
		last = c->num;
	total = c->num + c->extra;

	for (i = first; i < last; i++, n++)
	{
		const char *choice;
		cvar_t *var;
		//note: if cl_chatmode is 0, then we shouldn't show the leading /, however that is how the console link stuff recognises it as command text, so we always display it.
		int col = (con_commandmatch == i+1)?3:2;
		choice = c->completions[i].repl ? c->completions[i].repl : c->completions[i].text;
		var = Cvar_FindVar(choice);
		if (var)
		{
			const char *def = var->defaultstr ? var->defaultstr : (var->enginevalue ? var->enginevalue : var->string);
			const char *cur = var->latched_string ? var->latched_string : var->string;
			char safedef[1024], safecur[1024];
			if (var->flags & CVAR_NOUNSAFEEXPAND)
			{
				/*Never turn completion into a password/key disclosure surface.*/
				if (strcmp(def, cur))
					Con_PushCompletion(con, va("^[^%i/%s^]  ^3<value hidden>^7  ^9(default hidden)^7", col, c->completions[i].text));
				else
					Con_PushCompletion(con, va("^[^%i/%s^]  ^9<value hidden> (default)^7", col, c->completions[i].text));
			}
			else
			{
				Con_EscapeConsoleMarkup(def, safedef, sizeof(safedef));
				Con_EscapeConsoleMarkup(cur, safecur, sizeof(safecur));
				if (strcmp(def, cur))
					Con_PushCompletion(con, va("^[^%i/%s^]  ^3\"%s\"^7  ^9(default \"%s\")^7", col, c->completions[i].text, safecur, safedef));
				else
					Con_PushCompletion(con, va("^[^%i/%s^]  ^9\"%s\" (default)^7", col, c->completions[i].text, safedef));
			}
		}
		else
			Con_PushCompletion(con, va("^[^%i/%s^]", col, c->completions[i].text));
	}

	/*
	Always put one status row at the list's far end.  "N more" mixed viewport
	overflow with Cmd_Complete's hard-cap overflow, so a short visible list could
	report a startling number with no indication of which row the arrows had
	selected.  The ordinal is stable as the viewport scrolls; before navigation,
	the same slot reports the total without pretending that anything is selected.
	*/
	if (showstatus)
	{
		if (con_commandmatch > 0 && con_commandmatch <= (int)c->num)
		{
			if (c->extra)
				Con_PushCompletion(con, va("^9%i of %u selectable (%u matches)^7", con_commandmatch, (unsigned)c->num, (unsigned)total)), n++;
			else
				Con_PushCompletion(con, va("^9%i of %u^7", con_commandmatch, (unsigned)total)), n++;
		}
		else if (c->extra)
			Con_PushCompletion(con, va("^9%u selectable (%u matches)^7", (unsigned)c->num, (unsigned)total)), n++;
		else
			Con_PushCompletion(con, va("^9%u match%s^7", (unsigned)total, total == 1 ? "" : "es")), n++;
	}

	return n;
}

//FTESurf Patch 211: "r g b" -> three floats, falling back to the given default
//rather than to black, because a typo'd colour cvar that blanks the console is a
//trap you cannot type your way out of.
static void Con_ParseColour(cvar_t *var, float *out, float dr, float dg, float db)
{
	if (sscanf(var->string, " %f %f %f", out+0, out+1, out+2) != 3)
	{
		out[0] = dr;
		out[1] = dg;
		out[2] = db;
	}
}

extern cvar_t log_developer;

void con_window_cb(cvar_t *var, char *oldval)
{
	if (!con_main)
		return;	//doesn't matter right now.

	if (var->ival)
	{
		/*
		FTESurf: a floating console and its in-game notifications are two different
		surfaces.  Keep CONF_NOTIFY so a closed window uses Con_DrawNotifyOne at the
		top-left instead of fading the entire window at its last dragged position.
		Con_DrawNotify suppresses this flag again while the main window is open.
		*/
		con_main->flags |= CONF_NOTIFY;
		if (!(con_main->flags & CONF_ISWINDOW))
		{
			con_main->flags |= CONF_ISWINDOW;
			if (con_current == con_main)
				Con_SetActive(con_main);
		}
	}
	else
	{
		con_main->flags |= CONF_NOTIFY;
		if (con_main->flags & CONF_ISWINDOW)
		{
			con_main->flags &= ~CONF_ISWINDOW;
			if (con_curwindow == con_main)
				Con_SetActive(con_main);
		}
	}
}
static cvar_t con_window = CVARCD("con_window", "0", con_window_cb, "States whether the console should be a floating window as in source engine games, or a top-of-the-screen-only thing.");

/*FTESurf Patch 242: 24 -> 64.  "I want to see the full list of errors if they
  appear and I don't have the console open" -- a failing map load prints well
  over twenty lines, and the old ceiling silently clipped con_notifylines to 24
  with no indication that it had.  It sizes three arrays local to
  Con_DrawNotifyOne, so the cost is about 1.3KB of stack in one function.*/
#define	NUM_CON_TIMES 64

qboolean	con_initialized;

/*makes sure the console object works*/
void Con_Finit (console_t *con)
{
	if (con->current == NULL)
	{
		con->oldest = con->current = Z_Malloc(sizeof(conline_t));
		con->linecount = 0;
	}
	if (con->display == NULL)
		con->display = con->current;

	con->selstartline = NULL;
	con->selendline = NULL;

	con->defaultcharbits = CON_WHITEMASK;
	con->parseflags = 0;
}

/*returns a bitmask:
1: currently active
2: has text that has not been seen yet
*/
int Con_IsActive (console_t *con)
{
	return (con == con_current) | (con->unseentext*2);
}
/*kills a console_t object. will never destroy the main console (which will only be cleared)*/
void Con_Destroy (console_t *con)
{
	shader_t *shader;
	console_t **link;
	conline_t *t;

	if (con->close)
	{
		con->close(con, true);
		con->close = NULL;
	}

	/*purge the lines from the console*/
	while (con->current)
	{
		t = con->current;
		con->current = t->older;
		Z_Free(t);
	}
	con->display = con->current = con->oldest = NULL;

	Con_Footerf(con, false, "");
	Con_FreeCompletion(con);	//FTESurf Patch 211: it is a chain now, not one line

	for (link = &con_head; *link; link = &(*link)->next)
	{
		if (*link == con)
		{
			(*link) = con->next;
			break;
		}
	}

	shader = con->backshader;

	BZ_Free(con);

	//make sure any special references are fixed up now that its gone
	if (con_mouseover == con)
		con_mouseover = NULL;
	if (con_current == con)
		con_current = con_head;

	if (con_curwindow == con)
	{
		for (con_curwindow = con_head; con_curwindow; con_curwindow = con_curwindow->next)
		{
			if (con_curwindow->flags & CONF_ISWINDOW)
				break;
		}
		if (!con_curwindow)
			Key_Dest_Remove(kdm_cwindows);
	}
	con_mouseover = NULL;

	if (shader)
		R_UnloadShader(shader);
}

/*just purges the background images for various consoles on restart/shutdown*/
void Con_FlushBackgrounds(void)
{
	console_t *con;
	//fixme: we really need to handle videomaps differently here, for vid_restarts.
	for (con = con_head; con; con = con->next)
	{
		if (con->backshader)
			R_UnloadShader(con->backshader);
		con->backshader = NULL;
	}
}

/*obtains a console_t without creating*/
console_t *Con_FindConsole(const char *name)
{
	console_t *con;
	if (!strcmp(name, "current") && con_current)
		return con_current;
	if (!strcmp(name, "head") && con_current)
		return con_head;
	for (con = con_head; con; con = con->next)
	{
		if (!strcmp(con->name, name))
			return con;
	}
	return NULL;
}
/*creates a potentially duplicate console_t - please use Con_FindConsole first, as its confusing otherwise*/
console_t *Con_Create(const char *name, unsigned int flags)
{
	console_t *con, *p;
	if (!name)
	{
		static unsigned long seq;
		name = va("c%lu", seq++);
	}
	if (!strcmp(name, "current"))
		return NULL;
	if (!strcmp(name, "head"))
		return NULL;
	con = Z_Malloc(sizeof(console_t));
	Q_strncpyz(con->name, name, sizeof(con->name));
	Q_strncpyz(con->title, name, sizeof(con->title));
	Q_strncpyz(con->prompt, "]", sizeof(con->prompt));

	con->flags = flags;
	Con_Finit(con);

	//insert at end. make it active if you must.
	if (!con_head)
		con_head = con;
	else
	{
		for (p = con_head; p->next; p = p->next)
			;
		p->next = con;
	}

	return con;
}

static qboolean Con_Main_BlockClose(console_t *con, qboolean force)
{
	if (!force)
	{	//trying to close it just hides it (this is to avoid it getting cleared).
		if (con_curwindow == con)
			Key_Dest_Remove(kdm_cwindows);
		return false;
	}
	con_main = NULL;	//its forced to die. and don't forget it.
	return true;
}
/*
FTESurf Patch 213: ONE place that turns con_window_rect into a window rect.

The NULL guard is not defensive padding, it is the bug this patch fixes: .string
is NULL until Cvar_Register runs, and Con_GetMain is reached before that (and can
be reached earlier still, by any Con_Printf during startup). See the essay in
Con_GetMain for what that cost.
*/
static void Con_ApplyWindowRect(console_t *w)
{
	float x, y, cw, ch;
	if (!w || !con_window_rect.string)
		return;
	if (sscanf(con_window_rect.string, " %f %f %f %f", &x, &y, &cw, &ch) != 4)
		return;
	if (cw < 64 || ch < 64)
		return;		//Con_DrawConsole clamps a window bigger than the screen, but a
					//zero-sized one never becomes visible enough to fix by hand
	w->wnd_x = x;
	w->wnd_y = y;
	w->wnd_w = cw;
	w->wnd_h = ch;
}
static void QDECL con_window_rect_cb(cvar_t *var, char *oldval)
{	//a value arriving with the config reaches a window that already exists
	Con_ApplyWindowRect(con_main);
}

console_t *Con_GetMain(void)
{
	if (!con_main)
	{
		con_main = Con_Create("", 0);

		con_main->linebuffered = Con_ExecuteLine;
		con_main->commandcompletion = true;
		/*
		FTESurf Patch 211: where the window first appears, from a cvar.

		Was 640x480 at 0,0 -- pinned to the top-left corner with no gap.  A cvar
		rather than four new constants because nothing persists wnd_*: the geometry
		resets on every launch, so re-tuning it must not mean re-tuning the engine.

		PATCH 213 -- THIS WAS BROKEN, AND IT WAS BROKEN IN THE DIRECTION THAT LOOKS
		LIKE IT WORKS.  Patch 211's comment here claimed an unregistered cvar_t
		"still carries its compile-time default" in .string.  It does not: cvar.h's
		CVARAFCD initialiser sets .string to NULL and puts the default in
		.enginevalue, and Con_Init called Con_GetMain THIRTY LINES BEFORE it
		registered this cvar.  So the sscanf ran on a null pointer, failed (or
		worse), and the hardcoded fallback below it won -- which happened to be
		exactly the geometry FTESurf wanted, so the feature looked correct.

		Two things were actually wrong.  quakers, which never sets this cvar and
		whose engine default is "0 0 640 480" precisely so its console does not
		move, got the 64,64 960x640 window anyway -- the exact bisection failure the
		Build 27 regression existed to catch, and it slipped through because that
		run checked the cvar's VALUE and not the window's position.  And FTESurf's
		own default.cfg value never reached here either, since cfg/default.cfg is
		exec'd long after Con_Init; it only agreed by coincidence.

		So: registration moved above this call (Con_Init), the parse is guarded, the
		fallback is the ENGINE default rather than FTESurf's numbers, and a callback
		below applies a later `set` -- which is the only way a value that arrives
		with the config can reach a window that already exists.
		*/
		con_main->wnd_x = 0;
		con_main->wnd_y = 0;
		con_main->wnd_w = 640;
		con_main->wnd_h = 480;
		Con_ApplyWindowRect(con_main);
		con_main->close = Con_Main_BlockClose;
		//FTESurf Patch 211: this string is the window's title bar. It said "MAIN".
		Q_strncpyz(con_main->title, "Console", sizeof(con_main->title));
		Q_strncpyz(con_main->prompt, "]", sizeof(con_main->prompt));

		Cvar_ForceCallback(&con_window);
	}
	return con_main;
}
/*sets a console as the active one*/
void Con_SetActive (console_t *con)
{
	if (con->flags & CONF_ISWINDOW)
	{
		console_t *prev;
		Key_Dest_Add(kdm_cwindows);
		Key_Dest_Remove(kdm_console);

		if (con_curwindow == con)
			return;

		for (prev = con_head; prev; prev = prev->next)
		{
			if (prev->next == con)
			{
				prev->next = con->next;
				while(prev->next)
				{
					prev = prev->next;
				}
				prev->next = con;
				con->next = NULL;
				break;
			}
		}
		con_curwindow = con;
	}
	else
	{
		if (con_curwindow == con)
			con_curwindow = NULL;
		Key_Dest_Add(kdm_console);
		Key_Dest_Remove(kdm_cwindows);
		con_current = con;
	}

	Con_Footerf(con, false, "");
	con->buttonsdown = CB_NONE;
}
/*for enumerating consoles*/
qboolean Con_NameForNum(int num, char *buffer, int buffersize)
{
	console_t *con;
	for (con = con_head; con; con = con->next, num--)
	{
		if (num <= 0)
		{
			Q_strncpyz(buffer, con->name, buffersize);
			return true;
		}
	}
	if (buffersize>0)
		*buffer = '\0';
	return false;
}

#ifdef QTERM
void QT_Kill(qterm_t *qt, qboolean killconsole)
{
	qterm_t **link;
	qt->console->close = NULL;
	qt->console->userdata = NULL;
	qt->console->redirect = NULL;
	if (killconsole)
		Con_Destroy(qt->console);

	//yes this loop will crash if you're not careful. it makes it easier to debug.
	for (link = &qterms; ; link = &(*link)->next)
	{
		if (*link == qt)
		{
			*link = qt->next;
			break;
		}
	}

	CloseHandle(qt->pipein);
	CloseHandle(qt->pipeout);

	CloseHandle(qt->pipeinih);
	CloseHandle(qt->pipeoutih);
	CloseHandle(qt->process);

	Z_Free(qt);
}
void QT_Update(void)
{
	char buffer[2048];
	DWORD ret;
	qterm_t *qt, *n;
	for (qt = qterms; qt; )
	{
		if (qt->running)
		{
			if (WaitForSingleObject(qt->process, 0) == WAIT_TIMEOUT)
			{
				if ((ret=GetFileSize(qt->pipeout, NULL)))
				{
					if (ret!=INVALID_FILE_SIZE)
					{
						ReadFile(qt->pipeout, buffer, sizeof(buffer)-32, &ret, NULL);
						buffer[ret] = '\0';
						Con_PrintCon(qt->console, buffer, PFS_NOMARKUP);
					}
				}
			}
			else
			{
				Con_PrintCon(qt->console, "Process ended\n", PFS_NOMARKUP);
				qt->running = false;
			}
		}

		n = qt->next;
		if (!qt->running)
		{
			if (!Con_IsActive(qt->console))
				QT_Kill(qt, true);
		}
		qt = n;
	}
}

qboolean QT_KeyPress(console_t *con, unsigned int unicode, int key)
{
	qbyte k[2];
	qterm_t *qt = con->userdata;
	DWORD send = key;	//get around a gcc warning


	k[0] = key;
	k[1] = '\0';

	if (qt->running)
	{
		if (*k == '\r')
		{
//					*k = '\r';
//					WriteFile(qt->pipein, k, 1, &key, NULL);
//					Con_PrintCon(k, &qt->console, PFS_NOMARKUP);
			*k = '\n';
		}
//		if (GetFileSize(qt->pipein, NULL)<512)
		{
			WriteFile(qt->pipein, k, 1, &send, NULL);
			Con_PrintCon(qt->console, k, PFS_NOMARKUP);
		}
	}
	return true;
}

qboolean	QT_Close(struct console_s *con, qboolean force)
{
	qterm_t *qt = con->userdata;
	QT_Kill(qt, false);

	return true;
}

void QT_Create(char *command)
{
	HANDLE StdIn[2];
	HANDLE StdOut[2];
	qterm_t *qt;
	SECURITY_ATTRIBUTES sa;
	STARTUPINFO SUInf;
	PROCESS_INFORMATION ProcInfo;

	int ret;

	qt = Z_Malloc(sizeof(*qt));

	memset(&sa,0,sizeof(sa));
	sa.nLength=sizeof(sa);
	sa.bInheritHandle=true;

	CreatePipe(&StdOut[0], &StdOut[1], &sa, 1024);
	CreatePipe(&StdIn[1], &StdIn[0], &sa, 1024);

	memset(&SUInf, 0, sizeof(SUInf));
	SUInf.cb = sizeof(SUInf);
	SUInf.dwFlags = STARTF_USESTDHANDLES;
/*
	qt->pipeout		= StdOut[0];
	qt->pipein		= StdIn[0];
*/
	qt->pipeoutih	= StdOut[1];
	qt->pipeinih	= StdIn[1];

	if (!DuplicateHandle(GetCurrentProcess(), StdIn[0],
						GetCurrentProcess(), &qt->pipein, 0,
						FALSE,                  // not inherited
						DUPLICATE_SAME_ACCESS))
		qt->pipein = StdIn[0];
	else
		CloseHandle(StdIn[0]);
	if (!DuplicateHandle(GetCurrentProcess(), StdOut[0],
						GetCurrentProcess(), &qt->pipeout, 0,
						FALSE,                  // not inherited
						DUPLICATE_SAME_ACCESS))
		qt->pipeout = StdOut[0];
	else
		CloseHandle(StdOut[0]);

	SUInf.hStdInput		= qt->pipeinih;
	SUInf.hStdOutput	= qt->pipeoutih;
	SUInf.hStdError		= qt->pipeoutih;	//we don't want to have to bother working out which one was written to first.

	if (!SetStdHandle(STD_OUTPUT_HANDLE, SUInf.hStdOutput))
		Con_Printf("Windows sucks\n");
	if (!SetStdHandle(STD_ERROR_HANDLE, SUInf.hStdError))
		Con_Printf("Windows sucks\n");
	if (!SetStdHandle(STD_INPUT_HANDLE, SUInf.hStdInput))
		Con_Printf("Windows sucks\n");

	printf("Started app\n");
	ret = CreateProcess(NULL, command, NULL, NULL, true, CREATE_NO_WINDOW, NULL, NULL, &SUInf, &ProcInfo);

	qt->process = ProcInfo.hProcess;
	CloseHandle(ProcInfo.hThread);

	qt->running = true;

	qt->console = Con_Create("QTerm", 0);
	qt->console->redirect = QT_KeyPress;
	qt->console->close = QT_Close;
	qt->console->userdata = qt;
	Con_PrintCon(qt->console, "Started Process\n", PFS_NOMARKUP);
	Con_SetActive(qt->console);

	qt->next = qterms;
	qterms = activeqterm = qt;
}

void Con_QTerm_f(void)
{
	if(Cmd_IsInsecure())
		Con_Printf("Server tried stuffcmding a restricted command: qterm %s\n", Cmd_Args());
	else
		QT_Create(Cmd_Args());
}
#endif




void Key_ClearTyping (void)
{
	key_lines[edit_line] = BZ_Realloc(key_lines[edit_line], 1);
	key_lines[edit_line][0] = 0;	// clear any typing
	key_linepos = 0;
}

void Con_History_Load(void)
{
	char line[8192];
	char *cr;
	vfsfile_t *file = FS_OpenVFS("conhistory.txt", "rb", FS_GAMEONLY);	//nettest: was FS_ROOT (basedir) -> keep it inside the gamedir (nettest/) so the install root stays clean

	for (edit_line=0 ; edit_line<=CON_EDIT_LINES_MASK ; edit_line++)
	{
		key_lines[edit_line] = BZ_Realloc(key_lines[edit_line], 1);
		key_lines[edit_line][0] = 0;
	}
	edit_line = 0;
	key_linepos = 0;

	if (file)
	{
		while (VFS_GETS(file, line, sizeof(line)-1))
		{
			//strip a trailing \r if its from windows.
			cr = line + strlen(line);
			if (cr > line && cr[-1] == '\r')
				cr[-1] = '\0';
			key_lines[edit_line] = BZ_Realloc(key_lines[edit_line], strlen(line)+1);
			strcpy(key_lines[edit_line], line);
			edit_line = (edit_line+1) & CON_EDIT_LINES_MASK;
		}
		VFS_CLOSE(file);
	}
	history_line = edit_line;
}
void Con_History_Save(void)
{
	vfsfile_t *file;
	int line;

	if (!FS_GameIsInitialised())
		return;

	if (!con_savehistory.ival)
		return;

	file = FS_OpenVFS("conhistory.txt", "wb", FS_GAMEONLY);	//nettest: was FS_ROOT (basedir) -> write into the gamedir (nettest/) instead
	if (file)
	{
		line = edit_line - CON_EDIT_LINES_MASK;
		if (line < 0)
			line = 0;
		for(; line < edit_line; line++)
		{
			VFS_PUTS(file, key_lines[line]);
#ifdef _WIN32	//use an \r\n for readability with notepad.
			VFS_PUTS(file, "\r\n");
#else
			VFS_PUTS(file, "\n");
#endif
		}
		VFS_CLOSE(file);
	}
}

/*
================
Con_ToggleConsole_f
================
*/
void Con_ToggleConsole_Force(void)
{
	console_t *con = Con_GetMain();

	SCR_EndLoadingPlaque();
	Key_ClearTyping ();

	if (con->flags & CONF_ISWINDOW)
	{
		if (con_curwindow == con && Key_Dest_Has(kdm_cwindows))
		{
			con_curwindow = NULL;
			Key_Dest_Remove(kdm_cwindows);
		}
		else
		{
			con_curwindow = con;
			Key_Dest_Add(kdm_cwindows);
			VRUI_SnapAngle();
		}
	}
	else
	{
		if (Key_Dest_Has(kdm_console))
			Key_Dest_Remove(kdm_console);
		else
		{
			Key_Dest_Add(kdm_console);
			VRUI_SnapAngle();
		}
	}
}
void Con_ToggleConsole_f (void)
{
	extern cvar_t con_stayhidden;

	Con_GetMain();

	if (!con_curwindow)
	{
		for (con_curwindow = con_head; con_curwindow; con_curwindow = con_curwindow->next)
			if (con_curwindow->flags & CONF_ISWINDOW)
				break;
	}

	if (con_curwindow && !Key_Dest_Has(kdm_cwindows|kdm_console))
	{
		Key_Dest_Add(kdm_cwindows);
		return;
	}

#ifdef CSQC_DAT
	if (!editormodal && CSQC_ConsoleCommand(-1, "toggleconsole"))
	{
		Key_Dest_Remove(kdm_console);
		return;
	}
#endif

	if (con_stayhidden.ival >= 3)
	{
		Key_Dest_Remove(kdm_cwindows);
		return;	//its hiding!
	}

	Con_ToggleConsole_Force();
}

void Con_ClearCon(console_t *con)
{
	conline_t *t;
	while (con->current)
	{
		t = con->current;
		con->current = t->older;
		Z_Free(t);
	}
	con->display = con->current = con->oldest = NULL;
	con->displayscroll = 0;	//nettest: reset the smooth-scroll offset too - else `clear` while scrolled up leaves the view stuck (a non-zero displayscroll fails the auto-scroll-to-bottom gate in Con_PrintCon)
	con->selstartline = NULL;
	con->selendline = NULL;

	/*reset the line pointers, create an active line*/
	Con_Finit(con);
}

/*
================
Con_Clear_f
================
*/
void Con_Clear_f (void)
{
	console_t *con = Con_FindConsole(Cmd_Argv(1));
	if (!con || Cmd_IsInsecure())
		return;
	Con_ClearCon(con);
}

//nettest: Ctrl+F find-in-scrollback (driven from keys.c Key_Console). Search the scrollback from
//con->display in direction `dir` (-1 = older/up, +1 = newer/down) for the first line containing
//`text` (case-insensitive). On a hit, scroll the view to it (con->display) and select the matched
//span (+ CONF_KEEPSELECTION) so the existing selection-draw highlights it. Returns true on a hit.
//Callers reset con->display = con->current first for an incremental "search from the bottom".
qboolean Con_SearchText(console_t *con, const char *text, int dir)
{
	conline_t *l;
	conchar_t *cc;
	char buf[2048];
	int i, n;
	int tlen = text ? (int)strlen(text) : 0;
	if (!tlen || !con->display)
		return false;
	for (l = (dir < 0) ? con->display->older : con->display->newer; l; l = (dir < 0) ? l->older : l->newer)
	{
		if (l == con->current)
			continue;	//skip the live input line
		cc = (conchar_t*)(l+1);
		n = l->length;
		if (n > (int)sizeof(buf)-1)
			n = sizeof(buf)-1;
		for (i = 0; i < n; i++)
		{
			int ch = cc[i] & CON_CHARMASK;
			buf[i] = (ch >= 32 && ch < 127) ? ch : ' ';
		}
		buf[n] = 0;
		for (i = 0; i + tlen <= n; i++)
		{
			if (!Q_strncasecmp(buf+i, text, tlen))
			{	//hit - scroll to it + select the span
				con->display = l;
				con->displayscroll = 0;
				con->selstartline = con->selendline = l;
				con->selstartoffset = i;
				con->selendoffset = i + tlen;
				con->flags |= CONF_KEEPSELECTION;
				return true;
			}
		}
	}
	return false;
}

void Cmd_ConEchoCenter_f(void)
{
	console_t *con;
	con = Con_FindConsole(Cmd_Argv(1));
	if (!con)
		con = Con_Create(Cmd_Argv(1), 0);
	if (con)
	{
		Cmd_ShiftArgs(1, false);
		Con_PrintCon(con, Cmd_Args(), con->parseflags|PFS_NONOTIFY|PFS_CENTERED );
		Con_PrintCon(con, "\n", con->parseflags|PFS_NONOTIFY|PFS_CENTERED);
	}
}
void Cmd_ConEcho_f(void)
{
	console_t *con;
	con = Con_FindConsole(Cmd_Argv(1));
	if (!con)
		con = Con_Create(Cmd_Argv(1), 0);
	if (con)
	{
		Cmd_ShiftArgs(1, false);
		Con_PrintCon(con, Cmd_Args(), con->parseflags);
		Con_PrintCon(con, "\n", con->parseflags);
	}
}

void Cmd_ConClear_f(void)
{
	console_t *con;
	con = Con_FindConsole(Cmd_Argv(1));
	if (con)
		Con_ClearCon(con);
}
void Cmd_ConClose_f(void)
{
	console_t *con;
	con = Con_FindConsole(Cmd_Argv(1));
	if (con)
		Con_Destroy(con);
}
void Cmd_ConActivate_f(void)
{
	console_t *con;
	con = Con_FindConsole(Cmd_Argv(1));
	if (con)
		Con_SetActive(con);
}

/*
================
Con_MessageMode_f
================
*/
void Con_MessageMode_f (void)
{
	chat_team = false;
	Key_Dest_Add(kdm_message);
	Key_Dest_Remove(kdm_console);
}

/*
================
Con_MessageMode2_f
================
*/
void Con_MessageMode2_f (void)
{
	chat_team = true;
	Key_Dest_Add(kdm_message);
	Key_Dest_Remove(kdm_console);
}

void Con_ForceActiveNow(void)
{
	Key_Dest_Add(kdm_console);
	scr_con_target = scr_con_current = vid.height;
}

/*
================
Con_Init
================
*/
void Log_Init (void);

void Con_Init (void)
{
	con_current = NULL;
	con_head = NULL;

	//FTESurf Patch 213: registered BEFORE the console that reads it is created.
	//Patch 211 had this the other way round and the read landed on a NULL .string.
	Cvar_Register (&con_window_rect, "Console controls");

	con_main = Con_GetMain();

	con_initialized = true;
//	Con_TPrintf ("Console initialized.\n");

//
// register our commands
//
	Cvar_Register (&con_centernotify, "Console controls");
	Cvar_Register (&con_notifytime, "Console controls");
	Cvar_Register (&con_notifytime_error, "Console controls");	//FTESurf Patch 242
	Cvar_Register (&con_notifystyle, "Console controls");		//FTESurf Patch 242
	Cvar_Register (&con_notifyfade, "Console controls");
	Cvar_Register (&con_notify_x, "Console controls");
	Cvar_Register (&con_notify_y, "Console controls");
	Cvar_Register (&con_notify_w, "Console controls");
	Cvar_Register (&con_numnotifylines, "Console controls");
	Cvar_Register (&con_displaypossibilities, "Console controls");
	Cvar_Register (&con_showcompletion, "Console controls");
	Cvar_Register (&cl_chatmode, "Console controls");
	Cvar_Register (&con_maxlines, "Console controls");
	Cvar_Register (&con_numnotifylines_chat, "Console controls");
	Cvar_Register (&con_notifytime_chat, "Console controls");
	Cvar_Register (&con_separatechat, "Console controls");
	Cvar_Register (&con_timestamps, "Console controls");
	Cvar_Register (&con_timeformat, "Console controls");
	Cvar_Register (&con_textsize, "Console controls");
	Cvar_Register (&con_window, "Console controls");
	Cvar_Register (&con_savehistory, "Console controls");
	Cvar_Register (&con_keepscroll, "Console controls");
	//con_window_rect is registered at the TOP of this function -- Patch 213.
	Cvar_Register (&con_window_titlepad, "Console controls");
	Cvar_Register (&con_scrollwidth, "Console controls");
	Cvar_Register (&con_colour_back, "Console controls");
	Cvar_Register (&con_colour_accent, "Console controls");
	Cvar_Register (&con_completionrows, "Console controls");
	Cvar_Register (&con_completiondown, "Console controls");	//FTESurf Patch 215
	Cvar_Register (&con_gripsize, "Console controls");
	Cvar_Register (&con_palette, "Console controls");
	Cvar_ForceCallback(&con_palette);	//the table is only written by the callback
	Cvar_ForceCallback(&con_window);

	Cmd_AddCommand ("toggleconsole", Con_ToggleConsole_f);
	Cmd_AddCommand ("messagemode", Con_MessageMode_f);
	Cmd_AddCommand ("messagemode2", Con_MessageMode2_f);
	Cmd_AddCommand ("clear", Con_Clear_f);
#ifdef QTERM
	Cmd_AddCommand ("qterm", Con_QTerm_f);
#endif

	Cmd_AddCommandD ("conecho_center", Cmd_ConEchoCenter_f, "conecho_center consolename The Text To Echo\nUse \"\" for the main console.\nAny added lines will be aligned to the middle of the console.");
	Cmd_AddCommandD ("conecho", Cmd_ConEcho_f, "conecho consolename The Text To Echo\nEchos text to a named console instead of just the main one.");
	Cmd_AddCommandD ("conclear", Cmd_ConClear_f, "Clears a named console (instead of just the main one)");
	Cmd_AddCommandD ("conclose", Cmd_ConClose_f, "Destroys a named console");
	Cmd_AddCommandD ("conactivate", Cmd_ConActivate_f, "Brings focus to the named console. Will not do anything if the named console is not created yet (so be sure to do any echos before using this command)");

	Log_Init();
}

void Con_Shutdown(void)
{
	int i;

	for (i = 0; i <= CON_EDIT_LINES_MASK; i++)
	{
		BZ_Free(key_lines[i]);
	}

	while(con_head)
		Con_Destroy(con_head);
	con_initialized = false;
}

void TTS_SayConString(conchar_t *stringtosay);

/*
================
Con_Print

Handles cursor positioning, line wrapping, etc
All console printing must go through this in order to be logged to disk
If no console is visible, the notify window will pop up.
================
*/

//reallocates a line (with its buffer), and updates its links. if shrinking, be sure to reduce the length
conline_t *Con_ResizeLineBuffer(console_t *con, conline_t *old, unsigned int length)
{
	conline_t *l;

	old->maxlength = length & 0xffff;
	if (old->maxlength < old->length)
		return NULL;	//overflow.
	l = BZ_Realloc(old, sizeof(*l)+(old->maxlength)*sizeof(conchar_t));

	if (l->newer)
		l->newer->older = l;
	if (l->older)
		l->older->newer = l;

	if (con->selstartline == old)
		con->selstartline = l;
	if (con->selendline == old)
		con->selendline = l;
	if (con->display == old)
		con->display = l;
	if (con->oldest == old)
		con->oldest = l;
	if (con->current == old)
		con->current = l;
	if (con->footerline == old)
		con->footerline = l;
	if (con->userline == old)
		con->userline = l;
	if (con->highlightline == old)
		con->highlightline = old;
	return l;
}

qboolean Con_InsertConChars (console_t *con, conline_t *line, int offset, conchar_t *c, int len)
{
	conchar_t *o;

	if (line->length+len > line->maxlength)
	{
		line = Con_ResizeLineBuffer(con, line, line->length+len + 8);
		if (!line)
			return false;	//overflowed!
	}

	o = (conchar_t *)(line+1);
	if (line->length-offset)
		memmove(o+offset+len, o+offset, (line->length-offset)*sizeof(conchar_t));
	memcpy(o+offset, c, sizeof(*o) * len);
	line->length+=len;
	return true;
}

void Con_PrintCon (console_t *con, const char *txt, unsigned int parseflags)
{
	conchar_t expanded[4096];
	conchar_t *c, *n;
	conline_t *reuse;
	int maxlines;
	unsigned flags, codepoint;

	if (con->maxlines)
		maxlines = con->maxlines;
	else
		maxlines = con_maxlines.ival;

	/*FTESurf Patch 242: does this text announce itself as a failure?

	  There is no print LEVEL in this engine -- Con_Printf takes a string and
	  nothing else -- but there is a convention, and it is universal: an error
	  begins with CON_ERROR and a warning with CON_WARNING, which are the colour
	  markers "^&C0" and "^&E0".  438 call sites already spell themselves that
	  way, so reading the prefix here marks every one of them without touching a
	  single one, and without inventing a second way to say the same thing.

	  Tested on the RAW text rather than after COM_ParseFunString, because after
	  parsing the marker has become colour bits on the first character and
	  telling "this is an error" from "this line happens to start red" would
	  need a colour comparison that any theme change could break.

	  Sticky on the console rather than a local, because Con_Printf need not end
	  in a newline: the flag has to survive to whichever call finally completes
	  the line, and is cleared there.*/
	if (!strncmp(txt, CON_ERROR, 4) || !strncmp(txt, CON_WARNING, 4))
		con->pendingerror = true;

	COM_ParseFunString(con->defaultcharbits, txt, expanded, sizeof(expanded), parseflags);

	c = expanded;
	if (*c)
		con->unseentext = true;
	for (;*c; c=n)
	{
		n = Font_Decode(c, &flags, &codepoint);
		if (codepoint=='\r' && !(flags&CON_HIDDEN))
			con->cr = true;
		else if (codepoint=='\n' && !(flags&CON_HIDDEN))
		{
			con->cr = false;
			reuse = NULL;
			while (con->linecount >= maxlines)
			{
				if (con->oldest == con->current)
					break;

				if (con->selstartline == con->oldest)
					con->selstartline = NULL;
				if (con->selendline == con->oldest)
					con->selendline = NULL;

				if (con->display == con->oldest)
					con->display = con->oldest->newer;
				con->oldest = con->oldest->newer;
				if (reuse)
					Z_Free(con->oldest->older);
				else
					reuse = con->oldest->older;
				con->oldest->older = NULL;
				con->linecount--;
			}
			con->linecount++;
			con->current->time = realtime;
			con->current->flags = 0;
			//FTESurf Patch 242: the line is finished, so the pending mark
			//belongs to it and to nothing after it.
			if (con->pendingerror)
			{
				con->current->flags |= CONL_ERROR;
				con->pendingerror = false;
			}
			if (parseflags & PFS_CENTERED)
				con->current->flags |= CONL_CENTERED;
			if (parseflags & PFS_NONOTIFY)
				con->current->flags |= CONL_NONOTIFY;
			else if (!Key_Dest_Has(~kdm_game) && (con->flags & CONF_NOTIFY))
				VRUI_SnapAngle();

#if defined(HAVE_SPEECHTOTEXT)
			if (con->current)
				TTS_SayConString((conchar_t*)(con->current+1));
#endif

			if (!reuse)
			{
				reuse = Z_Malloc(sizeof(conline_t) + sizeof(conchar_t));
				reuse->maxlength = 1;
			}
			else
			{
				reuse->newer = NULL;
				reuse->older = NULL;
			}
			reuse->id = (++con->nextlineid) & 0xffff;
			reuse->older = con->current;
			con->current->newer = reuse;
			con->current = reuse;
			con->current->length = 0;
			if (con->display == con->current->older && con->displayscroll==0)
				con->display = con->current;
		}
		else
		{
			if (con->cr)
			{
				con->current->length = 0;
				con->cr = false;
			}
			if (!con->current->numlines)
				con->current->numlines = 1;

			if (!con->current->length && con_timestamps.ival && !(parseflags & PFS_CENTERED))
			{
				char timeasc[64];
				conchar_t timecon[64], *timeconend;
				time_t rawtime;
				time (&rawtime);
				strftime(timeasc, sizeof(timeasc), con_timeformat.string, localtime (&rawtime));
				timeconend = COM_ParseFunString(con->defaultcharbits, timeasc, timecon, sizeof(timecon), false);
				Con_InsertConChars(con, con->current, con->current->length, timecon, timeconend-timecon);
			}

			//FIXME: don't do this a char at a time
			Con_InsertConChars(con, con->current, con->current->length, c, n-c);
		}
	}

	con->current->time = realtime;
}

void Con_CenterPrint(const char *txt)
{
	console_t *c = Con_GetMain();
	int flags = c->parseflags|PFS_NONOTIFY|PFS_CENTERED;
	Con_PrintCon(c, "^Ue01d^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01f\n", flags);
	Con_PrintCon(c, txt, flags);	//client console
	Con_PrintCon(c, "\n^Ue01d^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01f\n", flags);
}

void Con_Print (const char *txt)
{
	console_t *c = Con_GetMain();
	Con_PrintCon(c, txt, c->parseflags);	//client console
}
void Con_PrintFlags(const char *txt, unsigned int setflags, unsigned int clearflags)
{
	console_t *c = Con_GetMain();
	setflags |= c->parseflags;
	setflags &= ~clearflags;

// also echo to debugging console
	Sys_Printf ("%s", txt);	// also echo to debugging console

// log all messages to file
	Con_Log (txt);

	if (con_initialized)
		Con_PrintCon(c, txt, setflags);
}

void Con_CycleConsole(void)
{
	console_t *first = con_current?con_current:con_head;
	while(1)
	{
		con_current = con_current->next;
		if (!con_current)
			con_current = con_head;
		if (con_current == first)
		{
			if (con_current->flags & (CONF_HIDDEN|CONF_ISWINDOW))
				con_current = NULL; //no valid consoles
			break;	//we wrapped? oh noes
		}

		if (con_current->flags & (CONF_HIDDEN|CONF_ISWINDOW))
			continue;	//this is a valid choice
		break;
	}
}

/*
================
Con_Printf

Handles cursor positioning, line wrapping, etc
================
*/

#ifdef HAVE_SERVER
extern redirect_t	sv_redirected;
extern char	sv_redirected_buf[8000];
void SV_FlushRedirect (void);
#endif
vfsfile_t *con_pipe;

#define	MAXPRINTMSG	4096
static void Con_PrintFromThread (void *ctx, void *data, size_t a, size_t b)
{
	Con_Printf("%s", (char*)data);
	BZ_Free(data);
}

vfsfile_t *Con_POpen(const char *conname)
{
	if (!conname || !*conname)
	{
		if (con_pipe)
			VFS_CLOSE(con_pipe);
		con_pipe = VFSPIPE_Open(2, false);
		return con_pipe;
	}
	return NULL;
}

// FIXME: make a buffer size safe vsprintf?
void VARGS Con_Printf (const char *fmt, ...)
{
	va_list		argptr;
	char		msg[MAXPRINTMSG];

	va_start (argptr,fmt);
	vsnprintf (msg,sizeof(msg), fmt,argptr);
	va_end (argptr);

	if (!Sys_IsMainThread())
	{
		COM_AddWork(WG_MAIN, Con_PrintFromThread, NULL, Z_StrDup(msg), 0, 0);
		return;
	}

#ifdef HAVE_SERVER
	// add to redirected message
	if (sv_redirected)
	{
		if (strlen (msg) + strlen(sv_redirected_buf) > sizeof(sv_redirected_buf) - 1)
			SV_FlushRedirect ();
		strcat (sv_redirected_buf, msg);
		return;
	}
#endif

// also echo to debugging console
	Sys_Printf ("%s", msg);	// also echo to debugging console

// log all messages to file
	Con_Log (msg);

	if (con_pipe)
		VFS_PUTS(con_pipe, msg);

	if (!con_initialized)
		return;

// write it to the scrollable buffer
	Con_Print (msg);
}

void VARGS Con_SafePrintf (const char *fmt, ...)
{	//obsolete version of the function
	va_list		argptr;
	char		msg[MAXPRINTMSG];

	va_start (argptr,fmt);
	vsnprintf (msg,sizeof(msg)-1, fmt,argptr);
	va_end (argptr);

// write it to the scrollable buffer
	Con_Printf ("%s", msg);
}

void VARGS Con_TPrintf (translation_t text, ...)
{
	va_list		argptr;
	char		msg[MAXPRINTMSG];
	const char *fmt = localtext(text);

	va_start (argptr,text);
	vsnprintf (msg,sizeof(msg), fmt,argptr);
	va_end (argptr);

// write it to the scrollable buffer
	Con_Printf ("%s", msg);
}

void VARGS Con_SafeTPrintf (translation_t text, ...)
{
	va_list		argptr;
	char		msg[MAXPRINTMSG];
	const char *fmt = localtext(text);

	va_start (argptr,text);
	vsnprintf (msg,sizeof(msg), fmt,argptr);
	va_end (argptr);

// write it to the scrollable buffer
	Con_Printf ("%s", msg);
}

static void Con_DPrintFromThread (void *ctx, void *data, size_t a, size_t b)
{
	if (log_developer.ival || !a)
		Con_Log(data);
	if (developer.ival >= (int)a)
	{
		console_t *c = Con_GetMain();
		Sys_Printf ("%s", (const char*)data);	// also echo to debugging console
		Con_PrintCon(c, data, c->parseflags);
	}
	BZ_Free(data);
}
/*
================
Con_DPrintf

A Con_Printf that only shows up if the "developer" cvar is set
================
*/
void VARGS Con_DPrintf (const char *fmt, ...)
{
	va_list		argptr;
	char		msg[MAXPRINTMSG];

#ifdef CRAZYDEBUGGING
	va_start (argptr,fmt);
	vsnprintf (msg,sizeof(msg)-1, fmt,argptr);
	va_end (argptr);
	Sys_Printf("%s", msg);
	return;
#else
	if (!developer.ival && !log_developer.ival)
		return; // early exit
#endif

	va_start (argptr,fmt);
	vsnprintf (msg,sizeof(msg)-1, fmt,argptr);
	va_end (argptr);

	if (!Sys_IsMainThread())
	{
		COM_AddWork(WG_MAIN, Con_DPrintFromThread, NULL, Z_StrDup(msg), 1, 0);
		return;
	}

	if (log_developer.ival)
		Con_Log(msg);
	if (developer.ival)
	{
		console_t *c = Con_GetMain();
		Sys_Printf ("%s", msg);	// also echo to debugging console
		Con_PrintCon(c, msg, c->parseflags);
	}
}
void VARGS Con_DLPrintf (int level, const char *fmt, ...)
{
	va_list		argptr;
	char		msg[MAXPRINTMSG];

#ifdef CRAZYDEBUGGING
	va_start (argptr,fmt);
	vsnprintf (msg,sizeof(msg)-1, fmt,argptr);
	va_end (argptr);
	Sys_Printf("%s", msg);
	return;
#else
	if (developer.ival<level && (!log_developer.ival && level))
		return; // early exit
#endif

	va_start (argptr,fmt);
	vsnprintf (msg,sizeof(msg)-1, fmt,argptr);
	va_end (argptr);

	if (!Sys_IsMainThread())
	{
		COM_AddWork(WG_MAIN, Con_DPrintFromThread, NULL, Z_StrDup(msg), level, 0);
		return;
	}

	if (log_developer.ival || !level)
		Con_Log(msg);
	if (developer.ival >= level)
	{
		Sys_Printf ("%s", msg);	// also echo to debugging console
		if (con_initialized)
		{
			console_t *c = Con_GetMain();
			Con_PrintCon(c, msg, c->parseflags);
		}
	}
}

void VARGS Con_ThrottlePrintf (float *timer, int developerlevel, const char *fmt, ...)
{
	va_list		argptr;
	char		msg[MAXPRINTMSG];
	float now = realtime;

	if (*timer > now)
		;	//in the future? zomg
	else if (*timer >= now-1)
		return;	//within the last second
	*timer = now;	//in the future? zomg

	va_start (argptr,fmt);
	vsnprintf (msg,sizeof(msg)-1, fmt,argptr);
	va_end (argptr);

	if (developerlevel)
		Con_DLPrintf(developerlevel, "%s", msg);
	else
		Con_Printf("%s", msg);
}

static void Con_FooterMarked(console_t *con, qboolean append, conchar_t *marked, conchar_t *markedend)
{
	int oldlen, newlen;
	conline_t *newf = NULL, *l;
	unsigned fl, cp;
	conchar_t *nl, *n;

	if (!append)
	{
		while(con->footerline)
		{
			l = con->footerline;
			con->footerline = l->older;
			if (con->selstartline == l)
				con->selstartline = NULL;
			if (con->selendline == l)
				con->selendline = NULL;
			Z_Free(l);
		}
		con->footerline = NULL;
	}
	for (append = true; marked < markedend; marked = n, append = false)
	{
		n = markedend;
		for (nl = marked; nl < markedend; nl=n)
		{
			n = Font_Decode(nl, &fl, &cp);
			if (cp == '\n' && !(fl&CONF_HIDDEN))
				break;
		}

		newlen = nl - marked;
		if (append && con->footerline)
			oldlen = con->footerline->length;
		else
			oldlen = 0;

		if (newlen || !append)
		{
			newf = Z_Malloc(sizeof(*newf) + (oldlen + newlen) * sizeof(conchar_t));
			if (append && con->footerline)
			{
				memcpy(newf, con->footerline, sizeof(*con->footerline)+oldlen*sizeof(conchar_t));
				Z_Free(con->footerline);
			}
			else
				newf->older = con->footerline;
			if (newf->older)
				newf->older->newer = newf;

			memcpy((conchar_t*)(newf+1)+oldlen, marked, newlen*sizeof(conchar_t));
			newf->length = oldlen + newlen;
			con->footerline = newf;
		}
	}
}

/*description text at the bottom of the console*/
void Con_Footerf(console_t *con, qboolean append, const char *fmt, ...)
{
	va_list		argptr;
	char		msg[MAXPRINTMSG];
	conchar_t	marked[MAXPRINTMSG], *markedend;

	if (!con)
		con = con_current;
	if (!con)
		return;

	va_start (argptr,fmt);
	vsnprintf (msg,sizeof(msg)-1, fmt,argptr);
	va_end (argptr);
	markedend = COM_ParseFunString((COLOR_YELLOW << CON_FGSHIFT)|(con->backshader?CON_NONCLEARBG:0), msg, marked, sizeof(marked), false);

	Con_FooterMarked(con, append, marked, markedend);
}

/*
==============================================================================

DRAWING

==============================================================================
*/

qboolean COM_InsertIME(conchar_t *buffer, size_t buffersize, conchar_t **cursor, conchar_t **textend)
{
	conchar_t *in = vid.ime_preview;
	if (in && *in && *textend+vid.ime_previewlen < buffer+buffersize)
	{
		memmove(buffer + (*cursor-buffer) + vid.ime_previewlen, *cursor, ((*textend-*cursor)+1)*sizeof(conchar_t));
		memcpy(buffer + (*cursor-buffer), in, vid.ime_previewlen*sizeof(conchar_t));
		*cursor += vid.ime_caret;
		*textend += vid.ime_previewlen;

		return true;
	}
	return false;
}

/*
================
Con_DrawInput

The input line scrolls horizontally if typing goes beyond the right edge
y is the bottom of the input
return value is the top of the region
================
*/
int Con_DrawInput (console_t *con, qboolean focused, int left, int right, int y, int top, qboolean selactive, int selsx, int selex, int selsy, int seley)
{
	int		i;
	int		drewdown = 0;	//FTESurf Patch 215: rows the drop-DOWN already drew below the input line
	int lhs, rhs;
	int p;
	unsigned char	*text, *fname = NULL;
	extern int con_commandmatch;
	conchar_t maskedtext[2048];
	conchar_t *endmtext;
	conchar_t *cursor;
	conchar_t *cchar;
	conchar_t *textstart;
	size_t textsize;
	qboolean cursorframe;
	unsigned int codeflags, codepoint;
	int cursorpos;
	qboolean hidecomplete;

	int x;

	if (focused)
	{
		vid.ime_allow = true;
		vid.ime_position[0] = ((float)left/vid.pixelwidth)*vid.width;
		vid.ime_position[1] = ((float)y/vid.pixelheight)*vid.height;
	}

	if (!con->linebuffered || con->linebuffered == Con_Navigate)
	{
		if (con->footerline)
		{
			y = Con_DrawConsoleLines(con, con->footerline, 0, left, right, y, 0, selactive, selsx, selex, selsy, seley, 0);
		}
		return y;	//fixme: draw any unfinished lines of the current console instead.
	}

	y -= Font_CharHeight();

	if (!focused)
		return y;		// don't draw anything (always draw if not active)

	text = key_lines[edit_line];

	/*
	FTESurf Patch 215: the drop-DOWN.

		"is it possible to display the option preview below the typing field?
		 source has it be a drop down, not a drop up?"

	Con_DrawInput is bottom-anchored: the caller hands it the BOTTOM of the console
	text region and everything it draws walks upward from there -- input line
	first, then the footer, then the completion list, and whatever y it returns is
	where the scrollback then starts. So there is no space "below the input line"
	to draw into; the input line IS the bottom.

	Drawing the list outside the console instead -- Source's true overlay -- is not
	available either: a console window is scissored to its own rect, so anything
	below the bottom edge is clipped away rather than drawn over the game.

	So "below the input line" is implemented as: the list claims the bottom band,
	and the input line moves up to sit directly above it. y+Font_CharHeight() is
	the bottom the caller passed in, before the line above reserved the input row.
	Con_DrawConsoleLines returns the top of what it drew, so the input row is one
	character height above that -- no row counting, and a row that WRAPS in a
	narrow window is accounted for for free.

	Consequence worth stating rather than burying: the input line moves as the
	match count changes. That is inherent to doing it in HERE -- a bottom-anchored
	console can either pin the field or put the list under it, not both. Patch 226
	adds mode 2, which pins the field by taking the list out of this function's
	vertical budget entirely; see con_popupcon and Con_DrawCompletionPopup.
	*/
	if (con->commandcompletion && con_displaypossibilities.ival >= 2 && con_displaypossibilities.value &&
		text[0] && !(text[0] == '/' && !text[1]))
	{
		if (con_completiondown.ival == 2)
		{
			/*
			FTESurf Patch 226: build the chain, record where the field is, draw
			nothing, and DO NOT TOUCH y. The popup lives outside the console, so
			its height is its own business rather than a claim on this budget --
			which is exactly why the input line stops moving.

			con_completionrows+1 rather than a height-derived budget for the same
			reason: the space that bounds this list is the SCREEN, and the screen
			is not known here. Con_DrawCompletionPopup clamps against it.
			*/
			int rows = Con_BuildCompletion(con, text, con_completionrows.ival + 1);
			if (rows > 0 && con->completionline)
			{
				con_popupcon  = con;
				con_popuprows = rows;
				con_popupl    = left;
				con_popupr    = right;
				con_popupb    = y + Font_CharHeight();	//the bottom the caller passed in
			}
			/*Set even when there is nothing to show, so the drop-UP arm below
			  cannot rebuild the chain and draw it inside the console instead.*/
			drewdown = 1;
		}
		else if (con_completiondown.ival)
		{
			/*
			How many rows fit. y is already the input row's tentative top, so the
			band below it runs from y+charheight down, and the input row lands at
			y - rows*charheight: the exact fit is (y-top)/charheight. One row is
			given back because the footer is drawn AFTER this point and is not in y
			yet, and because a console showing the list and the input line and no
			scrollback at all is not worth drawing. top is 0 for the fullscreen
			console, where the limit is simply the top of the screen.
			*/
			int maxrows = ((y - (top>0?top:0)) / Font_CharHeight()) - 1;
			if (maxrows > 0)
			{
				drewdown = Con_BuildCompletion(con, text, maxrows);
				if (drewdown && con->completionline)
				{
					/*
					top+charheight, not top: Con_DrawConsoleLines stops at whatever
					`top` it is given, and the input row then goes one height ABOVE
					where it stopped. Handing it the real top would let a wrapped row
					spend the last of the budget and push the input line out of the
					console. Reserving that row here is what makes the entry budget
					above advisory rather than load-bearing.
					*/
					y = Con_DrawConsoleLines(con, con->completionline, 0, left, right, y + Font_CharHeight(),
							(top>0)?top+Font_CharHeight():0, selactive, selsx, selex, selsy, seley, 0) - Font_CharHeight();
				}
			}
		}
	}

	//the input row has moved; say where it actually is. (Nothing on win32 reads
	//this today -- the engine never places an OS IME candidate window -- but a
	//coordinate that is knowingly wrong is not worth keeping.)
	if (focused)
		vid.ime_position[1] = ((float)y/vid.pixelheight)*vid.height;

	cursorpos = key_linepos;

	//copy it to an alternate buffer and fill in text colouration escape codes.
	//if it's recognised as a command, colour it yellow.
	//if it's not a command, and the cursor is at the end of the line, leave it as is,
	//	but add to the end to show what the compleation will be.

	textstart = COM_ParseFunString(CON_WHITEMASK, con->prompt, maskedtext, sizeof(maskedtext) - sizeof(maskedtext[0]), PFS_FORCEUTF8);
	textsize = (countof(maskedtext) - (textstart-maskedtext) - 1) * sizeof(maskedtext[0]);
	i = text[cursorpos];
	text[cursorpos] = 0;
	cursor = COM_ParseFunString(CON_WHITEMASK, text, textstart, textsize, PFS_KEEPMARKUP | PFS_FORCEUTF8);
	//okay, so that's where the cursor is. heal the input string and reparse (so we don't mess up escapes)
	text[cursorpos] = i;
	endmtext = COM_ParseFunString(CON_WHITEMASK, text, textstart, textsize, PFS_KEEPMARKUP | PFS_FORCEUTF8);
//	endmtext = COM_ParseFunString(CON_WHITEMASK, text+key_linepos, cursor, ((char*)maskedtext)+sizeof(maskedtext) - (char*)(cursor+1), PFS_KEEPMARKUP | PFS_FORCEUTF8);

	hidecomplete = COM_InsertIME(maskedtext, countof(maskedtext), &cursor, &endmtext);
/*	if (cursorpos == strlen(text) && vid.ime_preview)
	{
		endmtext = COM_ParseFunString(COLOR_MAGENTA<<CON_FGSHIFT, vid.ime_preview, endmtext, (countof(maskedtext) - (endmtext-maskedtext) - 1) * sizeof(maskedtext[0]), PFS_KEEPMARKUP | PFS_FORCEUTF8);
		cursor += strlen(vid.ime_preview);
		cursorpos += strlen(vid.ime_preview);
		text = va("%s%s", text, vid.ime_preview);
	}
*/

	if ((char*)endmtext == (char*)(maskedtext-2) + sizeof(maskedtext))
		endmtext[-1] = CON_WHITEMASK | '+' | CON_NONCLEARBG;
	endmtext[1] = 0;

	if ((*cursor & CON_HIDDEN) && cursor[0] != (CON_HIDDEN|'^') && cursor[1] != CON_LINKSTART)
	{	//if we're in the middle of a link (but not at the very first char - to make life prettier) then reveal the hidden text so that you can actually see what you're editing.
		for (i = cursor-textstart; textstart[i]; i++)
		{
			if (textstart[i] == CON_LINKSTART)
				break;
			if (textstart[i] == CON_LINKEND)
			{
				textstart[i] &= ~CON_HIDDEN;
				break;
			}
			textstart[i] &= ~CON_HIDDEN;
		}
		for (i = cursor-textstart; i>=0; i--)
		{
			if (textstart[i] == CON_LINKEND)
				break;
			if (textstart[i] == CON_LINKSTART)
			{
				textstart[i] &= ~CON_HIDDEN;
				if (--i >= 0)	//make the ^ of the ^[ shown too.
					textstart[i] &= ~CON_HIDDEN;
				break;
			}
			textstart[i] &= ~CON_HIDDEN;
		}
	}

	i = 0;
	x = left;

	if (!hidecomplete && con->commandcompletion && con_showcompletion.ival && text[0] && !(text[0] == '/' && !text[1]))
	{
		if (cl_chatmode.ival && (text[0] == '/' || (cl_chatmode.ival == 2 && Cmd_IsCommand(text))))
		{	//color the first token yellow, it's a valid command
			for (p = 0; (textstart[p]&CON_CHARMASK)>' '; p++)
				textstart[p] = (textstart[p]&CON_CHARMASK) | (COLOR_YELLOW<<CON_FGSHIFT);
		}

		if (cursor == endmtext)	//cursor is at end
		{
			int cmdstart;
			cmdstart = text[0] == '/'?1:0;
			fname = Cmd_CompleteCommand(text+cmdstart, true, true, max(1, con_commandmatch), NULL);
			/*
			FTESurf Patch 215: only paint the green inline hint when the match
			actually EXTENDS what you typed.

			The loop below starts writing at cursorpos and copies the completion
			from that offset on, which is right precisely because a prefix match's
			first cursorpos characters ARE what you typed. sv_mapcompletion breaks
			that: `map kits` matches "map surf_kitsune", whose 8th character
			onwards is "_kitsune", and the line drew as `map kits_kitsune` -- text
			that was never typed and would never be run. Measured on the first
			screenshot of this patch.

			When the match is not an extension there is simply nothing to hint at,
			so nothing is drawn; the dropdown underneath is still the answer.
			*/
			if (fname && strlen(fname) < 256 &&
				!Q_strncasecmp(fname, text+cmdstart, cursorpos-cmdstart))	//we can compleate it to:
			{
				for (p = min(strlen(fname), cursorpos-cmdstart); fname[p]>0; p++)
					textstart[p+cmdstart] = (unsigned int)fname[p] | (COLOR_GREEN<<CON_FGSHIFT);
				if (p < cursorpos-cmdstart)
					p = cursorpos-cmdstart;
				p = min(p+cmdstart, sizeof(maskedtext)/sizeof(maskedtext[0]) - 3);
				textstart[p] = 0;
				textstart[p+1] = 0;
			}
		}
	}

	if (!vid.activeapp)
		cursorframe = 0;
	else
		cursorframe = ((int)(realtime*con_cursorspeed)&1);

	//FIXME: support tab somehow
	for (lhs = 0, cchar = maskedtext; cchar < cursor; )
	{
		cchar = Font_Decode(cchar, &codeflags, &codepoint);
		lhs += Font_CharWidth(codeflags, codepoint);
	}
	for (rhs = 0, cchar = cursor; *cchar; )
	{
		cchar = Font_Decode(cchar, &codeflags, &codepoint);
		rhs += Font_CharWidth(codeflags, codepoint);
	}

	//put the cursor in the middle
	x = (right-left)/2 + left;
	//move the line to the right if there's not enough text to touch the right hand side
	if (x < right-rhs - Font_CharWidth(CON_WHITEMASK, 0xe000|11))
		x = right - rhs - Font_CharWidth(CON_WHITEMASK, 0xe000|11);
	//if the left hand side is on the right of the left point (overrides right alignment)
	if (x > lhs + left)
		x = lhs + left;

	lhs = x - lhs;
	for (cchar = maskedtext; cchar < cursor; )
	{
		cchar = Font_Decode(cchar, &codeflags, &codepoint);
		lhs = Font_DrawChar(lhs, y, codeflags, codepoint);
	}
	rhs = x;
	cchar = Font_Decode(cursor, &codeflags, &codepoint);
	if (cursorframe)
	{
//		extern cvar_t com_parseutf8;
//		if (com_parseutf8.ival)
//			Font_DrawChar(rhs, y, (*cursor&~(CON_BGMASK|CON_FGMASK)) | (COLOR_BLUE<<CON_BGSHIFT) | CON_NONCLEARBG | CON_WHITEMASK);
//		else
			Font_DrawChar(rhs, y, CON_WHITEMASK, 0xe000|11);
	}
	else if (codepoint)
	{
		Font_DrawChar(rhs, y, codeflags, codepoint);
	}
	if (codepoint)
	{
		rhs += Font_CharWidth(codeflags, codepoint);
		while (*cchar)
		{
			cchar = Font_Decode(cchar, &codeflags, &codepoint);
			rhs = Font_DrawChar(rhs, y, codeflags, codepoint);
		}
	}

	/*if its getting completed to something, show some help about the command that is going to be used*/
	if (con->footerline)
	{
		y = Con_DrawConsoleLines(con, con->footerline, 0, left, right, y, 0, selactive, selsx, selex, selsy, seley, 0);
	}

	/*
	just above that, we have the tab completion list

	FTESurf Patch 211: a scrollable vertical DROPDOWN rather than a wrapped
	paragraph, at con_displaypossibilities 2.

	Three changes, and the shape of the first is what makes the rest cheap:

	 - ONE conline_t PER MATCH, linked by ->older, exactly the way Con_Footerf
	   already builds a multi-line footer. Con_BuildCompletion owns that loop --
	   and Patch 215 corrected its direction; the claim that used to stand here,
	   that pushing backwards put entry[first] at the top, was the reverse of what
	   the code did and is what made Up move the highlight down.
	 - a window of con_completionrows entries starting at con_completionscroll,
	   so a prefix with 200 matches scrolls instead of wrapping into a wall.
	 - at mode 2 it is drawn whenever there is anything to complete, not only
	   after Tab. con_commandmatch stays 0 until you actually navigate, and that
	   is deliberate: it is the same variable K_ENTER tests, so a list that merely
	   APPEARS cannot change what ENTER does to a line you typed in full.

	Mode 1 is build 26 byte for byte: gated on con_commandmatch, one line, tabs.

	Patch 215: at con_completiondown 1 the rows were already built and drawn near
	the top of this function, in the band BELOW the input line -- so this arm has
	nothing left to do and must not free the chain out from under them.
	*/
	/*
	FTESurf Patch 226: mode 2 owns the list outright -- Con_DrawCompletionPopup
	draws it, or nothing does. Returning unconditionally rather than only on
	drewdown covers the case where the popup's gate declined and this one would
	not have: con_commandmatch alone satisfies the test below, so an empty line
	carrying a stale highlight would draw a list INSIDE the console on the one
	frame the popup had nothing to say. Free the chain on the way out for the same
	reason -- nothing downstream is going to, and leaving it allocated makes last
	frame's rows eligible to reappear.
	*/
	if (con_completiondown.ival == 2)
	{
		if (con_popupcon != con)
			Con_FreeCompletion(con);
		return y;
	}
	if (drewdown)
		return y;
	if ((con_commandmatch || (con_displaypossibilities.ival >= 2 && text[0] && !(text[0] == '/' && !text[1]))) && con_displaypossibilities.value)
	{
		conchar_t *end, *s;
		const char *cmd;//, *desc;
		int cmdstart;
		size_t newlen;
		cmd_completion_t *c;
		qboolean dropdown = (con_displaypossibilities.ival >= 2);
		cmdstart = text[0] == '/'?1:0;
		end = maskedtext;

		c = Cmd_Complete(text+cmdstart, true);

		if (dropdown)
		{
			//FTESurf Patch 215: one speller for the rows -- see Con_BuildCompletion.
			//maxrows keeps a tall con_completionrows from drawing out through the
			//top of a short console, which top=0 used to allow.
			int maxrows = (y - (top>0?top:0)) / Font_CharHeight();
			if (maxrows > 0)
			{
				if (Con_BuildCompletion(con, text, maxrows) && con->completionline)
					y = Con_DrawConsoleLines(con, con->completionline, 0, left, right, y, top, selactive, selsx, selex, selsy, seley, 0);
			}
			else
				Con_FreeCompletion(con);
			return y;
		}

		if (!con->completionline || con->completionline->length + 512 > con->completionline->maxlength)
		{
			newlen = (con->completionline?con->completionline->length:0) + 2048;

			Con_FreeCompletion(con);
			con->completionline = Z_Malloc(sizeof(*con->completionline) + newlen*sizeof(conchar_t));
			con->completionline->maxlength = newlen;
		}
		con->completionline->length = 0;

		for (i = 0; i < c->num; i++)
		{
			int col = (con_commandmatch == i+1)?3:2;
			s = (conchar_t*)(con->completionline+1);

			//note: if cl_chatmode is 0, then we shouldn't show the leading /, however that is how the console link stuff recognises it as command text, so we always display it.
			cmd = c->completions[i].text;
//			desc = c->completions[i].desc;
//			if (desc)
//				end = COM_ParseFunString((COLOR_GREEN<<CON_FGSHIFT), va("^[^%i/%s\\tip\\%s^]\t", col, cmd, desc), s+con->completionline->length, (con->completionline->maxlength-con->completionline->length)*sizeof(maskedtext[0]), true);
//			else
				end = COM_ParseFunString((COLOR_GREEN<<CON_FGSHIFT), va("^[^%i/%s^]\t", col, cmd), s+con->completionline->length, (con->completionline->maxlength-con->completionline->length)*sizeof(maskedtext[0]), true);
			con->completionline->length = end - s;
		}
		if (c->extra)
		{
			s = (conchar_t*)(con->completionline+1);
			end = COM_ParseFunString((COLOR_WHITE<<CON_FGSHIFT), va("%u MORE", (unsigned)c->extra), s+con->completionline->length, (con->completionline->maxlength-con->completionline->length)*sizeof(maskedtext[0]), true);
			con->completionline->length = end - s;
		}

		if (con->completionline->length)
			y = Con_DrawConsoleLines(con, con->completionline, 0, left, right, y, 0, selactive, selsx, selex, selsy, seley, 0);
	}
	else if (con->completionline)
		Con_FreeCompletion(con);	//FTESurf Patch 211: the chain must not outlive the list

	return y;
}

/*
================
Con_DrawNotify

Draws the last few lines of output transparently over the game top
================
*/
void Con_DrawNotifyOne (console_t *con)
{
	struct font_s *notifyfont = (con == con_main) ? font_default : font_console;
	conchar_t *starts[NUM_CON_TIMES], *ends[NUM_CON_TIMES];
	float alphas[NUM_CON_TIMES], a;
	conchar_t *c;
	conline_t *l;
	int lines=con->notif_l;
	int line;
	int nx, y;
	int nw;
	int x;
	unsigned int codeflags, codepoint;

	int maxlines;
	float t;
	float hold, maxage;	//FTESurf Patch 242
	int i, step, stop;	//FTESurf Patch 242

	/*The main-console overlay is HUD text, not a miniature copy of the console.*/
	Font_BeginString(notifyfont, con->notif_x * vid.width, con->notif_y * vid.height, &nx, &y);
	Font_Transform(con->notif_w * vid.width, 0, &nw, NULL);

	if (con->notif_l < 0)
		con->notif_l = 0;
	if (con->notif_l > NUM_CON_TIMES)
		con->notif_l = NUM_CON_TIMES;
	lines = maxlines = con->notif_l;

	if (!con->notif_x && !con->notif_y && con->notif_w == 1)
		y = Con_DrawProgress(0, nw, 0);

	/*FTESurf Patch 242: the oldest a line of ANY kind can still be showing.
	  The walk below used to stop at the first expired line, which was sound
	  while every line aged at the same rate.  With con_notifytime_error it is
	  not: an ordinary line printed eight seconds ago is finished while an ERROR
	  printed ten seconds ago is not, and the walk meets the ordinary one first.
	  So an expired line no longer ends the walk -- it is skipped -- and this is
	  what still bounds it, because nothing older than this can be live whatever
	  its flags say.*/
	maxage = ((con->notif_t_err > con->notif_t) ? con->notif_t_err : con->notif_t) + con->notif_fade;

	l = con->current;
	if (!l->length)
		l = l->older;
	for (; l && lines > con->notif_l-maxlines; l = l->older)
	{
		if (l->flags & CONL_NONOTIFY)
			continue; //hidden from notify
		/*FTESurf Patch 242: errors and warnings hold longer.  The > 0 test is
		  what makes this safe for the chat console and the frag tracker, which
		  never set notif_t_err and would otherwise expire an error instantly.*/
		hold = ((l->flags & CONL_ERROR) && con->notif_t_err > 0) ? con->notif_t_err : con->notif_t;
		t = realtime - (l->time+hold);
		if (t > 0)
		{
			if (t > con->notif_fade)
			{
				l->flags |= CONL_NONOTIFY;
				if (realtime - l->time > maxage)
					break;
				continue;
			}
			a = 1 - (t/con->notif_fade);
		}
		else a = 1;

		line = Font_LineBreaks((conchar_t*)(l+1), (conchar_t*)(l+1)+l->length, nw, lines, starts, ends);
		if (!line && lines > 0)
		{
			lines--;
			starts[lines] = NULL;
			ends[lines] = NULL;
			alphas[lines] = a;
		}
		while(line --> 0 && lines > 0)
		{
			lines--;
			starts[lines] = starts[line];
			ends[lines] = ends[line];
			alphas[lines] = a;
		}
		if (lines == 0)
			break;
	}

	//clamp it properly
	while (lines < con->notif_l-maxlines)
	{
		lines++;
	}
	if (con->flags & CONF_NOTIFY_BOTTOM)
		y -= (con->notif_l - lines) * Font_CharHeight();

	/*FTESurf Patch 242: THE ORDER, which is the one thing this surface never had.

	  The gather above fills starts[] from the top of the array downwards while
	  walking newest -> oldest, so starts[lines] is the OLDEST line held and
	  starts[notif_l-1] is the NEWEST.  Drawing that range forwards with y
	  increasing therefore puts the oldest at the top and the newest at the
	  bottom -- classic Quake, and every notify surface in this engine.  It is
	  still style 0 and still the default for the chat console and the tracker.

	  Style 1 walks the same array BACKWARDS: newest at the top, older sliding
	  down beneath it.  The newest line is then always on the same row rather
	  than moving as the block fills, which is what makes a burst of errors
	  readable without reading it bottom-up.

	  Style 2 is style 0 with CONF_NOTIFY_BOTTOM, set for con_main in
	  Con_DrawNotify -- new lines arrive at the bottom and push older ones up,
	  anchored to the bottom of the block rather than the top.  The anchor
	  itself is the pre-existing line just above.

	  y is always advanced DOWNWARDS; only which end of the array is visited
	  first changes.  Nothing here needs to know how many lines there are.*/
	if (con->notif_style == 1)
	{
		i    = con->notif_l - 1;
		stop = lines - 1;
		step = -1;
	}
	else
	{
		i    = lines;
		stop = con->notif_l;
		step = 1;
	}

	for (; i != stop; i += step)
	{
		x = 0;
		R2D_ImageColours(1, 1, 1, alphas[i]);
		if (con->flags & CONF_NOTIFY_RIGHT)
		{
			for (c = starts[i]; c < ends[i]; )
			{
				c = Font_Decode(c, &codeflags, &codepoint);
				x += Font_CharWidth(codeflags, codepoint);
			}
			x = (nw - x);
		}
		else if (con_centernotify.value)
		{
			for (c = starts[i]; c < ends[i]; )
			{
				c = Font_Decode(c, &codeflags, &codepoint);
				x += Font_CharWidth(codeflags, codepoint);
			}
			x = (nw - x) / 2;
		}
		Font_LineDraw(nx+x, y, starts[i], ends[i]);

		y += Font_CharHeight();
	}

	Font_EndString(notifyfont);

	R2D_ImageColours(1,1,1,1);
}

void Con_ClearNotify(void)
{
	console_t *con;
	conline_t *l;
	for (con = con_head; con; con = con->next)
	{
		for (l = con->current; l; l = l->older)
			l->flags |= CONL_NONOTIFY;
	}
}
void Con_DrawNotify (void)
{
	extern int startuppending;
	console_t *con;

	if (con_main)
	{
		/*keep the main console up to date*/
		con_main->notif_l = con_numnotifylines.ival;
		con_main->notif_w = con_notify_w.value;
		con_main->notif_x = con_notify_x.value;
		con_main->notif_y = con_notify_y.value;
		con_main->notif_t = con_notifytime.value;
		/*Historically con_notifytime 0 disabled notifications immediately.*/
		con_main->notif_fade = con_notifytime.value > 0 ? max(0, con_notifyfade.value) : 0;

		/*FTESurf Patch 242.  0 means "no separate error hold", which is the
		  stock behaviour and the default; anything else is used for CONL_ERROR
		  lines only.  Not clamped upwards against con_notifytime on purpose --
		  a SHORTER error hold is a legitimate thing to want and refusing it
		  would be the cvar deciding it knows better.*/
		con_main->notif_t_err = con_notifytime_error.value > 0 ? con_notifytime_error.value : con_main->notif_t;

		/*FTESurf Patch 242.  Style 2 is the classic order with the block
		  anchored to its BOTTOM, which is what CONF_NOTIFY_BOTTOM already did
		  for the chat console -- so it is set here rather than reimplemented.
		  Assigned every frame, both ways, because the cvar can change at any
		  time and a flag that is only ever ORed in never comes back off.*/
		con_main->notif_style = con_notifystyle.ival;
		if (con_main->notif_style == 2)
			con_main->flags |= CONF_NOTIFY_BOTTOM;
		else
			con_main->flags &= ~CONF_NOTIFY_BOTTOM;
	}

	if (con_chat)
	{
		con_chat->notif_l = con_numnotifylines_chat.ival;
		con_chat->notif_w = 1;
		con_chat->notif_y = (vid.height - sb_lines - 8*4) / vid.width;
		con_chat->notif_t = con_notifytime_chat.value;
		con_chat->notif_t_err = con_chat->notif_t;	//FTESurf Patch 242: chat has no error lines.
	}

	if (startuppending)
	{
		int x,y;
		Font_BeginString(font_console, 0, 0, &x, &y);
		Con_DrawProgress(0, vid.width, 0);
		Font_EndString(font_console);
	}
	else
	{
		for (con = con_head; con; con = con->next)
		{
			/*The open floating main console already draws its complete scrollback.*/
			if ((con->flags & CONF_NOTIFY) &&
				!(con == con_main && Key_Dest_Has(kdm_cwindows) && con_curwindow == con_main))
				Con_DrawNotifyOne(con);
		}
	}

	if (Key_Dest_Has(kdm_message))
	{
		int x, y;
		conchar_t *starts[8];
		conchar_t *ends[8];
		conchar_t markup[MAXCMDLINE+64];
		conchar_t *c, *end;
		char demoji[8192];
		char *foo = va(chat_team?"say_team: %s":"say: %s", Key_Demoji(demoji, sizeof(demoji), chat_buffer?(char*)chat_buffer:""));
		int lines, i, pos;
		Font_BeginString(font_console, 0, 0, &x, &y);
		y = con_numnotifylines.ival * Font_CharHeight();

		i = chat_team?10:5;
		pos = strlen(foo)+i;
		pos = min(pos, chat_bufferpos + i);

		//figure out where the cursor is, if its safe
		i = foo[pos];
		foo[pos] = 0;
		c = COM_ParseFunString(CON_WHITEMASK, foo, markup, sizeof(markup), PFS_KEEPMARKUP|PFS_FORCEUTF8);
		foo[pos] = i;

		//k, build the string properly.
		end = COM_ParseFunString(CON_WHITEMASK, foo, markup, sizeof(markup) - sizeof(markup[0])-1, PFS_KEEPMARKUP | PFS_FORCEUTF8);

		//and overwrite the cursor so that it blinks.
		*end = ' '|CON_WHITEMASK;
		if (((int)(realtime*con_cursorspeed)&1))
			*c = 0xe00b|CON_WHITEMASK;
		if (c == end)
			end++;

		lines = Font_LineBreaks(markup, end, Font_ScreenWidth(), countof(starts), starts, ends);
		for (i = 0; i < lines; i++)
		{
			x = 0;
			Font_LineDraw(x, y, starts[i], ends[i]);
			y += Font_CharHeight();
		}
		Font_EndString(font_console);

		vid.ime_allow = true;
		vid.ime_position[0] = 0;
		vid.ime_position[1] = y;
	}
}

//send all the stuff that was con_printed to sys_print.
//This is so that system consoles in windows can scroll up and have all the text.
void Con_PrintToSys(void)
{
	console_t *curcon = con_main;
	conline_t *l;
	int i;
	conchar_t *t;
	char buf[16];

	if (!curcon)
		return;

	for (l = curcon->oldest; l; l = l->newer)
	{
		t = (conchar_t*)(l+1);
		//fixme: utf8?
		for (i = 0; i < l->length; i++)
		{
			if (!(t[i] & CON_HIDDEN))
			{
				if (com_parseutf8.ival>0)
				{
					int cl = utf8_encode(buf, t[i]&CON_CHARMASK, sizeof(buf)-1);
					if (cl)
					{
						buf[cl] = 0;
						Sys_Printf("%s", buf);
					}
				}
				else
					Sys_Printf("%c", t[i]&0xff);
			}
		}
		Sys_Printf("\n");
	}
}

//returns the bottom of the progress bar
static int Con_DrawProgress(int left, int right, int y)
{
	conchar_t			dlbar[1024], *chr;
	unsigned char	progresspercenttext[128];
	const char *progresstext = NULL;
	const char *txt;
	int x, tw;
	int i;
	int barwidth, barleft;
	float progresspercent = 0;
	unsigned int codeflags, codepoint;
	*progresspercenttext = 0;

	// draw the download bar
	// figure out width
	if (cls.download)
	{
		unsigned int count;
		qofs_t total;
		qboolean extra;
		progresstext = cls.download->localname;
		progresspercent = cls.download->percent;

		if (cls.download->sizeunknown && cls.download->size == 0)
			progresspercent = -1;

		CL_GetDownloadSizes(&count, &total, &extra);

		if (progresspercent < 0)
		{
			if ((int)(realtime/2)&1 || total == 0)
				sprintf(progresspercenttext, " (%ukB/s)", CL_DownloadRate()/1000);
			else
			{
				char tmp[64];
				sprintf(progresspercenttext, " (%s%s)", FS_AbbreviateSize(tmp,sizeof(tmp), total), extra?"+":"");
			}

			//do some marquee thing, so the user gets the impression that SOMETHING is happening.
			progresspercent = realtime - (int)realtime;
			if ((int)realtime & 1)
				progresspercent  = 1 - progresspercent;
			progresspercent *= 100;
		}
		else
		{
			if ((int)(realtime/2)&1 || total == 0)
				sprintf(progresspercenttext, " %5.1f%% (%ukB/s)", progresspercent, CL_DownloadRate()/1000);
			else
			{
				sprintf(progresspercenttext, " %5.1f%% (%u%sKiB)", progresspercent, (int)(total/1024), extra?"+":"");
			}
		}
	}
#ifdef RUNTIMELIGHTING
	else if ((progresstext=RelightGetProgress(&progresspercent)))
	{
		sprintf(progresspercenttext, " %02d%%", (int)progresspercent);
	}
#endif

	//at this point:
	//progresstext: what is being downloaded/done (can end up truncated)
	//progresspercent: its percentage (used only for the slider)
	//progresspercenttext: that percent as text, essentually the right hand part of the bar.

	if (progresstext)
	{
		//chop off any leading path
		if ((txt = strrchr(progresstext, '/')) != NULL)
			txt++;
		else
			txt = progresstext;

		x = 0;
		COM_ParseFunString(CON_WHITEMASK, txt, dlbar, sizeof(dlbar), false);
		for (i=0,chr = dlbar; *chr; )
		{
			chr = Font_Decode(chr, &codeflags, &codepoint);
			x += Font_CharWidth(codeflags, codepoint);
			i++;
		}

		//if the string is wider than a third of the screen
		if (x > (right - left)/3)
		{
			//truncate the file name and add ...
			x += 3*Font_CharWidth(CON_WHITEMASK, '.');
			while (x > (right - left)/3)
			{
				chr = Font_DecodeReverse(chr, dlbar, &codeflags, &codepoint);
				x -= Font_CharWidth(codeflags, codepoint);
			}

			dlbar[i++] = '.'|CON_WHITEMASK;
			dlbar[i++] = '.'|CON_WHITEMASK;
			dlbar[i++] = '.'|CON_WHITEMASK;
			dlbar[i] = 0;
		}

		//i is the char index of the dlbar so far, x is the char width of it.

		//add a couple chars
		dlbar[i] = ':'|CON_WHITEMASK;
		x += Font_CharWidth(CON_WHITEMASK, ':');
		i++;
		dlbar[i] = ' '|CON_WHITEMASK;
		x += Font_CharWidth(CON_WHITEMASK, ' ');
		i++;

		COM_ParseFunString(CON_WHITEMASK, progresspercenttext, dlbar+i, sizeof(dlbar)-i*sizeof(conchar_t), false);
		for (chr = &dlbar[i], tw = 0; *chr; )
		{
			chr = Font_Decode(chr, &codeflags, &codepoint);
			tw += Font_CharWidth(codeflags, codepoint);
		}

		barwidth = (right-left) - (x + tw);

		//draw the right hand side
		x = right - tw;
		for (chr = &dlbar[i]; *chr; )
		{
			chr = Font_Decode(chr, &codeflags, &codepoint);
			x = Font_DrawChar(x, y, codeflags, codepoint);
		}

		//draw the left hand side
		x = left;
		for (chr = dlbar; chr < &dlbar[i]; )
		{
			chr = Font_Decode(chr, &codeflags, &codepoint);
			x = Font_DrawChar(x, y, codeflags, codepoint);
		}

		//and in the middle we have lots of stuff

		barwidth -= (Font_CharWidth(CON_WHITEMASK, 0xe080) + Font_CharWidth(CON_WHITEMASK, 0xe082));
		x = Font_DrawChar(x, y, CON_WHITEMASK, 0xe080);
		barleft = x;
		for(;;)
		{
			if (x + Font_CharWidth(CON_WHITEMASK, 0xe081) > barleft+barwidth)
				break;
			x = Font_DrawChar(x, y, CON_WHITEMASK, 0xe081);
		}
		x = Font_DrawChar(x, y, CON_WHITEMASK, 0xe082);

		if (progresspercent >= 0)
			Font_DrawChar(barleft+(barwidth*progresspercent)/100 - Font_CharWidth(CON_WHITEMASK, 0xe083)/2, y, CON_WHITEMASK, 0xe083);

		y += Font_CharHeight();
	}
	return y;
}

//draws console selection choices at the top of the screen, if multiple consoles are available
//its ctrl+tab to switch between them
int Con_DrawAlternateConsoles(int lines)
{
	char *txt;
	int x, y = 0, lx;
	int consshown = 0;
	console_t *con, *om = con_mouseover;
	conchar_t buffer[512], *end, *start;
	unsigned int codeflags, codepoint;

	for (con = con_head; con; con = con->next)
	{
		if (!(con->flags & (CONF_HIDDEN|CONF_ISWINDOW)))
			consshown++;
	}

	if (lines == (int)scr_con_target && consshown > 1)
	{
		int mx, my, h;
		Font_BeginString(font_console, mousecursor_x, mousecursor_y, &mx, &my);
		Font_BeginString(font_console, 0, y, &x, &y);
		h = Font_CharHeight();
		for (x = 0, con = con_head; con; con = con->next)
		{
			if (con->flags & (CONF_HIDDEN|CONF_ISWINDOW))
				continue;
			txt = con->title;

			//yeah, om is an evil 1-frame delay. whatever
			end = COM_ParseFunString(CON_WHITEMASK, va("^&%c%i%s", ((con!=om)?'F':'B'), (con==con_current)+con->unseentext*4, txt), buffer, sizeof(buffer), false);

			lx = 0;
			for (lx = x, start = buffer; start < end; )
			{
				start = Font_Decode(start, &codeflags, &codepoint);
				lx = Font_CharEndCoord(font_console, lx, codeflags, codepoint);
			}
			if (lx > Font_ScreenWidth())
			{
				x = 0;
				y += h;
			}
			for (lx = x, start = buffer; start < end; )
			{
				start = Font_Decode(start, &codeflags, &codepoint);
				lx = Font_DrawChar(lx, y, codeflags, codepoint);
			}
			lx += 8;
			if (mx >= x && mx < lx && my >= y && my < y+h)
				con_mouseover = con;
			x = lx;
		}
		y+= h;
		Font_EndString(font_console);

		y = (y*(int)vid.height) / (float)vid.rotpixelheight;
	}
	return y;
}

static void Con_DrawImageClip(float x, float y, float w, float h, float bottom, shader_t *pic)
{
	if (bottom < y+h)
	{
		if (bottom <= y)
			return;
		R2D_Image(x,y,w,bottom-y,0,0,1,(bottom-y)/h,pic);
	}
	else
		R2D_Image(x,y,w,h,0,0,1,1,pic);
}

//draws the conline_t list bottom-up within the width of the screen until the top of the screen is reached.
//if text is selected, the selstartline globals will be updated, so make sure the lines persist or check them.
static int Con_DrawConsoleLines(console_t *con, conline_t *l, float displayscroll, int sx, int ex, int y, int top, int selactive, int selsx, int selex, int selsy, int seley, float lineagelimit)
{
	int linecount;
	conchar_t *starts[64], *ends[sizeof(starts)/sizeof(starts[0])];
	conchar_t *s, *e, *c;
	int x;
	int charh = Font_CharHeight();
	unsigned int codeflags, codepoint;
	float alphaval = 1;
	float chop;

	chop = displayscroll * Font_CharHeight();

	if (l != con->completionline)
	if (l != con->footerline)
	if (l != con->current)
	{
		y -= Font_CharHeight();
	// draw arrows to show the buffer is backscrolled
		for (x = sx ; x<ex; )
			x = (Font_DrawChar (x, y, CON_WHITEMASK, '^')-x)*4+x;

		if (chop)
		{
			y -= Font_CharHeight();
			chop += 2*Font_CharHeight();
		}
	}

	y += chop;

	if (selactive != -1)
	{
		if (!selactive)
			selactive = 2;	//calculate, but don't draw (to track mouse-over)

		//deactivate the selection if the start and end is outside
		if (
			(selsx < sx && selex < sx) ||
			(selsx > ex && selex > ex) ||
			(selsy < top && seley < top) ||
			(selsy > y && seley > y)
			)
			selactive = false;	//don't track it at all

		if (selactive)
		{
			//clip it
			if (selsx < sx)
				selsx = sx;
			if (selex < sx)
				selex = sx;

			if (selsy > y)
				selsy = y;
			if (seley > y)
				seley = y;

			//scale the y coord to be in lines instead of pixels
			selsy -= y;
			seley -= y;
	//		selsy -= charh;
	//		seley -= charh;

			//invert the selections to make sense, text-wise
			/*if (selsy == seley)
			{
				//single line selected backwards
				if (selex < selsx)
				{
					x = selex;
					selex = selsx;
					selsx = x;
				}
			}
			*/
			if (seley <= selsy)
			{	//selection goes upwards
				x = selsy;
				selsy = seley;
				seley = x;

				x = selex;
				selex = selsx;
				selsx = x;
				con->flags &= ~CONF_BACKSELECTION;
			}
			else
				con->flags |= CONF_BACKSELECTION;
	//		selsy *= Font_CharHeight();
	//		seley *= Font_CharHeight();
			selsy += y;
			seley += y;
		}
	}

	if (l && l == con->current && l->length == 0 && con->userline != l)
		l = l->older;
	for (; l; l = l->older)
	{
		shader_t *pic = NULL;
		float picw=0, pich=0;
		s = (conchar_t*)(l+1);

		if (lineagelimit)
		{
			alphaval = realtime - (l->time+lineagelimit);
			if (alphaval > 0)
			{
				float fadetime = con->notif_fade?con->notif_fade:1;
				if (alphaval > fadetime)
					break;	//we're done here
				alphaval = 1 - (alphaval/fadetime);
			}
			else
				alphaval = 1;
		}

		if (l->length >= 2 && *s == CON_LINKSTART && (s[1]&CON_CHARMASK) == '\\')
		{	//leading tag with no text, look for an image in there
			conchar_t *e;
			char linkinfo[256];
			int linkinfolen = 0;
			for (e = s+1; e < s+l->length; e++)
			{
				if (*e == CON_LINKEND)
				{
					char *imgname;
					linkinfo[linkinfolen] = 0;

					imgname = Info_ValueForKey(linkinfo, "imgptr");
					if (*imgname)
					{
						image_t *img = Image_TextureIsValid(strtoull(imgname, NULL, 0));
						if (img && (img->flags & IF_TEXTYPEMASK)==IF_TEXTYPE_CUBE)
						{
							pic = R_RegisterShader("tiprawimgcube", 0, "{\nprogram postproc_equirectangular\n{\nmap \"$cube:$reflectcube\"\n}\n}");
							pic->defaulttextures->reflectcube = img;
						}
						else if (img && (img->flags & IF_TEXTYPEMASK)==IF_TEXTYPE_2D_ARRAY)
						{
							pic = R_RegisterShader("tiprawimgarray", 0, "{\nprogram default2danim\n{\nmap \"$2darray:$diffuse\"\n}\n}");
							pic->defaulttextures->base = img;
						}
						else
						{
							pic = R2D_SafeCachePic("tiprawimg");
							pic->defaulttextures->base = img;
						}
						if (img)
						{
							if (!img->width || !img->height || !TEXLOADED(img))
								picw = pich = 64;
							else if (img->width > img->height)
							{
								picw = 64;
								pich = (64.0*img->height)/img->width;
							}
							else
							{
								picw = (64.0*img->width)/img->height;
								pich = 64;
							}
							break;
						}
					}


					imgname = Info_ValueForKey(linkinfo, "img");
					if (*imgname)
					{
						char *fl = Info_ValueForKey(linkinfo, "imgtype");
						if (*fl)
							pic = R_RegisterCustom(NULL, imgname, atoi(fl), NULL, NULL);
						else
							pic = R_RegisterPic(imgname, NULL);
						if (pic)
						{
							imgname = Info_ValueForKey(linkinfo, "s");
							if (*imgname)
							{
								if (pic->width <= 0 || pic->height <= 0)
									picw = pich = 64;
								else if (pic->width > pic->height)
								{
									picw = atof(imgname);
									pich = picw * (float)pic->height/pic->width;
								}
								else
								{
									pich = atof(imgname);
									picw = pich * (float)pic->width/pic->height;
								}
							}
							else
							{
								imgname = Info_ValueForKey(linkinfo, "w");
								if (*imgname)
									picw = atof(imgname);
								else
									picw = -1;
								imgname = Info_ValueForKey(linkinfo, "h");
								if (*imgname)
									pich = atof(imgname);
								else
									pich = -1;

								if (picw<0 && pich<0)
								{
									if (pic->width && pic->height)
									{
										pich = (pic->height * vid.pixelheight) / vid.height;
										picw = (pic->width * vid.pixelwidth) / vid.width;
									}
									else
										picw = pich = 64;
								}
								else if (picw<0)
									picw = pich * (float)pic->width/pic->height;
								else if (pich<0)
									pich = picw * (float)pic->height/pic->width;
							}
							picw *= charh/8.0;
							pich *= charh/8.0;

							if (picw >= ex-sx)
							{
								pich *= (float)(ex-sx) / picw;
								picw = ex-sx;
							}
						}

						//a fall back image (mostly for delay-loading or whatever.
						if (R_GetShaderSizes(pic, NULL, NULL, false) <= 0)
						{
							imgname = Info_ValueForKey(linkinfo, "fbimg");
							if (*imgname)
								pic = R_RegisterPic(imgname, NULL);
						}
					}
					break;
				}
				linkinfolen += unicode_encode(linkinfo+linkinfolen, (*e & CON_CHARMASK), sizeof(linkinfo)-1-linkinfolen, true);
			}
		}

		if (con->flags & CONF_NOWRAP)
		{
			linecount = 1;
			starts[0] = s;
			ends[0] = s+l->length;
		}
		else
		{
			linecount = Font_LineBreaks(s, s+l->length, ex-sx-picw, sizeof(starts)/sizeof(starts[0]), starts, ends);
			//if Con_LineBreaks didn't find any lines at all, then it was an empty line, and we need to ensure that its still drawn
			if (linecount == 0 && !pic)
			{
				linecount = 1;
				starts[0] = ends[0] = s;
			}
		}

		if (pic)
		{
			float szx = (float)vid.width / vid.pixelwidth;
			float szy = (float)vid.height / vid.pixelheight;
			int texth = (linecount) * Font_CharHeight();
			if (R2D_Flush)
				R2D_Flush();
			R2D_ImageColours(1.0, 1.0, 1.0, 1.0);
			if (texth > pich)
			{
				texth = pich + (texth-pich)/2;
				Con_DrawImageClip(sx*szx, (y-texth)*szy, picw*szx, pich*szy, (y-chop+Font_CharHeight())*szy, pic);
				pich = 0;	//don't pad the text...
			}
			else
			{
				Con_DrawImageClip(sx*szx, (y-pich)*szy, picw*szx, pich*szy, (y-chop+Font_CharHeight())*szy, pic);
				pich -= texth;
				y-= pich/2;	//skip some space above and below the text block, to keep the text and image aligned.

				if (chop)
					chop -= pich/2;
			}
			if (R2D_Flush)
				R2D_Flush();

//			if (selsx < picw && selex < picw)

			l->numlines = ceil((texth+pich)/Font_CharHeight());
		}
		else
			l->numlines = linecount;

		while(linecount-- > 0)
		{
			s = starts[linecount];
			e = ends[linecount];

			y -= Font_CharHeight();

			if (chop)
			{
				chop -= Font_CharHeight();
				if (chop < 0)
					chop = 0;
				else
					continue;
			}

			if (top && y < top)
				break;

			if (l->flags & (CONL_BREAKPOINT|CONL_EXECUTION))
			{
				if (l->flags & CONL_EXECUTION)
				{
					if (l->flags & CONL_BREAKPOINT)
						R2D_ImageColours(SRGBA(0.3,0.15,0.0, alphaval));
					else
						R2D_ImageColours(SRGBA(0.3,0.3,0.0, alphaval));
				}
				else //if (l->flags & CONL_BREAKPOINT)
					R2D_ImageColours(SRGBA(0.3,0.0,0.0, alphaval));
				R2D_FillBlock((sx*(float)vid.width)/(float)vid.rotpixelwidth, (y*vid.height)/(float)vid.rotpixelheight, ((ex - sx)*vid.width)/(float)vid.rotpixelwidth, (Font_CharHeight()*vid.height)/(float)vid.rotpixelheight);
				R2D_Flush();
			}

			if (selactive < 0)
			{	//display an existing selection
				int sstart = picw;
				int send = sstart;
				int center;
				if (selactive == -2 || l == con->selendline || l == con->selstartline)
				{
					for (c = s; c < e; )
					{
						c = Font_Decode(c, &codeflags, &codepoint);
						send = Font_CharEndCoord(font_console, send, codeflags, codepoint);
					}
					//show something on blank lines
					if (send == sstart)
						send = Font_CharEndCoord(font_console, send, CON_WHITEMASK, ' ');

					center = sx;
					if (l->flags&CONL_CENTERED)
						center += ((ex-sx) - send)/2;
				
					if (l == con->selendline)
					{
						selactive = -2;	//all following lines need to be selected, until we see the other end of the selection
						send = sstart;
						for (c = s; c < (conchar_t*)(con->selendline+1)+con->selendoffset; )
						{
							c = Font_Decode(c, &codeflags, &codepoint);
							send = Font_CharEndCoord(font_console, send, codeflags, codepoint);
						}
					}
					if (l == con->selstartline)
					{
						for (c = s; c < (conchar_t*)(con->selstartline+1)+con->selstartoffset; )
						{
							c = Font_Decode(c, &codeflags, &codepoint);
							sstart = Font_CharEndCoord(font_console, sstart, codeflags, codepoint);
						}
						if (c == (conchar_t*)(con->selstartline+1)+con->selstartoffset)
							selactive = 0;	//no need to track any other selections.
					}

					sstart += center;
					send += center;

					R2D_ImageColours(SRGBA(0.1,0.1,0.3, alphaval));
					if (send < sstart)
						R2D_FillBlock((send*(float)vid.width)/(float)vid.rotpixelwidth, (y*vid.height)/(float)vid.rotpixelheight, ((sstart - send)*vid.width)/(float)vid.rotpixelwidth, (Font_CharHeight()*vid.height)/(float)vid.rotpixelheight);
					else
						R2D_FillBlock((sstart*(float)vid.width)/(float)vid.rotpixelwidth, (y*vid.height)/(float)vid.rotpixelheight, ((send - sstart)*vid.width)/(float)vid.rotpixelwidth, (Font_CharHeight()*vid.height)/(float)vid.rotpixelheight);
					R2D_Flush();
				}
			}
			else if (selactive)
			{
				if (y+charh >= selsy)
				{
					if (y < seley)
					{
						int sstart;
						int send;
						int center;
						send = sstart = picw;
						for (c = s; c < e; )
						{
							c = Font_Decode(c, &codeflags, &codepoint);
							send = Font_CharEndCoord(font_console, send, codeflags, codepoint);
						}

						//show something on blank lines
						if (send == sstart)
							send = Font_CharEndCoord(font_console, send, CON_WHITEMASK, ' ');

						center = sx;
						if (l->flags&CONL_CENTERED)
							center += ((ex-sx) - send)/2;

						if (y+charh >= seley && y < selsy)
						{	//if they're both on the same line, make sure sx is to the left of ex, so our stuff makes sense
							if (selex < selsx)
							{
								x = selex;
								selex = selsx;
								selsx = x;
							}
						}

						if (y+charh >= seley)
						{
							send = sstart;
							for (c = s; c < e; )
							{
								c = Font_Decode(c, &codeflags, &codepoint);
								send = Font_CharEndCoord(font_console, send, codeflags, codepoint);

								if (send+center > selex)
									break;
							}

							con->selendline = l;
							if (s)
								con->selendoffset = c - (conchar_t*)(l+1);
							else
								con->selendoffset = 0;
						}
						if (y < selsy)
						{
							for (c = s; c < e; )
							{
								Font_Decode(c, &codeflags, &codepoint);
								x = Font_CharEndCoord(font_console, sstart, codeflags, codepoint);
								if (x+center > selsx)
									break;
								c = Font_Decode(c, &codeflags, &codepoint);
								sstart = x;
							}

							con->selstartline = l;
							if (s)
								con->selstartoffset = c - (conchar_t*)(l+1);
							else
								con->selstartoffset = 0;

							if (selactive == 2 && s)
							{	//checking for mouseover
								//scan earlier to find any link enclosure
								for(c--; c >= (conchar_t*)(l+1); c--)
								{
									if (*c == CON_LINKSTART)
									{
										selactive = 3;	//we're mouse-overing a link!
										con->selstartoffset = c - (conchar_t*)(l+1);
										break;
									}
									if (*c == CON_LINKEND)
										break;	//some other link ended here. don't use its start.
								}

								if (selactive == 3 && con->selendline==l)
								{
									for (; c < (conchar_t*)(l+1)+l->length; c++)
										if (*c == CON_LINKEND)
										{
											con->selendoffset = c - (conchar_t*)(l+1);
											break;
										}

									sstart = picw;
									for (c = s; c < (conchar_t*)(l+1)+con->selstartoffset; )
									{
										c = Font_Decode(c, &codeflags, &codepoint);
										sstart = Font_CharEndCoord(font_console, sstart, codeflags, codepoint);
									}
									send = sstart;
									for (; c < (conchar_t*)(l+1)+con->selendoffset; )
									{
										c = Font_Decode(c, &codeflags, &codepoint);
										send = Font_CharEndCoord(font_console, send, codeflags, codepoint);
									}
								}
							}
						}

						sstart += center;
						send += center;

						if (selactive != 2)
						{
							if (selactive == 1)
								R2D_ImageColours(SRGBA(0.1,0.1,0.3, alphaval));	//selected
							else
								R2D_ImageColours(SRGBA(0.3,0.3,0.3, alphaval));	//mouseover.

							if (send < sstart)
							{
								center = sstart;
								sstart = send;
								send = center;
							}
							if (selactive == 3)	//2 pixels high
								R2D_FillBlock((sstart*vid.width)/(float)vid.rotpixelwidth, ((y+Font_CharHeight()-2)*vid.height)/(float)vid.rotpixelheight, ((send - sstart)*vid.width)/(float)vid.rotpixelwidth, (2*vid.height)/(float)vid.rotpixelheight);
							else				//full height
								R2D_FillBlock((sstart*vid.width)/(float)vid.rotpixelwidth, (y*vid.height)/(float)vid.rotpixelheight, ((send - sstart)*vid.width)/(float)vid.rotpixelwidth, (Font_CharHeight()*vid.height)/(float)vid.rotpixelheight);
							R2D_Flush();
						}
					}
				}
			}
			R2D_ImageColours(1.0, 1.0, 1.0, alphaval);

			x = sx + picw;

			if (l->flags&CONL_CENTERED)
			{
				int send = 0;
				for (c = s; c < e; )
				{
					c = Font_Decode(c, &codeflags, &codepoint);
					send = Font_CharEndCoord(font_console, send, codeflags, codepoint);
				}

				x += ((ex-sx) - send)/2;
			}

			Font_LineDraw(x, y, s, e);


			if (con->userline == l && s <= (conchar_t*)(l+1)+con->useroffset && (conchar_t*)(l+1)+con->useroffset <= e)
			if ((int)(realtime*4)&1)
			{
				int sstart;
				sstart = picw;
				for (c = s; c < (conchar_t*)(l+1)+con->useroffset; )
				{
					c = Font_Decode(c, &codeflags, &codepoint);
					sstart = Font_CharEndCoord(font_console, sstart, codeflags, codepoint);
				}
				Font_DrawChar(sx+sstart, y, CON_WHITEMASK, 0xe00b);
			}

			if (y < top)
				break;
		}
		y -= pich/2;
		if (chop)
			chop -= pich/2;
		if (y < top)
			break;
	}
	return y;
}

void Draw_ExpandedString(float x, float y, conchar_t *str);

static void Con_DrawModelPreview(model_t *model, float x, float y, float w, float h)
{
	playerview_t pv;
	entity_t ent;
	vec3_t fwd, rgt, up;
	vec3_t lightpos = {1, 1, 0};
	float transforms[12];
	float scale;

	if (R2D_Flush)
		R2D_Flush();

	memset(&pv, 0, sizeof(pv));

	CL_DecayLights ();
	CL_ClearEntityLists();
	V_ClearRefdef(&pv);
	r_refdef.drawsbar = false;
	V_CalcRefdef(&pv);

	r_refdef.grect.width = w;
	r_refdef.grect.height = h;
	r_refdef.grect.x = x;
	r_refdef.grect.y = y;
	r_refdef.time = realtime;

	r_refdef.flags = RDF_NOWORLDMODEL;

	r_refdef.afov = 60;
	r_refdef.fov_x = 0;
	r_refdef.fov_y = 0;
	r_refdef.dirty |= RDFD_FOV;

	VectorClear(r_refdef.viewangles);
	r_refdef.viewangles[0] = 20;
//	r_refdef.viewangles[1] = realtime * 90;
	AngleVectors(r_refdef.viewangles, fwd, rgt, up);
	VectorScale(fwd, -64, r_refdef.vieworg);

	memset(&ent, 0, sizeof(ent));
	ent.model = model;
	ent.scale = 1;
	ent.angles[1] = realtime*90;//mods->yaw;
//	ent.angles[0] = realtime*23.4;//mods->pitch;
	AngleVectorsMesh(ent.angles, ent.axis[0], ent.axis[1], ent.axis[2]);
	VectorInverse(ent.axis[1]);

	//ent.origin[2] -= (ent.model->maxs[2]-ent.model->mins[2]) * 0.5 + ent.model->mins[2];

	ent.scale = 1;
	scale = max(max(fabs(ent.model->maxs[0]-ent.model->mins[0]), fabs(ent.model->maxs[1]-ent.model->mins[1])), fabs(ent.model->maxs[2]-ent.model->mins[2]));
	scale = scale?64.0/scale:1;
	ent.origin[2] -= (ent.model->maxs[2]-ent.model->mins[2]) * 0.5 + ent.model->mins[2];
	Vector4Set(ent.shaderRGBAf, 1, 1, 1, 1);
	VectorScale(ent.axis[0], scale, ent.axis[0]);
	VectorScale(ent.axis[1], scale, ent.axis[1]);
	VectorScale(ent.axis[2], scale, ent.axis[2]);
	ent.topcolour = TOP_DEFAULT;
	ent.bottomcolour = BOTTOM_DEFAULT;
//	ent.fatness = sin(realtime)*5;
	ent.playerindex = -1;
	ent.skinnum = 0;
	ent.shaderTime = 0;//realtime;
	ent.framestate.g[FS_REG].lerpweight[0] = 1;
	ent.framestate.g[FS_REG].frametime[0] = ent.framestate.g[FS_REG].frametime[1] = realtime;
	ent.framestate.g[FS_REG].endbone = 0x7fffffff;
	if (model->submodelof)
		;
	else
	{
		ent.customskin = Mod_RegisterSkinFile(va("%s_0.skin", model->publicname));
		if (ent.customskin == 0)
		{
			char haxxor[MAX_QPATH];
			COM_StripExtension(model->publicname, haxxor, sizeof(haxxor));
			ent.customskin = Mod_RegisterSkinFile(va("%s_default.skin", haxxor));
		}
	}

	Vector4Set(ent.shaderRGBAf, 1,1,1,1);
	VectorSet(ent.glowmod, 1,1,1);
	ent.light_avg[0] = ent.light_avg[1] = ent.light_avg[2] = 0.66;
	ent.light_range[0] = ent.light_range[1] = ent.light_range[2] = 0.33;

	V_ApplyRefdef();

	if (ent.model->camerabone>0 && Mod_GetTag(ent.model, ent.model->camerabone, &ent.framestate, transforms))
	{
		VectorClear(ent.origin);
		AngleVectorsMesh(ent.angles, ent.axis[0], ent.axis[1], ent.axis[2]);
		VectorInverse(ent.axis[1]);
		scale = 1;
		{
			vec3_t fwd, up;
			float camera[12], et[12] = {
				ent.axis[0][0], ent.axis[1][0], ent.axis[2][0], ent.origin[0],
				ent.axis[0][1], ent.axis[1][1], ent.axis[2][1], ent.origin[1],
				ent.axis[0][2], ent.axis[1][2], ent.axis[2][2], ent.origin[2],
				};

			R_ConcatTransforms((void*)et, (void*)transforms, (void*)camera);
			VectorSet(fwd, camera[2], camera[6], camera[10]);
			VectorNegate(fwd, fwd);
			VectorSet(up, camera[1], camera[5], camera[9]);
			VectorSet(r_refdef.vieworg, camera[3], camera[7], camera[11]);
			VectorAngles(fwd, up, r_refdef.viewangles, false);
		}
	}
	else
	{
		ent.angles[1] = realtime*90;//mods->yaw;
		AngleVectorsMesh(ent.angles, ent.axis[0], ent.axis[1], ent.axis[2]);
		VectorScale(ent.axis[0], scale, ent.axis[0]);
		VectorScale(ent.axis[1], -scale, ent.axis[1]);
		VectorScale(ent.axis[2], scale, ent.axis[2]);
	}

	ent.scale = scale;

	VectorNormalize(lightpos);
	ent.light_dir[0] = DotProduct(lightpos, ent.axis[0]);
	ent.light_dir[1] = DotProduct(lightpos, ent.axis[1]);
	ent.light_dir[2] = DotProduct(lightpos, ent.axis[2]);

	ent.light_known = 2;

	V_AddEntity(&ent);

	R_RenderView();
}

static void Con_DrawMouseOver(console_t *mouseconsole)
{
	char *tiptext = NULL;
	shader_t *shader = NULL;
	model_t *model = NULL;
	sfx_t	*audio = NULL;

	char *mouseover;
	if (!mouseconsole->mouseover || !mouseconsole->mouseover(mouseconsole, &tiptext, &shader))
	{
		mouseover = Con_CopyConsole(mouseconsole, false, true, true);
		if (mouseover)
		{
			char *end = strstr(mouseover, "^]");
			char *info = strchr(mouseover, '\\');
			if (!info)
				info = "";
			if (end)
				*end = 0;
#ifdef PLUGINS
			if (!Plug_ConsoleLinkMouseOver(mousecursor_x, mousecursor_y, mouseover+2, info))
#endif
			{
				char *key;
				key = Info_ValueForKey(info, "tipimg");
				if (*key)
				{
					char *fl = Info_ValueForKey(info, "tipimgtype");
					if (*fl)
						shader = R_RegisterCustom(NULL, key, atoi(fl), NULL, NULL);
					else
						shader = R2D_SafeCachePic(key);
				}
				else
				{
					image_t *img = NULL;
					key = Info_ValueForKey(info, "tiprawimg");
					if (*key)
					{
						img = Image_FindTexture(key, NULL, IF_NOREPLACE|IF_PREMULTIPLYALPHA|IF_TEXTYPE_ANY);
						if (!img)
							img = Image_FindTexture(key, NULL, IF_NOREPLACE|IF_TEXTYPE_ANY);
						if (!img)
						{
							size_t fsize;
							char *buf;
							img = Image_CreateTexture(key, NULL, IF_NOREPLACE|IF_PREMULTIPLYALPHA|IF_TEXTYPE_ANY);
							if ((buf = FS_LoadMallocFile (key, &fsize)))
								Image_LoadTextureFromMemory(img, img->flags|IF_NOWORKER, key, key, buf, fsize);
						}
					}

					key = Info_ValueForKey(info, "tipimgptr");
					if (*key)
						img = Image_TextureIsValid(strtoull(key, NULL, 0));
					if (img && img->status == TEX_LOADED)
					{
						if ((img->flags & IF_TEXTYPEMASK)==IF_TEXTYPE_CUBE)
						{
							shader = R_RegisterShader("tiprawimgcube", 0, "{\nprogram postproc_equirectangular\n{\nmap \"$cube:$reflectcube\"\n}\n}");
							shader->defaulttextures->reflectcube = img;
						}
						else if ((img->flags & IF_TEXTYPEMASK)==IF_TEXTYPE_2D_ARRAY)
						{
							shader = R_RegisterShader("tiprawimgarray", 0, "{\nprogram default2danim\n{\nmap \"$2darray:$diffuse\"\n}\n}");
							shader->defaulttextures->base = img;
						}
						else if ((img->flags&IF_TEXTYPEMASK) == IF_TEXTYPE_2D)
						{
							shader = R2D_SafeCachePic("tiprawimg");
							shader->defaulttextures->base = img;
						}

						if (shader)
						{
							shader->width = img->width;
							shader->height = img->height;
							if (shader->width > 320)
							{
								shader->height *= 320.0/shader->width;
								shader->width = 320;
							}
							if (shader->height > 240)
							{
								shader->width *= 240.0/shader->height;
								shader->height = 240;
							}
						}
					}
					else
						shader = NULL;
					if (!vrui.enabled)
					{
						key = Info_ValueForKey(info, "modelviewer");
						if (*key)
						{
							model = Mod_ForName(key, MLV_WARN);
							if (model->loadstate != MLS_LOADED)
								model = NULL;
						}
					}

					key = Info_ValueForKey(info, "playaudio");
					if (*key)
					{
						audio = S_PrecacheSound(key);
						if (audio && audio->loadstate != SLS_LOADED)
							audio = NULL;
					}
				}
				tiptext = Info_ValueForKey(info, "tip");
			}
			Z_Free(mouseover);
		}
	}
	if ((tiptext && *tiptext) || shader || model || audio)
	{
		//FIXME: draw a proper background.
		//FIXME: support line breaks.
		conchar_t buffer[2048], *starts[64], *ends[countof(starts)], *eot;
		int lines, i, px, py;
		float tw, th;
		float ih = 0, iw = 0;
		float x = mousecursor_x+8;
		float y = mousecursor_y+8;

		Font_BeginString(font_console, x, y, &px, &py);
		eot = COM_ParseFunString(CON_WHITEMASK, tiptext, buffer, sizeof(buffer), false);
		if (audio)
		{
			struct sfxcache_s cache;
			char name[MAX_OSPATH];
			float len;
			*name = 0;
			len = audio->decoder.querydata?audio->decoder.querydata(audio, &cache, name, sizeof(name)):-1;
			if (len >= 0)
			{
				eot = COM_ParseFunString(CON_WHITEMASK, va("\n\n%s\n%gkhz, %s, %ibit, %g seconds%s",
							name, cache.speed/1000.0, cache.numchannels==1?"mono":"stereo", QAF_BYTES(cache.format)*8, len, audio->loopstart>=0?" looped":""
							), eot, sizeof(buffer)-((char*)eot-(char*)buffer), false);
			}
			else
			{
				cache = *(struct sfxcache_s *)audio->decoder.buf;
				len = (double)cache.length / cache.speed;
				eot = COM_ParseFunString(CON_WHITEMASK, va("\n\n\n%gkhz, %s, %ibit, %g seconds%s",
							cache.speed/1000.0, cache.numchannels==1?"mono":"stereo", QAF_BYTES(cache.format)*8, len, audio->loopstart>=0?" looped":""
							), eot, sizeof(buffer)-((char*)eot-(char*)buffer), false);
			}
		}
		lines = Font_LineBreaks(buffer, eot, (256.0 * vid.pixelwidth) / vid.width, countof(starts), starts, ends);
		th = (Font_CharHeight()*lines * vid.height) / vid.pixelheight;

		if (model)
		{
			iw = 128;
			ih = 128;
		}
		else if (shader)
		{
			int w, h;
			if (R_GetShaderSizes(shader, &w, &h, false) >= 0)
			{
				iw = w;
				ih = h;
			}
			else
				shader = NULL;
		}
		if (iw  > (vid.width/4.0))
		{
			ih *= (vid.width/4.0)/iw;
			iw *= (vid.width/4.0)/iw;
		}
		if (ih  > (vid.height/4.0))
		{
			iw *= (vid.width/4.0)/ih;
			ih *= (vid.width/4.0)/ih;
		}

		if (x + iw/2 + 8 + 256 > vid.width)
			x = vid.width - (iw/2 + 8 + 256);
		if (x < iw/2)
			x = iw/2;
		x += iw/2 + 8;

		if (y+max(th, ih) > vid.height)
			y = mousecursor_y - 8 - max(th, ih);
		if (y < 0)
			y = 0;

		Font_BeginString(font_console, x, y + (max(th, ih) - th)/2, &px, &py);
		for (i = 0, tw = 0; i < lines; i++)
		{
			int lw = Font_LineWidth(starts[i], ends[i]);
			if (lw > tw)
				tw = lw;
		}
		tw *= (float)vid.width / vid.pixelwidth;
		Font_EndString(font_console);
		R2D_ImageColours(0, 0, 0, .75);
		R2D_FillBlock(x, y + (max(th, ih) - th)/2, tw, th);
		R2D_ImageColours(1, 1, 1, 1);
		Font_BeginString(font_console, x, y + (max(th, ih) - th)/2, &px, &py);
		for (i = 0; i < lines; i++)
		{
			Font_LineDraw(px, py, starts[i], ends[i]);
			py += Font_CharHeight();
		}
		Font_EndString(font_console);

		if (model)
			Con_DrawModelPreview(model, x-8-iw, y+((th>ih)?(th-ih)/2:0), iw, ih);
		if (shader)
		{
			if (th > ih)
				y += (th-ih)/2;
			R2D_Image(x-8-iw, y, iw, ih, 0, 0, 1, 1, shader);
		}
	}
}

/*
================
Con_DrawConsole

Draws the console with the solid background
================
*/
/*
FTESurf Patch 226: paint the floating completion popup (con_completiondown 2).

CALLED FROM INSIDE THE CONSOLE'S OWN DRAW, not once at the end of
Con_DrawConsole, and that is not a preference. Con_DrawOneConsole clears
con->selstartline at the top of every frame so a stale drag cannot survive, and
reads it again at the bottom to turn a release into a link activation. A popup
painted after all of that would set selstartline just in time for the next frame
to clear it, and its rows could never be clicked -- which is how you FIND a map.
Painted here it is inside the same brackets as the console's own rows, and
clicking works for the same reason it already works for them.

The cost is that the popup is painted before any LATER console window in the
loop. A list can only belong to a FOCUSED console -- Con_DrawInput returns before
the completion arms otherwise -- so the case where that is visible needs two
focused consoles, which is not a state this engine has.
*/
static void Con_DrawCompletionPopup(console_t *con)
{
	int ch, h, w, top, bot;
	int selsx, selsy, selex, seley;
	unsigned int oldflags;
	float back[3], accent[3];
	float sxv, syv, vx, vy, vw, vh;

	if (con_popupcon != con || !con || !con->completionline || con_popuprows < 1)
	{
		if (con_popupshown == con)
			con_popupshown = NULL;
		return;
	}
	con_popupcon = NULL;	//one paint per record

	ch = Font_CharHeight();
	if (ch < 1)
		return;

	h = con_popuprows * ch;
	w = con_popupr - con_popupl;
	if (w < ch*4)
		w = ch*4;

	top = con_popupb;			//the bottom of the input row: the popup starts here
	if (top + h > (int)vid.pixelheight)
		top = con_popupb - ch - h;	//no room below: flip above the field, one row tall
	if (top < 0)
		top = 0;
	bot = top + h;

	/*
	The height is exact ONLY because the rows are forced to one line each.

	Con_DrawConsoleLines always walks UPWARD from a bottom it is handed, so a box
	that grows downward has to know how tall it is before it draws -- and
	Font_LineBreaks can turn one conline_t into several screen rows, which is
	precisely what the existing in-console arms rely on and account for for free.
	CONF_NOWRAP makes linecount 1 per node, so rows == nodes and the arithmetic
	above closes. A long cvar row then clips at the popup's right edge instead of
	quietly making the box too short, and clipping a long entry is what a real
	combobox does anyway.
	*/
	oldflags = con->flags;
	con->flags |= CONF_NOWRAP;

	/*
	Lift the window clip, and this is the whole trick. Patch 215 rejected a true
	overlay because Con_DrawOneConsole runs inside BE_Scissor(&srect) and anything
	past the window's bottom edge is thrown away. That was right, and it is still
	right -- so the popup takes the clip off, paints, and puts it back. The
	R2D_Flush either side is mandatory: 2D geometry is batched and a scissor change
	with a batch in flight applies to the wrong quads.
	*/
	/*
	Font_EndString FIRST, for the same reason it is needed on the way out: the
	console's own rows may have auto-flushed mid-list and left their tail queued
	with R2D_Flush NULL, in which case `if (R2D_Flush)` is false, the flush below
	does nothing, and those glyphs surface LATER -- on top of the background this
	is about to paint. That is not theoretical either: the flipped popup, which
	overlaps the console's text region, showed the scrollback straight through it.
	*/
	Font_EndString(font_console);
	if (R2D_Flush)
		R2D_Flush();
	if (con_popupclipped)
		BE_Scissor(NULL);

	sxv = (float)vid.width  / vid.pixelwidth;	//physical font space -> virtual, the
	syv = (float)vid.height / vid.pixelheight;	//same conversion Con_DrawImageClip does
	vx = con_popupl * sxv;
	vy = top * syv;
	vw = w * sxv;
	vh = h * syv;

	Con_ParseColour(&con_colour_back,   back,   0.0f, 0.05f, 0.1f);
	Con_ParseColour(&con_colour_accent, accent, 0.55f, 0.7f, 0.95f);

	/*
	OPAQUE, not the console's own alpha, and R2D_FillBlock cares about the
	difference: at alpha 1 it picks shader_draw_fill rather than
	shader_draw_fill_trans. This hangs over the game and, when it flips, over the
	console's own scrollback -- a list of map names read against either is not a
	list. At 0.96 the scrollback behind a flipped popup was still legible through
	it, which is what settled this.
	*/
	R2D_ImageColours(back[0], back[1], back[2], 1.0f);
	R2D_FillBlock(vx, vy, vw, vh);
	R2D_ImageColours(accent[0], accent[1], accent[2], 0.85f);
	R2D_FillBlock(vx,        vy,        vw, 1);
	R2D_FillBlock(vx,        vy+vh-1,   vw, 1);
	R2D_FillBlock(vx,        vy,        1,  vh);
	R2D_FillBlock(vx+vw-1,   vy,        1,  vh);
	R2D_ImageColours(1, 1, 1, 1);
	if (R2D_Flush)
		R2D_Flush();

	/*
	The hit box straight from the cursor, with NO window skew.

	Patch 213's essay in Con_DrawOneConsole exists because con->mousecursor[] is
	measured from (wnd_x+CON_WNDBORDER, wnd_y) while the code there treats it as
	measured from (fx, fy). None of that applies to a popup placed in absolute
	screen space, so the global cursor converts straight across -- and applying
	that skew here would reintroduce exactly the bug Patch 213 removed.

	A degenerate box (start == end) at selactive 0 is mouse-over tracking with no
	drawn selection, which is all a dropdown wants: Con_DrawConsoleLines promotes
	0 to 2 ("calculate, but don't draw") and drops the box entirely when the
	cursor is outside the rows.
	*/
	Font_BeginString(font_console, mousecursor_x, mousecursor_y, &selsx, &selsy);
	selex = selsx;
	seley = selsy;

	Con_DrawConsoleLines(con, con->completionline, 0, con_popupl, con_popupl+w,
			bot, top, 0, selsx, selex, selsy, seley, 0);

	/*
	Font_EndString before the flush, and it is load-bearing rather than tidy.

	Font_Flush() sets R2D_Flush = NULL on its way in (gl_font.c). The glyph batch
	auto-flushes every FONT_CHAR_BUFFER characters, so any list longer than that
	leaves the TAIL of its rows queued with no flush pointer at all -- `if
	(R2D_Flush)` is then false, the rows are still in flight when the scissor
	comes back, and they get clipped away by the window they were drawn outside
	of. That is not a hypothetical: it drew six rows of a thirteen-row list and
	left the other seven as empty space inside a correctly-sized box.

	Font_EndString re-arms the pointer exactly when the mesh is non-empty, which
	is why Con_DrawConsole has always ended with one. The Font_BeginString after
	puts the caller's font state back so this is invisible from outside.
	*/
	Font_EndString(font_console);
	if (R2D_Flush)
		R2D_Flush();
	if (con_popupclipped)
		BE_Scissor(&con_popupclip);
	Font_BeginString(font_console, 0, 0, &selsx, &selsy);

	con->flags = oldflags;

	con_popupshown = con;
	con_popuphit[0] = vx;
	con_popuphit[1] = vy;
	con_popuphit[2] = vw;
	con_popuphit[3] = vh;
}

//FTESurf Patch 226: is (x,y) over the popup the console last painted? Virtual pixels.
static qboolean Con_PopupCovers(console_t *con, float x, float y)
{
	if (con_popupshown != con || con_completiondown.ival != 2)
		return false;
	return x >= con_popuphit[0] && x < con_popuphit[0]+con_popuphit[2] &&
	       y >= con_popuphit[1] && y < con_popuphit[1]+con_popuphit[3];
}

void Con_DrawConsole (int lines, qboolean noback)
{
	extern qboolean scr_con_forcedraw;
	int x, y, sx, ex;
	conline_t *l;
	int selsx, selsy, selex, seley, selactive;
	qboolean haveprogress;
	console_t *w, *mouseconsole;
	float fadetime;

	if (!con_current)
		con_current = Con_GetMain();

	con_mouseover = NULL;
	con_popupcon = NULL;	//FTESurf Patch 226: a frame that draws no input line leaves no popup

	//draw any windowed consoles (under main console)
	for (w = con_head; w; w = w->next)
	{
		srect_t srect;
		int keepback = -1;	//nettest: con_keepscroll - how many lines above the live tail the user was reading. -1 = not scrolled / disabled.
		float keepscroll = 0;
		int top, sw, gr;	//FTESurf Patch 211/213: title height, scrollbar width, grip size
		if ((w->flags & (CONF_HIDDEN|CONF_ISWINDOW)) != CONF_ISWINDOW)
			continue;
		/*
		The closed main window is represented by Con_DrawNotifyOne, at the HUD's
		top-left in the fixed 8px font.  Drawing this window as well is what made
		notifications inherit its last dragged rectangle and 16px console face.
		*/
		if (w == con_main && (!Key_Dest_Has(kdm_cwindows) || con_curwindow != w))
			continue;

		//FTESurf Patch 211: read once, here, so the draw below and the hit tests in
		//keys.c cannot be looking at different numbers within one frame.
		top = Con_WindowTitleHeight();
		sw  = Con_WindowScrollWidth();
		gr  = Con_WindowGripSize();		//Patch 213

		if (Key_Dest_Has(kdm_cwindows))
			fadetime = 0;	//nothing fades when focused.
		else
			fadetime = 4;

		if (w->wnd_w > vid.width)
			w->wnd_w = vid.width;
		if (w->wnd_h > vid.height)
			w->wnd_h = vid.height;
		/*
		FTESurf Patch 213: the size floor has to know how big the chrome is.

		This is not a new hazard, it is one this patch found and Build 27 shipped.
		Everything that ACTS on a drag -- CB_MOVE, all three CB_SIZE* -- lives in
		Key_GetConsoleSelectionBox, which for a window is only ever reached from
		Con_DrawOneConsole, which is only called inside the `srect.width > 0 &&
		srect.height > 0` guard below. So a window whose chrome does not fit
		inside it stops drawing AND stops answering the mouse in the same frame:
		blank, unresizable, unmovable, with the input line gone too. You can still
		blind-type your way out, which is not a recovery anyone should need.

		The old floors were 64 and 16 against a title bar that Patch 211 made as
		tall as its font. At con_textsize 16 that is 22, so wnd_h 16 gave
		srect.height = 16-22-8 = -14 and the window bricked the moment you dragged
		it that short -- in build 27, before this patch existed.

		So the floors come from the chrome instead of being typed. +8 and +32 are
		one row of text and a few columns of it: the point is that the text region
		is never allowed to reach zero, because zero is what turns the guard off.
		*/
		if (w->wnd_w < CON_WNDBORDER + gr + sw + 32)
			w->wnd_w = CON_WNDBORDER + gr + sw + 32;
		if (w->wnd_h < top + gr + 8)
			w->wnd_h = top + gr + 8;
		//windows that move off the top of the screen somehow are bad.
		if (w->wnd_y > vid.height - 8)
			w->wnd_y = vid.height - 8;
		if (w->wnd_y < 0)
			w->wnd_y = 0;
		if (w->wnd_x > vid.width-32)
			w->wnd_x = vid.width-32;
		if (w->wnd_x < -w->wnd_w+32)
			w->wnd_x = -w->wnd_w+32;

		if (w->wnd_h < 8)
			w->wnd_h = 8;

		/*FTESurf Patch 226: the completion popup hangs OUTSIDE this rect by design,
		  so a cursor over one of its rows would fail this test and the click would
		  never be routed to the console that owns it. Con_PopupCovers answers with
		  the rect the popup last painted -- see the note on con_popupshown for why
		  one frame of staleness is the right trade here.*/
		if ((mousecursor_x >= w->wnd_x && mousecursor_x < w->wnd_x+w->wnd_w && mousecursor_y >= w->wnd_y && mousecursor_y < w->wnd_y+w->wnd_h && mousecursor_y > lines)
			|| Con_PopupCovers(w, mousecursor_x, mousecursor_y))
			con_mouseover = w;

		w->mousecursor[0] = mousecursor_x - (w->wnd_x+CON_WNDBORDER);
		w->mousecursor[1] = mousecursor_y - w->wnd_y;

		if (Key_Dest_Has(kdm_cwindows))
		{
			/*
			FTESurf Patch 211.  Four changes here and each answers one report:

			  top     was a literal 8 and is now the height of the font the title
			          is actually drawn in, plus padding -- the bar is the drag
			          handle, and an 8px handle under a 16px face is why it was
			          hard to hit.  keys.c hit-tests against the same accessor.
			  font    was Draw_FunStringWidth, i.e. font_default, the engine's 8px
			          bitmap.  font_console IS con_textfont at con_textsize, so
			          naming it here is both "the Google font" and "twice as big"
			          in one token, with no second font object to keep alive across
			          a vid_restart.
			  align   the `2` was CENTRED (sbar.c:257). 0 is left.
			  colour  the background was SRGBA(0, 0.05, 0.1) -- a navy with zero
			          red, which is the blue. Now two cvars and everything derived.

			The title strip is a second fill over the first rather than a shorter
			background: the window fill has to cover the whole rect anyway (it is
			what makes the text legible), so painting the bar on top of it costs
			one block and keeps the two alphas in step.
			*/
			int titleh = top;
			float back[3], accent[3], a;
			Con_ParseColour(&con_colour_back, back, 0.0f, 0.05f, 0.1f);
			Con_ParseColour(&con_colour_accent, accent, 0.55f, 0.7f, 0.95f);
			a = (con_curwindow==w)?0.8f:0.5f;

			R2D_ImageColours(SRGBA(back[0], back[1], back[2], a));
			R2D_FillBlock(w->wnd_x, w->wnd_y, w->wnd_w, w->wnd_h);
			//the bar, lifted off the ground so it reads as a handle
			R2D_ImageColours(SRGBA(back[0]*1.9f+0.03f, back[1]*1.9f+0.03f, back[2]*1.9f+0.03f, a));
			R2D_FillBlock(w->wnd_x, w->wnd_y, w->wnd_w, titleh);
			R2D_ImageColours(1, 1, 1, 1);

			Draw_FunStringWidthFont(font_console, w->wnd_x+CON_WNDBORDER, w->wnd_y+(int)con_window_titlepad.value, w->title, w->wnd_w-CON_WNDBORDER-titleh, 0, (con_curwindow==w)?true:false);
			Draw_FunStringWidthFont(font_console, w->wnd_x+w->wnd_w-titleh, w->wnd_y+(int)con_window_titlepad.value, "X", titleh, 2, ((w->buttonsdown == CB_CLOSE && w->mousecursor[0] > w->wnd_w-(CON_WNDBORDER+titleh) && w->mousecursor[1] < titleh) || (con_curwindow==w && w->mousecursor[0] >= w->wnd_w-(CON_WNDBORDER+titleh) && w->mousecursor[0] < w->wnd_w-CON_WNDBORDER && w->mousecursor[1] >= 0 && w->mousecursor[1] < titleh))?true:false);

			if (w->backshader || *w->backimage)
			{
				shader_t *shader = w->backshader;
				if (!shader)
					shader = w->backshader = R_RegisterPic(w->backimage, NULL);// R_RegisterCustom(w->backimage, SUF_NONE, Shader_DefaultCinematic, w->backimage);
				if (shader)
				{
					float backx = w->wnd_x+8;
					float backy = w->wnd_y+top;
					float backw = w->wnd_w-16;
					float backh = w->wnd_h-8-top;
#ifdef HAVE_MEDIA_DECODER
					cin_t *cin = R_ShaderGetCinematic(shader);
					if (cin)
					{
						const char *url = Media_Send_GetProperty(cin, "url");
						if (url)
						{
							float x = 0;
//							float r = x+w->wnd_w-16;
							const char *buttons[] = {"bck", "fwd", "rld", "home", "", ((w->linebuffered == Con_Navigate)?(char*)key_lines[edit_line]:url)};
							const char *buttoncmds[] = {"cmd:back", "cmd:forward", "cmd:refresh",
							#ifdef QUAKETC	//total conversions should have their own website.
								ENGINEWEBSITE
							#else			//otherwise use some more useful page, for quake mods.
								"cmd:home"
							#endif
								, NULL, NULL};
							float tw;
							int i, fl;

							for (i = 0; i < countof(buttons); i++)
							{
								if (i == countof(buttons)-1)
									tw = FLT_MAX;
								else if (i == countof(buttons)-2)
								{
									tw = 8+8;
									if (*w->icon)
										R2D_Image(w->wnd_x+8+x, w->wnd_y+top, tw, tw, 0, 0, 1, 1, R_RegisterPic(w->icon, NULL));
									else
										tw = 0;
								}
								else if (i == countof(buttons)-3)
									tw = 40;
								else
									tw = 32;
								fl = con_curwindow==w;
								//FTESurf Patch 213: was `>= 8 && < 16`, i.e. the row under
								//an 8px title bar. keys.c arms CB_ACTIONBAR over
								//[wtop, wtop+8), so once the bar grew past 8 the two
								//bands stopped intersecting and these buttons became
								//unclickable. (Reached only by a cinematic/browser
								//console, which FTESurf does not open -- fixed because
								//it is the same one-speller defect, not because it bit.)
								if (w->mousecursor[1] >= top && w->mousecursor[1] < top+8 && w->mousecursor[0] >= x && w->mousecursor[0] < x+tw)
								{
									fl |= 2;
									if (w->buttonsdown == CB_ACTIONBAR)
									{
										w->buttonsdown = CB_NONE;
										if (buttoncmds[i])
											Media_Send_Command(cin, buttoncmds[i]);
										else if (w->linebuffered != Con_Navigate)
										{
											Key_ConsoleReplace(url);
											w->linebuffered = Con_Navigate;
										}
									}
								}
								if (tw > w->wnd_w-16 - x)
									tw = w->wnd_w-16 - x;
								Draw_FunStringWidth(w->wnd_x+8+x, w->wnd_y+top, buttons[i], tw, false, fl);
								x += tw;
							}
							top += 8;
							backy += 8;
							backh -= 8;
						}

						//convert these to pixels.
						backx = (backx*(int)vid.rotpixelwidth) / (float)vid.width;
						backy = (backy*(int)vid.rotpixelheight) / (float)vid.height;
						backw = (backw*(int)vid.rotpixelwidth) / (float)vid.width;
						backh = (backh*(int)vid.rotpixelheight) / (float)vid.height;
						//snap to pixels. this avoids issues with linear filtering
						backx = (int)backx;
						backy = (int)backy;
						backw = (int)backw;
						backh = (int)backh;
						Media_Send_Resize(cin, backw, backh);
						//convert back to screen coords now.
						backx = (backx*(int)vid.width) / (float)vid.rotpixelwidth;
						backy = (backy*(int)vid.height) / (float)vid.rotpixelheight;
						backw = (backw*(int)vid.width) / (float)vid.rotpixelwidth;
						backh = (backh*(int)vid.height) / (float)vid.rotpixelheight;

						Media_Send_MouseMove(cin, (w->mousecursor[0]) / backw, (w->mousecursor[1]-top) / backh);
						if (con_curwindow==w)
							Media_Send_Command(cin, "cmd:focus");
						else
							Media_Send_Command(cin, "cmd:unfocus");
					}
#endif
					R2D_Image(backx, backy, backw, backh, 0, 0, 1, 1, shader);
				}
			}

			w->unseentext = false;
		}
		else
		{
			w->buttonsdown = 0;
			//Non-main floating consoles retain their legacy faded live-tail display. The
			//closed main console never reaches here now; Con_DrawNotifyOne owns its fixed
			//top-left HUD overlay. For these remaining windows, Con_DrawConsoleLines only
			//walks OLDER than the line it is given, so drawing from a scrolled-up display
			//would hide new prints. Snap to the tail for the draw, then put the user's
			//reading position back afterwards so reopening preserves it (con_keepscroll).
			if (con_keepscroll.ival && w->display && w->display != w->current)
			{	//store it as a DISTANCE, not a pointer: Con_DrawConsoleLines can Con_Printf (failed
				//link-image registration), which can evict and free a line out from under us.
				conline_t *cl;
				keepback = 0;
				for (cl = w->display; cl && cl != w->current; cl = cl->newer)
					keepback++;
				keepscroll = w->displayscroll;
			}
			w->display = w->current;
			w->displayscroll = 0;
		}

		/*
		FTESurf Patch 211: the text region, derived rather than typed.

		Was x+8, y+8, w-24, h-16 -- where the first 8 is the left inset, the 24 is
		"both insets plus an 8px scrollbar strip", and the y+8/h-16 assumed an 8px
		title bar.  The title bar is now as tall as its font and the scrollbar is
		as wide as con_scrollwidth, so every one of those numbers had to come from
		the same two accessors keys.c hit-tests against.

		Patch 213 moves the right-hand and bottom bounds off CON_WNDBORDER and onto
		the grip, so the last line of text can never sit UNDER the bottom grip --
		text you can read but cannot select, because clicking it resizes instead.
		*/
		srect.x = (w->wnd_x+CON_WNDBORDER) / vid.width;
		srect.y = (w->wnd_y+top) / vid.height;
		srect.width = (w->wnd_w-(CON_WNDBORDER+gr+sw)) / vid.width;
		srect.height = (w->wnd_h-top-gr) / vid.height;
		srect.dmin = -99999;
		srect.dmax = 99999;
		srect.y = (1-srect.y) - srect.height;
		if (srect.width > 0 && srect.height > 0)
		{
			float back[3], accent[3];
			Con_ParseColour(&con_colour_back, back, 0.0f, 0.05f, 0.1f);
			Con_ParseColour(&con_colour_accent, accent, 0.55f, 0.7f, 0.95f);

			if (!fadetime)
			{
				//FTESurf Patch 211: was SRGBA(0, 0.1, 0.2, 1.0), a solid navy edge
				R2D_ImageColours(SRGBA(accent[0], accent[1], accent[2], 0.25f));
				if ((w->buttonsdown & CB_SIZELEFT) || (con_curwindow==w && w->mousecursor[0] >= -CON_WNDBORDER && w->mousecursor[0] < 0 && w->mousecursor[1] >= top && w->mousecursor[1] < w->wnd_h))
					R2D_FillBlock(w->wnd_x, w->wnd_y+top, CON_WNDBORDER, w->wnd_h-top);
				/*
				The right GRIP is `gr` wide at the window's own edge and does not
				move with the scrollbar. mousecursor[] is offset by CON_WNDBORDER,
				so the window's last column is wnd_w-CON_WNDBORDER -- and the grip
				is the `gr` columns below that, NOT below wnd_w. Confusing those
				two is the one arithmetic slip here that still looks like it works.
				*/
				if ((w->buttonsdown & CB_SIZERIGHT) || (con_curwindow==w && w->mousecursor[0] >= w->wnd_w-CON_WNDBORDER-gr && w->mousecursor[0] < w->wnd_w-CON_WNDBORDER && w->mousecursor[1] >= top && w->mousecursor[1] < w->wnd_h))
					R2D_FillBlock(w->wnd_x+w->wnd_w-gr, w->wnd_y+top, gr, w->wnd_h-top);
				if ((w->buttonsdown & CB_SIZEBOTTOM) || (con_curwindow==w && w->mousecursor[0] >= -CON_WNDBORDER && w->mousecursor[0] < w->wnd_w-CON_WNDBORDER && w->mousecursor[1] >= w->wnd_h-gr && w->mousecursor[1] < w->wnd_h))
					R2D_FillBlock(w->wnd_x, w->wnd_y+w->wnd_h-gr, w->wnd_w, gr);
			}
			//nettest: scrollbar in the strip on the right (freed by narrowing the text). That
			//strip is already the CB_SCROLL drag region, so dragging it scrolls; the thumb tracks con->display.
			if (Key_Dest_Has(kdm_cwindows) && w->linecount > 0)	//nettest: only show the scrollbar while the console is focused/open
			{
				//FTESurf Patch 213: sits just INSIDE the right grip, not under it
				float trkx = w->wnd_x + w->wnd_w - (gr+sw);
				float trky = w->wnd_y + top;
				float trkh = w->wnd_h - top - gr;
				int total = w->linecount, above = 0;
				conline_t *cl;
				float vis, thumbh, thumby, pos, ch;
				for (cl = w->oldest; cl && cl != w->display; cl = cl->newer)
					above++;
				ch = Font_CharVHeight(font_console);	//nettest: explicit font - Font_CharHeight() derefs the global curfont, which is NOT bound at this point in Con_DrawConsole (was a NULL-deref crash on the first frame)
				if (ch < 1) ch = 1;
				vis = trkh / ch;
				if (vis < 1) vis = 1;
				thumbh = (total > vis) ? (vis / (float)total) * trkh : trkh;
				if (thumbh < 8) thumbh = 8;
				if (thumbh > trkh) thumbh = trkh;
				pos = (total > 1) ? (above / (float)(total-1)) : 0;
				if (pos < 0) pos = 0;
				if (pos > 1) pos = 1;
				thumby = trky + pos * (trkh - thumbh);
				//FTESurf Patch 211: sized off sw, coloured off con_colour_accent
				R2D_ImageColours(SRGBA(accent[0], accent[1], accent[2], 0.10f));	//track
				R2D_FillBlock(trkx+1, trky, sw-2, trkh);
				R2D_ImageColours(SRGBA(accent[0], accent[1], accent[2], 0.85f));	//thumb
				R2D_FillBlock(trkx+1, thumby, sw-2, thumbh);
				R2D_ImageColours(1,1,1,1);
			}
			if (R2D_Flush)
				R2D_Flush();
			BE_Scissor(&srect);
			//FTESurf Patch 226: what Con_DrawCompletionPopup has to put back after it
			//lifts the clip to paint outside this window. BE_Scissor is a flat set
			//with no stack, so the rect has to be kept rather than pushed.
			con_popupclip = srect;
			con_popupclipped = true;
			/*
			FTESurf Patch 211: fy is offset by the title bar rather than assuming it
			is 8 tall. Con_DrawOneConsole lays out from fy+fsy upward, so the pair
			(fy, fsy) has to describe the SAME box the scissor above does.

			Patch 213: what matters is the BOTTOM edge, fy+fsy, and it has to land on
			wnd_y+wnd_h-gr like the scissor now does. fy is deliberately left alone --
			it overshoots upward and the scissor clips it -- so the whole grip change
			lands in fsy. At the default gr == CON_WNDBORDER this is wnd_h-top, i.e.
			byte-identical to build 27.
			*/
			Con_DrawOneConsole(w, con_curwindow == w && Key_Dest_Has(kdm_console|kdm_cwindows) == kdm_cwindows, font_console, w->wnd_x+CON_WNDBORDER, w->wnd_y+top-CON_WNDBORDER, w->wnd_w-(CON_WNDBORDER+gr+sw), w->wnd_h-top+CON_WNDBORDER-gr, fadetime);
			if (R2D_Flush)
				R2D_Flush();
			BE_Scissor(NULL);
			con_popupclipped = false;
		}

		if (keepback >= 0)
		{	//nettest: put the reading position back now the draw is done, so reopening the console
			//lands where the user left off. Walking older from the (never-freed) current line means
			//an eviction mid-draw just costs us a row or two of accuracy instead of a dangling pointer.
			conline_t *cl = w->current;
			while (keepback-- > 0 && cl && cl->older)
				cl = cl->older;
			if (cl)
			{
				w->display = cl;
				w->displayscroll = keepscroll;
			}
		}

		if (w->selstartline)
			mouseconsole = w;
		if (!con_curwindow)
			con_curwindow = w;
	}

	//draw main console...
	if (lines > 0 && con_current && !(con_current->flags & CONF_ISWINDOW))
	{
		int top;
#ifdef QTERM
		if (qterms)
			QT_Update();
#endif

// draw the background
		if (!noback)
			R2D_ConsoleBackground (0, lines, scr_con_forcedraw);

		con_current->unseentext = false;

		con_current->vislines = lines;

		top = Con_DrawAlternateConsoles(lines);

		if (!con_current->display)
			con_current->display = con_current->current;

		x = 8;
		y = lines;

		con_current->mousecursor[0] = mousecursor_x;
		con_current->mousecursor[1] = mousecursor_y;
		if (!(con_current->flags & CONF_KEEPSELECTION))
		{
			con_current->selstartline = NULL;
			con_current->selendline = NULL;
		}
		selactive = Key_GetConsoleSelectionBox(con_current, &selsx, &selsy, &selex, &seley);

		if ((con_current->flags & CONF_KEEPSELECTION) && con_current->selstartline && con_current->selendline && con_current->buttonsdown != CB_SELECTED && con_current->buttonsdown != CB_TAPPED)
			selactive = -1;

		Font_BeginString(font_console, x, y, &x, &y);
		Font_BeginString(font_console, selsx, selsy, &selsx, &selsy);
		Font_BeginString(font_console, selex, seley, &selex, &seley);
		ex = Font_ScreenWidth();
		sx = x;
		ex -= sx;

		y -= Font_CharHeight();
		haveprogress = Con_DrawProgress(x, ex - x, y) != y;
		y = Con_DrawInput (con_current, Key_Dest_Has(kdm_console), x, ex - x, y, top, selactive, selsx, selex, selsy, seley);	//FTESurf Patch 215: `top` bounds the dropdown

		l = con_current->display;

		y = Con_DrawConsoleLines(con_current, l, con_current->displayscroll, sx, ex, y, top, selactive, selsx, selex, selsy, seley, 0);

		//FTESurf Patch 226: same call, the fullscreen console's copy. No scissor is
		//live on this path (con_popupclipped is false), so the popup simply paints.
		Con_DrawCompletionPopup(con_current);

		if (!haveprogress && lines == vid.height)
		{
			char *version = version_string();
			int i;
			Font_BeginString(font_console, vid.width, lines, &x, &y);
			y -= Font_CharHeight();
			//assumption: version == ascii
			for (i = 0; version[i]; i++)
				x -= Font_CharWidth(CON_WHITEMASK|CON_HALFALPHA, version[i]);
			for (i = 0; version[i]; i++)
				x = Font_DrawChar(x, y, CON_WHITEMASK|CON_HALFALPHA, version[i]);
		}

		Font_EndString(font_console);
		mouseconsole = con_mouseover?con_mouseover:con_current;


		if (con_current->buttonsdown == CB_SELECTED || con_current->buttonsdown == CB_TAPPED)
		{	//select was released...
			console_t *con = con_current;
			char *buffer;
			qboolean tapped = con->buttonsdown==CB_TAPPED;
			con->buttonsdown = CB_NONE;
			if (con->selstartline)
			{
				if (tapped)
					con->flags &= ~CONF_KEEPSELECTION;
				else
					con->flags |= CONF_KEEPSELECTION;
				if (con->userline)
				{
					if (con->flags & CONF_BACKSELECTION)
					{
						con->userline = con->selendline;
						con->useroffset = con->selendoffset;
					}
					else
					{
						con->userline = con->selstartline;
						con->useroffset = con->selstartoffset;
					}
				}
				if (con->selstartline == con->selendline && con->selendoffset <= con->selstartoffset+1)
				{
					if (keydown[K_LSHIFT] || keydown[K_RSHIFT])
						;
					else
					{
						buffer = Con_CopyConsole(con, false, true, false);
						if (buffer)
						{
							Key_HandleConsoleLink(con, buffer);
							Z_Free(buffer);
						}
					}
				}
				else
				{
					buffer = Con_CopyConsole(con, true, false, true);	//don't keep markup if we're copying to the clipboard
					if (buffer)
					{
						Sys_SaveClipboard(CBT_SELECTION,  buffer);
						Z_Free(buffer);
					}
				}
			}
		}
	}
	else
		mouseconsole = con_mouseover?con_mouseover:NULL;

	if (mouseconsole && mouseconsole->selstartline)
		Con_DrawMouseOver(mouseconsole);
}

void Con_DrawOneConsole(console_t *con, qboolean focused, struct font_s *font, float fx, float fy, float fsx, float fsy, float lineagelimit)
{
	int selactive, selsx, selsy, selex, seley;
	int x, y, sx, sy;
	Font_BeginString(font, fx, fy, &x, &y);
	Font_BeginString(font, fx+fsx, fy+fsy, &sx, &sy);

	if (con == con_current && Key_Dest_Has(kdm_console))
	{
		selactive = false;	//don't change selections if this is the main console and we're looking at the console, because that main console has focus instead anyway.
		selsx = selsy = selex = seley = 0;
	}
	else
	{
		selactive = Key_GetConsoleSelectionBox(con, &selsx, &selsy, &selex, &seley);
		if ((con->flags & CONF_KEEPSELECTION) && con->selstartline && con->selendline)
		{
			selactive = -1;
			selsx = selsy = selex = seley = 0;
		}
		else
		{
			con->selstartline = NULL;
			con->selendline = NULL;
			/*
			FTESurf Patch 213: put the selection box into the same space the four
			lines below assume it is already in.

			Key_GetConsoleSelectionBox hands back con->mousecursor[], which for a
			window is measured from (wnd_x+CON_WNDBORDER, wnd_y). The `+= x` / `+= y`
			below treat it as measured from (fx, fy). X agrees by construction --
			fx IS wnd_x+CON_WNDBORDER. Y agreed only while the title bar was 8 tall,
			because the caller passes fy = wnd_y+top-CON_WNDBORDER and top was 8.

			Patch 211 made top the height of the console font, so since build 27 every
			selection and every LINK CLICK in a console window has been out by
			top-CON_WNDBORDER -- 14px at con_textsize 16, about one row. That is why
			clicking a completion row could take the row above it.
			*/
			if (con->flags & CONF_ISWINDOW)
			{
				int skew = (int)(fy - con->wnd_y);
				selsy -= skew;
				seley -= skew;
			}
			Font_BeginString(font, selsx, selsy, &selsx, &selsy);
			Font_BeginString(font, selex, seley, &selex, &seley);
			selsx += x;
			selsy += y;
			selex += x;
			seley += y;
		}
	}

	R2D_ImageColours(1, 1, 1, 1);
	sy = Con_DrawInput (con, focused, x, sx, sy, y, selactive, selsx, selex, selsy, seley);	//FTESurf Patch 215: `y` is this window's top, and bounds the dropdown

	sx -= con->displayoffset;
	selsx -= con->displayoffset;
	selex -= con->displayoffset;

	if (!con->display)
		con->display = con->current;
	Con_DrawConsoleLines(con, con->display, con->displayscroll, x, sx, sy, y, selactive, selsx, selex, selsy, seley, lineagelimit);

	/*FTESurf Patch 226: the floating completion list, over the top of everything
	  this window just drew and outside its clip. BEFORE the release handling
	  below, which is what turns a click on one of its rows into a link.*/
	Con_DrawCompletionPopup(con);

	if (con->buttonsdown == CB_SELECTED || con->buttonsdown == CB_TAPPED)
	{	//select was released...
		char *buffer;
		qboolean tapped = con->buttonsdown==CB_TAPPED;
		con->buttonsdown = CB_NONE;
		if (con->selstartline)
		{
			if (tapped)
				con->flags &= ~CONF_KEEPSELECTION;
			else
				con->flags |= CONF_KEEPSELECTION;
			if (con->userline)
			{
				if (con->flags & CONF_BACKSELECTION)
				{
					con->userline = con->selendline;
					con->useroffset = con->selendoffset;
				}
				else
				{
					con->userline = con->selstartline;
					con->useroffset = con->selstartoffset;
				}
			}
			if (con->selstartline == con->selendline && con->selendoffset <= con->selstartoffset+1)
			{
				if (keydown[K_LSHIFT] || keydown[K_RSHIFT])
					;
				else
				{
					buffer = Con_CopyConsole(con, false, true, false);
					if (buffer)
					{
						Key_HandleConsoleLink(con, buffer);
						Z_Free(buffer);
					}
				}
			}
			else
			{
				buffer = Con_CopyConsole(con, true, false, true);	//don't keep markup if we're copying to the clipboard
				if (buffer)
				{
					Sys_SaveClipboard(CBT_SELECTION,  buffer);
					Z_Free(buffer);
				}
			}
		}
	}

	Font_EndString(font);
}

//false=don't walk over it.
//true=fine and dandy
//2=ignore only if at the end.
static qbyte Con_IsTokenChar(unsigned int chr)
{
	if (chr >= 0x80)	//unicode chars are all continuation
		return true;
	if (chr == '(' || chr == ')' || chr == '{' || chr == '}')
		return false;
	if (chr == '/' || chr == '\\')
		return 2;	//on left only
	if (chr == '.' || chr == ':')
		return 3;	//disallow only if followed by whitespace
	if (chr >= 'a' && chr <= 'z')
		return true;
	if (chr >= 'A' && chr <= 'Z')
		return true;
	if (chr >= '0' && chr <= '9')
		return true;
	if (chr == '[' || chr == ']' || chr == '_')
		return true;
	return false;
}
void Con_ExpandConsoleSelection(console_t *con)
{
	conchar_t *cur, *n;
	conline_t *l;
	conchar_t *lstart;
	conchar_t *lend;
	unsigned int cf, uc;

	//no selection to expand...
	if (!con->selstartline || !con->selendline)
		return;

	l = con->selstartline;
	lstart = (conchar_t*)(l+1);
	cur = lstart + con->selstartoffset;

	if (con->selstartline == con->selendline)
	{
		if (con->selstartoffset+1 == con->selendoffset)
		{
			//they only selected a single char?
			//fix that up to select the entire token
			while (cur > lstart)
			{
				cur--;
				uc = (*cur & CON_CHARMASK);
				if (!Con_IsTokenChar(uc))
				{
					cur++;
					break;
				}
				if (*cur == CON_LINKSTART)
					break;
			}
			for (n = lstart+con->selendoffset; con->selendoffset < l->length; )
			{
				n = Font_Decode(n, &cf, &uc);
				if (Con_IsTokenChar(uc)==3)
					continue;

				if (Con_IsTokenChar(uc)==1 && lstart[con->selendoffset] != CON_LINKEND)
					con->selendoffset = n-lstart;
				else
					break;
			}
			/*while (con->selendoffset > l->length)
			{
				uc = (((conchar_t*)(l+1))[con->selendoffset] & CON_CHARMASK);
				if (Con_IsTokenChar(uc) == 2)
					con->selendoffset--;
				else
					break;
			}*/
		}
	}

	//scan backwards to find any link enclosure
	for(lend = cur-1; lend >= (conchar_t*)(l+1); lend--)
	{
		if (*lend == CON_LINKSTART)
		{
			//found one
			cur = lend;
			break;
		}
		if (*lend == CON_LINKEND)
		{
			//some other link ended here. don't use its start.
			break;
		}
	}
	//scan forwards to find the end of the selected link
	if (l->length && cur < (conchar_t*)(l+1)+l->length && *cur == CON_LINKSTART)
	{
		for(lend = (conchar_t*)(con->selendline+1) + con->selendoffset; lend < (conchar_t*)(con->selendline+1) + con->selendline->length; lend++)
		{
			if (*lend == CON_LINKEND)
			{
				con->selendoffset = lend+1 - (conchar_t*)(con->selendline+1);
				break;
			}
		}
	}

	con->selstartoffset = cur-(conchar_t*)(l+1);
}
char *Con_CopyConsole(console_t *con, qboolean nomarkup, qboolean onlyiflink, qboolean forceutf8)
{
	conchar_t *cur;
	conline_t *l;
	conchar_t *lend;
	char *result;
	int outlen, maxlen;
	int finalendoffset;
	unsigned int uc;

	if (!con->selstartline || !con->selendline)
		return NULL;

//	for (cur = (conchar_t*)(selstartline+1), finalendoffset = 0; cur < (conchar_t*)(selstartline+1) + selstartline->length; cur++, finalendoffset++)
//		result[finalendoffset] = *cur & 0xffff;

	l = con->selstartline;
	cur = (conchar_t*)(l+1) + con->selstartoffset;
	finalendoffset = con->selendoffset;

	if (con->selstartline == con->selendline)
	{
		if (con->selstartoffset+1 == finalendoffset)
		{
			//they only selected a single char?
			//fix that up to select the entire token
			while (cur > (conchar_t*)(l+1))
			{
				cur--;
				uc = (*cur & CON_CHARMASK);
				if (!Con_IsTokenChar(uc))
				{
					cur++;
					break;
				}
				if (*cur == CON_LINKSTART)
					break;
			}
			while (finalendoffset < l->length)
			{
				uc = (((conchar_t*)(l+1))[finalendoffset] & CON_CHARMASK);
				if (Con_IsTokenChar(uc)==1 && ((conchar_t*)(l+1))[finalendoffset] != CON_LINKEND)
					finalendoffset++;
				else
					break;
			}
			/*while (finalendoffset > l->length)
			{
				uc = (((conchar_t*)(l+1))[finalendoffset] & CON_CHARMASK);
				if (Con_IsTokenChar(uc) == 2)
					finalendoffset--;
				else
					break;
			}*/
		}
	}

	//scan backwards to find any link enclosure
	for(lend = cur-1; lend >= (conchar_t*)(l+1); lend--)
	{
		if (*lend == CON_LINKSTART)
		{
			//found one
			cur = lend;
			break;
		}
		if (*lend == CON_LINKEND)
		{
			//some other link ended here. don't use its start.
			break;
		}
	}
	//scan forwards to find the end of the selected link
	if (l->length && cur < (conchar_t*)(l+1)+l->length && *cur == CON_LINKSTART)
	{
		for(lend = (conchar_t*)(con->selendline+1) + finalendoffset; lend < (conchar_t*)(con->selendline+1) + con->selendline->length; lend++)
		{
			if (*lend == CON_LINKEND)
			{
				finalendoffset = lend+1 - (conchar_t*)(con->selendline+1);
				break;
			}
		}
	}
	else if (onlyiflink)
		return NULL;

	maxlen = 1024*1024;
	result = Z_Malloc(maxlen+1);

	outlen = 0;
	for (;;)
	{
		if (l == con->selendline)
			lend = (conchar_t*)(l+1) + finalendoffset;
		else
			lend = (conchar_t*)(l+1) + l->length;

		outlen = COM_DeFunString(cur, lend, result + outlen, maxlen - outlen, nomarkup, forceutf8||!!(con->parseflags & PFS_FORCEUTF8)) - result;

		if (l == con->selendline)
			break;

		l = l->newer;
		if (!l)
		{
			Con_Printf("Error: Bad console buffer\n");
			break;
		}

		if (outlen+3 > maxlen)
			break;
//#ifdef _WIN32
//		result[outlen++] = '\r';
//#endif
		result[outlen++] = '\n';
		cur = (conchar_t*)(l+1);
	}
	result[outlen++] = 0;

	return result;
}
