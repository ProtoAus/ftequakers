/* Direct v49 knife animation groups. Format facts: studio frame-animation
 * streams have independent constant/per-frame lanes and 48-bit quaternions.
 * No recursive model loads; every MDL/ANI offset is checked before decoding. */
static const char *HL2_BoundedName(const qbyte *data, size_t size, size_t base, unsigned int relative)
{
	size_t offset;
	if (base > size || relative > size-base) return NULL;
	offset = base+relative;
	if (!HL2_EventSpan(size, offset, 1, 1) || !memchr(data+offset, 0, size-offset)) return NULL;
	return (const char*)data+offset;
}

static qboolean HL2_WeaponPath(const char *name, char *normalized, size_t capacity, const char *suffix)
{
	size_t i, length;
	if (!name || strlen(name) >= capacity) return false;
	Q_strlcpy(normalized, name, capacity);
	for (i = 0; normalized[i]; i++) if (normalized[i] == '\\') normalized[i] = '/';
	length = strlen(normalized);
	return length >= 27 && !strncmp(normalized, "models/weapons/v_knife_", 23) &&
		!strstr(normalized, "..") && !strchr(normalized, ':') && !strcmp(normalized+length-4, suffix);
}

static qbyte *HL2_WeaponFile(const char *path, size_t maximum, size_t *size)
{
	qbyte *data;
	vfsfile_t *file = filefuncs->OpenVFS(path, "rb", FS_GAME);
	if (!file) return NULL;
	*size = file->GetLen(file);
	if (!*size || *size > maximum) { file->Close(file); return NULL; }
	data = plugfuncs->Malloc(*size);
	if (file->ReadBytes(file, data, *size) != (int)*size)
	{
		plugfuncs->Free(data); file->Close(file); return NULL;
	}
	file->Close(file);
	return data;
}

static qboolean HL2_WeaponQuat(const qbyte *data, qboolean smallest, vec4_t quat)
{
	unsigned short value[3];
	unsigned int i, first;
	float sum = 0;
	memcpy(value, data, sizeof(value));
	if (smallest)
	{
		first = ((value[0] >> 15) << 1) | (value[1] >> 15);
		for (i = 0; i < 3; i++)
		{
			quat[(first+i)&3] = ((int)(value[i]&32767)-16384) / 23168.0f;
			sum += quat[(first+i)&3]*quat[(first+i)&3];
		}
	}
	else
	{
		first = 0;
		quat[0] = ((int)value[0]-32768) / 32768.0f;
		quat[1] = ((int)value[1]-32768) / 32768.0f;
		quat[2] = ((int)(value[2]&32767)-16384) / 16384.0f;
		for (i = 0; i < 3; i++) sum += quat[i]*quat[i];
	}
	if (sum > 1.0002f) return false;
	quat[(first+3)&3] = sqrtf(max(0, 1-sum));
	if (value[2]&32768) quat[(first+3)&3] = -quat[(first+3)&3];
	return true;
}

static unsigned int HL2_WeaponLane(unsigned int flags, qboolean constant)
{
	if (constant) return ((flags&64)?1:0) | ((flags&2)?2:0) | ((flags&1)?4:0) | ((flags&32)?8:0);
	return ((flags&128)?1:0) | ((flags&8)?2:0) | ((flags&4)?4:0) | ((flags&16)?8:0);
}

static qboolean HL2_WeaponValues(const qbyte **data, unsigned int lane, vec3_t position, vec4_t rotation)
{
	unsigned short half[3];
	if (lane&3)
	{
		if (!HL2_WeaponQuat(*data, (lane&1)!=0, rotation)) return false;
		*data += 6;
	}
	if (lane&4)
	{
		memcpy(half, *data, sizeof(half)); *data += 6;
		position[0] = HalfToFloat(half[0]); position[1] = HalfToFloat(half[1]); position[2] = HalfToFloat(half[2]);
	}
	if (lane&8) { memcpy(position, *data, sizeof(vec3_t)); *data += sizeof(vec3_t); }
	return true;
}

/* boneofs is opaque; advance frames in float elements, not bytes. */
static float *HL2_WeaponFrameBase(void *buffer, size_t frame, size_t bones)
{
	return (float*)buffer+frame*bones*12;
}

static qboolean HL2_WeaponFrame(const qbyte *data, size_t size, size_t offset,
	const hl2mdlbone_t *bones, unsigned int count, unsigned int frame, float *poses)
{
	// CS:GO Legacy has three offsets/lengths AND three reserved words. Its
	// bone flags start at byte 24, not the 12-byte layout used by some readers.
	unsigned int header[6], i, lane, constantbytes = 0, framebytes = 0;
	const qbyte *flags, *constant, *varying;
	if (!count || count > 256 || frame >= 4096 || !HL2_EventSpan(size, offset, 1, sizeof(header)) ||
		!HL2_EventSpan(size, offset+sizeof(header), count, 1)) return false;
	memcpy(header, data+offset, sizeof(header));
	if (header[3] || header[4] || header[5]) return false; // fail closed on another layout
	flags = data+offset+sizeof(header);
	for (i = 0; i < count; i++)
	{
		lane = HL2_WeaponLane(flags[i], true);
		if ((lane&3)==3 || (lane&12)==12) return false;
		constantbytes += ((lane&3)?6:0)+((lane&4)?6:0)+((lane&8)?12:0);
		lane = HL2_WeaponLane(flags[i], false);
		if ((lane&3)==3 || (lane&12)==12) return false;
		framebytes += ((lane&3)?6:0)+((lane&4)?6:0)+((lane&8)?12:0);
	}
	if (header[0] > size-offset || header[1] > size-offset) return false;
	if (constantbytes && (header[0] < sizeof(header)+count || !HL2_EventSpan(size, offset+header[0], 1, constantbytes))) return false;
	if (framebytes && (header[1] < sizeof(header)+count || header[2] < framebytes || header[2] > 6144 ||
		!HL2_EventSpan(size, offset+header[1], frame+1, header[2]))) return false;
	constant = data+offset+header[0];
	varying = data+offset+header[1]+(framebytes ? frame*header[2] : 0);
	for (i = 0; i < count; i++)
	{
		vec3_t position, scale = {1,1,1};
		vec4_t rotation;
		unsigned int j;
		memcpy(position, bones[i].pos, sizeof(position));
		memcpy(rotation, bones[i].quat, sizeof(rotation));
		if (!HL2_WeaponValues(&constant, HL2_WeaponLane(flags[i], true), position, rotation) ||
			!HL2_WeaponValues(&varying, HL2_WeaponLane(flags[i], false), position, rotation)) return false;
		for (j = 0; j < 3; j++) if (!isfinite(position[j])) return false;
		for (j = 0; j < 4; j++) if (!isfinite(rotation[j])) return false;
		modfuncs->GenMatrixPosQuat4Scale(position, rotation, scale, poses+i*12);
	}
	return true;
}

static qboolean HL2_WeaponPose(const hl2mdlheader_t *header, const qbyte *mdl, size_t mdlsize,
	const qbyte *ani, size_t anisize, const hl2mdlanim_t *anim, unsigned int frame,
	const hl2mdlbone_t *bones, float *poses)
{
	struct poseofs_s pose = anim->defpose;
	const qbyte *data = mdl;
	size_t size = mdlsize, base = (const qbyte*)anim-mdl;
	qofs_t offset;
	if (anim->posespersection)
	{
		unsigned int section;
		if (anim->numframes > anim->posespersection && frame == anim->numframes-1)
		{
			section = anim->numframes/anim->posespersection+1;
			frame = 0;
		}
		else { section = frame/anim->posespersection; frame %= anim->posespersection; }
		if (anim->ofs_posesections > mdlsize-base || !HL2_EventSpan(mdlsize, base+anim->ofs_posesections, section+1, sizeof(pose))) return false;
		memcpy(&pose, mdl+base+anim->ofs_posesections+section*sizeof(pose), sizeof(pose));
	}
	if (pose.externalblock)
	{
		unsigned int block[2];
		if (!ani || pose.externalblock >= header->num_aniblocks) return false;
		memcpy(block, mdl+header->ofs_aniblocks+pose.externalblock*sizeof(block), sizeof(block));
		if (block[0] > block[1] || block[1] > anisize) return false;
		data = ani; size = anisize; base = block[0];
	}
	// Reused sections legitimately point backwards into an earlier ANI block.
	offset = (qofs_t)base+(int)pose.ofs_pose;
	if (offset < 0 || (size_t)offset >= size) return false;
	return HL2_WeaponFrame(data, size, offset, bones, header->num_bones, frame, poses);
}

static void Mod_HL2_IncludeWeaponAnimations(hl2parsecontext_t *ctx, size_t parentsize)
{
	const qbyte *parent = (const qbyte*)ctx->header;
	hl2mdlheader_t *h;
	const hl2mdlbone_t *bones;
	const hl2mdlanim_t *anim;
	const char *path, *name, *failure = "include path";
	qbyte *data = NULL, *ani = NULL;
	size_t size, anisize = 0, i, j, frame, totalposes = 0;
	unsigned int nameofs;
	int map[256];
	float *pose = NULL;
	galiasanimation_t *animations;
	char normalized[MAX_QPATH];
	qboolean loaded = false;

	if (ctx->header->version != 49 || !ctx->header->num_bones || ctx->num_animations || !ctx->header->_336 ||
		strncmp(ctx->mod->name, "models/weapons/v_knife_", 23)) return;
	if (ctx->header->_336 != 1 || !HL2_EventSpan(parentsize, ctx->header->_340, 1, 8)) goto done;
	memcpy(&nameofs, parent+ctx->header->_340+4, sizeof(nameofs));
	path = HL2_BoundedName(parent, parentsize, ctx->header->_340, nameofs);
	if (!HL2_WeaponPath(path, normalized, sizeof(normalized), ".mdl")) goto done;
	data = HL2_WeaponFile(normalized, 16*1024*1024, &size);
	failure = "included header";
	if (!data || size < sizeof(*h)) goto done;
	h = (hl2mdlheader_t*)data;
	if (memcmp(data, "IDST", 4) || h->version != 49 || h->_336 ||
		(h->ofs_bones & 3) || (h->ofs_anims & 3) || h->num_aniblocks > 64 ||
		!h->num_bones || h->num_bones > 256 || !h->num_anims || h->num_anims > 128 ||
		!HL2_EventSpan(size, h->ofs_bones, h->num_bones, sizeof(*bones)) ||
		!HL2_EventSpan(size, h->ofs_anims, h->num_anims, sizeof(*anim)) ||
		!HL2_EventSpan(size, h->ofs_aniblocks, h->num_aniblocks, 8)) goto done;
	if (h->num_aniblocks)
	{
		path = HL2_BoundedName(data, size, 0, h->ofs_aniname);
		failure = "external animation file";
		if (!HL2_WeaponPath(path, normalized, sizeof(normalized), ".ani")) goto done;
		ani = HL2_WeaponFile(normalized, 64*1024*1024, &anisize);
		if (!ani) goto done;
	}
	failure = "bone mapping";
	bones = (const hl2mdlbone_t*)(data+h->ofs_bones);
	for (i = 0; i < h->num_bones; i++)
	{
		name = HL2_BoundedName(data, size, h->ofs_bones+i*sizeof(*bones), bones[i].name_ofs);
		if (!name || bones[i].parent >= (int)i || bones[i].parent < -1) goto done;
	}
	for (i = 0; i < ctx->header->num_bones; i++)
	{
		map[i] = -1;
		for (j = 0; j < h->num_bones; j++)
		{
			name = (const char*)&bones[j]+bones[j].name_ofs;
			if (!strcmp(ctx->bones[i].name, name)) { map[i] = j; break; }
		}
		if (map[i] >= 0)
		{
			int a = ctx->bones[i].parent, b = bones[map[i]].parent;
			if ((a < 0) != (b < 0)) goto done;
			if (a >= 0 && strcmp(ctx->bones[a].name, (const char*)&bones[b]+bones[b].name_ofs)) goto done;
		}
	}
	failure = "animation table";
	anim = (const hl2mdlanim_t*)(data+h->ofs_anims);
	for (i = 0; i < h->num_anims; i++)
	{
		if (!anim[i].numframes || anim[i].numframes > 4096 || !(anim[i].fps > 0 && anim[i].fps <= 240) ||
			!(anim[i].loop & 64) || (anim[i].loop & ~67u) ||
			!HL2_BoundedName(data, size, h->ofs_anims+i*sizeof(*anim), anim[i].name_ofs)) goto done;
		totalposes += anim[i].numframes;
		if (totalposes > (64*1024*1024)/(48*ctx->header->num_bones)) goto done;
	}
	pose = plugfuncs->Malloc(h->num_bones*48);
	animations = plugfuncs->GMalloc(&ctx->mod->memgroup, sizeof(*animations)*h->num_anims);
	memset(animations, 0, sizeof(*animations)*h->num_anims);
	failure = "frame stream";
	for (i = 0; i < h->num_anims; i++)
	{
		galiasanimation_t *a = &animations[i];
		a->skeltype = SKEL_RELATIVE;
		a->numposes = anim[i].numframes;
		a->loop = (anim[i].loop & 1) != 0;
		a->rate = anim[i].fps;
		a->action = -1;
		Q_strlcpy(a->name, (const char*)&anim[i]+anim[i].name_ofs, sizeof(a->name));
		a->boneofs = plugfuncs->GMalloc(&ctx->mod->memgroup, 48*a->numposes*ctx->header->num_bones);
		for (frame = 0; frame < a->numposes; frame++)
		{
			float *out = HL2_WeaponFrameBase(a->boneofs, frame, ctx->header->num_bones);
			if (!HL2_WeaponPose(h, data, size, ani, anisize, &anim[i], frame, bones, pose)) goto done;
			for (j = 0; j < ctx->header->num_bones; j++)
				memcpy(out+j*12, map[j] < 0 ? ctx->baserelpose+j*12 : pose+map[j]*12, 48);
			for (j = 0; j < ctx->header->num_bones*12; j++) if (!isfinite(out[j])) goto done;
		}
	}
	if (!Mod_ParseModelEvents(ctx->mod, animations, h->num_anims))
		Mod_HL2_SequenceEvents(ctx->mod, h, size, animations, h->num_anims);
	ctx->weapon_animations = true;
	ctx->ofs_animations = animations;
	ctx->num_animations = h->num_anims;
	loaded = true;
	Con_DPrintf("%s: imported %u named-bone animations\n", ctx->mod->name, h->num_anims);
done:
	if (!loaded) Con_Printf("%s: unsupported or invalid included weapon animations (%s)\n", ctx->mod->name, failure);
	plugfuncs->Free(pose); plugfuncs->Free(ani); plugfuncs->Free(data);
}
