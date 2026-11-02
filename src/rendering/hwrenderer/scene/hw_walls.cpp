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

#include "p_local.h"
#include "p_lnspec.h"
#include "a_dynlight.h"
#include "a_sharedglobal.h"
#include "r_defs.h"
#include "r_sky.h"
#include "r_utility.h"
#include "p_maputl.h"
#include "doomdata.h"
#include "g_levellocals.h"
#include "actorinlines.h"
#include "texturemanager.h"
#include "hw_dynlightdata.h"
#include "hw_material.h"
#include "hw_cvars.h"
#include "hw_clock.h"
#include "hw_lighting.h"
#include "hwrenderer/scene/hw_drawinfo.h"
#include "hwrenderer/scene/hw_drawstructs.h"
#include "hwrenderer/scene/hw_portal.h"
#include "hw_lightbuffer.h"
#include "hw_renderstate.h"
#include "hw_skydome.h"
#include "hw_walldispatcher.h"

#include "common/rendering/gl_load/gl_system.h"
#include "common/rendering/gl_load/gl_interface.h"

HWWall* g_isCurrentlyHWWallDrawing = nullptr; // mainly for GL1x/GL2x modes (externed in gl_20.cpp)


void SetGlowPlanes(FRenderState &state, const secplane_t& top, const secplane_t& bottom)
{
	auto& tn = top.Normal();
	auto& bn = bottom.Normal();
	FVector4 tp = { (float)tn.X, (float)tn.Y, (float)top.negiC, (float)top.fD() };
	FVector4 bp = { (float)bn.X, (float)bn.Y, (float)bottom.negiC, (float)bottom.fD() };
	state.SetGlowPlanes(tp, bp);
}

void SetGradientPlanes(FRenderState& state, const secplane_t& top, const secplane_t& bottom)
{
	auto& tn = top.Normal();
	auto& bn = bottom.Normal();
	FVector4 tp = { (float)tn.X, (float)tn.Y, (float)top.negiC, (float)top.fD() };
	FVector4 bp = { (float)bn.X, (float)bn.Y, (float)bottom.negiC, (float)bottom.fD() };
	state.SetGradientPlanes(tp, bp);
}

void SetSplitPlanes(FRenderState& state, const secplane_t& top, const secplane_t& bottom)
{
	auto& tn = top.Normal();
	auto& bn = bottom.Normal();
	FVector4 tp = { (float)tn.X, (float)tn.Y, (float)top.negiC, (float)top.fD() };
	FVector4 bp = { (float)bn.X, (float)bn.Y, (float)bottom.negiC, (float)bottom.fD() };
	state.SetSplitPlanes(tp, bp);
}


//==========================================================================
//
// General purpose wall rendering function
// everything goes through here
//
//==========================================================================

void HWWall::RenderWall(FRenderState &state, int textured)
{
	bool ditherT = false;
	if (seg->sidedef != nullptr)
	{
		ditherT = (type == RENDERWALL_BOTTOM) && (seg->sidedef->Flags & WALLF_DITHERTRANS_BOTTOM);
		ditherT |= (type == RENDERWALL_TOP) && (seg->sidedef->Flags & WALLF_DITHERTRANS_TOP);
		ditherT = ditherT || (seg->sidedef->Flags & WALLF_DITHERTRANS_MID);
	}
	if (ditherT)
	{
		state.SetEffect(EFF_DITHERTRANS);
	}
	assert(vertcount > 0);
	state.SetLightIndex(dynlightindex);
	state.Draw(DT_TriangleFan, vertindex, vertcount);
	vertexcount += vertcount;
	if (ditherT)
	{
		state.SetEffect(EFF_NONE);
		switch(type) // reset this every frame
		{
		case RENDERWALL_TOP:
			seg->sidedef->Flags &= ~WALLF_DITHERTRANS_TOP;
			break;
		case RENDERWALL_BOTTOM:
			seg->sidedef->Flags &= ~WALLF_DITHERTRANS_BOTTOM;
			break;
		default:
			if (seg->sidedef->dithertranscount-- <= 0) seg->sidedef->Flags &= ~WALLF_DITHERTRANS_MID;
		}
	}
}

//==========================================================================
//
// 
//
//==========================================================================

void HWWall::RenderFogBoundary(HWWallDispatcher* di, FRenderState& state)
{
	if (gl_fogmode && !di->isFullbrightScene())
	{
		if (gl.legacyMode)
		{
			RenderFogBoundaryCompat(di->di);
			return; // Safe exit to completely bypass standard core shader state mutations!
		}

		int rel = rellight + getExtraLight();
		state.EnableDrawBufferAttachments(false);
		SetFog(state, di->Level, di->lightmode, lightlevel, rel, false, &Colormap, false);
		state.SetEffect(EFF_FOGBOUNDARY);
		state.AlphaFunc(Alpha_GEqual, 0.f);
		state.SetDepthBias(-1, -128);
		RenderWall(state, HWWall::RWF_BLANK);
		state.ClearDepthBias();
		state.SetEffect(EFF_NONE);
		state.EnableDrawBufferAttachments(true);
	}
}


//==========================================================================
//
// 
//
//==========================================================================
void HWWall::RenderMirrorSurface(HWWallDispatcher*di, FRenderState &state)
{
	if (!TexMan.mirrorTexture.isValid()) return;

	state.SetDepthFunc(DF_LEqual);

	// we use texture coordinates and texture matrix to pass the normal stuff to the shader so that the default vertex buffer format can be used as is.
	state.EnableTextureMatrix(true);

	// Use sphere mapping for this
	state.SetEffect(EFF_SPHEREMAP);
	SetColor(state, di->Level, di->lightmode, lightlevel, 0, di->isFullbrightScene(), Colormap, 0.1f);
	SetFog(state, di->Level, di->lightmode, lightlevel, 0, di->isFullbrightScene(), &Colormap, true);
	state.SetRenderStyle(STYLE_Add);
	state.AlphaFunc(Alpha_Greater, 0);

	auto tex = TexMan.GetGameTexture(TexMan.mirrorTexture, false);
	state.SetMaterial(tex, UF_None, 0, CLAMP_NONE, NO_TRANSLATION, -1); // do not upscale the mirror texture.

	flags &= ~HWWall::HWF_GLOW;
	RenderWall(state, HWWall::RWF_BLANK);

	state.EnableTextureMatrix(false);
	state.SetEffect(EFF_NONE);
	state.AlphaFunc(Alpha_GEqual, gl_mask_sprite_threshold);

	state.SetDepthFunc(DF_Less);

	// This is drawn in the translucent pass which is done after the decal pass
	// As a result the decals have to be drawn here, right after the wall they are on,
	// because the depth buffer won't get set by translucent items.
	if (di->di && seg->sidedef->AttachedDecals)
	{
		DrawDecalsForMirror(di->di, state, di->di->Decals[1]);
	}
	state.SetRenderStyle(STYLE_Translucent);
}

//==========================================================================
//
// 
//
//==========================================================================

static const uint8_t renderwalltotier[] =
{
	side_t::none,
	side_t::top,
	side_t::mid,
	side_t::mid,
	side_t::bottom,
	side_t::none,
	side_t::none,
	side_t::mid,
	side_t::none,
	side_t::mid,
};

#ifdef NPOT_EMULATION
CVAR(Bool, hw_npottest, false, 0)
#endif

void HWWall::RenderTexturedWall(HWWallDispatcher* di, FRenderState& state, int rflags)
{
	int tmode = state.GetTextureMode();
	int rel = rellight + getExtraLight();

	// [Darkcrafter07] - CRITICAL NULLPTR SHIELD! 
	// If frontsector is missing, completely skip glow planes configuration to block memory corruptions!
	if (frontsector != nullptr && (flags & HWWall::HWF_GLOW))
	{
		state.EnableGlow(true);
		state.SetGlowParams(topglowcolor, bottomglowcolor);
		SetGlowPlanes(state, frontsector->ceilingplane, frontsector->floorplane);
	}

	// Wraps texture binding and warping logic inside explicit legacy mode guards
	bool skipTextureBlocks = false;
	if (gl.legacyMode && texture == nullptr)
	{
		skipTextureBlocks = true;
	}

	if (!skipTextureBlocks)
	{
		state.SetMaterial(texture, UF_Texture, 0, flags & 3, NO_TRANSLATION, -1);
#ifdef NPOT_EMULATION
		if (hw_npottest)
		{
			int32_t size = xs_CRoundToInt(texture->GetDisplayHeight());
			int32_t size2;
			for (size2 = 1; size2 < size; size2 += size2) {}
			if (size == size2)
				state.SetNpotEmulation(0.f, 0.f);
			else
			{
				float xOffset = 1.f / texture->GetDisplayWidth();
				state.SetNpotEmulation((1.f * size2) / size, xOffset);
			}
		}
#endif

		if (flags & HWWall::HWF_CLAMPY && (type == RENDERWALL_M2S || type == RENDERWALL_M2SNF) && !texture->isWarped())
		{
			state.SetTextureClamp(true);
		}
	}
	else
	{
		state.EnableTexture(false);
	}

	if (type == RENDERWALL_M2SNF)
	{
		SetFog(state, di->Level, di->lightmode, 255, 0, di->isFullbrightScene(), nullptr, false);
	}

	// [Darkcrafter07] - EXTENDED FRONTSECTOR SAFETY LOCK ENVIRONMENT
	// If frontsector is invalid or empty, we strictly bypass colors evaluation to stop 0x00000184 reads!
	if (frontsector != nullptr && type != RENDERWALL_COLOR && seg != nullptr && seg->sidedef != nullptr)
	{
		auto side = seg->sidedef;
		if (side->Flags & WALLF_EXTCOLOR) // this block incurs a costly cache miss, so only process if needed.
		{
			auto tierndx = renderwalltotier[type];
			auto& tier = side->textures[tierndx];

			// Safe evaluation of special Doom64 wall gradient colors
			PalEntry color1 = side->GetSpecialColor(tierndx, side_t::walltop, frontsector);
			PalEntry color2 = side->GetSpecialColor(tierndx, side_t::wallbottom, frontsector);

			state.SetObjectColor(color1);
			state.SetObjectColor2((color1 != color2) ? color2 : PalEntry(0));
			state.SetAddColor(side->GetAdditiveColor(tierndx, frontsector));

			if (!skipTextureBlocks)
			{
				state.ApplyTextureManipulation(&tier.TextureFx);
			}

			if (color1 != color2)
			{
				state.EnableGradient(true);
				if ((tier.flags & side_t::part::ClampGradient) && backsector)
				{
					if (tierndx == side_t::top)
					{
						SetGradientPlanes(state, frontsector->ceilingplane, backsector->ceilingplane);
					}
					else if (tierndx == side_t::mid)
					{
						SetGradientPlanes(state, backsector->ceilingplane, backsector->floorplane);
					}
					else // side_t::bottom:
					{
						SetGradientPlanes(state, backsector->floorplane, frontsector->floorplane);
					}
				}
				else
				{
					SetGradientPlanes(state, frontsector->ceilingplane, frontsector->floorplane);
				}
			}
		}
	}
	else
	{
		// Safe fallback color if frontsector context is temporarily corrupted
		state.SetObjectColor(PalEntry(255, 255, 255));
		state.SetObjectColor2(PalEntry(0));
		state.SetAddColor(0);
	}

	float absalpha = fabsf(alpha);
	if (lightlist == nullptr)
	{
		if (type != RENDERWALL_M2SNF) SetFog(state, di->Level, di->lightmode, lightlevel, rel, di->isFullbrightScene(), &Colormap, RenderStyle == STYLE_Add);
		SetColor(state, di->Level, di->lightmode, lightlevel, rel, di->isFullbrightScene(), Colormap, absalpha);
		RenderWall(state, rflags);
	}
	else
	{
		state.EnableSplit(true);

		if (frontsector != nullptr)
		{
			for (unsigned i = 0; i < lightlist->Size(); i++)
			{
				secplane_t& lowplane = i == (*lightlist).Size() - 1 ? frontsector->floorplane : (*lightlist)[i + 1].plane;
				float low1 = lowplane.ZatPoint(vertexes[0]);
				float low2 = lowplane.ZatPoint(vertexes[1]);

				if (low1 < ztop[0] || low2 < ztop[1])
				{
					int thisll = (*lightlist)[i].caster != nullptr ? hw_ClampLight(*(*lightlist)[i].p_lightlevel) : lightlevel;
					FColormap thiscm;
					thiscm.FadeColor = Colormap.FadeColor;
					thiscm.FogDensity = Colormap.FogDensity;
					CopyFrom3DLight(thiscm, &(*lightlist)[i]);
					SetColor(state, di->Level, di->lightmode, thisll, rel, false, thiscm, absalpha);
					if (type != RENDERWALL_M2SNF) SetFog(state, di->Level, di->lightmode, thisll, rel, false, &thiscm, RenderStyle == STYLE_Add);
					SetSplitPlanes(state, (*lightlist)[i].plane, lowplane);
					RenderWall(state, rflags);
				}
				if (low1 <= zbottom[0] && low2 <= zbottom[1]) break;
			}
		}

		state.EnableSplit(false);
	}

	state.SetNpotEmulation(0.f, 0.f);
	state.SetObjectColor(0xffffffff);
	state.SetObjectColor2(0);
	state.SetAddColor(0);
	state.SetTextureMode(tmode);
	state.SetTextureClamp(false);
	state.EnableGlow(false);
	state.EnableGradient(false);
	state.ApplyTextureManipulation(nullptr);
}

//==========================================================================
//
// 
//
//==========================================================================

void HWWall::RenderTranslucentWall(HWWallDispatcher*di, FRenderState &state)
{
	state.SetRenderStyle(RenderStyle);
	if (texture)
	{
		if (!texture->GetTranslucency()) state.AlphaFunc(Alpha_GEqual, gl_mask_threshold);
		else state.AlphaFunc(Alpha_GEqual, 0.f);
		RenderTexturedWall(di, state, HWWall::RWF_TEXTURED | HWWall::RWF_NOSPLIT);
	}
	else
	{
		state.AlphaFunc(Alpha_GEqual, 0.f);
		SetColor(state, di->Level, di->lightmode, lightlevel, 0, false, Colormap, fabsf(alpha));
		SetFog(state, di->Level, di->lightmode, lightlevel, 0, false, &Colormap, RenderStyle == STYLE_Add);
		state.EnableTexture(false);
		RenderWall(state, HWWall::RWF_NOSPLIT);
		state.EnableTexture(true);
	}
	state.SetRenderStyle(STYLE_Translucent);
}

//==========================================================================
//
// 
//
//==========================================================================
void HWWall::DrawWall(HWWallDispatcher*di, FRenderState &state, bool translucent)
{
	g_isCurrentlyHWWallDrawing = this; // we're drawing walls now

	if (screen->BuffersArePersistent())
	{
		if (di->di && di->Level->HasDynamicLights && !di->isFullbrightScene() && texture != nullptr)
		{
			SetupLights(di->di, lightdata);
		}
		MakeVertices(!!(flags & HWWall::HWF_TRANSLUCENT));
	}

	state.SetNormal(glseg.Normal());
	if (!translucent)
	{
		RenderTexturedWall(di, state, HWWall::RWF_TEXTURED);
	}
	else
	{
		switch (type)
		{
		case RENDERWALL_MIRRORSURFACE:
			RenderMirrorSurface(di, state);
			break;

		case RENDERWALL_FOGBOUNDARY:
			RenderFogBoundary(di, state);
			break;

		default:
			RenderTranslucentWall(di, state);
			break;
		}
	}

	g_isCurrentlyHWWallDrawing = nullptr; // we're not drawing walls anymore
}

//==========================================================================
//
// 
//
//==========================================================================
void HWWall::DrawGL1xWallImplementation(HWWallDispatcher* dispatcher, HWDrawInfo* di, FRenderState& state, int pass)
{
	float absalpha = 1.0f;
	const float invMul255 = 1.0f / 255.0f;

	g_isCurrentlyHWWallDrawing = this; // we're drawing walls now

	state.SetDepthFunc(DF_Less);
	state.SetNormal(glseg.Normal());

	switch (pass)
	{
	case GLPASS_LIGHTSONLY:
		if (di && di->Level->HasDynamicLights && texture != nullptr)
		{
			SetupLights(di, lightdata);
		}
		break;

	case GLPASS_ALL:
		if (di && di->Level->HasDynamicLights && texture != nullptr)
		{
			SetupLights(di, lightdata);
		}
		// fall through
	case GLPASS_PLAIN:
		state.EnableTexture(false);
		state.EnableBrightmap(false);
		absalpha = fabsf(alpha);
		SetColor(state, di->Level, di->lightmode, lightlevel, rellight + getExtraLight(), di->isFullbrightScene(), Colormap, absalpha);
		RenderWall(state, HWWall::RWF_TEXTURED);
		break;

	case GLPASS_TRANSLUCENT:
	{
		int backupLight = lightlevel;

		if (gl.legacyMode && seg && seg->frontsector)
		{
			if (lightlevel < 100) lightlevel = 100;
		}

		switch (type)
		{
		case RENDERWALL_MIRRORSURFACE:
			RenderMirrorSurface(dispatcher, state);
			break;

		case RENDERWALL_FOGBOUNDARY:
			RenderFogBoundaryCompat(di);
			break;

		default:
			RenderTranslucentWall(dispatcher, state);
			break;
		}

		lightlevel = backupLight;
		break;
	}

	case GLPASS_LIGHTTEX:
	case GLPASS_LIGHTTEX_ADDITIVE:
	case GLPASS_LIGHTTEX_FOGGY:
	case GLPASS_TRANSLUCENT_LIGHTTEX:
		RenderLightsCompat(di, state, pass);
		break;

	case GLPASS_TEXONLY:
	{
		if (gl.legacyMode) // GL1x/GL2x way
		{
			auto side = seg ? seg->sidedef : nullptr;
			auto tierndx = renderwalltotier[type];

			// [Darkcrafter07] - THE ULTIMATE FRONTSECTOR BACKUP GUARD LAYER!
			// If the local wall's frontsector tracker is nullptr, we dynamically fallback 
			// to the segment's native frontsector pointer to permanently smash 0x00000184 crashes!
			sector_t* safeSector = frontsector;
			if (safeSector == nullptr && seg != nullptr)
			{
				safeSector = seg->frontsector;
			}

			PalEntry color1 = PalEntry(255, 255, 255);
			PalEntry color2 = PalEntry(255, 255, 255);

			// Execute GetSpecialColor only if BOTH sidedef and frontsector contexts are 100% verified!
			if (side != nullptr && safeSector != nullptr)
			{
				color1 = side->GetSpecialColor(tierndx, side_t::walltop, safeSector);
				color2 = side->GetSpecialColor(tierndx, side_t::wallbottom, safeSector);

				state.SetObjectColor(color1);
				state.SetObjectColor2((color1 != color2) ? color2 : PalEntry(0));
			}
			else
			{
				// High-fidelity factory safety defaults if context gets temporarily corrupted
				state.SetObjectColor(PalEntry(255, 255, 255));
				state.SetObjectColor2(PalEntry(0));
			}

			// // --- Detect Doom64 gradient lit walls in the sector ---
			bool hasDoom64Gradient = (side != nullptr && color1 != color2 && (color1 != PalEntry(255, 255, 255) || color2 != PalEntry(255, 255, 255)));

			if (hasDoom64Gradient)
			{
				// Enforce discrete multiplicative blending for hardware Gouraud shading engine
				state.BlendGL1xFunc(GL_DST_COLOR, GL_ZERO);

				// Resolve material from the native HWWall texture object
				FMaterial* wallMat = FMaterial::ValidateTexture(texture, 0); // 0 for clean default scaleflags

				if (wallMat != nullptr)
				{
					// [Darkcrafter07] - Pass flags & 3 cleanly matching the 4.14dev clampmode matrix tracks
					state.SetGL1xMaterial(wallMat, flags & 3, 0, -1, false);
				}
				state.ApplyFixedGL1xFunction(nullptr);

				// Enforce pixel depth matching to protect multi-pass layout sync
				glDepthFunc(GL_EQUAL);
				glDepthMask(false);
				glShadeModel(GL_SMOOTH); // Hard-lock smooth vertex interpolation!

				float r1 = color1.r * invMul255; float g1 = color1.g * invMul255; float b1 = color1.b * invMul255;
				float r2 = color2.r * invMul255; float g2 = color2.g * invMul255; float b2 = color2.b * invMul255;

				// Reset counter to safely compile the exact same seamless VBO mesh nodes for this pass
				vertcount = 0;
				MakeVertices(false);

				glBegin(GL_QUADS);
				for (int i = 0; i < vertcount; i++)
				{
					float vx = glseg.x1 + (glseg.x2 - glseg.x1) * (float)i / (float)vertcount;
					float vz = ztop[0] + (ztop[1] - ztop[0]) * (float)i / (float)vertcount; // Fixed track indices
					float vy = glseg.y1 + (glseg.y2 - glseg.y1) * (float)i / (float)vertcount;

					// // --- Slope Interpolation ---
					float horizontalFactor = 0.0f;
					float wallLengthX = glseg.x2 - glseg.x1;
					float wallLengthY = glseg.y2 - glseg.y1;
					float totalWallLengthSq = (wallLengthX * wallLengthX) + (wallLengthY * wallLengthY);

					if (totalWallLengthSq > 0.0f)
					{
						horizontalFactor = ((vx - glseg.x1) * wallLengthX + (vy - glseg.y1) * wallLengthY) / totalWallLengthSq;
						if (horizontalFactor > 1.0f) horizontalFactor = 1.0f;
						if (horizontalFactor < 0.0f) horizontalFactor = 0.0f;
					}

					// // Linearly calculate the exact floor and ceiling boundaries at the current vertex position
					float currentWallBottom = zbottom[0] + (zbottom[1] - zbottom[0]) * horizontalFactor;
					float currentWallTop = ztop[0] + (ztop[1] - ztop[0]) * horizontalFactor;

					float heightFactor = 0.0f;
					float totalHeight = currentWallTop - currentWallBottom;
					if (totalHeight > 0.0f)
					{
						heightFactor = (vz - currentWallBottom) / totalHeight;
						if (heightFactor > 1.0f) heightFactor = 1.0f;
						if (heightFactor < 0.0f) heightFactor = 0.0f;
					}

					// // Blend colors safely between bottom (color2) and top (color1) bounds
					float r = r2 + (r1 - r2) * heightFactor;
					float g = g2 + (g1 - g2) * heightFactor;
					float b = b2 + (b1 - b2) * heightFactor;

					glTexCoord2f(tcs[i].u, tcs[i].v);
					glColor4f(r, g, b, 1.0f);
					glVertex3f(vx, vz, vy);
				}
				glEnd();

				// Hardware registry recovery reset
				glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
			}
			else
			{
				// // Regular walls without Doom64 sector gradient lighting
				FMaterial* wallMat = FMaterial::ValidateTexture(texture, 0);
				if (wallMat != nullptr)
				{
					state.SetGL1xMaterial(wallMat, flags & 3, 0, -1, false);
				}
				state.ApplyFixedGL1xFunction(nullptr);

				// Direct zero-overhead hardware vertex array rendering pass!
				glDrawArrays(GL_TRIANGLE_FAN, vertindex, vertcount);
			}
		}
		else // // GL3+ way
		{
			RenderTexturedWall(dispatcher, state, HWWall::RWF_TEXTURED);
		}
		break;
	}
	case GLPASS_LIGHTTEX_OVERBRIGHT1_LEGACY: // DISABLED AND UNUSED BUT CAN SHOW HOW TO DO IT ALTERNATIVELY (in hw_drawinfo.cpp)
	{
		if (seg->sidedef != nullptr && seg->v1 != nullptr && seg->v2 != nullptr)
		{
			// Define local viewport culling radius bubble for open-space landscape sectors
			float cullDist = 16000.0f;
			if (cullDist > 0.0f)
			{
				float maxAllowedDistSq = cullDist * cullDist;

				// Calculate squared distance from camera viewpoint to both vertices of the current wall segment
				float dist1 = (float)(seg->v1->fPos() - r_viewpoint.Pos).LengthSquared();
				float dist2 = (float)(seg->v2->fPos() - r_viewpoint.Pos).LengthSquared();

				// If both vertices escape the culling bubble, skip the entire heavy overbright pass
				if (dist1 > maxAllowedDistSq && dist2 > maxAllowedDistSq)
				{
					break;
				}
			}

			state.BlendGL1xFunc(GL_DST_COLOR, GL_ONE);
			glDepthFunc(GL_EQUAL);
			glDepthMask(false);

			FMaterial* wallMat = FMaterial::ValidateTexture(texture, 0);
			if (wallMat != nullptr)
			{
				state.SetGL1xMaterial(wallMat, CLAMP_NONE, 0, -1, false);
			}

			FLightNode* node = (seg->sidedef->Flags & WALLF_POLYOBJ) ? seg->sidedef->lighthead : (sub ? sub->section->lighthead : nullptr);
			if (node == nullptr)
			{
				break;
			}

			texcoord save[4];
			memcpy(save, tcs, sizeof(tcs)); // Save original wall coordinates

			float baseFactor = clamp((float)gl_legacy_dynlight_overbright_walls, 0.0f, 0.2f);
			float passIntensityMultiplier = 1.5f;

			// Re-sync with structural cachers mapping fields
			float x1 = (float)glseg.x1; float y1 = (float)glseg.y1;
			float x2 = (float)glseg.x2; float y2 = (float)glseg.y2;
			float zb0 = (float)zbottom[0]; float zt0 = (float)ztop[0];
			float zt1 = (float)ztop[1]; float zb1 = (float)zbottom[1];

			while (node)
			{
				FDynamicLight* light = node->lightsource;
				if (light && light->IsActive() && light->GetRadius() > 0.0f)
				{
					state.EnableFog(false);
					state.BlendGL1xEquation(GL_FUNC_ADD);

					float radius = light->GetRadius();
					float radiusSq = radius * radius;
					float lx = (float)light->X();
					float ly = (float)light->Y();
					float lz = (float)light->Z();

					float vtx[] = { x1, zb0, y1, x1, zt0, y1, x2, zt1, y2, x2, zb1, y2 };

					state.ApplyFixedGL1xFunction(nullptr);
					glEnable(GL_TEXTURE_2D);
					glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

					// --- SINGLE FAST PASS WITH DOUBLE INTENSITY ---
					glBegin(GL_QUADS);
					for (int i = 0; i < 4; i++)
					{
						float vx = vtx[i * 3]; float vz = vtx[i * 3 + 1]; float vy = vtx[i * 3 + 2];
						float dx = vx - lx; float dy = vy - ly; float dz = vz - lz;
						float dist2 = (dx * dx) + (dy * dy) + (dz * dz);

						float intensity = 0.0f;
						if (dist2 < radiusSq)
						{
							float dist = sqrtf(dist2);
							intensity = (1.0f - (dist / radius)) * baseFactor * passIntensityMultiplier * 2.0f;
						}

						if (intensity > 1.0f) intensity = 1.0f;
						if (intensity < 0.0f) intensity = 0.0f;

						glTexCoord2f(tcs[i].u, tcs[i].v);
						glColor4f(intensity, intensity, intensity, 1.0f);
						glVertex3f(vx, vz, vy);
					}
					glEnd();
				}
				node = node->nextLight;
			}

			memcpy(tcs, save, sizeof(tcs)); // Restore layout
			vertcount = 0;

			// CLEAN ARCHITECTURAL FIX: SYNC STATE MACHINE AND PREVENT BLEND LEAKS INTO SKY
			state.EnableFog(true);
			state.BlendGL1xFunc(GL_ONE, GL_ZERO);
			state.SetTextureMode(TM_MODULATE);
			glDepthFunc(GL_LESS);
			glDepthMask(true);

			state.ApplyFixedGL1xFunction(nullptr);
			glEnable(GL_FOG);
			glBlendFunc(GL_ONE, GL_ZERO);
			glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		}
	}
	break;

	case GLPASS_LIGHTEFFECTS_LEGACY:
	{
		int wallLight = this->lightlevel;
		float intensityFactor;
		if (wallLight < 96) intensityFactor = 1.0f;
		else
		{
			const float rangeFactorInv = 1.0f / (255.0f - 96.0f);
			float factor = 1.0f - ((float)(wallLight - 96) * rangeFactorInv);
			factor = factor + 0.1f * factor * (1.0f - factor);
			intensityFactor = (factor * 0.99f) + 0.01f;
		}
		if (intensityFactor > 1.0f) intensityFactor = 1.0f;
		if (intensityFactor < 0.0f) intensityFactor = 0.0f;

		float topglow[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		float bottomglow[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

		bool hasWallGlow = (frontsector != nullptr) && frontsector->GetWallGlow(topglow, bottomglow);
		FMaterial* wallMat = FMaterial::ValidateTexture(texture, 0); // 0 for clean default scaleflags
		FMaterial* bm = (wallMat != nullptr) ? wallMat->GetBrightmapLegacy() : nullptr;

		float x1 = (float)glseg.x1; float y1 = (float)glseg.y1;
		float x2 = (float)glseg.x2; float y2 = (float)glseg.y2;
		float zb0 = (float)zbottom[0]; float zt0 = (float)ztop[0];
		float zt1 = (float)ztop[1]; float zb1 = (float)zbottom[1];

		state.SetTextureMode(TM_BRIGHTMAP_LEGACY);
		glEnable(GL_TEXTURE_2D);

		if (bm) // PASS A: RENDER BRIGHTMAPS
		{
			state.SetGL1xMaterial(bm, flags & 3, 0, -1, false);
			state.ApplyFixedGL1xFunction(nullptr);

			glBegin(GL_QUADS);
			for (int i = 0; i < 4; i++)
			{
				glTexCoord2f(tcs[i].u, tcs[i].v);
				glColor4f(intensityFactor, intensityFactor, intensityFactor, 1.0f);

				float vx = (i == 0 || i == 1) ? x1 : x2;
				float vy = (i == 0 || i == 1) ? y1 : y2;
				float vz = (i == 0) ? zb0 : (i == 1) ? zt0 : (i == 2) ? zt1 : zb1;
				glVertex3f(vx, vz, vy);
			}
			glEnd();
		}

		if (gl_legacy_dynlight_baked_huge) // PASS B: VERTEX LIGHTMAP BAKING FOR BIG DYNLIGHTS
		{
			FLightNode *node = (seg != nullptr && seg->sidedef != nullptr) ? 
				(!(seg->sidedef->Flags & WALLF_POLYOBJ) ? 
					seg->sidedef->lighthead : (sub ? sub->section->lighthead : nullptr)) : nullptr;

			if (node != nullptr) // Process only if active lights exist
			{
				float r_accum[4] = { 0.f, 0.f, 0.f, 0.f };
				float g_accum[4] = { 0.f, 0.f, 0.f, 0.f };
				float b_accum[4] = { 0.f, 0.f, 0.f, 0.f };
				bool hasActiveBigLights = false;
				bool isCamglowDynlightHere = false;

				// Re-extract original float bounds exactly matching your hardware quad layout
				float zb0 = (float)zbottom[0]; float zt0 = (float)ztop[0];
				float zt1 = (float)ztop[1];    float zb1 = (float)zbottom[1];

				// Map the 4 un-padded physical vertex tracking anchors onto a DVector3 stack
				DVector3 vtxPoints[4];
				vtxPoints[0] = DVector3(x1, y1, zb0); // Vertex 0: Bottom-Left
				vtxPoints[1] = DVector3(x1, y1, zt0); // Vertex 1: Top-Left
				vtxPoints[2] = DVector3(x2, y2, zt1); // Vertex 2: Top-Right
				vtxPoints[3] = DVector3(x2, y2, zb1); // Vertex 3: Bottom-Right

				// Extract active light level context by evaluating both sides 
				// of the 2S linedef to prevent massive overbrightening on sector junctions.
				int fakeContrastValue = 0;
				int contrastlevlight = 0;
				const float invMul255 = 1.0f / 255.0f;

				float intensityModifier = 1.0f;
				{
					// Otherwise it gets too bright on initially bright walls
					if      (contrastlevlight <= 136)                           { intensityModifier *= 0.995f; }
					else if (contrastlevlight >= 137 && contrastlevlight < 144) { intensityModifier *= 0.950f; }
					else if (contrastlevlight >= 144 && contrastlevlight < 152) { intensityModifier *= 0.800f; }
					else if (contrastlevlight >= 152 && contrastlevlight < 160) { intensityModifier *= 0.750f; }
					else if (contrastlevlight >= 160 && contrastlevlight < 168) { intensityModifier *= 0.600f; }
					else if (contrastlevlight >= 168 && contrastlevlight < 176) { intensityModifier *= 0.550f; }
					else if (contrastlevlight >= 176 && contrastlevlight < 184) { intensityModifier *= 0.600f; }
					else if (contrastlevlight >= 184 && contrastlevlight < 192) { intensityModifier *= 0.750f; }
					else if (contrastlevlight >= 192 && contrastlevlight < 200) { intensityModifier *= 0.800f; }
					else if (contrastlevlight >= 200 && contrastlevlight < 216) { intensityModifier *= 0.850f; }
					else if (contrastlevlight >= 216 && contrastlevlight < 232) { intensityModifier *= 0.900f; }
					else if (contrastlevlight >= 232 && contrastlevlight < 248) { intensityModifier *= 0.950f; }
					else if (contrastlevlight >= 255)                           { intensityModifier *= 0.995f; }
				}
				intensityModifier *= 0.75f;

				float fakeContrastScaleFactor = 1.0f + ((float)fakeContrastValue / 128.0f);
				if (fakeContrastScaleFactor < 0.2f) fakeContrastScaleFactor = 0.2f; // Safe clamp floor bounds

				while (node) // Parse through the active segment light nodes thread chain
				{
					FDynamicLight *light = node->lightsource;
					if (light->IsDynlightCamglow()) isCamglowDynlightHere = true;

					// Target exclusively large broad static lamps (> 468 radius) to kill multi-pass lag
					if (light && light->IsActive() && /* !isCamglowDynlightHere && */ light->GetRadius() > 468.0f)
					{
						hasActiveBigLights = true;

						// Extract raw un-corrupted RGB spectrum channels straight from light memory map
						float lr = (float)light->GetRed() * invMul255;
						float lg = (float)light->GetGreen() * invMul255;
						float lb = (float)light->GetBlue() * invMul255;

						float radius = light->GetRadius();
						DVector3 lpos = light->PosRelative(seg->frontsector->PortalGroup);

						// 1. COMPUTE TRUE PRECISIOH 3D DISTANCE ATTENUATION FOR EACH VERTEX CORNER CORNER.
						for (int i = 0; i < 4; i++)
						{
							DVector3 dirToVert = vtxPoints[i] - lpos; // Distance vector from center to angle corner
							float true3DDist = (float)dirToVert.Length();

							if (true3DDist >= radius) continue; // Out of boundary sphere limits

							// Smooth linear falloff curve for circular gradients on walls
							float ratio = true3DDist / radius;
							float cs = 1.0f - ratio;

							cs *= intensityModifier; // Factor in your overall glow scalar dynamically

							if (light->IsAdditive()) cs *= 0.2f; // Keep additive channels bounded

							// Sum vectors directly into size-4 local matrix channels
							r_accum[i] += lr * cs;
							g_accum[i] += lg * cs;
							b_accum[i] += lb * cs;
						}
					}
					node = node->nextLight; // Move safely down the chain link, otherwise FREEZING CRASH
				}

				if (hasActiveBigLights)
				{
					for (int v = 0; v < 4; v++)
					{
						if (r_accum[v] > 1.0f) r_accum[v] = 1.0f;
						if (g_accum[v] > 1.0f) g_accum[v] = 1.0f;
						if (b_accum[v] > 1.0f) b_accum[v] = 1.0f;
					}

					// Re-bind the diffuse wall texture so that our lightmaps blend identically to glowing flats
					state.SetTextureMode(TM_MODULATE);
					state.SetGL1xMaterial(bm, flags & 3, 0, -1, false);
					state.ApplyFixedGL1xFunction(nullptr); // Commit state machine updates into hardware cleanly

					// ANTI-Z-FIGHTING PIPELINE SECURE
					glDepthFunc(GL_LEQUAL);
					glDepthMask(false);
					glEnable(GL_POLYGON_OFFSET_FILL);
					glPolygonOffset(-0.1f, -0.1f);

					glShadeModel(GL_SMOOTH); // Hard-lock smooth vertex interpolation!

					glBegin(GL_QUADS);
					// Vertex 0: Bottom-Left Boundary
					glTexCoord2f(tcs[0].u, tcs[0].v);
					glColor4f(r_accum[0], g_accum[0], b_accum[0], 1.0f);
					glVertex3f(x1, zb0, y1);
					// Vertex 1: Top-Left Upper Edge
					glTexCoord2f(tcs[1].u, tcs[1].v);
					glColor4f(r_accum[1], g_accum[1], b_accum[1], 1.0f);
					glVertex3f(x1, zt0, y1);
					// Vertex 2: Top-Right Upper Edge
					glTexCoord2f(tcs[2].u, tcs[2].v);
					glColor4f(r_accum[2], g_accum[2], b_accum[2], 1.0f);
					glVertex3f(x2, zt1, y2);
					// Vertex 3: Bottom-Right Boundary
					glTexCoord2f(tcs[3].u, tcs[3].v);
					glColor4f(r_accum[3], g_accum[3], b_accum[3], 1.0f);
					glVertex3f(x2, zb1, y2);
					glEnd();

					// Rollback temporary pipeline offset states cleanly
					glDisable(GL_POLYGON_OFFSET_FILL);
					glDepthMask(true);
					glDepthFunc(GL_EQUAL);

					// Re-synchronize back to brightmap state layer for the next conditional passes
					state.SetTextureMode(TM_BRIGHTMAP_LEGACY);
					state.ApplyFixedGL1xFunction(nullptr); // Flush blending trackers into hardware
				}
			}
		}

		if (hasWallGlow) // PASS C: RENDER PROCEDURAL SECTOR GLOW GRADIENTS (GLOWING FLATS)
		{
			FMaterial* wallMat = FMaterial::ValidateTexture(texture, 0); // 0 for clean default scaleflags
			if (wallMat != nullptr)
			{
				state.SetGL1xMaterial(wallMat, flags & 3, 0, -1, false);
			}
			state.ApplyFixedGL1xFunction(nullptr);

			glDepthFunc(GL_LEQUAL);
			glDepthMask(false);
			glEnable(GL_POLYGON_OFFSET_FILL);
			glPolygonOffset(-0.5f, -0.5f);

			float floorGlowHeight = bottomglow[3];
			float ceilGlowHeight = topglow[3];

			if (floorGlowHeight > 0.0f)
			{
				float trueFloorZ0 = (float)frontsector->floorplane.ZatPoint(seg->v1->fPos());
				float trueFloorZ1 = (float)frontsector->floorplane.ZatPoint(seg->v2->fPos());
				float splitZ0 = trueFloorZ0 + floorGlowHeight;
				float splitZ1 = trueFloorZ1 + floorGlowHeight;
				float rBottomZ0 = trueFloorZ0; if (rBottomZ0 < zb0) rBottomZ0 = zb0;
				float rBottomZ1 = trueFloorZ1; if (rBottomZ1 < zb1) rBottomZ1 = zb1;
				float rTopZ0 = splitZ0; if (rTopZ0 > zt0) rTopZ0 = zt0;
				float rTopZ1 = splitZ1; if (rTopZ1 > zt1) rTopZ1 = zt1;

				if (rTopZ0 > rBottomZ0 && rTopZ1 > rBottomZ1)
				{
					float v_range0 = tcs[1].v - tcs[0].v; float v_range1 = tcs[2].v - tcs[3].v;
					float h_total0 = zt0 - zb0; float h_total1 = zt1 - zb1;

					float sliceBottomV0 = tcs[0].v + (h_total0 > 0.0f ? ((rBottomZ0 - zb0) / h_total0) * v_range0 : 0.0f);
					float sliceBottomV1 = tcs[3].v + (h_total1 > 0.0f ? ((rBottomZ1 - zb1) / h_total1) * v_range1 : 0.0f);
					float sliceTopV0 = tcs[0].v + (h_total0 > 0.0f ? ((rTopZ0 - zb0) / h_total0) * v_range0 : 0.0f);
					float sliceTopV1 = tcs[3].v + (h_total1 > 0.0f ? ((rTopZ1 - zb1) / h_total1) * v_range1 : 0.0f);

					float intensityBottom0 = 1.0f - ((rBottomZ0 - trueFloorZ0) / floorGlowHeight);
					float intensityBottom1 = 1.0f - ((rBottomZ1 - trueFloorZ1) / floorGlowHeight);
					float intensityTop0 = 1.0f - ((rTopZ0 - trueFloorZ0) / floorGlowHeight);
					float intensityTop1 = 1.0f - ((rTopZ1 - trueFloorZ1) / floorGlowHeight);

					glBegin(GL_QUADS);
					glTexCoord2f(tcs[0].u, sliceBottomV0);
					glColor4f(bottomglow[0] * intensityBottom0, bottomglow[1] * intensityBottom0, bottomglow[2] * intensityBottom0, 1.0f);
					glVertex3f(x1, rBottomZ0, y1);

					glTexCoord2f(tcs[1].u, sliceTopV0);
					glColor4f(bottomglow[0] * intensityTop0, bottomglow[1] * intensityTop0, bottomglow[2] * intensityTop0, 1.0f);
					glVertex3f(x1, rTopZ0, y1);

					glTexCoord2f(tcs[2].u, sliceTopV1);
					glColor4f(bottomglow[0] * intensityTop1, bottomglow[1] * intensityTop1, bottomglow[2] * intensityTop1, 1.0f);
					glVertex3f(x2, rTopZ1, y2);

					glTexCoord2f(tcs[3].u, sliceBottomV1);
					glColor4f(bottomglow[0] * intensityBottom1, bottomglow[1] * intensityBottom1, bottomglow[2] * intensityBottom1, 1.0f);
					glVertex3f(x2, rBottomZ1, y2);
					glEnd();
				}
			}

			if (ceilGlowHeight > 0.0f)
			{
				float trueCeilZ0 = (float)frontsector->ceilingplane.ZatPoint(seg->v1->fPos());
				float trueCeilZ1 = (float)frontsector->ceilingplane.ZatPoint(seg->v2->fPos());
				float splitZ0 = trueCeilZ0 - ceilGlowHeight;
				float splitZ1 = trueCeilZ1 - ceilGlowHeight;
				float rTopZ0 = trueCeilZ0; if (rTopZ0 > zt0) rTopZ0 = zt0;
				float rTopZ1 = trueCeilZ1; if (rTopZ1 > zt1) rTopZ1 = zt1;
				float rBottomZ0 = splitZ0; if (rBottomZ0 < zb0) rBottomZ0 = zb0;
				float rBottomZ1 = splitZ1; if (rBottomZ1 < zb1) rBottomZ1 = zb1;

				if (rTopZ0 > rBottomZ0 && rTopZ1 > rBottomZ1)
				{
					float v_range0 = tcs[1].v - tcs[0].v; float v_range1 = tcs[2].v - tcs[3].v;
					float h_total0 = zt0 - zb0; float h_total1 = zt1 - zb1;

					float sliceBottomV0 = tcs[0].v + (h_total0 > 0.0f ? ((rBottomZ0 - zb0) / h_total0) * v_range0 : 0.0f);
					float sliceBottomV1 = tcs[3].v + (h_total1 > 0.0f ? ((rBottomZ1 - zb1) / h_total1) * v_range1 : 0.0f);
					float sliceTopV0 = tcs[0].v + (h_total0 > 0.0f ? ((rTopZ0 - zb0) / h_total0) * v_range0 : 0.0f);
					float sliceTopV1 = tcs[3].v + (h_total1 > 0.0f ? ((rTopZ1 - zb1) / h_total1) * v_range1 : 0.0f);

					float intensityBottom0 = 1.0f - ((trueCeilZ0 - rBottomZ0) / ceilGlowHeight);
					float intensityBottom1 = 1.0f - ((trueCeilZ1 - rBottomZ1) / ceilGlowHeight);
					float intensityTop0 = 1.0f - ((trueCeilZ0 - rTopZ0) / ceilGlowHeight);
					float intensityTop1 = 1.0f - ((trueCeilZ1 - rTopZ1) / ceilGlowHeight);

					glBegin(GL_QUADS);
					glTexCoord2f(tcs[0].u, sliceBottomV0);
					glColor4f(topglow[0] * intensityBottom0, topglow[1] * intensityBottom0, topglow[2] * intensityBottom0, 1.0f);
					glVertex3f(x1, rBottomZ0, y1);

					glTexCoord2f(tcs[1].u, sliceTopV0);
					glColor4f(topglow[0] * intensityTop0, topglow[1] * intensityTop0, topglow[2] * intensityTop0, 1.0f);
					glVertex3f(x1, rTopZ0, y1);

					glTexCoord2f(tcs[2].u, sliceTopV1);
					glColor4f(topglow[0] * intensityTop1, topglow[1] * intensityTop1, topglow[2] * intensityTop1, 1.0f);
					glVertex3f(x2, rTopZ1, y2);

					glTexCoord2f(tcs[3].u, sliceBottomV1);
					glColor4f(topglow[0] * intensityBottom1, topglow[1] * intensityBottom1, topglow[2] * intensityBottom1, 1.0f);
					glVertex3f(x2, rBottomZ1, y2);
					glEnd();
				}
			}

			glDisable(GL_POLYGON_OFFSET_FILL);
			glDepthMask(true);
			glDepthFunc(GL_EQUAL);
		}

		// Clean hardware recovery matrix gate
		glMatrixMode(GL_MODELVIEW);
		state.SetTextureMode(TM_MODULATE);
		state.ApplyFixedGL1xFunction(nullptr);
	}
	break;

	default:
		break;
	}

	g_isCurrentlyHWWallDrawing = nullptr;
}

//==========================================================================
//
// Collect lights for shader
//
//==========================================================================

// GL3+ analog of RenderLightsCompat
void HWWall::SetupLights(HWDrawInfo*di, FDynLightData &lightdata)
{
	lightdata.Clear();

	if (RenderStyle == STYLE_Add && !di->Level->lightadditivesurfaces) return;	// no lights on additively blended surfaces.

	// check for wall types which cannot have dynamic lights on them (portal types never get here so they don't need to be checked.)
	switch (type)
	{
	case RENDERWALL_FOGBOUNDARY:
	case RENDERWALL_MIRRORSURFACE:
	case RENDERWALL_COLOR:
		return;
	}

	float vtx[]={glseg.x1,zbottom[0],glseg.y1, glseg.x1,ztop[0],glseg.y1, glseg.x2,ztop[1],glseg.y2, glseg.x2,zbottom[1],glseg.y2};
	Plane p;


	auto normal = glseg.Normal();
	p.Set(normal, -normal.X * glseg.x1 - normal.Z * glseg.y1);

	FLightNode *node;
	if (seg->sidedef == NULL)
	{
		node = NULL;
	}
	else if (!(seg->sidedef->Flags & WALLF_POLYOBJ))
	{
		node = seg->sidedef->lighthead;
	}
	else if (sub)
	{
		// Polobject segs cannot be checked per sidedef so use the subsector instead.
		node = sub->section->lighthead;
	}
	else node = NULL;

	// Iterate through all dynamic lights which touch this wall and render them
	while (node)
	{
		if (node->lightsource->IsActive() && !node->lightsource->DontLightMap())
		{
			iter_dlight++;

			DVector3 posrel = node->lightsource->PosRelative(seg->frontsector->PortalGroup);
			float x = posrel.X;
			float y = posrel.Y;
			float z = posrel.Z;
			float dist = fabsf(p.DistToPoint(x, z, y));
			float radius = node->lightsource->GetRadius();
			float scale = 1.0f / ((2.f * radius) - dist);
			FVector3 fn, pos;

			if (radius > 0.f && dist < radius)
			{
				FVector3 nearPt, up, right;

				pos = { x, z, y };
				fn = p.Normal();

				fn.GetRightUp(right, up);

				FVector3 tmpVec = fn * dist;
				nearPt = pos + tmpVec;

				FVector3 t1;
				int outcnt[4]={0,0,0,0};
				texcoord tcs[4];

				// do a quick check whether the light touches this polygon
				for(int i=0;i<4;i++)
				{
					t1 = FVector3(&vtx[i*3]);
					FVector3 nearToVert = t1 - nearPt;
					tcs[i].u = ((nearToVert | right) * scale) + 0.5f;
					tcs[i].v = ((nearToVert | up) * scale) + 0.5f;

					if (tcs[i].u<0) outcnt[0]++;
					if (tcs[i].u>1) outcnt[1]++;
					if (tcs[i].v<0) outcnt[2]++;
					if (tcs[i].v>1) outcnt[3]++;

				}
				if (outcnt[0]!=4 && outcnt[1]!=4 && outcnt[2]!=4 && outcnt[3]!=4) 
				{
					draw_dlight += GetLight(lightdata, seg->frontsector->PortalGroup, p, node->lightsource, true);
				}
			}
		}
		node = node->nextLight;
	}
	dynlightindex = screen->mLights->UploadLights(lightdata);
}


const char HWWall::passflag[] = {
	0,		//RENDERWALL_NONE,             
	1,		//RENDERWALL_TOP,              // unmasked
	1,		//RENDERWALL_M1S,              // unmasked
	2,		//RENDERWALL_M2S,              // depends on render and texture settings
	1,		//RENDERWALL_BOTTOM,           // unmasked
	3,		//RENDERWALL_FOGBOUNDARY,      // translucent
	1,		//RENDERWALL_MIRRORSURFACE,    // only created from PORTALTYPE_MIRROR
	2,		//RENDERWALL_M2SNF,            // depends on render and texture settings, no fog, used on mid texture lines with a fog boundary.
	3,		//RENDERWALL_COLOR,            // translucent
	2,		//RENDERWALL_FFBLOCK           // depends on render and texture settings
};

//==========================================================================
//
// 
//
//==========================================================================
void HWWall::PutWall(HWWallDispatcher *di, bool translucent)
{
	if (texture && texture->GetTranslucency() && passflag[type] == 2)
	{
		translucent = true;
	}

	auto ddi = di->di;
	if (translucent)
	{
		flags |= HWF_TRANSLUCENT;
	}

	if (di->di)
	{
		if (translucent)
		{
			ViewDistance = (ddi->Viewpoint.Pos - (seg->linedef->v1->fPos() + seg->linedef->Delta() / 2)).XY().LengthSquared();
		}

		if (ddi->isFullbrightScene())
		{
			// light planes don't get drawn with fullbright rendering
			if (texture == NULL) return;
			Colormap.Clear();
		}

		if (ddi->isFullbrightScene() || (Colormap.LightColor.isWhite() && lightlevel == 255))
		{
			flags &= ~HWF_GLOW;
		}

		if (!screen->BuffersArePersistent())
		{
			if (ddi->Level->HasDynamicLights && !ddi->isFullbrightScene() && texture != nullptr)
			{
				SetupLights(ddi, lightdata);
			}
			MakeVertices(translucent);
		}



		bool solid;
		if (passflag[type] == 1) solid = true;
		else if (type == RENDERWALL_FFBLOCK) solid = texture && !texture->isMasked();
		else solid = false;

		bool hasDecals = solid && seg->sidedef && seg->sidedef->AttachedDecals;
		if (hasDecals)
		{
			// If we want to use the light infos for the decal we cannot delay the creation until the render pass.
			if (screen->BuffersArePersistent())
			{
				if (ddi->Level->HasDynamicLights && !ddi->isFullbrightScene() && texture != nullptr)
				{
					SetupLights(ddi, lightdata);
				}
			}
			ProcessDecals(ddi);
		}
	}


	di->AddWall(this);

	lightlist = nullptr;
	// make sure that following parts of the same linedef do not get this one's vertex and lighting info.
	vertcount = 0;	
	dynlightindex = -1;
	flags &= ~HWF_TRANSLUCENT;
}

//==========================================================================
//
// 
//
//==========================================================================

void HWWall::PutPortal(HWWallDispatcher *di, int ptype, int plane)
{
	HWPortal * portal = nullptr;

	auto ddi = di->di;
	if (ddi)
	{
		MakeVertices(false);
		switch (ptype)
		{
			// portals don't go into the draw list.
			// Instead they are added to the portal manager
		case PORTALTYPE_HORIZON:
			horizon = portalState.UniqueHorizons.Get(horizon);
			portal = ddi->FindPortal(horizon);
			if (!portal)
			{
				portal = new HWHorizonPortal(&portalState, horizon, ddi->Viewpoint);
				ddi->Portals.Push(portal);
			}
			portal->AddLine(this);
			break;

		case PORTALTYPE_SKYBOX:
			portal = ddi->FindPortal(secportal);
			if (!portal)
			{
				// either a regular skybox or an Eternity-style horizon
				if (secportal->mType != PORTS_SKYVIEWPOINT) portal = new HWEEHorizonPortal(&portalState, secportal);
				else
				{
					portal = new HWSkyboxPortal(&portalState, secportal);
					ddi->Portals.Push(portal);
				}
			}
			portal->AddLine(this);
			break;

		case PORTALTYPE_SECTORSTACK:
			portal = ddi->FindPortal(this->portal);
			if (!portal)
			{
				portal = new HWSectorStackPortal(&portalState, this->portal);
				ddi->Portals.Push(portal);
			}
			portal->AddLine(this);
			break;

		case PORTALTYPE_PLANEMIRROR:
			if (ddi->Viewpoint.bDoOrtho ? (ddi->Viewpoint.ViewVector3D.dot(planemirror->Normal()) < 0)
				: (portalState.PlaneMirrorMode * planemirror->fC() <= 0))
			{
				planemirror = portalState.UniquePlaneMirrors.Get(planemirror);
				portal = ddi->FindPortal(planemirror);
				if (!portal)
				{
					portal = new HWPlaneMirrorPortal(&portalState, planemirror);
					ddi->Portals.Push(portal);
				}
				portal->AddLine(this);
			}
			break;

		case PORTALTYPE_MIRROR:
			portal = ddi->FindPortal(seg->linedef);
			if (!portal)
			{
				portal = new HWMirrorPortal(&portalState, seg->linedef);
				ddi->Portals.Push(portal);
			}
			portal->AddLine(this);
			if (gl_mirror_envmap)
			{
				// draw a reflective layer over the mirror
				ddi->AddMirrorSurface(this);
			}
			break;

		case PORTALTYPE_LINETOLINE:
			if (!lineportal)
				return;
			portal = ddi->FindPortal(lineportal);
			if (!portal)
			{
				line_t* otherside = lineportal->lines[0]->mDestination;
				if (otherside != nullptr && otherside->portalindex < ddi->Level->linePortals.Size())
				{
					ddi->ProcessActorsInPortal(otherside->getPortal()->mGroup, ddi->in_area);
				}
				portal = new HWLineToLinePortal(&portalState, lineportal);
				ddi->Portals.Push(portal);
			}
			portal->AddLine(this);
			break;

		case PORTALTYPE_SKY:
			sky = portalState.UniqueSkies.Get(sky);
			portal = ddi->FindPortal(sky);
			if (!portal)
			{
				portal = new HWSkyPortal(screen->mSkyData, &portalState, sky);
				ddi->Portals.Push(portal);
			}
			portal->AddLine(this);
			break;
		}
		vertcount = 0;

		if (plane != -1 && portal)
		{
			portal->planesused |= (1 << plane);
		}
	}
	else
	{
		portaltype = ptype;
		portalplane = plane;
		di->AddPortal(this);
	}
}

//==========================================================================
//
//	Sets 3D-floor lighting info
//
//==========================================================================

void HWWall::Put3DWall(HWWallDispatcher *di, lightlist_t * lightlist, bool translucent)
{
	// only modify the light di->Level-> if it doesn't originate from the seg's frontsector. This is to account for light transferring effects
	if (lightlist->p_lightlevel != &seg->sidedef->sector->lightlevel)
	{
		lightlevel = hw_ClampLight(*lightlist->p_lightlevel);
	}
	// relative light won't get changed here. It is constant across the entire wall.

	CopyFrom3DLight(Colormap, lightlist);
	PutWall(di, translucent);
}

//==========================================================================
//
//  Splits a wall vertically if a 3D-floor
//	creates different lighting across the wall
//
//==========================================================================

bool HWWall::SplitWallComplex(HWWallDispatcher *di, sector_t * frontsector, bool translucent, float& maplightbottomleft, float& maplightbottomright)
{
	// check for an intersection with the upper plane
	if ((maplightbottomleft<ztop[0] && maplightbottomright>ztop[1]) ||
		(maplightbottomleft>ztop[0] && maplightbottomright<ztop[1]))
	{
		float clen = max<float>(fabsf(glseg.x2 - glseg.x1), fabsf(glseg.y2 - glseg.y1));

		float dch = ztop[1] - ztop[0];
		float dfh = maplightbottomright - maplightbottomleft;
		float coeff = (ztop[0] - maplightbottomleft) / (dfh - dch);

		// check for inaccuracies - let's be a little generous here!
		if (coeff*clen<.1f)
		{
			maplightbottomleft = ztop[0];
		}
		else if (coeff*clen>clen - .1f)
		{
			maplightbottomright = ztop[1];
		}
		else
		{
			// split the wall in two at the intersection and recursively split both halves
			HWWall copyWall1 = *this, copyWall2 = *this;

			copyWall1.glseg.x2 = copyWall2.glseg.x1 = glseg.x1 + coeff * (glseg.x2 - glseg.x1);
			copyWall1.glseg.y2 = copyWall2.glseg.y1 = glseg.y1 + coeff * (glseg.y2 - glseg.y1);
			copyWall1.ztop[1] = copyWall2.ztop[0] = ztop[0] + coeff * (ztop[1] - ztop[0]);
			copyWall1.zbottom[1] = copyWall2.zbottom[0] = zbottom[0] + coeff * (zbottom[1] - zbottom[0]);
			copyWall1.glseg.fracright = copyWall2.glseg.fracleft = glseg.fracleft + coeff * (glseg.fracright - glseg.fracleft);
			copyWall1.tcs[UPRGT].u = copyWall2.tcs[UPLFT].u = tcs[UPLFT].u + coeff * (tcs[UPRGT].u - tcs[UPLFT].u);
			copyWall1.tcs[UPRGT].v = copyWall2.tcs[UPLFT].v = tcs[UPLFT].v + coeff * (tcs[UPRGT].v - tcs[UPLFT].v);
			copyWall1.tcs[LORGT].u = copyWall2.tcs[LOLFT].u = tcs[LOLFT].u + coeff * (tcs[LORGT].u - tcs[LOLFT].u);
			copyWall1.tcs[LORGT].v = copyWall2.tcs[LOLFT].v = tcs[LOLFT].v + coeff * (tcs[LORGT].v - tcs[LOLFT].v);
			copyWall1.lightuv[UPRGT].u = copyWall2.lightuv[UPLFT].u = lightuv[UPLFT].u + coeff * (lightuv[UPRGT].u - lightuv[UPLFT].u);
			copyWall1.lightuv[UPRGT].v = copyWall2.lightuv[UPLFT].v = lightuv[UPLFT].v + coeff * (lightuv[UPRGT].v - lightuv[UPLFT].v);
			copyWall1.lightuv[LORGT].u = copyWall2.lightuv[LOLFT].u = lightuv[LOLFT].u + coeff * (lightuv[LORGT].u - lightuv[LOLFT].u);
			copyWall1.lightuv[LORGT].v = copyWall2.lightuv[LOLFT].v = lightuv[LOLFT].v + coeff * (lightuv[LORGT].v - lightuv[LOLFT].v);

			copyWall1.SplitWall(di, frontsector, translucent);
			copyWall2.SplitWall(di, frontsector, translucent);
			return true;
		}
	}

	// check for an intersection with the lower plane
	if ((maplightbottomleft<zbottom[0] && maplightbottomright>zbottom[1]) ||
		(maplightbottomleft>zbottom[0] && maplightbottomright<zbottom[1]))
	{
		float clen = max<float>(fabsf(glseg.x2 - glseg.x1), fabsf(glseg.y2 - glseg.y1));

		float dch = zbottom[1] - zbottom[0];
		float dfh = maplightbottomright - maplightbottomleft;
		float coeff = (zbottom[0] - maplightbottomleft) / (dfh - dch);

		// check for inaccuracies - let's be a little generous here because there's
		// some conversions between floats and fixed_t's involved
		if (coeff*clen<.1f)
		{
			maplightbottomleft = zbottom[0];
		}
		else if (coeff*clen>clen - .1f)
		{
			maplightbottomright = zbottom[1];
		}
		else
		{
			// split the wall in two at the intersection and recursively split both halves
			HWWall copyWall1 = *this, copyWall2 = *this;

			copyWall1.glseg.x2 = copyWall2.glseg.x1 = glseg.x1 + coeff * (glseg.x2 - glseg.x1);
			copyWall1.glseg.y2 = copyWall2.glseg.y1 = glseg.y1 + coeff * (glseg.y2 - glseg.y1);
			copyWall1.ztop[1] = copyWall2.ztop[0] = ztop[0] + coeff * (ztop[1] - ztop[0]);
			copyWall1.zbottom[1] = copyWall2.zbottom[0] = zbottom[0] + coeff * (zbottom[1] - zbottom[0]);
			copyWall1.glseg.fracright = copyWall2.glseg.fracleft = glseg.fracleft + coeff * (glseg.fracright - glseg.fracleft);
			copyWall1.tcs[UPRGT].u = copyWall2.tcs[UPLFT].u = tcs[UPLFT].u + coeff * (tcs[UPRGT].u - tcs[UPLFT].u);
			copyWall1.tcs[UPRGT].v = copyWall2.tcs[UPLFT].v = tcs[UPLFT].v + coeff * (tcs[UPRGT].v - tcs[UPLFT].v);
			copyWall1.tcs[LORGT].u = copyWall2.tcs[LOLFT].u = tcs[LOLFT].u + coeff * (tcs[LORGT].u - tcs[LOLFT].u);
			copyWall1.tcs[LORGT].v = copyWall2.tcs[LOLFT].v = tcs[LOLFT].v + coeff * (tcs[LORGT].v - tcs[LOLFT].v);
			copyWall1.lightuv[UPRGT].u = copyWall2.lightuv[UPLFT].u = lightuv[UPLFT].u + coeff * (lightuv[UPRGT].u - lightuv[UPLFT].u);
			copyWall1.lightuv[UPRGT].v = copyWall2.lightuv[UPLFT].v = lightuv[UPLFT].v + coeff * (lightuv[UPRGT].v - lightuv[UPLFT].v);
			copyWall1.lightuv[LORGT].u = copyWall2.lightuv[LOLFT].u = lightuv[LOLFT].u + coeff * (lightuv[LORGT].u - lightuv[LOLFT].u);
			copyWall1.lightuv[LORGT].v = copyWall2.lightuv[LOLFT].v = lightuv[LOLFT].v + coeff * (lightuv[LORGT].v - lightuv[LOLFT].v);

			copyWall1.SplitWall(di, frontsector, translucent);
			copyWall2.SplitWall(di, frontsector, translucent);
			return true;
		}
	}

	return false;
}

void HWWall::SplitWall(HWWallDispatcher *di, sector_t * frontsector, bool translucent)
{
	float maplightbottomleft;
	float maplightbottomright;
	unsigned int i;
	int origlight = lightlevel;
	FColormap origcm=Colormap;

	TArray<lightlist_t> & lightlist=frontsector->e->XFloor.lightlist;

	if (glseg.x1==glseg.x2 && glseg.y1==glseg.y2)
	{
		return;
	}
	//::SplitWall.Clock();

#ifdef _DEBUG
	if (seg->linedef->Index() == 1)
	{
		int a = 0;
	}
#endif

	if (lightlist.Size()>1)
	{
		for(i=0;i<lightlist.Size()-1;i++)
		{
			if (i<lightlist.Size()-1) 
			{
				secplane_t &p = lightlist[i+1].plane;
				maplightbottomleft = p.ZatPoint(glseg.x1,glseg.y1);
				maplightbottomright= p.ZatPoint(glseg.x2,glseg.y2);
			}
			else 
			{
				maplightbottomright = maplightbottomleft = -32000;
			}

			// The light is completely above the wall!
			if (maplightbottomleft>=ztop[0] && maplightbottomright>=ztop[1])
			{
				continue;
			}

			// check for an intersection with the upper and lower planes of the wall segment
			if ((maplightbottomleft<ztop[0] && maplightbottomright>ztop[1]) ||
				(maplightbottomleft > ztop[0] && maplightbottomright < ztop[1]) ||
				(maplightbottomleft<zbottom[0] && maplightbottomright>zbottom[1]) ||
				(maplightbottomleft > zbottom[0] && maplightbottomright < zbottom[1]))
			{
				if (!(screen->hwcaps & RFL_NO_CLIP_PLANES))
				{
					// Use hardware clipping if this cannot be done cleanly.
					this->lightlist = &lightlist;
					PutWall(di, translucent);

					goto out;
				}
				// crappy fallback if no clip planes available
				else if (SplitWallComplex(di, frontsector, translucent, maplightbottomleft, maplightbottomright))
				{
					goto out;
				}
			}

			// 3D floor is completely within this light
			if (maplightbottomleft<=zbottom[0] && maplightbottomright<=zbottom[1])
			{
				Put3DWall(di, &lightlist[i], translucent);
				goto out;
			}

			if (maplightbottomleft<=ztop[0] && maplightbottomright<=ztop[1] &&
				(maplightbottomleft!=ztop[0] || maplightbottomright!=ztop[1]))
			{
				HWWall copyWall1 = *this;

				copyWall1.flags |= HWF_NOSPLITLOWER;
				flags |= HWF_NOSPLITUPPER;
				ztop[0]=copyWall1.zbottom[0]=maplightbottomleft;
				ztop[1]=copyWall1.zbottom[1]=maplightbottomright;
				tcs[UPLFT].v=copyWall1.tcs[LOLFT].v=copyWall1.tcs[UPLFT].v+ 
					(maplightbottomleft-copyWall1.ztop[0])*(copyWall1.tcs[LOLFT].v-copyWall1.tcs[UPLFT].v)/(zbottom[0]-copyWall1.ztop[0]);
				tcs[UPRGT].v=copyWall1.tcs[LORGT].v=copyWall1.tcs[UPRGT].v+ 
					(maplightbottomright-copyWall1.ztop[1])*(copyWall1.tcs[LORGT].v-copyWall1.tcs[UPRGT].v)/(zbottom[1]-copyWall1.ztop[1]);
				lightuv[UPLFT].v=copyWall1.lightuv[LOLFT].v=copyWall1.lightuv[UPLFT].v+ 
					(maplightbottomleft-copyWall1.ztop[0])*(copyWall1.lightuv[LOLFT].v-copyWall1.lightuv[UPLFT].v)/(zbottom[0]-copyWall1.ztop[0]);
				lightuv[UPRGT].v=copyWall1.lightuv[LORGT].v=copyWall1.lightuv[UPRGT].v+ 
					(maplightbottomright-copyWall1.ztop[1])*(copyWall1.lightuv[LORGT].v-copyWall1.lightuv[UPRGT].v)/(zbottom[1]-copyWall1.ztop[1]);
				copyWall1.Put3DWall(di, &lightlist[i], translucent);
			}
			if (ztop[0]==zbottom[0] && ztop[1]==zbottom[1]) 
			{
				//::SplitWall.Unclock();
				goto out;
			}
		}
	}

	Put3DWall(di, &lightlist[lightlist.Size()-1], translucent);

out:
	lightlevel=origlight;
	Colormap=origcm;
	flags &= ~HWF_NOSPLITUPPER;
	this->lightlist = NULL;
	//::SplitWall.Unclock();
}


//==========================================================================
//
// 
//
//==========================================================================
bool HWWall::DoHorizon(HWWallDispatcher *di, seg_t * seg,sector_t * fs, vertex_t * v1,vertex_t * v2)
{
	HWHorizonInfo hi;
	lightlist_t * light;

	// ZDoom doesn't support slopes in a horizon sector so I won't either!
	ztop[1] = ztop[0] = fs->GetPlaneTexZ(sector_t::ceiling);
	zbottom[1] = zbottom[0] = fs->GetPlaneTexZ(sector_t::floor);

	auto ddi = di->di;
	if (ddi)
	{
		auto vpz = ddi->Viewpoint.Pos.Z;
		if (vpz < fs->GetPlaneTexZ(sector_t::ceiling))
		{
			if (vpz > fs->GetPlaneTexZ(sector_t::floor))
				zbottom[1] = zbottom[0] = vpz;

			if (fs->GetTexture(sector_t::ceiling) == skyflatnum)
			{
				SkyPlane(di, fs, sector_t::ceiling, false);
			}
			else
			{
				hi.plane.GetFromSector(fs, sector_t::ceiling);
				hi.lightlevel = hw_ClampLight(fs->GetCeilingLight());
				hi.colormap = fs->Colormap;
				hi.specialcolor = fs->SpecialColors[sector_t::ceiling];

				if (fs->e->XFloor.ffloors.Size())
				{
					light = P_GetPlaneLight(fs, &fs->ceilingplane, true);

					if (!(fs->GetFlags(sector_t::ceiling) & PLANEF_ABSLIGHTING)) hi.lightlevel = hw_ClampLight(*light->p_lightlevel);
					hi.colormap.CopyLight(light->extra_colormap);
				}

				if (ddi->isFullbrightScene()) hi.colormap.Clear();
				horizon = &hi;
				PutPortal(di, PORTALTYPE_HORIZON, -1);
			}
			ztop[1] = ztop[0] = zbottom[0];
		}

		if (vpz > fs->GetPlaneTexZ(sector_t::floor))
		{
			zbottom[1] = zbottom[0] = fs->GetPlaneTexZ(sector_t::floor);
			if (fs->GetTexture(sector_t::floor) == skyflatnum)
			{
				SkyPlane(di, fs, sector_t::floor, false);
			}
			else
			{
				hi.plane.GetFromSector(fs, sector_t::floor);
				hi.lightlevel = hw_ClampLight(fs->GetFloorLight());
				hi.colormap = fs->Colormap;
				hi.specialcolor = fs->SpecialColors[sector_t::floor];

				if (fs->e->XFloor.ffloors.Size())
				{
					light = P_GetPlaneLight(fs, &fs->floorplane, false);

					if (!(fs->GetFlags(sector_t::floor) & PLANEF_ABSLIGHTING)) hi.lightlevel = hw_ClampLight(*light->p_lightlevel);
					hi.colormap.CopyLight(light->extra_colormap);
				}

				if (ddi->isFullbrightScene()) hi.colormap.Clear();
				horizon = &hi;
				PutPortal(di, PORTALTYPE_HORIZON, -1);
			}
		}
	}
	else
	{
		// we cannot build the real portal yet, the mesh builder just needs a generic 'horizon' portal that needs to be filled in in the render pass.
		PutPortal(di, PORTALTYPE_HORIZON, -1);
	}
	return true;
}

static float ZeroLightmapUVs[8] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };

//==========================================================================
//
// 
//
//==========================================================================
bool HWWall::SetWallCoordinates(seg_t * seg, FTexCoordInfo *tci, float texturetop,
	float topleft, float topright, float bottomleft, float bottomright, float t_ofs, float skew)
{
	//
	//
	// set up texture coordinate stuff
	//
	// 
	float l_ul;
	float texlength;

	if (texture)
	{
		float length = seg->sidedef ? seg->sidedef->TexelLength : Dist2(glseg.x1, glseg.y1, glseg.x2, glseg.y2);

		l_ul = tci->FloatToTexU(tci->TextureOffset(t_ofs));
		texlength = tci->FloatToTexU(length);
	}
	else
	{
		tci = NULL;
		l_ul = 0;
		texlength = 0;
	}

	texcoord* srclightuv;
	if (lightmap && lightmap->Type != ST_NULL)
	{
		srclightuv = (texcoord*)lightmap->TexCoords;
		lindex = (float)lightmap->LightmapNum;
	}
	else
	{
		srclightuv = (texcoord*)ZeroLightmapUVs;
		lindex = -1.0f;
	}

	//
	//
	// set up coordinates for the left side of the polygon
	//
	// check left side for intersections
	if (topleft >= bottomleft)
	{
		// normal case
		ztop[0] = topleft;
		zbottom[0] = bottomleft;

		if (tci)
		{
			tcs[UPLFT].v = tci->FloatToTexV(-ztop[0] + texturetop);
			tcs[LOLFT].v = tci->FloatToTexV(-zbottom[0] + texturetop);
		}

		lightuv[UPLFT].v = srclightuv[UPLFT].v;
		lightuv[LOLFT].v = srclightuv[LOLFT].v;
	}
	else
	{
		// ceiling below floor - clip to the visible part of the wall
		float dch = topright - topleft;
		float dfh = bottomright - bottomleft;
		float inter_x = (bottomleft - topleft) / (dch - dfh);

		float inter_y = topleft + inter_x * dch;

		glseg.x1 = glseg.x1 + inter_x * (glseg.x2 - glseg.x1);
		glseg.y1 = glseg.y1 + inter_x * (glseg.y2 - glseg.y1);
		glseg.fracleft = inter_x;

		zbottom[0] = ztop[0] = inter_y;

		if (tci)
		{
			tcs[LOLFT].v = tcs[UPLFT].v = tci->FloatToTexV(-ztop[0] + texturetop);
		}

		lightuv[UPLFT].v = srclightuv[UPLFT].v + inter_x * (srclightuv[UPRGT].v - srclightuv[UPLFT].v);
		lightuv[LOLFT].v = srclightuv[LOLFT].v + inter_x * (srclightuv[LORGT].v - srclightuv[LOLFT].v);
	}

	//
	//
	// set up coordinates for the right side of the polygon
	//
	// check left side for intersections
	if (topright >= bottomright)
	{
		// normal case
		ztop[1] = topright;
		zbottom[1] = bottomright;

		if (tci)
		{
			tcs[UPRGT].v = tci->FloatToTexV(-ztop[1] + texturetop + skew);
			tcs[LORGT].v = tci->FloatToTexV(-zbottom[1] + texturetop + skew);
		}

		lightuv[UPRGT].v = srclightuv[UPRGT].v;
		lightuv[LORGT].v = srclightuv[LORGT].v;
	}
	else
	{
		// ceiling below floor - clip to the visible part of the wall
		float dch = topright - topleft;
		float dfh = bottomright - bottomleft;
		float inter_x = (bottomleft - topleft) / (dch - dfh);

		float inter_y = topleft + inter_x * dch;

		glseg.x2 = glseg.x1 + inter_x * (glseg.x2 - glseg.x1);
		glseg.y2 = glseg.y1 + inter_x * (glseg.y2 - glseg.y1);
		glseg.fracright = inter_x;

		zbottom[1] = ztop[1] = inter_y;
		if (tci)
		{
			tcs[LORGT].v = tcs[UPRGT].v = tci->FloatToTexV(-ztop[1] + texturetop + skew);
		}

		lightuv[UPRGT].v = srclightuv[UPRGT].v + inter_x * (srclightuv[UPRGT].v - srclightuv[UPLFT].v);
		lightuv[LORGT].v = srclightuv[LORGT].v + inter_x * (srclightuv[LORGT].v - srclightuv[LOLFT].v);
	}

	tcs[UPLFT].u = tcs[LOLFT].u = l_ul + texlength * glseg.fracleft;
	tcs[UPRGT].u = tcs[LORGT].u = l_ul + texlength * glseg.fracright;

	lightuv[UPLFT].u = srclightuv[UPLFT].u + (srclightuv[UPRGT].u - srclightuv[UPLFT].u) * glseg.fracleft;
	lightuv[LOLFT].u = srclightuv[LOLFT].u + (srclightuv[LORGT].u - srclightuv[LOLFT].u) * glseg.fracleft;
	lightuv[UPRGT].u = srclightuv[UPLFT].u + (srclightuv[UPRGT].u - srclightuv[UPLFT].u) * glseg.fracright;
	lightuv[LORGT].u = srclightuv[LOLFT].u + (srclightuv[LORGT].u - srclightuv[LOLFT].u) * glseg.fracright;

	if (texture != NULL)
	{
		bool normalize = false;
		if (texture->isHardwareCanvas()) normalize = true;
		else if (flags & HWF_CLAMPY)
		{
			// for negative scales we can get negative coordinates here.
			normalize = (tcs[UPLFT].v > tcs[LOLFT].v || tcs[UPRGT].v > tcs[LORGT].v);
		}
		if (normalize)
		{
			// we have to shift the y-coordinate from [-1..0] to [0..1] when using texture clamping with a negative scale
			tcs[UPLFT].v += 1.f;
			tcs[UPRGT].v += 1.f;
			tcs[LOLFT].v += 1.f;
			tcs[LORGT].v += 1.f;
		}
	}

	return true;
}

//==========================================================================
//
// Do some tweaks with the texture coordinates to reduce visual glitches
//
//==========================================================================

void HWWall::CheckTexturePosition(FTexCoordInfo *tci)
{
	float sub;

	if (texture->isHardwareCanvas()) return;

	// clamp texture coordinates to a reasonable range.
	// Extremely large values can cause visual problems
	if (tci->mScale.Y > 0)
	{
		if (tcs[UPLFT].v < tcs[UPRGT].v)
		{
			sub = float(xs_FloorToInt(tcs[UPLFT].v));
		}
		else
		{
			sub = float(xs_FloorToInt(tcs[UPRGT].v));
		}
		tcs[UPLFT].v -= sub;
		tcs[UPRGT].v -= sub;
		tcs[LOLFT].v -= sub;
		tcs[LORGT].v -= sub;

		if ((tcs[UPLFT].v == 0.f && tcs[UPRGT].v == 0.f && tcs[LOLFT].v <= 1.f && tcs[LORGT].v <= 1.f) ||
			(tcs[UPLFT].v >= 0.f && tcs[UPRGT].v >= 0.f && tcs[LOLFT].v == 1.f && tcs[LORGT].v == 1.f))
		{
			flags |= HWF_CLAMPY;
		}
	}
	else
	{
		if (tcs[LOLFT].v < tcs[LORGT].v)
		{
			sub = float(xs_FloorToInt(tcs[LOLFT].v));
		}
		else
		{
			sub = float(xs_FloorToInt(tcs[LORGT].v));
		}
		tcs[UPLFT].v -= sub;
		tcs[UPRGT].v -= sub;
		tcs[LOLFT].v -= sub;
		tcs[LORGT].v -= sub;

		if ((tcs[LOLFT].v == 0.f && tcs[LORGT].v == 0.f && tcs[UPLFT].v <= 1.f && tcs[UPRGT].v <= 1.f) ||
			(tcs[LOLFT].v >= 0.f && tcs[LORGT].v >= 0.f && tcs[UPLFT].v == 1.f && tcs[UPRGT].v == 1.f))
		{
			flags |= HWF_CLAMPY;
		}
	}

	// Check if this is marked as a skybox and if so, do the same for x.
	// This intentionally only tests the seg's frontsector.
	if (seg->frontsector->special == GLSector_Skybox)
	{
		sub = (float)xs_FloorToInt(tcs[UPLFT].u);
		tcs[UPLFT].u -= sub;
		tcs[UPRGT].u -= sub;
		tcs[LOLFT].u -= sub;
		tcs[LORGT].u -= sub;
		if ((tcs[UPLFT].u == 0.f && tcs[LOLFT].u == 0.f && tcs[UPRGT].u <= 1.f && tcs[LORGT].u <= 1.f) ||
			(tcs[UPLFT].u >= 0.f && tcs[LOLFT].u >= 0.f && tcs[UPRGT].u == 1.f && tcs[LORGT].u == 1.f))
		{
			flags |= HWF_CLAMPX;
		}
	}
}


static void GetTexCoordInfo(FGameTexture *tex, FTexCoordInfo *tci, side_t *side, int texpos)
{
	tci->GetFromTexture(tex, (float)side->GetTextureXScale(texpos), (float)side->GetTextureYScale(texpos), !!(side->GetLevel()->flags3 & LEVEL3_FORCEWORLDPANNING));
}

//==========================================================================
//
//  Handle one sided walls, upper and lower texture
//
//==========================================================================
void HWWall::DoTexture(HWWallDispatcher *di, int _type,seg_t * seg, int peg,
					   float ceilingrefheight,float floorrefheight,
					   float topleft,float topright,
					   float bottomleft,float bottomright,
					   float v_offset, float skew)
{
	if (topleft<=bottomleft && topright<=bottomright) return;

	// The Vertex values can be destroyed in this function and must be restored aferward!
	HWSeg glsave=glseg;
	float flh=ceilingrefheight-floorrefheight;
	int texpos;
	uint8_t savedflags = flags;

	switch (_type)
	{
	case RENDERWALL_TOP:
		texpos = side_t::top;
		break;
	case RENDERWALL_BOTTOM:
		texpos = side_t::bottom;
		break;
	default:
		texpos = side_t::mid;
		break;
	}

	FTexCoordInfo tci;

	GetTexCoordInfo(texture, &tci, seg->sidedef, texpos);

	type = _type;

	if (seg->sidedef->lightmap && type >= RENDERWALL_TOP && type <= RENDERWALL_BOTTOM)
	{
		lightmap = &seg->sidedef->lightmap[type - RENDERWALL_TOP];
	}
	else
	{
		lightmap = nullptr;
	}

	float floatceilingref = ceilingrefheight + tci.RowOffset(seg->sidedef->GetTextureYOffset(texpos));
	if (peg) floatceilingref += tci.mRenderHeight - flh - v_offset;

	if (!SetWallCoordinates(seg, &tci, floatceilingref, topleft, topright, bottomleft, bottomright, 
							seg->sidedef->GetTextureXOffset(texpos), skew)) return;

	if (seg->linedef->special == Line_Mirror && _type == RENDERWALL_M1S && gl_mirrors && !(di->Level->ib_compatflags & BCOMPATF_NOMIRRORS))
	{
		PutPortal(di, PORTALTYPE_MIRROR, -1);
	}
	else
	{

		CheckTexturePosition(&tci);

		// Add this wall to the render list
		sector_t * sec = sub ? sub->sector : seg->frontsector;
		if (sec->e->XFloor.lightlist.Size()==0 ||  di->isFullbrightScene()) PutWall(di, false);
		else SplitWall(di, sec, false);
	}

	glseg = glsave;
	flags = savedflags;
}


//==========================================================================
//
// 
//
//==========================================================================

void HWWall::DoMidTexture(HWWallDispatcher *di, seg_t * seg, bool drawfogboundary,
						  sector_t * front, sector_t * back,
						  sector_t * realfront, sector_t * realback,
						  float fch1, float fch2, float ffh1, float ffh2,
						  float bch1, float bch2, float bfh1, float bfh2, float zalign, float skew)
								
{
	FTexCoordInfo tci;
	float topleft,bottomleft,topright,bottomright;
	HWSeg glsave=glseg;
	float texturetop, texturebottom;
	bool wrap = (seg->linedef->flags&ML_WRAP_MIDTEX) || (seg->sidedef->Flags&WALLF_WRAP_MIDTEX);
	bool mirrory = false;
	float rowoffset = 0;

	//
	//
	// Get the base coordinates for the texture
	//
	//
	if (texture)
	{
		// Align the texture to the ORIGINAL sector's height!!
		// At this point slopes don't matter because they don't affect the texture's z-position

		GetTexCoordInfo(texture, &tci, seg->sidedef, side_t::mid);
		if (tci.mRenderHeight < 0)
		{
			mirrory = true;
			tci.mRenderHeight = -tci.mRenderHeight;
			tci.mScale.Y = -tci.mScale.Y;
		}
		rowoffset = tci.RowOffset(seg->sidedef->GetTextureYOffset(side_t::mid));
		if ((seg->linedef->flags & ML_DONTPEGBOTTOM) >0)
		{
			texturebottom = max(realfront->GetPlaneTexZ(sector_t::floor), realback->GetPlaneTexZ(sector_t::floor) + zalign) + rowoffset;
			texturetop = texturebottom + tci.mRenderHeight;
		}
		else
		{
			texturetop = min(realfront->GetPlaneTexZ(sector_t::ceiling), realback->GetPlaneTexZ(sector_t::ceiling) + zalign) + rowoffset;
			texturebottom = texturetop - tci.mRenderHeight;
		}
	}
	else texturetop=texturebottom=0;

	//
	//
	// Depending on missing textures and possible plane intersections
	// decide which planes to use for the polygon
	//
	//
	if (realfront!=realback || drawfogboundary || wrap || realfront->GetHeightSec())
	{
		//
		//
		// Set up the top
		//
		//
		auto tex = TexMan.GetGameTexture(seg->sidedef->GetTexture(side_t::top), true);
		if (!tex || !tex->isValid())
		{
			if (front->GetTexture(sector_t::ceiling) == skyflatnum &&
				back->GetTexture(sector_t::ceiling) == skyflatnum && !wrap && skew == 0)
			{
				// intra-sky lines do not clip the texture at all if there's no upper texture.
				topleft = topright = texturetop;
			}
			else
			{
				// texture is missing - use the higher plane
				topleft = max(bch1,fch1);
				topright = max(bch2,fch2);
			}
		}
		else if ((bch1>fch1 || bch2>fch2) && 
				 (seg->frontsector->GetTexture(sector_t::ceiling)!=skyflatnum || seg->backsector->GetTexture(sector_t::ceiling)==skyflatnum)) 
				 // (!((bch1<=fch1 && bch2<=fch2) || (bch1>=fch1 && bch2>=fch2)))
		{
			// Use the higher plane and let the geometry clip the extruding part
			topleft = bch1;
			topright = bch2;
		}
		else
		{
			// But not if there can be visual artifacts.
			topleft = min(bch1,fch1);
			topright = min(bch2,fch2);
		}
		
		//
		//
		// Set up the bottom
		//
		//
		tex = TexMan.GetGameTexture(seg->sidedef->GetTexture(side_t::bottom), true);
		if (!tex || !tex->isValid())
		{
			// texture is missing - use the lower plane
			bottomleft = min(bfh1,ffh1);
			bottomright = min(bfh2,ffh2);
		}
		else if (bfh1<ffh1 || bfh2<ffh2) // (!((bfh1<=ffh1 && bfh2<=ffh2) || (bfh1>=ffh1 && bfh2>=ffh2)))
		{
			// the floor planes intersect. Use the backsector's floor for drawing so that
			// drawing the front sector's plane clips the polygon automatically.
			bottomleft = bfh1;
			bottomright = bfh2;
		}
		else
		{
			// normal case - use the higher plane
			bottomleft = max(bfh1,ffh1);
			bottomright = max(bfh2,ffh2);
		}
		
		//
		//
		// if we don't need a fog sheet let's clip away some unnecessary parts of the polygon
		//
		//
		if (!drawfogboundary && !wrap && skew == 0)
		{
			if (texturetop < topleft && texturetop < topright) topleft = topright = texturetop;
			if (texturebottom > bottomleft && texturebottom > bottomright) bottomleft = bottomright = texturebottom;
		}
	}
	else
	{
		//
		//
		// if both sides of the line are in the same sector and the sector
		// doesn't have a Transfer_Heights special don't clip to the planes
		// Clipping to the planes is not necessary and can even produce
		// unwanted side effects.
		//
		//
		topleft = texturetop;
		topright = texturetop + skew;
		bottomleft = texturebottom;
		bottomright = texturebottom + skew;
	}

	// nothing visible - skip the rest
	if (topleft<=bottomleft && topright<=bottomright) return;


	//
	//
	// set up texture coordinate stuff
	//
	// 
	float t_ofs = seg->sidedef->GetTextureXOffset(side_t::mid);

	if (texture)
	{
		// First adjust the texture offset so that the left edge of the linedef is inside the range [0..1].
		float texwidth = tci.TextureAdjustWidth();

		if (t_ofs >= 0)
		{
			float div = t_ofs / texwidth;
			t_ofs = (div - xs_FloorToInt(div)) * texwidth;
		}
		else
		{
			float div = (-t_ofs) / texwidth;
			t_ofs = texwidth - (div - xs_FloorToInt(div)) * texwidth;
		}


		// Now check whether the linedef is completely within the texture range of [0..1].
		// If so we should use horizontal texture clamping to prevent filtering artifacts
		// at the edges.

		float textureoffset = tci.TextureOffset(t_ofs);
		int righttex = int(textureoffset) + seg->sidedef->TexelLength;
		
		if ((textureoffset == 0 && righttex <= tci.mRenderWidth) ||
			(textureoffset >= 0 && righttex == tci.mRenderWidth))
		{
			flags |= HWF_CLAMPX;
		}
		else
		{
			flags &= ~HWF_CLAMPX;
		}
		if (!wrap)
		{
			flags |= HWF_CLAMPY;
		}
	}
	if (mirrory)
	{
		tci.mRenderHeight = -tci.mRenderHeight;
		tci.mScale.Y = -tci.mScale.Y;
		flags |= HWF_NOSLICE;
	}
	if (seg->linedef->isVisualPortal())
	{
		// mid textures on portal lines need the same offsetting as mid textures on sky lines
		flags |= HWF_SKYHACK;
	}
	SetWallCoordinates(seg, &tci, texturetop, topleft, topright, bottomleft, bottomright, t_ofs, skew);

	//
	//
	// draw fog sheet if required
	//
	// 
	if (drawfogboundary)
	{
		flags |= HWF_NOSPLITUPPER|HWF_NOSPLITLOWER;
		type=RENDERWALL_FOGBOUNDARY;
		auto savetex = texture;
		texture = NULL;
		PutWall(di, true);
		if (!savetex) 
		{
			flags &= ~(HWF_NOSPLITUPPER|HWF_NOSPLITLOWER);
			return;
		}
		texture = savetex;
		type=RENDERWALL_M2SNF;
	}
	else type=RENDERWALL_M2S;

	//
	//
	// set up alpha blending
	//
	// 
	bool sideHasAlpha = seg->sidedef->HasAlpha();
	if (sideHasAlpha || seg->linedef->alpha != 0)
	{
		bool translucent = false;

		switch (seg->linedef->flags& ML_ADDTRANS)//TRANSBITS)
		{
		case 0:
			RenderStyle=STYLE_Translucent;
			alpha = sideHasAlpha ? seg->sidedef->alpha : seg->linedef->alpha;
			translucent =alpha < 1. || (texture && texture->GetTranslucency());
			break;

		case ML_ADDTRANS:
			RenderStyle=STYLE_Add;
			alpha = sideHasAlpha ? seg->sidedef->alpha : seg->linedef->alpha;
			translucent=true;
			break;
		}

		//
		//
		// for textures with large empty areas only the visible parts are drawn.
		// If these textures come too close to the camera they severely affect performance
		// if stacked closely together
		// Recognizing vertical gaps is rather simple and well worth the effort.
		//
		//
		FloatRect *splitrect;
		int v = texture->GetAreas(&splitrect);
		if (seg->frontsector == seg->backsector) flags |= HWF_NOSPLIT;	// we don't need to do vertex splits if a line has both sides in the same sector
		if (v>0 && !drawfogboundary && !(seg->linedef->flags&ML_WRAP_MIDTEX) && !(flags & HWF_NOSLICE) && skew == 0)
		{
			// split the poly!
			int i,t=0;
			float v_factor=(zbottom[0]-ztop[0])/(tcs[LOLFT].v-tcs[UPLFT].v);
			// only split the vertical area of the polygon that does not contain slopes.
			float splittopv = max(tcs[UPLFT].v, tcs[UPRGT].v);
			float splitbotv = min(tcs[LOLFT].v, tcs[LORGT].v);

			// this is split vertically into sections.
			for(i=0;i<v;i++)
			{
				// the current segment is below the bottom line of the splittable area
				// (IOW. the whole wall has been done)
				if (splitrect[i].top>=splitbotv) break;

				float splitbot=splitrect[i].top+splitrect[i].height;

				// the current segment is above the top line of the splittable area
				if (splitbot<=splittopv) continue;

				HWWall split = *this;

				// the top line of the current segment is inside the splittable area
				// use the splitrect's top as top of this segment
				// if not use the top of the remaining polygon
				if (splitrect[i].top>splittopv)
				{
					split.ztop[0]=split.ztop[1]= ztop[0]+v_factor*(splitrect[i].top-tcs[UPLFT].v);
					split.tcs[UPLFT].v=split.tcs[UPRGT].v=splitrect[i].top;
				}
				// the bottom line of the current segment is inside the splittable area
				// use the splitrect's bottom as bottom of this segment
				// if not use the bottom of the remaining polygon
				if (splitbot<=splitbotv)
				{
					split.zbottom[0]=split.zbottom[1]=ztop[0]+v_factor*(splitbot-tcs[UPLFT].v);
					split.tcs[LOLFT].v=split.tcs[LORGT].v=splitbot;
				}
				//
				//
				// Draw the stuff
				//
				//
				if (front->e->XFloor.lightlist.Size()==0 || di->isFullbrightScene()) split.PutWall(di, translucent);
				else split.SplitWall(di, front, translucent);

				t=1;
			}
			render_texsplit+=t;
		}
		else
		{
			//
			//
			// Draw the stuff without splitting
			//
			//
			if (front->e->XFloor.lightlist.Size()==0 || di->isFullbrightScene()) PutWall(di, translucent);
			else SplitWall(di, front, translucent);
		}
		alpha=1.0f;
	}
	// restore some values that have been altered in this function
	glseg=glsave;
	flags&=~(HWF_CLAMPX|HWF_CLAMPY|HWF_NOSPLITUPPER|HWF_NOSPLITLOWER);
	RenderStyle = STYLE_Normal;
}


//==========================================================================
//
// 
//
//==========================================================================
void HWWall::BuildFFBlock(HWWallDispatcher *di, seg_t * seg, F3DFloor * rover, int roverIndex,
	float ff_topleft, float ff_topright,
	float ff_bottomleft, float ff_bottomright)
{
	side_t * mastersd = rover->master->sidedef[0];
	float to;
	lightlist_t * light;
	bool translucent;
	int savelight = lightlevel;
	FColormap savecolor = Colormap;
	float ul;
	float texlength;
	FTexCoordInfo tci;

	lightmap = nullptr;
	if (seg->sidedef == seg->linedef->sidedef[0])
		lightmap = seg->linedef->sidedef[1]->lightmap;
	else
		lightmap = seg->linedef->sidedef[0]->lightmap;

	if (lightmap)
		lightmap += 4 + roverIndex;

	if (rover->flags&FF_FOG)
	{
		if (!di->isFullbrightScene())
		{
			// this may not yet be done
			light = P_GetPlaneLight(rover->target, rover->top.plane, true);
			Colormap.Clear();
			Colormap.LightColor = light->extra_colormap.FadeColor;
			// the fog plane defines the light di->Level->, not the front sector
			lightlevel = hw_ClampLight(*light->p_lightlevel);
			texture = NULL;
			type = RENDERWALL_FFBLOCK;
		}
		else return;
	}
	else
	{

		if (rover->flags&FF_UPPERTEXTURE)
		{
			texture = TexMan.GetGameTexture(seg->sidedef->GetTexture(side_t::top), true);
			if (!texture || !texture->isValid()) return; 
			GetTexCoordInfo(texture, &tci, seg->sidedef, side_t::top);
		}
		else if (rover->flags&FF_LOWERTEXTURE)
		{
			texture = TexMan.GetGameTexture(seg->sidedef->GetTexture(side_t::bottom), true);
			if (!texture || !texture->isValid()) return;
			GetTexCoordInfo(texture, &tci, seg->sidedef, side_t::bottom);
		}
		else
		{
			texture = TexMan.GetGameTexture(mastersd->GetTexture(side_t::mid), true);
			if (!texture || !texture->isValid()) return;
			GetTexCoordInfo(texture, &tci, mastersd, side_t::mid);
		}

		to = (rover->flags&(FF_UPPERTEXTURE | FF_LOWERTEXTURE)) ? 0 : tci.TextureOffset(mastersd->GetTextureXOffset(side_t::mid));

		ul = tci.FloatToTexU(to + tci.TextureOffset(seg->sidedef->GetTextureXOffset(side_t::mid)));

		texlength = tci.FloatToTexU(seg->sidedef->TexelLength);

		tcs[UPLFT].u = tcs[LOLFT].u = ul + texlength * glseg.fracleft;
		tcs[UPRGT].u = tcs[LORGT].u = ul + texlength * glseg.fracright;

		float rowoffset = tci.RowOffset(seg->sidedef->GetTextureYOffset(side_t::mid));
		to = (rover->flags&(FF_UPPERTEXTURE | FF_LOWERTEXTURE)) ?
			0.f : tci.RowOffset(mastersd->GetTextureYOffset(side_t::mid));

		to += rowoffset + rover->top.model->GetPlaneTexZ(rover->top.isceiling);

		tcs[UPLFT].v = tci.FloatToTexV(to - ff_topleft);
		tcs[UPRGT].v = tci.FloatToTexV(to - ff_topright);
		tcs[LOLFT].v = tci.FloatToTexV(to - ff_bottomleft);
		tcs[LORGT].v = tci.FloatToTexV(to - ff_bottomright);
		type = RENDERWALL_FFBLOCK;
		CheckTexturePosition(&tci);

		texcoord* srclightuv;
		if (lightmap && lightmap->Type != ST_NULL)
		{
			srclightuv = (texcoord*)lightmap->TexCoords;
			lindex = (float)lightmap->LightmapNum;
		}
		else
		{
			srclightuv = (texcoord*)ZeroLightmapUVs;
			lindex = -1.0f;
		}

		lightuv[UPLFT].u = srclightuv[UPLFT].u + (srclightuv[UPRGT].u - srclightuv[UPLFT].u) * glseg.fracleft;
		lightuv[LOLFT].u = srclightuv[LOLFT].u + (srclightuv[LORGT].u - srclightuv[LOLFT].u) * glseg.fracleft;
		lightuv[UPRGT].u = srclightuv[UPLFT].u + (srclightuv[UPRGT].u - srclightuv[UPLFT].u) * glseg.fracright;
		lightuv[LORGT].u = srclightuv[LOLFT].u + (srclightuv[LORGT].u - srclightuv[LOLFT].u) * glseg.fracright;

		lightuv[UPLFT].v = srclightuv[UPLFT].v;
		lightuv[UPRGT].v = srclightuv[UPRGT].v;
		lightuv[LOLFT].v = srclightuv[LOLFT].v;
		lightuv[LORGT].v = srclightuv[LORGT].v;
	}

	ztop[0] = ff_topleft;
	ztop[1] = ff_topright;
	zbottom[0] = ff_bottomleft;//-0.001f;
	zbottom[1] = ff_bottomright;

	if (rover->flags&(FF_TRANSLUCENT | FF_ADDITIVETRANS | FF_FOG))
	{
		alpha = rover->alpha / 255.0f;
		RenderStyle = (rover->flags&FF_ADDITIVETRANS) ? STYLE_Add : STYLE_Translucent;
		translucent = true;
		type = texture ? RENDERWALL_M2S : RENDERWALL_COLOR;
	}
	else
	{
		alpha = 1.0f;
		RenderStyle = STYLE_Normal;
		translucent = false;
	}

	sector_t * sec = sub ? sub->sector : seg->frontsector;

	if (sec->e->XFloor.lightlist.Size() == 0 || di->isFullbrightScene()) PutWall(di, translucent);
	else SplitWall(di, sec, translucent);

	alpha = 1.0f;
	lightlevel = savelight;
	Colormap = savecolor;
	flags &= ~HWF_CLAMPY;
	RenderStyle = STYLE_Normal;
}


//==========================================================================
//
// 
//
//==========================================================================

__forceinline void HWWall::GetPlanePos(F3DFloor::planeref *planeref, float &left, float &right)
{
	left=planeref->plane->ZatPoint(vertexes[0]);
	right=planeref->plane->ZatPoint(vertexes[1]);
}

//==========================================================================
//
// 
//
//==========================================================================
void HWWall::InverseFloors(HWWallDispatcher *di, seg_t * seg, sector_t * frontsector,
	float topleft, float topright,
	float bottomleft, float bottomright)
{
	TArray<F3DFloor *> & frontffloors = frontsector->e->XFloor.ffloors;

	for (unsigned int i = 0; i < frontffloors.Size(); i++)
	{
		F3DFloor * rover = frontffloors[i];
		if (rover->flags & FF_THISINSIDE) continue;	// only relevant for software rendering.
		if (!(rover->flags&FF_EXISTS)) continue;
		if (!(rover->flags&FF_RENDERSIDES)) continue;
		if (!(rover->flags&(FF_INVERTSIDES | FF_ALLSIDES))) continue;

		float ff_topleft;
		float ff_topright;
		float ff_bottomleft;
		float ff_bottomright;

		GetPlanePos(&rover->top, ff_topleft, ff_topright);
		GetPlanePos(&rover->bottom, ff_bottomleft, ff_bottomright);

		// above ceiling
		if (ff_bottomleft > topleft && ff_bottomright > topright) continue;

		if (ff_topleft > topleft && ff_topright > topright)
		{
			// the new section overlaps with the previous one - clip it!
			ff_topleft = topleft;
			ff_topright = topright;
		}
		if (ff_bottomleft < bottomleft && ff_bottomright < bottomright)
		{
			ff_bottomleft = bottomleft;
			ff_bottomright = bottomright;
		}
		if (ff_topleft < ff_bottomleft || ff_topright < ff_bottomright) continue;

		BuildFFBlock(di, seg, rover, i, ff_topleft, ff_topright, ff_bottomleft, ff_bottomright);
		topleft = ff_bottomleft;
		topright = ff_bottomright;

		if (topleft <= bottomleft && topright <= bottomright) return;
	}
}

//==========================================================================
//
// 
//
//==========================================================================
void HWWall::ClipFFloors(HWWallDispatcher *di, seg_t * seg, F3DFloor * ffloor, int ffloorIndex, sector_t * frontsector,
	float topleft, float topright,
	float bottomleft, float bottomright)
{
	TArray<F3DFloor *> & frontffloors = frontsector->e->XFloor.ffloors;

	const unsigned int flags = ffloor->flags & (FF_SWIMMABLE | FF_TRANSLUCENT);

	for (unsigned int i = 0; i < frontffloors.Size(); i++)
	{
		F3DFloor * rover = frontffloors[i];
		if (rover->flags & FF_THISINSIDE) continue;	// only relevant for software rendering.
		if (!(rover->flags&FF_EXISTS)) continue;
		if (!(rover->flags&FF_RENDERSIDES)) continue;
		if ((rover->flags&(FF_SWIMMABLE | FF_TRANSLUCENT)) != flags) continue;

		float ff_topleft;
		float ff_topright;
		float ff_bottomleft;
		float ff_bottomright;

		GetPlanePos(&rover->top, ff_topleft, ff_topright);

		// we are completely below the bottom so unless there are some
		// (unsupported) intersections there won't be any more floors that
		// could clip this one.
		if (ff_topleft < bottomleft && ff_topright < bottomright) goto done;

		GetPlanePos(&rover->bottom, ff_bottomleft, ff_bottomright);
		// above top line?
		if (ff_bottomleft > topleft && ff_bottomright > topright) continue;

		// overlapping the top line
		if (ff_topleft >= topleft && ff_topright >= topright)
		{
			// overlapping with the entire range
			if (ff_bottomleft <= bottomleft && ff_bottomright <= bottomright) return;
			else if (ff_bottomleft > bottomleft && ff_bottomright > bottomright)
			{
				topleft = ff_bottomleft;
				topright = ff_bottomright;
			}
			else
			{
				// an intersecting case.
				// proper handling requires splitting but
				// I don't need this right now.
			}
		}
		else if (ff_topleft <= topleft && ff_topright <= topright)
		{
			BuildFFBlock(di, seg, ffloor, ffloorIndex, topleft, topright, ff_topleft, ff_topright);
			if (ff_bottomleft <= bottomleft && ff_bottomright <= bottomright) return;
			topleft = ff_bottomleft;
			topright = ff_bottomright;
		}
		else
		{
			// an intersecting case.
			// proper handling requires splitting but
			// I don't need this right now.
		}
	}

done:
	// if the program reaches here there is one block left to draw
	BuildFFBlock(di, seg, ffloor, ffloorIndex, topleft, topright, bottomleft, bottomright);
}

//==========================================================================
//
// 
//
//==========================================================================
void HWWall::DoFFloorBlocks(HWWallDispatcher *di, seg_t * seg, sector_t * frontsector, sector_t * backsector,
	float fch1, float fch2, float ffh1, float ffh2,
	float bch1, float bch2, float bfh1, float bfh2)

{
	TArray<F3DFloor *> & backffloors = backsector->e->XFloor.ffloors;
	float topleft, topright, bottomleft, bottomright;
	bool renderedsomething = false;

	// if the ceilings intersect use the backsector's height because this sector's ceiling will
	// obstruct the redundant parts.

	if (fch1 < bch1 && fch2 < bch2)
	{
		topleft = fch1;
		topright = fch2;
	}
	else
	{
		topleft = bch1;
		topright = bch2;
	}

	if (ffh1 > bfh1 && ffh2 > bfh2)
	{
		bottomleft = ffh1;
		bottomright = ffh2;
	}
	else
	{
		bottomleft = bfh1;
		bottomright = bfh2;
	}

	for (unsigned int i = 0; i < backffloors.Size(); i++)
	{
		F3DFloor * rover = backffloors[i];
		if (rover->flags & FF_THISINSIDE) continue;	// only relevant for software rendering.
		if (!(rover->flags&FF_EXISTS)) continue;
		if (!(rover->flags&FF_RENDERSIDES) || (rover->flags&FF_INVERTSIDES)) continue;

		float ff_topleft;
		float ff_topright;
		float ff_bottomleft;
		float ff_bottomright;

		GetPlanePos(&rover->top, ff_topleft, ff_topright);
		GetPlanePos(&rover->bottom, ff_bottomleft, ff_bottomright);

		// completely below floor
		if (ff_topleft <= bottomleft && ff_topright <= bottomright) continue;

		// completely above ceiling
		if (ff_bottomleft > topleft && ff_bottomright > topright && !renderedsomething) continue;

		if (ff_topleft > topleft && ff_topright > topright)
		{
			// the new section overlaps with the previous one - clip it!
			ff_topleft = topleft;
			ff_topright = topright;
		}

		// do all inverse floors above the current one it there is a gap between the
		// last 3D floor and this one.
		if (topleft > ff_topleft && topright > ff_topright)
			InverseFloors(di, seg, frontsector, topleft, topright, ff_topleft, ff_topright);

		// if translucent or liquid clip away adjoining parts of the same type of FFloors on the other side
		if (rover->flags&(FF_SWIMMABLE | FF_TRANSLUCENT))
			ClipFFloors(di, seg, rover, i, frontsector, ff_topleft, ff_topright, ff_bottomleft, ff_bottomright);
		else
			BuildFFBlock(di, seg, rover, i, ff_topleft, ff_topright, ff_bottomleft, ff_bottomright);

		topleft = ff_bottomleft;
		topright = ff_bottomright;
		renderedsomething = true;
		if (topleft <= bottomleft && topright <= bottomright) return;
	}

	// draw all inverse floors below the lowest one
	if (frontsector->e->XFloor.ffloors.Size() > 0)
	{
		if (topleft > bottomleft || topright > bottomright)
			InverseFloors(di, seg, frontsector, topleft, topright, bottomleft, bottomright);
	}
}

inline int CalcRelLight(int lightlevel, int orglightlevel, int rel)
{
	if (orglightlevel >= 253)			// with the software renderer fake contrast won't be visible above this.
	{
		return 0;
	}
	else if (lightlevel - rel > 256)	// the brighter part of fake contrast will be clamped so also clamp the darker part by the same amount for better looks
	{
		return 256 - lightlevel + rel;
	}
	else
	{
		return rel;
	}
}

CVAR(Int, topskew, 0, 0)
CVAR(Int, midskew, 0, 0)
CVAR(Int, bottomskew, 0, 0)

//==========================================================================
//
// 
//
//==========================================================================
void HWWall::Process(HWWallDispatcher *di, seg_t *seg, sector_t * frontsector, sector_t * backsector, bool isculled)
{
	vertex_t * v1, *v2;
	float fch1;
	float ffh1;
	float fch2;
	float ffh2;
	float frefz, crefz;
	sector_t * realfront;
	sector_t * realback;
	sector_t * segfront;
	sector_t * segback;

#ifdef _DEBUG
	if (seg->linedef->Index() == 759)
	{
		int a = 0;
	}
#endif

	lightmap = nullptr;

	// note: we always have a valid sidedef and linedef reference when getting here.

	this->seg = seg;
	this->frontsector = frontsector;
	this->backsector = backsector;
	vertindex = 0;
	vertcount = 0;

	if ((seg->sidedef->Flags & WALLF_POLYOBJ) && seg->backsector)
	{
		// Textures on 2-sided polyobjects are aligned to the actual seg's sectors
		segfront = realfront = seg->frontsector;
		segback = realback = seg->backsector;
	}
	else
	{
		// Need these for aligning the textures
		realfront = &di->Level->sectors[frontsector->sectornum];
		realback = backsector ? &di->Level->sectors[backsector->sectornum] : NULL;
		segfront = frontsector;
		segback = backsector;
	}
	frefz = realfront->GetPlaneTexZ(sector_t::floor);
	crefz = realfront->GetPlaneTexZ(sector_t::ceiling);

	if (seg->sidedef == seg->linedef->sidedef[0])
	{
		v1 = seg->linedef->v1;
		v2 = seg->linedef->v2;
	}
	else
	{
		v1 = seg->linedef->v2;
		v2 = seg->linedef->v1;
	}

	if (!(seg->sidedef->Flags & WALLF_POLYOBJ))
	{
		glseg.fracleft = 0;
		glseg.fracright = 1;
		if (gl_seamless)
		{
			if (v1->dirty) v1->RecalcVertexHeights();
			if (v2->dirty) v2->RecalcVertexHeights();
		}
	}
	else	// polyobjects must be rendered per seg.
	{
		if (fabs(v1->fX() - v2->fX()) > fabs(v1->fY() - v2->fY()))
		{
			glseg.fracleft = (seg->v1->fX() - v1->fX()) / (v2->fX() - v1->fX());
			glseg.fracright = (seg->v2->fX() - v1->fX()) / float(v2->fX() - v1->fX());
		}
		else
		{
			glseg.fracleft = (seg->v1->fY() - v1->fY()) / (v2->fY() - v1->fY());
			glseg.fracright = (seg->v2->fY() - v1->fY()) / (v2->fY() - v1->fY());
		}
		v1 = seg->v1;
		v2 = seg->v2;
		flags |= HWF_NOSPLITUPPER | HWF_NOSPLITLOWER;	// seg-splitting not needed for single segs.
	}


	vertexes[0] = v1;
	vertexes[1] = v2;

	glseg.x1 = v1->fX();
	glseg.y1 = v1->fY();
	glseg.x2 = v2->fX();
	glseg.y2 = v2->fY();
	Colormap = frontsector->Colormap;
	flags = 0;
	dynlightindex = -1;
	lightlist = NULL;

	int rel = 0;
	int orglightlevel = hw_ClampLight(frontsector->lightlevel);
	bool foggy = (!Colormap.FadeColor.isBlack() || di->Level->flags&LEVEL_HASFADETABLE);	// fog disables fake contrast

	alpha = 1.0f;
	RenderStyle = STYLE_Normal;
	texture = NULL;


	if (frontsector->GetWallGlow(topglowcolor, bottomglowcolor)) flags |= HWF_GLOW;

	zfloor[0] = ffh1 = segfront->floorplane.ZatPoint(v1);
	zfloor[1] = ffh2 = segfront->floorplane.ZatPoint(v2);
	zceil[0] = fch1 = segfront->ceilingplane.ZatPoint(v1);
	zceil[1] = fch2 = segfront->ceilingplane.ZatPoint(v2);

	if (seg->linedef->special == Line_Horizon)
	{
		SkyNormal(di, frontsector, v1, v2);
		DoHorizon(di, seg, frontsector, v1, v2);
		return;
	}

	if (isculled)
	{
		if (frontsector->GetTexture(sector_t::ceiling) == skyflatnum)
		{
			SkyNormal(di, frontsector, v1, v2);
		}
		else
		{
			texture = TexMan.GetGameTexture(frontsector->GetTexture(sector_t::ceiling), true);
			if (texture && texture->isValid())
			{
				DoTexture(di, RENDERWALL_TOP, seg, true,
					crefz, frefz,
					fch1, fch2, ffh1, ffh2, 0, 0);
			}
		}
		return;
	}

	bool isportal = seg->linedef->isVisualPortal() && seg->sidedef == seg->linedef->sidedef[0];

	//return;
	// [GZ] 3D middle textures are necessarily two-sided, even if they lack the explicit two-sided flag
	if (!backsector || (!(seg->linedef->flags&(ML_TWOSIDED | ML_3DMIDTEX)) && !isportal)) // one sided
	{
		// sector's sky
		SkyNormal(di, frontsector, v1, v2);

		if (isportal)
		{
			lineportal = seg->linedef->getPortal()->mGroup;
			ztop[0] = zceil[0];
			ztop[1] = zceil[1];
			zbottom[0] = zfloor[0];
			zbottom[1] = zfloor[1];
			PutPortal(di, PORTALTYPE_LINETOLINE, -1);
		}
		else if (seg->linedef->GetTransferredPortal())
		{
			SkyLine(di, frontsector, seg->linedef);
		}
		else
		{
			// normal texture
			lightlevel = hw_ClampLight(seg->sidedef->GetLightLevel(foggy, orglightlevel, side_t::mid, false, &rel));
			rellight = CalcRelLight(lightlevel, orglightlevel, rel);
			texture = TexMan.GetGameTexture(seg->sidedef->GetTexture(side_t::mid), true);
			if (texture && texture->isValid())
			{
				int skewflag = seg->sidedef->textures[side_t::mid].skew;
				if (skewflag == 0) skewflag = midskew;
				float skew = 
					skewflag == side_t::skew_front_ceiling ? fch2 - fch1 :
					skewflag == side_t::skew_front_floor ? ffh2 - ffh1 : 0.;
				DoTexture(di, RENDERWALL_M1S, seg, (seg->linedef->flags & ML_DONTPEGBOTTOM) > 0,
					crefz, frefz,	// must come from the original!
					fch1, fch2, ffh1, ffh2, 0, skew);
			}
		}
	}
	else // two sided
	{
		float bfh1 = segback->floorplane.ZatPoint(v1);
		float bfh2 = segback->floorplane.ZatPoint(v2);
		float bch1 = segback->ceilingplane.ZatPoint(v1);
		float bch2 = segback->ceilingplane.ZatPoint(v2);
		float zalign = 0.f;


		if (isportal && seg->backsector == nullptr)
		{
			SkyNormal(di, frontsector, v1, v2); // For sky rendering purposes this needs to be treated as a one-sided wall.

			// If this is a one-sided portal and we got floor or ceiling alignment, the upper/lower texture position needs to be adjusted for that.
			// (We assume that this portal won't involve slopes!)
			switch (seg->linedef->getPortalAlignment())
			{
			case PORG_FLOOR:
				zalign = ffh1 - bfh1;
				bch1 += zalign;
				bch2 += zalign;
				bfh1 += zalign;
				bfh2 += zalign;
				return;

			case PORG_CEILING:
				zalign = fch1 - bch1;
				bch1 += zalign;
				bch2 += zalign;
				bfh1 += zalign;
				bfh2 += zalign;
				return;

			default:
				break;
			}
		}
		else
		{
			SkyTop(di, seg, frontsector, backsector, v1, v2);
			SkyBottom(di, seg, frontsector, backsector, v1, v2);
		}

		// upper texture
		if (frontsector->GetTexture(sector_t::ceiling) != skyflatnum || backsector->GetTexture(sector_t::ceiling) != skyflatnum)
		{
			float bch1a = bch1, bch2a = bch2;
			if (frontsector->GetTexture(sector_t::floor) != skyflatnum || backsector->GetTexture(sector_t::floor) != skyflatnum)
			{
				// the back sector's floor obstructs part of this wall
				if (ffh1 > bch1 && ffh2 > bch2 && (seg->linedef->flags & ML_DRAWFULLHEIGHT) == 0)
				{
					bch2a = ffh2;
					bch1a = ffh1;
				}
			}

			if (bch1a < fch1 || bch2a < fch2)
			{
				lightlevel = hw_ClampLight(seg->sidedef->GetLightLevel(foggy, orglightlevel, side_t::top, false, &rel));
				rellight = CalcRelLight(lightlevel, orglightlevel, rel);
				texture = TexMan.GetGameTexture(seg->sidedef->GetTexture(side_t::top), true);
				if (texture && texture->isValid())
				{
					int skewflag = seg->sidedef->textures[side_t::top].skew;
					float skew;
					if (skewflag == 0) skewflag = topskew;

					switch (skewflag)
					{
					default:
						skew = 0;
						break;
					case side_t::skew_front_ceiling:
						skew = fch2 - fch1;
						break;
					case side_t::skew_back_ceiling:
						skew = bch2 - bch1;
						break;
					case side_t::skew_front_floor:
						skew = ffh2 - ffh1;
						break;
					case side_t::skew_back_floor:
						skew = bfh2 - bfh1;
						break;
					}
					DoTexture(di, RENDERWALL_TOP, seg, (seg->linedef->flags & (ML_DONTPEGTOP)) == 0,
						crefz, realback->GetPlaneTexZ(sector_t::ceiling),
						fch1, fch2, bch1a, bch2a, 0, skew);
				}
				else if (!(seg->sidedef->Flags & WALLF_POLYOBJ))
				{
					if ((frontsector->ceilingplane.isSlope() || backsector->ceilingplane.isSlope()) &&
						frontsector->GetTexture(sector_t::ceiling) != skyflatnum &&
						backsector->GetTexture(sector_t::ceiling) != skyflatnum)
					{
						texture = TexMan.GetGameTexture(frontsector->GetTexture(sector_t::ceiling), true);
						if (texture && texture->isValid())
						{
							DoTexture(di, RENDERWALL_TOP, seg, (seg->linedef->flags & (ML_DONTPEGTOP)) == 0,
								crefz, realback->GetPlaneTexZ(sector_t::ceiling),
								fch1, fch2, bch1a, bch2a, 0, 0);
						}
					}
					else
					{
						// skip processing if the back is a malformed subsector
						if (seg->PartnerSeg != NULL && !(seg->PartnerSeg->Subsector->hacked & 4))
						{
							di->AddUpperMissingTexture(seg->sidedef, sub, bch1a);
						}
					}
				}
			}
		}


		/* mid texture */
		sector_t *backsec = isportal? seg->linedef->getPortalDestination()->frontsector : backsector;

		bool drawfogboundary = !di->isFullbrightScene() && CheckFog(di->Level, frontsector, backsec, di->lightmode);
		auto tex = TexMan.GetGameTexture(seg->sidedef->GetTexture(side_t::mid), true);
		if (tex != NULL && tex->isValid())
		{
			if (di->Level->i_compatflags & COMPATF_MASKEDMIDTEX)
			{
				auto rawtexid = TexMan.GetRawTexture(tex->GetID());
				auto rawtex = TexMan.GetGameTexture(rawtexid);
				if (rawtex) tex = rawtex;
			}
			texture = tex;
		}
		else texture = nullptr;
		lightlevel = hw_ClampLight(seg->sidedef->GetLightLevel(foggy, orglightlevel, side_t::mid, false, &rel));
		rellight = CalcRelLight(lightlevel, orglightlevel, rel);

		float skew;
		int skewflag = seg->sidedef->textures[side_t::mid].skew;
		if (skewflag == 0) skewflag = midskew;
		switch (skewflag)
		{
		default:
			skew = 0;
			break;
		case side_t::skew_front_ceiling:
			skew = fch2 - fch1;
			break;
		case side_t::skew_back_ceiling:
			skew = bch2 - bch1;
			break;
		case side_t::skew_front_floor:
			skew = ffh2 - ffh1;
			break;
		case side_t::skew_back_floor:
			skew = bfh2 - bfh1;
			break;
		}

		if (isportal)
		{
			lineportal = seg->linedef->getPortal()->mGroup;
			ztop[0] = bch1;
			ztop[1] = bch2;
			zbottom[0] = bfh1;
			zbottom[1] = bfh2;
			PutPortal(di, PORTALTYPE_LINETOLINE, -1);

			if (texture && seg->backsector != nullptr)
			{
				DoMidTexture(di, seg, drawfogboundary, frontsector, backsector, realfront, realback,
					fch1, fch2, ffh1, ffh2, bch1, bch2, bfh1, bfh2, zalign, skew);
			}
		}
		else
		{

			if (texture || drawfogboundary)
			{
				DoMidTexture(di, seg, drawfogboundary, frontsector, backsector, realfront, realback,
					fch1, fch2, ffh1, ffh2, bch1, bch2, bfh1, bfh2, zalign, skew);
			}

			if (backsector->e->XFloor.ffloors.Size() || frontsector->e->XFloor.ffloors.Size())
			{
				lightlevel = hw_ClampLight(seg->sidedef->GetLightLevel(foggy, orglightlevel, side_t::top, false, &rel));
				rellight = CalcRelLight(lightlevel, orglightlevel, rel);
				DoFFloorBlocks(di, seg, frontsector, backsector, fch1, fch2, ffh1, ffh2, bch1, bch2, bfh1, bfh2);
			}
		}

		/* bottom texture */
		// the back sector's ceiling obstructs part of this wall (specially important for sky sectors)
		float bfh1a = bfh1, bfh2a = bfh2;
		if (fch1 < bfh1 && fch2 < bfh2 && (seg->linedef->flags & ML_DRAWFULLHEIGHT) == 0)
		{
			bfh1 = fch1;
			bfh2 = fch2;
		}

		if (bfh1 > ffh1 || bfh2 > ffh2)
		{
			lightlevel = hw_ClampLight(seg->sidedef->GetLightLevel(foggy, orglightlevel, side_t::bottom, false, &rel));
			rellight = CalcRelLight(lightlevel, orglightlevel, rel);
			texture = TexMan.GetGameTexture(seg->sidedef->GetTexture(side_t::bottom), true);
			if (texture && texture->isValid())
			{
				int skewflag = seg->sidedef->textures[side_t::bottom].skew;
				if (skewflag == 0) skewflag = bottomskew;
				float skew;
				switch (skewflag)
				{
				default:
					skew = 0;
					break;
				case side_t::skew_front_ceiling:
					skew = fch2 - fch1;
					break;
				case side_t::skew_back_ceiling:
					skew = bch2 - bch1;
					break;
				case side_t::skew_front_floor:
					skew = ffh2 - ffh1;
					break;
				case side_t::skew_back_floor:
					skew = bfh2a - bfh1a;
					break;
				}

				DoTexture(di, RENDERWALL_BOTTOM, seg, (seg->linedef->flags & ML_DONTPEGBOTTOM) > 0,
					realback->GetPlaneTexZ(sector_t::floor), frefz,
					bfh1, bfh2, ffh1, ffh2,
					frontsector->GetTexture(sector_t::ceiling) == skyflatnum && backsector->GetTexture(sector_t::ceiling) == skyflatnum ?
					frefz - realback->GetPlaneTexZ(sector_t::ceiling) :
					frefz - crefz, skew);
			}
			else if (!(seg->sidedef->Flags & WALLF_POLYOBJ))
			{
				if ((frontsector->floorplane.isSlope() || backsector->floorplane.isSlope()) &&
					frontsector->GetTexture(sector_t::floor) != skyflatnum &&
					backsector->GetTexture(sector_t::floor) != skyflatnum)
				{
					// render it anyway with the sector's floor texture. With a background sky
					// there are ugly holes otherwise and slopes are simply not precise enough
					// to mach in any case.
					texture = TexMan.GetGameTexture(frontsector->GetTexture(sector_t::floor), true);
					if (texture && texture->isValid())
					{
						DoTexture(di, RENDERWALL_BOTTOM, seg, (seg->linedef->flags & ML_DONTPEGBOTTOM) > 0,
							realback->GetPlaneTexZ(sector_t::floor), frefz,
							bfh1, bfh2, ffh1, ffh2, frefz - crefz, 0);
					}
				}
				else if (backsector->GetTexture(sector_t::floor) != skyflatnum)
				{
					// skip processing if the back is a malformed subsector
					if (seg->PartnerSeg != NULL && !(seg->PartnerSeg->Subsector->hacked & 4))
					{
						di->AddLowerMissingTexture(seg->sidedef, sub, bfh1);
					}
				}
			}
		}
	}
}

//==========================================================================
//
// 
//
//==========================================================================
void HWWall::ProcessLowerMiniseg(HWWallDispatcher *di, seg_t *seg, sector_t * frontsector, sector_t * backsector)
{
	if (frontsector->GetTexture(sector_t::floor) == skyflatnum) return;
	lightlist = NULL;

	float ffh = frontsector->GetPlaneTexZ(sector_t::floor);
	float bfh = backsector->GetPlaneTexZ(sector_t::floor);

	vertindex = 0;
	vertcount = 0;
	if (bfh > ffh)
	{
		this->seg = seg;
		this->frontsector = frontsector;
		this->backsector = backsector;
		this->sub = NULL;
		this->lightmap = nullptr;

		vertex_t * v1 = seg->v1;
		vertex_t * v2 = seg->v2;
		vertexes[0] = v1;
		vertexes[1] = v2;

		glseg.x1 = v1->fX();
		glseg.y1 = v1->fY();
		glseg.x2 = v2->fX();
		glseg.y2 = v2->fY();
		glseg.fracleft = 0;
		glseg.fracright = 1;

		flags = 0;

		// can't do fake contrast without a sidedef
		lightlevel = hw_ClampLight(frontsector->lightlevel);
		rellight = 0;

		alpha = 1.0f;
		RenderStyle = STYLE_Normal;
		Colormap = frontsector->Colormap;

		if (frontsector->GetWallGlow(topglowcolor, bottomglowcolor)) flags |= HWF_GLOW;
		dynlightindex = -1;

		zfloor[0] = zfloor[1] = ffh;

		texture = TexMan.GetGameTexture(frontsector->GetTexture(sector_t::floor), true);

		if (texture && texture->isValid())
		{
			FTexCoordInfo tci;
			type = RENDERWALL_BOTTOM;
			tci.GetFromTexture(texture, 1, 1, false);
			SetWallCoordinates(seg, &tci, bfh, bfh, bfh, ffh, ffh, 0, 0);
			PutWall(di, false);
		}
	}
}
