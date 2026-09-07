#include "../plugin.h"

static plugimagefuncs_t *imagefuncs;
extern cvar_t *hl2_texdiag;	//FTESurf Patch 239, registered in mod_vbsp.c as "r_texdiag"

//many of these look like dupes, not really sure how they're meant to work. probably legacy.
typedef enum {
	VMF_INVALID=-1,
	VMF_RGBA8=0,
	VMF_ABGR8=1,	//FTESurf Patch 239: 421 instances across 73 maps.  No PTI_ABGR8 exists; byte-reversed in place, below.
	VMF_RGB8=2,
	VMF_BGR8=3,
//	VMF_RGB565=4,	//zero instances in the library; BGR565 (17) is the one that shows up, and FTE has no PTI_BGR565
	VMF_I8=5,
	VMF_IA8=6,
//	VMF_P8=7,
//	VMF_A8=8,
//	VMF_RGB8_BS=9,
//	VMF_BGR8_BS=10,
//	VMF_ARGB_BS=11,
	VMF_BGRA8=12,
	VMF_BC1=13,
	VMF_BC2=14,
	VMF_BC3=15,
	VMF_BGRX8=16,
//	VMF_BGR565=17,
//	VMF_BGRX5551=18,
//	VMF_BGRA4444=19,
	VMF_BC1A=20,
//	VMF_BGRA5551=21,
	VMF_UV88=22,
//	VMF_UVWQ8=23,
	VMF_RGBA16F=24,
	VMF_RGBA16N=25,	//FTESurf Patch 239: an exact PTI_RGBA16 match, so it costs nothing to accept
//	VMF_UVLX8=26,
	VMF_MAX,
	/*
	FTESurf Patch 239: Strata Source -- the VTF 7.6 branch Momentum Mod ships --
	appends its own block formats high, clear of the console ids the other
	branches took.  Neither is in any published IMAGE_FORMAT enum, so they were
	identified from the shipped files themselves: both are 16 bytes per 4x4
	block (exact file-size arithmetic on a 1024x1024/11-mip sample and a
	32x32/6-mip/6-face one), 70's blocks are 99.7% BC7 mode 6 with zero invalid
	blocks -- a textbook BC7 encoder signature -- and 71 appears only on
	`.hdr.vtf` env cubemaps, which is BC6H's whole purpose.  375 instances
	across 40 maps between them.
	*/
	VMF_BC7=70,
	VMF_BC6H=71
} fmtfmt_t;
static uploadfmt_t ImageVTF_VtfToFTE(fmtfmt_t f, unsigned int vtfflags)
{
	switch(f)
	{
	case VMF_BC1:
		/*
		FTESurf Patch 239: TEXTUREFLAGS_ONEBITALPHA.

		DXT1 always decodes a c0<=c1 block's index-3 texels as transparent, so
		vtex records the intent in the header FLAGS and keeps writing plain
		DXT1 -- which is why IMAGE_FORMAT_DXT1_ONEBITALPHA (20), the case
		handled just below, appears ZERO times in the whole library while the
		flag appears on ordinary DXT1.  Reading the format alone therefore
		never saw a cutout.

		Honest scope: 2 instances, one unique file (materials/plar/mipmap.vtf,
		in bhop_/surf_tripportals), and that file's 1367 blocks contain no
		texel that decodes transparent -- so this changes no pixel that exists
		today.  It is here because the next map to ship one should not have to
		rediscover why its cutout is opaque.
		*/
		return (vtfflags & 0x1000)?PTI_BC1_RGBA:PTI_BC1_RGB;
	case VMF_BC1A:
		return PTI_BC1_RGBA;
	case VMF_BC2:
		return PTI_BC2_RGBA;
	case VMF_BC3:
		return PTI_BC3_RGBA;
	case VMF_RGB8:
		return PTI_RGB8;
	case VMF_RGBA8:
		return PTI_RGBA8;
	case VMF_BGR8:
		return PTI_BGR8;
	case VMF_BGRA8:
		return PTI_BGRA8;
	case VMF_BGRX8:
		return PTI_BGRX8;
	case VMF_RGBA16F:
		return PTI_RGBA16F;
	case VMF_RGBA16N:
		return PTI_RGBA16;
	case VMF_ABGR8:
		return PTI_RGBA8;	//byte order corrected in place after the mips are laid out, see below
	case VMF_BC7:
		return PTI_BC7_RGBA;
	case VMF_BC6H:
		return PTI_BC6_RGB_UFLOAT;
	case VMF_UV88:
		return PTI_RG8;
	case VMF_I8:
		return PTI_L8;
	case VMF_IA8:
		return PTI_L8A8;
	case VMF_INVALID:
		return PTI_INVALID;

	default:
		return PTI_INVALID;
	}
}
struct pendingtextureinfo *Image_ReadVTFFile(unsigned int flags, const char *fname, qbyte *filedata, size_t filesize)
{
	struct vtf_s
	{
		char magic[4];
		unsigned int major,minor;
		unsigned int headersize;

		unsigned short width, height;
		unsigned int flags;
		unsigned short numframes, firstframe;
		unsigned int pad1;

		vec3_t reflectivity;
		float pad2;

		float bumpmapscale;
		unsigned int imgformat;
		unsigned char mipmapcount;
		unsigned char lowresfmt_misaligned[4];
		unsigned char lowreswidth;
		unsigned char lowresheight;

		//7.2
		unsigned char depth_misaligned[2];
		//7.3
		unsigned char pad3[3];
		unsigned int numresources;
		/*
		FTESurf Patch 239: the resource table starts at 80, not at 72.

		Valve's VTFFileHeader_t ends `unsigned int numResources;` followed by
		`unsigned char pad5[8];`, and this struct omitted the padding -- so
		`filedata+sizeof(*vtf)` addressed the table eight bytes, i.e. exactly
		one entry, early.  Every restable[i] then read entry i-1, and the loop
		(i < numresources) fell one short of the last real entry.

		Measured, not assumed: across 3807 v7.3+ VTFs packed in the map
		library, headerSize == 80 + numresources*8 in 3807 of 3807 cases,
		bytes 72..79 are zero in all of them, and the first real resource tag
		(0x30 high-res, or 0x01 low-res) sits at 80.

		It has been harmless by luck.  The files where the walk missed the last
		entry all fell through to the headerSize path below, which computed a
		byte-identical offset every time.  That luck ends at the first 7.6 file
		whose image resource is last in the table -- Strata's auxiliary
		compression puts other resources ahead of it.
		*/
		unsigned char pad4[8];
	} *vtf;
	fmtfmt_t vmffmt, lrfmt;
	unsigned int bw, bh, bd, bb;
	qbyte *end = filedata + filesize;
	unsigned int faces, storedfaces, frame, frames, miplevel, miplevels, img;
	unsigned int w, h, d = 1;
	size_t	datasize;
	unsigned int version;

	struct pendingtextureinfo *mips;

	vtf = (void*)filedata;

	if (memcmp(vtf->magic, "VTF\0", 4))
		return NULL;

	// erysdren: do endian swapping
	vtf->major = LittleLong(vtf->major);
	vtf->minor = LittleLong(vtf->minor);
	vtf->headersize = LittleLong(vtf->headersize);
	vtf->width = LittleShort(vtf->width);
	vtf->height = LittleShort(vtf->height);
	vtf->flags = LittleLong(vtf->flags);
	vtf->numframes = LittleShort(vtf->numframes);
	vtf->firstframe = LittleShort(vtf->firstframe);
	vtf->pad1 = LittleLong(vtf->pad1);
	vtf->reflectivity[0] = LittleFloat(vtf->reflectivity[0]);
	vtf->reflectivity[1] = LittleFloat(vtf->reflectivity[1]);
	vtf->reflectivity[2] = LittleFloat(vtf->reflectivity[2]);
	vtf->pad2 = LittleFloat(vtf->pad2);
	vtf->bumpmapscale = LittleFloat(vtf->bumpmapscale);

	version = (vtf->major<<16)|vtf->minor;
	//7.6 (Strata Source) is 7.5 + optional extra resource types; the fields we read and the resource
	//table (parsed for 7.3+ below) are unchanged, so accept it. NOTE: 7.6 also allows per-image
	//"auxiliary compression" (an 'AXC' resource); maps that use it would need that decoded here too, but
	//the packed 7.6 textures seen so far carry an uncompressed high-res image resource like 7.5.
	if (version > 0x00070006)
	{
		Con_Printf("%s: VTF version %i.%i is not supported\n", fname, vtf->major, vtf->minor);
		return NULL;
	}

	//FTESurf Patch 239: byte 1 was shifted 16, colliding with byte 2, so any
	//low-res format above 255 decoded wrong.  The bytes are also assembled
	//little-end-first right here, which means the result is ALREADY in host
	//order -- the LittleLong() that wrapped it would have swapped it a second
	//time on a big-endian host.  (Exposure today is nil: every lowResImageFormat
	//in the library is 5, 12, 13, 14, 15 or -1, and the -1 files all have
	//lowreswidth/height 0, which the guard below skips.)
	lrfmt = (fmtfmt_t)(int)((vtf->lowresfmt_misaligned[0]<<0)|(vtf->lowresfmt_misaligned[1]<<8)|(vtf->lowresfmt_misaligned[2]<<16)|((unsigned int)vtf->lowresfmt_misaligned[3]<<24));
	vmffmt = LittleLong(vtf->imgformat);

	/*
	FTESurf Patch 239: FAIL HERE, LOUDLY, RATHER THAN RETURN A TEXTURE MADE OF
	NOTHING.

	An IMAGE_FORMAT this loader does not map returns PTI_INVALID, and
	PTI_INVALID's block size is zero bytes -- Image_BlockSizeForEncoding starts
	its byte count at 0 and the TF_INVALID arm never writes it.  Every mip level
	then computes datasize 0, `filedata` never advances, and the function
	returns a NON-NULL texture containing no data at all.  Downstream,
	GL_LoadTextureMips finds a null format entry, the texture is quietly marked
	TEX_FAILED, and not one line is printed by anybody.  The surface draws as
	the missing-texture placeholder and the log says nothing whatsoever, which
	is the worst possible shape for a bug -- it looks exactly like a texture the
	map forgot to pack.

	This is not theoretical and it is not rare: 808 VTF instances across 112 of
	the 1310 maps (8.5%) take this path today.  The bulk of them are the three
	formats Patch 239 now decodes -- ABGR8888 (421), Strata BC6H (193) and
	Strata BC7 (182+19) -- and after those the residue is 21 files in formats
	FTE genuinely has no pixel layout for (BGR565, BGRA4444, A8, the two
	_BLUESCREEN colour-key formats, UVWQ8888, UVLX8888).  Those 21 now say so by
	name instead of vanishing.

	Placed before anything is allocated and before `filedata` has moved, so
	returning NULL leaves the caller's BZ_Free(filedata) correct.
	*/
	if (ImageVTF_VtfToFTE(vmffmt, vtf->flags) == PTI_INVALID)
	{
		Con_Printf(CON_ERROR"%s: unsupported VTF image format %i\n", fname, (int)vmffmt);
		return NULL;
	}

	//And the other half of "it was undiagnosable": say what every file actually
	//is, under the same r_texdiag the rest of the plugin reports through.  The
	//formats this patch added are the interesting ones -- 1 (ABGR8888), 70 (BC7)
	//and 71 (BC6H) used to end here as a silent 0-byte texture.
	if (hl2_texdiag && hl2_texdiag->ival)
		Con_Printf("[texdiag] VTF %-46s v%i.%i fmt=%i flags=0x%x -> pti=%i %ix%i\n",
			fname, (int)vtf->major, (int)vtf->minor, (int)vmffmt, (unsigned)vtf->flags,
			(int)ImageVTF_VtfToFTE(vmffmt, vtf->flags), (int)vtf->width, (int)vtf->height);

	mips = NULL;
	if (version >= 0x00070003)
	{
		int i;
		struct
		{
			unsigned int rtype;
			unsigned int rdata; //usually an offset.
		} *restable = (void*)(filedata+sizeof(*vtf));
		for (i = 0; i < LittleLong(vtf->numresources); i++, restable++)
		{
			if ((LittleLong(restable->rtype) & 0x00ffffff) == 0x30)
			{
				mips = plugfuncs->Malloc(sizeof(*mips));
				mips->extrafree = filedata;
				filedata += LittleLong(restable->rdata);
				break;
			}
			//other unknown resource types.
		}
	}
	if (!mips)
	{
		mips = plugfuncs->Malloc(sizeof(*mips));
		mips->extrafree = filedata;

		//skip the header
		filedata += vtf->headersize;
		//and skip the low-res image too.
		if (vtf->lowreswidth && vtf->lowresheight)
			imagefuncs->BlockSizeForEncoding(ImageVTF_VtfToFTE(lrfmt, 0), &bb, &bw, &bh, &bd);	//0: the header flags describe the high-res image; a thumbnail is never a cutout
		else
			bb=bw=bh=bd=1;
		datasize = ((vtf->lowreswidth+bw-1)/bw) * ((vtf->lowresheight+bh-1)/bh) * ((1/*vtf->lowresdepth*/+bd-1)/bd) * bb;
		filedata += datasize;
	}

	//now handle the high-res image
	if (mips)
	{
		mips->type = (vtf->flags & 0x4000)?PTI_CUBE:PTI_2D;

		/*
		FTESurf Patch 195: a multi-frame VTF, when something asks for one.

		`frames = 1;//vtf->numframes;` truncated every flipbook in the game to
		frame 0 -- 3,342 materials across 741 maps drive one with an
		AnimatedTexture proxy, including surf_demise's smoke, which Patch 189
		gave a scroll and which still could not cycle.

		The parsing was already all here: the loop below walks
		`frame < vtf->numframes` and the file stores every frame of a mip level
		CONTIGUOUSLY, which is exactly the layout PTI_2D_ARRAY wants -- one mip
		per level, `depth` layers deep.  So this is a layout reinterpretation,
		not new decoding.

		OPT-IN, on the requested texture type.  A VTF is loaded by name and the
		loader cannot know what the shader will do with it, so returning an array
		for every multi-frame file would hand a sampler2DArray to every existing
		sampler2D and break materials that render correctly today.  The caller
		asks with IF_TEXTYPE_2D_ARRAY -- which the shader script already spells
		`map "$2darray:name"` (gl_shader.c:1022) -- and anything that does not ask
		keeps frame 0 and the behaviour that shipped.
		*/
		if ((flags & IF_TEXTYPEMASK) == IF_TEXTYPE_2D_ARRAY && mips->type == PTI_2D && vtf->numframes > 1)
			mips->type = PTI_2D_ARRAY;

		mips->encoding = ImageVTF_VtfToFTE(vmffmt, vtf->flags);	//flags is host order already, swapped at the top
		imagefuncs->BlockSizeForEncoding(mips->encoding, &bb, &bw, &bh, &bd);

		miplevels = vtf->mipmapcount;
		frames = (mips->type==PTI_2D_ARRAY)?vtf->numframes:1;
		faces = ((mips->type==PTI_CUBE)?6:1);

		/*
		FTESurf Build 13: VTF cubemaps before 7.5 store SEVEN faces, not six.

		Valve's CVTFTexture::FaceCount() returns CUBEMAP_FACE_COUNT (7) for an
		envmap when the minor version is below 5, and 6 at 7.5 and above -- the
		seventh is a precomputed spheremap that was dropped in 7.5.  It is
		stored per mip level, interleaved with the real faces, so it is part of
		the STRIDE whether or not anything wants it.

		This matters the moment env_cubemap works at all: surf_666's and
		surf_pipeline's baked cubemaps are v7.4, so with a stride of 6 every
		mip after the first reads from the wrong offset and the cube decodes to
		noise.  surf_utopia's are v7.6 and would have been fine, which is
		exactly the kind of split that looks like "cubemaps work on some maps".

		So: seven for the stride, six for what we upload.
		*/
		storedfaces = faces;
		if (mips->type==PTI_CUBE && vtf->major == 7 && vtf->minor < 5)
			storedfaces = 7;

		/*
		Patch 195: an array is one mip PER LEVEL, `frames` layers deep -- not one
		mip per (level, frame) pair.  So the decay loop below only has miplevels
		to give, and it must never drop layers: mip[].datasize says how many are
		there, and shortening the count while leaving the size would read past
		the file.  In practice it never fires for an array -- a VTF has at most
		~14 mip levels against countof(mips->mip).
		*/
		if (mips->type == PTI_2D_ARRAY)
			mips->mipcount = miplevels;
		else
			mips->mipcount = miplevels * frames;
		while (mips->mipcount > countof(mips->mip))
		{
			if (miplevels > 1)
				miplevels--;
			else if (mips->type != PTI_2D_ARRAY && frames > 1)
				frames--;
			else
				break;
			mips->mipcount = (mips->type == PTI_2D_ARRAY) ? miplevels : miplevels * frames;
		}
		if (!mips->mipcount || mips->mipcount > countof(mips->mip))
		{
			plugfuncs->Free(mips);
			return NULL;
		}
		for (miplevel = vtf->mipmapcount; miplevel-- > 0;)
		{	//smallest to largest, which is awkward.
			qbyte *levelbase = filedata;	//Patch 195: frame 0 of THIS level
			w = vtf->width>>miplevel;
			h = vtf->height>>miplevel;
			if (!w)
				w = 1;
			if (!h)
				h = 1;
			datasize = ((w+bw-1)/bw) * ((h+bh-1)/bh) * ((d+bd-1)/bd) * bb;
			for (frame = 0; frame < vtf->numframes; frame++)
			{
				if (miplevel < miplevels && mips->type != PTI_2D_ARRAY)
				{
					img = miplevel + frame*miplevels;
					if (img >= countof(mips->mip))
						break;	//erk?
					if (filedata + datasize > end)
						break;	//no more data here...
					mips->mip[img].width = w;
					mips->mip[img].height = h;
					mips->mip[img].depth = faces;
					mips->mip[img].data = filedata;
					mips->mip[img].datasize = datasize*faces;
				}
				filedata += datasize*storedfaces;
			}
			//Patch 195: every frame of this level in one layered mip.  faces and
			//storedfaces are both 1 here (a cube is never turned into an array
			//above), so the frames really are contiguous from levelbase.
			if (miplevel < miplevels && mips->type == PTI_2D_ARRAY)
			{
				if (levelbase + datasize*frames > end)
				{	//truncated file: keep what is whole, drop this level and the rest
					mips->mipcount = miplevels = miplevel;
					continue;
				}
				mips->mip[miplevel].width = w;
				mips->mip[miplevel].height = h;
				mips->mip[miplevel].depth = frames;
				mips->mip[miplevel].data = levelbase;
				mips->mip[miplevel].datasize = datasize*frames;
			}
		}
	}
	/*
	FTESurf Patch 239: IMAGE_FORMAT_ABGR8888 -> PTI_RGBA8, reversed in place.

	This is the single biggest format the loader used to reject -- 421 instances
	in 73 maps -- and FTE has no PTI_ABGR8 to name it with, so the four bytes of
	each texel are reversed where they lie.  That is safe here: the bytes are in
	the file buffer that mips->extrafree owns and will free, no two mip[] entries
	overlap, and every mip[] pointer has already been assigned above, so the
	walk covers exactly what will be uploaded and nothing else.

	mip.datasize is the authority on how much to touch, not width*height*depth:
	for a cubemap it is the six faces we upload rather than the seven a pre-7.5
	file stores, and for a 2D array it is every layer of the level.  Both are
	contiguous from mip.data.
	*/
	if (mips && vmffmt == VMF_ABGR8)
	{
		unsigned int m, p, n;
		for (m = 0; m < mips->mipcount; m++)
		{
			qbyte *px = mips->mip[m].data;
			if (!px)
				continue;
			n = (unsigned int)(mips->mip[m].datasize / 4);
			for (p = 0; p < n; p++, px += 4)
			{
				qbyte a=px[0], b=px[1], g=px[2], r=px[3];
				px[0]=r; px[1]=g; px[2]=b; px[3]=a;
			}
		}
	}

	//nettest: Source compressed-HDR (RGBS) sky face — stored in a BGRA8 VTF, but the alpha
	// byte is a per-pixel LINEAR HDR scale (not real alpha). Flagged via the $hdr: shader name
	// prefix (IF_HDRDECOMPRESS). Uploading the raw bytes treated the scale as alpha -> banded/
	// dark. Decode to linear float (the engine repacks RGBA32F -> RGBA16F/byte on upload):
	//   linear.rgb = srgb_to_linear(rgb/255) * (alpha/255) * 8.0
	// matching Valve's sky_hdr_compressed_rgbs shader (result.rgb = rgb*alpha; result *=
	// InputScale=8; with an sRGB read on rgb). $color tint is ignored (defaults to white).
	if (mips && (flags & IF_HDRDECOMPRESS) && vmffmt == VMF_BGRA8)
	{
		float srgblin[256];
		unsigned int m, n;
		for (n = 0; n < 256; n++)
		{
			float c = n / 255.0f;
			srgblin[n] = (c <= 0.04045f) ? (c / 12.92f) : powf((c + 0.055f) / 1.055f, 2.4f);
		}
		for (m = 0; m < mips->mipcount; m++)
		{
			const qbyte *src = mips->mip[m].data;
			unsigned int px = mips->mip[m].width * mips->mip[m].height * (unsigned int)mips->mip[m].depth;
			float *dst, *out;
			unsigned int p;
			if (!src || !px)
				continue;
			out = dst = plugfuncs->Malloc(px * 4 * sizeof(float));
			for (p = 0; p < px; p++, src += 4, out += 4)
			{
				float a = src[3] * (8.0f / 255.0f);	//alpha = linear HDR scale; *8 = InputScale
				out[0] = srgblin[src[2]] * a;	//R  (VTF byte order is B,G,R,A)
				out[1] = srgblin[src[1]] * a;	//G
				out[2] = srgblin[src[0]] * a;	//B
				out[3] = 1.0f;
			}
			mips->mip[m].data = dst;
			mips->mip[m].datasize = px * 4 * (unsigned int)sizeof(float);
			mips->mip[m].needfree = true;
		}
		mips->encoding = PTI_RGBA32F;
	}

	return mips;
}

static plugimageloaderfuncs_t vtffuncs =
{
	"Valve Texture File",
	sizeof(struct pendingtextureinfo),
	true,
	Image_ReadVTFFile,
};

qboolean VTF_Init(void)
{
	imagefuncs = plugfuncs->GetEngineInterface(plugimagefuncs_name, sizeof(*imagefuncs));
	if (!imagefuncs)
		return false;
	return plugfuncs->ExportInterface(plugimageloaderfuncs_name, &vtffuncs, sizeof(vtffuncs));
}

