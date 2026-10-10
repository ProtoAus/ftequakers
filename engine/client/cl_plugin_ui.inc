//Included by cl_plugin.inc. Trusted service callbacks, never arbitrary QC pointers.
#include "pr_common.h"
#include "gl_draw.h"

#define PLUGUI_MAXID 16777215u //exact in QC float32; never recycle a stale handle
#define PLUGUI_MAX_OPENS 4u //open attempts per VM per frame

typedef struct
{
	pubprogfuncs_t *vm;
	pluguiowner_t owner;
	pluguimodel2_t model, staging;
	unsigned int staged;
	qboolean stageactive, stagebad;
	qboolean drawing, clipped;
	srect_t scissor; //backend normalized bottom-left, including depth bounds
	unsigned int lastframe, inputframe, inputs, polls;
	unsigned int openframe, opens;
	qboolean drawok;
	//NativeUIPlot/1 staging (cl_plugin_ui_plot.inc), freed with the owner.
	pluguiplot_t plot;
	float *plotx, *plota, *plotb;
	qbyte *plotbrk;
	unsigned int plotcap, plotseries, plotrevision, plotframe, plotbegins;
	qboolean plotactive, plotbad, plotrows;
} pluguivm_t;
static pluguivm_t plugui_vms[2];
static plugin_t *plugui_service;
static pluguiinputservice_t plugui_input;
static pluguimodelservice_t plugui_model;
static pluguimodelservice2_t plugui_model2;
static pluguiplotservice_t plugui_plot;
static void Plug_NativeUI_PlotFree(pluguivm_t *v);
static unsigned int plugui_serial;
static qboolean plugui_busy;
static const pluguiframe_t *plugui_dispatch;
extern unsigned int r2d_be_flags;

static qboolean Plug_NativeUI_Ready(void)
{
	return plugui_service && !plugui_busy && qrenderer != QR_NONE &&
		BE_DrawMesh_Single && vid.width > 0 && vid.height > 0 &&
		vid.pixelwidth > 0 && vid.pixelheight > 0;
}
static pluguivm_t *Plug_NativeUI_VM(pubprogfuncs_t *vm)
{
	unsigned int i;
	for (i = 0; i < countof(plugui_vms); i++)
		if (plugui_vms[i].vm == vm)
			return &plugui_vms[i];
	return NULL;
}
static void Plug_NativeUI_CloseOwner(pluguivm_t *v, unsigned int reason)
{
	plugin_t *old = currentplug;
	pluguiowner_t owner = v->owner;
	memset(&v->owner, 0, sizeof(v->owner)); //invalidate before callback
	v->drawok = false;
	memset(&v->model, 0, sizeof(v->model));
	memset(&v->staging, 0, sizeof(v->staging));
	v->staged = 0; v->stageactive = v->stagebad = false;
	Plug_NativeUI_PlotFree(v);
	if (!owner.generation || !plugui_service)
		return;
	currentplug = plugui_service;
	plugui_busy = true;
	plugui_service->nativeui.Close(&owner, reason);
	plugui_busy = false;
	currentplug = old;
}
static qboolean Plug_NativeUI_Register(const pluguiservice_t *s, size_t size)
{
	if (!s || size != sizeof(*s) || plugui_service || plugui_busy)
		return false;
	if (s->structsize != sizeof(*s) || s->version != PLUGUI_VERSION ||
		s->capabilities != PLUGUI_CAP_INDEXED2D || !s->Open || !s->Draw || !s->Close)
		return false;
	currentplug->nativeui = *s; //caller cannot mutate the published table later
	plugui_service = currentplug;
	return true;
}
static void Plug_NativeUI_PluginClose(plugin_t *p)
{
	unsigned int i;
	if (p != plugui_service)
		return;
	for (i = 0; i < countof(plugui_vms); i++)
		Plug_NativeUI_CloseOwner(&plugui_vms[i], PLUGUI_CLOSE_PLUGIN);
	memset(&plugui_input, 0, sizeof(plugui_input));
	memset(&plugui_model, 0, sizeof(plugui_model));
	memset(&plugui_model2, 0, sizeof(plugui_model2));
	memset(&plugui_plot, 0, sizeof(plugui_plot));
	plugui_service = NULL;
}
void Plug_NativeUI_RendererShutdown(void)
{
	unsigned int i;
	for (i = 0; i < countof(plugui_vms); i++)
		Plug_NativeUI_CloseOwner(&plugui_vms[i], PLUGUI_CLOSE_RENDERER);
}
void Plug_NativeUI_Release(pubprogfuncs_t *vm)
{
	pluguivm_t *v = Plug_NativeUI_VM(vm);
	if (v)
	{
		Plug_NativeUI_CloseOwner(v, PLUGUI_CLOSE_VM);
		memset(v, 0, sizeof(*v));
	}
}
void Plug_NativeUI_Begin(pubprogfuncs_t *vm, unsigned int frontend)
{
	pluguivm_t *v;
	if (plugui_busy || !vm || frontend < PLUGUI_VM_MENU || frontend > PLUGUI_VM_CLIENT)
		return;
	v = &plugui_vms[frontend-1];
	if (v->vm != vm)
	{
		Plug_NativeUI_CloseOwner(v, PLUGUI_CLOSE_VM);
		memset(v, 0, sizeof(*v));
		v->vm = vm;
		v->lastframe = ~0u;
		v->openframe = ~0u;
	}
	v->drawing = true;
	v->clipped = false; //frontend entry resets the backend clip
}
void Plug_NativeUI_End(pubprogfuncs_t *vm)
{
	pluguivm_t *v = Plug_NativeUI_VM(vm);
	if (v && !plugui_busy)
		v->drawing = false;
}
void Plug_NativeUI_SetClip(pubprogfuncs_t *vm, const void *rect)
{
	pluguivm_t *v = Plug_NativeUI_VM(vm);
	if (!v || !v->drawing || plugui_busy)
		return;
	v->clipped = (rect != NULL);
	if (rect)
		v->scissor = *(const srect_t *)rect;
}
static qboolean Plug_NativeUI_Clip(float *clip)
{
	unsigned int i;
	if (!plugui_dispatch || currentplug != plugui_service)
		return false;
	for (i = 0; i < 2; i++)
	{
		clip[i] = max(clip[i], plugui_dispatch->clip[i]);
		clip[i+2] = min(clip[i+2], plugui_dispatch->clip[i+2]);
	}
	return true;
}
static unsigned int Plug_NativeUI_ID(float f)
{
	if (IS_NAN(f) || f < 1 || f > PLUGUI_MAXID)
		return 0;
	if (f != (unsigned int)f)
		return 0;
	return (unsigned int)f;
}
void QCBUILTIN PF_ui_native_status(pubprogfuncs_t *prinst, struct globalvars_s *pr_globals)
{
	(void)prinst;
	G_FLOAT(OFS_RETURN) = Plug_NativeUI_Ready();
}
void QCBUILTIN PF_ui_native_open(pubprogfuncs_t *prinst, struct globalvars_s *pr_globals)
{
	pluguivm_t *v = Plug_NativeUI_VM(prinst);
	plugin_t *old = currentplug;
	unsigned int owner = Plug_NativeUI_ID(G_FLOAT(OFS_PARM0));
	qboolean ok;
	G_FLOAT(OFS_RETURN) = 0;
	if (!v || !v->drawing || !owner || !Plug_NativeUI_Ready())
		return;
	if (v->owner.generation)
	{
		if (v->owner.owner == owner)
			G_FLOAT(OFS_RETURN) = v->owner.generation;
		return;
	}
	//Each attempt can cost a context, an atlas upload (freed only at frame end on
	//Vulkan) and a serial that is never reused: budget them like draw/input/poll.
	if (v->openframe != (unsigned int)host_framecount)
	{
		v->openframe = (unsigned int)host_framecount;
		v->opens = 0;
	}
	if (v->opens == PLUGUI_MAX_OPENS)
		return;
	v->opens++;
	if (plugui_serial == PLUGUI_MAXID)
		return; //fail closed on exhaustion; no wrap/reuse
	v->owner.vm = (v == &plugui_vms[0]) ? PLUGUI_VM_MENU : PLUGUI_VM_CLIENT;
	v->owner.owner = owner;
	v->owner.generation = ++plugui_serial;
	currentplug = plugui_service;
	plugui_busy = true;
	ok = plugui_service->nativeui.Open(&v->owner);
	plugui_busy = false;
	currentplug = old;
	if (!ok)
		Plug_NativeUI_CloseOwner(v, PLUGUI_CLOSE_FAILED);
	else
		G_FLOAT(OFS_RETURN) = v->owner.generation;
}
void QCBUILTIN PF_ui_native_draw(pubprogfuncs_t *prinst, struct globalvars_s *pr_globals)
{
	pluguivm_t *v = Plug_NativeUI_VM(prinst);
	plugin_t *old = currentplug;
	pluguiframe_t frame;
	vec4_t colour;
	unsigned int i, flags, handle = Plug_NativeUI_ID(G_FLOAT(OFS_PARM0));
	qboolean ok;
	G_FLOAT(OFS_RETURN) = 0;
	if (!v || !v->drawing || !handle || handle != v->owner.generation ||
		v->lastframe == (unsigned int)host_framecount || !Plug_NativeUI_Ready())
		return;
	if (*r_refdef.rt_destcolour[0].texname)
	{
		Plug_NativeUI_CloseOwner(v, PLUGUI_CLOSE_FAILED);
		return; //P589 is a physical main-framebuffer contract, not a target ABI
	}
	memset(&frame, 0, sizeof(frame));
	frame.structsize = sizeof(frame);
	frame.owner = v->owner;
	frame.frame = (unsigned int)host_framecount;
	frame.virtualwidth = vid.width;
	frame.virtualheight = vid.height;
	frame.pixelwidth = vid.pixelwidth;
	frame.pixelheight = vid.pixelheight;
	frame.clip[2] = vid.pixelwidth;
	frame.clip[3] = vid.pixelheight;
	if (v->clipped)
	{
		frame.clip[0] = v->scissor.x*vid.pixelwidth;
		frame.clip[1] = (1-v->scissor.y-v->scissor.height)*vid.pixelheight;
		frame.clip[2] = (v->scissor.x+v->scissor.width)*vid.pixelwidth;
		frame.clip[3] = (1-v->scissor.y)*vid.pixelheight;
	}
	for (i = 0; i < countof(frame.clip); i++)
		if (IS_NAN(frame.clip[i])) //also Inf; conversion itself may have overflowed
		{
			Plug_NativeUI_CloseOwner(v, PLUGUI_CLOSE_FAILED);
			return; //do not bypass mesh validation through inherited clip intersection
		}
	v->lastframe = frame.frame;
	v->drawok = false;
	v->polls = 0;
	if (R2D_Flush)
		R2D_Flush(); //QC-before must submit before native, including equal materials
	R2D_GetImageColours(colour);
	flags = r2d_be_flags;
	currentplug = plugui_service;
	plugui_busy = true;
	plugui_dispatch = &frame;
	ok = plugui_service->nativeui.Draw(&frame);
	if (R2D_Flush)
		R2D_Flush(); //no legacy draw queued by the service may leak past this call
	plugui_dispatch = NULL;
	plugui_busy = false;
	currentplug = old;
	r2d_be_flags = flags;
	R2D_ImageColours(colour[0], colour[1], colour[2], colour[3]);
	BE_Scissor(v->clipped ? &v->scissor : NULL);
	if (!ok)
		Plug_NativeUI_CloseOwner(v, PLUGUI_CLOSE_FAILED);
	else
	{
		v->drawok = true;
		G_FLOAT(OFS_RETURN) = 1;
	}
}
void QCBUILTIN PF_ui_native_close(pubprogfuncs_t *prinst, struct globalvars_s *pr_globals)
{
	pluguivm_t *v = Plug_NativeUI_VM(prinst);
	unsigned int handle = Plug_NativeUI_ID(G_FLOAT(OFS_PARM0));
	G_FLOAT(OFS_RETURN) = 0;
	if (!v || plugui_busy || !handle || handle != v->owner.generation)
		return;
	Plug_NativeUI_CloseOwner(v, PLUGUI_CLOSE_EXPLICIT);
	G_FLOAT(OFS_RETURN) = 1;
}

#include "cl_plugin_ui_input.inc"
#include "cl_plugin_ui_model.inc"
#include "cl_plugin_ui_plot.inc"
