//==========================================================================
//
// v_blend.h
// [Darkcrafter07] - Unified interface for screen blending and colormaps management
//
//==========================================================================

#pragma once

#include "vectors.h"
#include "palentry.h"

struct sector_t;
struct player_t;

// Base screen blend calculators from v_blend.cpp
void V_AddBlend(float r, float g, float b, float a, float v_blend[4]);
void V_AddPlayerBlend(player_t *CPlayer, float blend[4], float maxinvalpha, int maxpainblend);

// The main accumulator used by the hardware renderers to calculate full screen screen overlays
FVector4 V_CalcBlend(sector_t* viewsector, PalEntry* modulateColor);


