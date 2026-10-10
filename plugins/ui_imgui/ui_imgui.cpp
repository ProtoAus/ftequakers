#include "backend.h"
#include "vendor/imgui_internal.h"
#include <cstdio>
#include <algorithm>
#include <new>
#include <cmath>
#include <cstring>
#include "../ui_model.h"

namespace FteImGui {
static plugcorefuncs_t *core;
static plugmeshfuncs_t *mesh;
static Counters stats;
//Own bounded /2 snapshots/actions; /1 copies are converted at its callback.
//Never retain frontend/VM string pointers.
struct Context {
	pluguiowner_t owner;
	ImGuiContext *imgui = nullptr;
	plugmeshtex_t atlas = 0;
	ImFont *scoresfonts[4] = {};
	ImGuiStyle scoresstyle;
	Renderer renderer;
	unsigned events = 0, actions = 0, nextaction = 0, clicks = 0;
	bool enabled = false, actionoverflow = false, scoretop = false;
	char text[128] = {};
	pluguiaction_t pending[PLUGUI_INPUT_MAX_ACTIONS] = {};
	pluguimodel2_t model = {};
	pluguimodelaction_t modelpending[PLUGUI_INPUT_MAX_ACTIONS] = {};
};
static Context *contexts[2];
#include "input.inc"
static void ModelAction(Context &c, const pluguiwidget_t &w, float value);
#include "scores.inc"
#include "model.inc"
static void Gallery()
{
	ImGui::SetNextWindowPos(ImVec2(40,70),ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(540,330),ImGuiCond_Always);
	ImGui::Begin("Native gallery",nullptr,ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);
	ImGui::TextUnformatted("Dear ImGui 1.91.9b / explicit QC draw site");
	ImGui::Button("Rounded control",ImVec2(150,28)); ImGui::SameLine();
	bool enabled = true; ImGui::Checkbox("Presentation only",&enabled);
	ImGui::BeginChild("scroll",ImVec2(0,200),ImGuiChildFlags_Borders,ImGuiWindowFlags_NoInputs);
	ImGui::SetScrollY(180);
	if (ImGui::BeginTable("bounded",3,ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
	{
		ImGui::TableSetupColumn("ID"); ImGui::TableSetupColumn("Text"); ImGui::TableSetupColumn("Value");
		ImGui::TableHeadersRow();
		ImGuiListClipper rows; rows.Begin(1000);
		while (rows.Step())
			for (int row = rows.DisplayStart; row < rows.DisplayEnd; row++)
			{
				ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("%d",row);
				ImGui::TableNextColumn(); ImGui::TextUnformatted("Bounded visible row");
				ImGui::TableNextColumn(); ImGui::Text("%.2f",row*0.25);
			}
		ImGui::EndTable();
	}
	ImGui::EndChild();
	ImGui::TextUnformatted("No input ownership, model transport or implicit file writes");
	ImGui::End();
	ImGui::SetNextWindowPos(ImVec2(350,345),ImGuiCond_Always);
	ImGui::BeginTooltip(); ImGui::TextUnformatted("Clipped tooltip"); ImGui::EndTooltip();
	//Deterministic order/alpha sentinels built by real ImGui draw-list operations.
	ImDrawList *list = ImGui::GetForegroundDrawList();
	list->AddRectFilled(ImVec2(160,100),ImVec2(400,300),IM_COL32(0,255,0,255),8);
	list->PushClipRect(ImVec2(430,120),ImVec2(500,180),true);
	list->AddRectFilled(ImVec2(410,100),ImVec2(530,200),IM_COL32(255,0,0,128),12);
	list->PopClipRect();
}
static qboolean QDECL Open(const pluguiowner_t *o)
{
	if (!o || o->vm < 1 || o->vm > 2 || !o->generation || contexts[o->vm-1]) return qfalse;
	if (o->owner != (o->vm == PLUGUI_VM_MENU ? GalleryMenu : GalleryClient))
		if (!inputRegistered || !Interactive(*o)) return qfalse;
	Context *c = new (std::nothrow) Context;
	if (!c) return qfalse;
	contexts[o->vm-1] = c; c->owner = *o; stats.opens++;
	ImGuiContext *previous = ImGui::GetCurrentContext();
	c->imgui = ImGui::CreateContext();
	if (!c->imgui) { ImGui::SetCurrentContext(previous); return qfalse; }
	ImGui::SetCurrentContext(c->imgui);
	ImGuiIO &io = ImGui::GetIO();
	io.IniFilename = nullptr; io.LogFilename = nullptr;
	if (Interactive(*o)) io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	ImGuiPlatformIO &platform = ImGui::GetPlatformIO();
	platform.Platform_GetClipboardTextFn = nullptr;
	platform.Platform_SetClipboardTextFn = nullptr;
	platform.Platform_SetImeDataFn = nullptr;
	io.BackendRendererName = "FTE 2DMesh/1";
	io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
	ImGui::StyleColorsDark();
	ImGui::GetStyle().WindowRounding = 8;
	ImGui::GetStyle().FrameRounding = 6;
	ImGui::GetStyle().AntiAliasedLinesUseTex = false;
	if (ScoresOwner(*o))
	{
		c->scoresstyle = ImGui::GetStyle();
		//Actual physical bakes share one immutable atlas. No file IO or rebaking
		//on size changes, and no global/window/framebuffer bitmap enlargement.
		for (unsigned i = 0; i < 4; i++)
		{
			ImFontConfig config;
			config.SizePixels = ScoresFontPixels[i];
			config.OversampleH = config.OversampleV = 1; config.PixelSnapH = true;
			c->scoresfonts[i] = io.Fonts->AddFontDefault(&config);
		}
		io.FontDefault = c->scoresfonts[0];
	}
	unsigned char *rgba = nullptr; int w = 0, h = 0;
	io.Fonts->GetTexDataAsRGBA32(&rgba,&w,&h);
	if (rgba && w > 0 && h > 0 && w <= 4096 && h <= 4096)
		c->atlas = mesh->CreateTextureRGBA(w,h,rgba,size_t(w)*size_t(h)*4);
	if (c->atlas) { stats.uploads++; io.Fonts->SetTexID(ImTextureID(c->atlas)); io.Fonts->ClearTexData(); }
	ImGui::SetCurrentContext(previous);
	return c->atlas ? qtrue : qfalse;
}
static qboolean QDECL Draw(const pluguiframe_t *f)
{
	if (!f || f->structsize != sizeof(*f) || f->owner.vm < 1 || f->owner.vm > 2) return qfalse;
	Context *c = contexts[f->owner.vm-1];
	if (!c || c->owner.owner != f->owner.owner || c->owner.generation != f->owner.generation || !c->atlas) return qfalse;
	ImGuiContext *previous = ImGui::GetCurrentContext(); ImGui::SetCurrentContext(c->imgui);
	ImGuiIO &io = ImGui::GetIO();
	io.DisplaySize = ImVec2(f->pixelwidth,f->pixelheight); io.DisplayFramebufferScale = ImVec2(1,1);
	io.DeltaTime = 0.01f;
	if (ScoresOwner(c->owner)) ScoresFont(*c);
	c->actions = c->nextaction = 0; c->actionoverflow = false;
	ImGui::NewFrame(); stats.frames++;
	bool panelok = true;
	if (ScoresOwner(c->owner)) panelok = ScoresGallery(*c,*f);
	else if (c->model.revision) ModelGallery(*c);
	else if (Interactive(c->owner)) InteractiveGallery(*c); else Gallery();
	ImGui::Render(); c->events = 0;
	bool ok = panelok && !c->actionoverflow && c->renderer.Submit(*ImGui::GetDrawData(),c->atlas,*f,*mesh,stats);
	if (!ok) c->actions = c->nextaction = 0;
	if (!ok) stats.rejected++;
	ImGui::SetCurrentContext(previous);
	return ok ? qtrue : qfalse;
}
static void QDECL Close(const pluguiowner_t *o, unsigned reason)
{
	if (!o || o->vm < 1 || o->vm > 2) return;
	Context *c = contexts[o->vm-1];
	if (!c || c->owner.generation != o->generation || c->owner.owner != o->owner) return;
	if (c->atlas) mesh->DestroyTexture(c->atlas);
	ImGuiContext *previous = ImGui::GetCurrentContext();
	if (c->imgui) ImGui::DestroyContext(c->imgui);
	if (previous != c->imgui) ImGui::SetCurrentContext(previous);
	delete c; contexts[o->vm-1] = nullptr; stats.closes++;
	if (reason < 6) stats.closeReasons[reason]++;
}
static void QDECL Status()
{
	char text[512];
	std::snprintf(text,sizeof(text),"UIIMGUI STATUS version=%s live=%d opens=%u closes=%u frames=%u uploads=%u submits=%u vertices=%u indices=%u commands=%u rejected=%u explicit=%u vm=%u plugin=%u renderer=%u failed=%u\n",
		IMGUI_VERSION,!!contexts[0]+!!contexts[1],stats.opens,stats.closes,stats.frames,stats.uploads,
		stats.submissions,stats.vertices,stats.indices,stats.commands,stats.rejected,
		stats.closeReasons[1],stats.closeReasons[2],stats.closeReasons[3],stats.closeReasons[4],stats.closeReasons[5]);
	core->Print(text);
}
static void QDECL Shutdown()
{
	//Host normally closes owners before shutdown; defensive idempotent cleanup.
	for (Context *c : contexts) if (c) { pluguiowner_t owner = c->owner; Close(&owner,PLUGUI_CLOSE_PLUGIN); }
}
}
extern "C" NATIVEEXPORT qboolean QDECL FTEPlug_Init(plugcorefuncs_t *c)
{
	using namespace FteImGui;
	core = c;
	mesh = static_cast<plugmeshfuncs_t *>(core->GetEngineInterface(plugmeshfuncs_name,sizeof(*mesh)));
	plugcmdfuncs_t *cmd = static_cast<plugcmdfuncs_t *>(core->GetEngineInterface(plugcmdfuncs_name,sizeof(plugcmdfuncs_t)));
	if (!mesh || !cmd) return qfalse;
	pluguiservice_t service = {sizeof(service),PLUGUI_VERSION,PLUGUI_CAP_INDEXED2D,Open,Draw,Close};
	if (!core->ExportFunction("Shutdown",reinterpret_cast<funcptr_t>(Shutdown)) ||
		!cmd->AddCommand("ui_imgui_status",Status,"Report explicitly opened native gallery work") ||
		!core->ExportInterface(pluguiservice_name,&service,sizeof(service))) return qfalse;
	pluguiinputservice_t input = {sizeof(input),PLUGUI_INPUT_VERSION,PLUGUI_INPUT_CAP_EVENTS,Input,Poll};
	inputRegistered = core->ExportInterface(pluguiinputservice_name,&input,sizeof(input)) != qfalse;
	pluguimodelservice_t model = {sizeof(model),PLUGUI_MODEL_VERSION,PLUGUI_MODEL_CAP_WIDGETS,SetModel,PollModel};
	if (inputRegistered && core->ExportInterface(pluguimodelservice_name,&model,sizeof(model)))
	{
		pluguimodelservice2_t dense = {sizeof(dense),PLUGUI_MODEL2_VERSION,PLUGUI_MODEL_CAP_WIDGETS,SetModel2,PollModel};
		core->ExportInterface(pluguimodelservice2_name,&dense,sizeof(dense));
	}
	//Older hosts may reject additive interfaces: prior owners remain usable.
	return qtrue;
}
