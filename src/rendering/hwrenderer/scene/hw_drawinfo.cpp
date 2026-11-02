// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2000-2018 Christoph Oelckers
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
** gl_drawinfo.cpp
** Basic scene draw info management class
**
*/

#include "a_sharedglobal.h"
#include "r_utility.h"
#include "r_sky.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "hw_fakeflat.h"
#include "hw_portal.h"
#include "hw_renderstate.h"
#include "hw_drawinfo.h"
#include "po_man.h"
#include "models.h"
#include "hw_clock.h"
#include "hw_cvars.h"
#include "hw_viewpointbuffer.h"
#include "flatvertices.h"
#include "hw_lightbuffer.h"
#include "hw_bonebuffer.h"
#include "hw_vrmodes.h"
#include "hw_clipper.h"
#include "v_draw.h"
#include "a_corona.h"
#include "texturemanager.h"
#include "actorinlines.h"
#include "g_levellocals.h"

#include "g_cvars.h"
#include "gl/gl_renderstate.h"
#include "common/rendering/gl/gl_shader.h"
#include "common/rendering/gl/gl_renderer.h"
#include "common/rendering/gl_load/gl_system.h"
#include "common/rendering/gl_load/gl_interface.h"
#include "common/rendering/hwrenderer/data/hw_cvars.h" // for "gl_usegl1mode"
#include "rendering/hwrenderer/scene/hw_drawstructs.h"
#include "rendering/hwrenderer/scene/hw_walldispatcher.h"
#include "common/rendering/hwrenderer/data/hw_renderstate.h"


EXTERN_CVAR(Float, r_visibility)
CVAR(Bool, gl_bandedswlight, false, CVAR_ARCHIVE)
CVAR(Bool, gl_sort_textures, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, gl_no_skyclear, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, gl_enhanced_nv_stealth, 3, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, gl_texture, true, 0)
CVAR(Float, gl_mask_threshold, 0.5f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, gl_mask_sprite_threshold, 0.5f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, gl_coronas, true, CVAR_ARCHIVE);

sector_t * hw_FakeFlat(sector_t * sec, sector_t * dest, area_t in_area, bool back);

//==========================================================================
//
//
//
//==========================================================================

class FDrawInfoList
{
public:
	TDeletingArray<HWDrawInfo *> mList;

	HWDrawInfo * GetNew();
	void Release(HWDrawInfo *);
};


FDrawInfoList di_list;

//==========================================================================
//
// Try to reuse the lists as often as possible as they contain resources that
// are expensive to create and delete.
//
// Note: If multithreading gets used, this class needs synchronization.
//
//==========================================================================

HWDrawInfo *FDrawInfoList::GetNew()
{
	if (mList.Size() > 0)
	{
		HWDrawInfo *di;
		mList.Pop(di);
		return di;
	}
	return new HWDrawInfo();
}

void FDrawInfoList::Release(HWDrawInfo * di)
{
	di->ClearBuffers();
	di->Level = nullptr;
	mList.Push(di);
}

//==========================================================================
//
// Sets up a new drawinfo struct
//
//==========================================================================

HWDrawInfo *HWDrawInfo::StartDrawInfo(FLevelLocals *lev, HWDrawInfo *parent, FRenderViewpoint &parentvp, HWViewpointUniforms *uniforms)
{
	HWDrawInfo *di = di_list.GetNew();
	di->Level = lev;
	di->StartScene(parentvp, uniforms);
	return di;
}


//==========================================================================
//
//
//
//==========================================================================

static Clipper staticClipper;		// Since all scenes are processed sequentially we only need one clipper.
static Clipper staticVClipper;		// Another clipper to clip vertically (used if (VPSF_ALLOWOUTOFBOUNDS & camera->viewpos->Flags)).
static Clipper staticRClipper;		// Another clipper for radar (doesn't actually clip. Changes SSECMF_DRAWN setting).
static HWDrawInfo * gl_drawinfo;	// This is a linked list of all active DrawInfos and needed to free the memory arena after the last one goes out of scope.

HWDrawInfo* HWDrawInfo::CurrentGL1xInfo = nullptr; // initialize global GL1x/GL2x fixed-function frame context keeper

void HWDrawInfo::StartScene(FRenderViewpoint &parentvp, HWViewpointUniforms *uniforms)
{
	staticClipper.Clear();
	staticVClipper.Clear();
	staticRClipper.Clear();
	mClipper = &staticClipper;
	vClipper = &staticVClipper;
	rClipper = &staticRClipper;
	rClipper->amRadar = true;

	Viewpoint = parentvp;
	lightmode = getRealLightmode(Level, true);

	if (uniforms)
	{
		VPUniforms = *uniforms;
		// The clip planes will never be inherited from the parent drawinfo.
		VPUniforms.mClipLine.X = -1000001.f;
		VPUniforms.mClipHeight = 0;
		VPUniforms.mClipHeightDirection = 0.f;
	}
	else
	{
		VPUniforms.mProjectionMatrix.loadIdentity();
		VPUniforms.mViewMatrix.loadIdentity();
		VPUniforms.mNormalViewMatrix.loadIdentity();
		ProjectionMatrix2.loadIdentity();
		VPUniforms.mViewHeight = viewheight;
		if (lightmode == ELightMode::Build)
		{
			VPUniforms.mGlobVis = 1 / 64.f;
			VPUniforms.mPalLightLevels = 32 | (static_cast<int>(gl_fogmode) << 8) | ((int)lightmode << 16);
		}
		else
		{
			VPUniforms.mGlobVis = (float)R_GetGlobVis(r_viewwindow, r_visibility) / 32.f;
			VPUniforms.mPalLightLevels = static_cast<int>(gl_bandedswlight) | (static_cast<int>(gl_fogmode) << 8) | ((int)lightmode << 16);
		}
		VPUniforms.mClipLine.X = -10000000.0f;
		VPUniforms.mShadowmapFilter = gl_shadowmap_filter;
		VPUniforms.mLightBlendMode = (level.info ? (int)level.info->lightblendmode : 0);
	}
	mClipper->SetViewpoint(Viewpoint);
	vClipper->SetViewpoint(Viewpoint);
	rClipper->SetViewpoint(Viewpoint);

	ClearBuffers();

	for (int i = 0; i < GLDL_TYPES; i++) drawlists[i].Reset();
	hudsprites.Clear();
//	Coronas.Clear();
	vpIndex = 0;

	// Fullbright information needs to be propagated from the main view.
	if (outer != nullptr) FullbrightFlags = outer->FullbrightFlags;
	else FullbrightFlags = 0;

	outer = gl_drawinfo;
	gl_drawinfo = this;

}

//==========================================================================
//
//
//
//==========================================================================

HWDrawInfo *HWDrawInfo::EndDrawInfo()
{
	assert(this == gl_drawinfo);
	for (int i = 0; i < GLDL_TYPES; i++) drawlists[i].Reset();
	gl_drawinfo = outer;
	di_list.Release(this);
	if (gl_drawinfo == nullptr)
		ResetRenderDataAllocator();
	return gl_drawinfo;
}


//==========================================================================
//
//
//
//==========================================================================

void HWDrawInfo::ClearBuffers()
{
    otherFloorPlanes.Clear();
    otherCeilingPlanes.Clear();
    floodFloorSegs.Clear();
    floodCeilingSegs.Clear();

	// clear all the lists that might not have been cleared already
	MissingUpperTextures.Clear();
	MissingLowerTextures.Clear();
	MissingUpperSegs.Clear();
	MissingLowerSegs.Clear();
	SubsectorHacks.Clear();
	//CeilingStacks.Clear();
	//FloorStacks.Clear();
	HandledSubsectors.Clear();
	spriteindex = 0;

	if (Level)
	{
		CurrentMapSections.Resize(Level->NumMapSections);
		CurrentMapSections.Zero();

		section_renderflags.Resize(Level->sections.allSections.Size());
		ss_renderflags.Resize(Level->subsectors.Size());
		no_renderflags.Resize(Level->subsectors.Size());

		memset(&section_renderflags[0], 0, Level->sections.allSections.Size() * sizeof(section_renderflags[0]));
		memset(&ss_renderflags[0], 0, Level->subsectors.Size() * sizeof(ss_renderflags[0]));
		memset(&no_renderflags[0], 0, Level->nodes.Size() * sizeof(no_renderflags[0]));
	}

	Decals[0].Clear();
	Decals[1].Clear();

	mClipPortal = nullptr;
	mCurrentPortal = nullptr;
}

//==========================================================================
//
//
//
//==========================================================================

void HWDrawInfo::UpdateCurrentMapSection()
{
	int mapsection = Level->PointInRenderSubsector(Viewpoint.Pos)->mapsection;
	if (Viewpoint.bDoOob || Viewpoint.bDoOrtho)
		mapsection = Level->PointInRenderSubsector(Viewpoint.OffPos)->mapsection;
	CurrentMapSections.Set(mapsection);
}


//-----------------------------------------------------------------------------
//
// Sets the area the camera is in
//
//-----------------------------------------------------------------------------

void HWDrawInfo::SetViewArea()
{
	auto &vp = Viewpoint;
	// The render_sector is better suited to represent the current position in GL
	vp.sector = Level->PointInRenderSubsector(vp.Pos)->render_sector;
	if (Viewpoint.bDoOob)
		vp.sector = Level->PointInRenderSubsector(vp.camera->Pos())->render_sector;

	// Get the heightsec state from the render sector, not the current one!
	if (vp.sector->GetHeightSec())
	{
		in_area = vp.Pos.Z <= vp.sector->heightsec->floorplane.ZatPoint(vp.Pos) ? area_below :
			(vp.Pos.Z > vp.sector->heightsec->ceilingplane.ZatPoint(vp.Pos) &&
				!(vp.sector->heightsec->MoreFlags&SECMF_FAKEFLOORONLY)) ? area_above : area_normal;
	}
	else
	{
		in_area = Level->HasHeightSecs ? area_default : area_normal;	// depends on exposed lower sectors, if map contains heightsecs.
	}
}

//-----------------------------------------------------------------------------
//
// 
//
//-----------------------------------------------------------------------------

int HWDrawInfo::SetFullbrightFlags(player_t *player)
{
	FullbrightFlags = 0;

	// check for special colormaps
	player_t * cplayer = player? player->camera->player : nullptr;
	if (cplayer)
	{
		int cm = CM_DEFAULT;
		if (cplayer->extralight == INT_MIN)
		{
			cm = CM_FIRSTSPECIALCOLORMAP + REALINVERSECOLORMAP;
			Viewpoint.extralight = 0;
			FullbrightFlags = Fullbright;
			// This does never set stealth vision.
		}
		else if (cplayer->fixedcolormap != NOFIXEDCOLORMAP)
		{
			cm = CM_FIRSTSPECIALCOLORMAP + cplayer->fixedcolormap;
			FullbrightFlags = Fullbright;
			if (gl_enhanced_nv_stealth > 2) FullbrightFlags |= StealthVision;
		}
		else if (cplayer->fixedlightlevel != -1)
		{
			auto torchtype = PClass::FindActor(NAME_PowerTorch);
			auto litetype = PClass::FindActor(NAME_PowerLightAmp);
			for (AActor *in = cplayer->mo->Inventory; in; in = in->Inventory)
			{
				// Need special handling for light amplifiers 
				if (in->IsKindOf(torchtype))
				{
					FullbrightFlags = Fullbright;
					if (gl_enhanced_nv_stealth > 1) FullbrightFlags |= StealthVision;
				}
				else if (in->IsKindOf(litetype))
				{
					FullbrightFlags = Fullbright;
					if (gl_enhanced_nightvision) FullbrightFlags |= Nightvision;
					if (gl_enhanced_nv_stealth > 0) FullbrightFlags |= StealthVision;
				}
			}
		}
		return cm;
	}
	else
	{
		return CM_DEFAULT;
	}
}

//-----------------------------------------------------------------------------
//
// R_FrustumAngle
//
//-----------------------------------------------------------------------------

angle_t OoBFrustumAngle(FRenderViewpoint* Viewpoint)
{
	// If pitch is larger than this you can look all around at an FOV of 90 degrees
	if (fabs(Viewpoint->HWAngles.Pitch.Degrees()) > 89.0)  return 0xffffffff;
	int aspMult = AspectMultiplier(r_viewwindow.WidescreenRatio); // 48 == square window
	double absPitch = fabs(Viewpoint->HWAngles.Pitch.Degrees());
	 // Smaller aspect ratios still clip too much. Need a better solution
	if (aspMult > 36 && absPitch > 30.0)  return 0xffffffff;
	else if (aspMult > 40 && absPitch > 25.0)  return 0xffffffff;
	else if (aspMult > 45 && absPitch > 20.0)  return 0xffffffff;
	else if (aspMult > 47 && absPitch > 10.0) return 0xffffffff;

	double xratio = r_viewwindow.FocalTangent / Viewpoint->PitchCos;
	double floatangle = 0.05 + atan ( xratio ) * 48.0 / aspMult; // this is radians
	angle_t a1 = DAngle::fromRad(floatangle).BAMs();

	if (a1 >= ANGLE_90) return 0xffffffff;
	return a1;
}

angle_t HWDrawInfo::FrustumAngle()
{
	if (Viewpoint.bDoOob)
	{
		return OoBFrustumAngle(&Viewpoint);
	}
	else
	{
		float tilt = fabs(Viewpoint.HWAngles.Pitch.Degrees());

		// If the pitch is larger than this you can look all around at a FOV of 90°
		if (tilt > 46.0f) return 0xffffffff;

		// ok, this is a gross hack that barely works...
		// but at least it doesn't overestimate too much...
		double floatangle = 2.0 + (45.0 + ((tilt / 1.9)))*Viewpoint.FieldOfView.Degrees() * 48.0 / AspectMultiplier(r_viewwindow.WidescreenRatio) / 90.0;
		angle_t a1 = DAngle::fromDeg(floatangle).BAMs();
		if (a1 >= ANGLE_180) return 0xffffffff;
		return a1;
	}
}

//-----------------------------------------------------------------------------
//
// Setup the modelview matrix
//
//-----------------------------------------------------------------------------

void HWDrawInfo::SetViewMatrix(const FRotator &angles, float vx, float vy, float vz, bool mirror, bool planemirror)
{
	float mult = mirror ? -1.f : 1.f;
	float planemult = planemirror ? -Level->info->pixelstretch : Level->info->pixelstretch;

	VPUniforms.mViewMatrix.loadIdentity();
	VPUniforms.mViewMatrix.rotate(angles.Roll.Degrees(), 0.0f, 0.0f, 1.0f);
	VPUniforms.mViewMatrix.rotate(angles.Pitch.Degrees(), 1.0f, 0.0f, 0.0f);
	VPUniforms.mViewMatrix.rotate(angles.Yaw.Degrees(), 0.0f, mult, 0.0f);
	VPUniforms.mViewMatrix.translate(vx * mult, -vz * planemult, -vy);
	VPUniforms.mViewMatrix.scale(-mult, planemult, 1);
}


//-----------------------------------------------------------------------------
//
// SetupView
// Setup the view rotation matrix for the given viewpoint
//
//-----------------------------------------------------------------------------
void HWDrawInfo::SetupView(FRenderState &state, float vx, float vy, float vz, bool mirror, bool planemirror)
{
	auto &vp = Viewpoint;
	vp.SetViewAngle(r_viewwindow);
	SetViewMatrix(vp.HWAngles, vx, vy, vz, mirror, planemirror);
	SetCameraPos(vp.Pos);
	VPUniforms.CalcDependencies();
	vpIndex = screen->mViewpoints->SetViewpoint(state, &VPUniforms);
}

//-----------------------------------------------------------------------------
//
//
//
//-----------------------------------------------------------------------------

HWPortal * HWDrawInfo::FindPortal(const void * src)
{
	int i = Portals.Size() - 1;

	while (i >= 0 && Portals[i] && Portals[i]->GetSource() != src) i--;
	return i >= 0 ? Portals[i] : nullptr;
}

//-----------------------------------------------------------------------------
//
//
//
//-----------------------------------------------------------------------------

HWDecal *HWDrawInfo::AddDecal(bool onmirror)
{
	auto decal = (HWDecal*)RenderDataAllocator.Alloc(sizeof(HWDecal));
	Decals[onmirror ? 1 : 0].Push(decal);
	return decal;
}

//-----------------------------------------------------------------------------
//
// CreateScene
//
// creates the draw lists for the current scene
//
//-----------------------------------------------------------------------------

void HWDrawInfo::CreateScene(bool drawpsprites)
{
	const auto &vp = Viewpoint;
	angle_t a1 = FrustumAngle(); // horizontally clip the back of the viewport
	mClipper->SafeAddClipRangeRealAngles(vp.Angles.Yaw.BAMs() + a1, vp.Angles.Yaw.BAMs() - a1);
	Viewpoint.FrustAngle = a1;
	if (Viewpoint.bDoOob) // No need for vertical clipper if viewpoint not allowed out of bounds
	{
		double a2 = 20.0 + 0.5*Viewpoint.FieldOfView.Degrees(); // FrustumPitch for vertical clipping
		if (a2 > 179.0) a2 = 179.0;
		double pitchmult = !!(portalState.PlaneMirrorFlag & 1) ? -1.0 : 1.0;
		vClipper->SafeAddClipRangeDegPitches(pitchmult * vp.HWAngles.Pitch.Degrees() - a2, pitchmult * vp.HWAngles.Pitch.Degrees() + a2); // clip the suplex range
		Viewpoint.PitchSin *= pitchmult;
	}

	// reset the portal manager
	portalState.StartFrame();

	ProcessAll.Clock();

	// clip the scene and fill the drawlists
	screen->mVertexData->Map();
	screen->mLights->Map();
	screen->mBones->Map();

	RenderBSP(Level->HeadNode(), drawpsprites);

	// And now the crappy hacks that have to be done to avoid rendering anomalies.
	// These cannot be multithreaded when the time comes because all these depend
	// on the global 'validcount' variable.

	HandleMissingTextures(in_area);	// Missing upper/lower textures
	HandleHackedSubsectors();	// open sector hacks for deep water
	PrepareUnhandledMissingTextures();
	DispatchRenderHacks();
	screen->mLights->Unmap();
	screen->mBones->Unmap();
	screen->mVertexData->Unmap();

	ProcessAll.Unclock();

}

//-----------------------------------------------------------------------------
//
// RenderScene
//
// Draws the current draw lists for the non GLSL renderer
//
//-----------------------------------------------------------------------------

void HWDrawInfo::RenderScene(FRenderState &state)
{
	const auto &vp = Viewpoint;
	RenderAll.Clock();

	state.SetDepthMask(true);

	state.EnableFog(true);
	state.SetRenderStyle(STYLE_Source);

	if (gl_sort_textures)
	{
		drawlists[GLDL_PLAINWALLS].SortWalls();
		drawlists[GLDL_PLAINFLATS].SortFlats();
		drawlists[GLDL_MASKEDWALLS].SortWalls();
		drawlists[GLDL_MASKEDFLATS].SortFlats();
		drawlists[GLDL_MASKEDWALLSOFS].SortWalls();
	}

	bool haslights = (FullbrightFlags == CM_DEFAULT) && gl_lights;
	bool hasbrightmaps = state.IsBrightmapEnabled();
	//bool hasbrightmaps = OpenGLRenderer::gl_RenderState.IsBrightmapEnabled();

	// process everything that needs to handle textured dynamic lights.
	if (gl.legacyMode && (haslights || hasbrightmaps))
	{
		RenderMultipassStuff();

		// DISABLED DUE TO BETTER IMPLEMENTATION FOUND
		//	// --- Legacy GL1x/GL2x overbright dynlights pass - START ---
		//if (gl_legacy_dynlight_overbright && gl.legacyMode && (FullbrightFlags == CM_DEFAULT))
		//{
		//	// Pure OpenGL 1.1 State Machine Lock - 100% isolated from state caches!
		//	glDisable(GL_FOG);        // Ensure hardware fixed fog doesn't blow out our spots
		//	glDepthMask(false);       // Lock Z-Buffer writing to eliminate depth-fighting seams
		//	glDepthFunc(GL_EQUAL);    // Only draw pixels that perfectly match existing coordinates
		//
		//	glEnable(GL_BLEND);
		//	glBlendFunc(GL_DST_COLOR, GL_ONE); // Multiplicative overbright blending window
		//
		//	// Push state machine matrix target cleanly before calling drawlists
		//	glMatrixMode(GL_TEXTURE);
		//
		//	// --- PART 1: WALLS OVERBRIGHT PIPELINE SPEED-UP ---
		//	if (drawlists[GLDL_PLAINWALLS].drawitems.Size() > 0)
		//		drawlists[GLDL_PLAINWALLS].DrawWalls(this, state, true);
		//
		//	if (drawlists[GLDL_MASKEDWALLS].drawitems.Size() > 0)
		//		drawlists[GLDL_MASKEDWALLS].DrawWalls(this, state, true);
		//
		//	if (drawlists[GLDL_FOGWALLS].drawitems.Size() > 0)
		//		drawlists[GLDL_FOGWALLS].DrawWalls(this, state, true);
		//
		//	if (drawlists[GLDL_FOGMASKEDWALLS].drawitems.Size() > 0)
		//		drawlists[GLDL_FOGMASKEDWALLS].DrawWalls(this, state, true);
		//
		//
		//	// --- PART 2: FLATS OVERBRIGHT PIPELINE SPEED-UP ---
		//	if (drawlists[GLDL_PLAINFLATS].drawitems.Size() > 0)
		//		drawlists[GLDL_PLAINFLATS].DrawFlats(this, state, true);
		//
		//	if (drawlists[GLDL_MASKEDFLATS].drawitems.Size() > 0)
		//		drawlists[GLDL_MASKEDFLATS].DrawFlats(this, state, true);
		//
		//	if (drawlists[GLDL_FOGFLATS].drawitems.Size() > 0)
		//		drawlists[GLDL_FOGFLATS].DrawFlats(this, state, true);
		//
		//	if (drawlists[GLDL_FOGMASKEDFLATS].drawitems.Size() > 0)
		//		drawlists[GLDL_FOGMASKEDFLATS].DrawFlats(this, state, true);
		//
		//
		//	// FIXED-FUNCTION SYMMETRIC CONTEXT RESET
		//	glMatrixMode(GL_MODELVIEW);
		//
		//	glEnable(GL_FOG);
		//	glDepthMask(true);
		//	glDepthFunc(GL_LESS);
		//	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		//
		//	// Revert texture environment configuration back from custom overbright leaks
		//	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		//
		//	// SYNCHRONIZER FOR SKYBOX 3D MODELS
		//	// Hard-flushing via localized method state reference object safely
		//	state.EnableFog(true);
		//	state.BlendGL1xFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		//	state.SetTextureMode(TM_MODULATE);
		//
		//	// Force clear internal texture units generation slots straight in hardware
		//	glActiveTexture(GL_TEXTURE1);
		//	glDisable(GL_TEXTURE_GEN_S);
		//	glDisable(GL_TEXTURE_GEN_T);
		//	glActiveTexture(GL_TEXTURE0);
		//	glDisable(GL_TEXTURE_GEN_S);
		//	glDisable(GL_TEXTURE_GEN_T);
		//
		//	// THE MONOLITHIC SYNCHRONIZATION FLUSH
		//	OpenGLRenderer::ApplyGL1xStateBridge(state, nullptr);
		//}
		//	// --- Legacy GL1x/GL2x overbright dynlights pass - FINISH ---
	}

	// Part 1: solid geometry. This is set up so that there are no transparent parts
	state.SetDepthFunc(DF_Less);
	state.AlphaFunc(Alpha_GEqual, 0.f);
	state.ClearDepthBias();

	state.EnableTexture(gl_texture);
	state.EnableBrightmap(true);
	drawlists[GLDL_PLAINWALLS].DrawWalls(this, state, false);
	drawlists[GLDL_PLAINFLATS].DrawFlats(this, state, false);


	// Part 2: masked geometry. This is set up so that only pixels with alpha>gl_mask_threshold will show
	state.AlphaFunc(Alpha_GEqual, gl_mask_threshold);
	drawlists[GLDL_MASKEDWALLS].DrawWalls(this, state, false);
	drawlists[GLDL_MASKEDFLATS].DrawFlats(this, state, false);

	// Part 3: masked geometry with polygon offset. This list is empty most of the time so only waste time on it when in use.
	if (drawlists[GLDL_MASKEDWALLSOFS].Size() > 0)
	{
		state.SetDepthBias(-1, -128);
		drawlists[GLDL_MASKEDWALLSOFS].DrawWalls(this, state, false);
		state.ClearDepthBias();
	}

	screen->mBones->Map();
	drawlists[GLDL_MODELS].Draw(this, state, false);
	screen->mBones->Unmap();

	state.SetRenderStyle(STYLE_Translucent);

	// Part 4: Draw decals (not a real pass)
	state.SetDepthFunc(DF_LEqual);
	DrawDecals(state, Decals[0]);

	// --- Legacy GL1x/GL2x brightmaps block start ---
	if (gl.legacyMode && OpenGLRenderer::gl_RenderState.IsBrightmapEnabled())
	{
		// Prepare global state
		OpenGLRenderer::gl_RenderState.EnableFog(false);
		OpenGLRenderer::gl_RenderState.SetTextureMode(TM_BRIGHTMAP_LEGACY);
		OpenGLRenderer::gl_RenderState.SetDynLight(0, 0, 0);
		OpenGLRenderer::gl_RenderState.SetAddColor(0);
		OpenGLRenderer::gl_RenderState.SetObjectColor(0xffffffff);
		OpenGLRenderer::gl_RenderState.BlendGL1xFunc(GL_ONE, GL_ONE); // Additive blending

		// Set Brightmap Intensity (Effect Amount)
		float intensity = 1.0f;
		OpenGLRenderer::gl_RenderState.SetColor(intensity, intensity, intensity, 1.0f);

		// Precision & Depth Control
		if (gl_seamless)
		{
			glEnable(GL_POLYGON_OFFSET_FILL);
			glPolygonOffset(-0.125f, -0.125f);
			glDepthFunc(GL_LEQUAL);
		}
		else
		{
			glDepthFunc(GL_EQUAL);
		}

		glDepthMask(false);

		// 4. Draw Geometry Lists
		// [Darkcrafter07] - Fully swapped legacy vectors with stable, filled 4.14dev memory tracks!
		drawlists[GLDL_PLAINWALLS].DrawWalls(this, state, false);
		drawlists[GLDL_PLAINFLATS].DrawFlats(this, state, false);
		drawlists[GLDL_MASKEDWALLS].DrawWalls(this, state, false);
		drawlists[GLDL_MASKEDFLATS].DrawFlats(this, state, false);

		// Draw Dynamic/Multipass Geometry Lists
		drawlists[GLDL_PLAINWALLS].DrawWalls(this, state, false);
		drawlists[GLDL_MASKEDWALLS].DrawWalls(this, state, false);
		drawlists[GLDL_PLAINFLATS].DrawFlats(this, state, false);
		drawlists[GLDL_MASKEDFLATS].DrawFlats(this, state, false);

		drawlists[GLDL_FOGWALLS].DrawWalls(this, state, false);
		drawlists[GLDL_FOGMASKEDWALLS].DrawWalls(this, state, false);
		drawlists[GLDL_FOGFLATS].DrawFlats(this, state, false);
		drawlists[GLDL_FOGMASKEDFLATS].DrawFlats(this, state, false);

		// Restore states
		if (gl_seamless)
		{
			glDisable(GL_POLYGON_OFFSET_FILL);
		}
		glDepthFunc(GL_LESS);
		glDepthMask(true);
		OpenGLRenderer::gl_RenderState.EnableFog(true);
		OpenGLRenderer::gl_RenderState.BlendGL1xFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		OpenGLRenderer::gl_RenderState.SetTextureMode(TM_MODULATE);
	}
	// --- Legacy GL1x/GL2x brightmaps block finish ---

	RenderAll.Unclock();
}

//-----------------------------------------------------------------------------
//
// RenderTranslucent
//
//-----------------------------------------------------------------------------

void HWDrawInfo::RenderTranslucent(FRenderState &state)
{
	RenderAll.Clock();

	// final pass: translucent stuff
	state.AlphaFunc(Alpha_GEqual, gl_mask_sprite_threshold);
	state.SetRenderStyle(STYLE_Translucent);

	state.EnableBrightmap(true);
	drawlists[GLDL_TRANSLUCENTBORDER].Draw(this, state, true);
	state.SetDepthMask(false);

	drawlists[GLDL_TRANSLUCENT].DrawSorted(this, state);
	state.EnableBrightmap(false);


	state.AlphaFunc(Alpha_GEqual, 0.5f);
	state.SetDepthMask(true);

	RenderAll.Unclock();
}


//-----------------------------------------------------------------------------
//
// RenderTranslucent
//
//-----------------------------------------------------------------------------

void HWDrawInfo::RenderPortal(HWPortal *p, FRenderState &state, bool usestencil)
{
	auto gp = static_cast<HWPortal *>(p);
	gp->SetupStencil(this, state, usestencil);
	auto new_di = StartDrawInfo(this->Level, this, Viewpoint, &VPUniforms);
	new_di->mCurrentPortal = gp;
	state.SetLightIndex(-1);
	gp->DrawContents(new_di, state);
	new_di->EndDrawInfo();
	state.SetVertexBuffer(screen->mVertexData);
	screen->mViewpoints->Bind(state, vpIndex);
	gp->RemoveStencil(this, state, usestencil);

}

void HWDrawInfo::DrawCorona(FRenderState& state, ACorona* corona, double dist)
{
#if 0
	spriteframe_t* sprframe = &SpriteFrames[sprites[corona->sprite].spriteframes + (size_t)corona->SpawnState->GetFrame()];
	FTextureID patch = sprframe->Texture[0];
	if (!patch.isValid()) return;
	auto tex = TexMan.GetGameTexture(patch, false);
	if (!tex || !tex->isValid()) return;

	// Project the corona sprite center
	FVector4 worldPos((float)corona->X(), (float)corona->Z(), (float)corona->Y(), 1.0f);
	FVector4 viewPos, clipPos;
	VPUniforms.mViewMatrix.multMatrixPoint(&worldPos[0], &viewPos[0]);
	VPUniforms.mProjectionMatrix.multMatrixPoint(&viewPos[0], &clipPos[0]);
	if (clipPos.W < -1.0f) return; // clip z nearest
	float halfViewportWidth = screen->GetWidth() * 0.5f;
	float halfViewportHeight = screen->GetHeight() * 0.5f;
	float invW = 1.0f / clipPos.W;
	float screenX = halfViewportWidth + clipPos.X * invW * halfViewportWidth;
	float screenY = halfViewportHeight - clipPos.Y * invW * halfViewportHeight;

	float alpha = corona->CoronaFade * float(corona->Alpha);

	// distance-based fade - looks better IMO
	float distNearFadeStart = float(corona->RenderRadius()) * 0.1f;
	float distFarFadeStart = float(corona->RenderRadius()) * 0.5f;
	float distFade = 1.0f;

	if (float(dist) < distNearFadeStart)
		distFade -= abs(((float(dist) - distNearFadeStart) / distNearFadeStart));
	else if (float(dist) >= distFarFadeStart)
		distFade -= (float(dist) - distFarFadeStart) / distFarFadeStart;

	alpha *= distFade;

	state.SetColorAlpha(0xffffff, alpha, 0);
	if (isSoftwareLighting()) state.SetSoftLightLevel(255);
	else state.SetNoSoftLightLevel();

	state.SetLightIndex(-1);
	state.SetRenderStyle(corona->RenderStyle);
	state.SetTextureMode(corona->RenderStyle);

	state.SetMaterial(tex, UF_Sprite, CTF_Expand, CLAMP_XY_NOMIP, 0, 0);

	float scale = screen->GetHeight() / 1000.0f;
	float tileWidth = corona->Scale.X * tex->GetDisplayWidth() * scale;
	float tileHeight = corona->Scale.Y * tex->GetDisplayHeight() * scale;
	float x0 = screenX - tileWidth, y0 = screenY - tileHeight;
	float x1 = screenX + tileWidth, y1 = screenY + tileHeight;

	float u0 = 0.0f, v0 = 0.0f;
	float u1 = 1.0f, v1 = 1.0f;

	auto vert = screen->mVertexData->AllocVertices(4);
	auto vp = vert.first;
	unsigned int vertexindex = vert.second;

	vp[0].Set(x0, y0, 1.0f, u0, v0);
	vp[1].Set(x1, y0, 1.0f, u1, v0);
	vp[2].Set(x0, y1, 1.0f, u0, v1);
	vp[3].Set(x1, y1, 1.0f, u1, v1);

	state.Draw(DT_TriangleStrip, vertexindex, 4);
#endif
}

//==========================================================================
//
// TraceCallbackForDitherTransparency
// Toggles dither flag on anything that occludes the actor's
// position from viewpoint.
//
//==========================================================================

static ETraceStatus TraceCallbackForDitherTransparency(FTraceResults& res, void* userdata)
{
	BitArray* CurMapSections = (BitArray*)userdata;
	double bf, bc;

	switch(res.HitType)
	{
	case TRACE_HitWall:
		{
			sector_t* linesec = res.Line->sidedef[res.Side]->sector;
			if (linesec->subsectorcount > 0 && (*CurMapSections)[linesec->subsectors[0]->mapsection])
			{
				bf = res.Line->sidedef[res.Side]->sector->floorplane.ZatPoint(res.HitPos.XY());
				bc = res.Line->sidedef[res.Side]->sector->ceilingplane.ZatPoint(res.HitPos.XY());
				if (res.Line->sidedef[!res.Side])
				{
					// Two sided line! So let's find out if mid, top, or bottom texture needs dithered transparency
					bf = max(bf, res.Line->sidedef[!res.Side]->sector->floorplane.ZatPoint(res.HitPos.XY()));
					bc = min(bc, res.Line->sidedef[!res.Side]->sector->ceilingplane.ZatPoint(res.HitPos.XY()));
					if (res.HitPos.Z <= bf) res.Line->sidedef[res.Side]->Flags |= WALLF_DITHERTRANS_BOTTOM;
					else if (res.HitPos.Z < bc) res.Line->sidedef[res.Side]->Flags |= WALLF_DITHERTRANS_MID;
					else res.Line->sidedef[res.Side]->Flags |= WALLF_DITHERTRANS_TOP;

					res.Line->sidedef[res.Side]->dithertranscount = max<int>(1, res.Line->sidedef[!res.Side]->sector->e->XFloor.ffloors.Size());
				}
				else if ((res.HitPos.Z <= bc) && (res.HitPos.Z >= bf))
				{
					res.Line->sidedef[res.Side]->Flags |= WALLF_DITHERTRANS_MID;
					res.Line->sidedef[res.Side]->dithertranscount = 1;
				}
			}
		}
		break;
	case TRACE_HitFloor:
		if (res.Sector->subsectorcount > 0 && (*CurMapSections)[res.Sector->subsectors[0]->mapsection] && res.HitVector.dot(res.Sector->floorplane.Normal()) < 0.0)
		{
			if (res.HitPos.Z == res.Sector->floorplane.ZatPoint(res.HitPos))
			{
				res.Sector->floorplane.dithertransflag = true;
			}
			else if (res.Sector->e->XFloor.ffloors.Size()) // Maybe it was 3D floors
			{
				F3DFloor *rover;
				int kk;
				for (kk = 0; kk < (int)res.Sector->e->XFloor.ffloors.Size(); kk++)
				{
					rover = res.Sector->e->XFloor.ffloors[kk];
					if ((rover->flags&(FF_EXISTS | FF_RENDERPLANES | FF_THISINSIDE)) == (FF_EXISTS | FF_RENDERPLANES))
					{
						if (res.HitPos.Z == rover->top.plane->ZatPoint(res.HitPos))
						{
							rover->top.plane->dithertransflag = true;
							break; // Out of for loop
						}
					}
				}
			}
		}
		break;
	case TRACE_HitCeiling:
		if (res.Sector->subsectorcount > 0 && (*CurMapSections)[res.Sector->subsectors[0]->mapsection] && res.HitVector.dot(res.Sector->ceilingplane.Normal()) < 0.0)
		{
			if (res.HitPos.Z == res.Sector->ceilingplane.ZatPoint(res.HitPos))
			{
				res.Sector->ceilingplane.dithertransflag = true;
			}
			else if (res.Sector->e->XFloor.ffloors.Size()) // Maybe it was 3D floors
			{
				F3DFloor *rover;
				int kk;
				for (kk = 0; kk < (int)res.Sector->e->XFloor.ffloors.Size(); kk++)
				{
					rover = res.Sector->e->XFloor.ffloors[kk];
					if ((rover->flags&(FF_EXISTS | FF_RENDERPLANES | FF_THISINSIDE)) == (FF_EXISTS | FF_RENDERPLANES))
					{
						if (res.HitPos.Z == rover->bottom.plane->ZatPoint(res.HitPos))
						{
							rover->bottom.plane->dithertransflag = true;
							break; // Out of for loop
						}
					}
				}
			}
		}
		break;
	case TRACE_HitActor:
	default:
		break;
	}

	return TRACE_ContinueOutOfBounds;
}


void HWDrawInfo::SetDitherTransFlags(AActor* actor)
{
	// This should really be moved to a shader and have the GPU do some shape-tracing.
	if (actor && actor->Sector)
	{
		FTraceResults results;
		double horix = Viewpoint.Sin * actor->radius;
		double horiy = Viewpoint.Cos * actor->radius;
		DVector3 actorpos = actor->Pos();
		DVector3 vvec = actorpos - Viewpoint.Pos;
		if (Viewpoint.bDoOrtho)
		{
			vvec = 5.0 * Viewpoint.camera->ViewPos->Offset.Length() * Viewpoint.ViewVector3D; // Should be 4.0? (since zNear is behind screen by 3*dist in VREyeInfo::GetProjection())
		}
		double distance = vvec.Length() - actor->radius;
		DVector3 campos = actorpos - vvec;
		sector_t* startsec;

		vvec = vvec.Unit();
		campos.X -= horix; campos.Y += horiy; campos.Z += actor->Height * 0.25;
		for (int iter = 0; iter < 3; iter++)
		{
			startsec = Level->PointInRenderSubsector(campos)->sector;
			Trace(campos, startsec, vvec, distance,
				  0, 0, actor, results, TRACE_PortalRestrict, TraceCallbackForDitherTransparency, &CurrentMapSections);
			campos.Z += actor->Height * 0.5;
			Trace(campos, startsec, vvec, distance,
				  0, 0, actor, results, TRACE_PortalRestrict, TraceCallbackForDitherTransparency, &CurrentMapSections);
			campos.Z -= actor->Height * 0.5;
			campos.X += horix; campos.Y -= horiy;
		}

		// Tracers don't work on 3D floors when you are starting in the same sector (standing under them, for example)
		if (actor->Sector->e->XFloor.ffloors.Size()) // 3D floor
		{
			F3DFloor *rover;
			for (int kk = 0; kk < (int)actor->Sector->e->XFloor.ffloors.Size(); kk++)
			{
				rover = actor->Sector->e->XFloor.ffloors[kk];
				rover->top.plane->dithertransflag = true;
				rover->bottom.plane->dithertransflag = true;
			}
		}
	}
}

static ETraceStatus CheckForViewpointActor(FTraceResults& res, void* userdata)
{
	FRenderViewpoint* data = (FRenderViewpoint*)userdata;
	if (res.HitType == TRACE_HitActor && res.Actor && res.Actor == data->ViewActor)
	{
		return TRACE_Skip;
	}

	return TRACE_Stop;
}


void HWDrawInfo::DrawCoronas(FRenderState& state)
{
	state.EnableDepthTest(false);
	state.SetDepthMask(false);

	HWViewpointUniforms vp = VPUniforms;
	vp.mViewMatrix.loadIdentity();
	vp.mProjectionMatrix = VRMode::GetVRMode(true)->GetHUDSpriteProjection();
	screen->mViewpoints->SetViewpoint(state, &vp);

	float timeElapsed = (screen->FrameTime - LastFrameTime) / 1000.0f;
	LastFrameTime = screen->FrameTime;

#if 0
	for (ACorona* corona : Coronas)
	{
		auto cPos = corona->Vec3Offset(0., 0., corona->Height * 0.5);
		DVector3 direction = Viewpoint.Pos - cPos;
		double dist = direction.Length();

		// skip coronas that are too far
		if (dist > corona->RenderRadius())
			continue;

		static const float fadeSpeed = 9.0f;

		direction.MakeUnit();
		FTraceResults results;
		if (!Trace(cPos, corona->Sector, direction, dist, MF_SOLID, ML_BLOCKEVERYTHING, corona, results, 0, CheckForViewpointActor, &Viewpoint))
		{
			corona->CoronaFade = std::min(corona->CoronaFade + timeElapsed * fadeSpeed, 1.0f);
		}
		else
		{
			corona->CoronaFade = std::max(corona->CoronaFade - timeElapsed * fadeSpeed, 0.0f);
		}

		if (corona->CoronaFade > 0.0f)
			DrawCorona(state, corona, dist);
	}
#endif

	state.SetTextureMode(TM_NORMAL);
	screen->mViewpoints->Bind(state, vpIndex);
	state.EnableDepthTest(true);
	state.SetDepthMask(true);
}


//-----------------------------------------------------------------------------
//
// Draws player sprites and color blend
//
//-----------------------------------------------------------------------------


void HWDrawInfo::EndDrawScene(sector_t * viewsector, FRenderState &state)
{
	state.EnableFog(false);

	/*if (gl_coronas && Coronas.Size() > 0)
	{
		DrawCoronas(state);
	}*/

	// [BB] HUD models need to be rendered here. 
	const bool renderHUDModel = IsHUDModelForPlayerAvailable(players[consoleplayer].camera->player);
	if (renderHUDModel)
	{
		// [BB] The HUD model should be drawn over everything else already drawn.
		state.Clear(CT_Depth);
		screen->mBones->Map();
		DrawPlayerSprites(true, state);
		screen->mBones->Unmap();
	}

	state.EnableStencil(false);
	state.SetViewport(screen->mScreenViewport.left, screen->mScreenViewport.top, screen->mScreenViewport.width, screen->mScreenViewport.height);

	// Restore standard rendering state
	state.SetRenderStyle(STYLE_Translucent);
	state.ResetColor();
	state.EnableTexture(true);
	state.SetScissor(0, 0, -1, -1);
}

void HWDrawInfo::DrawEndScene2D(sector_t * viewsector, FRenderState &state)
{
	const bool renderHUDModel = IsHUDModelForPlayerAvailable(players[consoleplayer].camera->player);
	auto vrmode = VRMode::GetVRMode(true);

	HWViewpointUniforms vp = VPUniforms;
	vp.mViewMatrix.loadIdentity();
	vp.mProjectionMatrix = vrmode->GetHUDSpriteProjection();
	screen->mViewpoints->SetViewpoint(state, &vp);
	state.EnableDepthTest(false);
	state.EnableMultisampling(false);

	DrawPlayerSprites(false, state);

	state.SetNoSoftLightLevel();

	// Restore standard rendering state
	state.SetRenderStyle(STYLE_Translucent);
	state.ResetColor();
	state.EnableTexture(true);
	state.SetScissor(0, 0, -1, -1);
}

//-----------------------------------------------------------------------------
//
// sets 3D viewport and initial state
//
//-----------------------------------------------------------------------------

void HWDrawInfo::Set3DViewport(FRenderState &state)
{
	// Always clear all buffers with scissor test disabled.
	// This is faster on newer hardware because it allows the GPU to skip
	// reading from slower memory where the full buffers are stored.
	state.SetScissor(0, 0, -1, -1);
	state.Clear(CT_Color | CT_Depth | CT_Stencil);

	const auto &bounds = screen->mSceneViewport;
	state.SetViewport(bounds.left, bounds.top, bounds.width, bounds.height);
	state.SetScissor(bounds.left, bounds.top, bounds.width, bounds.height);
	state.EnableMultisampling(true);
	state.EnableDepthTest(true);
	state.EnableStencil(true);
	state.SetStencil(0, SOP_Keep, SF_AllOn);
}

//-----------------------------------------------------------------------------
//
// gl_drawscene - this function renders the scene from the current
// viewpoint, including mirrors and skyboxes and other portals
// It is assumed that the HWPortal::EndFrame returns with the 
// stencil, z-buffer and the projection matrix intact!
//
//-----------------------------------------------------------------------------

void HWDrawInfo::DrawScene(int drawmode)
{
	static int recursion = 0;
	static int ssao_portals_available = 0;
	auto& vp = Viewpoint;

	bool applySSAO = false;
	if (drawmode == DM_MAINVIEW)
	{
		ssao_portals_available = gl_ssao_portals;
		applySSAO = true;
		if (r_dithertransparency && vp.bDoOob)
		{
			vp.camera->tracer ? SetDitherTransFlags(vp.camera->tracer) : SetDitherTransFlags(players[consoleplayer].mo);
		}
	}
	else if (drawmode == DM_OFFSCREEN)
	{
		ssao_portals_available = 0;
	}
	else if (drawmode == DM_PORTAL && ssao_portals_available > 0)
	{
		applySSAO = (mCurrentPortal->AllowSSAO() || Level->flags3&LEVEL3_SKYBOXAO);
		ssao_portals_available--;
	}

	if (vp.camera != nullptr)
	{
		ActorRenderFlags savedflags = vp.camera->renderflags;
		CreateScene(drawmode == DM_MAINVIEW);
		vp.camera->renderflags = savedflags;
	}
	else
	{
		CreateScene(false);
	}
	auto& RenderState = *screen->RenderState();

	RenderState.SetDepthMask(true);
	if (!gl_no_skyclear) portalState.RenderFirstSkyPortal(recursion, this, RenderState);

	RenderScene(RenderState);

	if (applySSAO && RenderState.GetPassType() == GBUFFER_PASS)
	{
		screen->AmbientOccludeScene(VPUniforms.mProjectionMatrix.get()[5]);
		screen->mViewpoints->Bind(RenderState, vpIndex);
	}

	// Handle all portals after rendering the opaque objects but before
	// doing all translucent stuff
	recursion++;
	portalState.EndFrame(this, RenderState);
	recursion--;
	RenderTranslucent(RenderState);
}

//-----------------------------------------------------------------------------
//
// R_RenderView - renders one view - either the screen or a camera texture
//
//-----------------------------------------------------------------------------

void HWDrawInfo::ProcessScene(bool toscreen)
{
	portalState.BeginScene();

	int mapsection = Level->PointInRenderSubsector(Viewpoint.Pos)->mapsection;
	if (Viewpoint.bDoOob || Viewpoint.bDoOrtho)
		mapsection = Level->PointInRenderSubsector(Viewpoint.OffPos)->mapsection;
	CurrentMapSections.Set(mapsection);
	DrawScene(toscreen ? DM_MAINVIEW : DM_OFFSCREEN);
}

//==========================================================================
//
//
//
//==========================================================================

void HWDrawInfo::AddSubsectorToPortal(FSectorPortalGroup *ptg, subsector_t *sub)
{
	auto portal = FindPortal(ptg);
	if (!portal)
	{
        portal = new HWSectorStackPortal(&portalState, ptg);
		Portals.Push(portal);
	}
    auto ptl = static_cast<HWSectorStackPortal*>(portal);
	ptl->AddSubsector(sub);
}







// =============== GL1x/GL2x functionality - START =====================
#include "common/scripting/vm/vm.h"
#include "rendering/hwrenderer/scene/hw_clipper.h"
#include "common/rendering/gl/gl_hwtexture.h"

// global frame textures counters tracker for OpenGLFrameBuffer::Swap loops
int camtexcount = 0;
const bool gl_aalines = false;

EXTERN_CVAR(Bool, r_drawplayersprites)

angle_t HWDrawInfo::FrustumAngleGL1x()
{
	// Extract absolute camera pitch directly from the native 4.14dev viewpoint tracking structures
	float tilt = (float)fabs(Viewpoint.Angles.Pitch.Degrees());

	// If the pitch is larger than this you can look all around at a FOV of 90 degrees
	if (tilt > 46.0f)
	{
		return 0xffffffff;
	}

	// [Darkcrafter07] - Universal 4.14dev math translation bridge!
	// Replaced old GLRenderer matrix variables with active Viewpoint and Level locals properties.
	float floatangle = 2.0 + (45.0 + (tilt / 1.9)) * Viewpoint.FieldOfView.Degrees() * 48.0 / r_viewwindow.WidescreenRatio / 90.0;

	angle_t a1 = DAngle::fromDeg(floatangle).BAMs();

	if (a1 >= ANGLE_180)
	{
		return 0xffffffff;
	}

	return a1;
}

void HWDrawInfo::SetViewAreaGL1x()
{
	// Safety context guard protection shields from parsing unbound map subsectors chunks
	if (Level == nullptr || Viewpoint.sector == nullptr) return;

	// [Darkcrafter07] - Securely update our dedicated legacy camera position status slot tracker
	// Re-mapped from old r_viewpoint.sector straight to the native 4.14dev camera tracker
	if (Viewpoint.sector->GetHeightSec())
	{
		// 0: area_normal, 1: area_above, 2: area_below matching the old enum definitions
		if (Viewpoint.Pos.Z <= Viewpoint.sector->heightsec->floorplane.ZatPoint(Viewpoint.Pos))
		{
			in_area_legacy = 2; // area_below
		}
		else if (Viewpoint.Pos.Z > Viewpoint.sector->heightsec->ceilingplane.ZatPoint(Viewpoint.Pos) &&
			!(Viewpoint.sector->heightsec->MoreFlags & SECMF_FAKEFLOORONLY))
		{
			in_area_legacy = 1; // area_above
		}
		else
		{
			in_area_legacy = 0; // area_normal
		}
	}
	else
	{
		// Fallback check maps safely to the active 4.14dev level local flags registries matrix
		in_area_legacy = Level->HasHeightSecs ? 0 : 0; // area_default maps cleanly to normal tracking lanes
	}
}

void HWDrawInfo::Set3DViewportGL1x(bool mainview)
{
	// Re-mapped from old GLRenderer->mBuffers to the native 4.14dev screen context loop
	if (mainview)
	{
		bool useSSAO = (gl_ssao != 0);
		// Fixed pipeline bypasses modern deferred rendering gbuffer layers completely
		OpenGLRenderer::gl_RenderState.SetPassType(NORMAL_PASS);
		OpenGLRenderer::gl_RenderState.EnableDrawBuffers(OpenGLRenderer::gl_RenderState.GetPassDrawBufferCount());

		// Flush our restored legacy state machine directly into target hardware registries
		OpenGLRenderer::ApplyGL1xStateBridge(OpenGLRenderer::gl_RenderState, nullptr);
	}

	// Always clear all buffers with scissor test disabled to prevent memory stalls
	glDisable(GL_SCISSOR_TEST);

	// Access the baseline clear color stored in the core frame buffer structure
	glClearColor(screen->mSceneClearColor[0], screen->mSceneClearColor[1], screen->mSceneClearColor[2], 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

	// Sync viewport boundaries directly from the modern screen layout cached values
	const auto& bounds = screen->mSceneViewport;
	glViewport(bounds.left, bounds.top, bounds.width, bounds.height);
	glScissor(bounds.left, bounds.top, bounds.width, bounds.height);

	glEnable(GL_SCISSOR_TEST);

	glEnable(GL_MULTISAMPLE);
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_STENCIL_TEST);
	glStencilFunc(GL_ALWAYS, 0, ~0); // default stencil configuration
	glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
}

void HWDrawInfo::SetViewAngleGL1x(DAngle viewangle)
{
	// Viewpoint is the active camera tracker object layout inside the HWDrawInfo class
	Viewpoint.Angles.Yaw = viewangle;

	// [Darkcrafter07] - FIXED LINKER ERROR! Rerouted from deleted global R_SetViewAngle
	// straight to the native object member function implemented inside 4.14dev!
	Viewpoint.SetViewAngle(r_viewwindow);
}

void HWDrawInfo::SetViewMatrixGL1x(float vx, float vy, float vz, bool mirror, bool planemirror)
{
	float mult = mirror ? -1.0f : 1.0f;
	float planemult = planemirror ? -(float)Level->info->pixelstretch : (float)Level->info->pixelstretch;

	// Reset our dedicated fixed-function ModelView matrix layout cleanly
	gl1xViewMatrix.loadIdentity();

	// Execute core rotations cascade using strict VSMatrix API signatures (angle, x, y, z)
	gl1xViewMatrix.rotate((float)Viewpoint.Angles.Roll.Degrees(), 0.0f, 0.0f, 1.0f);
	gl1xViewMatrix.rotate((float)Viewpoint.Angles.Pitch.Degrees(), 1.0f, 0.0f, 0.0f);
	gl1xViewMatrix.rotate((float)Viewpoint.Angles.Yaw.Degrees(), 0.0f, mult, 0.0f);

	// Apply world-space units translations and scale biases mapping
	gl1xViewMatrix.translate(vx * mult, -vz * planemult, -vy);
	gl1xViewMatrix.scale(-mult, planemult, 1.0f);
}

void HWDrawInfo::SetupViewGL1x(float vx, float vy, float vz, DAngle va, bool mirror, bool planemirror)
{
	// 1. Synchronize player camera viewing yaw angle registers
	SetViewAngleGL1x(va);

	// 2. Build the final hardware ModelView transformation matrix elements
	SetViewMatrixGL1x(vx, vy, vz, mirror, planemirror);

	// 3. [Darkcrafter07] - THE DIRECT HARDWARE MATRIX FLUSH!
	// Extracted strictly from the legacy FShaderManager::ApplyMatrices logic.
	// Since we are running under gl.legacyMode, we dump the calculated Lighthouse3D 
	// matrix streams straight into raw Windows XP hardware registers via glLoadMatrixf!
	if (gl.legacyMode)
	{
		// A. Flush Hardware Projection Matrix Channel
		glMatrixMode(GL_PROJECTION);
		glLoadMatrixf(gl1xProjectionMatrix.get());

		// B. Flush Hardware ModelView Matrix Channel
		glMatrixMode(GL_MODELVIEW);
		glLoadMatrixf(gl1xViewMatrix.get());
	}
}

void HWDrawInfo::CreateGL1xScene()
{
	for (int i = 0; i < 13; i++)
	{
		drawlists_legacy[i].Reset();
	}

	// 1. DYNAMIC FRUSTUM CULLING EXPANSION PASS
	angle_t a1 = FrustumAngleGL1x();

	Clipper legacyClipper;
	legacyClipper.Clear();

	legacyClipper.SetViewpoint(Viewpoint);
	legacyClipper.SafeAddClipRangeRealAngles(Viewpoint.Angles.Yaw.BAMs() + a1, Viewpoint.Angles.Yaw.BAMs() - a1);

	// [Darkcrafter07] - FIXED E0276! Re-routed from old GLPortal to the modern 
	// 4.14dev portalState manager tracker structure natively!
	portalState.StartFrame();

	// FIXED TOO FEW ARGS! Passed the active Level locals pointer smoothly down the pipe!
	PO_LinkToSubsectors(Level);

	ProcessAll.Clock();

	Bsp.Clock();

	// Safely map/lock polygonal arrays storage buffers onto active rendering context thread
	if (OpenGLRenderer::GLRenderer != nullptr && OpenGLRenderer::GLRenderer->mVBO != nullptr)
	{
		OpenGLRenderer::GLRenderer->mVBO->Map();
	}

	// RESTORED SetView LAYOUT CACHE SEGMENT!
	gl1xViewX = (float)Viewpoint.Pos.X;
	gl1xViewY = (float)Viewpoint.Pos.Y;

	validcount++; // Synchronize sidedefs processing generation token

	// ==============================================================================
	// [Darkcrafter07] - THE MONOLITHIC FIXED-FUNCTION BSP TRAVERSAL RUN!
	// Re-routed from modern multithread queues straight to our isolated single-thread buster!
	// This guarantees that all drawlists and drawitems arrays are 100% filled and valid!
	// ==============================================================================
	if (Level != nullptr && Level->HeadNode() != nullptr)
	{
		this->RenderPolyBSPNodeGL1x(Level->HeadNode());
	}

	// FIXED E0135! Re-routed tracking logic directly via the stable mCurrentPortal pointer
	if (mCurrentPortal != nullptr)
	{
		mCurrentPortal->RenderAttached(this);
	}

	Bsp.Unclock();

	// 2. FIXED-FUNCTION ANOMALIES AND TEXTURES HOLES CURED PASS
	// Directly calls local class members to patch raw sector stacks and water leaks
	this->HandleMissingTextures(static_cast<area_t>(in_area_legacy)); // Compensate missing wall sheets
	this->HandleHackedSubsectors(); // Execute deep water software hacks translation

	// [Darkcrafter07] - CUT REMOVED 4.14 METHOD TO FIX UNRESOLVED EXTERNAL!
	// Sector stacks merging is now fully automatic inside modern mesh builders.
	// this->ProcessSectorStacks(static_cast<area_t>(in_area_legacy));   

	// Safely release hardware vertex arrays data streams locks back to system
	if (OpenGLRenderer::GLRenderer != nullptr && OpenGLRenderer::GLRenderer->mVBO != nullptr)
	{
		OpenGLRenderer::GLRenderer->mVBO->Unmap();
	}

	ProcessAll.Unclock();
}

void HWDrawInfo::DrawSceneGL1x(int drawmode)
{
	static int recursion = 0;
	auto& vp = Viewpoint;

	if (vp.camera != nullptr)
	{
		ActorRenderFlags savedflags = vp.camera->renderflags;
		CreateGL1xScene(); // Call our optimized, non-crashing BSP compiler layout loop!
		vp.camera->renderflags = savedflags;
	}
	else
	{
		CreateGL1xScene();
	}

	// Protect portal manager states machines cleanly
	mCurrentPortal = nullptr;

	// Execute solid room geometry drawing (including our fixed multipass lights)
	RenderSceneGL1x(recursion);

	// Handle all portals and recursive mirrors chunks after solid geometry pass
	recursion++;
	portalState.EndFrame(this, OpenGLRenderer::gl_RenderState);
	recursion--;

	// Route down straight to translucent blending pipelines
	RenderTranslucentGL1x();
}

void HWDrawInfo::RenderTranslucentGL1x()
{
	RenderAll.Clock();

	// [Darkcrafter07] - FIXED UNDEFINED! Call our own class camera cacher to bind uniforms
	this->SetCameraPos(Viewpoint.Pos);

	// Final pass: translucent particles, windows, and masked midtextures
	OpenGLRenderer::gl_RenderState.AlphaGL1xFunc(GL_GEQUAL, gl_mask_sprite_threshold);
	OpenGLRenderer::gl_RenderState.BlendGL1xFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	OpenGLRenderer::gl_RenderState.EnableBrightmap(true);

	// Draw transparent boundaries utilizing stable native filled 4.14 arrays
	drawlists[GLDL_TRANSLUCENTBORDER].DrawGL1xWalls(nullptr, this, OpenGLRenderer::gl_RenderState, GLPASS_TRANSLUCENT);

	glDepthMask(false);
	drawlists[GLDL_TRANSLUCENT].DrawGL1xWalls(nullptr, this, OpenGLRenderer::gl_RenderState, GLPASS_TRANSLUCENT);

	OpenGLRenderer::gl_RenderState.EnableBrightmap(false);
	glDepthMask(true);

	OpenGLRenderer::gl_RenderState.AlphaGL1xFunc(GL_GEQUAL, 0.5f);

	RenderAll.Unclock();

	// [Darkcrafter07] - Forward the live gl_RenderState instance to safely back-link weapons math
	EndDrawSceneGL1x(Viewpoint.sector);
}

void HWDrawInfo::EndDrawSceneGL1x(sector_t* viewsector)
{
	OpenGLRenderer::gl_RenderState.EnableFog(false);

	// Render high-fidelity 3D weapon models if active via our class member
	const bool renderHUDModel = IsHUDModelForPlayerAvailable(Viewpoint.camera->player);
	if (renderHUDModel)
	{
		glClear(GL_DEPTH_BUFFER_BIT);

		// [Darkcrafter07] - FIXED E0461! Forwarded gl_RenderState directly to match non-const reference signature
		DrawPlayerSprites(true, OpenGLRenderer::gl_RenderState);
	}

	glDisable(GL_STENCIL_TEST);

	// ==============================================================================
	// [Darkcrafter07] - RESTORED OPENGL 1.1 Begin2D STATE MACHINE ENVIRONMENT
	// ==============================================================================
	// Directly configures hardware orthogonal view matrix stacks to secure 2D text clipping
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, screen->GetWidth(), screen->GetHeight(), 0, -1.0f, 1.0f);

	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);

	// Korshun: ENABLE AUTOMAP ANTIALIASING FOR FIXED PIPELINES!!!
	if (gl_aalines)
	{
		glEnable(GL_LINE_SMOOTH);
		glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
	}
	else
	{
		glDisable(GL_MULTISAMPLE);
		glDisable(GL_LINE_SMOOTH);
		glLineWidth(1.0f);
	}

	// Force clear hardware clipping matrices before pushing UI fonts elements
	glViewport(screen->mScreenViewport.left, screen->mScreenViewport.top, screen->mScreenViewport.width, screen->mScreenViewport.height);

	// Draw player weapon sprites and process screen color blends overlays
	DrawEndScene2DGL1x(viewsector);
}

void HWDrawInfo::FillScreenGL1x()
{
	OpenGLRenderer::gl_RenderState.AlphaGL1xFunc(GL_GEQUAL, 0.f);
	OpenGLRenderer::gl_RenderState.EnableTexture(false);

	// Flush registers cleanly before submitting vertex arrays down to video memory map
	OpenGLRenderer::ApplyGL1xStateBridge(OpenGLRenderer::gl_RenderState, nullptr);

	// Direct hardware draw arrays call!
	// Extracted natively from the main FFlatVertexBuffer using the preset index bounds.
	glDrawArrays(GL_TRIANGLE_STRIP, FFlatVertexBuffer::FULLSCREEN_INDEX, 4);
}

//==========================================================================
//
// Restored inventory and ZScript colormaps solver for legacy pipelines.
// Evaluates active player inventory chains via native ZScript VMCall macros.
// Declared as a native member of class HWDrawInfo (hw_drawinfo.cpp).
//
//==========================================================================
void HWDrawInfo::SetFixedColormapGL1x(player_t* player)
{
	// Force-reset our active fullbright flags cache back to default software settings
	FullbrightFlagsLegacy = CM_DEFAULT;

	if (player == nullptr || player->camera == nullptr) return;

	// Check special colormaps from the active camera actor instance safely
	player_t* cplayer = player->camera->player;
	if (cplayer != nullptr)
	{
		if (cplayer->extralight == INT_MIN)
		{
			FullbrightFlagsLegacy = CM_FIRSTSPECIALCOLORMAP + INVERSECOLORMAP;
			Viewpoint.extralight = 0; // Clear viewport overburn bounds
		}
		else if (cplayer->fixedcolormap != NOFIXEDCOLORMAP)
		{
			FullbrightFlagsLegacy = CM_FIRSTSPECIALCOLORMAP + cplayer->fixedcolormap;
		}
		else if (cplayer->fixedlightlevel != -1)
		{
			// Re-bind native PClass registry actors tokens natively matching 4.14dev signatures
			auto torchtype = PClass::FindActor(NAME_PowerTorch);
			auto litetype = PClass::FindActor(NAME_PowerLightAmp);

			if (cplayer->mo != nullptr)
			{
				for (AActor* in = cplayer->mo->Inventory; in; in = in->Inventory)
				{
					PalEntry color = 0;

					// Execute deep native ZScript VM stack lookup to parse active item blending
					IFVIRTUALPTRNAME(in, NAME_Inventory, GetBlend)
					{
						// [Darkcrafter07] - FIXED E0413! Restored the formal stack-allocated 1-element array 
						// constructor layout to guarantee that VMCall receives a valid VMValue* pointer address!
						VMValue params[1] = { in };
						VMReturn ret((int*)&color.d);

						// Now parameter parsing passes safely with zero type conversion conflicts
						VMCall(func, params, 1, &ret, 1);
					}

					// Need special handling for light amplifiers and radar torches 
					if (in->IsKindOf(torchtype))
					{
						FullbrightFlagsLegacy = cplayer->fixedlightlevel + CM_TORCH;
					}
					else if (in->IsKindOf(litetype))
					{
						FullbrightFlagsLegacy = CM_LITE;
					}
				}
			}
		}
	}

	// Commit the solved layout directly into our public fixed-function state cache
	OpenGLRenderer::gl_RenderState.SetFixedGL1xColormap(FullbrightFlagsLegacy);
}

void HWDrawInfo::DrawTargeterSpritesGL1x()
{
	if (Viewpoint.camera == nullptr || Viewpoint.camera->player == nullptr) return;

	player_t* player = Viewpoint.camera->player;

	// Security gate shields: don't draw if the player is fully invisible
	if (Viewpoint.camera->renderflags & RF_INVISIBLE || !r_drawplayersprites) return;

	OpenGLRenderer::gl_RenderState.EnableBrightmap(false);
	OpenGLRenderer::gl_RenderState.BlendGL1xFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	OpenGLRenderer::gl_RenderState.AlphaGL1xFunc(GL_GREATER, gl_mask_sprite_threshold);
	OpenGLRenderer::gl_RenderState.BlendGL1xEquation(GL_FUNC_ADD);
	OpenGLRenderer::gl_RenderState.ResetColor();
	OpenGLRenderer::gl_RenderState.SetTextureMode(TM_MODULATE);

	// [Darkcrafter07] - FIXED E0167! Instead of scanning the old player->psprites ZScript memory chain,
	// we iterate straight through the native, fully-interpolated 'hudsprites' array of class HWDrawInfo!
	for (auto& hudsprite : hudsprites)
	{
		// Filter targeter layers layout (PSP_TARGETCENTER layers are strictly >= INT_MAX - 2 / 1000+)
		// We safely read the ID field mapped natively from the original overlay sprite definitions
		if (hudsprite.lightindex >= 1000)
		{
			// [Darkcrafter07] - Direct zero-overhead draw call utilizing 100% compatible parameters!
			// Passes the address of the active HUDSprite item cleanly down the pipeline.
			DrawPSprite(&hudsprite, OpenGLRenderer::gl_RenderState);
		}
	}
}

void HWDrawInfo::DrawEndScene2DGL1x(sector_t* viewsector)
{
	player_t* player = Viewpoint.camera ? Viewpoint.camera->player : nullptr;

	const bool renderHUDModel = IsHUDModelForPlayerAvailable(Viewpoint.camera->player);

	if (renderHUDModel == false)
	{
		// Draw classic 2D weapon patch sprites safely using stable 4.14 slots
		DrawPlayerSprites(false, OpenGLRenderer::gl_RenderState);
	}

	if (gl.legacyMode)
	{
		// Force inject active inventory colormaps (night vision, radsuit, inverse masks)
		if (player != nullptr)
		{
			this->SetFixedColormapGL1x(player);
		}
	}

	// Draw targeter brackets automap overlays via the native 4.14 array sheets
	DrawTargeterSpritesGL1x();

	// Injects your master-class dynamic screen color tints overlay 
	// (pain flashes, radsuits, blood, underwater fog) re-mapped smoothly into the 4.14 context!
	DrawGL1xColormapOverlay(viewsector);

	// Restore standard factory rendering state registers for subsequent frames
	OpenGLRenderer::gl_RenderState.BlendGL1xFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	OpenGLRenderer::gl_RenderState.ResetColor();
	OpenGLRenderer::gl_RenderState.EnableTexture(true);

	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_LINE_SMOOTH);

	// ==============================================================================
	// [Darkcrafter07] - THE MONOLITHIC WinXP HARDWARE SCREEN OVERRIDE FLIP!
	// ==============================================================================
	if (gl.legacyMode)
	{
		// 1. Force the GPU graphics pipeline execution queue to finish drawing instantly!
		// This freezes the complete 3D scene geometry bytes inside raw VRAM pools.
		glFlush();
		glFinish();

		// 2. Direct core layout bridge: execute immediate hardware page flip!
		// Re-routed straight from old functions into the live 4.14dev screen context object.
		// This bypasses screen->PostProcessScene and instantly lights up the monitor registers!
		if (screen != nullptr)
		{
			// Fires the underlying Win32GLFrameBuffer::SwapBuffers engine routine natively!
			screen->Update();
		}
	}
}

struct GL1x_IRECT // from old GZ3x gl_renderer.h
{
	int left, top;
	int width, height;

	void Offset(int xofs, int yofs)
	{
		left += xofs;
		top += yofs;
	}
};

void HWDrawInfo::RenderTextureViewGL1x(FCanvasTexture* tex, AActor* viewpoint, double fov)
{
	// Context guard shield protects viewport thread from parsing empty pointers
	if (tex == nullptr || viewpoint == nullptr) return;

	// [Darkcrafter07] - FIXED E0135! Build a valid FTextureID container on the spot 
	// utilizing the native GetSourceLump() integer slot from the FTexture base class layout!
	FTextureID nativeTexId = FSetTextureID(tex->GetSourceLump());

	// Safely retrieve our active FGameTexture container from the global manager
	FGameTexture* gltex = TexMan.GetGameTexture(nativeTexId);
	if (gltex == nullptr || gltex->GetTexture() == nullptr) return;

	// [Darkcrafter07] - FIXED MISSING MEMBERS! 
	// Directly extract dimensions straight through the legal GetWidth() / GetHeight() methods!
	int width = gltex->GetTexture()->GetWidth();
	int height = gltex->GetTexture()->GetHeight();

	// In legacy fixed mode under Windows XP, fail if the requested texture exceeds canvas bounds
	if (screen != nullptr)
	{
		if (gltex->GetTexture()->GetWidth() > screen->GetWidth() ||
			gltex->GetTexture()->GetHeight() > screen->GetHeight()) return;
	}

	// Zero-overhead raw hardware matrix flush before starting the offscreen traversal
	glFlush();

	// Configure destination drawing bounding rectangle bounds
	GL1x_IRECT bounds;
	bounds.left = 0;
	bounds.top = 0;

	// Explicitly specified full OpenGLRenderer namespace paths to expose GetTexDimension static helper
	bounds.width = OpenGLRenderer::FHardwareTexture::GetTexDimension(gltex->GetTexture()->GetWidth());
	bounds.height = OpenGLRenderer::FHardwareTexture::GetTexDimension(gltex->GetTexture()->GetHeight());

	// Backup parent state parameter trackers before overriding scene boundaries
	int backupInArea = in_area_legacy;
	int backupColormap = FullbrightFlagsLegacy;

	// Enforce standard software lighting default settings inside the canvas texture
	FullbrightFlagsLegacy = CM_DEFAULT;

	// Setup camera transform matrices properties straight inside the Viewpoint cacher
	Viewpoint.camera = viewpoint;
	Viewpoint.FieldOfView = DAngle::fromDeg(fov);

	// Calculate dynamic aspect ratio multipliers natively
	float aspect = (height > 0) ? ((float)width / (float)height) : 1.0f;

	// ==============================================================================
	// [Darkcrafter07] - THE OFFSCREEN FIXED SCENE DISPATCH!
	// We force execute our independent, non-crashing DrawSceneGL1x pipeline loop,
	// passing 2 (DM_OFFSCREEN) to freeze drawing strictly inside the viewport buffer masks!
	// ==============================================================================
	DrawSceneGL1x(2);

	// Restore original frame variables right after offscreen extraction pass
	in_area_legacy = backupInArea;
	FullbrightFlagsLegacy = backupColormap;

	glFlush(); // GL1.1 texture memory copy pass

	// Forward the layout directly into the native 4.14dev SetMaterial overload
	OpenGLRenderer::gl_RenderState.SetMaterial(gltex, UF_Texture, 0, CLAMP_NONE, 0, -1);
	OpenGLRenderer::ApplyGL1xStateBridge(OpenGLRenderer::gl_RenderState, nullptr);

	glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, bounds.width, bounds.height);

	tex->SetUpdated(false);

	// Increment our globally declared frames textures counter cleanly
	camtexcount++;
}

//==========================================================================
//
// [Darkcrafter07] - HWDrawInfo::RenderSceneGL1x
// Reconstructed core multi-pass renderer engine for single-view 3D layouts.
// Fully bypasses the broken old GLPASS_LIGHTSONLY deferred buffer cascades.
// Routes all rendering draws strictly via stable, filled 4.14dev memory arrays.
// Declared as a native member of class HWDrawInfo (hw_drawinfo.cpp).
//
//==========================================================================
void HWDrawInfo::RenderSceneGL1x(int recursion)
{
	RenderAll.Clock();

	// Reference our global public fixed-function state cache object on the spot
	FRenderState& state = OpenGLRenderer::gl_RenderState;

	state.SetDepthMask(true);

	// Force-enable baseline fog environments overlays
	state.EnableFog(true);
	state.SetRenderStyle(STYLE_Source);

	if (gl_sort_textures)
	{
		drawlists[GLDL_PLAINWALLS].SortWalls();
		drawlists[GLDL_PLAINFLATS].SortFlats();
		drawlists[GLDL_MASKEDWALLS].SortWalls();
		drawlists[GLDL_MASKEDFLATS].SortFlats();
		drawlists[GLDL_MASKEDWALLSOFS].SortWalls();
	}

	// [Darkcrafter07] - DETECT DYNAMIC TEXTURED MULTI-PASS PROCESSING MATRIX
	// Safely scales via our new unified FullbrightFlagsLegacy instance slot cache
	bool haslights = (FullbrightFlagsLegacy == CM_DEFAULT) && gl_lights;
	bool hasbrightmaps = state.IsBrightmapEnabled();

	//if (gl.legacyMode && (haslights || hasbrightmaps))
	//{
	//	RenderMultipassStuff();
	//}

	// Part 1: Solid room geometry baseline hardware environment setup pass
	state.SetDepthFunc(DF_Less);
	state.AlphaFunc(Alpha_GEqual, 0.0f);
	state.ClearDepthBias();

	state.EnableTexture(gl_texture);
	state.EnableBrightmap(true);

	// Draw opaque walls and floors elements straight to frame buffer using stable 4.14 arrays
	drawlists[GLDL_PLAINWALLS].DrawWalls(this, state, false);
	drawlists[GLDL_PLAINFLATS].DrawFlats(this, state, false);

	// Part 2: masked geometry fences and transparent midtextures alpha-clipping pass
	state.AlphaFunc(Alpha_GEqual, gl_mask_threshold);
	drawlists[GLDL_MASKEDWALLS].DrawWalls(this, state, false);
	drawlists[GLDL_MASKEDFLATS].DrawFlats(this, state, false);

	// Part 3: masked geometry with anti-z-fighting polygon offset
	// This list is empty most of the time so only waste time on it when in use.
	if (drawlists[GLDL_MASKEDWALLSOFS].Size() > 0)
	{
		// Native 4.14dev compliant depth bias push instead of raw glPolygonOffset leakage
		state.SetDepthBias(-1.0f, -128.0f);
		drawlists[GLDL_MASKEDWALLSOFS].DrawWalls(this, state, false);
		state.ClearDepthBias();
	}

	// Draw skeletal 3D models and MD3/MD5 actor meshes natively matching new buffers
	screen->mBones->Map();
	drawlists[GLDL_MODELS].Draw(this, state, false);
	screen->mBones->Unmap();

	state.SetRenderStyle(STYLE_Translucent);

	// Part 4: Draw decals (not a real pass, straight hardware depth comparison)
	state.SetDepthFunc(DF_LEqual);

	// [Darkcrafter07] - FIXED ARGS! Forwarded the native Decals vector array straight from 4.14dev layout
	DrawDecals(state, Decals[0]);

	// --- Legacy GL1x/GL2x brightmaps block start ---
	//if (gl.legacyMode && state.IsBrightmapEnabled())
	//{
	//	// Instantiate a localized stack dispatcher container to protect secondary passes execution
	//	HWWallDispatcher legacyDispatcher(this);
	//
	//	// Prepare global state parameters
	//	state.EnableFog(false);
	//	state.SetTextureMode(TM_BRIGHTMAP_LEGACY);
	//	state.SetDynLight(0, 0, 0);
	//	state.SetAddColor(0);
	//	state.SetObjectColor(0xffffffff);
	//	state.BlendGL1xFunc(GL_ONE, GL_ONE); // Pure hardware additive blending
	//
	//	// Set Brightmap Intensity (Effect Amount)
	//	float intensity = 1.0f;
	//	state.SetColor(intensity, intensity, intensity, 1.0f);
	//
	//	// Precision & Depth Control
	//	if (gl_seamless) // aka "gl_render_precise" quality dynamic light rendering mode
	//	{
	//		// in this mode it zfights so we pull the brightmaps up a little manually
	//		glEnable(GL_POLYGON_OFFSET_FILL);
	//		glPolygonOffset(-0.125f, -0.125f);
	//		glDepthFunc(GL_LEQUAL); // also different depth otherwise walls get darker
	//	}
	//	else
	//	{
	//		glDepthFunc(GL_EQUAL);  // for speed mode we use regular depth sorting
	//	}
	//
	//	glDepthMask(false);
	//
	//	// 4. Draw Geometry Lists utilizing the 100% stable 4.14dev memory tracks
	//	drawlists[GLDL_PLAINWALLS].DrawGL1xWalls(&legacyDispatcher, this, state, GLPASS_LIGHTEFFECTS_LEGACY);
	//	drawlists[GLDL_PLAINFLATS].DrawGL1xFlats(this, state, GLPASS_LIGHTEFFECTS_LEGACY);
	//	drawlists[GLDL_MASKEDWALLS].DrawGL1xWalls(&legacyDispatcher, this, state, GLPASS_LIGHTEFFECTS_LEGACY);
	//	drawlists[GLDL_MASKEDFLATS].DrawGL1xFlats(this, state, GLPASS_LIGHTEFFECTS_LEGACY);
	//
	//	// Draw Dynamic/Multipass Geometry Lists
	//	drawlists[GLDL_PLAINWALLS].DrawGL1xWalls(&legacyDispatcher, this, state, GLPASS_LIGHTEFFECTS_LEGACY);
	//	drawlists[GLDL_MASKEDWALLS].DrawGL1xWalls(&legacyDispatcher, this, state, GLPASS_LIGHTEFFECTS_LEGACY);
	//	drawlists[GLDL_PLAINFLATS].DrawGL1xFlats(this, state, GLPASS_LIGHTEFFECTS_LEGACY);
	//	drawlists[GLDL_MASKEDFLATS].DrawGL1xFlats(this, state, GLPASS_LIGHTEFFECTS_LEGACY);
	//
	//	drawlists[GLDL_FOGWALLS].DrawGL1xWalls(&legacyDispatcher, this, state, GLPASS_LIGHTEFFECTS_LEGACY);
	//	drawlists[GLDL_FOGMASKEDWALLS].DrawGL1xWalls(&legacyDispatcher, this, state, GLPASS_LIGHTEFFECTS_LEGACY);
	//	drawlists[GLDL_FOGFLATS].DrawGL1xFlats(this, state, GLPASS_LIGHTEFFECTS_LEGACY);
	//	drawlists[GLDL_FOGMASKEDFLATS].DrawGL1xFlats(this, state, GLPASS_LIGHTEFFECTS_LEGACY);
	//
	//	// Restore hardware register states cleanly
	//	if (gl_seamless)
	//	{
	//		glDisable(GL_POLYGON_OFFSET_FILL);
	//	}
	//	glDepthFunc(GL_LESS);
	//	glDepthMask(true);
	//	state.EnableFog(true);
	//	state.BlendGL1xFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	//	state.SetTextureMode(TM_MODULATE);
	//}
	// --- Legacy GL1x/GL2x brightmaps block finish ---

	RenderAll.Unclock();
}

//==========================================================================
//
// Monolithic encapsulated frame submission shield.
// Safely executes low-level OpenGL flushes and triggers the screen page flip
// inside win32glvideo.cpp without causing DrawText macro corruption loops in d_main!
// Declared as a native member of class HWDrawInfo (hw_drawinfo.cpp).
//
//==========================================================================
#include "i_mainwindow.h"
#include "common/rendering/gl/gl_framebuffer.h"

extern void* g_Legacy_hDC_Bridge;
extern void* g_Legacy_hRC_Bridge;
extern void* GetSDLWindowHandleLegacy();
extern "C"
{
	HGLRC zd_wglCreateContext(HDC Arg1);
	BOOL zd_wglDeleteContext(HGLRC Arg1);
	BOOL zd_wglMakeCurrent(HDC Arg1, HGLRC Arg2);
	PROC zd_wglGetProcAddress(LPCSTR name);
}

//==========================================================================
//
// [Darkcrafter07] - HWDrawInfo::SubmitGL1xFrame
// Monolithic encapsulated frame submission shield for Windows XP.
// Synchronizes internal gl_load.c states via native zd_ core wrappers
// right before flashing a direct Fixed-Function test quad to the monitor DC!
// Declared as a native member of class HWDrawInfo (hw_drawinfo.cpp).
//
//==========================================================================
void HWDrawInfo::SubmitGL1xFrame()
{
	if (!gl.legacyMode) return;

	// 1. Force the hardware graphics queue to completely finish drawing fields!
	glFlush();
	glFinish();

	HWND hWndTarget = mainwindow.GetHandle();
	if (hWndTarget != NULL)
	{
		HDC hDCTarget = GetDC(hWndTarget);
		if (hDCTarget != NULL)
		{
			// ==============================================================================
			// STAGE 1: DYNAMIC Win32 GDI CONTEXT FORCE-CAPTURE
			// ==============================================================================
			// Instead of trusting static startup variables, we extract the TRUE active
			// rendering context handle straight from the live GPU thread state via opengl32!
			typedef HGLRC(WINAPI* PFNWGLGETCURRENTCONTEXTPROC) (VOID);
			HMODULE hOGL = GetModuleHandleA("opengl32.dll");

			if (hOGL != NULL)
			{
				PFNWGLGETCURRENTCONTEXTPROC p_wglGetCurrentContext = (PFNWGLGETCURRENTCONTEXTPROC)GetProcAddress(hOGL, "wglGetCurrentContext");
				if (p_wglGetCurrentContext != nullptr)
				{
					HGLRC hActiveRCLive = p_wglGetCurrentContext();

					if (hActiveRCLive != NULL)
					{
						// [Darkcrafter07] - Force-lock the live thread context right into the window DC!
						// This perfectly syncs gl_load.c even if Windows re-created the window behind our backs!
						zd_wglMakeCurrent(hDCTarget, hActiveRCLive);
					}
				}
			}

			// ==============================================================================
			// STAGE 2: FIXED-FUNCTION HARDWARE SANITIZE PURGE
			// ==============================================================================
			glUseProgram(0);

			// Hard-reset GPU framebuffers back to the true window screen backbuffer context
			typedef void (APIENTRY* PFNGLBINDFRAMEBUFFERPROC) (unsigned int target, unsigned int framebuffer);
			static PFNGLBINDFRAMEBUFFERPROC p_glBindFramebuffer = nullptr;
			if (p_glBindFramebuffer == nullptr)
			{
				p_glBindFramebuffer = (PFNGLBINDFRAMEBUFFERPROC)zd_wglGetProcAddress("glBindFramebuffer");
			}
			if (p_glBindFramebuffer != nullptr)
			{
				p_glBindFramebuffer(0x8D40, 0); // 0x8D40 matches GL_FRAMEBUFFER
			}

			// Reset Vertex Array Objects (VAO) to unlock unindexed geometry pipes
			typedef void (APIENTRY* PFNGLBINDVERTEXARRAYPROC) (unsigned int array);
			static PFNGLBINDVERTEXARRAYPROC p_glBindVertexArray = nullptr;
			if (p_glBindVertexArray == nullptr)
			{
				p_glBindVertexArray = (PFNGLBINDVERTEXARRAYPROC)zd_wglGetProcAddress("glBindVertexArray");
			}
			if (p_glBindVertexArray != nullptr)
			{
				p_glBindVertexArray(0);
			}

			// Lock texture unit 0 and drop any multi-texturing leftovers from postprocess shaders
			glActiveTexture(GL_TEXTURE0);
			glDisable(GL_TEXTURE_2D);

			// Safely block active modern attribute array slots from generating driver stalls
			for (int attr_idx = 0; attr_idx < 16; attr_idx++)
			{
				glDisableVertexAttribArray(attr_idx);
			}

			// Extract resolution variables into clean independent primitives to clear compiler errors
			float target_view_width = 640.0f;
			float target_view_height = 480.0f;

			if (screen != nullptr)
			{
				target_view_width = (float)screen->GetWidth();
				target_view_height = (float)screen->GetHeight();
			}

			// 2. Set clean 2D orthogonal matrix projection view streams
			glMatrixMode(GL_PROJECTION);
			glLoadIdentity();
			glOrtho(0.0, (double)target_view_width, (double)target_view_height, 0.0, -1.0, 1.0);

			glMatrixMode(GL_MODELVIEW);
			glLoadIdentity();

			glDisable(GL_DEPTH_TEST);
			glDisable(GL_LIGHTING);
			glDisable(GL_BLEND);
			glDisable(GL_ALPHA_TEST);
			glDisable(GL_SCISSOR_TEST);
			glDisable(GL_CULL_FACE);
			glDepthMask(GL_FALSE);

			// ==============================================================================
			// STAGE 3: STREAM FIXED-FUNCTION QUAD (DARK-RED TEST)
			// ==============================================================================
			glBegin(GL_QUADS);
			glColor4f(0.5f, 0.0f, 0.0f, 1.0f); // Solid dark-red matrix base
			glVertex2f(0.0f, 0.0f);
			glVertex2f(target_view_width, 0.0f);
			glVertex2f(target_view_width, target_view_height);
			glVertex2f(0.0f, target_view_height);
			glEnd();

			glFlush();
			glFinish();

			// ==============================================================================
			// STAGE 4: NATIVE WIN32 GDI SWAPBUFFERS FLIP
			// ==============================================================================
			::SwapBuffers(hDCTarget);

			// Release device context handle cleanly back to OS
			ReleaseDC(hWndTarget, hDCTarget);
		}
	}

	glFinish();
}

// ===============GL1x/GL2x functionality - FINISH =====================