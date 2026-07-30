/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

// screen.c -- master for refresh, status bar, console, chat, notify, etc

#include "quakedef.h"
extern qboolean gammaworks;
#ifdef GLQUAKE
#include "glquake.h"
#include "shader.h"
#include "gl_draw.h"

#include <time.h>

qboolean GLSCR_UpdateScreen (void);


extern qboolean	scr_drawdialog;

extern cvar_t vid_triplebuffer;
extern cvar_t          scr_fov;

extern qboolean        scr_initialized;
extern float oldsbar;
extern qboolean        scr_drawloading;

extern cvar_t vid_conautoscale;
extern qboolean		scr_con_forcedraw;
extern qboolean		depthcleared;

/*
==================
SCR_UpdateScreen

This is called every frame, and can also be called explicitly to flush
text to the screen.

WARNING: be very careful calling this from elsewhere, because the refresh
needs almost the entire 256k of stack space!
==================
*/
void SCR_DrawCursor(void);
qboolean GLSCR_UpdateScreen (void)
{
	qboolean nohud;
	qboolean noworld;
	extern cvar_t vid_srgb;
	RSpeedLocals();	//nettest: shared by the five SCR_ gap buckets below

	//nettest: SCREEN SETUP gap.  Everything from here to the 3d dispatch -- vid_srgb changes,
	//Shader_DoReload, SCR_SetUpToDrawConsole and the r_clear glClear -- was in no bucket.
	//The early returns below deliberately skip the matching RSpeedEnd; on those frames nothing
	//accumulates, which is correct (no 3d frame was drawn either).
	RSpeedRemark();

	r_refdef.pxrect.maxheight = vid.pixelheight;

	vid.numpages = 2 + vid_triplebuffer.value;

	if (!scr_initialized || !con_initialized)
	{
		return false;                         // not initialized yet
	}

	R2D_Font_Changed();

	if (vid_srgb.modified)
	{
		vid_srgb.modified = false;

		//vid_srgb can be changed between 0 and 1, but other values need texture reloads. do that without too much extra weirdness.
		if ((vid.flags & VID_SRGB_CAPABLE) && gl_config.arb_framebuffer_srgb)
		{	//srgb-capable
			if (vid_srgb.ival > 0 && (vid.flags & VID_SRGBAWARE))
			{	//full srgb wanted (and textures are loaded)
				qglEnable(GL_FRAMEBUFFER_SRGB);
				vid.flags |= VID_SRGB_FB_LINEAR;
			}
			else if (vid_srgb.ival < 0 || (vid.flags & VID_SRGBAWARE))
			{	//srgb wanted only for the framebuffer, for gamma tricks.
				qglEnable(GL_FRAMEBUFFER_SRGB);
				vid.flags |= VID_SRGB_FB_LINEAR;
			}
			else
			{	//srgb not wanted...
				qglDisable(GL_FRAMEBUFFER_SRGB);
				vid.flags &= ~VID_SRGB_FB_LINEAR;
			}
		}
	}

	if (scr_disabled_for_loading)
	{
		extern char levelshotname[];
		extern float scr_disabled_time;
		float now = Sys_DoubleTime();
		if (now - scr_disabled_time > 60 || Key_Dest_Has(~kdm_game))
		{
			//FIXME: instead of reenabling the screen, we should just draw the relevent things skipping only the game (except that this requires a copy of the game beneath or otherwise results in flickering).
			scr_disabled_for_loading = false;
		}
		else if (!*levelshotname && !CSQC_UseGamecodeLoadingScreen() && !MP_UsingGamecodeLoadingScreen()
#ifdef MENU_NATIVECODE
				&& !(mn_entry && mn_entry->DrawLoading)
#endif
				)
			return false;	//don't refresh if we can't do so safely.
		else
		{
			SCR_DrawLoading (true);
			SCR_SetUpToDrawConsole();
			if (Key_Dest_Has(kdm_console))
				SCR_DrawConsole(false);
			if (R2D_Flush)
				R2D_Flush();
			VID_SwapBuffers();
			return true;
		}
	}


	Shader_DoReload();

	qglDisable(GL_SCISSOR_TEST);

#ifdef TEXTEDITOR
	if (editormodal)
	{
		Editor_Draw();
		V_UpdatePalette (false);
		R2D_BrightenScreen();
		Media_RecordFrame();

		if (key_dest_mask & kdm_console)
			Con_DrawConsole(vid.height/2, false);
		else
			Con_DrawConsole(0, false);
		SCR_DrawCursor();
	}
	else
#endif
	{
		//
		// do 3D refresh drawing, and then update the screen
		//
		SCR_SetUpToDrawConsole ();

		noworld = false;
		nohud = false;

		if (r_clear.ival)
		{
			GL_ForceDepthWritable();
			if (r_clearcolour.ival)
			{
				r_clearcolour.vec4[0] = host_basepal[(r_clearcolour.ival & 0xFF)*3+0]/255.0;
				r_clearcolour.vec4[1] = host_basepal[(r_clearcolour.ival & 0xFF)*3+1]/255.0;
				r_clearcolour.vec4[2] = host_basepal[(r_clearcolour.ival & 0xFF)*3+2]/255.0;
			}
			qglClearColor(r_clearcolour.vec4[0], r_clearcolour.vec4[1], r_clearcolour.vec4[2], 1);
			qglClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
			depthcleared = true;
		}

		RSpeedEnd(RSPEED_SCR_SETUP);

		if (topmenu && topmenu->isopaque)
			nohud = true;
#ifdef VM_CG
		else if (q3 && q3->cg.Redraw(cl.time))
			nohud = true;
#endif
#ifdef CSQC_DAT
		else if (CSQC_DrawView())
			nohud = true;
#endif
		else
		{
			if ((r_worldentity.model && cls.state == ca_active) || vrui.enabled)
				V_RenderView (nohud);
			else
				noworld = true;
		}

		//nettest: SCREEN COMPOSITE gap.  GL_Set2D(false) is the call that tears down the 3d
		//state and re-establishes the 2d ortho pass over the finished scene; on the FBO path it
		//is also where the gameview target stops being current.  It and the noworld fallback
		//below were in no bucket.
		RSpeedRemark();

		GL_Set2D (false);

		scr_con_forcedraw = false;
		if (noworld)
		{
			//draw the levelshot or the conback fullscreen
			if (R2D_DrawLevelshot())
				;
			else if (scr_con_current != vid.height)
			{
#ifdef HAVE_LEGACY
				extern cvar_t dpcompat_console;
				if (dpcompat_console.ival)
				{
					R2D_ImageColours(0,0,0,1);
					R2D_FillBlock(0, 0, vid.width, vid.height);
					R2D_ImageColours(1,1,1,1);
				}
				else
#endif
					R2D_ConsoleBackground(0, vid.height, true);
			}
			else
				scr_con_forcedraw = true;

			nohud = true;
		}

		RSpeedEnd(RSPEED_SCR_COMPOSITE);

		r_refdef.playerview = &cl.playerview[0];
		if (!vrui.enabled)
			SCR_DrawTwoDimensional(nohud);

		V_UpdatePalette (false);

		//nettest: BRIGHTEN/CAPTURE gap.  R2D_BrightenScreen is a full-screen blend pass when
		//v_contrast/v_brightness are off their defaults, and Media_RecordFrame does a blocking
		//glReadPixels while capturing.  Neither was in a bucket.
		RSpeedRemark();
		R2D_BrightenScreen();
		Media_RecordFrame();
		RSpeedEnd(RSPEED_SCR_BRIGHTEN);
	}

	//nettest: the OBSERVER EFFECT bucket.  RSpeedShow draws ~37 strings -- 1000+ glyph quads --
	//every single frame, and the R2D_Flush immediately after is what submits them.  None of that was
	//in any bucket, so the cost of LOOKING at r_speeds was silently inflating the very residual we
	//were chasing.  This has to be subtracted before trusting any r_speeds 2 total.
	RSpeedRemark();
	RSpeedShow();
	if (R2D_Flush)
		R2D_Flush();
	RSpeedEnd(RSPEED_RSPEEDSHOW);

	{
#if defined(_WIN32) && !defined(FTE_SDL)
		extern void Sys_FramePacePresent(void);
		extern qboolean Sys_FramePacePresentActive(void);
		extern void Sys_FramePace_RecordPresent(void);
		//nettest: FRAME PACING bucket.  This is where the missing third of the frame was.
		//
		//The drain below sat in no RSPEED_ bucket, and it could not have been caught by any of
		//the others either: RSpeedEnd only issues its own qglFinish at r_speeds > 2 (render.h),
		//so at the r_speeds 2 anyone actually measures with, NO child bucket contains a GPU sync.
		//All GPU-tail time was therefore structurally forced into the unattributed remainder of
		//"Total refresh", where it read as mystery CPU cost.  Measured on fy_killzone at a fixed
		//viewpoint (340 draw calls, 2.0M indices), sys_framepacing 4, drain depth 1:
		//
		//                       r_renderscale 2      r_renderscale 1
		//   Total refresh          2764.51 us           2576.52 us
		//   Frame pacing            623.09               501.51
		//   ...with drain off      2076.21              2061.48
		//
		//Note what that last row says: with the drain off, r_renderscale 2 costs 14.7us -- 4x the
		//pixels for nothing.  The GPU was never the limiter.  The drain was simply forbidding
		//frame N's GPU work from overlapping frame N+1's CPU work, and charging the tail latency
		//to the CPU every frame.  Hence the depth-2 default below.
		//
		//The paced hold further down contributes nothing to this bucket: Sys_FramePacePresent
		//early-outs on cl_maxfps 0.  So what this reads is the drain alone.
		RSpeedRemark();
		if (Sys_FramePacePresentActive())
		{	//Bound the GPU backlog so the paced flip below IS the real present and the cadence can't
			//sawtooth.  sys_framepacing_drain chooses how far back to wait:
			//  1 = this frame -- the original behaviour.  Correct, but it forbids frame N's GPU work
			//      from overlapping frame N+1's CPU work, and that measured 623us/frame of dead
			//      stall on fy_killzone (22% of the frame; 361 -> 481 fps with it off) on a machine
			//      whose GPU had headroom to spare.
			//  2 = previous frame (default) -- queue depth is still capped at one frame, so the flip
			//      still lands on a nearly-drained GPU, but the pipeline keeps its overlap and the
			//      stall shrinks to however far the GPU is genuinely behind.
			//  3 = two frames back -- loosest; more latency, least stall.
			extern int Sys_FramePaceDrainDepth(void);
			int drain = Sys_FramePaceDrainDepth();
			if (drain > 0)
			{
				if (!qglFenceSync || !qglClientWaitSync || !qglDeleteSync)
					qglFinish();	//no ARB_sync available: a full serialising drain is the only option
				else if (drain == 1)
				{	//legacy: fence this frame, then immediately block on it
					GLsync fence = qglFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
					if (fence)
					{
						qglClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 100000000ull);	//<=100ms guard; proceed even if it ever times out
						qglDeleteSync(fence);
					}
					else
						qglFinish();
				}
				else
				{	//Rotate through (drain-1) slots so we block on the fence inserted (drain-1) frames
					//ago.  On the first frames after a context creation the slot is still NULL and
					//nothing is waited on, which is correct -- there is no backlog to bound yet.
					int slots = drain - 1;			//drain 2 -> 1 slot (N-1); drain 3 -> 2 slots (N-2)
					int i = gl_framepace_slot % slots;
					GLsync prev = gl_framepace_fence[i];
					if (prev)
					{
						qglClientWaitSync(prev, GL_SYNC_FLUSH_COMMANDS_BIT, 100000000ull);
						qglDeleteSync(prev);
						gl_framepace_fence[i] = NULL;	//cleared before the re-fence so a failed qglFenceSync can't leave a stale handle
					}
					gl_framepace_fence[i] = qglFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
					gl_framepace_slot = (gl_framepace_slot + 1) % slots;
				}
			}
		}
		Sys_FramePacePresent();	//sys_framepacing 4: hold the now-complete flip to the present grid (no-op in other modes)
		RSpeedEnd(RSPEED_SCR_PACING);
#endif
		RSpeedRemark();
		VID_SwapBuffers();
		RSpeedEnd(RSPEED_PRESENT);
#if defined(_WIN32) && !defined(FTE_SDL)
		RSpeedRemark();
		Sys_FramePace_RecordPresent();	//sample present-to-present cadence for sys_framepacing_stats (every mode)
		RSpeedEnd(RSPEED_SCR_PACING);
#endif
	}

	//gl 4.5 / GL_ARB_robustness / GL_KHR_robustness
	//nettest: GL RESET CHECK gap.  qglGetGraphicsResetStatus is a synchronous driver round-trip
	//issued unconditionally once per frame, and it was in no bucket.  Cheap on most drivers, but
	//that is an assumption, and assumptions are what this whole exercise is replacing.
	RSpeedRemark();
	if (qglGetGraphicsResetStatus)
	{
		char *reason;
		GLenum err = qglGetGraphicsResetStatus();
		switch(err)
		{
		case GL_NO_ERROR:
			break;
		case GL_GUILTY_CONTEXT_RESET:	//we did it
		case GL_INNOCENT_CONTEXT_RESET:	//something else broke the hardware and broke our ram
		case GL_UNKNOWN_CONTEXT_RESET:	//whodunit
		default:
			if (err == GL_GUILTY_CONTEXT_RESET)
				reason = "guilty";
			else if (err == GL_INNOCENT_CONTEXT_RESET)
				reason = "innocent";
			else
				reason = "unknown";
			Con_Printf("OpenGL reset detected (%s)\n", reason);
			Sys_Sleep(3.0);
			Cmd_ExecuteString("vid_restart", RESTRICT_LOCAL);
			break;
		}
	}
	RSpeedEnd(RSPEED_SCR_RESET);
	return true;
}


char *GLVID_GetRGBInfo(int *bytestride, int *truewidth, int *trueheight, enum uploadfmt *fmt)
{	//returns a BZ_Malloced array
	extern qboolean gammaworks;
	int i, c;
	qbyte *ret;
	extern qboolean r2d_canhwgamma;
	qboolean hdr = false;

	*bytestride = 0;
	*truewidth = vid.fbpwidth;
	*trueheight = vid.fbpheight;

	if (*r_refdef.rt_destcolour[0].texname)
	{
		unsigned int w,h;
		texid_t tid = R2D_RT_GetTexture(r_refdef.rt_destcolour[0].texname, &w, &h);
		if (tid)
			hdr = (tid->format==PTI_RGBA16F)||(tid->format==PTI_RGBA32F)||(tid->format==PTI_B10G11R11F);
	}
	if (hdr)
	{
		*fmt = PTI_RGBA16F;
		ret = BZ_Malloc((*truewidth)*(*trueheight)*8);
		qglReadPixels (0, 0, (*truewidth), (*trueheight), GL_RGBA, GL_HALF_FLOAT, ret);
		*bytestride = *truewidth*-8;
	}
	/*else if (1)
	{
		float *p;

		p = BZ_Malloc(vid.pixelwidth*vid.pixelheight*sizeof(float));
		qglReadPixels (0, 0, vid.pixelwidth, vid.pixelheight, GL_DEPTH_COMPONENT, GL_FLOAT, p); 

		ret = BZ_Malloc(vid.pixelwidth*vid.pixelheight*3);

		c = vid.pixelwidth*vid.pixelheight;
		for (i = 1; i < c; i++)
		{
			ret[i*3+0]=p[i]*p[i]*p[i]*255;
			ret[i*3+1]=p[i]*p[i]*p[i]*255;
			ret[i*3+2]=p[i]*p[i]*p[i]*255;
		}
		BZ_Free(p);
	}*/
	else if (gl_config.gles || (*truewidth&3))
	{
		//gles:
		//Only two format/type parameter pairs are accepted.
		//GL_RGBA/GL_UNSIGNED_BYTE is always accepted, and the other acceptable pair can be discovered by querying GL_IMPLEMENTATION_COLOR_READ_FORMAT and GL_IMPLEMENTATION_COLOR_READ_TYPE.
		//thus its simpler to only use GL_RGBA/GL_UNSIGNED_BYTE
		//desktopgl:
		//total line byte length must be aligned to GL_PACK_ALIGNMENT. by reading rgba instead of rgb, we can ensure the line is a multiple of 4 bytes.

		*fmt = PTI_RGBA8;
		ret = BZ_Malloc((*truewidth)*(*trueheight)*4);
		qglReadPixels (0, 0, (*truewidth), (*trueheight), GL_RGBA, GL_UNSIGNED_BYTE, ret);
		*bytestride = *truewidth*-4;
	}
#if 1//def _DEBUG
	else if (!gl_config.gles && sh_config.texfmt[PTI_BGRA8])
	{
		*fmt = PTI_BGRA8;
		ret = BZ_Malloc((*truewidth)*(*trueheight)*4);
		qglReadPixels (0, 0, (*truewidth), (*trueheight), GL_BGRA_EXT, GL_UNSIGNED_INT_8_8_8_8_REV, ret); 
		*bytestride = *truewidth*-4;
	}
#endif
	else
	{
		*fmt = PTI_RGB8;
		ret = BZ_Malloc((*truewidth)*(*trueheight)*3);
		qglReadPixels (0, 0, (*truewidth), (*trueheight), GL_RGB, GL_UNSIGNED_BYTE, ret); 
		*bytestride = *truewidth*-3;
	}

	if (gammaworks && r2d_canhwgamma)
	{
		extern qbyte		gammatable[256];
		int pxsize = 4;
		c = (*truewidth)*(*trueheight);
		switch(*fmt)
		{
		case PTI_RGB8:
		case PTI_BGR8:
			pxsize = 3;
			//fallthrough
		case PTI_LLLA8:
		case PTI_LLLX8:
		case PTI_RGBA8:
		case PTI_RGBX8:
		case PTI_BGRA8:
		case PTI_BGRX8:
			//pxsize is 4 (or 3 if we fell through above)
			c*=pxsize;
			for (i=0 ; i<c ; i+=pxsize)
			{
				ret[i+0] = gammatable[ret[i+0]];
				ret[i+1] = gammatable[ret[i+1]];
				ret[i+2] = gammatable[ret[i+2]];
			}
			break;
		default:
			break;	//some kind of bug.
		}
	}
	
	return ret;
}
#endif
