//Included by cl_plugin.inc: owned texture tokens and synchronous indexed 2D.
#define PLUGMESH_SLOTS 256
//Vertices a backend draw carries. At 512 a plot's curves were one draw call per 170
//triangles. The arrays are static: Submit is main-thread and never nested.
#define PLUGMESH_DRAW_VERTS 4096
#define PLUGMESH_IMAGE_FLAGS (IF_NOMIPMAP|IF_NOPICMIP|IF_CLAMP|IF_UIPIC|IF_NOPURGE|IF_NOGAMMA|IF_NOSRGB)
static struct
{
	plugin_t *owner;
	plugmeshtex_t token;
	shader_t *shader;
	image_t *image;
	size_t bytes;
} plugmesh_textures[PLUGMESH_SLOTS];
static plugmeshtex_t plugmesh_serial;

static int Plug_Mesh_Find(plugmeshtex_t token)
{
	int i;
	if (!currentplug || !token || qrenderer == QR_NONE)
		return -1;
	for (i = 0; i < PLUGMESH_SLOTS; i++)
		if (plugmesh_textures[i].owner == currentplug && plugmesh_textures[i].token == token)
			return i;
	return -1;
}
static void Plug_Mesh_FreeSlot(int i)
{
	if (R2D_Flush)
		R2D_Flush();
	R_UnloadShader(plugmesh_textures[i].shader);
	Image_DestroyTexture(plugmesh_textures[i].image);
	memset(&plugmesh_textures[i], 0, sizeof(plugmesh_textures[i]));
}
//Called before renderer teardown, not after its shader/image pointers are freed.
void Plug_Mesh_RendererShutdown(void)
{
	int i;
	for (i = 0; i < PLUGMESH_SLOTS; i++)
		if (plugmesh_textures[i].owner)
			Plug_Mesh_FreeSlot(i);
}
static void Plug_Mesh_Close(plugin_t *owner)
{
	int i;
	for (i = 0; i < PLUGMESH_SLOTS; i++)
		if (plugmesh_textures[i].owner == owner)
			Plug_Mesh_FreeSlot(i);
}
static plugmeshtex_t QDECL Plug_Mesh_CreateTexture(unsigned int w, unsigned int h, const qbyte *rgba, size_t bytes)
{
	int i, slot = -1, owned = 0;
	size_t used = 0, total = 0;
	char name[64];
	image_t *image;
	shader_t *shader;
	if (!currentplug || qrenderer == QR_NONE || !rgba || !w || !h || w > 4096 || h > 4096
		|| bytes != (size_t)w*h*4 || bytes > PLUGMESH_MAX_TEXTURE_BYTES || plugmesh_serial == ~(plugmeshtex_t)0)
		return 0;
	for (i = 0; i < PLUGMESH_SLOTS; i++)
	{
		if (!plugmesh_textures[i].owner && slot < 0)
			slot = i;
		total += plugmesh_textures[i].bytes;
		if (plugmesh_textures[i].owner == currentplug)
		{
			owned++;
			used += plugmesh_textures[i].bytes;
		}
	}
	if (slot < 0 || owned >= PLUGMESH_MAX_TEXTURES || used + bytes > PLUGMESH_MAX_TEXTURE_BYTES
		|| total + bytes > 4*PLUGMESH_MAX_TEXTURE_BYTES)
		return 0;
	//Unique engine-owned names prevent resource sharing with legacy named uploads.
	Q_snprintfz(name, sizeof(name), "_plugmesh/%llu", (unsigned long long)++plugmesh_serial);
	image = Image_CreateTexture(name, NULL, PLUGMESH_IMAGE_FLAGS);
	if (!TEXVALID(image))
		return 0;
	Image_Upload(image, TF_RGBA32, (void *)rgba, NULL, w, h, 1, PLUGMESH_IMAGE_FLAGS);
	if (image->status != TEX_LOADED)
	{
		Image_DestroyTexture(image);
		return 0;
	}
	shader = R_RegisterShader(name, SUF_2D,
		"{\ncull none\nprogram default2d\nnomipmaps\nnopicmip\n{\nmap $diffuse\nrgbgen exactvertex\nalphagen vertex\nblendfunc blend\n}\n}\n");
	if (!shader)
	{
		Image_DestroyTexture(image);
		return 0;
	}
	shader->defaulttextures->base = image;
	plugmesh_textures[slot].owner = currentplug;
	plugmesh_textures[slot].token = plugmesh_serial;
	plugmesh_textures[slot].shader = shader;
	plugmesh_textures[slot].image = image;
	plugmesh_textures[slot].bytes = bytes;
	return plugmesh_serial;
}
static qboolean QDECL Plug_Mesh_DestroyTexture(plugmeshtex_t token)
{
	int i = Plug_Mesh_Find(token);
	if (i < 0)
		return false;
	Plug_Mesh_FreeSlot(i);
	return true;
}
static qboolean Plug_Mesh_Finite(float f, float limit)
{
	return isfinite(f) && fabsf(f) <= limit;
}
static qboolean Plug_Mesh_Validate(const plugmeshbatch_t *b)
{
	unsigned int i, j, total = 0;
	if (!currentplug || qrenderer == QR_NONE || !vid.pixelwidth || !vid.pixelheight || !vid.width || !vid.height
		|| !b || b->structsize != sizeof(*b) || !b->vertices || !b->indices || !b->commands
		|| !b->numvertices || b->numvertices > PLUGMESH_MAX_VERTICES
		|| !b->numindices || b->numindices > PLUGMESH_MAX_INDICES
		|| !b->numcommands || b->numcommands > PLUGMESH_MAX_COMMANDS)
		return false;
	for (i = 0; i < b->numvertices; i++)
		for (j = 0; j < 2; j++)
			if (!Plug_Mesh_Finite(b->vertices[i].xy[j], 10000000) || !Plug_Mesh_Finite(b->vertices[i].uv[j], 10000))
				return false;
	for (i = 0; i < b->numcommands; i++)
	{
		const plugmeshcommand_t *c = &b->commands[i];
		if (Plug_Mesh_Find(c->texture) < 0 || c->firstindex > b->numindices
			|| c->indexcount > b->numindices - c->firstindex || c->indexcount%3
			|| c->vertexoffset >= b->numvertices || c->indexcount > PLUGMESH_MAX_INDICES-total)
			return false;
		total += c->indexcount;
		for (j = 0; j < 4; j++)
			if (!Plug_Mesh_Finite(c->clip[j], 10000000))
				return false;
		for (j = 0; j < c->indexcount; j++)
			if (b->indices[c->firstindex+j] >= b->numvertices-c->vertexoffset)
				return false;
	}
	return true;
}

typedef struct
{
	float xy[2], uv[2], rgba[4];
} plugmeshclipvertex_t;
//Sutherland-Hodgman; a triangle clipped by a rectangle has at most seven vertices.
static unsigned int Plug_Mesh_Clip(const plugmeshclipvertex_t *in, unsigned int n,
	plugmeshclipvertex_t *out, unsigned int axis, float bound, qboolean lower)
{
	unsigned int i, j, count = 0;
	const plugmeshclipvertex_t *a, *b;
	qboolean ai, bi;
	float t;
	if (!n)
		return 0;
	a = &in[n-1];
	ai = lower ? a->xy[axis] >= bound : a->xy[axis] <= bound;
	for (i = 0; i < n; i++)
	{
		b = &in[i];
		bi = lower ? b->xy[axis] >= bound : b->xy[axis] <= bound;
		if (ai != bi)
		{
			t = (bound-a->xy[axis])/(b->xy[axis]-a->xy[axis]);
			for (j = 0; j < 2; j++)
			{
				out[count].xy[j] = a->xy[j] + t*(b->xy[j]-a->xy[j]);
				out[count].uv[j] = a->uv[j] + t*(b->uv[j]-a->uv[j]);
			}
			out[count].xy[axis] = bound;
			for (j = 0; j < 4; j++)
				out[count].rgba[j] = a->rgba[j] + t*(b->rgba[j]-a->rgba[j]);
			count++;
		}
		if (bi)
			out[count++] = *b;
		a = b;
		ai = bi;
	}
	return count;
}
static qboolean QDECL Plug_Mesh_Submit(const plugmeshbatch_t *b)
{
	static vecV_t xyz[PLUGMESH_DRAW_VERTS];
	static vec2_t uv[PLUGMESH_DRAW_VERTS];
	static avec4_t rgba[PLUGMESH_DRAW_VERTS];
	static index_t indices[PLUGMESH_DRAW_VERTS*3];
	mesh_t mesh;
	plugmeshclipvertex_t a[8], scratch[8];
	unsigned int ci, i, j, k, n;
	float clip[4], sx, sy;
	shader_t *shader;
	if (!Plug_Mesh_Validate(b))
		return false;
	if (R2D_Flush)
		R2D_Flush();
	sx = (float)vid.width/vid.pixelwidth;
	sy = (float)vid.height/vid.pixelheight;
	memset(&mesh, 0, sizeof(mesh));
	mesh.xyz_array = xyz;
	mesh.st_array = uv;
	mesh.colors4f_array[0] = rgba;
	mesh.indexes = indices;
	for (ci = 0; ci < b->numcommands; ci++)
	{
		const plugmeshcommand_t *c = &b->commands[ci];
		shader = plugmesh_textures[Plug_Mesh_Find(c->texture)].shader;
		clip[0] = max(0, c->clip[0]);
		clip[1] = max(0, c->clip[1]);
		clip[2] = min(vid.pixelwidth, c->clip[2]);
		clip[3] = min(vid.pixelheight, c->clip[3]);
		Plug_NativeUI_Clip(clip); //service cannot widen its explicit QC caller's clip
		if (clip[0] >= clip[2] || clip[1] >= clip[3])
			continue;
		for (i = 0; i < c->indexcount; i += 3)
		{
			for (j = 0; j < 3; j++)
			{
				const plugmeshvertex_t *v = &b->vertices[c->vertexoffset+b->indices[c->firstindex+i+j]];
				Vector2Copy(v->xy, a[j].xy);
				Vector2Copy(v->uv, a[j].uv);
				for (k = 0; k < 4; k++)
					a[j].rgba[k] = v->rgba[k]/255.0f;
			}
			//Nearly every triangle lies wholly inside its clip, where the four passes
			//hand back the same three vertices in the same order (their tests are these,
			//inclusive). They are skipped there.
			if (a[0].xy[0] >= clip[0] && a[0].xy[0] <= clip[2] && a[0].xy[1] >= clip[1] && a[0].xy[1] <= clip[3] &&
				a[1].xy[0] >= clip[0] && a[1].xy[0] <= clip[2] && a[1].xy[1] >= clip[1] && a[1].xy[1] <= clip[3] &&
				a[2].xy[0] >= clip[0] && a[2].xy[0] <= clip[2] && a[2].xy[1] >= clip[1] && a[2].xy[1] <= clip[3])
				n = 3;
			else
			{
				n = Plug_Mesh_Clip(a, 3, scratch, 0, clip[0], true);
				n = Plug_Mesh_Clip(scratch, n, a, 0, clip[2], false);
				n = Plug_Mesh_Clip(a, n, scratch, 1, clip[1], true);
				n = Plug_Mesh_Clip(scratch, n, a, 1, clip[3], false);
			}
			if (n < 3)
				continue;
			if (mesh.numvertexes+n > PLUGMESH_DRAW_VERTS)
			{
				BE_DrawMesh_Single(shader, &mesh, NULL, 0);
				mesh.numvertexes = mesh.numindexes = 0;
			}
			for (j = 0; j < n; j++)
			{
				k = mesh.numvertexes+j;
				memset(xyz[k], 0, sizeof(xyz[k]));
				xyz[k][0] = a[j].xy[0]*sx;
				xyz[k][1] = a[j].xy[1]*sy;
				Vector2Copy(a[j].uv, uv[k]);
				Vector4Copy(a[j].rgba, rgba[k]);
			}
			for (j = 2; j < n; j++)
			{
				indices[mesh.numindexes++] = mesh.numvertexes;
				indices[mesh.numindexes++] = mesh.numvertexes+j-1;
				indices[mesh.numindexes++] = mesh.numvertexes+j;
			}
			mesh.numvertexes += n;
		}
		if (mesh.numindexes)
			BE_DrawMesh_Single(shader, &mesh, NULL, 0);
		mesh.numvertexes = mesh.numindexes = 0;
	}
	//No global colour, flags or BE_Scissor writes; all mesh draws are synchronous.
	return true;
}
