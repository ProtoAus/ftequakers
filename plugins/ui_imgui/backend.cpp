#include "backend.h"
#include <cmath>
#include <cstring>
#include <algorithm>

namespace FteImGui {
static bool Finite(float v) { return std::isfinite(v); }
static float X(float v, const ImDrawData &d) { return (v-d.DisplayPos.x)*d.FramebufferScale.x; }
static float Y(float v, const ImDrawData &d) { return (v-d.DisplayPos.y)*d.FramebufferScale.y; }

bool Renderer::Flush(plugmeshfuncs_t &mesh, Counters &stats)
{
	if (!nv) return true;
	plugmeshbatch_t batch = {sizeof(batch),vertices,indices,commands,nv,nv,nc};
	if (!mesh.Submit(&batch)) return false;
	stats.submissions++; stats.vertices += nv; stats.indices += nv; stats.commands += nc;
	nv = nc = 0;
	return true;
}

bool Renderer::Submit(const ImDrawData &d, plugmeshtex_t atlas, const pluguiframe_t &f,
	plugmeshfuncs_t &mesh, Counters &stats)
{
	nv = nc = 0;
	//Preflight the entire draw data before the first host submission.
	if (!d.Valid || !atlas || d.CmdListsCount < 0 || d.CmdListsCount > MaxLists ||
		d.CmdLists.Size != d.CmdListsCount || (d.CmdListsCount && !d.CmdLists.Data) ||
		!Finite(d.DisplayPos.x) || !Finite(d.DisplayPos.y) ||
		!Finite(d.DisplaySize.x) || !Finite(d.DisplaySize.y) || d.DisplaySize.x <= 0 || d.DisplaySize.y <= 0 ||
		!Finite(d.FramebufferScale.x) || !Finite(d.FramebufferScale.y) || d.FramebufferScale.x <= 0 || d.FramebufferScale.y <= 0 ||
		!Finite(f.pixelwidth) || !Finite(f.pixelheight) || f.pixelwidth <= 0 || f.pixelheight <= 0 ||
		!Finite(d.DisplaySize.x*d.FramebufferScale.x) || !Finite(d.DisplaySize.y*d.FramebufferScale.y) ||
		std::fabs(d.DisplaySize.x*d.FramebufferScale.x-f.pixelwidth) > 0.01f ||
		std::fabs(d.DisplaySize.y*d.FramebufferScale.y-f.pixelheight) > 0.01f)
		return false;
	for (float v : f.clip) if (!Finite(v)) return false;
	unsigned totalv = 0, totali = 0, totalc = 0, consumed = 0;
	for (int n = 0; n < d.CmdListsCount; n++)
	{
		const ImDrawList *list = d.CmdLists[n];
		if (!list || list->VtxBuffer.Size < 0 || list->IdxBuffer.Size < 0 || list->CmdBuffer.Size < 0 ||
			(list->VtxBuffer.Size && !list->VtxBuffer.Data) || (list->IdxBuffer.Size && !list->IdxBuffer.Data) ||
			(list->CmdBuffer.Size && !list->CmdBuffer.Data)) return false;
		if (unsigned(list->VtxBuffer.Size) > MaxVertices-totalv || unsigned(list->IdxBuffer.Size) > MaxIndices-totali ||
			unsigned(list->CmdBuffer.Size) > MaxCommands-totalc) return false;
		totalv += list->VtxBuffer.Size; totali += list->IdxBuffer.Size; totalc += list->CmdBuffer.Size;
		for (const ImDrawVert &v : list->VtxBuffer)
			if (!Finite(X(v.pos.x,d)) || !Finite(Y(v.pos.y,d)) || !Finite(v.uv.x) || !Finite(v.uv.y)) return false;
		for (const ImDrawCmd &c : list->CmdBuffer)
		{
			if (c.UserCallback)
			{
				if (c.UserCallback != ImDrawCallback_ResetRenderState) return false;
				continue; //each host Submit owns/restores its state; never execute arbitrary callbacks
			}
			if (c.GetTexID() != ImTextureID(atlas) || c.ElemCount % 3 || c.IdxOffset > unsigned(list->IdxBuffer.Size) ||
				c.ElemCount > unsigned(list->IdxBuffer.Size)-c.IdxOffset || c.ElemCount > MaxIndices-consumed ||
				c.VtxOffset > unsigned(list->VtxBuffer.Size)) return false;
			consumed += c.ElemCount;
			if (!Finite(X(c.ClipRect.x,d)) || !Finite(Y(c.ClipRect.y,d)) ||
				!Finite(X(c.ClipRect.z,d)) || !Finite(Y(c.ClipRect.w,d))) return false;
			for (unsigned k = 0; k < c.ElemCount; k++)
				if (unsigned(list->IdxBuffer[c.IdxOffset+k]) >= unsigned(list->VtxBuffer.Size)-c.VtxOffset) return false;
		}
	}
	if (d.TotalVtxCount != int(totalv) || d.TotalIdxCount != int(totali)) return false;
	for (int n = 0; n < d.CmdListsCount; n++)
	{
		const ImDrawList &list = *d.CmdLists[n];
		for (const ImDrawCmd &c : list.CmdBuffer)
		{
			if (c.UserCallback || !c.ElemCount) continue;
			float clip[4] = {std::max(X(c.ClipRect.x,d),f.clip[0]), std::max(Y(c.ClipRect.y,d),f.clip[1]),
				std::min(X(c.ClipRect.z,d),f.clip[2]), std::min(Y(c.ClipRect.w,d),f.clip[3])};
			if (clip[0] >= clip[2] || clip[1] >= clip[3]) continue;
			unsigned k = 0;
			while (k < c.ElemCount)
			{
				if (nv+3 > PLUGMESH_MAX_VERTICES || nc == PLUGMESH_MAX_COMMANDS)
					if (!Flush(mesh,stats)) return false;
				plugmeshcommand_t &out = commands[nc++];
				out = {}; out.texture = atlas; std::memcpy(out.clip,clip,sizeof(clip)); out.firstindex = nv;
				unsigned end = std::min(c.ElemCount,k+((PLUGMESH_MAX_VERTICES-nv)/3)*3);
				for (; k < end; k++, nv++)
				{
					const ImDrawVert &v = list.VtxBuffer[c.VtxOffset+list.IdxBuffer[c.IdxOffset+k]];
					vertices[nv].xy[0] = X(v.pos.x,d); vertices[nv].xy[1] = Y(v.pos.y,d);
					vertices[nv].uv[0] = v.uv.x; vertices[nv].uv[1] = v.uv.y;
					vertices[nv].rgba[0] = (v.col >> IM_COL32_R_SHIFT) & 255;
					vertices[nv].rgba[1] = (v.col >> IM_COL32_G_SHIFT) & 255;
					vertices[nv].rgba[2] = (v.col >> IM_COL32_B_SHIFT) & 255;
					vertices[nv].rgba[3] = (v.col >> IM_COL32_A_SHIFT) & 255;
					indices[nv] = nv; out.indexcount++;
				}
			}
		}
	}
	return Flush(mesh,stats);
}
}
