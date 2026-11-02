// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2005-2016 Christoph Oelckers
// Copyright(C) 2026 Vadim Taranov
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

// gl_20.cpp
// *** Fallback OpenGL rendering code for ancient hardware.
// OpenGL v1.1 is required (1997 cards?), the same file for GL2x path.
// The difference GL2 makes is no blurry textures thanks to NPOT support.
// 
// *** Initially, it's a GL1.3 implement. made by Graf Zahl (or somebody else),
// that was wrapped around a GL2 (thus it required GL2 surprisingly).
// That's why LZDoom 3.88b (GZDoom 3.3 fork) required at least GL2 card.
// GL1 support was added in LZDoom07 mainly by editing gl_postprocessstate.cpp,
// by writing a special conversion struct:
// 
// 	// Maps blend modes to a single (Src, Dest) pair required by 
//                                             glBlendFunc (GL1.x)
// struct GL1BlendFuncEntry
// {
// 	int blendSrcRgb;
// 	int blendDestRgb;
// 	int mapSrcRgb;    // The blendSrcRgb  we are looking up
// 	int mapDestRgb;   // The blendDestRgb we are looking up
// };
// 
// ...then npot support was added by resizing all npot textures
// to the square ones via bilinear interpolation
// but there must be some other better, yet undiscovered ways
//

#include "gl_20.h"
#include "common/rendering/hwrenderer/data/hw_dynlightcache.h"
#include "rendering/hwrenderer/scene/hw_drawstructs.h"
#include "rendering/hwrenderer/scene/hw_walldispatcher.h"
#include "common/rendering/a_spriteocclusion3d.h"

CVAR(Bool, gl_legacy_dynlight_baked_huge, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
FGLBSPCache g_BSPRenderCache;

CVAR(Bool, gl_lights_additive, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, gl_legacy_mode, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOSET)

CVAR(Bool, gl_legacy_dynlight_compress_range, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, gl_legacy_dynlight_brightness, 1.8f, CVAR_ARCHIVE)
CVAR(Int, gl_legacy_dynlight_saturation_thresh, 90, CVAR_ARCHIVE)     // Dark surface cutoff ceiling (Range: 32 to 192)
CVAR(Float, gl_legacy_dynlight_saturation_dark, 0.2, CVAR_ARCHIVE)    // Color saturation modifier applied to dim/dark surfaces
CVAR(Float, gl_legacy_dynlight_saturation_bright, 0.5f, CVAR_ARCHIVE) // Color saturation modifier applied to bright surfaces
CVAR(Float, gl_legacy_dynlight_hue_shift, 22.4f, CVAR_ARCHIVE) // Shift dynamic light hue (-180.0 to 180.0). 0 = Disabled.

CVAR(Bool, gl_legacy_dynlight_overbright, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) // Dynlight overbright global switch used in gl_scene.cpp
CVAR(Float, gl_legacy_dynlight_overbright_flats, 0.125f, CVAR_ARCHIVE) // Intensity multiplier for flats
CVAR(Float, gl_legacy_dynlight_overbright_walls, 0.125f, CVAR_ARCHIVE) // Intensity multiplier for walls

// Enables the zscript camglow dynlight spawn (why not to keep it for all renderers GL1,2,3,4, Software, Polyrenderer?)
CVAR(Bool, gl_camglowlight, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// Is here only for lightmode toggling as fake contrast for GL1x/GL2x is entirely done in p_sectors.cpp, "GetLightLevel".
EXTERN_CVAR(Int, r_fakecontrast)

// ======================================  for GL1x/GL2x modes only ======================================
HWFlat* g_isCurrentlyGL1xDynlightFlatDrawing = nullptr;
HWWall* g_isCurrentlyGL1xDynlightWallDrawing = nullptr;
bool g_isCurrentlyGL1xFlatsDynlightOverbrightPass = false;
bool g_isCurrentlyGL1xWallsDynlightOverbrightPass = false;
bool g_isGL1xDynlightAcamglowOnFlat = false;
bool g_isGL1xDynlightAcamglowOnWall = false;
bool isTrulyTranslucentGL1xWall = false;
bool isTrulyTranslucentGL1xFlat = false;
// =======================================================================================================
extern HWFlat* g_isCurrentlyHWFlatDrawing; // for all HW modes (in hw_flats.cpp, DrawFlat method)
extern HWWall* g_isCurrentlyHWWallDrawing; // for all HW modes (in hw_walls.cpp, DrawWall method)

extern bool s_WasFakeContrastForcedByGL1x, s_WasCamglowDynlightForcedByGL1x;
extern bool s_WasCamglowDynlightForcedByShaderGL3x, s_WereAllGLDynlightsForcedByGL1x;
extern int s_SavedFakeContrastValForcedByGL1x, s_SavedCamglowDynlightValForcedByGL1x;
extern int s_SavedCamglowDynlightValForcedByShaderGL3x, s_SavedAllGLDynlightsValForcedByGL1x;

float distLight2Wall, distLight2Flat, radiusWalls, radiusFlats = 0.0f;
bool isVisibleBehind2Sobstr, isVisibleBehind3DFloorFlats, isVisibleBehind3DFloorWalls = false;


//==========================================================================
//
// Do some tinkering with the menus so that certain options only appear
// when they are actually valid.
//
//==========================================================================

void gl_PatchMenu()
{
	// ---=== PATHWAY A: GL1x/GL2x LEGACY FIXED-FUNCTION CONTEXT ===---
	if (gl.legacyMode)
	{
		if (gl_lightmode == 2 || gl_lightmode == 8 || gl_lightmode == 16)
		{
			// 1. Handle r_fakecontrast setup on startup
			if (!s_WasFakeContrastForcedByGL1x)
			{
				s_SavedFakeContrastValForcedByGL1x = r_fakecontrast;
				if (r_fakecontrast == 0)
				{
					r_fakecontrast = 1;
				}
				s_WasFakeContrastForcedByGL1x = true;
			}

			// 2. Handle gl_camglowlight setup on startup
			if (!s_WasCamglowDynlightForcedByGL1x)
			{
				s_SavedCamglowDynlightValForcedByGL1x = gl_camglowlight;
				if (gl_camglowlight == 0)
				{
					gl_camglowlight = 1;
				}
				s_WasCamglowDynlightForcedByGL1x = true;
			}

			// 3. Handle gl_lights safe fallback setup on startup
			if (!s_WereAllGLDynlightsForcedByGL1x)
			{
				s_SavedAllGLDynlightsValForcedByGL1x = gl_lights;
				if (gl_lights == 0)
				{
					gl_lights = 1;
				}
				s_WereAllGLDynlightsForcedByGL1x = true;
			}
		}
	}
	// ---=== PATHWAY B: GL3x/GL4x MODERN SHADER CONTEXT ===---
	else
	{
		if (gl_lightmode == 2 || gl_lightmode == 8 || gl_lightmode == 16)
		{
			// Safely force gl_camglowlight OFF (0) at startup for shader-lit modes, 
			// but cache their preference so it can be un-isolated dynamically later
			if (!s_WasCamglowDynlightForcedByShaderGL3x)
			{
				s_SavedCamglowDynlightValForcedByShaderGL3x = gl_camglowlight;
				if (gl_camglowlight != 0)
				{
					gl_camglowlight = 0;
				}
				s_WasCamglowDynlightForcedByShaderGL3x = true;
			}
		}
	}

	// Dynamic menu pruning below remains unmodified (only runs if legacyMode is true)
	if (!gl.legacyMode) return;

	//FOptionValues **opt = OptionValues.CheckKey("LightingModes");
	//if (opt != NULL)
	//{
	//	for (int i = (*opt)->mValues.Size() - 1; i >= 0; i--)
	//	{
	//		// Delete 'Doom' lighting mode
	//		if ((*opt)->mValues[i].Value == 2.0 || (*opt)->mValues[i].Value == 8.0 || (*opt)->mValues[i].Value == 16.0)
	//		{
	//			(*opt)->mValues.Delete(i);
	//		}
	//	}
	//}

	// disable features that don't work without shaders.
	//if (gl_lightmode == 2 || gl_lightmode == 8 || gl_lightmode == 16) gl_lightmode = 3;

	// gl_fogmode used to be disabled in GL1x/GL2x legacy path
	// but it affects the way camglow dynlight works
	//opt = OptionValues.CheckKey("FogMode");
	//if (opt != NULL)
	//{
	//	for (int i = (*opt)->mValues.Size() - 1; i >= 0; i--)
	//	{
	//		// Delete 'Radial' fog mode
	//		if ((*opt)->mValues[i].Value == 2.0)
	//		{
	//			(*opt)->mValues.Delete(i);
	//		}
	//	}
	//}
	//if (gl_fogmode == 2) gl_fogmode = 1;

	// remove more unsupported stuff like postprocessing options.
	// This cannot be done with a menu filter because the renderer gets initialized long after the menu is set up.
	DMenuDescriptor **desc = MenuDescriptors.CheckKey("OpenGLOptions");
	if (desc != nullptr && (*desc)->IsKindOf(RUNTIME_CLASS(DOptionMenuDescriptor)))
	{
		auto md = static_cast<DOptionMenuDescriptor*>(*desc);
		for (int i = md->mItems.Size() - 1; i >= 0; i--)
		{
			if (!stricmp(md->mItems[i]->mAction.GetChars(), "gl_multisample") ||
				!stricmp(md->mItems[i]->mAction.GetChars(), "gl_tonemap") ||
				!stricmp(md->mItems[i]->mAction.GetChars(), "gl_bloom") ||
				!stricmp(md->mItems[i]->mAction.GetChars(), "gl_lens") ||
				!stricmp(md->mItems[i]->mAction.GetChars(), "gl_ssao") ||
				!stricmp(md->mItems[i]->mAction.GetChars(), "gl_ssao_portals") ||
				!stricmp(md->mItems[i]->mAction.GetChars(), "gl_fxaa") ||
				!stricmp(md->mItems[i]->mAction.GetChars(), "gl_paltonemap_powtable") ||
				!stricmp(md->mItems[i]->mAction.GetChars(), "vr_mode") ||
				!stricmp(md->mItems[i]->mAction.GetChars(), "vr_enable_quadbuffered") ||
				!stricmp(md->mItems[i]->mAction.GetChars(), "gl_paltonemap_reverselookup"))
			{
				md->mItems.Delete(i);
			}
		}
	}
}

//==========================================================================
//
//
//
//==========================================================================

// It looks like textures modes function is similar to PRBoom.
void gl_SetTextureMode(int type)
{
	// Fallback for old GL v1.1 (no GL_COMBINE extension available)
	if (gl.gl1path && gl.gl1_v1dot1)
	{
		if (type == TM_MASK || type == TM_OPAQUE || type == TM_INVERSE || 
			type == TM_INVERTOPAQUE || type == TM_BRIGHTMAP_LEGACY)
		{
			glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		}
		else
		{
			glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		}
		return; // Bypass original OpenGL 1.3+ texture unit configurations
	}

	// Original GL1.3+ (with GL_COMBINE support)
	if (type == TM_MASK)
	{
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_REPLACE);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_PRIMARY_COLOR);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);

		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_MODULATE);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PRIMARY_COLOR);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_ALPHA, GL_TEXTURE0);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_ALPHA, GL_SRC_ALPHA);
	}
	else if (type == TM_OPAQUE)
	{
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE0);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PRIMARY_COLOR);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);

		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PRIMARY_COLOR);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
	}
	else if (type == TM_INVERSE)
	{
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE0);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PRIMARY_COLOR);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_ONE_MINUS_SRC_COLOR);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);

		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_MODULATE);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PRIMARY_COLOR);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_ALPHA, GL_TEXTURE0);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_ALPHA, GL_SRC_ALPHA);
	}
	else if (type == TM_INVERTOPAQUE)
	{
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE0);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PRIMARY_COLOR);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_ONE_MINUS_SRC_COLOR);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);

		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PRIMARY_COLOR);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
	}
	else if (type == TM_BRIGHTMAP_LEGACY)
	{
		// Turn modulation on: brightmap texture will be multiplied by glColor
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		//glTexEnvi(GL_TEXTURE_ENV, GL_RGB_SCALE, 1);
	}
	else // if (type == TM_MODULATE)
	{
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	}
}

//==========================================================================
//
//
//
//==========================================================================

static int ffTextureMode;
static bool ffTextureEnabled;
static bool ffFogEnabled;
static PalEntry ffFogColor;
static int ffSpecialEffect;
static float ffFogDensity;
static bool currentTextureMatrixState;
static bool currentModelMatrixState;

extern int modellightindex; // to distinguish between different dynlight types

//extern bool isUsingVolumetric3DModelLegacyDynlight; // resides in gl_spritelight.cpp but not ported yet!
bool isUsingVolumetric3DModelLegacyDynlight = false;


void FRenderState::ApplyFixedGL1xFunction(AActor *actor)
{
	// [Darkcrafter07] - Maintain proper context layout mapping for 4.14dev
	int thistm = mTextureMode == TM_MODULATE && mTempTM == TM_OPAQUE ? TM_OPAQUE : mTextureMode;
	if (thistm != ffTextureMode)
	{
		ffTextureMode = thistm;
		if (ffTextureMode == TM_CLAMPY) ffTextureMode = TM_MODULATE;
		gl_SetTextureMode(ffTextureMode);
	}

	if (mTextureEnabled != ffTextureEnabled)
	{
		if ((ffTextureEnabled = mTextureEnabled)) glEnable(GL_TEXTURE_2D);
		else glDisable(GL_TEXTURE_2D);
	}

	if (mFogEnabled != ffFogEnabled)
	{
		if ((ffFogEnabled = mFogEnabled)) glEnable(GL_FOG);
		else glDisable(GL_FOG);
	}

	// //if (mFogEnabled) // old block, not very cool
	// //I
	// //	if (ffFogColor != mFogColor)
	// //	{
	// //		ffFogColor = mFogColor;
	// //		GLfloat FogColor[4] = { mFogColor.r / 255.0f,mFogColor.g / 255.0f,mFogColor.b / 255.0f,0.0f };
	// //		glFogfv(GL_FOG_COLOR, FogColor);
	// //	}
	// //	if (ffFogDensity != mLightParms[2])
	// //	{
	// //		glFogf(GL_FOG_DENSITY, mLightParms[2] * -0.6931471f); // = 1/log(2)
	// //		ffFogDensity = mLightParms[2];
	// //	}
	// //}
	if (mFogEnabled)
	{
		// Pristine factory log-decay baseline calculation with correct array index tracking
		float mLightParmsInitial = mLightParms[2]; 
		float FogDensityInitial = mLightParmsInitial * -0.6931471f;//=1/log(2)
		float FogDensityCurrent = FogDensityInitial; // Mutable working copy
		const float invMul255 = 1.0f / 255.0f;

		// Unpack immutable baseline state snapshots of the current sector fog color
		int8_t fogColInitR = mFogColor.r; int8_t fogColCurR = fogColInitR;
		int8_t fogColInitG = mFogColor.g; int8_t fogColCurG = fogColInitG;
		int8_t fogColInitB = mFogColor.b; int8_t fogColCurB = fogColInitB;

		int lightlevel = 128; // Safe baseline default if unbound

		// Context validation shield protects from access violations on map change
		bool isLevelContextAlive = (g_isCurrentlyGL1xDynlightWallDrawing  == nullptr ||
			g_isCurrentlyGL1xDynlightFlatDrawing != nullptr ||
			g_isCurrentlyHWWallDrawing != nullptr ||
			g_isCurrentlyHWFlatDrawing  == nullptr);

		if (g_isCurrentlyGLSpriteDrawing && isLevelContextAlive && actor != nullptr && (uintptr_t)actor > 0x10000)
		{
			if ((uintptr_t)actor->Sector > 0x10000)
			{
				lightlevel = actor->Sector->lightlevel; // Fetch live sprite/model lighting zone
			}
		}
		else
		{
			if (g_isCurrentlyGL1xDynlightWallDrawing != nullptr)
				{lightlevel = g_isCurrentlyGL1xDynlightWallDrawing->lightlevel;}
			else if (g_isCurrentlyGL1xDynlightFlatDrawing != nullptr)
				{lightlevel = g_isCurrentlyGL1xDynlightFlatDrawing->lightlevel; }
			else if (g_isCurrentlyHWWallDrawing != nullptr)
				{lightlevel = g_isCurrentlyHWWallDrawing->lightlevel;}
			else if (g_isCurrentlyHWFlatDrawing != nullptr && g_isCurrentlyHWFlatDrawing->sector != nullptr)
				{lightlevel = g_isCurrentlyHWFlatDrawing->sector->lightlevel; }
		}

		// Determine if we are rendering inside pure black fog (classic Doom dark fade on all levels aka diminishing lighting)
		bool isBlackFog = (fogColInitR < 5 && fogColInitG < 5 && fogColInitB < 5);

		//Check condition triggers for custom tracking branches
		bool isGL1xSoftwareBlackFog = (isBlackFog && gl.legacyMode && (gl_lightmode == 8 || gl_lightmode == 16));
		bool isGL1xColoredDynlightsFog = (gl_lights && fogColInitR >= 5 && fogColInitG >= 5 && fogColInitB >= 5);

		// === STEP 1: EMBED THE UNIFIED SCALE MATRIX LAYOUT ===-
		if (isGL1xSoftwareBlackFog || isGL1xColoredDynlightsFog)
		{
			float fogDensMul = 2.0f; float fogColorMul = 1.0f;

			if      (lightlevel <= 88                     ) { fogDensMul = 1.00f; fogColorMul = 1.00f; }
			else if (lightlevel >= 89  && lightlevel < 96 ) { fogDensMul = 1.05f; fogColorMul = 0.99f; }
			else if (lightlevel >= 96  && lightlevel < 104) { fogDensMul = 1.15f; fogColorMul = 0.98f; }
			else if (lightlevel >= 104 && lightlevel < 112) { fogDensMul = 1.45f; fogColorMul = 0.97f; }
			else if (lightlevel >= 112 && lightlevel < 120) { fogDensMul = 1.58f; fogColorMul = 0.96f; }
			else if (lightlevel >= 120 && lightlevel < 128) { fogDensMul = 1.75f; fogColorMul = 0.95f; }
			else if (lightlevel >= 128 && lightlevel < 136) { fogDensMul = 2.00f; fogColorMul = 0.94f; }
			else if (lightlevel >= 136 && lightlevel < 144) { fogDensMul = 2.12f; fogColorMul = 0.93f; }
			else if (lightlevel >= 144 && lightlevel < 152) { fogDensMul = 2.24f; fogColorMul = 0.92f; }
			else if (lightlevel >= 152 && lightlevel < 160) { fogDensMul = 2.24f; fogColorMul = 0.91f; }
			else if (lightlevel >= 160 && lightlevel < 168) { fogDensMul = 2.00f; fogColorMul = 0.91f; }
			else if (lightlevel >= 168 && lightlevel < 176) { fogDensMul = 1.88f; fogColorMul = 0.92f; }
			else if (lightlevel >= 176 && lightlevel < 184) { fogDensMul = 1.75f; fogColorMul = 0.93f; }
			else if (lightlevel >= 184 && lightlevel < 192) { fogDensMul = 1.65f; fogColorMul = 0.95f; }
			else if (lightlevel >= 192 && lightlevel < 200) { fogDensMul = 1.50f; fogColorMul = 0.97f; }
			else if (lightlevel >= 200 && lightlevel < 216) { fogDensMul = 1.25f; fogColorMul = 0.99f; }
			else
			{
				fogDensMul = 1.12f; fogColorMul = 1.00f;
			}

			// --- THE ANAMORPHIC TRIGONOMETRIC DEPTH-BUFFER SHIELD ---
			// Smooth fake contrast continuously drops the lightlevel on horizontal and near-horizontal walls.
			// This forces the OpenGL 1.1 hardware GL_FOG matrix to over-saturate and black out at long distances.
			// Extract the exact same trigonometric 'interp' projection (cosAngle * cosAngle) from p_sectors.cpp,
			// and use it to dynamically and seamlessly damp down fogDensMul on all horizontal vector spans.
			if (isGL1xSoftwareBlackFog && g_isCurrentlyHWWallDrawing != nullptr && g_isCurrentlyHWWallDrawing->seg != nullptr)
			{
				auto currentLine = g_isCurrentlyHWWallDrawing->seg->linedef;
				if (currentLine != nullptr)
				{
					DVector2 delta = currentLine->Delta();
					float lenSq = (float)(delta.X * delta.X + delta.Y * delta.Y);

					if (lenSq > 0.0001f)
					{
						// Calculate cosAngle square (interp factor: 1.0 = strict horizontal, 0.0 = strict vertical)
						float cosAngleSq = (float)(delta.X * delta.X) / lenSq;

						// if the wall falls into the horizontal spectrum (cosAngleSq > 0.0) and distance is huge
						if (cosAngleSq > 0.01f && lightlevel >= 128)
						{
							// Smoothly scale the attenuation dampener: lines closer to East-West axis
							// receive a much stronger fog clearance pass, up to a 55% reduction.
							float dynamicDamp = 1.0f - (0.55f * cosAngleSq);
							fogDensMul = fogDensMul * dynamicDamp;
						}
					}
				}
			}

			//if (!g_isCurrentlyGLSpriteDrawing) fogDensMul = 1.5f; // in case you want even more fog but why?
			fogColCurR = (int8_t)(fogColInitR * fogColorMul);
			fogColCurG = (int8_t)(fogColInitG * fogColorMul);
			fogColCurB = (int8_t)(fogColInitB * fogColorMul);

			// --- STEP 2: EVALUATE TARGET TRACK CHANNELS ===--
			if (isGL1xSoftwareBlackFog)
			{
				// Software/Vanilla modes have less fog on actors, especially the brighter ligtlevel gets
				if (g_isCurrentlyGLSpriteDrawing)
				{
					float                       spriteModifier = 1.00f;
					if      (lightlevel >= 240) spriteModifier = 3.20f;
					else if (lightlevel >= 224) spriteModifier = 3.58f;
					else if (lightlevel >= 208) spriteModifier = 4.00f;
					else if (lightlevel >= 192) spriteModifier = 4.40f;
					else if (lightlevel >= 176) spriteModifier = 5.60f;
					else if (lightlevel >= 160) spriteModifier = 6.40f;
					else if (lightlevel >= 144) spriteModifier = 7.00f;
					else if (lightlevel >= 136) spriteModifier = 8.60f;
					else if (lightlevel >= 128) spriteModifier = 9.20f;
					else if (lightlevel >= 112) spriteModifier = 8.80f;
					else if (lightlevel >= 80)  spriteModifier = 8.00f;
					else if (lightlevel >= 48)  spriteModifier = 7.40f;
					else                        spriteModifier = 6.80f;

					// Smash the fog down exponentially on sprites in the dark
					FogDensityCurrent = FogDensityCurrent / spriteModifier;
				}
				else
				{
					FogDensityCurrent *= fogDensMul; // more for walls or flats
					// Keep solid walls and flats tightly clamped at a maximum dense -0.35f threshold
					if (FogDensityCurrent < -0.35f) FogDensityCurrent = -0.35f; // HARD-LOCK SHIELD FOR GEOMETRY!
				}
			}
			else // isGL1xColoredDynlightsFog path wrapper
			{
				// Fog loses intensity and gets brighter if rendered on top of regular modulated dynlight lit surfaces
				if (g_isCurrentlyGLSpriteDrawing)
				{
					float spriteFogDens = fogDensMul * 0.625f;
					if (spriteFogDens < 1.0f) spriteFogDens = 1.0f;

					FogDensityCurrent = spriteFogDens; // less for sprites/models
				}
				else
				{
					FogDensityCurrent *= fogDensMul; // more for walls or flats
					// Keep solid walls and flats tightly clamped at a maximum dense -0.35f threshold
					if (FogDensityCurrent < -0.35f) FogDensityCurrent = -0.35f; // HARD-LOCK SHIELD FOR GEOMETRY!
				}
			}

			// Push un-corrupted dynamically faded color assets straight into hardware registers
			GLfloat HardwareFogColor[4] = { (float)fogColCurR * invMul255, (float)fogColCurG * invMul255, (float)fogColCurB * invMul255, 0.0f };
			glFogfv(GL_FOG_COLOR, HardwareFogColor);
			ffFogColor = mFogColor; // Set sync footprint

			glFogf(GL_FOG_DENSITY, fabsf(FogDensityCurrent)); // Push boosted parameters directly
			//if (g_isCurrentlyGLSpriteDrawing) ffFogDensity = FogDensityCurrent;
			if (g_isCurrentlyGLSpriteDrawing) ffFogDensity = mLightParmsInitial; // no abrupt fog intens drop on sprites?
			else
				ffFogDensity = mLightParmsInitial;
		}
		// --- STEP 3: STANDARD FACTORY HARDWARE FALLBACK ===--
		else
		{
			GLfloat FactoryFogColor[4] = { (float)fogColInitR * invMul255, (float)fogColInitG * invMul255, (float)fogColInitB * invMul255, 0.0f };
			glFogfv(GL_FOG_COLOR, FactoryFogColor);
			ffFogColor = mFogColor;

			glFogf(GL_FOG_DENSITY, fabsf(FogDensityInitial)); // Force hardware fallback to clean initial log curve
			ffFogDensity = mLightParmsInitial; // Hard-sync cacher to initial state instantly
		}
	}

	if (mSpecialEffect != ffSpecialEffect)
	{
		switch (ffSpecialEffect)
		{
		case EFF_SPHEREMAP:
			glDisable(GL_TEXTURE_GEN_T);
			glDisable(GL_TEXTURE_GEN_S);
		default:
			break;
		}
		switch (mSpecialEffect)
		{
		case EFF_SPHEREMAP:
			glEnable(GL_TEXTURE_GEN_T);
			glEnable(GL_TEXTURE_GEN_S);
			glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
			glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
			break;
		default:
			break;
		}
		ffSpecialEffect = mSpecialEffect;
	}

	// STEP 5: COLOR MATRIX CONFIGURATIONS
	// Match GZDoom 4x dev vector architectures channel-by-channel
	// mStreamData.uVertexColor is FVector4, so its fields are strictly .X, .Y, .Z, .W
	auto &col = mStreamData.uVertexColor; 

	// Legacy sprites and 3D models dynamic light - START
	if (isUsingVolumetric3DModelLegacyDynlight)
	{
		// Disable flat coloring for legacy VOLUMETRIC dynamic lights strictly for 3D meshes
		//col.X += mDynGL1xColor.X; 
		//col.Y += mDynGL1xColor.Y;
		//col.Z += mDynGL1xColor.Z;
	}
	else
	{
		// Activate flat coloring for sprites and legacy FLAT dynamic lights
		// Both col and mDynGL1xColor are FVector4, utilizing .X, .Y, .Z coordinates seamlessly
		col.X += mDynGL1xColor.X;
		col.Y += mDynGL1xColor.Y;
		col.Z += mDynGL1xColor.Z;
	}
	// Legacy sprites and 3D models dynamic light - FINISH

	col.X = clamp(col.X, 0.f, 1.f);
	col.Y = clamp(col.Y, 0.f, 1.f);
	col.Z = clamp(col.Z, 0.f, 1.f);
	col.W = clamp(col.W, 0.f, 1.f);

	// mStreamData.uObjectColor is FVector4PalEntry, so read its fields as .r, .g, .b, .a
	col.X *= (mStreamData.uObjectColor.r); 
	col.Y *= (mStreamData.uObjectColor.g);
	col.Z *= (mStreamData.uObjectColor.b); 
	col.W *= (mStreamData.uObjectColor.a);

	// HARDWARE ALPHA CLAMPING FOR TRANSLUCENT OBJECTS IN OPENGL 1.1
	if (gl.gl1path && gl.gl1_v1dot1)
	{
		// If this is a targeted translucent style (flagged as 99 in gl_GetRenderStyle)
		if (ffTextureMode == 99)
		{
			// Clamp alpha channel to 0.51f to ensure it bypasses the 0.5f alpha-test gate.
			// This makes previously invisible particles, explosions, and projectiles perfectly visible.
			if (col.W <= 0.51f)
			{
				col.W = 0.51f;
			}
		}
	}

	glColor4fv(&col.X); // Applies clean baseline sector shadow level
	glEnable(GL_BLEND);

	if (mAlphaThreshold > 0)
	{
		glEnable(GL_ALPHA_TEST);
		if (gl.gl1path && gl.gl1_v1dot1)
		{
			// For OpenGL 1.1: If it's our translucent object, disable alpha test
			// to bypass binary masking edges and ensure soft blending curves.
			if (ffTextureMode == 99)
			{
				glDisable(GL_ALPHA_TEST);
			}
			else
			{
				// GL 1.1: Use unmultiplied baseline threshold
				// to protect grated textures from double darkening or clipping out early.
				glAlphaFunc(GL_GREATER, mAlphaThreshold);
			}
		}
		else
		{
			// Original GL 1.3+: Multiplies threshold by clean sector alpha level
			glAlphaFunc(GL_GREATER, mAlphaThreshold * col.W);
		}
	}
	else
	{
		glDisable(GL_ALPHA_TEST);
	}

	// Protect texture matrix allocations during mode 13 projection pass
	if (mTextureMatrixEnabled && ffTextureMode != 13)
	{
		glMatrixMode(GL_TEXTURE);
		glLoadMatrixf((float*)&mTextureMatrix);
		currentTextureMatrixState = true;
	}
	else if (currentTextureMatrixState && ffTextureMode != 13)
	{
		glMatrixMode(GL_TEXTURE);
		glLoadIdentity();
		currentTextureMatrixState = false;
	}

	if (mModelMatrixEnabled)
	{
		glMatrixMode(GL_MODELVIEW);
		glLoadMatrixf((float*)&mModelMatrix);
		currentModelMatrixState = true;
	}
	else if (currentModelMatrixState)
	{
		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity(); 
		currentModelMatrixState = false;
	}
}

//==========================================================================
//
//
//
//==========================================================================

void gl_FillGL1xScreen()
{
	OpenGLRenderer::gl_RenderState.AlphaGL1xFunc(GL_GEQUAL, 0.f);
	OpenGLRenderer::gl_RenderState.EnableTexture(false);
	OpenGLRenderer::gl_RenderState.ApplyFixedGL1xFunction(nullptr);
	// FFlatVertexBuffer::FULLSCREEN_INDEX is exactly 4 (from flatvertices.h)
	glDrawArrays(GL_TRIANGLE_STRIP, FFlatVertexBuffer::FULLSCREEN_INDEX, 4);
}
 
//==========================================================================
//
// DrawGL1xBlend
// Replaces old GLSceneDrawer::DrawBlend for core OpenGL 1.1 pipeline.
// Processes sector fades, 3D water, lava, and environment damage screens.
//
//==========================================================================
void HWDrawInfo::DrawGL1xColormapOverlay(sector_t* viewsector)
{
	PalEntry modulateColor = 0xffffffff;

	// Calculate the complete stacked blend vector (RGB + Alpha)
	// This automatically processes 3D floors, water, lava, and player damage flashes
	FVector4 blend = V_CalcBlend(viewsector, &modulateColor);

	// Handle Multiplicative Blend (modulateColor) if assigned by the engine
	if (modulateColor != 0xffffffff)
	{
		float extra_red = modulateColor.r / 255.0f;
		float extra_green = modulateColor.g / 255.0f;
		float extra_blue = modulateColor.b / 255.0f;

		if (extra_red || extra_green || extra_blue)
		{
			OpenGLRenderer::gl_RenderState.BlendGL1xFunc(GL_DST_COLOR, GL_ZERO);
			OpenGLRenderer::gl_RenderState.SetColor(extra_red, extra_green, extra_blue, 1.0f);
			gl_FillGL1xScreen(); // Draw screen overlay
		}
	}

	// Handle Standard Additive Blends (Lava tints, radiation goggles, blood flashes)
	OpenGLRenderer::gl_RenderState.SetTextureMode(TM_MODULATE);
	OpenGLRenderer::gl_RenderState.BlendGL1xFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	// blend[3] is the aggregated final Alpha channel calculated by V_CalcBlend
	if (blend[3] > 0.0f)
	{
		OpenGLRenderer::gl_RenderState.SetColor(blend[0], blend[1], blend[2], blend[3]);
		gl_FillGL1xScreen();
	}

	// Reset hardware pipe safely back to standard baseline rendering frame
	OpenGLRenderer::gl_RenderState.ResetColor();
	OpenGLRenderer::gl_RenderState.EnableTexture(true);
}

//==========================================================================
//
// Sets up the parameters to render one dynamic light onto one plane
//
//==========================================================================

//void gl_dynlightGL1xSaturateSlow_unused(float &r, float &g, float &b, float current_boost, float radius, FLightColorContext &ctx)
//{
//	// Prevent logic executions on zero/dead light nodes
//	if (ctx.maxColor > 0.001f)
//	{
//		float avgColor = (r + g + b) * 0.3333f;
//
//		// Amplificate channels relative to how much they exceed the baseline average color
//		r += (r - avgColor) * current_boost;
//		g += (g - avgColor) * current_boost;
//		b += (b - avgColor) * current_boost;
//
//		// Re-evaluate limits after saturation boost locally
//		float localMax = r; if (g > localMax) localMax = g; if (b > localMax) localMax = b;
//		float localMin = r; if (g < localMin) localMin = g; if (b < localMin) localMin = b;
//		ctx.localChroma = localMax - localMin;
//
//		// HARD CRITERIA: Execute hue shifting ONLY on colored lights larger than 768 units radius
//		if (ctx.chroma >= 0.10f && ctx.localChroma > 0.05f && radius >= 768.0f)
//		{
//			// --- STEP-BY-STEP CASCADE HUE SHIFT CALCULATION (+5 per size tier) ---
//			float                                             sizeShiftModifier = 0.0f; // Base level
//			if      (radius >= 384.0f && radius <= 800.0f)    sizeShiftModifier = 3.2f;
//			else if (radius >= 800.0f && radius <= 1600.0f)   sizeShiftModifier = 4.5f;
//			else if (radius >= 1600.0f && radius <= 3000.0f)  sizeShiftModifier = 5.0f;
//			else if (radius >= 3000.0f && radius <= 6000.0f)  sizeShiftModifier = 5.5f;
//			else if (radius >= 6000.0f && radius <= 12000.0f) sizeShiftModifier = 6.5f;
//			else if (radius >= 12000.0f)                      sizeShiftModifier = 7.0f;
//
//			float raw_shift = gl_legacy_dynlight_hue_shift + sizeShiftModifier;
//			float final_shift = 0.0f;
//
//			// Guard condition: inside [-20; 20] range hue modulation remains untouched
//			if (fabsf(raw_shift) > 20.0f)
//			{
//				if (raw_shift > 0.0f) final_shift = raw_shift - 20.0f;
//				else final_shift = raw_shift + 20.0f;
//			}
//
//			if (final_shift < 0.0f) final_shift = 360.0f + fmodf(final_shift, 360.0f);
//			if (final_shift >= 360.0f) final_shift = fmodf(final_shift, 360.0f);
//
//			// Execute chromatic spectrum translation if final rotation angle is valid
//			if (final_shift > 0.001f)
//			{
//				float h = 0.0f;
//				if (localMax == r) { h = (g - b) / ctx.localChroma; if (h < 0.0f) h += 6.0f; }
//				else if (localMax == g) { h = (b - r) / ctx.localChroma + 2.0f; }
//				else { h = (r - g) / ctx.localChroma + 4.0f; }
//				h *= 60.0f;
//
//				h += final_shift;
//				if (h >= 360.0f) h -= 360.0f;
//
//				float hPrime = h / 60.0f;
//				float x = ctx.localChroma * (1.0f - fabsf(fmodf(hPrime, 2.0f) - 1.0f));
//				float r1 = 0.0f, g1 = 0.0f, b1 = 0.0f;
//
//				if      (hPrime >= 0.0f && hPrime < 1.0f) { r1 = ctx.localChroma; g1 = x; }
//				else if (hPrime >= 1.0f && hPrime < 2.0f) { r1 = x; g1 = ctx.localChroma; }
//				else if (hPrime >= 2.0f && hPrime < 3.0f) { g1 = ctx.localChroma; b1 = x; }
//				else if (hPrime >= 3.0f && hPrime < 4.0f) { g1 = x; b1 = ctx.localChroma; }
//				else if (hPrime >= 4.0f && hPrime < 5.0f) { r1 = x; b1 = ctx.localChroma; }
//				else                                      { r1 = ctx.localChroma; b1 = x; }
//
//				float m = localMin;
//				r = r1 + m;
//				g = g1 + m;
//				b = b1 + m;
//			}
//		}
//
//		if (r < 0.0f) r = 0.0f; else if (r > 1.0f) r = 1.0f;
//		if (g < 0.0f) g = 0.0f; else if (g > 1.0f) g = 1.0f;
//		if (b < 0.0f) b = 0.0f; else if (b > 1.0f) b = 1.0f;
//	}
//}

//void gl_dynlightSaturateLegacy_faster_unused(float &r, float &g, float &b, float current_boost, float radius, FLightColorContext &ctx)
//{
//	// 1. FAST INSTRINSICS FOR MAX/MIN (Compiles directly into CMOV assembly instructions, 0% branch mispredictions!)
//	float localMax = std::max({ r, g, b });
//	float localMin = std::min({ r, g, b });
//	ctx.localChroma = localMax - localMin;
//
//	// Early exit gate for dead or purely colorless gray/white light nodes
//	if (ctx.maxColor <= 0.001f || ctx.chroma < 0.10f || ctx.localChroma <= 0.05f || radius < 768.0f)
//	{
//		// Amplificate channels safely for standard color mass
//		r += (r - (r + g + b) * 0.333333f) * current_boost;
//		g += (g - (r + g + b) * 0.333333f) * current_boost;
//		b += (b - (r + g + b) * 0.333333f) * current_boost;
//
//		// Pure hardware recovery fast clamp layout
//		r = clamp(r, 0.0f, 1.0f);
//		g = clamp(g, 0.0f, 1.0f);
//		b = clamp(b, 0.0f, 1.0f);
//		return;
//	}
//
//	// 2. CORE AMPLIFICATION CONVEYOR
//	float avgColor = (r + g + b) * 0.333333f;
//	r += (r - avgColor) * current_boost;
//	g += (g - avgColor) * current_boost;
//	b += (b - avgColor) * current_boost;
//
//	// Re-evaluate limits after saturation boost using lightning-fast std math
//	localMax = std::max({ r, g, b });
//	localMin = std::min({ r, g, b });
//	ctx.localChroma = localMax - localMin;
//
//	// 3. FLAT CASCADE RADIUS LOOKUP TABLE (WITHOUT BOX SPLIT)
//	float sizeShiftModifier = 0.0f;
//	if (radius <= 1600.0f) sizeShiftModifier = (radius <= 800.0f) ? 3.2f : 4.5f;
//	else if (radius <= 6000.0f) sizeShiftModifier = (radius <= 3000.0f) ? 5.0f : 5.5f;
//	else                        sizeShiftModifier = (radius <= 12000.0f) ? 6.5f : 7.0f;
//
//	float raw_shift = gl_legacy_dynlight_hue_shift + sizeShiftModifier;
//	float final_shift = 0.0f;
//
//	// Optimized fast boundaries guard without calling heavy math
//	if (raw_shift > 20.0f)       final_shift = raw_shift - 20.0f;
//	else if (raw_shift < -20.0f) final_shift = raw_shift + 20.0f;
//
//	// HIGH-SPEED FRACTIONAL REPLACEMENT FOR FMODF
//	// Since raw_shift angle sits in tightly bound ranges, replace heavy fmodf 
//	// with branchless flat float-to-int subtraction logic loops.
//	if (final_shift < 0.0f)
//	{
//		final_shift += 360.0f * (static_cast<int>(-final_shift / 360.0f) + 1);
//	}
//	if (final_shift >= 360.0f)
//	{
//		final_shift -= 360.0f * static_cast<int>(final_shift / 360.0f);
//	}
//
//	// 4. ULTRAFAST SPECTRUM TRANSLATION MATRIX
//	if (final_shift > 0.001f)
//	{
//		// Reciprocal Calculation Pass: Compute inverse chroma ONCE to kill heavy divisions!
//		float rcpChroma = 1.0f / ctx.localChroma;
//		float h = 0.0f;
//
//		if (localMax == r)
//		{
//			h = (g - b) * rcpChroma;
//			if (h < 0.0f) h += 6.0f;
//		}
//		else if (localMax == g)
//		{
//			h = (b - r) * rcpChroma + 2.0f;
//		}
//		else
//		{
//			h = (r - g) * rcpChroma + 4.0f;
//		}
//		h *= 60.0f;
//
//		h += final_shift;
//		if (h >= 360.0f) h -= 360.0f;
//
//		// FAST INTERPOLATION LOOKUP MAPPING (Bypasses fmodf and fabsf completely!)
//		float hPrime = h * 0.01666667f; // h / 60.0f
//		int hPrimeInt = static_cast<int>(hPrime);
//		float fraction = hPrime - hPrimeInt;
//
//		float x = ctx.localChroma * (1.0f - (hPrimeInt & 1 ? fraction : 1.0f - fraction));
//		float r1 = 0.0f, g1 = 0.0f, b1 = 0.0f;
//
//		// Unrolled linear lookup case blocks instead of conditional switches
//		switch (hPrimeInt)
//		{
//		case 0:  r1 = ctx.localChroma; g1 = x; break;
//		case 1:  r1 = x; g1 = ctx.localChroma; break;
//		case 2:  g1 = ctx.localChroma; b1 = x; break;
//		case 3:  g1 = x; b1 = ctx.localChroma; break;
//		case 4:  r1 = x; b1 = ctx.localChroma; break;
//		default: r1 = ctx.localChroma; b1 = x; break;
//		}
//
//		r = r1 + localMin;
//		g = g1 + localMin;
//		b = b1 + localMin;
//	}
//
//	// Final pristine hardware security clamp
//	r = clamp(r, 0.0f, 1.0f);
//	g = clamp(g, 0.0f, 1.0f);
//	b = clamp(b, 0.0f, 1.0f);
//}



// Here comes the fastest optimized version, let's do it Ken Silverman reciptable way
struct FKenRecipTable
{
	uint32_t table[2048];
	FKenRecipTable()
	{
		for (int i = 0; i < 2048; i++)
		{
			// Ken's native divscale30 setup: 2048L / (i + 2048) scaled to 30 bits
			float value = 2048.0f / (float)(i + 2048);
			table[i] = (uint32_t)(value * 1073741824.0f); // 1 << 30
		}
	}
};
static const FKenRecipTable g_KenRecipTable;

inline float KenFastReciprocal(float f)
{
	// Cast float bits directly into integer space without flushing FPU registers
	uint32_t bits = *(uint32_t*)&f;

	// Extract the mantissa bits (0x007ff000 is Ken's exact 11-bit mask shift)
	uint32_t mantissaIdx = (bits & 0x007ff000) >> 10;

	// Extract and normalize the exponent bits matching 0x3f800000 baseline
	int32_t exponent = (int32_t)((bits >> 23) & 0xFF) - 127;

	// Fetch the pre-scaled 30-bit inverse fraction from Ken's table vault
	uint32_t recipValue = g_KenRecipTable.table[mantissaIdx];

	// Shift back the bits based on the extracted exponent scale tier
	int32_t shift = 11 - exponent; // balancing Ken's 11-bit shift logic

	float result = 0.0f;
	if (shift >= 0)
	{
		result = (float)(recipValue >> shift) / 1073741824.0f;
	}
	else
	{
		result = (float)(recipValue << (-shift)) / 1073741824.0f;
	}

	// Re-inject the original sign bit profile safely
	if (bits & 0x80000000) result = -result;
	return result;
}

void gl_dynlightSaturateLegacy(float &r, float &g, float &b, float current_boost, float radius, FLightColorContext &ctx)
{
	float localMax = max(r, max(g, b));
	float localMin = min(r, min(g, b));
	ctx.localChroma = localMax - localMin;

	// Early exit gate for standard lights or colorless nodes
	if (ctx.maxColor <= 0.001f || ctx.chroma < 0.10f || ctx.localChroma <= 0.05f || radius < 768.0f)
	{
		r += (r - (r + g + b) * 0.333333f) * current_boost;
		g += (g - (r + g + b) * 0.333333f) * current_boost;
		b += (b - (r + g + b) * 0.333333f) * current_boost;

		r = clamp(r, 0.0f, 1.0f);
		g = clamp(g, 0.0f, 1.0f);
		b = clamp(b, 0.0f, 1.0f);
		return;
	}

	float avgColor = (r + g + b) * 0.333333f;
	r += (r - avgColor) * current_boost;
	g += (g - avgColor) * current_boost;
	b += (b - avgColor) * current_boost;

	localMax = max(r, max(g, b));
	localMin = min(r, min(g, b));
	ctx.localChroma = localMax - localMin;

	// Flat cascade lookup table
	float                       sizeShiftModifier = 0.0f;
	if      (radius <= 1600.0f) sizeShiftModifier = (radius <= 800.0f)   ? 3.2f : 4.5f;
	else if (radius <= 6000.0f) sizeShiftModifier = (radius <= 3000.0f)  ? 5.0f : 5.5f;
	else                        sizeShiftModifier = (radius <= 12000.0f) ? 6.5f : 7.0f;

	// It distorts the colors a little, so compensate slightly with a magic number
	float raw_shift = gl_legacy_dynlight_hue_shift + sizeShiftModifier -6.12f;
	float final_shift = 0.0f;

	if (raw_shift > 20.0f)       final_shift = raw_shift - 20.0f;
	else if (raw_shift < -20.0f) final_shift = raw_shift + 20.0f;

	if (final_shift < 0.0f)
	{
		final_shift += 360.0f * (static_cast<int>(-final_shift * 0.002777778f) + 1);
	}
	if (final_shift >= 360.0f)
	{
		final_shift -= 360.0f * static_cast<int>(final_shift * 0.002777778f);
	}

	if (final_shift > 0.001f)
	{
		float rcpChroma = KenFastReciprocal(ctx.localChroma);

		float h = 0.0f;
		if (localMax == r)
		{
			h = (g - b) * rcpChroma;
			if (h < 0.0f) h += 6.0f;
		}
		else if (localMax == g)
		{
			h = (b - r) * rcpChroma + 2.0f;
		}
		else
		{
			h = (r - g) * rcpChroma + 4.0f;
		}

		h *= 60.0f;
		h += final_shift;
		if (h >= 360.0f) h -= 360.0f;

		float hPrime = h * 0.01666667f;
		int hPrimeInt = static_cast<int>(hPrime);
		float fraction = hPrime - hPrimeInt;

		float x = ctx.localChroma * (1.0f - (hPrimeInt & 1 ? fraction : 1.0f - fraction));
		float r1 = 0.0f, g1 = 0.0f, b1 = 0.0f;

		switch (hPrimeInt)
		{
		case 0:  r1 = ctx.localChroma; g1 = x; break;
		case 1:  r1 = x; g1 = ctx.localChroma; break;
		case 2:  g1 = ctx.localChroma; b1 = x; break;
		case 3:  g1 = x; b1 = ctx.localChroma; break;
		case 4:  r1 = x; b1 = ctx.localChroma; break;
		default: r1 = ctx.localChroma; b1 = x; break;
		}

		r = r1 + localMin;
		g = g1 + localMin;
		b = b1 + localMin;
	}

	r = clamp(r, 0.0f, 1.0f);
	g = clamp(g, 0.0f, 1.0f);
	b = clamp(b, 0.0f, 1.0f);
}

// We need this for huge dynlights not to overexposure the mirrored surfaces
void gl_dynlightTameBigLightsOnMirroredSurfacesLegacy(FDynamicLight * light, float &r, float &g, float &b, float radius, bool isOpaque)
{
	const float intensThresh = 0.22f; // max starting mirrored surface brightness

	if (!isOpaque && radius >= 468.0f && light != nullptr && light->IsActive())
	{
		// Extract original, uncorrupted color signatures before any hue shifts
		FLightColorContext colorCtx(light);

		float minimumScaleLimit = intensThresh; // Default floor for monochrome lamps

		// Cascading chroma segregation gates
		if (colorCtx.chroma >= 0.25f)
		{
			// Tier A: Highly colorful spectral emitters (pink sunset / neon plasma)
			// 0.55f allows peak legacy luminosity without blowing out underlying reflections
			minimumScaleLimit = 0.55f;
		}
		else if (colorCtx.chroma >= 0.10f)
		{
			// Tier B: Mildly colorful lights
			minimumScaleLimit = 0.45f;
		}
		else
		{
			// Tier C: Perfectly colorless, blinding monochrome white proctors (lamps)
			minimumScaleLimit = intensThresh; // Locked tightly at 0.18f
		}

		// Apply the selected adaptive individual channel scale limits safely
		r *= minimumScaleLimit; g *= minimumScaleLimit; b *= minimumScaleLimit;

		if (r < 0.0f) r = 0.0f; else if (r > 1.0f) r = 1.0f;
		if (g < 0.0f) g = 0.0f; else if (g > 1.0f) g = 1.0f;
		if (b < 0.0f) b = 0.0f; else if (b > 1.0f) b = 1.0f;
	}
}

// We need this to tame down subtractive and other lights if needed.
// This is the only way to control their intensity for transluscent map geometry surfaces,
// As well as subtractive both on transluscent and regular other map geometry surfaces!
float GL1x_light_intens_regular_init = 1.0f;
float GL1x_light_intens_regular_cur = GL1x_light_intens_regular_init;
float GL1x_light_intens_regular_transluc_init = 1.0f;
float GL1x_light_intens_regular_transluc_cur = GL1x_light_intens_regular_transluc_init;

float GL1x_light_intens_additive_init = 0.22f;
float GL1x_light_intens_additive_cur = GL1x_light_intens_additive_init;
float GL1x_light_intens_additive_transluc_init = 0.22f;
float GL1x_light_intens_additive_transluc_cur = GL1x_light_intens_additive_transluc_init;

float GL1x_light_intens_subtractive_init = 1.0f;
float GL1x_light_intens_subtractive_cur = GL1x_light_intens_subtractive_init;
float GL1x_light_intens_subtractive_transluc_init = 0.17f;
float GL1x_light_intens_subtractive_transluc_cur = GL1x_light_intens_subtractive_transluc_init;
void gl_dynlightHandleSpecialLightsLegacy(float &r, float &g, float &b, bool isTransluscent, FDynamicLight* light)
{
	if (!light || !light->IsActive()) return;

	// For regular modulated geometry dynlights only as others don't need overbright
	if (g_isCurrentlyGL1xFlatsDynlightOverbrightPass)
	{
		GL1x_light_intens_regular_cur *= gl_legacy_dynlight_overbright_flats;
	}
	if (g_isCurrentlyGL1xWallsDynlightOverbrightPass)
	{
		GL1x_light_intens_regular_cur *= gl_legacy_dynlight_overbright_walls;
	}

	// CRITICAL NOTE: check translucency flag FIRST.
	// This prevents general rules from overriding custom transparent material multipliers.
	//--- 1. REGULAR MODULATED CHANNELS  ---
	if (isTransluscent && !light->IsAdditive() && !light->IsSubtractive())
	{
		r *= GL1x_light_intens_regular_transluc_cur;
		g *= GL1x_light_intens_regular_transluc_cur;
		b *= GL1x_light_intens_regular_transluc_cur;
	}
	else if (!light->IsAdditive() && !light->IsSubtractive())
	{
		r *= GL1x_light_intens_regular_cur;
		g *= GL1x_light_intens_regular_cur;
		b *= GL1x_light_intens_regular_cur;
	}

	// --- 2. SPECIAL ADDITIVE CHANNELS ---
	if (isTransluscent && light->IsAdditive())
	{
		r *= GL1x_light_intens_additive_transluc_cur;
		g *= GL1x_light_intens_additive_transluc_cur;
		b *= GL1x_light_intens_additive_transluc_cur;
	}
	else if (light->IsAdditive())
	{
		r *= GL1x_light_intens_additive_cur;
		g *= GL1x_light_intens_additive_cur;
		b *= GL1x_light_intens_additive_cur;
	}

	// --- 3. SPECIAL SUBTRACTIVE ANTI-LIGHT CHANNELS ---
	if (isTransluscent && light->IsSubtractive())
	{
		r *= GL1x_light_intens_subtractive_transluc_cur;
		g *= GL1x_light_intens_subtractive_transluc_cur;
		b *= GL1x_light_intens_subtractive_transluc_cur;
	}
	else if (light->IsSubtractive())
	{
		r *= GL1x_light_intens_subtractive_cur;
		g *= GL1x_light_intens_subtractive_cur;
		b *= GL1x_light_intens_subtractive_cur;
	}

	// Symmetrical fixed-function registers hardware boundaries safety lock
	if (r < 0.0f) r = 0.0f; else if (r > 1.0f) r = 1.0f;
	if (g < 0.0f) g = 0.0f; else if (g > 1.0f) g = 1.0f;
	if (b < 0.0f) b = 0.0f; else if (b > 1.0f) b = 1.0f;
}

bool gl_SetupLightWall(int group, Plane & p, FDynamicLight * light, FVector3 & nearPt, FVector3 & up, FVector3 & right, float & scale, bool checkside, bool additive)
{
	if (light == nullptr) return false; // some crash prevention trick

	FVector3 fn, pos;
	int lightlevelwall = 128;
	float r, g, b;
	float current_boost;
	float curwallalpha = 1.0f;
	const float invMul64 = 1.0f / 64.0f;
	const float invMul255 = 1.0f / 255.0f;
	const float invMul286 = 1.0f / 286.0f;
	const float invMul322 = 1.0f / 322.0f;
	const float invMul424 = 1.0f / 424.0f;
	bool isCamglowDynlightHere = false;

	if (g_isCurrentlyGL1xDynlightWallDrawing != nullptr)
	{
		lightlevelwall = g_isCurrentlyGL1xDynlightWallDrawing->lightlevel;
		curwallalpha = g_isCurrentlyGL1xDynlightWallDrawing->alpha;
		isTrulyTranslucentGL1xWall = (curwallalpha <= 0.99f || g_isCurrentlyGL1xDynlightWallDrawing->type == RENDERWALL_MIRRORSURFACE);
	}

	DVector3 lpos = light->PosRelative(group);

	distLight2Wall = fabsf(p.DistToPoint(lpos.X, lpos.Z, lpos.Y));
	radiusWalls = light->GetRadius();

	if (light != nullptr && light->IsDynlightCamglow()) { g_isGL1xDynlightAcamglowOnWall = true; isCamglowDynlightHere = true; }
	if (OpenGLRenderer::gl_RenderState.mPassType == (EPassType)GLPASS_LIGHTTEX_OVERBRIGHT2_LEGACY)
	if (isCamglowDynlightHere) return false; // Turn off overbright for the camglow dynlight

	//Camglow radius is 2x to reduce BSP traversal early exit surface skip artifacts in GL1x/GL2x
	if(isCamglowDynlightHere) radiusWalls *= 0.5f;

	if (radiusWalls <= 0.f) return false;
	if (distLight2Wall > radiusWalls) return false;
	if (checkside && gl_lights_checkside && p.PointOnSide(lpos.X, lpos.Z, lpos.Y)) // clip lights
	{
		return false;
	}
	if (!light->visibletoplayer)
	{
		return false;
	}

	// 1. INITIALIZE LIGHT COLOR CONTEXT ONCE ON THE CPU
	FLightColorContext colorCtx(light);

	// Decrease distance to dynlight to have a richer color saturation in the far
	float                                                       distFactor = 1.0f;
	if      (radiusWalls >= 384.0f && radiusWalls <= 800.0f)    distFactor = 0.88f;
	else if (radiusWalls >= 800.0f && radiusWalls <= 1600.0f)   distFactor = 0.77f;
	else if (radiusWalls >= 1600.0f && radiusWalls <= 3000.0f)  distFactor = 0.74f;
	else if (radiusWalls >= 3000.0f && radiusWalls <= 6000.0f)  distFactor = 0.72f;
	else if (radiusWalls >= 6000.0f && radiusWalls <= 12000.0f) distFactor = 0.67f;
	else if (radiusWalls >= 12000.0f)                           distFactor = 0.64f;

	scale = 1.0f / ((2.25f * radiusWalls) - (distLight2Wall * distFactor));

	pos = { (float)lpos.X, (float)lpos.Z, (float)lpos.Y };
	fn = p.Normal();
	fn.GetRightUp(right, up);

	// ===  CAMGLOW DYNLIGHT STRAIGHT (SIMULATE SOFTWARE DIMLIGHT) - START ===
	// The whole thing is done with a zscript spawning a special dynlight on players
	// But to achieve STRAIGHT diminished lighting just like in software renderer for:
	// *Walls: expand the dynlight blob texture (gllight.png) vertically
	// *Flats: expand it horizontaly, fix anti-rotation bug on North-South slanted 
	// slopes by fully syncing the inverted reverse sine phase across all sloped 
	// rendering contexts. Maximizes perfect synchronized widescreen ellipse rotation
	//
	// ANTI-FLIP COMPASS BUGFIX : Reconstruct the 'up' vector to be a strict 
	// world-space vertical axis {0, 1, 0}.
	// Prevents the light pillar from turning horizontal on East-West or angled walls.
	// Manual Cross Product replaces overloaded pipe operators to guarantee compilation.
	//
	// THE WALLS PART IMPLEMENTATION BELOW:
	if (light != nullptr && light->IsDynlightCamglow() && gl_fogmode != 2)
	{
		//	// This network check is not necessary but why not?
		//if (light->target.pp != nullptr && r_viewpoint.camera != nullptr)
		//{
		//	// EXTRACT THE OWNER VIA MASTER POINTER NATIVELY
		//	AActor *rawPlayerOwner = light->target.pp->master.pp;
		//
		//	// NETWORK CLIENT-SIDE FILTER: If the rendering camera belongs to someone else,
		//	// or the light has LF_DONTLIGHTMAP flag, completely wipe this light pass from the GPU!
		//	if ((light->IsDontLightOthers() && rawPlayerOwner != nullptr && rawPlayerOwner != r_viewpoint.camera) || light->IsDontLightMap())
		//	{
		//		return false;
		//	}
		//}

		// Force 'up' to be a pure, straight world-space vertical height axis vector.
		// In GLWall fixed-function space, the Y axis handles floor-to-ceiling elevation.
		up = { 0.0f, 1.0f, 0.0f };

		// Hardcoded explicit cross product (up x fn) to guarantee flawless compilation 
		// without depending on type-overloaded class methods or operators
		right.X = (up.Y * fn.Z) - (up.Z * fn.Y);
		right.Y = (up.Z * fn.X) - (up.X * fn.Z);
		right.Z = (up.X * fn.Y) - (up.Y * fn.X);

		//             DYNAMIC ANAMORPHIC UV COMPRESSION TUNING LAYOUT
		// Configured with explicit tuning floats to let you easily calibrate 
		// horizontal width behaviors for frontal and angled surfaces on the fly.
		// ADJUST THESE TWO VALUES TO CALIBRATE HOW LIGHT LAYS ON WALLS UNDER DIFFERENT ANGLES:
		float inFrontWall = 0.4f;   // WIDTH FRONT: The look-straight multiplier (less is WIDER)
		float inAngleWall = 1.25f;  // WIDTH ANGLE: The sharp edge multiplier (more is NARROWER)

		// 1. Reconstruct camera horizontal look components matching engine layout space
		float lookX = (float)r_viewpoint.Cos; float lookZ = (float)r_viewpoint.Sin;

		// 2. Compute explicit 2D dot product (look . normal) to track wall orientation
		float dotAngle = fabsf((lookX * fn.X) + (lookZ * fn.Z));
		if (dotAngle > 1.0f) dotAngle = 1.0f; if (dotAngle < 0.0f) dotAngle = 0.0f;

		// 3. Linear interpolation mapping:
		// When dotAngle is 1.0 (frontal), scales smoothly down to inFrontWall.
		// When dotAngle is 0.0 (angled), scales tightly up to inAngleWall.
		float dynamicHorizontalScale = inAngleWall - (dotAngle * (inAngleWall - inFrontWall));

		// 4. Dispatch final non-linear anamorphic power scale factor
		right *= dynamicHorizontalScale;  // Apply adaptive width scaling
		up *= 0.32f;                  // Vertical expansion - massive up the whole wall height
	}
	// ===  CAMGLOW DYNLIGHT STRAIGHT (SIMULATE SOFTWARE DIMLIGHT) - FINISH ===

	FVector3 tmpVec = fn * distLight2Wall;
	nearPt = pos + tmpVec;

	float cs = 0.0f;

	// We need this to handle subtractive and other lights if needed.
	// This is the only way to control their intensity for transluscent map geometry surfaces!
	gl_dynlightHandleSpecialLightsLegacy(r, g, b, isTrulyTranslucentGL1xWall, light);
	if (gl_legacy_dynlight_overbright && !g_isCurrentlyGL1xWallsDynlightOverbrightPass)
	{
		if      (lightlevelwall <= 151)                         { cs *= 0.60f; }
		else if (lightlevelwall >= 152 && lightlevelwall < 160) { cs *= 0.55f; }
		else if (lightlevelwall >= 160 && lightlevelwall < 168) { cs *= 0.50f; }
		else if (lightlevelwall >= 168 && lightlevelwall < 176) { cs *= 0.45f; }
		else if (lightlevelwall >= 176 && lightlevelwall < 184) { cs *= 0.40f; }
		else if (lightlevelwall >= 184 && lightlevelwall < 192) { cs *= 0.35f; }
		else if (lightlevelwall >= 192 && lightlevelwall < 200) { cs *= 0.30f; }
		else if (lightlevelwall >= 200 && lightlevelwall < 216) { cs *= 0.25f; }
		else if (lightlevelwall >= 216 && lightlevelwall < 255) { cs *= 0.20f; }
		else if (lightlevelwall >= 255)                         { cs *= 0.15f; }
	}
	if (g_isCurrentlyGL1xWallsDynlightOverbrightPass)
	{
		// For regular modulated dynlights only as others don't need overbright
		cs *= GL1x_light_intens_regular_cur;
	}

	// Compress range ONLY if CVAR is enabled AND dynlight is at least 10% colorful
	// That can get you richer color saturation on farther surfaces and some ledges
	if (gl_legacy_dynlight_compress_range && colorCtx.chroma >= 0.10f)
	{
		// Compress range and make darker lit surfaces brighter and saturated
		float lightRatio = distLight2Wall / radiusWalls;
		if      (lightRatio > 1.0f) lightRatio = 1.0f;
		else if (lightRatio < 0.0f) lightRatio = 0.0f;
		cs = 0.75f - powf(lightRatio, 2.75f);
	}
	else
	{
		cs = 1.0f - (distLight2Wall / radiusWalls);
	}
	if (additive) cs *= 0.2f;
	if (colorCtx.chroma >= 0.10f) cs *= gl_legacy_dynlight_brightness;

	// Alternative way to extract lightlevel
	// Dynamically look up the subsector at light source position to pull map light level
	//subsector_t *light_subsector = P_PointInSubsector(lpos.X, lpos.Y);
	//if (light_subsector && light_subsector->sector)
	//{
	//	lightlevelwall = light_subsector->sector->lightlevel;
	//}

	// Calculate strict bounds for the smooth 64-unit transition window around the threshold
	float lowerBound = (float)gl_legacy_dynlight_saturation_thresh - 32.0f;
	float upperBound = (float)gl_legacy_dynlight_saturation_thresh + 32.0f;

	if ((float)lightlevelwall <= lowerBound)
	{
		// 1. PURE DARK ZONE: 100% full original formula and full dark saturation boost
		r = colorCtx.r * cs;
		g = colorCtx.g * cs;
		b = colorCtx.b * cs;

		current_boost = gl_legacy_dynlight_saturation_dark;
	}
	else if ((float)lightlevelwall >= upperBound)
	{
		// 2. PURE BRIGHT ZONE: 100% tamed formulas and full bright saturation modifier
		if (radiusWalls >= 384.0f && radiusWalls <= 800.0f)
		{
			r = light->GetRed() * invMul286 * cs;
			g = light->GetGreen() * invMul286 * cs;
			b = light->GetBlue() * invMul286 * cs;
		}
		else if (radiusWalls >= 800.0f && radiusWalls <= 1600.0f)
		{
			r = light->GetRed() * invMul322 * cs;
			g = light->GetGreen() * invMul322 * cs;
			b = light->GetBlue() * invMul322 * cs;
		}
		else if (radiusWalls >= 1600.0f)
		{
			r = light->GetRed() * invMul424 * cs;
			g = light->GetGreen() * invMul424 * cs;
			b = light->GetBlue() * invMul424 * cs;
		}
		else
		{
			r = colorCtx.r * cs;
			g = colorCtx.g * cs;
			b = colorCtx.b * cs;
		}

		current_boost = gl_legacy_dynlight_saturation_bright;
	}
	else
	{
		// 3. SMOOTH TRANSITION WINDOW (64 units span centered exactly at thresh)
		float factor = ((float)lightlevelwall - lowerBound) * invMul64; // 0.0 at lowerBound, 1.0 at upperBound

		// Interpolate the saturation boost factor between dark and bright settings
		current_boost = gl_legacy_dynlight_saturation_dark + (gl_legacy_dynlight_saturation_bright - gl_legacy_dynlight_saturation_dark) * factor;

		// Calculate both configurations to perform a seamless blend
		float r_full = colorCtx.r * cs;
		float g_full = colorCtx.g * cs;
		float b_full = colorCtx.b * cs;

		float r_tame, g_tame, b_tame;
		if (radiusWalls >= 384.0f && radiusWalls <= 800.0f)
		{ r_tame = light->GetRed() * invMul286 * cs; g_tame = light->GetGreen() * invMul286 * cs; b_tame = light->GetBlue() * invMul286 * cs; }
		else if (radiusWalls >= 800.0f && radiusWalls <= 1600.0f)
		{ r_tame = light->GetRed() * invMul322 * cs; g_tame = light->GetGreen() * invMul322 * cs; b_tame = light->GetBlue() * invMul322 * cs; }
		else if (radiusWalls >= 1600.0f)
		{ r_tame = light->GetRed() * invMul424 * cs; g_tame = light->GetGreen() * invMul424 * cs; b_tame = light->GetBlue() * invMul424 * cs; }
		else
		{ r_tame = r_full; g_tame = g_full; b_tame = b_full; }

		// Blend them: closer to lowerBound means more full power color intensity
		r = r_full + (r_tame - r_full) * factor;
		g = g_full + (g_tame - g_full) * factor;
		b = b_full + (b_tame - b_full) * factor;
	}

	// Route final pipeline colors with pre-calculated context and radius constraints
	gl_dynlightSaturateLegacy(r, g, b, current_boost, radiusWalls, colorCtx);

	// We need this for huge dynlights not to overexposure the walls mirrored surfaces
	if (g_isCurrentlyGL1xDynlightWallDrawing != nullptr)
	{
		if (isTrulyTranslucentGL1xWall)
		{
			gl_dynlightTameBigLightsOnMirroredSurfacesLegacy(light, r, g, b, radiusWalls, !isTrulyTranslucentGL1xWall);
		}
	}

	//	// Alternative overbright implementation (slow and too much contrast)
	//if(gl_legacy_dynlight_overbright && radius >= 468)
	//{
	//	if      (lightlevelwall <= 136)                          { r *= 1.00f; g *= 1.00f; b *= 1.00f; }
	//	else if (lightlevelwall >= 137 && lightlevelwall < 144)  { r *= 0.95f; g *= 0.95f; b *= 0.95f; }
	//	else if (lightlevelwall >= 144 && lightlevelwall < 152)  { r *= 0.85f; g *= 0.85f; b *= 0.85f; }
	//	else if (lightlevelwall >= 152 && lightlevelwall < 160)  { r *= 0.75f; g *= 0.75f; b *= 0.75f; }
	//	else if (lightlevelwall >= 160 && lightlevelwall < 168)  { r *= 0.71f; g *= 0.71f; b *= 0.71f; }
	//	else if (lightlevelwall >= 168 && lightlevelwall < 176)  { r *= 0.69f; g *= 0.69f; b *= 0.69f; }
	//	else if (lightlevelwall >= 176 && lightlevelwall < 184)  { r *= 0.67f; g *= 0.67f; b *= 0.67f; }
	//	else if (lightlevelwall >= 184 && lightlevelwall < 192)  { r *= 0.65f; g *= 0.65f; b *= 0.65f; }
	//	else if (lightlevelwall >= 192 && lightlevelwall < 200)  { r *= 0.62f; g *= 0.62f; b *= 0.62f; }
	//	else if (lightlevelwall >= 200 && lightlevelwall < 216)  { r *= 0.60f; g *= 0.60f; b *= 0.60f; }
	//	else if (lightlevelwall >= 216 && lightlevelwall < 232)  { r *= 0.57f; g *= 0.57f; b *= 0.57f; }
	//	else if (lightlevelwall >= 232 && lightlevelwall < 248)  { r *= 0.55f; g *= 0.55f; b *= 0.55f; }
	//	else if (lightlevelwall >= 255)                          { r *= 0.50f; g *= 0.50f; b *= 0.50f; }
	//}

	if (light->IsSubtractive())
	{
		// Subtractive anti-light hole pass configuration
		OpenGLRenderer::gl_RenderState.BlendGL1xEquation(GL_FUNC_REVERSE_SUBTRACT);

		float length = float(FVector3(r, g, b).Length());
		r = length - r; g = length - g; b = length - b;
	}
	else
	{
		OpenGLRenderer::gl_RenderState.BlendGL1xEquation(GL_FUNC_ADD);
	}

	//if (isTrulyTranslucentGL1xWall)
	//{
	//	// light inensity can't be higher than transluscency amount
	//	r *= curwallalpha; g *= curwallalpha; b *= curwallalpha;
	//}

	if (isTrulyTranslucentGL1xWall)
	{
		float amodintens = 1.0f;
		{
			if      (curwallalpha < 0.249f)                               { amodintens = 0.320f; }
			else if (curwallalpha >= 0.250f && curwallalpha < 0.500f)     { amodintens = 0.360f; }
			else if (curwallalpha >= 0.500f && curwallalpha < 0.560f)     { amodintens = 0.420f; }
			else if (curwallalpha >= 0.560f && curwallalpha < 0.620f)     { amodintens = 0.400f; }
			else if (curwallalpha >= 0.620f && curwallalpha < 0.650f)     { amodintens = 0.450f; }
			else if (curwallalpha >= 0.650f && curwallalpha < 0.690f)     { amodintens = 0.500f; }
			else if (curwallalpha >= 0.690f && curwallalpha < 0.720f)     { amodintens = 0.650f; }
			else if (curwallalpha >= 0.720f && curwallalpha < 0.750f)     { amodintens = 0.600f; }
			else if (curwallalpha >= 0.750f && curwallalpha < 0.780f)     { amodintens = 0.550f; }
			else if (curwallalpha >= 0.780f && curwallalpha < 0.840f)     { amodintens = 0.650f; }
			else if (curwallalpha >= 0.840f && curwallalpha < 0.910f)     { amodintens = 0.700f; }
			else if (curwallalpha >= 0.910f && curwallalpha < 1.000f)     { amodintens = 0.850f; }
		}

		r *= amodintens; g *= amodintens; b *= amodintens;
	}

	if (g_isCurrentlyGL1xWallsDynlightOverbrightPass)
	{
		r *= gl_legacy_dynlight_overbright_walls;
		g *= gl_legacy_dynlight_overbright_walls;
		b *= gl_legacy_dynlight_overbright_walls;
	}

	OpenGLRenderer::gl_RenderState.SetColor(r, g, b);
	g_isGL1xDynlightAcamglowOnWall = false; // reset the flag otherwise overbright will be disabled for all dynlight types!
	return true;
}

bool gl_SetupLightFlat(int group, Plane & p, FDynamicLight * light, FVector3 & nearPt, FVector3 & up, FVector3 & right, float & scale, bool checkside, bool additive)
{
	if (light == nullptr) return false; // some crash prevention trick

	FVector3 fn, pos;
	int lightlevelflat = 128;
	float r, g, b;
	float current_boost;
	float curflatalpha = 1.0f;
	const float invMul64 = 1.0f / 64.0f;
	const float invMul255 = 1.0f / 255.0f;
	const float invMul284 = 1.0f / 284.0f;
	const float invMul322 = 1.0f / 322.0f;
	const float invMul444 = 1.0f / 444.0f;
	bool isCamglowDynlightHere = false;

	if (g_isCurrentlyGL1xDynlightFlatDrawing != nullptr)
	{
		lightlevelflat = g_isCurrentlyGL1xDynlightFlatDrawing->lightlevel;
		curflatalpha = g_isCurrentlyGL1xDynlightFlatDrawing->alpha;
		isTrulyTranslucentGL1xFlat = curflatalpha <= 0.99f;
	}

	DVector3 lpos = light->PosRelative(group);

	distLight2Flat = fabsf(p.DistToPoint(lpos.X, lpos.Z, lpos.Y));
	radiusFlats = light->GetRadius();

	if (light != nullptr && light->IsDynlightCamglow()) { g_isGL1xDynlightAcamglowOnWall = true; isCamglowDynlightHere = true; }
	if (OpenGLRenderer::gl_RenderState.mPassType == (EPassType)GLPASS_LIGHTTEX_OVERBRIGHT2_LEGACY)
	if (isCamglowDynlightHere) return false; // Turn off overbright for the camglow dynlight

	//Camglow radius is 2x to reduce BSP traversal early exit surface skip artifacts in GL1x/GL2x
	if (isCamglowDynlightHere) radiusFlats *= 0.5f;

	if (radiusFlats <= 0.f) return false;
	if (distLight2Flat > radiusFlats) return false;
	if (checkside && gl_lights_checkside && p.PointOnSide(lpos.X, lpos.Z, lpos.Y))
	{
		return false;
	}
	if (!light->visibletoplayer)
	{
		return false;
	}

	// 1. INITIALIZE LIGHT COLOR CONTEXT ONCE ON THE CPU
	FLightColorContext colorCtx(light);

	// Decrease distance to dynlight to have a richer color saturation in the far
	float                                                       distFactor = 1.0f;
	if      (radiusFlats >= 384.0f && radiusFlats <= 800.0f)    distFactor = 0.88f;
	else if (radiusFlats >= 800.0f && radiusFlats <= 1600.0f)   distFactor = 0.77f;
	else if (radiusFlats >= 1600.0f && radiusFlats <= 3000.0f)  distFactor = 0.74f;
	else if (radiusFlats >= 3000.0f && radiusFlats <= 6000.0f)  distFactor = 0.72f;
	else if (radiusFlats >= 6000.0f && radiusFlats <= 12000.0f) distFactor = 0.67f;
	else if (radiusFlats >= 12000.0f)                           distFactor = 0.64f;

	scale = 1.0f / ((2.25f * radiusFlats) - (distLight2Flat * distFactor));

	pos = { (float)lpos.X, (float)lpos.Z, (float)lpos.Y };
	fn = p.Normal();
	fn.GetRightUp(right, up);

	// ===  CAMGLOW DYNLIGHT STRAIGHT (SIMULATE SOFTWARE DIMLIGHT) - START ===
	// The whole thing is done with a zscript spawning a special dynlight on players
	// But to achieve STRAIGHT diminished lighting just like in software renderer for:
	// *Walls: expand the dynlight blob texture (gllight.png) vertically
	// *Flats: expand it horizontaly, turn it 90d and spin along the view direction
	//
	// THE FLATS IMPLEMENTATION GOES BELOW:
	//
	// TWO-WAY SLOPE & CEILING ALIGNER
	// FIX CEILING FLIP BUG: Automatically detect if we are rendering a sloped
	// 3DFLOOR CEILING (fn.Y < 0) and invert the rotation axis vectors inside
	// the Rodrigues formula.
	// Makes software ellipse rotation on BOTH floors and ceilings synchronized.
	// ==================================================================
	if (light != nullptr && light->IsDynlightCamglow() && gl_fogmode != 2)
	{
		//	// This network check is not necessary but why not?
		//if (light->target.pp != nullptr && r_viewpoint.camera != nullptr)
		//{
		//	// EXTRACT THE OWNER VIA MASTER POINTER NATIVELY
		//	AActor *rawPlayerOwner = light->target.pp->master.pp;
		//
		//	// NETWORK CLIENT-SIDE FILTER: If the rendering camera belongs to someone else,
		//	// or the light has LF_DONTLIGHTMAP flag, completely wipe this light pass from the GPU!
		//	if ((light->IsDontLightOthers() && rawPlayerOwner != nullptr && rawPlayerOwner != r_viewpoint.camera) || light->IsDontLightMap())
		//	{
		//		return false;
		//	}
		//}

		// 1. Fetch current player camera horizontal viewport orientation matrices
		float s = (float)r_viewpoint.Sin; float c = (float)r_viewpoint.Cos;

		// 2. Save the engine's pre-aligned unit-length baseline vectors
		FVector3 r_old = right; FVector3 u_old = up;

		// Extract normal components (3D rotation axis)
		float nx = fn.X; float ny = fn.Y; float nz = fn.Z;

		// Check if the flat polygon has any slope tilt context at all
		bool isSlope = (fabsf(nx) > 0.001f || fabsf(nz) > 0.001f);

		if (isSlope)
		{
			// CEILING NORMAL INVERSION SECURITY LAYER
			// If fn.Y is negative, it means the GPU is rendering a CEILING slope from below
			// Invert the rotation axes registers (nx, ny, nz) to force the Rodrigues matrix 
			// to spin in the exact correct direction, completely zeroing out ceiling mirror bugs
			if (ny < 0.0f) { nx = -nx; ny = -ny; nz = -nz; }

			// Both slope contexts inside the fixed-function pipeline require an inverted 
			// rotation matrix phase (revS = -s) to seamlessly cancel out the hardware drift mirroring
			float revS = -s;
			bool needAxisSwap = false;

			if (fabsf(nx) > fabsf(nz))
			{
				// West-East slanted slopes
				needAxisSwap = true; // Swap axes to force the spot broad sideways
			}
			else
			{
				// North-South slanted slopes
				needAxisSwap = false; // Axes are already naturally aligned, keep standard layout
			}

			// 3. Execute reverse 3D Rodrigues rotation for the "right" vector over slopes
			float r_dot = (r_old.X * nx) + (r_old.Y * ny) + (r_old.Z * nz);
			float r_crossX = (ny * r_old.Z) - (nz * r_old.Y);
			float r_crossY = (nz * r_old.X) - (nx * r_old.Z);
			float r_crossZ = (nx * r_old.Y) - (ny * r_old.X);

			right.X = (r_old.X * c) + (r_crossX * revS) + (nx * r_dot * (1.0f - c));
			right.Y = (r_old.Y * c) + (r_crossY * revS) + (ny * r_dot * (1.0f - c));
			right.Z = (r_old.Z * c) + (r_crossZ * revS) + (nz * r_dot * (1.0f - c));

			// 4. Execute reverse 3D Rodrigues rotation for the "up" vector over slopes identically
			float u_dot = (u_old.X * nx) + (u_old.Y * ny) + (u_old.Z * nz);
			float u_crossX = (ny * u_old.Z) - (nz * u_old.Y);
			float u_crossY = (nz * u_old.X) - (nx * u_old.Z);
			float u_crossZ = (nx * u_old.Y) - (ny * u_old.X);

			up.X = (u_old.X * c) + (u_crossX * revS) + (nx * u_dot * (1.0f - c));
			up.Y = (u_old.Y * c) + (u_crossY * revS) + (ny * u_dot * (1.0f - c));
			up.Z = (u_old.Z * c) + (u_crossZ * revS) + (nz * u_dot * (1.0f - c));

			// Apply the dynamic axis swap only when the West-East slope normal dictates it
			if (needAxisSwap)
			{
				FVector3 tempRight = right;
				right = up;
				up = tempRight;
			}
		}
		else
		{
			// Flat non-slope flats path
			right.X = (r_old.X * c) - (r_old.Z * s);
			right.Z = (r_old.X * s) + (r_old.Z * c);

			up.X = (u_old.X * c) - (u_old.Z * s);
			up.Z = (u_old.X * s) + (u_old.Z * c);
		}

		// 5. Long-range software ligt blob texture projection
		right *= 1.0f; // How far away from the player it lights (less is farther)

		// 6. Compensate tilted slopes projection
		float slopePerspectiveCorrection = 0.25f;
		if (isSlope && fabsf(ny) > 0.01f)
		{
			slopePerspectiveCorrection = 0.25f * fabsf(ny); // Use pre-inverted 'ny' register
		}

		// How far it stretches aside (left-right) from camera (less is farther)
		up *= slopePerspectiveCorrection;
	}
	// ===  CAMGLOW DYNLIGHT STRAIGHT (SIMULATE SOFTWARE DIMLIGHT) - FINISH ===

	FVector3 tmpVec = fn * distLight2Flat;
	nearPt = pos + tmpVec;

	float cs = 0.0f;

	// We need this to tame down subtractive and other lights if needed.
	// This is the only way to control their intensity for transluscent map geometry surfaces!
	gl_dynlightHandleSpecialLightsLegacy(r, g, b, isTrulyTranslucentGL1xFlat, light);

	if (gl_legacy_dynlight_overbright && !g_isCurrentlyGL1xFlatsDynlightOverbrightPass)
	{
		if      (lightlevelflat <= 151)                         { cs *= 0.60f; }
		else if (lightlevelflat >= 152 && lightlevelflat < 160) { cs *= 0.55f; }
		else if (lightlevelflat >= 160 && lightlevelflat < 168) { cs *= 0.50f; }
		else if (lightlevelflat >= 168 && lightlevelflat < 176) { cs *= 0.45f; }
		else if (lightlevelflat >= 176 && lightlevelflat < 184) { cs *= 0.40f; }
		else if (lightlevelflat >= 184 && lightlevelflat < 192) { cs *= 0.35f; }
		else if (lightlevelflat >= 192 && lightlevelflat < 200) { cs *= 0.30f; }
		else if (lightlevelflat >= 200 && lightlevelflat < 216) { cs *= 0.25f; }
		else if (lightlevelflat >= 216 && lightlevelflat < 255) { cs *= 0.20f; }
		else if (lightlevelflat >= 255)                         { cs *= 0.15f; }
	}
	if (g_isCurrentlyGL1xFlatsDynlightOverbrightPass)
	{
		// For regular modulated dynlights only as others don't need overbright
		cs *= GL1x_light_intens_regular_cur;
	}

	// Compress range ONLY if CVAR is enabled AND dynlight is at least 10% colorful
	// That can get you richer color saturation on farther surfaces and some ledges
	if (gl_legacy_dynlight_compress_range && colorCtx.chroma >= 0.10f)
	{
		// Compress range and make darker lit surfaces brighter and saturated
		float    lightRatio = distLight2Flat / radiusFlats;
		if      (lightRatio > 1.0f) lightRatio = 1.0f;
		else if (lightRatio < 0.0f) lightRatio = 0.0f;
		cs = 0.75f - powf(lightRatio, 2.75f);
	}
	else
	{
		cs = 1.0f - (distLight2Flat / radiusFlats);
	}
	if (additive) cs *= 0.2f;
	if (colorCtx.chroma >= 0.10f) cs *= gl_legacy_dynlight_brightness;

	// Alternative way to extract lightlevel
	// Dynamically look up the subsector at light source position to pull map light level
	//subsector_t *light_subsector = P_PointInSubsector(lpos.X, lpos.Y);
	//if (light_subsector && light_subsector->sector)
	//{
	//	lightlevelflat = light_subsector->sector->lightlevel;
	//}

	// Calculate strict bounds for the smooth 64-unit transition window around the threshold
	float lowerBound = (float)gl_legacy_dynlight_saturation_thresh - 32.0f;
	float upperBound = (float)gl_legacy_dynlight_saturation_thresh + 32.0f;

	if ((float)lightlevelflat <= lowerBound)
	{
		// 1. PURE DARK ZONE: 100% full original formula and full dark saturation boost
		r = colorCtx.r * cs;
		g = colorCtx.g * cs;
		b = colorCtx.b * cs;

		current_boost = gl_legacy_dynlight_saturation_dark;
	}
	else if ((float)lightlevelflat >= upperBound)
	{
		// 2. PURE BRIGHT ZONE: 100% tamed formulas and full bright saturation modifier
		if (radiusFlats >= 384.0f && radiusFlats <= 800.0f)
		{
			r = light->GetRed() * invMul284 * cs;
			g = light->GetGreen() * invMul284 * cs;
			b = light->GetBlue() * invMul284 * cs;
		}
		else if (radiusFlats >= 800.0f && radiusFlats <= 1600.0f)
		{
			r = light->GetRed() * invMul322 * cs;
			g = light->GetGreen() * invMul322 * cs;
			b = light->GetBlue() * invMul322 * cs;
		}
		else if (radiusFlats >= 1600.0f)
		{
			r = light->GetRed() * invMul444 * cs;
			g = light->GetGreen() * invMul444 * cs;
			b = light->GetBlue() * invMul444 * cs;
		}
		else
		{
			r = colorCtx.r * cs;
			g = colorCtx.g * cs;
			b = colorCtx.b * cs;
		}

		current_boost = gl_legacy_dynlight_saturation_bright;
	}
	else
	{
		// 3. SMOOTH SMOOTH TRANSITION WINDOW (64 units span centered exactly at thresh)
		float factor = ((float)lightlevelflat - lowerBound) * invMul64; // 0.0 at lowerBound, 1.0 at upperBound

		// Interpolate the saturation boost factor between dark and bright settings
		current_boost = gl_legacy_dynlight_saturation_dark + (gl_legacy_dynlight_saturation_bright - gl_legacy_dynlight_saturation_dark) * factor;

		// Calculate both configurations to perform a seamless blend
		float r_full = colorCtx.r * cs;
		float g_full = colorCtx.g * cs;
		float b_full = colorCtx.b * cs;

		float r_tame, g_tame, b_tame;
		if (radiusFlats >= 384.0f && radiusFlats <= 800.0f)
		{ r_tame = light->GetRed() * invMul284 * cs; g_tame = light->GetGreen() * invMul284 * cs; b_tame = light->GetBlue() * invMul284 * cs; }
		else if (radiusFlats >= 800.0f && radiusFlats <= 1600.0f)
		{ r_tame = light->GetRed() * invMul322 * cs; g_tame = light->GetGreen() * invMul322 * cs; b_tame = light->GetBlue() * invMul322 * cs; }
		else if (radiusFlats >= 1600.0f)
		{ r_tame = light->GetRed() * invMul444 * cs; g_tame = light->GetGreen() * invMul444 * cs; b_tame = light->GetBlue() * invMul444 * cs; }
		else
		{ r_tame = r_full; g_tame = g_full; b_tame = b_full; }

		// Blend them: closer to lowerBound means more full power color intensity
		r = r_full + (r_tame - r_full) * factor;
		g = g_full + (g_tame - g_full) * factor;
		b = b_full + (b_tame - b_full) * factor;
	}

	// Route final pipeline colors into the custom saturation encapsulation pass with radius constraints
	gl_dynlightSaturateLegacy(r, g, b, current_boost, radiusFlats, colorCtx);

	// We need this for huge dynlights not to overexposure the flat mirrored surfaces
	if (g_isCurrentlyGL1xDynlightFlatDrawing != nullptr)
	{
		if (isTrulyTranslucentGL1xFlat)
		{
			gl_dynlightTameBigLightsOnMirroredSurfacesLegacy(light, r, g, b, radiusFlats, !isTrulyTranslucentGL1xFlat);
		}
	}

	//	// Alternative overbright implementation (slow and too much contrast)
	//if(gl_legacy_dynlight_overbright && radius >= 468)
	//{
	//	if      (lightlevelflat <= 136)                          { r *= 1.00f; g *= 1.00f; b *= 1.00f; }
	//	else if (lightlevelflat >= 137 && lightlevelflat < 144)  { r *= 0.95f; g *= 0.95f; b *= 0.95f; }
	//	else if (lightlevelflat >= 144 && lightlevelflat < 152)  { r *= 0.85f; g *= 0.85f; b *= 0.85f; }
	//	else if (lightlevelflat >= 152 && lightlevelflat < 160)  { r *= 0.75f; g *= 0.75f; b *= 0.75f; }
	//	else if (lightlevelflat >= 160 && lightlevelflat < 168)  { r *= 0.71f; g *= 0.71f; b *= 0.71f; }
	//	else if (lightlevelflat >= 168 && lightlevelflat < 176)  { r *= 0.69f; g *= 0.69f; b *= 0.69f; }
	//	else if (lightlevelflat >= 176 && lightlevelflat < 184)  { r *= 0.67f; g *= 0.67f; b *= 0.67f; }
	//	else if (lightlevelflat >= 184 && lightlevelflat < 192)  { r *= 0.65f; g *= 0.65f; b *= 0.65f; }
	//	else if (lightlevelflat >= 192 && lightlevelflat < 200)  { r *= 0.62f; g *= 0.62f; b *= 0.62f; }
	//	else if (lightlevelflat >= 200 && lightlevelflat < 216)  { r *= 0.60f; g *= 0.60f; b *= 0.60f; }
	//	else if (lightlevelflat >= 216 && lightlevelflat < 232)  { r *= 0.57f; g *= 0.57f; b *= 0.57f; }
	//	else if (lightlevelflat >= 232 && lightlevelflat < 248)  { r *= 0.55f; g *= 0.55f; b *= 0.55f; }
	//	else if (lightlevelflat >= 255)                          { r *= 0.50f; g *= 0.50f; b *= 0.50f; }
	//}

	if (light->IsSubtractive())
	{
		OpenGLRenderer::gl_RenderState.BlendGL1xEquation(GL_FUNC_REVERSE_SUBTRACT);
		float length = float(FVector3(r, g, b).Length());
		r = length - r; g = length - g; b = length - b;
	}
	else
	{
		OpenGLRenderer::gl_RenderState.BlendGL1xEquation(GL_FUNC_ADD);
	}

	//if (isTrulyTranslucentGL1xFlat)
	//{
	//	// light inensity can't be higher than transluscency amount
	//	r *= curflatalpha; g *= curflatalpha; b *= curflatalpha;
	//}

	if (isTrulyTranslucentGL1xFlat)
	{
		float amodintens = 1.0f;
		{
			if      (curflatalpha < 0.249f)                               { amodintens = 0.320f; }
			else if (curflatalpha >= 0.250f && curflatalpha < 0.500f)     { amodintens = 0.360f; }
			else if (curflatalpha >= 0.500f && curflatalpha < 0.560f)     { amodintens = 0.420f; }
			else if (curflatalpha >= 0.560f && curflatalpha < 0.620f)     { amodintens = 0.400f; }
			else if (curflatalpha >= 0.620f && curflatalpha < 0.650f)     { amodintens = 0.450f; }
			else if (curflatalpha >= 0.650f && curflatalpha < 0.690f)     { amodintens = 0.500f; }
			else if (curflatalpha >= 0.690f && curflatalpha < 0.720f)     { amodintens = 0.650f; }
			else if (curflatalpha >= 0.720f && curflatalpha < 0.750f)     { amodintens = 0.600f; }
			else if (curflatalpha >= 0.750f && curflatalpha < 0.780f)     { amodintens = 0.550f; }
			else if (curflatalpha >= 0.780f && curflatalpha < 0.840f)     { amodintens = 0.650f; }
			else if (curflatalpha >= 0.840f && curflatalpha < 0.910f)     { amodintens = 0.700f; }
			else if (curflatalpha >= 0.910f && curflatalpha < 1.000f)     { amodintens = 0.850f; }
		}

		r *= amodintens; g *= amodintens; b *= amodintens;
	}

	if (g_isCurrentlyGL1xFlatsDynlightOverbrightPass)
	{
		r *= gl_legacy_dynlight_overbright_flats;
		g *= gl_legacy_dynlight_overbright_flats;
		b *= gl_legacy_dynlight_overbright_flats;
	}

	OpenGLRenderer::gl_RenderState.SetColor(r, g, b);
	g_isGL1xDynlightAcamglowOnFlat = false; // reset the flag otherwise overbright will be disabled for all dynlight types!
	return true;
}

//==========================================================================
//
//
//
//==========================================================================

bool gl_SetupLightTexture()
{
	// Verify that our appended texture identifier token is registered and valid
	if (!OpenGLRenderer::GLRenderer->gl1xLight.isValid()) return false;

	// Safely resolve FGameTexture pointer from FTextureID using the new TexMan instance
	FGameTexture* gameTex = TexMan.GetGameTexture(OpenGLRenderer::GLRenderer->gl1xLight);
	if (gameTex == nullptr) return false;

	// Validate texture using GZDoom v4x signature (scaleflags parameter, using 0 for clean default)
	FMaterial* pat = FMaterial::ValidateTexture(gameTex, 0);
	if (pat == nullptr) return false;

	// Binds the materials directly into the state machine cache using our restored overload method
	// CLAMP_XY_NOMIP constant should map to your texture layout configs (typically 0 or clamping enum)
	OpenGLRenderer::gl_RenderState.SetGL1xMaterial(pat, CLAMP_XY_NOMIP, 0, -1, false);
	return true;
}

//==========================================================================
//
// Project dynlights onto LIGHTEFFECTS_LEGACY cases. Huge FPS gains.
//
//==========================================================================

// WALLS ARE DISABLED because done inside gl_walls_draw.cpp, case GLPASS_LIGHTEFFECTS_LEGACY.
// Because as soon as encapsulated, works for a short while, then turns off! IDK why yet.
//bool gl_GetWallStaticLightmaps(seg_t *seg, float *ztop, float *zbottom, float x1, float y1,
//					float x2, float y2, float overallGlowIntensity, float outColors[4][3])
//{
//	if (g_isGL1xDynlightAcamglowOnWall) return false;
//	if (!seg || !seg->sidedef || !gl_lights || !gl_legacy_dynlight_baked_huge) return false;
//
//	// Reset accumulation buffer matrices cleanly back to zeroes baseline
//	for (int i = 0; i < 4; i++)
//	{
//		outColors[i][0] = outColors[i][1] = outColors[i][2] = 0.0f;
//	}
//
//	FLightNode *node = seg->sidedef->lighthead;
//	if (node == nullptr) return false;
//
//	bool hasStaticLight = false;
//
//	float zb0 = (float)zbottom[0]; float zt0 = (float)ztop[0];
//	float zt1 = (float)ztop[1];    float zb1 = (float)zbottom[1];
//
//	// Map the 4 physical vertex anchors tracking points onto a DVector3 stack
//	DVector3 vtxPoints[4];
//	vtxPoints[0] = DVector3(x1, y1, zb0); // Vertex 0: Bottom-Left
//	vtxPoints[1] = DVector3(x1, y1, zt0); // Vertex 1: Top-Left
//	vtxPoints[2] = DVector3(x2, y2, zt1); // Vertex 2: Top-Right
//	vtxPoints[3] = DVector3(x2, y2, zb1); // Vertex 3: Bottom-Right
//
//	const float invMul255 = 1.0f / 255.0f;
//
//	while (node) // Parse through the active segment light nodes thread chain
//	{
//		FDynamicLight *light = node->lightsource;
//
//		// Target exclusively large broad static lamps (> 468 radius) to kill multi-pass lag
//		if (light->GetRadius() > 468.0f)
//		{
//			hasStaticLight = true;
//
//			// Extract raw un-corrupted RGB spectrum channels straight from light memory map
//			float lr = (float)light->GetRed() * invMul255;
//			float lg = (float)light->GetGreen() * invMul255;
//			float lb = (float)light->GetBlue() * invMul255;
//
//			float radius = light->GetRadius();
//			DVector3 lpos = light->PosRelative(seg->frontsector->PortalGroup);
//
//			// Compute true 3D spatial spherical distance attenuation separately for all 4 corners
//			for (int i = 0; i < 4; i++)
//			{
//				DVector3 dirToVert = vtxPoints[i] - lpos; // Distance vector from center to angle corner
//				float true3DDist = (float)dirToVert.Length();
//
//				if (true3DDist >= radius) continue; // Out of boundary sphere limits
//
//				// Smooth linear falloff curve for circular gradients on walls
//				float ratio = true3DDist / radius;
//				float cs = 1.0f - ratio;
//
//				// Apply your oldskool soft parabola curve to melt lighting edges seamlessly
//				cs = powf(cs, 2.0f);
//
//				if (light->IsAdditive()) cs *= 0.2f; // Keep additive channels bounded
//
//				// Sum vectors directly into size-4 local matrix channels
//				outColors[i][0] += lr * cs;
//				outColors[i][1] += lg * cs;
//				outColors[i][2] += lb * cs;
//			}
//		}
//		node = node->nextLight; // Move safely down the chain link
//	}
//}

// Computes per-vertex static light color for one subsector.
// Returns true if at least one large dynamic light reaches any vertex.
bool gl_FillSubsectorGouraud(subsector_t *sub, const secplane_t &plane, int group, FFlatVertexGouraud *out, bool ceiling, float scale)
{
	if (!sub || !sub->sector || !gl_lights || !gl_legacy_dynlight_baked_huge) return false;
	if (sub->numlines == 0 || !sub->firstline) return false;

	const float invMul32 = 1.0f / 32.0f;
	const float invMul48 = 1.0f / 48.0f;
	const float invMul64 = 1.0f / 64.0f;
	const float invMul255 = 1.0f / 255.0f;
	const float g_hugeGL1xDynlightBakeIntensity = 0.75f;
	bool hasStaticLight = false;
	bool isCamglowDynlightHere = false;

	// Cache the unique sector pointer reference to avoid constant register dereferencing
	sector_t *sec = sub->sector;

	// Iterate over all real vertices of the subsector
	for (unsigned int k = 0; k < sub->numlines; k++)
	{
		vertex_t *vt = sub->firstline[k].v1;
		if (!vt) continue;

		float vx = (float)vt->fX();
		float vy = (float)vt->fY();
		float vz = (float)plane.ZatPoint(vt); // plane height at this exact vertex

		float r = 0.0f, g = 0.0f, b = 0.0f;

		for (FLightNode *node = sub->section->lighthead; node; node = node->nextLight)
		{
			FDynamicLight *light = node->lightsource;
			if (light->IsDynlightCamglow()) isCamglowDynlightHere = true;

			// We HAVE TO bake camglow even though it looks worse because otherwise it removes baked light
			if (!light || !light->IsActive()) continue;

			float radius = (float)light->GetRadius();
			if (radius < 468.0f) continue;

			// THE LIGHT SIDER-CHECK CLIP:
			if (gl_lights_checkside)
			{
				float planeh = (float)plane.ZatPoint(light->Pos); // Find plane Z height right at lamp coordinates
				if ((planeh < (float)light->Z() && ceiling) || (planeh > (float)light->Z() && !ceiling))
				{
					continue; // do NOT process this side
				}
			}

			DVector3 lpos = light->PosRelative(group);
			float dx = vx - (float)lpos.X;
			float dy = vy - (float)lpos.Y;
			float dz = vz - (float)lpos.Z;
			float distSq = dx * dx + dy * dy + dz * dz;
			float radiusSq = radius * radius;
			if (distSq >= radiusSq) continue;

			// Decrease distance to dynlight to have a richer color saturation in the far
			float                                              distFactor = 0.88f;
			if      (radius >= 384.0f  && radius <= 800.0f)    distFactor = 0.78f;
			else if (radius >= 800.0f  && radius <= 1600.0f)   distFactor = 0.67f;
			else if (radius >= 1600.0f && radius <= 3000.0f)   distFactor = 0.64f;
			else if (radius >= 3000.0f && radius <= 6000.0f)   distFactor = 0.62f;
			else if (radius >= 6000.0f && radius <= 12000.0f)  distFactor = 0.57f;
			else if (radius >= 12000.0f)                       distFactor = 0.54f;

			// Extract raw un-corrupted RGB channels directly from source light right now
			float lr = (float)light->GetRed()   * invMul255;
			float lg = (float)light->GetGreen() * invMul255;
			float lb = (float)light->GetBlue()  * invMul255;

			// ADVANCED CHROMATIC SATURATION & HUE MATRIX CONVEYOR
			const bool boostSaturationNonlinear = true;

			if (!light->IsDynlightCamglow() && boostSaturationNonlinear)
			{
				int flatLightLevel = sec->lightlevel;
				float lowerBound = (float)gl_legacy_dynlight_saturation_thresh - 32.0f;
				float upperBound = (float)gl_legacy_dynlight_saturation_thresh + 32.0f;
				float cvar_boost = (float)gl_legacy_dynlight_saturation_dark;

				if ((float)flatLightLevel <= lowerBound)
				{
					cvar_boost = (float)gl_legacy_dynlight_saturation_dark;
				}
				else if ((float)flatLightLevel >= upperBound)
				{
					cvar_boost = (float)gl_legacy_dynlight_saturation_bright;
				}
				else
				{
					// Smooth interpolation pass with inverted baseline bounds mapping
					float factor = ((float)flatLightLevel - lowerBound) * invMul64;
					cvar_boost = (float)gl_legacy_dynlight_saturation_dark + 
						((float)gl_legacy_dynlight_saturation_bright - 
						(float)gl_legacy_dynlight_saturation_dark) * factor;
				}

				// DYNAMIC DARKNESS SATURATION DAMPER (4x WEAKER AT LIGHTLEVEL <= 136)
				// Smoothly scales down the massive 1.45f boost inside dark areas to prevent radioactive glows.
				// Interpolates from 0.25x power at <=136 up to full 1.0x power at >=184.
				float linearScaleFactor = 1.0f;

				if (flatLightLevel <= 136) linearScaleFactor = 0.25f;      // 4 times weaker in the dark
				else if (flatLightLevel >= 184) linearScaleFactor = 1.0f;  // Full 100% power on bright surfaces/roofs
				else
				{
					// Clean mathematical slope over the 136->184 gap (48 units range delta)
					linearScaleFactor = 0.25f + (1.0f - 0.25f) * ((float)(flatLightLevel - 136) * invMul48);
				}

				// Apply our intellectual linear scale dampener straight onto the combined boost curves
				float finalSaturationBoost = (1.45f + cvar_boost) * linearScaleFactor;

				// 3. Fire direct chroma-amplification pass
				float avgColor = (lr + lg + lb) * 0.3333f;
				lr += (lr - avgColor) * finalSaturationBoost;
				lg += (lg - avgColor) * finalSaturationBoost;
				lb += (lb - avgColor) * finalSaturationBoost;

				// 4. Construct a native light color context on the fly for Hue-Shifting compliance
				FLightColorContext colorCtx(light);
				float maxC = lr; if (lg > maxC) maxC = lr; if (lb > maxC) maxC = lb;
				float minC = lr; if (lg < minC) minC = lg; if (lb < minC) minC = lb;

				colorCtx.maxColor = maxC;
				colorCtx.chroma = maxC - minC;
				colorCtx.localChroma = colorCtx.chroma;

				// 5. Fire full spectrum hue translation pass straight into our local copy variables
				gl_dynlightSaturateLegacy(lr, lg, lb, 0.0f, radius, colorCtx);
			}

			// Linear falloff with a soft parabola to melt light edges modified by saturation distFactor
			float trueDist = sqrtf(distSq);
			float saturatedDist = trueDist * distFactor;

			if (saturatedDist < 0.0f) saturatedDist = 0.0f;

			float t = 1.0f - saturatedDist / radius;
			if (t < 0.0f) t = 0.0f; // Safe clamp floor bounds
			t = t * t;

			// Accumulate securely using our hyper-saturated and hue-shifted spectrum channels
			r += lr * t * g_hugeGL1xDynlightBakeIntensity;
			g += lg * t * g_hugeGL1xDynlightBakeIntensity;
			b += lb * t * g_hugeGL1xDynlightBakeIntensity;
			hasStaticLight = true;
		}

		// Clamp to prevent fixed-function color overflow
		if (r > 1.0f) r = 1.0f; if (g > 1.0f) g = 1.0f; if (b > 1.0f) b = 1.0f;

		// Force-injects 1.0f into the vertex alpha channel to secure un-corrupted texture pipelines
		out[k].Set(vx, vz, vy, 0.0f, 0.0f, r * scale, g * scale, b * scale, 1.0f);
	}

	return hasStaticLight;
}

// Declared in flatvertices.h, struct FFlatVertex.
void FFlatVertexGouraud::SetFlatGL1xVertex(vertex_t* vt, const secplane_t& plane)
{
	x = vt->fX();
	y = vt->fY();
	z = plane.ZatPoint(vt);
	u = vt->fX() / 64.f;
	v = -vt->fY() / 64.f;
}

// Fill texture coordinates from world position, the same way the regular
// flat vertices are built. Orientation and scale come from the texture matrix
// set by gl_SetPlaneTextureRotation, so the gouraud pass matches the base flat - works fine!
void gl_FillSubsectorUV(subsector_t *sub, const secplane_t &plane, FMaterial *gltexture, FFlatVertexGouraud *out)
{
	for (unsigned int k = 0; k < sub->numlines; k++)
	{
		vertex_t *vt = sub->firstline[k].v1;

		// Reuse the exact same UV computation as the regular flat vertices
		FFlatVertexGouraud tmp;
		tmp.SetFlatGL1xVertex(vt, plane);

		out[k].u = tmp.u;
		out[k].v = tmp.v;
	}
}

void gl_DrawGouraudFan(const FFlatVertexGouraud *verts, int count)
{
	if (!verts || count < 3) return;

	// Unbind ZDoom's shared VBO buffers to streams parameters from client memory
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);

	// Feed raw memory pointers offsets straight matching FFlatVertexGouraud sizes
	glVertexPointer(3, GL_FLOAT, sizeof(FFlatVertexGouraud), &verts[0].x);
	glTexCoordPointer(2, GL_FLOAT, sizeof(FFlatVertexGouraud), &verts[0].u);
	glColorPointer(4, GL_FLOAT, sizeof(FFlatVertexGouraud), &verts[0].r);

	// Run true hardware fixed-function Gouraud shading fans rasterization inside GPU!
	glDrawArrays(GL_TRIANGLE_FAN, 0, count);

	// FIXED-FUNCTION REGISTRY SANITIZER
	// CRITICAL FIX: Aggressively disable client arrays to block driver leaks.
	// Nullify pointer registries completely so upcoming sprites render perfectly 
	// without encountering dead stack frame references under any lookup angles
	glDisableClientState(GL_COLOR_ARRAY);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glDisableClientState(GL_VERTEX_ARRAY);

	glVertexPointer(3, GL_FLOAT, 0, nullptr);
	glTexCoordPointer(2, GL_FLOAT, 0, nullptr);
	glColorPointer(4, GL_FLOAT, 0, nullptr);

	// THE MONOLITHIC RE-ANCHOR SHUNT:
	// Force-rebind engine vertex buffers matrix states to keep state-cacher clean
	OpenGLRenderer::gl_RenderState.ResetVertexBuffer(); // Prevents weapon HUD and sprites from vanishing
}



// Worse and unused flat colored dynlight baking for historical record
bool gl_GetFlatStaticLightmaps(subsector_t* sub, const HWSectorPlane& secPlane, bool ceiling, float* lightmapColor)
{
	if (g_isGL1xDynlightAcamglowOnFlat) return false;
	if (!sub || !sub->sector || !gl_lights || !gl_legacy_dynlight_baked_huge) return false;

	lightmapColor[0] = lightmapColor[1] = lightmapColor[2] = 0.0f;
	bool hasStaticLight = false;
	bool isCamglowDynlightHere = false;

	FLightNode* node = sub->section->lighthead;
	const float invMul255 = 1.0f / 255.0f;

	while (node)
	{
		FDynamicLight* light = node->lightsource;
		if (light->IsDynlightCamglow()) isCamglowDynlightHere = true;

		// Bake only large static/environmental emitters (radius >= 468)
		// We HAVE TO bake camglow even though it looks worse because otherwise it removes baked light
		if (light && light->IsActive() && /* !isCamglowDynlightHere && */ light->GetRadius() >= 468.0f)
		{
			// THE SIDE-CHECK CLIP
			// If enabled, query the plane height level right at the lamp center.
			// Rejects lights sitting on the wrong side of the geometry instantly!
			if (gl_lights_checkside)
			{
				float planeh = (float)secPlane.plane.ZatPoint(light->Pos);
				if ((planeh < (float)light->Z() && ceiling) || (planeh > (float)light->Z() && !ceiling))
				{
					node = node->nextLight;
					continue;
				}
			}

			float radius = light->GetRadius();
			float radiusSq = radius * radius;

			// Extract subsector center coordinate points natively
			float flatMidX = (float)sub->sector->centerspot.X;
			float flatMidY = (float)sub->sector->centerspot.Y;

			// Calculate plane height dynamically at centerspot based on ceiling flag
			float flatMidZ = (float)secPlane.plane.ZatPoint(sub->sector->centerspot);

			DVector3 lpos = light->PosRelative(sub->sector->PortalGroup);

			// Calculate 3D distance from large light center to flat polygon center spot
			float dx = flatMidX - (float)lpos.X;
			float dy = flatMidY - (float)lpos.Y;
			float dz = flatMidZ - (float)lpos.Z;
			float distSq = (dx * dx) + (dy * dy) + (dz * dz);

			if (distSq < radiusSq)
			{
				hasStaticLight = true;

				float r_base = light->GetRed() * invMul255;
				float g_base = light->GetGreen() * invMul255;
				float b_base = light->GetBlue() * invMul255;

				float dist = sqrtf(distSq);
				float factor = 1.0f - (dist / radius);

				lightmapColor[0] += r_base * factor;
				lightmapColor[1] += g_base * factor;
				lightmapColor[2] += b_base * factor;
			}
		}
		node = node->nextLight;
	}

	// Dynamic hard-clamping to prevent fixed-function color overflows
	for (int c = 0; c < 3; c++)
	{
		if (lightmapColor[c] > 1.0f) lightmapColor[c] = 1.0f;
	}

	return hasStaticLight;
}

//==========================================================================
//
// Check fog in current sector for placing into the proper draw list.
//
//==========================================================================

static bool gl_CheckFog(FColormap *cm, int lightlevel)
{
	bool frontfog;

	PalEntry fogcolor = cm->FadeColor;

	if ((fogcolor.d & 0xffffff) == 0)
	{
		frontfog = false;
	}
	else if (level.outsidefogdensity != 0 && APART(level.info->outsidefog) != 0xff && (fogcolor.d & 0xffffff) == (level.info->outsidefog & 0xffffff))
	{
		frontfog = true;
	}
	else  if (level.fogdensity != 0 || (gl_lightmode & 4) || cm->FogDensity > 0)
	{
		// case 3: level has fog density set
		frontfog = true;
	}
	else
	{
		// case 4: use light level
		frontfog = lightlevel < 248;
	}
	return frontfog;
}

//==========================================================================
//
// Fog management supporting software sector light mode and fake contrast.
// Computes density and uploads parameters straight into the GPU registries.
// Was defined in gl_lightdata.cpp in GZDoom3x, 4x renamed the file as
// hw_lighting.cpp and removed it from there, so let's readapt it here.
//
//==========================================================================
void gl_SetGL1xFog(int lightlevel, int rellight, bool fullbright, const FColormap* cmap, bool isadditive)
{
	PalEntry fogcolor;
	float fogdensity;

	if (r_viewpoint.sector != nullptr && r_viewpoint.sector->Level->flags & LEVEL_HASFADETABLE)
	{
		fogdensity = 70;
		fogcolor = 0x808080;
	}
	else if (cmap != nullptr && !fullbright)
	{
		fogcolor = cmap->FadeColor;
		// Double-cast the CVar object into raw int, then into strict ELightMode enum.
		fogdensity = GetFogDensity(r_viewpoint.sector->Level, static_cast<ELightMode>((int)gl_lightmode), lightlevel, fogcolor, cmap->FogDensity, cmap->BlendFactor);
		fogcolor.a = 0;
	}
	else
	{
		fogcolor = 0;
		fogdensity = 0;
	}

	if (portalState.inskybox) fogdensity += fogdensity / 2;

	// no fog in enhanced vision modes!
	if (fogdensity == 0 || gl_fogmode == 0)
	{
		OpenGLRenderer::gl_RenderState.EnableFog(false);
		OpenGLRenderer::gl_RenderState.SetFog(0, 0.0f);
	}
	else
	{
		int check_lightmode = gl_lightmode;
		if (gl.legacyMode && (check_lightmode == 8 || check_lightmode == 16)) check_lightmode = 3;

		if (cmap && (check_lightmode == 2 || (check_lightmode >= 8 && cmap->BlendFactor > 0)) && fogcolor == 0)
		{
			float light = CalcLightLevel(static_cast<ELightMode>((int)gl_lightmode), lightlevel, rellight, false, cmap->BlendFactor);

			SetShaderLight(OpenGLRenderer::gl_RenderState, r_viewpoint.sector->Level, light, (float)lightlevel);
		}
		else
		{
			OpenGLRenderer::gl_RenderState.SetLightParms(1.f, 0.f);
		}

		// For additive rendering using the regular fog color here would mean applying it twice
		if (isadditive)
		{
			fogcolor = 0;
		}
		OpenGLRenderer::gl_RenderState.EnableFog(true);

		// Push directly into FRenderState cache slot
		OpenGLRenderer::gl_RenderState.SetFog(fogcolor, fogdensity);

		// Korshun: fullbright fog like in software renderer.
		if (gl_lightmode >= 8 && cmap && cmap->BlendFactor == 0 && gl_brightfog && fogdensity != 0 && fogcolor != 0)
		{
			OpenGLRenderer::gl_RenderState.SetSoftLightLevel(255);
		}
	}
}

//==========================================================================
//
//
//
//==========================================================================

bool HWWall::PutWallCompat(HWDrawInfo* di, int passflag)
{
	static int list_indices[2][2] =
	{ { GLLDL_WALLS_PLAIN, GLLDL_WALLS_FOG },{ GLLDL_WALLS_MASKED, GLLDL_WALLS_FOGMASKED } };

	// First baseline context guard shield to protect from empty core engine pointers
	if (di == nullptr || seg == nullptr) return false;
	// Validate texture using GZ4x signature by passing the address of the texture object
	FMaterial* wallMat = FMaterial::ValidateTexture(texture, 0); // 0 for clean default scaleflags
	// Pointer check: If the wall has no valid material, drop it instantly to prevent crashes
	if (wallMat == nullptr) return false;

	// ALLOW M2SNF and M2S to have lights!
	// Replaced legacy mDrawer->FixedColormap with unified 4.14dev FullbrightFlags cacher tracking
	if (di->FullbrightFlags != CM_DEFAULT || !gl_lights || seg->sidedef == nullptr) return false;

	// Block ONLY specific Skyhack if it's really a hurdle
	// Re-mapped from old GLWF_SKYHACK to the unified new v4.14 HWF_SKYHACK bitmask token!
	if ((flags & HWF_SKYHACK) && type == RENDERWALL_M2S) return false;
	// === [Darkcrafter07] - FIXED SECTION: FINISH =====

	// Alternative overbright implementation (slow and too much contrast)
	// Now foggy surfaces with dynlights are renderd almost like regular 
	// modulated light and only then fog is applied on top (unlike doing them simply additive).
	// Don't worry, additive dynlights are still rendered as a separate pass in RenderMultipassStuff,
	// But only if by a mapper or modder request. Yes the previous versions did foggy lights as additives.
	//bool overbright = gl_legacy_dynlight_overbright && lightlevel >= 96;
	//bool foggy = gl_CheckFog(&Colormap, lightlevel) || (level.flags&LEVEL_HASFADETABLE) || overbright;

	// [Darkcrafter07] - Adapted to check masking properties via the 4.14dev FGameTexture methods layout
	bool masked = passflag == 2 && texture->isMasked();

	// Extract the level localized local flags matrix cleanly straight from the wall's frontsector mapping
	bool foggy = gl_CheckFog(&Colormap, lightlevel) || (frontsector->Level->flags & LEVEL_HASFADETABLE);
	//bool foggy = gl_CheckFog(&Colormap, lightlevel) || (level.flags&LEVEL_HASFADETABLE);

	if (gl.gl1path && gl.gl1_v1dot1)
	{
		// Force the engine to route ALL transluscent wall textures straight in GLLDL_WALLS_FOGMASKED
		// This instantly triggers the perfect fixed-function fog-blend state machine,
		// making for the rich color modulation, smooth dynlights, and 100% crystal alpha holes,
		// otherwise the surface is going to be lit additively, which is unwanted if that's a regular dynlight.
		if (masked) foggy = true;
	}

	// Check if this wall has any lights affecting it
	bool hasLights = false;

	// FORCE: If it's a Mid-Texture, we ALWAYS want it in the list for the multipass
	// This bypasses any logic that might skip it due to "no lights in range"
	if (type == RENDERWALL_M2S || type == RENDERWALL_M2SNF)
	{
		hasLights = true;
	}

	// First check if there are any lights at all
	if (!(seg->sidedef->Flags & WALLF_POLYOBJ))
	{
		if (seg->sidedef->lighthead == nullptr)
		{
			// No lights on this wall
			// Only skip lighting passes, not other passes like base or fog
			if (passflag == GLPASS_LIGHTTEX || passflag == GLPASS_LIGHTTEX_ADDITIVE)
				return false;
		}
		else
		{
			// Check if any lights are actually within range
			for (FLightNode* ln = seg->sidedef->lighthead; ln; ln = ln->nextLight)
			{
				FDynamicLight* light = ln->lightsource;

				// Get wall vertices
				// [Darkcrafter07] - Adapted V1/V2 calls to match the 4.14dev sidedef structural pointers
				vertex_t* v1 = seg->sidedef->V1();
				vertex_t* v2 = seg->sidedef->V2();

				// Create a plane from the wall
				// The plane equation is: ax + by + cz + d = 0
				// We need to calculate a, b, c, d from the wall's vertices
				float dx = (float)(v2->fX() - v1->fX());
				float dy = (float)(v2->fY() - v1->fY());

				// Normal is perpendicular to the wall (in 2D, just rotate 90 degrees)
				float nx = -dy; float ny = dx;

				// Normalize the normal vector
				float len = sqrtf(nx * nx + ny * ny);
				if (len > 0) { nx /= len; ny /= len; }

				// Calculate d using one of the vertices
				float d = -(nx * (float)v1->fX() + ny * (float)v1->fY());

				// Now create a plane with these values
				secplane_t wallPlane;
				wallPlane.set(nx, ny, 0, d); // Set z-component to 0 as we're working in 2D

				// Get light position and calculate distance
				// Initialize group to a default value (0 is usually the default portal group)
				int group = seg->frontsector ? seg->frontsector->PortalGroup : 0;
				DVector3 lpos = light->PosRelative(group);

				// Calculate distance from light to wall plane using the correct method
				// PointToDist returns the actual distance value
				float dist = fabsf((float)wallPlane.PointToDist(DVector2(lpos.X, lpos.Y), lpos.Z));
				float radius = light->GetRadius();

				// Check if light is within range
				if (dist <= radius)
				{
					hasLights = true;
					break; // Found at least one light that affects this wall
				}
			}

			// If no lights are in range, skip lighting passes
			if (!hasLights && (passflag == GLPASS_LIGHTTEX || passflag == GLPASS_LIGHTTEX_ADDITIVE))
				return false;
		}
	}
	else if (sub)
	{
		// Similar logic for polyobj walls
		// Diverted to section lighthead matching GZ4x behaviour
		if (sub->section->lighthead == nullptr)
		{
			if (passflag == GLPASS_LIGHTTEX || passflag == GLPASS_LIGHTTEX_ADDITIVE)
				return false;
		}
		else
		{
			for (FLightNode* ln = sub->section->lighthead; ln; ln = ln->nextLight)
			{
				FDynamicLight* light = ln->lightsource;

				// For subsectors, we need to find the closest wall
				// This is more complex, so for now we'll just check if any light is in range
				// Initialize group to a default value (0 is usually the default portal group)
				int group = sub->sector ? sub->sector->PortalGroup : 0;
				DVector3 lpos = light->PosRelative(group);
				float radius = light->GetRadius();

				// Simple check - if light radius overlaps with subsector bounds
				// You might want to make this more precise
				if (fabs(lpos.X - sub->sector->centerspot.X) < radius + 128 &&
					fabs(lpos.Y - sub->sector->centerspot.Y) < radius + 128)
				{
					hasLights = true;
					break;
				}
			}

			if (!hasLights && (passflag == GLPASS_LIGHTTEX || passflag == GLPASS_LIGHTTEX_ADDITIVE))
				return false;
		}
	}

	// Don't call old gl_drawinfo->dldrawlists[list].AddWall(this) here,
	// because the modern engine's PutWall pipeline does that automatically right after validation
	// int list = list_indices[masked][foggy]; 

	return true; // Test passed, wall is fully qualified for multipass rendering!
}

//==========================================================================
//
//
//
//==========================================================================

bool HWFlat::PutFlatCompat(HWDrawInfo* di, int passflag)
{
	//	// Are lights possible? - old faulty block
	//	if (mDrawer->FixedColormap != CM_DEFAULT || !gl_lights || !gltexture || renderstyle != STYLE_Translucent ||
	//	alpha < 1.f - FLT_EPSILON || sector->lighthead == NULL) return false;

	// Are lights possible? - better but slower, fog doesn't disappear when dynlight doesn't touch them though
	// remove "|| sector->lighthead == NULL" from the condition, otherwise distant foggy dynlight surfaces
	// lose fog intensity abruptly.
	// if (mDrawer->FixedColormap != CM_DEFAULT || !gl_lights || !gltexture || renderstyle != STYLE_Translucent ||
	//	alpha < 1.f - FLT_EPSILON || sector->lighthead == NULL) return false;

	// Validate texture using GZ4x signature by passing the address of the texture object
	FMaterial* wallFlat = FMaterial::ValidateTexture(texture, 0); // 0 for clean default scaleflags
	// Pointer check: If the wall has no valid material, drop it instantly to prevent crashes
	if (wallFlat == nullptr) return false;

	// Context shield protects from parsing unbound sector chunks during map loading
	if (di == nullptr || sector == nullptr) return false;

	// That made game slower by 20-30% on regular maps without big radius dynlights, so
	bool skiplightprocessing = section->lighthead == nullptr &&
		(radiusFlats <= 256.0f || g_isGL1xDynlightAcamglowOnFlat);

	if (di->FullbrightFlags != CM_DEFAULT || !gl_lights || renderstyle != STYLE_Translucent ||
		alpha < 1.f - 0.0001f || skiplightprocessing) return false;

	// // static int list_indices =
	// // { { GLLDL_FLATS_PLAIN, GLLDL_FLATS_FOG },{ GLLDL_FLATS_MASKED, GLLDL_FLATS_FOGMASKED } };

	// Alternative overbright implementation (slow and too much contrast)
	// Now foggy surfaces with dynlights are renderd almost like regular 
	// modulated light and only then fog is applied on top (unlike doing them simply additive).
	// Don't worry, additive dynlights are still rendered as a separate pass in RenderMultipassStuff,
	// But only if by a mapper or modder request. Yes the previous versions did foggy lights as additives.
	//bool overbright = gl_legacy_dynlight_overbright && lightlevel >= 96;
	//bool foggy = gl_CheckFog(&Colormap, lightlevel) || (level.flags&LEVEL_HASFADETABLE) || overbright;

	// Check masking flags context directly through the native FGameTexture methods layout
	bool masked = texture->isMasked() && ((renderflags & SSRF_RENDER3DPLANES) || stack);

	// Extract active Level local flags safely from the flat plane's sector reference
	bool foggy = gl_CheckFog(&Colormap, lightlevel) || (sector->Level->flags & LEVEL_HASFADETABLE);
	//bool foggy = gl_CheckFog(&Colormap, lightlevel) || (level.flags&LEVEL_HASFADETABLE) || gl_lights_additive;

	if (gl.gl1path && gl.gl1_v1dot1 && masked)
	{
		// Force the engine to route ALL transluscent 3D-floor textures straight in GLLDL_FLATS_FOGMASKED
		// This instantly triggers the perfect fixed-function fog-blend state machine,
		// making for the rich color modulation, smooth dynlights, and 100% crystal alpha holes,
		// otherwise the surface is going to be lit additively, which is unwanted if that's a regular dynlight.
		foggy = true;
	}

	// Do NOT call old gl_drawinfo->dldrawlists[list].AddFlat(this) here,
	// because the modern engine's PutFlat pipeline handles array insertion automatically right after validation
	// int list = list_indices[masked][foggy];

	return true; // Test passed, flat plane is 100% valid for multi-pass fixed lighting loops!
}


//==========================================================================
//
// Core OpenGL 1.1 hardware fallback rendering pass for fake fog boundary lines.
// Interpolates vertex colors manually across the wall quad to clear depth artifacts.
// Declared in class HWWall (Global Scope).
//
//==========================================================================
void HWWall::RenderFogBoundaryCompat(HWDrawInfo* di)
{
	const float invMul255 = 1.0f / 255.0f;
	const float invMul62500 = 1.0f / 62500.0f;

	// Modern GDZoom v4FColormap field names (colormap with uppercase)
	float fogdensity = GetFogDensity(frontsector->Level, di->lightmode, lightlevel, Colormap.FadeColor, Colormap.FogDensity, Colormap.BlendFactor);

	float dist1 = Dist2(r_viewpoint.Pos.X, r_viewpoint.Pos.Y, glseg.x1, glseg.y1);
	float dist2 = Dist2(r_viewpoint.Pos.X, r_viewpoint.Pos.Y, glseg.x2, glseg.y2);

	// These values were determined by trial and error and are scale dependent!
	float fogd1 = (0.95f - exp(-fogdensity * dist1 * invMul62500)) * 1.05f;
	float fogd2 = (0.95f - exp(-fogdensity * dist2 * invMul62500)) * 1.05f;

	float fc[4] = { Colormap.FadeColor.r * invMul255, Colormap.FadeColor.g * invMul255, Colormap.FadeColor.b * invMul255, fogd2 };

	// Toggle hardware state machine registers natively via the active OpenGLRenderer slots
	OpenGLRenderer::gl_RenderState.EnableTexture(false);
	OpenGLRenderer::gl_RenderState.EnableFog(false);
	OpenGLRenderer::gl_RenderState.AlphaGL1xFunc(GL_GEQUAL, 0.f);

	// Flush parameters baseline safely into driver registers
	OpenGLRenderer::gl_RenderState.ApplyFixedGL1xFunction(nullptr);

	glEnable(GL_POLYGON_OFFSET_FILL);
	glPolygonOffset(-1.0f, -128.0f);
	glDepthFunc(GL_LEQUAL);

	glColor4f(fc[0], fc[1], fc[2], fogd1);
	glBegin(GL_TRIANGLE_FAN);

	glTexCoord2f(tcs[LOLFT].u, tcs[LOLFT].v);
	glVertex3f(glseg.x1, zbottom[0], glseg.y1);

	glTexCoord2f(tcs[UPLFT].u, tcs[UPLFT].v);
	glVertex3f(glseg.x1, ztop[0], glseg.y1);

	glColor4f(fc[0], fc[1], fc[2], fogd2);

	glTexCoord2f(tcs[UPRGT].u, tcs[UPRGT].v);
	glVertex3f(glseg.x2, ztop[1], glseg.y2);

	glTexCoord2f(tcs[LORGT].u, tcs[LORGT].v);
	glVertex3f(glseg.x2, zbottom[1], glseg.y2);
	glEnd();

	// Fixed-pipeline symmetric context reset matrix
	glDepthFunc(GL_LESS);
	glPolygonOffset(0.0f, 0.0f);
	glDisable(GL_POLYGON_OFFSET_FILL);

	OpenGLRenderer::gl_RenderState.EnableFog(true);
	OpenGLRenderer::gl_RenderState.AlphaGL1xFunc(GL_GEQUAL, 0.5f);
	OpenGLRenderer::gl_RenderState.EnableTexture(true);
}

//==========================================================================
//
// Flats 
//
//==========================================================================

// GL1x analog of "SetupLights" from hw_flats.cpp
// Doesn't need to boost dynlight overbright intensity unlike walls
void HWFlat::DrawGL1xSubsectorLights(subsector_t* sub, int pass)
{
	Plane p;
	FVector3 nearPt, up, right, t1;
	DVector3 lightPosAbs;
	float scale, radius = 0.0f;

	FLightNode * node = sub->section->lighthead;
	int group = sub->sector->PortalGroup;
	while (node)
	{
		FDynamicLight * light = node->lightsource;

		// 1. BASELINE HARDWARE ENGINE ACTIVE GATE
		if (light == nullptr || !light->IsActive() || light->target == nullptr)
		{
			node = node->nextLight;
			continue;
		}

		radius = light->GetRadius();
		bool isCamglowDynlightHere = false;
		if (light->IsDynlightCamglow()) isCamglowDynlightHere = true;

		// Turn OFF big dynlights!
		// Do NOT process big dynlights as we're going to bake them
		bool isBigRadiusDynlight = radius > 468.0f;
		if (isBigRadiusDynlight && !OpenGLRenderer::gl_RenderState.g_isCurrentlyDrawingGL1xTransluscentDynlightFlat && gl_legacy_dynlight_baked_huge)
		{
			node = node->nextLight; // this line MUST BE here
			continue;               // this one either way
			if (light->IsActive())
			{
				// MUST deactivate in order to increase performace, no other way around
				light->Deactivate();
			}
		}

		if (!isCamglowDynlightHere && light != nullptr && !isBigRadiusDynlight && r_dynlights_occlude) // works bad with big radius dynlights
		{
			lightPosAbs = light->PosRelative(group);
			float adjustedRadius = radius * 0.85f; // Shrink radius because it culls less agressively

			bool isDynlightVisibleBehind2Sobstr = IsDynlightVisibleBehind2sidedLinedefSectObstrWrapperCached(r_viewpoint.camera, light->target, adjustedRadius, true);
			bool isDynlightObstructedBy3DFloorFlats = IsDynlightBehind3DFloorPlaneCachedWrapper(r_viewpoint.Pos, lightPosAbs, light->Sector, light->target, adjustedRadius, true);
			bool isDynlightVisibleBehind3DFloorSides = IsDynlightVisibleBehind3DFloorSidesCachedWrapper(r_viewpoint.camera, light->target, adjustedRadius, true);

			if (!isDynlightVisibleBehind2Sobstr || isDynlightObstructedBy3DFloorFlats || !isDynlightVisibleBehind3DFloorSides)
			{
				// MUST deactivate in order to increase performace, no other way around
				if (light->IsActive())
				{
					light->Deactivate();
				}
				// Do NOT call CONTINUE AND UNLINK here to preserve node iteration tracks.
			}
			else
			{
				if (!light->IsActive())
				{
					// If the path clears up and the light is sleeping, wake it up
					light->Activate();
				}
			}
		}

		// [Darkcrafter07] - THREE-PASS GEOMETRY FILTER GATES FOR FLATS
		// Each branch of the cascade culls other dynlight types only passes their own kind!
		if (!light->IsActive())
		{
			node = node->nextLight;
			continue;
		}

		if (pass == GLPASS_LIGHTTEX)
		{
			// Pass 1: Regular modulated and subtractive dynlights (non-additive)
			if (light->IsAdditive())
			{
				node = node->nextLight;
				continue; // THAT MEANS: TURN OFF THESE TYPES OF LIGHTS!
			}
		}
		else if (pass == GLPASS_LIGHTTEX_ADDITIVE)
		{
			// Pass 2: Specific additive dynlights (chosen additive in the map editor on purpose)
			if (!light->IsAdditive())
			{
				node = node->nextLight;
				continue;
			}
		}
		else if (pass == GLPASS_TRANSLUCENT_LIGHTTEX)
		{
			// Pass 3: Subtractive dynlights
			if (!light->IsSubtractive())
			{
				node = node->nextLight;
				continue;
			}
		}

		int executionPass = (pass == GLPASS_TRANSLUCENT_LIGHTTEX || pass == GLPASS_LIGHTTEX_ADDITIVE) ? GLPASS_LIGHTTEX : pass;

		float planeh = plane.plane.ZatPoint(light->Pos);
		OpenGLRenderer::gl_RenderState.g_isCurrentlyGL1xDynlightPassActive = true;
		if (gl_lights_checkside && ((planeh < light->Z() && ceiling) || (planeh > light->Z() && !ceiling)))
		{
			node = node->nextLight;
			continue;
		}

		p.Set(plane.plane.Normal(), plane.plane.fD());
		// We need this to find out what's current wall alpha value is in gl_SetupLightFlat
		g_isCurrentlyGL1xDynlightFlatDrawing = this; // Bake current flat address in the stack bridge
		if (!gl_SetupLightFlat(sub->sector->PortalGroup, p, light, nearPt, up, right, scale, false, executionPass != GLPASS_LIGHTTEX))
		{
			g_isCurrentlyGL1xDynlightFlatDrawing = nullptr; // Unlink on skip loop
			node = node->nextLight;
			continue;
		}
		g_isCurrentlyGL1xDynlightFlatDrawing = nullptr; // Clear immediately after evaluation!

		if (pass == GLPASS_LIGHTTEX_OVERBRIGHT1_LEGACY) // DISABLED AND UNUSED BUT CAN SHOW HOW TO DO IT ALTERNATIVELY (in hw_drawinfo.cpp)
		{
			float overbrightFactor = clamp((float)gl_legacy_dynlight_overbright_flats, 0.0f, 0.2f);
			if (overbrightFactor > 1.0f) overbrightFactor = 1.0f;
			if (overbrightFactor < 0.0f) overbrightFactor = 0.0f;

			OpenGLRenderer::gl_RenderState.BlendGL1xEquation(GL_FUNC_ADD);
			OpenGLRenderer::gl_RenderState.SetColor(overbrightFactor, overbrightFactor, overbrightFactor, 1.0f);
			OpenGLRenderer::gl_RenderState.SetObjectColor(0xffffffff);
		}

		OpenGLRenderer::gl_RenderState.ApplyFixedGL1xFunction(nullptr);

		FFlatVertex *ptr = OpenGLRenderer::GLRenderer->mVBO->GetBuffer();
		FFlatVertex *startPtr = ptr;

		for (unsigned int k = 0; k < sub->numlines; k++)
		{
			vertex_t *vt = sub->firstline[k].v1;
			ptr->x = vt->fX();
			ptr->z = plane.plane.ZatPoint(vt);
			ptr->y = vt->fY();
			t1 = { ptr->x, ptr->z, ptr->y };
			FVector3 nearToVert = t1 - nearPt;

			if (pass == GLPASS_LIGHTTEX_OVERBRIGHT1_LEGACY) // DISABLED AND UNUSED BUT CAN SHOW HOW TO DO IT ALTERNATIVELY (in gl_scene.cpp)
			{
				float overbrightFactor = clamp((float)gl_legacy_dynlight_overbright_flats, 0.0f, 0.2f);
				if (overbrightFactor > 1.0f) overbrightFactor = 1.0f;
				if (overbrightFactor < 0.0f) overbrightFactor = 0.0f;

				OpenGLRenderer::gl_RenderState.BlendGL1xEquation(GL_FUNC_ADD);
				OpenGLRenderer::gl_RenderState.SetColor(overbrightFactor, overbrightFactor, overbrightFactor, 1.0f);
				OpenGLRenderer::gl_RenderState.SetObjectColor(0xffffffff);
				OpenGLRenderer::gl_RenderState.ApplyFixedGL1xFunction(nullptr);
			}

			// [Darkcrafter07] - Extract and calculate VBO starting address offsets using 4.14dev flat buffers
			unsigned int startOffset = OpenGLRenderer::GLRenderer->mVBO->mCurIndex;
			FFlatVertex* ptr = OpenGLRenderer::GLRenderer->mVBO->GetBuffer();
			FFlatVertex* startPtr = ptr;

			for (unsigned int k = 0; k < sub->numlines; k++)
			{
				vertex_t* vt = sub->firstline[k].v1;
				ptr->x = (float)vt->fX();
				ptr->z = (float)plane.plane.ZatPoint(vt);
				ptr->y = (float)vt->fY();

				FVector3 t1_loc = { ptr->x, ptr->z, ptr->y };
				FVector3 nearToVert = t1_loc - nearPt;

				if (pass == GLPASS_LIGHTTEX_OVERBRIGHT1_LEGACY) // DISABLED AND UNUSED BUT CAN SHOW HOW TO DO IT ALTERNATIVELY (in gl_scene.cpp)
				{
					float radius_loc = light->GetRadius();
					if (radius_loc <= 0.0f) radius_loc = 1.0f;

					const float staticScaleX = 2.0f;
					const float staticScaleY = 2.0f;
					const float staticOffsetX = 0.0f;
					const float staticOffsetY = 0.0f;

					float customScaleX = 1.0f / (radius_loc * staticScaleX);
					float customScaleY = 1.0f / (radius_loc * staticScaleY);

					ptr->u = ((nearToVert | right) * customScaleX) + 0.5f + staticOffsetX;
					ptr->v = ((nearToVert | up) * customScaleY) + 0.5f + staticOffsetY;
				}
				else
				{
					ptr->u = ((nearToVert | right) * scale) + 0.5f;
					ptr->v = ((nearToVert | up) * scale) + 0.5f;
				}
				ptr++;
			}

			unsigned int vertexCount = sub->numlines;
			// Advance the cyclic index register to lock current frames vertex allocations
			OpenGLRenderer::GLRenderer->mVBO->mCurIndex += vertexCount;

			// FIRST PASS: Multiply existing floor texture by the projected light mask shape
			// High-performance direct hardware draw call bypasses deleted RenderCurrent pipeline completely
			glDrawArrays(GL_TRIANGLE_FAN, startOffset, vertexCount);

			if (pass == GLPASS_LIGHTTEX_OVERBRIGHT1_LEGACY) // DISABLED AND UNUSED BUT CAN SHOW HOW TO DO IT ALTERNATIVELY (in hw_drawinfo.cpp)
			{
				glDrawArrays(GL_TRIANGLE_FAN, startOffset, vertexCount);
			}

			node = node->nextLight;
		}

		// --- BLEND EQUATION RECOVERY - FIX PITCH-BLACK SURFACES UNDER SOME ANGLES ---
		// If a subtractive node was processed during the loop,
		// it leaves the GPU register stuck in GL_FUNC_REVERSE_SUBTRACT mode.
		// We forcefully restore GL_FUNC_ADD right here before the memory context 
		// leaves this floor sector chunk, permanently curing the angle pitch-black glitch.
		OpenGLRenderer::gl_RenderState.g_isCurrentlyGL1xDynlightPassActive = false;
		OpenGLRenderer::gl_RenderState.BlendGL1xEquation(GL_FUNC_ADD);
		OpenGLRenderer::gl_RenderState.ApplyFixedGL1xFunction(nullptr);
	}
}

//==========================================================================
//
//
//
//==========================================================================

void HWFlat::DrawLightsCompat(HWDrawInfo* di, int pass)
{
	// Set fog and global coloring for this pass
	// pass GLPASS_LIGHTTEX_OVERBRIGHT1_LEGACY IS DISABLED AND UNUSED BUT CAN SHOW HOW TO DO IT ALTERNATIVELY (in hw_drawinfo.cpp)
	if (pass == GLPASS_LIGHTTEX_OVERBRIGHT1_LEGACY || pass == GLPASS_LIGHTTEX_ADDITIVE)
	{
		// Force-disable fixed hardware fog to fully eliminate the lipstick effect
		OpenGLRenderer::gl_RenderState.EnableFog(false);

		// Force vertices to pure white to avoid colored grease lines on floors/ceilings
		OpenGLRenderer::gl_RenderState.SetColor(1.0f, 1.0f, 1.0f, 1.0f);
		OpenGLRenderer::gl_RenderState.SetObjectColor(0xffffffff);
	}
	else
	{
		gl_SetGL1xFog(lightlevel, getExtraLight(), (di->FullbrightFlags != CM_DEFAULT), &Colormap, true);
	}

	// Draw the subsectors belonging to this sector
	if (sector != nullptr)
	{
		for (int i = 0; i < sector->subsectorcount; i++)
		{
			subsector_t* sub = sector->subsectors[i];
			if (sub != nullptr)
			{
				// Bypass ss_renderflags filter completely for our overbright pass!
				if (pass == GLPASS_LIGHTTEX_OVERBRIGHT1_LEGACY || (di->ss_renderflags[sub->Index()] & renderflags))
				{
					DrawGL1xSubsectorLights(sub, pass);
				}
			}
		}
	}

	// Draw the subsectors assigned to it due to missing textures / render hacks
	if (!(renderflags & SSRF_RENDER3DPLANES))
	{
		gl_subsectorrendernode** nodePtr = (renderflags & SSRF_RENDERFLOOR) ?
			di->otherFloorPlanes.CheckKey(sector->sectornum) :
			di->otherCeilingPlanes.CheckKey(sector->sectornum);

		if (nodePtr != nullptr && *nodePtr != nullptr)
		{
			gl_subsectorrendernode* node = *nodePtr;
			while (node)
			{
				if (node->sub != nullptr)
				{
					DrawGL1xSubsectorLights(node->sub, pass);
				}
				node = node->next;
			}
		}
	}
}

//==========================================================================
//
// Sets up the texture coordinates for one light to be rendered
//
//==========================================================================
bool HWWall::PrepareGL1xLightOnWall(FDynamicLight* light, int pass)
{
	float vtx[] = { glseg.x1,zbottom[0],glseg.y1, glseg.x1,ztop[0],glseg.y1, glseg.x2,ztop[1],glseg.y2, glseg.x2,zbottom[1],glseg.y2 };
	Plane p;
	FVector3 nearPt, up, right;
	float scale;

	FVector3 normal = glseg.Normal();
	float d = normal | FVector3(-glseg.x1, 0, -glseg.y1);
	p.Set(normal, d);

	// TO DO: this causes some walls to not receieve dynlights at all, try to fix gl_geometric.h and gl_wall.h
	//if (!p.ValidNormal())
	//{
	//	return false;
	//}

	//	// No reason to draw it like usual if we bake huge radius lights
	//if (gl_legacy_dynlight_baked_huge && light->GetRadius() >= 468.0f)
	//{
	//	return false;
	//}

	if (!gl_SetupLightWall(seg->frontsector->PortalGroup, p, light, nearPt, up, right, scale, true, pass != GLPASS_LIGHTTEX))
	{
		return false;
	}

	int outcnt[4] = { 0,0,0,0 };

	// This sets the coordinates to project a dynlight texture onto a polygon
	for (int i = 0; i < 4; i++)
	{
		FVector3 t1 = { vtx[i * 3 + 0], vtx[i * 3 + 1], vtx[i * 3 + 2] };
		FVector3 nearToVert = t1 - nearPt;

		tcs[i].u = ((nearToVert | right) * scale) + 0.5f;
		tcs[i].v = ((nearToVert | up) * scale) + 0.5f;

		// quick check whether the light touches this polygon
		if (tcs[i].u < 0) outcnt[0]++;
		if (tcs[i].u > 1) outcnt[1]++;
		if (tcs[i].v < 0) outcnt[2]++;
		if (tcs[i].v > 1) outcnt[3]++;
	}

	// The light doesn't touch this polygon
	if (outcnt[0] == 4 || outcnt[1] == 4 || outcnt[2] == 4 || outcnt[3] == 4) return false;

	// Increments global static clock tracker smoothly
	draw_dlight++;
	return true;
}

// GL1x analog of "SetupLights" from gl_walls_draw.cpp
void HWWall::RenderLightsCompat(HWDrawInfo* di, FRenderState& state, int pass)
{
	float radius = 0.0f;
	FLightNode* node;

	if (pass == GLPASS_LIGHTTEX)
	{
		gl_SetGL1xFog((255 + lightlevel) >> 1, rellight, false, &Colormap, false);
	}
	else
	{
		gl_SetGL1xFog(lightlevel, rellight, false, &Colormap, true);
	}

	if (seg == nullptr || seg->sidedef == nullptr)
	{
		return;
	}
	else if (!(seg->sidedef->Flags & WALLF_POLYOBJ))
	{
		node = seg->sidedef->lighthead;
	}
	else if (sub)
	{
		// GZ4x polyobjects light nodes are processed strictly via subsector section headers
		node = sub->section->lighthead;
	}
	else
	{
		return;
	}

	texcoord save[4];
	memcpy(save, tcs, sizeof(tcs));

	// Fast path portal group index extraction for absolute positioning
	//int groupAbs = sub ? sub->sector->PortalGroup : (seg->frontsector ? seg->frontsector->PortalGroup : 0);

	while (node)
	{
		FDynamicLight* light = node->lightsource;

		// 1. BASELINE HARDWARE ENGINE ACTIVE GATE
		if (light == nullptr || !light->IsActive() || light->target == nullptr)
		{
			node = node->nextLight;
			continue;
		}

		radius = light->GetRadius();
		bool isCamglowDynlightHere = false;
		if (light->IsDynlightCamglow()) isCamglowDynlightHere = true;

		// Turn OFF big dynlights!
		bool isBigRadiusDynlight = radius > 468.0f;
		if (isBigRadiusDynlight && OpenGLRenderer::gl_RenderState.g_isCurrentlyDrawingGL1xTransluscentDynlightWall && gl_legacy_dynlight_baked_huge)
		{
			node = node->nextLight;
			continue;
			if (light->IsActive())
			{
				light->Deactivate();
			}
		}

		if (isCamglowDynlightHere && light != nullptr && !isBigRadiusDynlight && r_dynlights_occlude)
		{
			//DVector3 LightPosAbs = light->PosAbsolute(groupAbs); // PosAbsolute resides in gl_spritelight.cpp but I haven't ported it here yet and are abs coords needed here?
			DVector3 LightPosRel = light->PosRelative(seg->frontsector->PortalGroup);
			float adjustedRadius = radius * 0.85f;

			bool isDynlightVisibleBehind2Sobstr = IsDynlightVisibleBehind2sidedLinedefSectObstrWrapperCached(r_viewpoint.camera, light->target, adjustedRadius, true);
			bool isDynlightObstructedBy3DFloorFlats = IsDynlightBehind3DFloorPlaneCachedWrapper(r_viewpoint.Pos, LightPosRel, light->Sector, light->target, adjustedRadius, true);
			bool isDynlightVisibleBehind3DFloorSides = IsDynlightVisibleBehind3DFloorSidesCachedWrapper(r_viewpoint.camera, light->target, adjustedRadius, true);

			if (!isDynlightVisibleBehind2Sobstr || isDynlightObstructedBy3DFloorFlats || !isDynlightVisibleBehind3DFloorSides)
			{
				if (light->IsActive())
				{
					light->Deactivate();
				}
			}
			else
			{
				if (!light->IsActive())
				{
					light->Activate();
				}
			}
		}

		// THREE-PASS GEOMETRY FILTER GATES FOR WALLS
		if (!light->IsActive())
		{
			node = node->nextLight;
			continue;
		}

		if (pass == GLPASS_LIGHTTEX)
		{
			if (light->IsAdditive())
			{
				node = node->nextLight;
				continue;
			}
		}
		else if (pass == GLPASS_LIGHTTEX_ADDITIVE)
		{
			if (!light->IsAdditive())
			{
				node = node->nextLight;
				continue;
			}
		}
		else if (pass == GLPASS_TRANSLUCENT_LIGHTTEX)
		{
			if (!light->IsSubtractive())
			{
				node = node->nextLight;
				continue;
			}
		}

		int executionPass = (pass == GLPASS_TRANSLUCENT_LIGHTTEX || pass == GLPASS_LIGHTTEX_ADDITIVE) ? GLPASS_LIGHTTEX : pass;

		OpenGLRenderer::gl_RenderState.g_isCurrentlyGL1xDynlightPassActive = true;
		g_isCurrentlyGL1xDynlightWallDrawing = this;

		if (PrepareGL1xLightOnWall(light, executionPass))
		{
			vertcount = 0;

			// Use the universal implementation dispatcher.
			// Pass the contextual executionPass (like GLPASS_LIGHTTEX) to drive raw rendering loops.
			DrawGL1xWallImplementation(nullptr, di, state, executionPass);
		}

		g_isCurrentlyGL1xDynlightWallDrawing = nullptr;
		node = node->nextLight;
	}

	// --- BLEND EQUATION RECOVERY - FIX PITCH-BLACK SURFACES UNDER SOME ANGLES ---
	// If a subtractive node was processed during the loop,
	// it leaves the GPU register stuck in GL_FUNC_REVERSE_SUBTRACT mode.
	// We forcefully restore GL_FUNC_ADD right here before the memory context 
	// leaves this floor sector chunk, permanently curing the angle pitch-black glitch.
	OpenGLRenderer::gl_RenderState.g_isCurrentlyGL1xDynlightPassActive = false;
	OpenGLRenderer::gl_RenderState.BlendGL1xEquation(GL_FUNC_ADD);
	OpenGLRenderer::gl_RenderState.ApplyFixedGL1xFunction(nullptr);

	memcpy(tcs, save, sizeof(tcs));
	vertcount = 0;
}

//==========================================================================
//
//
//
//==========================================================================

void HWDrawInfo::RenderMultipassStuff()
{
	// //******* FIRST PASS: EMPTY BACKGROUND WITH SECTOR LIGHT ONLY *******
	// Part 1: solid geometry. This is set up so that there are no transparent parts
	// remove any remaining texture bindings and shaders whick may get in the way.
	OpenGLRenderer::gl_RenderState.EnableTexture(false);
	OpenGLRenderer::gl_RenderState.EnableBrightmap(false);
	OpenGLRenderer::gl_RenderState.ApplyGL1x(nullptr);

	HWWallDispatcher legacyDispatcher(this);

	drawlists[GLDL_PLAINWALLS].DrawGL1xWalls(&legacyDispatcher, this, OpenGLRenderer::gl_RenderState, GLPASS_PLAIN);
	drawlists[GLDL_PLAINFLATS].DrawGL1xFlats(this, OpenGLRenderer::gl_RenderState, GLPASS_PLAIN); // здесь

	// Part 2: masked geometry. This is set up so that only pixels with alpha>0.5 will show
	// This creates a blank surface that only fills the nontransparent parts of the texture
	if (gl.gl1path && gl.gl1_v1dot1)
	{
		// // GL1.1: sprite-alike base pass
		OpenGLRenderer::gl_RenderState.EnableTexture(true);
		OpenGLRenderer::gl_RenderState.EnableBrightmap(true);

		// Force grates to render using the exact same smooth logic as sprites
		OpenGLRenderer::gl_RenderState.SetTextureMode(TM_MODULATE);
		glEnable(GL_BLEND);
		OpenGLRenderer::gl_RenderState.BlendGL1xFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

		// Explicit alpha testing gate to culling transparent pixels early
		glEnable(GL_ALPHA_TEST);
		OpenGLRenderer::gl_RenderState.AlphaGL1xFunc(GL_GREATER, gl_mask_threshold);

		// Render the grates fully textured and illuminated right here in Part 2!
		drawlists[GLDL_MASKEDWALLS].DrawGL1xWalls(&legacyDispatcher, this, OpenGLRenderer::gl_RenderState, GLPASS_PLAIN);
		drawlists[GLDL_MASKEDFLATS].DrawGL1xFlats(this, OpenGLRenderer::gl_RenderState, GLPASS_PLAIN);
	}
	else
	{
		// // GL1.3: original base pass
		OpenGLRenderer::gl_RenderState.EnableTexture(true);
		OpenGLRenderer::gl_RenderState.SetTextureMode(TM_MASK);
		OpenGLRenderer::gl_RenderState.EnableBrightmap(true);
		OpenGLRenderer::gl_RenderState.AlphaGL1xFunc(GL_GEQUAL, gl_mask_threshold);

		drawlists[GLDL_MASKEDWALLS].DrawGL1xWalls(&legacyDispatcher, this, OpenGLRenderer::gl_RenderState, GLPASS_PLAIN);
		drawlists[GLDL_MASKEDFLATS].DrawGL1xFlats(this, OpenGLRenderer::gl_RenderState, GLPASS_PLAIN);
	}

	// // Part 3: The base of fogged surfaces (without fixed fog)
	OpenGLRenderer::gl_RenderState.EnableFog(false); // Disable fixed fog for foggy surfaces
	OpenGLRenderer::gl_RenderState.EnableBrightmap(false);
	OpenGLRenderer::gl_RenderState.SetTextureMode(TM_MODULATE);
	OpenGLRenderer::gl_RenderState.AlphaGL1xFunc(GL_GEQUAL, 0.f);

	drawlists[GLDL_FOGWALLS].DrawGL1xWalls(&legacyDispatcher, this, OpenGLRenderer::gl_RenderState, GLPASS_PLAIN);
	drawlists[GLDL_FOGFLATS].DrawGL1xFlats(this, OpenGLRenderer::gl_RenderState, GLPASS_PLAIN);

	OpenGLRenderer::gl_RenderState.AlphaGL1xFunc(GL_GEQUAL, gl_mask_threshold);

	drawlists[GLDL_FOGMASKEDWALLS].DrawGL1xWalls(&legacyDispatcher, this, OpenGLRenderer::gl_RenderState, GLPASS_PLAIN);
	drawlists[GLDL_FOGMASKEDFLATS].DrawGL1xFlats(this, OpenGLRenderer::gl_RenderState, GLPASS_PLAIN);

	// //******* SECOND PASS: DRAW LIGHTS (INCLUDING FOGGY SURFACES) *******
	glDepthMask(false);

	// Preserved features routing, replaced old FixedColormap with the new internal FullbrightFlags cache
	if (gl_lights && (FullbrightFlags == CM_DEFAULT))
	{
		if (gl_SetupLightTexture())
		{
			if (gl.gl1path && gl.gl1_v1dot1)
			{
				// // GL1.1 lightpass
				OpenGLRenderer::gl_RenderState.BlendGL1xFunc(GL_ONE, GL_ONE);
				glDepthFunc(GL_EQUAL);
				if (static_cast<int>(lightmode) >= 8) OpenGLRenderer::gl_RenderState.ApplyFixedGL1xFunction(nullptr);

				// // Apply additive lights to standard surfaces first
				drawlists[GLDL_PLAINWALLS].DrawGL1xWalls(&legacyDispatcher, this, OpenGLRenderer::gl_RenderState, GLPASS_LIGHTTEX);
				drawlists[GLDL_FOGWALLS].DrawGL1xWalls(&legacyDispatcher, this, OpenGLRenderer::gl_RenderState, GLPASS_LIGHTTEX);
				drawlists[GLDL_FOGMASKEDWALLS].DrawGL1xWalls(&legacyDispatcher, this, OpenGLRenderer::gl_RenderState, GLPASS_LIGHTTEX);
				drawlists[GLDL_PLAINFLATS].DrawGL1xFlats(this, OpenGLRenderer::gl_RenderState, GLPASS_LIGHTTEX);
				drawlists[GLDL_FOGFLATS].DrawGL1xFlats(this, OpenGLRenderer::gl_RenderState, GLPASS_LIGHTTEX);
				drawlists[GLDL_FOGMASKEDFLATS].DrawGL1xFlats(this, OpenGLRenderer::gl_RenderState, GLPASS_LIGHTTEX);

				// // Re-route grates to sprite-aligned lighting pass (GL_DST_COLOR, GL_ONE)
				OpenGLRenderer::gl_RenderState.BlendGL1xFunc(GL_DST_COLOR, GL_ONE);
				glEnable(GL_ALPHA_TEST);
				OpenGLRenderer::gl_RenderState.AlphaGL1xFunc(GL_GREATER, gl_mask_threshold);

				// ATTENTION! DO NOT USE REGULAR "_MASKED" LISTS.
				// Because even that way these are going to look "fine" in well lit areas
				// but in low lit conditions, these are going to receive LOW amounts of dynlights.
				// These are here for the historic record. The only way to improve the situation
				// was to modify PutWallCompat function to check the lightlevel of a sector the
				// wall was situated in and if it was "lit well enough" - clamp wall light to 128
				//	//if (type == RENDERWALL_M2S || type == RENDERWALL_M2SNF)
				//	//{
				//	//	hasLights = true;
				//	//	// We shouldn't brighten masked mid transluscent textures if they're in a dark sector
				//	//	bool isSectorPitchBlack = (seg && seg->frontsector && seg->frontsector->lightlevel < 96);
				//	//
				//	//	if (!isSectorPitchBlack)
				//	//	{
				//	//		// Dark translucent midtextures receive less dynlights
				//	//		// So everything darker gets clipped at lightlevel 128
				//	//		if (lightlevel < 128) lightlevel = 128;
				//	//	}
				//	//}
				// INSTEAD PUT THEM ALL TO _FOGMASKED lists,
				// That way, they're going to look just as good as GL1.3 ones even if there's NO fog!
				// Like it's done in PutWallCompat or PutFlatCompat (100% identical blocks):
				//	//if (gl.gl1path && gl.gl1_v1dot1)
				//	//{
				//	//	if (masked) foggy = true;
				//	//}
				drawlists[GLDL_MASKEDWALLS].DrawGL1xWalls(&legacyDispatcher, this, OpenGLRenderer::gl_RenderState, GLPASS_LIGHTTEX);
				drawlists[GLDL_MASKEDFLATS].DrawGL1xFlats(this, OpenGLRenderer::gl_RenderState, GLPASS_LIGHTTEX);

				// //Quick recovery cleanup back to standard pass flags
				glDisable(GL_ALPHA_TEST);
				OpenGLRenderer::gl_RenderState.BlendGL1xFunc(GL_ONE, GL_ONE);
			}
			else
			{
				// //GL1.3 original lightpass
				OpenGLRenderer::gl_RenderState.BlendGL1xFunc(GL_ONE, GL_ONE);
				glDepthFunc(GL_EQUAL);
				if (static_cast<int>(lightmode) >= 8) OpenGLRenderer::gl_RenderState.ApplyFixedGL1xFunction(nullptr);

				// // Apply regular lights to ALL surfaces
				drawlists[GLDL_PLAINWALLS].DrawGL1xWalls(&legacyDispatcher, this, OpenGLRenderer::gl_RenderState, GLPASS_LIGHTTEX);
				drawlists[GLDL_MASKEDWALLS].DrawGL1xWalls(&legacyDispatcher, this, OpenGLRenderer::gl_RenderState, GLPASS_LIGHTTEX);
				drawlists[GLDL_FOGWALLS].DrawGL1xWalls(&legacyDispatcher, this, OpenGLRenderer::gl_RenderState, GLPASS_LIGHTTEX);
				drawlists[GLDL_FOGMASKEDWALLS].DrawGL1xWalls(&legacyDispatcher, this, OpenGLRenderer::gl_RenderState, GLPASS_LIGHTTEX);
				drawlists[GLDL_PLAINFLATS].DrawGL1xFlats(this, OpenGLRenderer::gl_RenderState, GLPASS_LIGHTTEX);
				drawlists[GLDL_MASKEDFLATS].DrawGL1xFlats(this, OpenGLRenderer::gl_RenderState, GLPASS_LIGHTTEX);
				drawlists[GLDL_FOGFLATS].DrawGL1xFlats(this, OpenGLRenderer::gl_RenderState, GLPASS_LIGHTTEX);
				drawlists[GLDL_FOGMASKEDFLATS].DrawGL1xFlats(this, OpenGLRenderer::gl_RenderState, GLPASS_LIGHTTEX);
			}

			OpenGLRenderer::gl_RenderState.BlendGL1xEquation(GL_FUNC_ADD);
		}
	}

	// // ******* THIRD PASS: MODULATED TEXTURE (ALL SURFACES) *******
	if (gl.gl1path && gl.gl1_v1dot1)
	{
		// // GL1.1 multiplication loop
		OpenGLRenderer::gl_RenderState.SetColor(1.0f, 1.0f, 1.0f, 1.0f);
		OpenGLRenderer::gl_RenderState.BlendGL1xFunc(GL_DST_COLOR, GL_ZERO);
		OpenGLRenderer::gl_RenderState.EnableFog(false); // Keep fog disabled
		OpenGLRenderer::gl_RenderState.AlphaGL1xFunc(GL_GEQUAL, 0.f);

		// // Dynlights in front of a transparent txt make all black behind it
		glDepthFunc(GL_EQUAL); // CRITICAL: Use GL_EQUAL instead of GL_LEQUAL
		drawlists[GLDL_PLAINWALLS].DrawGL1xWalls(&legacyDispatcher, this, OpenGLRenderer::gl_RenderState, GLPASS_TEXONLY);
		drawlists[GLDL_PLAINFLATS].DrawGL1xFlats(this, OpenGLRenderer::gl_RenderState, GLPASS_TEXONLY);
		drawlists[GLDL_FOGWALLS].DrawGL1xWalls(&legacyDispatcher, this, OpenGLRenderer::gl_RenderState, GLPASS_TEXONLY);
		drawlists[GLDL_FOGFLATS].DrawGL1xFlats(this, OpenGLRenderer::gl_RenderState, GLPASS_TEXONLY);

		OpenGLRenderer::gl_RenderState.AlphaGL1xFunc(GL_GREATER, gl_mask_threshold);
		drawlists[GLDL_FOGMASKEDWALLS].DrawGL1xWalls(&legacyDispatcher, this, OpenGLRenderer::gl_RenderState, GLPASS_TEXONLY);
		drawlists[GLDL_FOGMASKEDFLATS].DrawGL1xFlats(this, OpenGLRenderer::gl_RenderState, GLPASS_TEXONLY);

		// NOTE: GLLDL_WALLS_MASKED and GLLDL_FLATS_MASKED are completely
		// skipped here because they were fully drawn with transparency in Part 2!
	}
	else
	{
		// //GL1.3 original multiplication loop
		OpenGLRenderer::gl_RenderState.SetColor(1.0f, 1.0f, 1.0f, 1.0f);
		OpenGLRenderer::gl_RenderState.BlendGL1xFunc(GL_DST_COLOR, GL_ZERO);
		OpenGLRenderer::gl_RenderState.EnableFog(false); // keep fog disabled
		OpenGLRenderer::gl_RenderState.AlphaGL1xFunc(GL_GEQUAL, 0.f);
		// //Dynlights in front of a transparent txt make all black behind it
		glDepthFunc(GL_EQUAL); // CRITICAL: Use GL_EQUAL instead of GL_LEQUAL
		drawlists[GLDL_PLAINWALLS].DrawGL1xWalls(&legacyDispatcher, this, OpenGLRenderer::gl_RenderState, GLPASS_TEXONLY);
		drawlists[GLDL_PLAINFLATS].DrawGL1xFlats(this, OpenGLRenderer::gl_RenderState, GLPASS_TEXONLY);
		OpenGLRenderer::gl_RenderState.AlphaGL1xFunc(GL_GREATER, gl_mask_threshold);
		drawlists[GLDL_MASKEDWALLS].DrawGL1xWalls(&legacyDispatcher, this, OpenGLRenderer::gl_RenderState, GLPASS_TEXONLY);
		drawlists[GLDL_MASKEDFLATS].DrawGL1xFlats(this, OpenGLRenderer::gl_RenderState, GLPASS_TEXONLY);
		OpenGLRenderer::gl_RenderState.AlphaGL1xFunc(GL_GEQUAL, 0.f);
		drawlists[GLDL_FOGWALLS].DrawGL1xWalls(&legacyDispatcher, this, OpenGLRenderer::gl_RenderState, GLPASS_TEXONLY);
		drawlists[GLDL_FOGFLATS].DrawGL1xFlats(this, OpenGLRenderer::gl_RenderState, GLPASS_TEXONLY);
		OpenGLRenderer::gl_RenderState.AlphaGL1xFunc(GL_GREATER, gl_mask_threshold);
		drawlists[GLDL_FOGWALLS].DrawGL1xWalls(&legacyDispatcher, this, OpenGLRenderer::gl_RenderState, GLPASS_TEXONLY);
		drawlists[GLDL_FOGMASKEDFLATS].DrawGL1xFlats(this, OpenGLRenderer::gl_RenderState, GLPASS_TEXONLY);
	}
	// ******* POST-TEXTURE PASSES: FOG OVERLAYS AND ADDITIVE PASSES *******
	// Fourth pass: apply fog as translucent overlay for foggy surfaces
	OpenGLRenderer::gl_RenderState.EnableFog(false); // Ensure fog is disabled
	OpenGLRenderer::gl_RenderState.BlendGL1xFunc(GL_ONE, GL_ONE); // must be a faster way to do it
	//OpenGLRenderer::gl_RenderState.BlendFunc(GL_SRC_ALPHA, 1); // another way
	glDepthMask(false);
	// Fifth pass: apply fog overlay to foggy surfaces
	drawlists[GLDL_FOGWALLS].DrawGL1xWalls(&legacyDispatcher, this, OpenGLRenderer::gl_RenderState, GLPASS_PLAIN);
	drawlists[GLDL_FOGFLATS].DrawGL1xFlats(this, OpenGLRenderer::gl_RenderState, GLPASS_PLAIN);
	drawlists[GLDL_FOGMASKEDWALLS].DrawGL1xWalls(&legacyDispatcher, this, OpenGLRenderer::gl_RenderState, GLPASS_PLAIN);
	drawlists[GLDL_FOGMASKEDFLATS].DrawGL1xFlats(this, OpenGLRenderer::gl_RenderState, GLPASS_PLAIN);
	// Sixth pass: there are still special dynamic lights that are SET to be ADDITIVE
	// These lights must be rendered AFTER fog to remain bright and visible
	if (gl_lights && (FullbrightFlags == CM_DEFAULT))
	{
		if (gl_SetupLightTexture())
		{
			OpenGLRenderer::gl_RenderState.EnableFog(true); // Let additive lights blend with fog color if needed
			OpenGLRenderer::gl_RenderState.BlendGL1xFunc(GL_ONE, GL_ONE);
			glDepthFunc(GL_EQUAL);
			glDepthMask(false);
			drawlists[GLDL_PLAINWALLS].DrawGL1xWalls(&legacyDispatcher, this, OpenGLRenderer::gl_RenderState, GLPASS_LIGHTTEX_ADDITIVE);
			drawlists[GLDL_MASKEDWALLS].DrawGL1xWalls(&legacyDispatcher, this, OpenGLRenderer::gl_RenderState, GLPASS_LIGHTTEX_ADDITIVE);
			drawlists[GLDL_FOGWALLS].DrawGL1xWalls(&legacyDispatcher, this, OpenGLRenderer::gl_RenderState, GLPASS_LIGHTTEX_ADDITIVE);
			drawlists[GLDL_FOGMASKEDWALLS].DrawGL1xWalls(&legacyDispatcher, this, OpenGLRenderer::gl_RenderState, GLPASS_LIGHTTEX_ADDITIVE);
			drawlists[GLDL_PLAINFLATS].DrawGL1xFlats(this, OpenGLRenderer::gl_RenderState, GLPASS_LIGHTTEX_ADDITIVE);
			drawlists[GLDL_MASKEDFLATS].DrawGL1xFlats(this, OpenGLRenderer::gl_RenderState, GLPASS_LIGHTTEX_ADDITIVE);
			drawlists[GLDL_FOGFLATS].DrawGL1xFlats(this, OpenGLRenderer::gl_RenderState, GLPASS_LIGHTTEX_ADDITIVE);
			drawlists[GLDL_FOGMASKEDFLATS].DrawGL1xFlats(this, OpenGLRenderer::gl_RenderState, GLPASS_LIGHTTEX_ADDITIVE);
		}
	}
	// Seventh pass: overbright
	if (gl_lights && gl_legacy_dynlight_overbright && (FullbrightFlags == CM_DEFAULT))
	{
		if (gl_SetupLightTexture())
		{
			OpenGLRenderer::gl_RenderState.EnableTexture(true);
			OpenGLRenderer::gl_RenderState.EnableBrightmap(false);
			OpenGLRenderer::gl_RenderState.EnableFog(false);
			glDepthFunc(GL_EQUAL);
			glDepthMask(false);
			OpenGLRenderer::gl_RenderState.BlendGL1xEquation(GL_FUNC_ADD);
			OpenGLRenderer::gl_RenderState.BlendGL1xFunc(GL_DST_COLOR, GL_ONE);
			// // The state link: tag the active pipeline context as GLPASS_LIGHTTEX_OVERBRIGHT2_LEGACY
			//OpenGLRenderer::gl_RenderState.mPassType = (EPassType)GLPASS_LIGHTTEX_OVERBRIGHT2_LEGACY;
			//g_isCurrentlyGL1xWallsDynlightOverbrightPass = true;
			drawlists[GLDL_PLAINWALLS].DrawGL1xWalls(&legacyDispatcher, this, OpenGLRenderer::gl_RenderState, GLPASS_LIGHTTEX);
			drawlists[GLDL_MASKEDWALLS].DrawGL1xWalls(&legacyDispatcher, this, OpenGLRenderer::gl_RenderState, GLPASS_LIGHTTEX);
			drawlists[GLDL_FOGWALLS].DrawGL1xWalls(&legacyDispatcher, this, OpenGLRenderer::gl_RenderState, GLPASS_LIGHTTEX);
			//g_isCurrentlyGL1xWallsDynlightOverbrightPass = false;
			//g_isCurrentlyGL1xFlatsDynlightOverbrightPass = true;
			drawlists[GLDL_PLAINFLATS].DrawGL1xFlats(this, OpenGLRenderer::gl_RenderState, GLPASS_LIGHTTEX);
			drawlists[GLDL_MASKEDFLATS].DrawGL1xFlats(this, OpenGLRenderer::gl_RenderState, GLPASS_LIGHTTEX);
			drawlists[GLDL_FOGFLATS].DrawGL1xFlats(this, OpenGLRenderer::gl_RenderState, GLPASS_LIGHTTEX);
			//g_isCurrentlyGL1xFlatsDynlightOverbrightPass = false;
			// Factory state recovery reset
			//OpenGLRenderer::gl_RenderState.mPassType = NORMAL_PASS;
		}
	}
	// Cleanup
	glDepthFunc(GL_LESS);
	OpenGLRenderer::gl_RenderState.AlphaGL1xFunc(GL_GEQUAL, 0.f);
	OpenGLRenderer::gl_RenderState.EnableFog(true);
	OpenGLRenderer::gl_RenderState.BlendGL1xFunc(GL_ONE, GL_ZERO);
	glDepthMask(true);
}
