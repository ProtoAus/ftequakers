all: mat_vmt_progs.h

# NOTE: this list is HAND-MAINTAINED and is the real prerequisite list -- dropping
# a new glsl/vmt/*.glsl file in is not enough, and make will cheerfully say
# "Nothing to be done for 'all'" while the program is missing from the DLL.
# See the warning beside the "hl2 glsl -> mat_vmt_progs.h" step in build.ps1.
# imposter: FTESurf Patch 263.  twotexture: FTESurf, UnlitTwoTexture's second
# layer plus the PlayerProximity/TextureScroll proxy chain compiled into defines.
# ccorrect: FTESurf Patch 288.  Not a material shader at all -- it is the
# POST-PROCESS the colour-correction stage runs, and it lives here because the
# builtin-program list a material loader exports is searched for every program
# name in the engine (gl_shader.c:2743-2748), not only for the ones its own
# materials ask for.  That is what lets a plugin ship a screen-space shader.
VMTPROGSBASE=lightmapped refract transition unlit vertexlit water rt flatdither animated emissive imposter twotexture ccorrect
VMTPROGS:=$(foreach p,$(VMTPROGSBASE),vmt/$p)
VMTPROGSFILES:=$(foreach p,$(VMTPROGS),glsl/$p.glsl)
mat_vmt_progs.h: $(VMTPROGSFILES)
	../../engine/shaders/generatebuiltinsl $@ $(VMTPROGS)

