all: mat_vmt_progs.h

# NOTE: this list is HAND-MAINTAINED and is the real prerequisite list -- dropping
# a new glsl/vmt/*.glsl file in is not enough, and make will cheerfully say
# "Nothing to be done for 'all'" while the program is missing from the DLL.
# See the warning beside the "hl2 glsl -> mat_vmt_progs.h" step in build.ps1.
# imposter: FTESurf Patch 263.
VMTPROGSBASE=lightmapped refract transition unlit vertexlit water rt flatdither animated emissive imposter
VMTPROGS:=$(foreach p,$(VMTPROGSBASE),vmt/$p)
VMTPROGSFILES:=$(foreach p,$(VMTPROGS),glsl/$p.glsl)
mat_vmt_progs.h: $(VMTPROGSFILES)
	../../engine/shaders/generatebuiltinsl $@ $(VMTPROGS)

