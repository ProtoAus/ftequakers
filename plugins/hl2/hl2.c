#include "../plugin.h"
qboolean GMA_Init(void);
qboolean VPK_Init(void);
qboolean TTH_Init(void);
qboolean VTF_Init(void);
qboolean CCRAW_Init(void);	//FTESurf Patch 288: the colour-correction .raw LUT
qboolean MDL_Init(void);
qboolean VMT_Init(void);
qboolean VBSP_Init(void);

qboolean Plug_Init(void)
{
	qboolean somethingisokay = false;
	char plugname[128];
	strcpy(plugname, "hl2");
	plugfuncs->GetPluginName(0, plugname, sizeof(plugname));

	if (!GMA_Init())	Con_Printf(CON_ERROR"%s: GMA support unavailable\n", plugname);	else	somethingisokay = true;
	if (!VPK_Init())	Con_Printf(CON_ERROR"%s: VPK support unavailable\n", plugname);	else	somethingisokay = true;
	if (!TTH_Init())	Con_Printf(CON_ERROR"%s: TTH support unavailable\n", plugname);	else	somethingisokay = true;
	if (!VTF_Init())	Con_Printf(CON_ERROR"%s: VTF support unavailable\n", plugname);	else	somethingisokay = true;
	//AFTER VTF_Init, and the order matters only for cost: image loaders are tried
	//in registration order (image.c:13974) and this one declines everything that
	//is not exactly 98,304 bytes with a .raw name, so putting it late costs
	//nothing and putting it early would ask it about every VTF in the game.
	if (!CCRAW_Init())	Con_Printf(CON_ERROR"%s: colour-correction LUT support unavailable\n", plugname);	else	somethingisokay = true;
	if (!VMT_Init())	Con_Printf(CON_ERROR"%s: VMT support unavailable\n", plugname);	else	somethingisokay = true;
	if (!MDL_Init())	Con_Printf(CON_ERROR"%s: MDL support unavailable\n", plugname);	else	somethingisokay = true;
	if (!VBSP_Init())	Con_Printf(CON_ERROR"%s: BSP support unavailable\n", plugname);	else	somethingisokay = true;
	return somethingisokay;
}

