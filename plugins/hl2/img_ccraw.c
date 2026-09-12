#include "../plugin.h"

/*
FTESurf Patch 288: Source's colour-correction lookup table, as an image.

A `.raw` next to a Source map is a HEADERLESS 32x32x32x3 lookup table -- 98,304
bytes of RGB, no magic, no dimensions, nothing.  `color_correction` entities name
one and a weight, and Source applies

    out = (1 - W) * in + sum( w[i] * LUT[i](in) )

to the whole frame after tonemapping.  FTE has never applied any of it: before
this patch the only mention of the feature anywhere in the engine was
`$toolcolorcorrection` sitting in mat_vmt.c's ignore list.

WHY A LOADER RATHER THAN A DECODER SOMEWHERE.  `$3d:` is already a shader
texture-name prefix (gl_shader.c:1089 -> IF_TEXTYPE_3D) and PTI_3D already
uploads, so the only missing link was something to turn 98,304 bytes into a
`struct pendingtextureinfo`.  Registering that as an image loader means the LUT
arrives through the ordinary filesystem, which matters here: both of
surf_tensor2's LUTs live inside the BSP's own pakfile, which the plugin already
mounts (mod_vbsp.c LoadMapArchive), so `materials/correction/x.raw` resolves with
no special case at all.

The engine's own generic `$3d:` path in Image_GenMip0 is NOT used, deliberately:
it has a pointer-arithmetic bug that walks four times the data it should.

THE BYTE ORDER IS ALREADY GL'S, and that was measured rather than assumed.  A
correlation of each output channel against each array axis over all 32,768
entries of surf_tensor2's LUT:

    out R  vs  stride 1 / 32 / 1024   ->   0.627  0.544  0.208
    out G                             ->   0.275  0.724  0.280
    out B                             ->   0.419  0.141  0.707

so stride 1 is red, stride 32 is green, stride 1024 is blue -- offset
((b*32 + g)*32 + r)*3, which is exactly (z*h + y)*w + x with x=r, y=g, z=b.  No
reordering, a straight copy.

Probing the CORNERS instead would have got this wrong.  This grade crushes
shadows hard and pushes everything cool, so its pure-red and pure-blue corners
happen to land on the same value and the red/green/blue axes are indistinguishable
for their first eight steps.  Two of the six possible axis orders survive a corner
test.  Only the whole-volume correlation separates them.

DECLINE UNLESS IT IS EXACTLY RIGHT.  `.raw` is a generic extension and this loader
is offered every image file the engine opens (image.c:13974), so it checks the
size AND the extension.  A loader that claimed anything called `.raw` would eat
unrelated files in silence and turn them into a 32-cube of garbage.

RGBA8 AND NOT RGB8, which is not a preference.  Image_ChangeFormat has no
conversion OUT of PTI_RGB8, so a driver that cannot upload three-byte rows has
nowhere to go and the texture fails with no message.  The 33% memory is 32 KiB.
*/

#define CCLUT_SIZE 32
#define CCLUT_BYTES (CCLUT_SIZE*CCLUT_SIZE*CCLUT_SIZE*3)

static plugimagefuncs_t *imagefuncs = NULL;

static struct pendingtextureinfo *Image_ReadCCRawFile(unsigned int flags, const char *fname, qbyte *filedata, size_t filesize)
{
	struct pendingtextureinfo *mips;
	qbyte *out;
	size_t i, n;
	size_t l;

	if (filesize != CCLUT_BYTES)
		return NULL;

	//...and it must actually be called .raw.  A 98,304-byte file of some other
	//kind is not impossible: that is 128x256 RGB, or 256x96 RGBA.
	l = strlen(fname);
	if (l < 4 || Q_strcasecmp(fname + l - 4, ".raw"))
		return NULL;

	mips = plugfuncs->Malloc(sizeof(*mips));
	if (!mips)
		return NULL;

	n = CCLUT_SIZE*CCLUT_SIZE*CCLUT_SIZE;
	out = plugfuncs->Malloc(n * 4);
	if (!out)
	{
		plugfuncs->Free(mips);
		return NULL;
	}
	for (i = 0; i < n; i++)
	{
		out[i*4+0] = filedata[i*3+0];
		out[i*4+1] = filedata[i*3+1];
		out[i*4+2] = filedata[i*3+2];
		out[i*4+3] = 255;
	}

	//extrafree owns the file buffer, mip[0] owns the expanded copy.  Same
	//split img_vtf.c's HDR path uses when it has to allocate.
	mips->extrafree = filedata;
	mips->type = PTI_3D;
	mips->encoding = PTI_RGBA8;
	mips->mipcount = 1;		//"we can't generate 3d mips" -- merged.h.  We do not want any.
	mips->mip[0].data = out;
	mips->mip[0].datasize = n * 4;
	mips->mip[0].width = CCLUT_SIZE;
	mips->mip[0].height = CCLUT_SIZE;
	mips->mip[0].depth = CCLUT_SIZE;
	mips->mip[0].needfree = true;

	Con_DPrintf("[cclut] %s: 32x32x32 colour-correction table\n", fname);
	return mips;
}

static plugimageloaderfuncs_t ccrawfuncs =
{
	"Source Colour Correction LUT",
	sizeof(struct pendingtextureinfo),
	false,	//a 3D texture is never a cubemap face
	Image_ReadCCRawFile,
};

qboolean CCRAW_Init(void)
{
	imagefuncs = plugfuncs->GetEngineInterface(plugimagefuncs_name, sizeof(*imagefuncs));
	if (!imagefuncs)
		return false;
	if (!plugfuncs->ExportInterface(plugimageloaderfuncs_name, &ccrawfuncs, sizeof(ccrawfuncs)))
		return false;
	/*
	SAY THAT IT REGISTERED.  This loader is silent by design -- it declines
	every file that is not exactly 98,304 bytes with a .raw name, so on a map
	with no grade it prints nothing at all.  That is also indistinguishable from
	never having been registered, which is one of the three things "the screen
	did not change colour" could mean.  One dprint at startup removes it.
	*/
	Con_DPrintf("[cclut] Source colour-correction .raw loader registered\n");
	return true;
}
