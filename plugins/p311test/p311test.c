/*FTESurf Patch 311 falsifier plugin.

  The smallest possible consumer of the plugin INPUT API, existing only to prove
  that a plugin can forge input and that the journal now says so.

  WHY IT HAS TO BE A REAL PLUGIN.  The hole is that plugins/plugin.h:356-360 hands
  out raw pointers to IN_KeyEvent/IN_MouseMove, so an ordinary native DLL -- no
  cheat gate, no memory patching, no hook -- can push events into the same ring
  the platform backend uses.  Nothing already in the tree exercises that path on
  this machine: openxr.c:1648 is the only in-tree caller and it needs a VR
  runtime.  A test that cannot reach the code under test is not a test.

  IT IS NOT A CHEAT AND COULD NOT BE ONE.  It injects a fixed, meaningless
  pattern on an explicit console command, and the whole point of the patch is
  that doing so now MARKS the recording as inadmissible.  It is built standalone
  (never via `make plugins-rel`, which would rebuild the concurrent session's hl2
  plugin) and is not deployed to the game directory.
*/
#include "../plugin.h"

extern plugcorefuncs_t *plugfuncs;	/*defined by plugin.c, as in every other plugin*/
extern plugcmdfuncs_t *cmdfuncs;
extern plugcvarfuncs_t *cvarfuncs;
static pluginputfuncs_t *inputfuncs;

/*Deliberately the SAME shape as in_journal_synth's own pattern, so the two arms
  differ only in which door the events came through.*/
static void P311_Inject(void)
{
	int i, n = 32;
	for (i = 0; i < n; i++)
		inputfuncs->MouseMove(0, 0, (float)((i % 7) + 1), (float)((i % 5) - 2), 0, 0);
	plugfuncs->Print("p311test: injected 32 mouse events through the plugin input API\n");
}

qboolean Plug_Init(void)
{
	inputfuncs = plugfuncs->GetEngineInterface(pluginputfuncs_name, sizeof(*inputfuncs));
	if (!inputfuncs)
		return false;
	cmdfuncs->AddCommand("p311_inject", P311_Inject, "FTESurf Patch 311 falsifier: forge input through the plugin API.");
	plugfuncs->Print("p311test loaded\n");
	return true;
}
