#ifndef FTE_IMGUI_BACKEND_H
#define FTE_IMGUI_BACKEND_H
#include "quakedef.h"
#include "plugin.h"
#undef snprintf
#undef vsnprintf
#include "vendor/imgui.h"

namespace FteImGui {
//Owners are explicit QC draw sites; production input policy remains in QC.
constexpr unsigned GalleryMenu = 101, GalleryClient = 202;
constexpr unsigned InteractiveMenu = 103, InteractiveClient = 204;
constexpr unsigned ScoresClient = 205;
constexpr unsigned PlotsClient = 206;
constexpr int MaxLists = 64, MaxVertices = 262144, MaxIndices = 786432, MaxCommands = 4096;
struct Counters {
	unsigned opens = 0, closes = 0, frames = 0, uploads = 0, submissions = 0;
	unsigned vertices = 0, indices = 0, commands = 0, rejected = 0;
	unsigned closeReasons[6] = {};
};
class Renderer {
	//Triangle expansion bounds arbitrary 16/32-bit indices and VtxOffset without
	//retaining ImGui pointers, mapping allocations or changing the host mesh ABI.
	plugmeshvertex_t vertices[PLUGMESH_MAX_VERTICES];
	unsigned indices[PLUGMESH_MAX_VERTICES];
	plugmeshcommand_t commands[PLUGMESH_MAX_COMMANDS];
	unsigned nv = 0, nc = 0;
	bool Flush(plugmeshfuncs_t &mesh, Counters &stats);
public:
	bool Submit(const ImDrawData &data, plugmeshtex_t atlas, const pluguiframe_t &frame,
		plugmeshfuncs_t &mesh, Counters &stats);
};
}
#endif
