// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2000-2016 Christoph Oelckers
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
** gl_flat.cpp
** Flat processing
**
*/

#include "a_sharedglobal.h"
#include "a_dynlight.h"
#include "r_defs.h"
#include "r_sky.h"
#include "r_utility.h"
#include "doomstat.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "actorinlines.h"
#include "p_lnspec.h"
#include "matrix.h"
#include "hw_dynlightdata.h"
#include "hw_cvars.h"
#include "hw_clock.h"
#include "hw_lighting.h"
#include "hw_material.h"
#include "hwrenderer/scene/hw_drawinfo.h"
#include "flatvertices.h"
#include "hw_lightbuffer.h"
#include "hw_drawstructs.h"
#include "hw_renderstate.h"
#include "texturemanager.h"
#include "hw_viewpointbuffer.h"

#include "common/rendering/gl_load/gl_system.h"
#include "common/rendering/gl_load/gl_interface.h"
#include "common/rendering/gl/gl_20.h"

#ifdef _DEBUG
CVAR(Int, gl_breaksec, -1, 0)
#endif

HWFlat* g_isCurrentlyHWFlatDrawing = nullptr; // mainly for GL1x/GL2x modes (externed in gl_20.cpp)

// Scratch storage for the Gouraud pass, reused every frame.
// Size is grown on demand to fit the largest subsector.
static TArray<FFlatVertexGouraud> gGouraudScratch;

//==========================================================================
//
// Sets the texture matrix according to the plane's texture positioning
// information
//
//==========================================================================

bool hw_SetPlaneTextureRotation(const HWSectorPlane * secplane, FGameTexture * gltexture, VSMatrix &dest)
{
	// only manipulate the texture matrix if needed.
	if (!secplane->Offs.isZero() ||
		secplane->Scale.X != 1. || secplane->Scale.Y != 1 ||
		secplane->Angle != 0 ||
		gltexture->GetDisplayWidth() != 64 ||
		gltexture->GetDisplayHeight() != 64)
	{
		float uoffs = secplane->Offs.X / gltexture->GetDisplayWidth();
		float voffs = secplane->Offs.Y / gltexture->GetDisplayHeight();

		float xscale1 = secplane->Scale.X;
		float yscale1 = secplane->Scale.Y;
		if (gltexture->isHardwareCanvas())
		{
			yscale1 = 0 - yscale1;
		}
		float angle = -secplane->Angle;

		float xscale2 = 64.f / gltexture->GetDisplayWidth();
		float yscale2 = 64.f / gltexture->GetDisplayHeight();

		dest.loadIdentity();
		dest.scale(xscale1, yscale1, 1.0f);
		dest.translate(uoffs, voffs, 0.0f);
		dest.scale(xscale2, yscale2, 1.0f);
		dest.rotate(angle, 0.0f, 0.0f, 1.0f);
		return true;
	}
	return false;
}

void SetPlaneTextureRotation(FRenderState &state, HWSectorPlane* plane, FGameTexture* texture)
{
	if (hw_SetPlaneTextureRotation(plane, texture, state.mTextureMatrix))
	{
		state.EnableTextureMatrix(true);
	}
}



//==========================================================================
//
// special handling for skyboxes which need texture clamping.
// This will find the bounding rectangle of the sector and just
// draw one single polygon filling that rectangle with a clamped
// texture.
//
//==========================================================================

void HWFlat::CreateSkyboxVertices(FFlatVertex *vert)
{
	float minx = FLT_MAX, miny = FLT_MAX;
	float maxx = -FLT_MAX, maxy = -FLT_MAX;

	for (auto ln : sector->Lines)
	{
		float x = ln->v1->fX();
		float y = ln->v1->fY();
		if (x < minx) minx = x;
		if (y < miny) miny = y;
		if (x > maxx) maxx = x;
		if (y > maxy) maxy = y;
		x = ln->v2->fX();
		y = ln->v2->fY();
		if (x < minx) minx = x;
		if (y < miny) miny = y;
		if (x > maxx) maxx = x;
		if (y > maxy) maxy = y;
	}

	static float uvals[] = { 0, 0, 1, 1 };
	static float vvals[] = { 1, 0, 0, 1 };
	int rot = -xs_FloorToInt(plane.Angle / 90.f);

	vert[0].Set(minx, z, miny, uvals[rot & 3], vvals[rot & 3]);
	vert[1].Set(minx, z, maxy, uvals[(rot + 1) & 3], vvals[(rot + 1) & 3]);
	vert[2].Set(maxx, z, miny, uvals[(rot + 3) & 3], vvals[(rot + 3) & 3]);
	vert[3].Set(maxx, z, maxy, uvals[(rot + 2) & 3], vvals[(rot + 2) & 3]);
}

//==========================================================================
//
//
//
//==========================================================================

void HWFlat::SetupLights(HWDrawInfo *di, FLightNode * node, FDynLightData &lightdata, int portalgroup)
{
	Plane p;

	lightdata.Clear();
	if (renderstyle == STYLE_Add && !di->Level->lightadditivesurfaces)
	{
		dynlightindex = -1;
		return;	// no lights on additively blended surfaces.
	}
	while (node)
	{
		FDynamicLight * light = node->lightsource;

		if (!light->IsActive() || light->DontLightMap())
		{
			node = node->nextLight;
			continue;
		}
		iter_dlightf++;

		// we must do the side check here because gl_GetLight needs the correct plane orientation
		// which we don't have for Legacy-style 3D-floors
		double planeh = plane.plane.ZatPoint(light->Pos);
		if ((planeh<light->Z() && ceiling) || (planeh>light->Z() && !ceiling))
		{
			node = node->nextLight;
			continue;
		}

		p.Set(plane.plane.Normal(), plane.plane.fD());
		draw_dlightf += GetLight(lightdata, portalgroup, p, light, false);
		node = node->nextLight;
	}

	dynlightindex = screen->mLights->UploadLights(lightdata);
}

//==========================================================================
//
//
//
//==========================================================================

void HWFlat::DrawSubsectors(HWDrawInfo *di, FRenderState &state)
{
	if (di->Level->HasDynamicLights && screen->BuffersArePersistent() && !di->isFullbrightScene())
	{
		SetupLights(di, section->lighthead, lightdata, sector->PortalGroup);
	}
	state.SetLightIndex(dynlightindex);


	state.DrawIndexed(DT_Triangles, iboindex + section->vertexindex, section->vertexcount);
	flatvertices += section->vertexcount;
	flatprimitives++;
}

//==========================================================================
//
// Restored fixed-function skybox polygon layout rendering pass with 
// custom texture clamping. Calculates the bounding rectangle of
// the sector and draws a single optimized flat quad.
//
//==========================================================================
void HWFlat::DrawGL1xSkyboxSector(HWDrawInfo* di, FRenderState& state, int pass, bool processlights)
{
	float minx = FLT_MAX, miny = FLT_MAX;
	float maxx = -FLT_MAX, maxy = -FLT_MAX;

	// Loop through all boundary lines inside the current target sector natively
	for (auto ln : sector->Lines)
	{
		float x = (float)ln->v1->fX();
		float y = (float)ln->v1->fY();
		if (x < minx) minx = x;
		if (y < miny) miny = y;
		if (x > maxx) maxx = x;
		if (y > maxy) maxy = y;
		x = (float)ln->v2->fX();
		y = (float)ln->v2->fY();
		if (x < minx) minx = x;
		if (y < miny) miny = y;
		if (x > maxx) maxx = x;
		if (y > maxy) maxy = y;
	}

	// Dynamic plane anchor calculations, passing zero coordinates to evaluate baseline offsets
	float z_pos = (float)plane.plane.ZatPoint(0.0, 0.0);
	static float uvals[] = { 0, 0, 1, 1 };
	static float vvals[] = { 1, 0, 0, 1 };
	int rot = -xs_FloorToInt((float)plane.Angle / 90.f);

	// Flush state machine variables directly down into target hardware registers
	state.ApplyFixedGL1xFunction(nullptr);

	// Hardware Immediate Mode draw call!
	// Replaces old FQuadDrawer allocation and pipeline matrix shaders jumps.
	glBegin(GL_QUADS);
	// Vertex 0: Bottom-Left Boundary
	glTexCoord2f(uvals[rot & 3], vvals[rot & 3]);
	glVertex3f(minx, z_pos, miny);

	// Vertex 1: Top-Left Upper Edge
	glTexCoord2f(uvals[(rot + 1) & 3], vvals[(rot + 1) & 3]);
	glVertex3f(minx, z_pos, maxy);

	// Vertex 2: Top-Right Upper Edge
	glTexCoord2f(uvals[(rot + 2) & 3], vvals[(rot + 2) & 3]);
	glVertex3f(maxx, z_pos, maxy);

	// Vertex 3: Bottom-Right Boundary
	glTexCoord2f(uvals[(rot + 3) & 3], vvals[(rot + 3) & 3]);
	glVertex3f(maxx, z_pos, miny);
	glEnd();

	// Synchronize vertex global counters to secure frame buffer layout allocations
	flatvertices += 4;
	flatprimitives++;
}


//==========================================================================
//
// Drawer for render hacks
//
//==========================================================================

void HWFlat::DrawOtherPlanes(HWDrawInfo *di, FRenderState &state)
{
    state.SetMaterial(texture, UF_Texture, 0, CLAMP_NONE, NO_TRANSLATION, -1);
    
    // Draw the subsectors assigned to it due to missing textures
    auto pNode = (renderflags&SSRF_RENDERFLOOR) ?
        di->otherFloorPlanes.CheckKey(sector->sectornum) : di->otherCeilingPlanes.CheckKey(sector->sectornum);
    
    if (!pNode) return;
    auto node = *pNode;
    
    while (node)
    {
        state.SetLightIndex(node->lightindex);
        auto num = node->sub->numlines;
        flatvertices += num;
        flatprimitives++;
        state.Draw(DT_TriangleFan,node->vertexindex, num);
        node = node->next;
    }
}

//==========================================================================
//
// Drawer for render hacks
//
//==========================================================================

void HWFlat::DrawFloodPlanes(HWDrawInfo *di, FRenderState &state)
{
	// Flood gaps with the back side's ceiling/floor texture
	// This requires a stencil because the projected plane interferes with
	// the depth buffer

	state.SetMaterial(texture, UF_Texture, 0, CLAMP_NONE, NO_TRANSLATION, -1);

	// Draw the subsectors assigned to it due to missing textures
	auto pNode = (renderflags&SSRF_RENDERFLOOR) ?
		di->floodFloorSegs.CheckKey(sector->sectornum) : di->floodCeilingSegs.CheckKey(sector->sectornum);
	if (!pNode) return;

	auto fnode = *pNode;

	state.SetLightIndex(-1);
	while (fnode)
	{
		flatvertices += 12;
		flatprimitives += 3;

		// Push bleeding floor/ceiling textures back a little in the z-buffer
		// so they don't interfere with overlapping mid textures.
		state.SetDepthBias(1, 128);

		// Create stencil
		state.SetEffect(EFF_STENCIL);
		state.EnableTexture(false);
		state.SetStencil(0, SOP_Increment, SF_ColorMaskOff);
		state.Draw(DT_TriangleStrip, fnode->vertexindex, 4);

		// Draw projected plane into stencil
		state.EnableTexture(true);
		state.SetEffect(EFF_NONE);
		state.SetStencil(1, SOP_Keep, SF_DepthMaskOff);
		state.EnableDepthTest(false);
		state.Draw(DT_TriangleStrip, fnode->vertexindex + 4, 4);

		// clear stencil
		state.SetEffect(EFF_STENCIL);
		state.EnableTexture(false);
		state.SetStencil(1, SOP_Decrement, SF_ColorMaskOff | SF_DepthMaskOff);
		state.Draw(DT_TriangleStrip, fnode->vertexindex, 4);

		// restore old stencil op.
		state.EnableTexture(true);
		state.EnableDepthTest(true);
		state.SetEffect(EFF_NONE);
		state.SetDepthBias(0, 0);
		state.SetStencil(0, SOP_Keep, SF_AllOn);

		fnode = fnode->next;
	}

}


//==========================================================================
//
//
//
//==========================================================================
void HWFlat::DrawFlat(HWDrawInfo *di, FRenderState &state, bool translucent)
{
	g_isCurrentlyHWFlatDrawing = this; // we're drawing flats now

#ifdef _DEBUG
	if (sector->sectornum == gl_breaksec)
	{
		int a = 0;
	}
#endif

	int rel = getExtraLight();

	state.SetNormal(plane.plane.Normal().X, plane.plane.Normal().Z, plane.plane.Normal().Y);

	SetColor(state, di->Level, di->lightmode, lightlevel, rel, di->isFullbrightScene(), Colormap, alpha);
	SetFog(state, di->Level, di->lightmode, lightlevel, rel, di->isFullbrightScene(), &Colormap, false);
	state.SetObjectColor(FlatColor | 0xff000000);
	state.SetAddColor(AddColor | 0xff000000);
	state.ApplyTextureManipulation(TextureFx);
	if (plane.plane.dithertransflag) state.SetEffect(EFF_DITHERTRANS);

	if (hacktype & SSRF_PLANEHACK)
	{
		DrawOtherPlanes(di, state);
	}
	else if (hacktype & SSRF_FLOODHACK)
	{
		DrawFloodPlanes(di, state);
	}
	else if (!translucent)
	{
		if (sector->special != GLSector_Skybox)
		{
			state.SetMaterial(texture, UF_Texture, 0, CLAMP_NONE, NO_TRANSLATION, -1);
			SetPlaneTextureRotation(state, &plane, texture);
			DrawSubsectors(di, state);
			state.EnableTextureMatrix(false);
		}
		else if (!hacktype)
		{
			state.SetMaterial(texture, UF_Texture, 0, CLAMP_XY, NO_TRANSLATION, -1);
			state.SetLightIndex(dynlightindex);
			state.Draw(DT_TriangleStrip,iboindex, 4);
			flatvertices += 4;
			flatprimitives++;
		}
	}
	else
	{
		state.SetRenderStyle(renderstyle);
		if (!texture || !texture->isValid())
		{
			state.AlphaFunc(Alpha_GEqual, 0.f);
			state.EnableTexture(false);
			DrawSubsectors(di, state);
			state.EnableTexture(true);
		}
		else
		{
			if (!texture->GetTranslucency()) state.AlphaFunc(Alpha_GEqual, gl_mask_threshold);
			else state.AlphaFunc(Alpha_GEqual, 0.f);
			state.SetMaterial(texture, UF_Texture, 0, CLAMP_NONE, NO_TRANSLATION, -1);
			SetPlaneTextureRotation(state, &plane, texture);
			DrawSubsectors(di, state);
			state.EnableTextureMatrix(false);
		}
		state.SetRenderStyle(DefaultRenderStyle());
	}
	state.SetObjectColor(0xffffffff);
	state.SetAddColor(0);
	state.ApplyTextureManipulation(nullptr);
	if (plane.plane.dithertransflag) state.SetEffect(EFF_NONE);

	g_isCurrentlyHWFlatDrawing = nullptr; // we're not drawing flats anymore
}

//==========================================================================
//
//
//
//==========================================================================
void HWFlat::DrawGL1xFlatImplementation(HWWallDispatcher* dispatcher, HWDrawInfo* di, FRenderState& state, int pass)
{
	// Context guard shield protects from empty pointers crashes during multipass tracking
	if (di == nullptr || sector == nullptr) return;

	g_isCurrentlyHWFlatDrawing = this; // we're drawing flats now

	int rel = getExtraLight(); // Extract fake contrast natively from the current flat zone

	FMaterial* flatMat = FMaterial::ValidateTexture(texture, 0); // 0 for clean default scaleflags

	switch (pass)
	{
	case GLPASS_PLAIN:
	case GLPASS_ALL:
		// Single-pass rendering, same, but also creates the dynlight data.
		SetColor(state, di->Level, di->lightmode, lightlevel, rel, di->isFullbrightScene(), Colormap, alpha);
		SetFog(state, di->Level, di->lightmode, lightlevel, rel, di->isFullbrightScene(), &Colormap, false);

		state.SetAddColor(AddColor | 0xff000000);

		if (flatMat != nullptr)
		{
			state.SetObjectColor(FlatColor | 0xff000000);
		}

		if (sector->special != GLSector_Skybox)
		{
			if (flatMat != nullptr)
			{
				state.SetGL1xMaterial(flatMat, CLAMP_NONE, 0, -1, false);
			}
			SetPlaneTextureRotation(state, &plane, texture);
			DrawSubsectors(di, state);
			state.EnableTextureMatrix(false);
		}
		else
		{
			if (flatMat != nullptr)
			{
				state.SetGL1xMaterial(flatMat, CLAMP_XY, 0, -1, false);
			}
			DrawGL1xSkyboxSector(di, state, pass, (pass == GLPASS_ALL || dynlightindex > -1));
		}
		state.SetObjectColor(0xffffffff);
		break;

	case GLPASS_LIGHTSONLY:
		if (flatMat != nullptr)
		{
			// Dynamic lights pass gathers per-sector tracking sheets via DrawLightsCompat context loop
			DrawLightsCompat(di, pass);
		}
		break;

	case GLPASS_TRANSLUCENT:
	{
		if (gl.legacyMode && sector->special != GLSector_Skybox)
		{
			// In GL1x/GL2x very dark translucent surfaces disappear,
			// ... thus we can't make them darker than this
			if (lightlevel < 100) lightlevel = 100;
		}

		int adjustedLightLevel = lightlevel;
		const float brightnessBoost = 0.55f;
		const int darkSurfLightlevelThresh = 150;
		float reduceBoostOnDarkSurf = 0.75f;

		if (gl.legacyMode && lightlevel < darkSurfLightlevelThresh)
		{
			// Linear decay multiplier: maps safely from 1.0f down to 0.45f inside deep shadows
			reduceBoostOnDarkSurf = 0.45f + ((float)(lightlevel - 100) / ((float)darkSurfLightlevelThresh - 100.0f)) * 0.55f;
		}

		// [Darkcrafter07] - Check active light node chain using the new 4.14dev section architecture
		if (gl.legacyMode && section != nullptr && section->lighthead && sector->special != GLSector_Skybox)
		{
			// 1. Detect a mirrored-reflected surface by an overlay style
			bool isTrueMirrorOverlay = (renderstyle == STYLE_Add);

			// 2. Detect a mirrored-reflected surface by "reflect" array on map sector
			int realSectorIndex = sector->sectornum;
			if (!isTrueMirrorOverlay && realSectorIndex >= 0 && realSectorIndex < (int)di->Level->sectors.Size())
			{
				sector_t* realSector = &di->Level->sectors[realSectorIndex];
				if (realSector && (realSector->reflect[0] > 0.0f || realSector->reflect[1] > 0.0f))
				{
					isTrueMirrorOverlay = true;
				}
			}

			// So far, this is for translucent 3D floors.
			// Do NOT use boost or turn it OFF for mirrored-reflected surface or a skybox
			if (!isTrueMirrorOverlay)
			{
				float totalFlatCpuintensity = 0.0f;
				float baseFactor = 0.15f * brightnessBoost * reduceBoostOnDarkSurf;
				float flatz = (float)plane.plane.ZatPoint(r_viewpoint.Pos);

				FLightNode* node = section->lighthead;
				while (node)
				{
					FDynamicLight* light = node->lightsource;
					if (light && light->IsActive() && light->GetRadius() > 0.0f)
					{
						float radius = light->GetRadius();
						float dx = (float)sector->centerspot.X - (float)light->X();
						float dy = (float)sector->centerspot.Y - (float)light->Y();
						float dz = flatz - (float)light->Z();
						float flatDist2 = (dx * dx) + (dy * dy) + (dz * dz);

						if (flatDist2 < (radius * radius))
						{
							float dist = sqrtf(flatDist2);
							totalFlatCpuintensity += (1.0f - (dist / radius)) * baseFactor * 5.0f;
						}
					}
					node = node->nextLight;
				}

				if (totalFlatCpuintensity > 0.0f)
				{
					// Protective plateau
					if (totalFlatCpuintensity > 0.75f) totalFlatCpuintensity = 0.75f;
					adjustedLightLevel += (int)(totalFlatCpuintensity * 255.0f);
					if (adjustedLightLevel > 255) adjustedLightLevel = 255;
				}
			}
		}

		if (renderstyle == STYLE_Add) state.BlendGL1xFunc(GL_SRC_ALPHA, GL_ONE);

		// Feed the resulting light to the engine cache via 4.14dev APIs
		SetColor(state, di->Level, di->lightmode, adjustedLightLevel, rel, di->isFullbrightScene(), Colormap, alpha);
		SetFog(state, di->Level, di->lightmode, adjustedLightLevel, rel, di->isFullbrightScene(), &Colormap, false);

		if (flatMat != nullptr)
		{
			state.SetObjectColor(FlatColor | 0xff000000);

			state.AlphaFunc(Alpha_GEqual, 0.f);
			state.EnableTexture(false);
			DrawSubsectors(di, state);
			state.EnableTexture(true);
		}
		else
		{
			state.AlphaFunc(Alpha_GEqual, 0.f);
			if (flatMat != nullptr)
			{
				state.SetGL1xMaterial(flatMat, CLAMP_NONE, 0, -1, false);
			}
			SetPlaneTextureRotation(state, &plane, texture);

			DrawSubsectors(di, state);
			state.EnableTextureMatrix(false);
		}

		if (renderstyle == STYLE_Add) state.BlendGL1xFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		state.SetObjectColor(0xffffffff);
		break;
	}

	case GLPASS_LIGHTTEX:
	case GLPASS_LIGHTTEX_ADDITIVE:
	case GLPASS_LIGHTTEX_FOGGY:
	case GLPASS_TRANSLUCENT_LIGHTTEX:
		DrawLightsCompat(di, pass);
		break;
	case GLPASS_TEXONLY:
		// [Darkcrafter07] - Universal Fixed-Function texture-only pass matching the walls layout
		if (flatMat != nullptr)
		{
			state.SetGL1xMaterial(flatMat, CLAMP_NONE, 0, -1, false);
		}
		SetPlaneTextureRotation(state, &plane, texture);
		DrawSubsectors(di, state);
		state.EnableTextureMatrix(false);
		break;

	case GLPASS_LIGHTTEX_OVERBRIGHT1_LEGACY: // IS DISABLED AND UNUSED BUT CAN SHOW HOW TO DO IT ALTERNATIVELY
		// Cleaned up legacy overbright brightening pass for flats
		// Uses low-level DrawLightsCompat routine integrated with fixed constants
		state.BlendGL1xFunc(GL_DST_COLOR, GL_ONE);
		state.SetDepthFunc(GL_EQUAL);
		state.SetDepthMask(false);

		// Bind the native dynamic light texture filter mask (glLight blob sheet)
		if (gl_SetupLightTexture())
		{
			if (this->sector && sector->special != GLSector_Skybox)
			{
				// Execute light pass loops. Light stacking now happens per-light inside DrawGL1xSubsectorLights.
				DrawLightsCompat(di, pass);	// Triggers our clean, seamless, scaled DrawGL1xSubsectorLights loop
			}
		}

		// FIX: SYNC STATE MACHINE AND PREVENT FLATS LEAKS INTO SKY
		// Force-update the local render state cache to prevent blending and depth corruption
		state.EnableFog(true);
		state.BlendGL1xFunc(GL_ONE, GL_ZERO);
		state.SetTextureMode(TM_MODULATE);

		state.SetDepthMask(true);
		state.SetDepthFunc(DF_Less);

		// Flush all fixed-function parameters directly into the GPU registers.
		state.ApplyFixedGL1xFunction(nullptr);

		// Hardware fallback override path for strict OpenGL 1.1 compliance
		glEnable(GL_FOG);
		glBlendFunc(GL_ONE, GL_ZERO);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		break;

	case GLPASS_LIGHTEFFECTS_LEGACY: // with big dynlights baking
	{
		const float overallGlowIntensity = 0.9f;

		// [Darkcrafter07] - Cleanly extract the legacy brightmap layer from our top-scope flatMat solver
		FMaterial* bm = (flatMat != nullptr) ? flatMat->GetBrightmapLegacy() : nullptr;

		// Calculate total current light level including extra light modifications
		int totalLight = lightlevel + rel;
		if (totalLight > 255) totalLight = 255;
		if (totalLight < 0) totalLight = 0;

		float intensityFactor;
		if (totalLight < 96)
		{
			// Full effect below 96 light level to prevent early dimming
			intensityFactor = 1.0f;
		}
		else
		{
			// Optimized inverse light factor for the 96-255 range using multiplication
			const float rangeFactorInv = 1.0f / (255.0f - 96.0f);
			float factor = 1.0f - ((float)(totalLight - 96) * rangeFactorInv);

			// Add a subtle ~2.5% parabola bump to mid-range without overbrightening high values
			factor = factor + 0.1f * factor * (1.0f - factor);

			// Scale intensity to smoothly drop from 1.0 (at 96) down to 0.01 (at 255)
			intensityFactor = (factor * 0.99f) + 0.01f;
		}

		if (intensityFactor > 1.0f) intensityFactor = 1.0f;
		if (intensityFactor < 0.0f) intensityFactor = 0.0f;

		// Switch the texture mode for the whole remainder of the frame
		state.SetTextureMode(TM_BRIGHTMAP_LEGACY);
		glEnable(GL_TEXTURE_2D);

		if (bm) // PASS A: BRIGHTMAPS RENDERING
		{
			// Apply calculated intensity as modulation color
			state.SetColor(intensityFactor, intensityFactor, intensityFactor, 1.0f);

			state.SetGL1xMaterial(bm, CLAMP_NONE, 0, -1, false);
			SetPlaneTextureRotation(state, &plane, texture);
			DrawSubsectors(di, state);
			state.EnableTextureMatrix(false);
		}

		// --- PASS B: FLATS HUGE DYNLIGHT RADIUS BAKING TO BOOST FPS ---
		const bool useSmoothGouradFlatDynlightBake = true;
		float intensityModifier = 1.0f;
		{
			// Otherwise it gets too bright on initially bright flats
			if (lightlevel <= 136) { intensityModifier *= 0.995f; }
			else if (lightlevel >= 137 && lightlevel < 144) { intensityModifier *= 0.950f; }
			else if (lightlevel >= 144 && lightlevel < 152) { intensityModifier *= 0.800f; }
			else if (lightlevel >= 152 && lightlevel < 160) { intensityModifier *= 0.750f; }
			else if (lightlevel >= 160 && lightlevel < 168) { intensityModifier *= 0.600f; }
			else if (lightlevel >= 168 && lightlevel < 176) { intensityModifier *= 0.550f; }
			else if (lightlevel >= 176 && lightlevel < 184) { intensityModifier *= 0.450f; }
			else if (lightlevel >= 184 && lightlevel < 192) { intensityModifier *= 0.650f; }
			else if (lightlevel >= 192 && lightlevel < 200) { intensityModifier *= 0.600f; }
			else if (lightlevel >= 200 && lightlevel < 216) { intensityModifier *= 0.500f; }
			else if (lightlevel >= 216 && lightlevel < 232) { intensityModifier *= 0.450f; }
			else if (lightlevel >= 232 && lightlevel < 248) { intensityModifier *= 0.550f; }
			else if (lightlevel >= 255) { intensityModifier *= 0.575f; }
		}
		intensityModifier *= 1.35f;

		if (useSmoothGouradFlatDynlightBake) // looks way better
		{
			if (gl_legacy_dynlight_baked_huge && sector && sector->subsectorcount > 0)
			{
				// Backup baseline blending states directly from public state trackers
				int backupTexMode = state.GetTextureMode();

				// Modulation: the flat texture times the per-vertex light color
				state.SetTextureMode(TM_MODULATE);
				if (flatMat != nullptr)
				{
					state.SetGL1xMaterial(flatMat, CLAMP_NONE, 0, -1, false);
				}
				state.SetColor(1.0f, 1.0f, 1.0f, 1.0f); // color comes from vertices

				// Texture matrix must be set AFTER the material and BEFORE Apply
				SetPlaneTextureRotation(state, &plane, texture);

				// SECURED BLENDING HOOK VIA REGISTRIES MAPPING:
				state.BlendGL1xFunc(GL_DST_COLOR, GL_ZERO);
				state.ApplyFixedGL1xFunction(nullptr); // Commit state machine updates into hardware cleanly

				// Hardware anti-z-fighting: draw slightly in front of the regular flat
				state.SetDepthFunc(GL_EQUAL);
				state.SetDepthMask(false);
				glEnable(GL_POLYGON_OFFSET_FILL);
				glPolygonOffset(-0.5f, -0.5f);

				// THE 3D-FLOORS PLANE ANCHOR
				// CRITICAL FIX: Extract the pristine 'secplane_t' straight from the current 
				// GLFlat instance context tracker ('this->plane.plane') instead of base sector.
				const secplane_t& activeRenderPlane = this->plane.plane; // The 3D-floor link

				for (int i = 0; i < sector->subsectorcount; ++i)
				{
					subsector_t* subSec = sector->subsectors[i];
					if (!subSec || subSec->numlines == 0) continue;

					if (gGouraudScratch.Size() < subSec->numlines)
						gGouraudScratch.Resize(subSec->numlines);

					FFlatVertexGouraud* verts = &gGouraudScratch[0];

					// Colors first; skip the subsector if no large light reaches it
					if (!gl_FillSubsectorGouraud(subSec, activeRenderPlane, sector->PortalGroup,
						verts, this->ceiling, intensityModifier)) continue;

					// World-space UVs; orientation comes from the texture matrix set above
					gl_FillSubsectorUV(subSec, activeRenderPlane, flatMat, verts);
					gl_DrawGouraudFan(verts, (int)subSec->numlines);
				}

				// Restore the state the rest of the frame expects, exactly as it was.
				state.EnableTextureMatrix(false);

				glDisable(GL_POLYGON_OFFSET_FILL);
				state.SetDepthMask(true);
				state.SetDepthFunc(GL_EQUAL);

				// Frame restoration:
				state.SetTextureMode(backupTexMode);    // Re-track previous texture mode
				state.BlendGL1xFunc(GL_ONE, GL_ZERO);   // Restore baseline blending factors

				// THE SHIELD: Call reset color routine
				state.ResetColor();
				state.ApplyFixedGL1xFunction(nullptr);
			}
		}
		else // way worse flat light baking, unused but kept for historical record
		{
			float accumulatedLight[3] = { 0.0f, 0.0f, 0.0f };
			float subsectorsValidCount = 0.0f;

			// Accumulate static lightmap data across all geometric partitions of this sector
			for (int i = 0; i < sector->subsectorcount; ++i)
			{
				subsector_t* subSec = sector->subsectors[i];
				if (subSec)
				{
					float singleSubLight[3] = { 0.0f, 0.0f, 0.0f };
					if (gl_GetFlatStaticLightmaps(subSec, this->plane, this->ceiling, singleSubLight))
					{
						accumulatedLight[0] += singleSubLight[0];
						accumulatedLight[1] += singleSubLight[1];
						accumulatedLight[2] += singleSubLight[2];
						subsectorsValidCount += 1.0f;
					}
				}
			}

			if (subsectorsValidCount > 0.0f)
			{
				float avgR = accumulatedLight[0] / subsectorsValidCount;
				float avgG = accumulatedLight[1] / subsectorsValidCount;
				float avgB = accumulatedLight[2] / subsectorsValidCount;

				if (avgR > 1.0f) avgR = 1.0f;
				if (avgG > 1.0f) avgG = 1.0f;
				if (avgB > 1.0f) avgB = 1.0f;

				state.SetTextureMode(TM_MODULATE);
				if (flatMat != nullptr)
				{
					state.SetGL1xMaterial(flatMat, CLAMP_NONE, 0, -1, false);
				}
				state.SetColor(avgR, avgG, avgB, intensityModifier);

				state.SetDepthFunc(GL_EQUAL);
				state.SetDepthMask(false);
				glEnable(GL_POLYGON_OFFSET_FILL);
				glPolygonOffset(-0.5f, -0.5f);

				SetPlaneTextureRotation(state, &plane, texture);
				DrawSubsectors(di, state);
				state.EnableTextureMatrix(false);

				glDisable(GL_POLYGON_OFFSET_FILL);
				state.SetDepthMask(true);
				state.SetDepthFunc(GL_EQUAL);
				state.SetTextureMode(TM_BRIGHTMAP_LEGACY);
			}
		}
		break;
	}

	default:
		break;
	}

	// Final safe clear of the pipeline thread tracking markers for flats geometry
	g_isCurrentlyHWFlatDrawing = nullptr;
}

//==========================================================================
//
// HWFlat::PutFlat
//
// submit to the renderer
//
//==========================================================================

inline void HWFlat::PutFlat(HWDrawInfo *di, bool fog)
{
	if (di->isFullbrightScene())
	{
		Colormap.Clear();
	}
	else if (!screen->BuffersArePersistent())
	{
		if (di->Level->HasDynamicLights && texture != nullptr && !di->isFullbrightScene() && !(hacktype & (SSRF_PLANEHACK|SSRF_FLOODHACK)) )
		{
			SetupLights(di, section->lighthead, lightdata, sector->PortalGroup);
		}
	}
	di->AddFlat(this, fog);
}

//==========================================================================
//
// This draws one flat 
// The whichplane boolean indicates if the flat is a floor(false) or a ceiling(true)
//
//==========================================================================

void HWFlat::Process(HWDrawInfo *di, sector_t * model, int whichplane, bool fog)
{
	plane.GetFromSector(model, whichplane);
	model->ceilingplane.dithertransflag = false; // Resetting this every frame
	model->floorplane.dithertransflag = false; // Resetting this every frame
	if (whichplane != int(ceiling))
	{
		// Flip the normal if the source plane has a different orientation than what we are about to render.
		plane.plane.FlipVert();
	}

	if (!fog)
	{
		texture =  TexMan.GetGameTexture(plane.texture, true);
		if (!texture || !texture->isValid()) return;
		if (texture->isFullbright()) 
		{
			Colormap.MakeWhite();
			lightlevel=255;
		}
	}
	else 
	{
		texture = NULL;
		lightlevel = abs(lightlevel);
	}

	z = plane.plane.ZatPoint(0.f, 0.f);
	if (sector->special == GLSector_Skybox)
	{
		auto vert = screen->mVertexData->AllocVertices(4);
		CreateSkyboxVertices(vert.first);
		iboindex = vert.second;
	}

	// For hacks this won't go into a render list.
	PutFlat(di, fog);
	rendered_flats++;
}

//==========================================================================
//
// Sets 3D floor info. Common code for all 4 cases 
//
//==========================================================================

void HWFlat::SetFrom3DFloor(F3DFloor *rover, bool top, bool underside)
{
	F3DFloor::planeref & plane = top? rover->top : rover->bottom;

	// FF_FOG requires an inverted logic where to get the light from
	lightlist_t *light = P_GetPlaneLight(sector, plane.plane, underside);
	lightlevel = hw_ClampLight(*light->p_lightlevel);

	if (rover->flags & FF_FOG)
	{
		Colormap.LightColor = light->extra_colormap.FadeColor;
		FlatColor = 0xffffffff;
		AddColor = 0;
		TextureFx = nullptr;
	}
	else
	{
		CopyFrom3DLight(Colormap, light);
		FlatColor = plane.model->SpecialColors[plane.isceiling];
		AddColor = plane.model->AdditiveColors[plane.isceiling];
		TextureFx = &plane.model->planes[plane.isceiling].TextureFx;
	}


	alpha = rover->alpha/255.0f;
	renderstyle = rover->flags&FF_ADDITIVETRANS? STYLE_Add : STYLE_Translucent;
	iboindex = plane.vindex;
}

//==========================================================================
//
// Process a sector's flats for rendering
// This function is only called once per sector.
// Subsequent subsectors are just quickly added to the ss_renderflags array
//
//==========================================================================

void HWFlat::ProcessSector(HWDrawInfo *di, sector_t * frontsector, int which)
{
	lightlist_t * light;
	FSectorPortal *port;

#ifdef _DEBUG
	if (frontsector->sectornum == gl_breaksec)
	{
		int a = 0;
	}
#endif

	// Get the real sector for this one.
	sector = &di->Level->sectors[frontsector->sectornum];
	extsector_t::xfloor &x = sector->e->XFloor;
	dynlightindex = -1;
    hacktype = (which & (SSRF_PLANEHACK|SSRF_FLOODHACK));

	uint8_t sink;
	uint8_t &srf = hacktype? sink : di->section_renderflags[di->Level->sections.SectionIndex(section)];
	auto &vp = di->Viewpoint;

	//
	//
	//
	// do floors
	//
	//
	//
	if ((which & SSRF_RENDERFLOOR) && (vp.bDoOrtho ? vp.ViewVector3D.dot(frontsector->floorplane.Normal()) < 0.0 : frontsector->floorplane.ZatPoint(vp.Pos) <= vp.Pos.Z) && (!section || !(section->flags & FSection::DONTRENDERFLOOR)))
	{
		// process the original floor first.

		srf |= SSRF_RENDERFLOOR;

		lightlevel = hw_ClampLight(frontsector->GetFloorLight());
		Colormap = frontsector->Colormap;
		FlatColor = frontsector->SpecialColors[sector_t::floor];
		AddColor = frontsector->AdditiveColors[sector_t::floor];
		TextureFx = &frontsector->planes[sector_t::floor].TextureFx;

		port = frontsector->ValidatePortal(sector_t::floor);
		if ((stack = (port != NULL)))
		{
            /* to be redone in a less invasive manner
			if (port->mType == PORTS_STACKEDSECTORTHING)
			{
				di->AddFloorStack(sector);	// stacked sector things require visplane merging.
			}
             */
			alpha = frontsector->GetAlpha(sector_t::floor);
		}
		else
		{
			alpha = 1.0f - frontsector->GetReflect(sector_t::floor);
		}

		if (alpha != 0.f && frontsector->GetTexture(sector_t::floor) != skyflatnum)
		{
			iboindex = frontsector->iboindex[sector_t::floor];

			ceiling = false;
			renderflags = SSRF_RENDERFLOOR;

			if (x.ffloors.Size())
			{
				light = P_GetPlaneLight(sector, &frontsector->floorplane, false);
				if ((!(sector->GetFlags(sector_t::floor)&PLANEF_ABSLIGHTING) || light->lightsource == NULL)
					&& (light->p_lightlevel != &frontsector->lightlevel))
				{
					lightlevel = hw_ClampLight(*light->p_lightlevel);
				}

				CopyFrom3DLight(Colormap, light);
			}
			renderstyle = STYLE_Translucent;
			Process(di, frontsector, sector_t::floor, false);
		}
	}

	//
	//
	//
	// do ceilings
	//
	// 
	//
	if ((which & SSRF_RENDERCEILING) && (vp.bDoOrtho ? vp.ViewVector3D.dot(frontsector->ceilingplane.Normal()) < 0.0 : frontsector->ceilingplane.ZatPoint(vp.Pos) >= vp.Pos.Z) && (!section || !(section->flags & FSection::DONTRENDERCEILING)))
	{
		// process the original ceiling first.

		srf |= SSRF_RENDERCEILING;

		lightlevel = hw_ClampLight(frontsector->GetCeilingLight());
		Colormap = frontsector->Colormap;
		FlatColor = frontsector->SpecialColors[sector_t::ceiling];
		AddColor = frontsector->AdditiveColors[sector_t::ceiling];
		TextureFx = &frontsector->planes[sector_t::ceiling].TextureFx;
		port = frontsector->ValidatePortal(sector_t::ceiling);
		if ((stack = (port != NULL)))
		{
            /* as above for floors
			if (port->mType == PORTS_STACKEDSECTORTHING)
			{
				di->AddCeilingStack(sector);
			}
             */
			alpha = frontsector->GetAlpha(sector_t::ceiling);
		}
		else
		{
			alpha = 1.0f - frontsector->GetReflect(sector_t::ceiling);
		}

		if (alpha != 0.f && frontsector->GetTexture(sector_t::ceiling) != skyflatnum)
		{
			iboindex = frontsector->iboindex[sector_t::ceiling];
			ceiling = true;
			renderflags = SSRF_RENDERCEILING;

			if (x.ffloors.Size())
			{
				light = P_GetPlaneLight(sector, &sector->ceilingplane, true);

				if ((!(sector->GetFlags(sector_t::ceiling)&PLANEF_ABSLIGHTING))
					&& (light->p_lightlevel != &frontsector->lightlevel))
				{
					lightlevel = hw_ClampLight(*light->p_lightlevel);
				}
				CopyFrom3DLight(Colormap, light);
			}
			renderstyle = STYLE_Translucent;
			Process(di, frontsector, sector_t::ceiling, false);
		}
	}

	//
	//
	//
	// do 3D floors
	//
	//
	//

	stack = false;
	if ((which & SSRF_RENDER3DPLANES) && x.ffloors.Size())
	{
		renderflags = SSRF_RENDER3DPLANES;
		srf |= SSRF_RENDER3DPLANES;
		// 3d-floors must not overlap!
		double lastceilingheight = sector->CenterCeiling();	// render only in the range of the
		double lastfloorheight = sector->CenterFloor();		// current sector part (if applicable)
		F3DFloor * rover;
		int k;

		// floors are ordered now top to bottom so scanning the list for the best match
		// is no longer necessary.

		ceiling = true;
		Colormap = frontsector->Colormap;
		for (k = 0; k < (int)x.ffloors.Size(); k++)
		{
			rover = x.ffloors[k];

			if ((rover->flags&(FF_EXISTS | FF_RENDERPLANES | FF_THISINSIDE)) == (FF_EXISTS | FF_RENDERPLANES))
			{
				if (rover->flags&FF_FOG && di->isFullbrightScene()) continue;
				if (!rover->top.copied && rover->flags&(FF_INVERTPLANES | FF_BOTHPLANES))
				{
					double ff_top = rover->top.plane->ZatPoint(sector->centerspot);
					if (ff_top < lastceilingheight)
					{
						if ((vp.bDoOrtho ? vp.ViewVector3D.dot(rover->top.plane->Normal()) > 0.0 : vp.Pos.Z <= rover->top.plane->ZatPoint(vp.Pos)))
						{
							SetFrom3DFloor(rover, true, !!(rover->flags&FF_FOG));
							Colormap.FadeColor = frontsector->Colormap.FadeColor;
							Process(di, rover->top.model, rover->top.isceiling, !!(rover->flags&FF_FOG));
						}
						lastceilingheight = ff_top;
					}
				}
				if (!rover->bottom.copied && !(rover->flags&FF_INVERTPLANES))
				{
					double ff_bottom = rover->bottom.plane->ZatPoint(sector->centerspot);
					if (ff_bottom < lastceilingheight)
					{
						if ((vp.bDoOrtho ? vp.ViewVector3D.dot(rover->bottom.plane->Normal()) > 0.0 : vp.Pos.Z <= rover->bottom.plane->ZatPoint(vp.Pos)))
						{
							SetFrom3DFloor(rover, false, !(rover->flags&FF_FOG));
							Colormap.FadeColor = frontsector->Colormap.FadeColor;
							Process(di, rover->bottom.model, rover->bottom.isceiling, !!(rover->flags&FF_FOG));
						}
						lastceilingheight = ff_bottom;
						if (rover->alpha < 255) lastceilingheight += EQUAL_EPSILON;
					}
				}
			}
		}

		ceiling = false;
		for (k = x.ffloors.Size() - 1; k >= 0; k--)
		{
			rover = x.ffloors[k];

			if ((rover->flags&(FF_EXISTS | FF_RENDERPLANES | FF_THISINSIDE)) == (FF_EXISTS | FF_RENDERPLANES))
			{
				if (rover->flags&FF_FOG && di->isFullbrightScene()) continue;
				if (!rover->bottom.copied && rover->flags&(FF_INVERTPLANES | FF_BOTHPLANES))
				{
					double ff_bottom = rover->bottom.plane->ZatPoint(sector->centerspot);
					if (ff_bottom > lastfloorheight || (rover->flags&FF_FIX))
					{
						if ((vp.bDoOrtho ? vp.ViewVector3D.dot(rover->bottom.plane->Normal()) > 0.0 : vp.Pos.Z >= rover->bottom.plane->ZatPoint(vp.Pos)))
						{
							SetFrom3DFloor(rover, false, !(rover->flags&FF_FOG));
							Colormap.FadeColor = frontsector->Colormap.FadeColor;

							if (rover->flags&FF_FIX)
							{
								lightlevel = hw_ClampLight(rover->model->lightlevel);
								Colormap = rover->GetColormap();
							}

							Process(di, rover->bottom.model, rover->bottom.isceiling, !!(rover->flags&FF_FOG));
						}
						lastfloorheight = ff_bottom;
					}
				}
				if (!rover->top.copied && !(rover->flags&FF_INVERTPLANES))
				{
					double ff_top = rover->top.plane->ZatPoint(sector->centerspot);
					if (ff_top > lastfloorheight)
					{
						if ((vp.bDoOrtho ? vp.ViewVector3D.dot(rover->top.plane->Normal()) > 0.0 : vp.Pos.Z >= rover->top.plane->ZatPoint(vp.Pos)))
						{
							SetFrom3DFloor(rover, true, !!(rover->flags&FF_FOG));
							Colormap.FadeColor = frontsector->Colormap.FadeColor;
							Process(di, rover->top.model, rover->top.isceiling, !!(rover->flags&FF_FOG));
						}
						lastfloorheight = ff_top;
						if (rover->alpha < 255) lastfloorheight -= EQUAL_EPSILON;
					}
				}
			}
		}
	}
}

