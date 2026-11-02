// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2004-2016 Christoph Oelckers
// All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
//--------------------------------------------------------------------------
//
/*
** hw_entrypoint.cpp, former gl_scene.cpp
** manages the rendering of the player's view
**
*/

#include "gi.h"
#include "a_dynlight.h"
#include "m_png.h"
#include "doomstat.h"
#include "r_data/r_interpolate.h"
#include "r_utility.h"
#include "d_player.h"
#include "i_time.h"
#include "swrenderer/r_swscene.h"
#include "swrenderer/r_renderer.h"
#include "hw_dynlightdata.h"
#include "hw_clock.h"
#include "flatvertices.h"
#include "v_palette.h"
#include "d_main.h"
#include "g_cvars.h"
#include "v_draw.h"

#include "hw_lightbuffer.h"
#include "hw_bonebuffer.h"
#include "hw_cvars.h"
#include "hwrenderer/data/hw_viewpointbuffer.h"
#include "hwrenderer/scene/hw_fakeflat.h"
#include "hwrenderer/scene/hw_clipper.h"
#include "hwrenderer/scene/hw_portal.h"
#include "hw_vrmodes.h"

#include "i_mainwindow.h"
#include <WinGDI.h>
#include "common/rendering/gl_load/gl_interface.h" // to get "if (gl.legacyMode) context
#include "common/rendering/gl_load/gl_system.h"    // for GL commands
#include "common/rendering/gl/gl_renderstate.h"

#include <windows.h>
#include <GL/gl.h>

// [Darkcrafter07] - CRITICAL LINKER FORCE SHIELD!
// Primes the MSVC compiler to natively inject the system OpenGL library binary,
// completely extinguishing unresolved external symbols (__imp__wglMakeCurrent etc.)!
#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "gdi32.lib")

EXTERN_CVAR(Bool, cl_capfps)
extern bool NoInterpolateView;

static SWSceneDrawer *swdrawer;

void CleanSWDrawer()
{
	if (swdrawer) delete swdrawer;
	swdrawer = nullptr;
}

#include "g_levellocals.h"
#include "a_dynlight.h"


void CollectLights(FLevelLocals* Level)
{
	IShadowMap* sm = &screen->mShadowMap;
	int lightindex = 0;

	// Todo: this should go through the blockmap in a spiral pattern around the player so that closer lights are preferred.
	for (auto light = Level->lights; light; light = light->next)
	{
		IShadowMap::LightsProcessed++;
		if (light->shadowmapped && light->IsActive() && lightindex < 1024)
		{
			IShadowMap::LightsShadowmapped++;

			light->mShadowmapIndex = lightindex;
			sm->SetLight(lightindex, (float)light->X(), (float)light->Y(), (float)light->Z(), light->GetRadius());
			lightindex++;
		}
		else
		{
			light->mShadowmapIndex = 1024;
		}

	}

	for (; lightindex < 1024; lightindex++)
	{
		sm->SetLight(lightindex, 0, 0, 0, 0);
	}
}


//-----------------------------------------------------------------------------
//
// Renders one viewpoint in a scene
//
//-----------------------------------------------------------------------------

sector_t* RenderViewpoint(FRenderViewpoint& mainvp, AActor* camera, IntRect* bounds, float fov, float ratio, float fovratio, bool mainview, bool toscreen)
{
	if (gl.legacyMode)
	{
		return RenderViewpointGL1x(mainvp, camera, bounds, fov, ratio, fovratio, mainview, toscreen);
	}
	else
	{
		return RenderViewpointGL3x(mainvp, camera, bounds, fov, ratio, fovratio, mainview, toscreen);
	}
}

//==========================================================================
//
// [Darkcrafter07] - RenderViewpointGL1x
// Fully restored, independent legacy single-view frame manager for OpenGL 1.x.
// Completely stripped of shadowmaps, SSBO buffers, and multi-threaded rendering traps.
// Forces strict hardware device context activation via wglMakeCurrent to cure black screen!
// Declared as a global member function inside (hw_entrypoint.cpp).
//
//==========================================================================
//sector_t* RenderViewpointGL1x(FRenderViewpoint& mainvp, AActor* camera, IntRect* bounds, float fov, float ratio, float fovratio, bool mainview, bool toscreen)
//{
//	//glColor4f(1.0f, 0.0f, 1.0f, 1.0f);
//	//glClearColor(1.0f, 0.0f, 1.0f, 1.0f); // Fill the entire buffer with solid magenta
//	//glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
//
//	// Force-inject active field of view degrees into the viewpoint structure cleanly
//	mainvp.FieldOfView = DAngle::fromDeg(fov);
//
//	// Setup legacy software frame coordinates and angles interpolation offsets
//	R_SetupFrame(mainvp, r_viewwindow, camera);
//
//	// Force calibrate viewport rectangles clipping boundaries natively straight from bounds token
//	screen->SetViewportRects(bounds);
//
//	// ==============================================================================
//	// [Darkcrafter07] - AUTONOMOUS WIN32 DEVICE CONTEXT LOCK (FIXED E020 / UNDEFINED)
//	// ==============================================================================
//	// Bypasses missing platform headers and Win32GLVideo types entirely!
//	// Extracts the active Win32 window context and links thread handles on the fly.
//	HWND hWndLegacy = mainwindow.GetHandle();
//	if (hWndLegacy != NULL)
//	{
//		HDC hDCLegacy = GetDC(hWndLegacy);
//
//		// Natively query the live active rendering context directly from the GPU thread state
//		HGLRC hRCLegacy = wglGetCurrentContext();
//
//		if (hDCLegacy != NULL && hRCLegacy != NULL)
//		{
//			// Force link the active rendering thread context straight into our legacy frame buffer!
//			wglMakeCurrent(hDCLegacy, hRCLegacy);
//		}
//
//		// Release early DC references safely to safeguard against system windows stalls
//		ReleaseDC(hWndLegacy, hDCLegacy);
//	}
//
//	// ==============================================================================
//	// [Darkcrafter07] - THE MONOLITHIC FBO SHADER BUSTER CHANNEL!
//	// ==============================================================================
//	if (gl.legacyMode)
//	{
//		// 0x8D40 is the native hardware hex-code definition for GL_FRAMEBUFFER.
//		typedef void (APIENTRY* PFNGLBINDFRAMEBUFFERPROC) (unsigned int target, unsigned int framebuffer);
//		static PFNGLBINDFRAMEBUFFERPROC p_glBindFramebuffer = (PFNGLBINDFRAMEBUFFERPROC)wglGetProcAddress("glBindFramebuffer");
//
//		if (p_glBindFramebuffer != nullptr)
//		{
//			p_glBindFramebuffer(0x8D40, 0); // Hard-reset GPU pipeline back to the screen backbuffer!
//		}
//	}
//
//	// 1. HARDWARE OPENGL 1.1 ENVIRONMENT PREPARATION LOCK
//	glDisable(GL_SCISSOR_TEST);
//	glDepthMask(GL_TRUE);
//	glEnable(GL_DEPTH_TEST);
//
//	// Unpacked the 4-component floating point array explicitly by its indices bounds
//	glClearColor(screen->mSceneClearColor[0], screen->mSceneClearColor[1], screen->mSceneClearColor[2], 1.0f);
//	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
//
//	// 2. INSTANTIATE FIXED-FUNCTION SCENE CONTAINER DESCRIPTOR
//	auto di = HWDrawInfo::StartDrawInfo(mainvp.ViewLevel, nullptr, mainvp, nullptr);
//	if (di != nullptr)
//	{
//		// Capture the active stack-allocated frame pointer right on the spot!
//		HWDrawInfo::CurrentGL1xInfo = di;
//
//		auto& vp = di->Viewpoint;
//
//		// Localize sector heightsec zones definitions for volumetric fog loops
//		di->SetViewArea();
//
//		// Fire inventory colormaps solver (night vision, radsuits, screen flashes)
//		if (mainview && vp.camera != nullptr && vp.camera->player != nullptr)
//		{
//			di->SetFixedColormapGL1x(vp.camera->player);
//		}
//		else
//		{
//			di->FullbrightFlagsLegacy = CM_DEFAULT;
//			OpenGLRenderer::gl_RenderState.SetFixedGL1xColormap(CM_DEFAULT);
//		}
//
//		// Recalculate Lighthouse3D gl1xViewMatrix transforms matrices on the spot
//		di->SetViewMatrixGL1x((float)vp.Pos.X, (float)vp.Pos.Y, (float)vp.Pos.Z, false, false);
//
//		// 3. PUSH CALCULATED MATRIX STREAMS DIRECTLY TO THE VIDEO CARD REGISTERS
//		glMatrixMode(GL_PROJECTION);
//		glLoadIdentity();
//		glLoadMatrixf((float*)&di->gl1xProjectionMatrix);
//
//		glMatrixMode(GL_MODELVIEW);
//		glLoadIdentity();
//		glLoadMatrixf((float*)&di->gl1xViewMatrix);
//
//		// 4. FIRE THE STANDALONE NON-CRASHING BSP RENDER ENGINE MONOLITH!
//		// Direct route triggers: DrawSceneGL1x -> CreateGL1xScene -> RenderPolyBSPNodeGL1x!
//		di->DrawSceneGL1x(mainview ? 1 : 2);
//
//		// Safe clearance before the pipeline destructor destroys the instance
//		HWDrawInfo::CurrentGL1xInfo = nullptr;
//
//		// 5. DE-ALLOCATE CONTEXT BLOCK SAFELY TO PREVENT RAM LEAKS
//		di->EndDrawInfo();
//	}
//
//	// Safely return active camera sector container tracking pointer back to the engine loop
//	return mainvp.sector;
//}

extern "C"
{
	HGLRC zd_wglCreateContext(HDC Arg1);
	BOOL zd_wglDeleteContext(HGLRC Arg1);
	BOOL zd_wglMakeCurrent(HDC Arg1, HGLRC Arg2);
	PROC zd_wglGetProcAddress(LPCSTR name);
}
extern void* g_Legacy_hDC_Bridge;
extern void* g_Legacy_hRC_Bridge;
sector_t* RenderViewpointGL1x(FRenderViewpoint& mainvp, AActor* camera, IntRect* bounds, float fov, float ratio, float fovratio, bool mainview, bool toscreen)
{
	// Force-inject active field of view degrees into the viewpoint structure cleanly
	mainvp.FieldOfView = DAngle::fromDeg(fov);

	// Setup legacy software frame coordinates and angles interpolation offsets
	R_SetupFrame(mainvp, r_viewwindow, camera);

	// Force calibrate viewport rectangles clipping boundaries natively straight from bounds token
	screen->SetViewportRects(bounds);

	// ==============================================================================
	// [Darkcrafter07] - THE ENGINE NATIVE zd_wgl CONTEXT BINDING ACTIVATOR LOCK
	// ==============================================================================
	// CRITICAL FIX: To prevent state desynchronization inside gl_load.c,
	// we invoke the engine's internal zd_wglMakeCurrent instead of raw Win32 APIs!
	if (g_Legacy_hDC_Bridge != nullptr && g_Legacy_hRC_Bridge != nullptr)
	{
		// This flawlessly synchronizes gl_load.c state trackers with the GPU context!
		zd_wglMakeCurrent((HDC)g_Legacy_hDC_Bridge, (HGLRC)g_Legacy_hRC_Bridge);
	}

	// ==============================================================================
	// [Darkcrafter07] - THE MONOLITHIC FBO SHADER BUSTER CHANNEL!
	// ==============================================================================
	if (gl.legacyMode)
	{
		typedef void (APIENTRY* PFNGLBINDFRAMEBUFFERPROC) (unsigned int target, unsigned int framebuffer);
		static PFNGLBINDFRAMEBUFFERPROC p_glBindFramebuffer = nullptr;

		if (p_glBindFramebuffer == nullptr)
		{
			// Natively resolve via the synced zd_ loader wrapper!
			p_glBindFramebuffer = (PFNGLBINDFRAMEBUFFERPROC)zd_wglGetProcAddress("glBindFramebuffer");
		}

		if (p_glBindFramebuffer != nullptr)
		{
			p_glBindFramebuffer(0x8D40, 0); // 0x8D40 maps natively to GL_FRAMEBUFFER
		}
	}

	// 1. HARDWARE OPENGL 1.1 ENVIRONMENT PREPARATION LOCK
	glDisable(GL_SCISSOR_TEST);
	glDepthMask(GL_TRUE);
	glEnable(GL_DEPTH_TEST);

	// Unpacked the 4-component floating point array explicitly by its indices bounds
	glClearColor(screen->mSceneClearColor[0], screen->mSceneClearColor[1], screen->mSceneClearColor[2], 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

	// 2. INSTANTIATE FIXED-FUNCTION SCENE CONTAINER DESCRIPTOR
	auto di = HWDrawInfo::StartDrawInfo(mainvp.ViewLevel, nullptr, mainvp, nullptr);
	if (di != nullptr)
	{
		// Capture the active stack-allocated frame pointer right on the spot!
		HWDrawInfo::CurrentGL1xInfo = di;

		auto& vp = di->Viewpoint;

		// Localize sector heightsec zones definitions for volumetric fog loops
		di->SetViewArea();

		// Fire inventory colormaps solver (night vision, radsuits, screen flashes)
		if (mainview && vp.camera != nullptr && vp.camera->player != nullptr)
		{
			di->SetFixedColormapGL1x(vp.camera->player);
		}
		else
		{
			di->FullbrightFlagsLegacy = CM_DEFAULT;
			OpenGLRenderer::gl_RenderState.SetFixedGL1xColormap(CM_DEFAULT);
		}

		// Recalculate Lighthouse3D gl1xViewMatrix transforms matrices on the spot
		di->SetViewMatrixGL1x((float)vp.Pos.X, (float)vp.Pos.Y, (float)vp.Pos.Z, false, false);

		// 3. PUSH CALCULATED MATRIX STREAMS DIRECTLY TO THE VIDEO CARD REGISTERS
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		glLoadMatrixf((float*)&di->gl1xProjectionMatrix);

		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();
		glLoadMatrixf((float*)&di->gl1xViewMatrix);

		// 4. FIRE THE STANDALONE NON-CRASHING BSP RENDER ENGINE MONOLITH!
		di->DrawSceneGL1x(mainview ? 1 : 2);

		// Safe clearance before the pipeline destructor destroys the instance
		HWDrawInfo::CurrentGL1xInfo = nullptr;

		// 5. DE-ALLOCATE CONTEXT BLOCK SAFELY TO PREVENT RAM LEAKS
		di->EndDrawInfo();
	}

	// Safely return active camera sector container tracking pointer back to the engine loop
	return mainvp.sector;
}

sector_t* RenderViewpointGL3x(FRenderViewpoint& mainvp, AActor* camera, IntRect* bounds, float fov, float ratio, float fovratio, bool mainview, bool toscreen)
{
	auto& RenderState = *screen->RenderState();

	R_SetupFrame(mainvp, r_viewwindow, camera);

	if (mainview && toscreen && !(camera->Level->flags3 & LEVEL3_NOSHADOWMAP) && camera->Level->HasDynamicLights && gl_light_shadowmap && screen->allowSSBO() && (screen->hwcaps & RFL_SHADER_STORAGE_BUFFER))
	{
		screen->SetAABBTree(camera->Level->aabbTree);
		screen->mShadowMap.SetCollectLights([=] {
			CollectLights(camera->Level);
			});
		screen->UpdateShadowMap();
	}
	else
	{
		// null all references to the level if we do not need a shadowmap. This will shortcut all internal calculations without further checks.
		screen->SetAABBTree(nullptr);
		screen->mShadowMap.SetCollectLights(nullptr);
	}

	screen->SetLevelMesh(camera->Level->levelMesh);

	// Update the attenuation flag of all light defaults for each viewpoint.
	// This function will only do something if the setting differs.
	FLightDefaults::SetAttenuationForLevel(!!(camera->Level->flags3 & LEVEL3_ATTENUATE));

	// Render (potentially) multiple views for stereo 3d
	// Fixme. The view offsetting should be done with a static table and not require setup of the entire render state for the mode.
	auto vrmode = VRMode::GetVRMode(mainview && toscreen);
	const int eyeCount = vrmode->mEyeCount;
	screen->FirstEye();
	for (int eye_ix = 0; eye_ix < eyeCount; ++eye_ix)
	{
		const auto& eye = vrmode->mEyes[eye_ix];
		screen->SetViewportRects(bounds);

		if (mainview) // Bind the scene frame buffer and turn on draw buffers used by ssao
		{
			bool useSSAO = (gl_ssao != 0);
			screen->SetSceneRenderTarget(useSSAO);
			RenderState.SetPassType(useSSAO ? GBUFFER_PASS : NORMAL_PASS);
			RenderState.EnableDrawBuffers(RenderState.GetPassDrawBufferCount(), true);
		}

		auto di = HWDrawInfo::StartDrawInfo(mainvp.ViewLevel, nullptr, mainvp, nullptr);
		auto& vp = di->Viewpoint;

		di->Set3DViewport(RenderState);
		di->SetViewArea();
		auto cm = di->SetFullbrightFlags(mainview ? vp.camera->player : nullptr);
		float flash = 1.f;

		// Only used by the GLES2 renderer
		RenderState.SetSpecialColormap(cm, flash);

		di->Viewpoint.FieldOfView = DAngle::fromDeg(fov);	// Set the real FOV for the current scene (it's not necessarily the same as the global setting in r_viewpoint)

		// Stereo mode specific perspective projection
		float inv_iso_dist = 1.0f;
		bool iso_ortho = (camera->ViewPos != NULL) && (camera->ViewPos->Flags & VPSF_ORTHOGRAPHIC);
		if (iso_ortho && (camera->ViewPos->Offset.Length() > 0)) inv_iso_dist = 1.0 / camera->ViewPos->Offset.Length();
		di->VPUniforms.mProjectionMatrix = eye.GetProjection(fov, ratio, fovratio * inv_iso_dist, iso_ortho);
		di->ProjectionMatrix2 = eye.GetProjection(fov, ratio, fovratio, false); // Regular ol' perspective projection matrix

		// Stereo mode specific viewpoint adjustment
		vp.Pos += eye.GetViewShift(vp.HWAngles.Yaw.Degrees());
		di->SetupView(RenderState, vp.Pos.X, vp.Pos.Y, vp.Pos.Z, false, false);

		di->ProcessScene(toscreen);

		if (mainview)
		{
			PostProcess.Clock();
			if (toscreen) di->EndDrawScene(mainvp.sector, RenderState); // do not call this for camera textures.

			if (RenderState.GetPassType() == GBUFFER_PASS) // Turn off ssao draw buffers
			{
				RenderState.SetPassType(NORMAL_PASS);
				RenderState.EnableDrawBuffers(1);
			}

			screen->PostProcessScene(false, cm, flash, [&]() { di->DrawEndScene2D(mainvp.sector, RenderState); });
			PostProcess.Unclock();
		}
		// Reset colormap so 2D drawing isn't affected
		RenderState.SetSpecialColormap(CM_DEFAULT, 1);

		di->EndDrawInfo();
		if (eyeCount - eye_ix > 1)
			screen->NextEye(eyeCount);
	}

	return mainvp.sector;
}

void DoWriteSavePic(FileWriter* file, ESSType ssformat, uint8_t* scr, int width, int height, sector_t* viewsector, bool upsidedown)
{
	PalEntry palette[256];
	PalEntry modulateColor;
	auto blend = V_CalcBlend(viewsector, &modulateColor);
	int pixelsize = 1;
	// Apply the screen blend, because the renderer does not provide this.
	if (ssformat == SS_RGB)
	{
		int numbytes = width * height * 3;
		pixelsize = 3;
		if (modulateColor != 0xffffffff)
		{
			float r = modulateColor.r / 255.f;
			float g = modulateColor.g / 255.f;
			float b = modulateColor.b / 255.f;
			for (int i = 0; i < numbytes; i += 3)
			{
				scr[i] = uint8_t(scr[i] * r);
				scr[i + 1] = uint8_t(scr[i + 1] * g);
				scr[i + 2] = uint8_t(scr[i + 2] * b);
			}
		}
		float iblendfac = 1.f - blend.W;
		blend.X *= blend.W;
		blend.Y *= blend.W;
		blend.Z *= blend.W;
		for (int i = 0; i < numbytes; i += 3)
		{
			scr[i] = uint8_t(scr[i] * iblendfac + blend.X);
			scr[i + 1] = uint8_t(scr[i + 1] * iblendfac + blend.Y);
			scr[i + 2] = uint8_t(scr[i + 2] * iblendfac + blend.Z);
		}
	}
	else
	{
		// Apply the screen blend to the palette. The colormap related parts get skipped here because these are already part of the image.
		DoBlending(GPalette.BaseColors, palette, 256, uint8_t(blend.X), uint8_t(blend.Y), uint8_t(blend.Z), uint8_t(blend.W * 255));
	}

	int pitch = width * pixelsize;
	if (upsidedown)
	{
		scr += ((height - 1) * width * pixelsize);
		pitch *= -1;
	}

	M_CreatePNG(file, scr, ssformat == SS_PAL ? palette : nullptr, ssformat, width, height, pitch, vid_gamma);
}

//===========================================================================
//
// Render the view to a savegame picture
//
//===========================================================================

void WriteSavePic(player_t* player, FileWriter* file, int width, int height)
{
	if (!V_IsHardwareRenderer())
	{
		SWRenderer->WriteSavePic(player, file, width, height);
	}
	else
	{
		IntRect bounds;
		bounds.left = 0;
		bounds.top = 0;
		bounds.width = width;
		bounds.height = height;
		auto& RenderState = *screen->RenderState();

		// we must be sure the GPU finished reading from the buffer before we fill it with new data.
		screen->WaitForCommands(false);

		// Switch to render buffers dimensioned for the savepic
		screen->SetSaveBuffers(true);
		screen->ImageTransitionScene(true);

		hw_postprocess.SetTonemapMode(level.info ? level.info->tonemap : ETonemapMode::None);
		hw_ClearFakeFlat();
		screen->mVertexData->Reset();
		RenderState.SetVertexBuffer(screen->mVertexData);
		screen->mLights->Clear();
		screen->mBones->Clear();
		screen->mViewpoints->Clear();

		// This shouldn't overwrite the global viewpoint even for a short time.
		FRenderViewpoint savevp;
		sector_t* viewsector = RenderViewpoint(savevp, players[consoleplayer].camera, &bounds, r_viewpoint.FieldOfView.Degrees(), 1.6f, 1.6f, true, false);
		RenderState.EnableStencil(false);
		RenderState.SetNoSoftLightLevel();

		TArray<uint8_t> scr(width * height * 3, true);
		screen->CopyScreenToBuffer(width, height, scr.Data());

		DoWriteSavePic(file, SS_RGB, scr.Data(), width, height, viewsector, screen->FlipSavePic());

		// Switch back the screen render buffers
		screen->SetViewportRects(nullptr);
		screen->SetSaveBuffers(false);
	}
}

//===========================================================================
//
// Renders the main view
//
//===========================================================================

static void CheckTimer(FRenderState &state, uint64_t ShaderStartTime)
{
	// if firstFrame is not yet initialized, initialize it to current time
	// if we're going to overflow a float (after ~4.6 hours, or 24 bits), re-init to regain precision
	if ((state.firstFrame == 0) || (screen->FrameTime - state.firstFrame >= 1 << 24) || ShaderStartTime >= state.firstFrame)
		state.firstFrame = screen->FrameTime - 1;
}


sector_t* RenderView(player_t* player)
{
	auto RenderState = screen->RenderState();
	RenderState->SetVertexBuffer(screen->mVertexData);
	screen->mVertexData->Reset();
	hw_postprocess.SetTonemapMode(level.info ? level.info->tonemap : ETonemapMode::None);

	sector_t* retsec;
	if (!V_IsHardwareRenderer())
	{
		screen->SetActiveRenderTarget();	// only relevant for Vulkan

		if (!swdrawer) swdrawer = new SWSceneDrawer;
		retsec = swdrawer->RenderView(player);
	}
	else
	{
		hw_ClearFakeFlat();

		iter_dlightf = iter_dlight = draw_dlight = draw_dlightf = 0;

		checkBenchActive();

		// reset statistics counters
		ResetProfilingData();

		// Get this before everything else
		if (cl_capfps || r_NoInterpolate) r_viewpoint.TicFrac = 1.;
		else r_viewpoint.TicFrac = I_GetTimeFrac();

		screen->mLights->Clear();
		screen->mBones->Clear();
		screen->mViewpoints->Clear();

		// NoInterpolateView should have no bearing on camera textures, but needs to be preserved for the main view below.
		bool saved_niv = NoInterpolateView;
		NoInterpolateView = false;

		// Shader start time does not need to be handled per level. Just use the one from the camera to render from.
		if (player->camera)
			CheckTimer(*RenderState, player->camera->Level->ShaderStartTime);

		// Draw all canvases that changed
		for (FCanvas* canvas : AllCanvases)
		{
			if (canvas->Tex && canvas->Tex->CheckNeedsUpdate())
			{
				screen->RenderTextureView(canvas->Tex, [=](IntRect& bounds)
					{
						screen->SetViewportRects(&bounds);
						Draw2D(&canvas->Drawer, *screen->RenderState(), 0, 0, canvas->Tex->GetWidth(), canvas->Tex->GetHeight());
						canvas->Drawer.Clear();
					});
				canvas->Tex->SetUpdated(true);
			}
		}

		// prepare all camera textures that have been used in the last frame.
		// This must be done for all levels, not just the primary one!
		for (auto Level : AllLevels())
		{
			Level->canvasTextureInfo.UpdateAll([&](AActor* camera, FCanvasTexture* camtex, double fov)
				{
					screen->RenderTextureView(camtex, [=](IntRect& bounds)
						{
							FRenderViewpoint texvp;
							float ratio = camtex->aspectRatio / Level->info->pixelstretch;
							RenderViewpoint(texvp, camera, &bounds, fov, ratio, ratio, false, false);
						});
				});
		}
		NoInterpolateView = saved_niv;

		// now render the main view
		float fovratio;
		float ratio = r_viewwindow.WidescreenRatio;
		if (r_viewwindow.WidescreenRatio >= 1.3f)
		{
			fovratio = 1.333333f;
		}
		else
		{
			fovratio = ratio;
		}

		screen->ImageTransitionScene(true); // Only relevant for Vulkan.

		retsec = RenderViewpoint(r_viewpoint, player->camera, NULL, r_viewpoint.FieldOfView.Degrees(), ratio, fovratio, true, true);
	}
	All.Unclock();
	return retsec;
}

