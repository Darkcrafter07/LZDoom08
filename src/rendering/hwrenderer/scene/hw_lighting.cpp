// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2002-2018 Christoph Oelckers
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
** hw_lighting.cpp, former gl_lightdata.cpp
** Light level / fog management / dynamic lights
**
**/

#include "c_cvars.h"
#include "r_sky.h"
#include "g_levellocals.h"
#include "hw_lighting.h"
#include "hwrenderer/scene/hw_drawinfo.h"

// externally settable lighting properties
static float distfogtable[2][256];	// light to fog conversion table for black fog
static float distgl1xsoftwarefogtable[2][256]; // light to fog conversion table for black fog for GL1x/GL2 software sector light modes

CVAR(Int, gl_weaponlight, 8, CVAR_ARCHIVE);
CVAR(Bool, gl_enhanced_nightvision, false, CVAR_ARCHIVE|CVAR_NOINITCALL)

//==========================================================================
//
// Sets up the fog tables
//
//==========================================================================

CUSTOM_CVAR(Int, gl_distfog, 70, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	for (int i = 0; i < 256; i++)
	{

		if (i < 164)
		{
			distfogtable[0][i] = (float)((gl_distfog >> 1) + (gl_distfog)*(164 - i) / 164);
		}
		else if (i < 230)
		{
			distfogtable[0][i] = (float)((gl_distfog >> 1) - (gl_distfog >> 1)*(i - 164) / (230 - 164));
		}
		else distfogtable[0][i] = 0;

		if (i < 128)
		{
			distfogtable[1][i] = 6.f + (float)((gl_distfog >> 1) + (gl_distfog)*(128 - i) / 48);
		}
		else if (i < 216)
		{
			distfogtable[1][i] = (216.f - i) / ((216.f - 128.f)) * gl_distfog / 10;
		}
		else distfogtable[1][i] = 0;
	}
}

// There's a need to have a separate fogtable for software/vanilla modes
CUSTOM_CVAR(Int, gl_distsoftwaregl1xfog, 70, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	const bool useHQtablegen = true;
	const float invMul255 = 1.0f / 255.0f;

	if (useHQtablegen)
	{
		for (int i = 0; i < 256; i++)
		{
			// Normalize index to a 0.0f - 1.0f scale
			float f = (float)i * invMul255;

			// --- SOFTWARE SECTOR LIGHT MODE #8 (ORIGINAL EXE) ---
			// We keep the heavy 1.40f multiplier to lock down rich software shadows in the dark,
			// but we modify the decay power dynamically.
			float baseDensity0 = (float)gl_distsoftwaregl1xfog * 1.40f;

			// Added an aggressive power-scaling shift (-4.10f * f - 1.65f * (f * f)).
			// In dark-to-mid zones (f < 0.4), the quadratic component is near zero, keeping the shadows heavy.
			// In bright sectors (160 to 176, f > 0.6), the (f * f) term kicks in like a booster, 
			// slamming the fog density down to absolute zero way faster. This fully stops the geometry 
			// from prematurely drowning in fog at long distances, matching the clear look of the original Dark.
			distgl1xsoftwarefogtable[0][i] = baseDensity0 * expf(-4.10f * f - 1.65f * (f * f));

			// Hard safety cushion gate right at the edge of fullbright to prevent register overflow
			if (i >= 235) distgl1xsoftwarefogtable[0][i] = 0.0f;


			// --- VANILLA SECTOR LIGHT MODE #16 ---
			// So far, the formulas may be closer to exe but it looks darker and wrong but let's have it too.
			// Mapped onto the same high-velocity clearing curve for bright layers
			float baseDensity1 = 6.0f + (float)gl_distsoftwaregl1xfog * 1.65f;
			distgl1xsoftwarefogtable[1][i] = baseDensity1 * expf(-4.60f * f - 1.95f * (f * f));

			if (i >= 220) distgl1xsoftwarefogtable[1][i] = 0.0f;
		}
	}
	else
	{
		// ---=== PIECEWISE APPROXIMATION OF OUR HQ EXP MATRIX ===---
		int baseVal = gl_distsoftwaregl1xfog;

		// PATCH: Restored heavy base multipliers back to match 1.40f and 1.65f HQ anchors.
		// This brings back pristine, pitch-black software shadow definition in dark areas!
		int sftDarkAnchor = (int)(baseVal * 1.40f);
		int vanDarkAnchor = 6 + (int)(baseVal * 1.65f);

		for (int i = 0; i < 256; i++)
		{
			// 1. --- SOFTWARE SECTOR LIGHT MODE #8 (HQ-ALIGNED PIECEWISE) ---
			if (i < 32)
			{
				distgl1xsoftwarefogtable[0][i] = sftDarkAnchor - (sftDarkAnchor * 0.15f) * i / 32;
			}
			else if (i < 64)
			{
				distgl1xsoftwarefogtable[0][i] = (int)(sftDarkAnchor * 0.85f) - (sftDarkAnchor * 0.23f) * (i - 32) / 32;
			}
			else if (i < 96)
			{
				distgl1xsoftwarefogtable[0][i] = (int)(sftDarkAnchor * 0.62f) - (sftDarkAnchor * 0.25f) * (i - 64) / 32;
			}
			else if (i < 128)
			{
				// Zone 3: Central Mid-Tones. The slope begins to take a steep dive here.
				distgl1xsoftwarefogtable[0][i] = (int)(sftDarkAnchor * 0.37f) - (sftDarkAnchor * 0.20f) * (i - 96) / 32;
			}
			else if (i < 164)
			{
				// Zone 4: Upper Mid-Tones. Accelerated drop-off completely mimics the (f * f) booster!
				distgl1xsoftwarefogtable[0][i] = (int)(sftDarkAnchor * 0.17f) - (sftDarkAnchor * 0.11f) * (i - 128) / 36;
			}
			else if (i < 196)
			{
				// Zone 5: Bright Room Baseline. Dropped sharply near zero to keep distant walls clear.
				distgl1xsoftwarefogtable[0][i] = (int)(sftDarkAnchor * 0.06f) - (sftDarkAnchor * 0.04f) * (i - 164) / 32;
			}
			else if (i < 235)
			{
				distgl1xsoftwarefogtable[0][i] = (int)(sftDarkAnchor * 0.02f) - (sftDarkAnchor * 0.02f) * (i - 196) / (235 - 196);
			}
			else
			{
				distgl1xsoftwarefogtable[0][i] = 0.0f;
			}

			// 2. --- VANILLA SECTOR LIGHT MODE #16 (HQ-ALIGNED PIECEWISE) ---
			if (i < 32)
			{
				distgl1xsoftwarefogtable[1][i] = vanDarkAnchor - (vanDarkAnchor * 0.22f) * i / 32;
			}
			else if (i < 64)
			{
				distgl1xsoftwarefogtable[1][i] = (int)(vanDarkAnchor * 0.78f) - (vanDarkAnchor * 0.26f) * (i - 32) / 32;
			}
			else if (i < 96)
			{
				distgl1xsoftwarefogtable[1][i] = (int)(vanDarkAnchor * 0.52f) - (vanDarkAnchor * 0.24f) * (i - 64) / 32;
			}
			else if (i < 128)
			{
				distgl1xsoftwarefogtable[1][i] = (int)(vanDarkAnchor * 0.28f) - (vanDarkAnchor * 0.16f) * (i - 96) / 32;
			}
			else if (i < 160)
			{
				distgl1xsoftwarefogtable[1][i] = (int)(vanDarkAnchor * 0.12f) - (vanDarkAnchor * 0.09f) * (i - 128) / 32;
			}
			else if (i < 192)
			{
				distgl1xsoftwarefogtable[1][i] = (int)(vanDarkAnchor * 0.03f) - (vanDarkAnchor * 0.03f) * (i - 160) / 32;
			}
			else if (i < 220)
			{
				distgl1xsoftwarefogtable[1][i] = 0.0f;
			}
			else
			{
				distgl1xsoftwarefogtable[1][i] = 0.0f;
			}
		}
	}
}

//==========================================================================
//
// Get current light level
//
//==========================================================================

int CalcLightLevel(ELightMode lightmode, int lightlevel, int rellight, bool weapon, int blendfactor)
{
	int light;

	if (lightlevel <= 0) return 0;

	bool darklightmode = (isDarkLightMode(lightmode)) || (isSoftwareLighting(lightmode) && blendfactor > 0);

	if (darklightmode && lightlevel < 192 && !weapon) 
	{
		if (lightlevel > 100)
		{
			light = xs_CRoundToInt(192.f - (192 - lightlevel)* 1.87f);
			if (light + rellight < 20)
			{
				light = 20 + (light + rellight - 20) / 5;
			}
			else
			{
				light += rellight;
			}
		}
		else
		{
			light = (lightlevel + rellight) / 5;
		}

	}
	else
	{
		light=lightlevel+rellight;
	}

	// Fake contrast should never turn a positive value into 0.
	return clamp(light, 1, 255);
}

//==========================================================================
//
// Get current light color
//
//==========================================================================

PalEntry CalcLightColor(ELightMode lightmode, int light, PalEntry pe, int blendfactor)
{
	int r,g,b;

	if (blendfactor == 0)
	{
		if (isSoftwareLighting(lightmode))
		{
			return pe;
		}

		r = pe.r * light / 255;
		g = pe.g * light / 255;
		b = pe.b * light / 255;
	}
	else
	{
		// This is what Legacy does with colored light in 3D volumes. No, it doesn't really make sense...
		// It also doesn't translate well to software style lighting.
		int mixlight = light * (255 - blendfactor);

		r = (mixlight + pe.r * blendfactor) / 255;
		g = (mixlight + pe.g * blendfactor) / 255;
		b = (mixlight + pe.b * blendfactor) / 255;
	}
	return PalEntry(255, uint8_t(r), uint8_t(g), uint8_t(b));
}

//==========================================================================
//
// calculates the current fog density
//
//	Rules for fog:
//
//  1. If bit 4 of gl_lightmode is set always use the level's fog density. 
//     This is what Legacy's GL render does.
//	2. black fog means no fog and always uses the distfogtable based on the level's fog density setting
//	3. If outside fog is defined and the current fog color is the same as the outside fog
//	   the engine always uses the outside fog density to make the fog uniform across the Level->
//	   If the outside fog's density is undefined it uses the level's fog density and if that is
//	   not defined it uses a default of 70.
//	4. If a global fog density is specified it is being used for all fog on the level
//	5. If none of the above apply fog density is based on the light level as for the software renderer.
//
//==========================================================================

float GetFogDensity(FLevelLocals* Level, ELightMode lightmode, int lightlevel, PalEntry fogcolor, int sectorfogdensity, int blendfactor)
{
	float density;

	auto oldlightmode = lightmode;
	if (isSoftwareLighting(lightmode) && blendfactor > 0) lightmode = ELightMode::Doom;	// The blendfactor feature does not work with software-style lighting.

	if (lightmode == ELightMode::DoomLegacy)
	{
		// uses approximations of Legacy's default settings.
		density = Level->fogdensity ? (float)Level->fogdensity : 18;
	}
	else if (sectorfogdensity != 0)
	{
		// case 1: Sector has an explicit fog density set.
		density = (float)sectorfogdensity;
	}
	else if ((fogcolor.d & 0xffffff) == 0)
	{
		// case 2: black fog
		if ((!isDoomSoftwareLighting(lightmode) || blendfactor > 0) && !(Level->flags3 & LEVEL3_NOLIGHTFADE))
		{
			density = distfogtable[lightmode != ELightMode::LinearStandard][hw_ClampLight(lightlevel)];
		}
		else
		{
			density = 0;
		}
	}
	else if (Level->outsidefogdensity != 0 && APART(Level->info->outsidefog) != 0xff && (fogcolor.d & 0xffffff) == (Level->info->outsidefog & 0xffffff))
	{
		// case 3. outsidefogdensity has already been set as needed
		density = (float)Level->outsidefogdensity;
	}
	else  if (Level->fogdensity != 0)
	{
		// case 4: level has fog density set
		density = (float)Level->fogdensity;
	}
	else if (lightlevel < 248)
	{
		// case 5: use light level
		density = (float)clamp<int>(255 - lightlevel, 30, 255);
	}
	else
	{
		density = 0.f;
	}
	return density;
}

//==========================================================================
//
// Check if the current linedef is a candidate for a fog boundary
//
// Requirements for a fog boundary:
// - front sector has no fog
// - back sector has fog
// - at least one of both does not have a sky ceiling.
//
//==========================================================================

bool CheckFog(FLevelLocals* Level, sector_t *frontsector, sector_t *backsector, ELightMode lightmode)
{
	if (frontsector == backsector) return false;	// there can't be a boundary if both sides are in the same sector.

	// Check for fog boundaries. This needs a few more checks for the sectors

	PalEntry fogcolor = frontsector->Colormap.FadeColor;

	if ((fogcolor.d & 0xffffff) == 0)
	{
		return false;
	}
	else if (fogcolor.a != 0)
	{
	}
	else if (Level->outsidefogdensity != 0 && APART(Level->info->outsidefog) != 0xff && (fogcolor.d & 0xffffff) == (Level->info->outsidefog & 0xffffff))
	{
	}
	else  if (Level->fogdensity!=0 || lightmode == ELightMode::DoomLegacy)
	{
		// case 3: level has fog density set
	}
	else 
	{
		// case 4: use light level
		if (frontsector->lightlevel >= 248) return false;
	}

	fogcolor = backsector->Colormap.FadeColor;

	if ((fogcolor.d & 0xffffff) == 0)
	{
	}
	else if (Level->outsidefogdensity != 0 && APART(Level->info->outsidefog) != 0xff && (fogcolor.d & 0xffffff) == (Level->info->outsidefog & 0xffffff))
	{
		return false;
	}
	else  if (Level->fogdensity!=0 || lightmode == ELightMode::DoomLegacy)
	{
		// case 3: level has fog density set
		return false;
	}
	else 
	{
		// case 4: use light level
		if (backsector->lightlevel < 248) return false;
	}

	// in all other cases this might create more problems than it solves.
	return ((frontsector->GetTexture(sector_t::ceiling)!=skyflatnum || 
			 backsector->GetTexture(sector_t::ceiling)!=skyflatnum));
}
