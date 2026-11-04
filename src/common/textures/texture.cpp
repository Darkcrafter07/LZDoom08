/*
** texture.cpp
** The base texture class
**
**---------------------------------------------------------------------------
** Copyright 2004-2007 Randy Heit
** Copyright 2006-2018 Christoph Oelckers
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
**
*/

#include "printf.h"
#include "files.h"
#include "filesystem.h"

#include "textures.h"
#include "bitmap.h"
#include "colormatcher.h"
#include "c_dispatch.h"
#include "m_fixed.h"
#include "imagehelpers.h"
#include "image.h"
#include "formats/multipatchtexture.h"
#include "texturemanager.h"
#include "c_cvars.h"
#include "imagehelpers.h"
#include "v_video.h"
#include "v_font.h"

#include "common/textures/hw_material.h"
#include "common/rendering/gl_load/gl_interface.h"

// Wrappers to keep the definitions of these classes out of here.
IHardwareTexture* CreateHardwareTexture(int numchannels);

// Make sprite offset adjustment user-configurable per renderer.
int r_spriteadjustSW, r_spriteadjustHW;

//==========================================================================
//
// 
//
//==========================================================================

FTexture::FTexture (int lumpnum)
	:  SourceLump(lumpnum), bHasCanvas(false)
{
	bTranslucent = -1;
}

//===========================================================================
//
// FTexture::GetBgraBitmap
//
// Default returns just an empty bitmap. This needs to be overridden by
// any subclass that actually does return a software pixel buffer.
//
//===========================================================================

FBitmap FTexture::GetBgraBitmap(const PalEntry* remap, int* ptrans)
{
	FBitmap bmp;
	bmp.Create(Width, Height);
	return bmp;
}

//====================================================================
//
// CheckRealHeight
//
// Checks the posts in a texture and returns the lowest row (plus one)
// of the texture that is actually used.
//
//====================================================================

int FTexture::CheckRealHeight()
{
	auto pixels = Get8BitPixels(false);

	for(int h = GetHeight()-1; h>= 0; h--)
	{
		for(int w = 0; w < GetWidth(); w++)
		{
			if (pixels[h + w * GetHeight()] != 0)
			{
				return h;
			}
		}
	}
	return 0;
}

//===========================================================================
// 
//	Finds gaps in the texture which can be skipped by the renderer
//  This was mainly added to speed up one area in E4M6 of 007LTSD
//
//===========================================================================

bool FTexture::FindHoles(const unsigned char * buffer, int w, int h)
{
	const unsigned char * li;
	int y, x;
	int startdraw, lendraw;
	int gaps[5][2];
	int gapc = 0;

	// already done!
	if (areacount) return false;

	// flats don't have transparent parts - [Darkcrafter07]: really? 3D floors?
	//if (UseType == ETextureType::Flat) return false;

	areacount = -1;	//whatever happens next, it shouldn't be done twice!


	// large textures and non-images are excluded for performance reasons
	if (h > 512 || !GetImage()) return false;

	// --- [Alpha-Channel Transparency Pre-Flight Check] ---
	// CRITICAL FIX: Before executing the original broken loop mergers, run 
	// a fast raw hardware scan over the actual alpha channel byte-array.
	// If the image contains zero transparent pixels (alpha == 0), it is 100% solid.
	// This bypasses the bugged end-of-loop startdraw == 0 crash gates.
	bool hasAnyTransparentPixels = false;
	const unsigned char *alphaPtr = buffer + 3; // Jump straight to the first alpha byte
	int totalPixels = w * h;

	for (int p = 0; p < totalPixels; p++, alphaPtr += 4)
	{
		if (*alphaPtr == 0) // Found a true alpha hole/cutout pixel!
		{
			hasAnyTransparentPixels = true;
			break;
		}
	}

	if (!hasAnyTransparentPixels)
	{
		areas = nullptr;
		areacount = 0;
		return false; // Solid texture confirmed, do not create split zones!
	}

	startdraw = -1;
	lendraw = 0;
	for (y = 0; y < h; y++)
	{
		li = buffer + w * y * 4 + 3;

		for (x = 0; x < w; x++, li += 4)
		{
			if (*li != 0) break;
		}

		if (x != w)
		{
			// non - transparent
			if (startdraw == -1)
			{
				startdraw = y;
				// merge transparent gaps of less than 16 pixels into the last drawing block
				if (gapc && y <= gaps[gapc - 1][0] + gaps[gapc - 1][1] + 16)
				{
					gapc--;
					startdraw = gaps[gapc][0];
					lendraw = y - startdraw;
				}
				if (gapc == 4) 
				{
					// [Darkcrafter07]: Too many splits, but we must explicitly enforce Masked flag
					// because the pre-flight check confirmed that alpha gaps actually exist!
					Masked = true;
					areacount = 0;
					return false;
				}
			}
			lendraw++;
		}
		else if (startdraw != -1)
		{
			if (lendraw == 1) lendraw = 2;
			gaps[gapc][0] = startdraw;
			gaps[gapc][1] = lendraw;
			gapc++;

			startdraw = -1;
			lendraw = 0;
		}
	}
	if (startdraw != -1)
	{
		gaps[gapc][0] = startdraw;
		gaps[gapc][1] = lendraw;
		gapc++;
	}

	// --- Validation Gate ---
	// Since the pre-flight scanner already proved the existence of alpha cuts,
	// bypass bugged logic line "if (startdraw == 0 && lendraw == h)".
	
	// [Darkcrafter07]: Ensure the engine treats this texture as truly masked via UZDoom API variable
	Masked = (gapc > 0) || hasAnyTransparentPixels;

	if (gapc > 0)
	{
		// [Darkcrafter07]: In v4 architecture, memory allocation must use the centralized ImageArena allocator
		FloatRect * rcs = (FloatRect*)ImageArena.Alloc(gapc * sizeof(sizeof(FloatRect)));

		for (x = 0; x < gapc; x++)
		{
			// gaps are stored as texture (u/v) coordinates
			rcs[x].width = rcs[x].left = -1.0f;
			rcs[x].top = (float)gaps[x][0] / (float)h;
			rcs[x].height = (float)gaps[x][1] / (float)h;
		}
		areas = rcs;
	}
	else
	{
		areas = nullptr;
	}

	return true;
}

//----------------------------------------------------------------------------
//
//
//
//----------------------------------------------------------------------------

void FTexture::CheckTrans(unsigned char* buffer, int size, int trans)
{
	if (bTranslucent == -1)
	{
		bTranslucent = trans;
		if (trans == -1)
		{
			uint32_t* dwbuf = (uint32_t*)buffer;
			for (int i = 0; i < size; i++)
			{
				uint32_t alpha = dwbuf[i] >> 24;

				if (alpha != 0xff && alpha != 0)
				{
					bTranslucent = 1;
					return;
				}
			}
			bTranslucent = 0;
		}
	}
}


//===========================================================================
// 
// smooth the edges of transparent fields in the texture
//
//===========================================================================

#ifdef WORDS_BIGENDIAN
#define MSB 0
#define SOME_MASK 0xffffff00
#else
#define MSB 3
#define SOME_MASK 0x00ffffff
#endif

#define CHKPIX(ofs) (l1[(ofs)*4+MSB]==255 ? (( ((uint32_t*)l1)[0] = ((uint32_t*)l1)[ofs]&SOME_MASK), trans=true ) : false)

bool FTexture::SmoothEdges(unsigned char* buffer, int w, int h)
{
	int x, y;
	bool trans = buffer[MSB] == 0; // If I set this to false here the code won't detect textures 
								   // that only contain transparent pixels.
	bool semitrans = false;
	unsigned char* l1;

	if (h <= 1 || w <= 1) return false;  // makes (a) no sense and (b) doesn't work with this code!

	l1 = buffer;


	if (l1[MSB] == 0 && !CHKPIX(1)) CHKPIX(w);
	else if (l1[MSB] < 255) semitrans = true;
	l1 += 4;
	for (x = 1; x < w - 1; x++, l1 += 4)
	{
		if (l1[MSB] == 0 && !CHKPIX(-1) && !CHKPIX(1)) CHKPIX(w);
		else if (l1[MSB] < 255) semitrans = true;
	}
	if (l1[MSB] == 0 && !CHKPIX(-1)) CHKPIX(w);
	else if (l1[MSB] < 255) semitrans = true;
	l1 += 4;

	for (y = 1; y < h - 1; y++)
	{
		if (l1[MSB] == 0 && !CHKPIX(-w) && !CHKPIX(1)) CHKPIX(w);
		else if (l1[MSB] < 255) semitrans = true;
		l1 += 4;
		for (x = 1; x < w - 1; x++, l1 += 4)
		{
			if (l1[MSB] == 0 && !CHKPIX(-w) && !CHKPIX(-1) && !CHKPIX(1) && !CHKPIX(-w - 1) && !CHKPIX(-w + 1) && !CHKPIX(w - 1) && !CHKPIX(w + 1)) CHKPIX(w);
			else if (l1[MSB] < 255) semitrans = true;
		}
		if (l1[MSB] == 0 && !CHKPIX(-w) && !CHKPIX(-1)) CHKPIX(w);
		else if (l1[MSB] < 255) semitrans = true;
		l1 += 4;
	}

	if (l1[MSB] == 0 && !CHKPIX(-w)) CHKPIX(1);
	else if (l1[MSB] < 255) semitrans = true;
	l1 += 4;
	for (x = 1; x < w - 1; x++, l1 += 4)
	{
		if (l1[MSB] == 0 && !CHKPIX(-w) && !CHKPIX(-1)) CHKPIX(1);
		else if (l1[MSB] < 255) semitrans = true;
	}
	if (l1[MSB] == 0 && !CHKPIX(-w)) CHKPIX(-1);
	else if (l1[MSB] < 255) semitrans = true;

	return trans || semitrans;
}

//===========================================================================
// 
// Post-process the texture data after the buffer has been created
//
//===========================================================================

bool FTexture::ProcessData(unsigned char* buffer, int w, int h, bool ispatch)
{
	if (Masked)
	{
		Masked = SmoothEdges(buffer, w, h);
		if (Masked && !ispatch) FindHoles(buffer, w, h);
	}
	return true;
}

//===========================================================================
// 
//	Initializes the buffer for the texture data
//
//===========================================================================

FTextureBuffer FTexture::CreateTexBuffer(int translation, int flags)
{
	FTextureBuffer result;
	if (flags & CTF_Indexed)
	{
		// Indexed textures will never be translated and never be scaled.
		int w = GetWidth(), h = GetHeight();

		auto store = Get8BitPixels(false);
		const uint8_t* p = store.Data();

		result.mBuffer = new uint8_t[w * h];
		result.mWidth = w;
		result.mHeight = h;
		result.mContentId = 0;
		ImageHelpers::FlipNonSquareBlock(result.mBuffer, p, h, w, h);
	}
	else
	{
		unsigned char* buffer = nullptr;
		int W, H;
		int isTransparent = -1;
		bool checkonly = !!(flags & CTF_CheckOnly);

		int exx = !!(flags & CTF_Expand);
		// ==============================================================================
		// DARKCRAFTER07: LEGACY HIGH-RES BRIGHTMAP BUFFER SYNCHRONIZATION
		// ==============================================================================
		// If we are in legacy mode and processing a brightmap layer that belongs to a
		// high-resolution pack texture replacement, we must enforce the master buffer
		// allocation size (W, H) to match the parent HD dimensions. This completely
		// eliminates memory corruption loops inside CopyTrueColorPixels().

		// Resolve the active FGameTexture container using GZ4x explicit id cast solver
		FGameTexture* thisGameTex = TexMan.GetGameTexture(FSetTextureID(SourceLump));
		FGameTexture* parentTexLink = (thisGameTex != nullptr) ? thisGameTex->ParentTexture : nullptr;

		// Universal High-Res Pack (HRP) buffer synchronization pass
		bool isVirtualHiResBrightmap = false;
		int parentW = 0, parentH = 0;

		// Re-mapped to read dimensions via the internal FTexture link
		if (gl.legacyMode && (parentTexLink != nullptr) && (parentTexLink->GetTexture() != nullptr))
		{
			parentW = parentTexLink->GetTexture()->GetWidth();
			parentH = parentTexLink->GetTexture()->GetHeight();

			// If parent texture dimensions are significantly larger, a hi-res pack is active
			if (parentW > GetWidth() || parentH > GetHeight())
			{
				isVirtualHiResBrightmap = true;
				W = parentW + 2 * exx;
				H = parentH + 2 * exx;
			}
		}

		// Standard fallback for low-res assets or if sizes already match
		if (!isVirtualHiResBrightmap)
		{
			W = GetWidth() + 2 * exx;
			H = GetHeight() + 2 * exx;
		}

		if (!checkonly)
		{
			auto remap = translation <= 0 || IsLuminosityTranslation(translation) ? nullptr : GPalette.TranslationToTable(translation);
			if (remap && remap->Inactive) remap = nullptr;
			if (remap) translation = remap->Index;

			int trans;
			
			if (isVirtualHiResBrightmap)
			{
				// Secure the original tiny low-res brightmap mask source data
				int lowW = GetWidth();
				int lowH = GetHeight();
				unsigned char *lowResSrc = new unsigned char[lowW * lowH * 4];
				memset(lowResSrc, 0, lowW * lowH * 4);
				FBitmap lowResBmp(lowResSrc, lowW * 4, lowW, lowH);
				
				// Replaced deleted CopyTrueColorPixels with native GetBgraBitmap pipeline dump
				auto ThisLowResPixels = this->GetBgraBitmap(nullptr, &trans);
				lowResBmp.Blit(0, 0, ThisLowResPixels);

				buffer = new unsigned char[W * (H + 1) * 4];
				memset(buffer, 0, W * (H + 1) * 4);

				// Nearest-neighbor high-fidelity software upscale interpolation
				for (int y = 0; y < H; y++)
				{
					for (int x = 0; x < W; x++)
					{
						int srcX = (x * lowW) / W;
						int srcY = (y * lowH) / H;
						if (srcX >= lowW) srcX = lowW - 1;
						if (srcY >= lowH) srcY = lowH - 1;

						int destIdx = 4 * (y * W + x);
						int srcIdx = 4 * (srcY * lowW + srcX);

						buffer[destIdx] = lowResSrc[srcIdx];
						buffer[destIdx + 1] = lowResSrc[srcIdx + 1];
						buffer[destIdx + 2] = lowResSrc[srcIdx + 2];
						buffer[destIdx + 3] = lowResSrc[srcIdx + 3];
					}
				}
				delete[] lowResSrc;
				isTransparent = bTranslucent; 
			}
			else
			{
				auto Pixels = GetBgraBitmap(remap ? remap->Palette : nullptr, &trans);
				
				if(!exx && Pixels.ClipRect.x == 0 && Pixels.ClipRect.y == 0 && Pixels.ClipRect.width == Pixels.Width && Pixels.ClipRect.height == Pixels.Height && (Pixels.FreeBuffer || !IsLuminosityTranslation(translation)))
				{
					buffer = Pixels.data;
					result.mFreeBuffer = Pixels.FreeBuffer;
					Pixels.FreeBuffer = false;
				}
				else
				{
					buffer = new unsigned char[W * (H + 1) * 4];
					memset(buffer, 0, W * (H + 1) * 4);

					FBitmap bmp(buffer, W * 4, W, H);

					bmp.Blit(exx, exx, Pixels);
				}
				isTransparent = bTranslucent;
			}
			
			if (IsLuminosityTranslation(translation))
			{
				V_ApplyLuminosityTranslation(LuminosityTranslationDesc::fromInt(translation), buffer, W * H);
			}

			// --- Darkcrafter07
			// --- Black and white brightmaps look ugly in GL1x/GL2x modes
			// --- so colorize them and cut them according to base texture transparency
			if (gl.legacyMode && (parentTexLink != nullptr) && (parentTexLink->GetTexture() != nullptr))
			{
				unsigned char * parentBuf = new unsigned char[W * (H + 1) * 4];
				memset(parentBuf, 0, W * (H + 1) * 4);
				FBitmap parentBmp(parentBuf, W * 4, W, H);

				// HRP textures initialized via script scaling metrics require pure unshifted 
				// pixel mapping. By forcing '0, 0' offset bounds instead of passing 'exx' filters,
				// the high-res diffuse texture aligns perfectly with the upscaled brightmap mask.
				if (isVirtualHiResBrightmap)
				{
					// Dump raw high-res parent bits through GetBgraBitmap
					auto ParentHiResPixels = parentTexLink->GetTexture()->GetBgraBitmap(remap ? remap->Palette : nullptr, &trans);
					parentBmp.Blit(0, 0, ParentHiResPixels);
				}
				else
				{
					// Standard fallback path using the correct FTexture instance channel routing
					auto ParentPixels = parentTexLink->GetTexture()->GetBgraBitmap(remap ? remap->Palette : nullptr, &trans);
					parentBmp.Blit(exx, exx, ParentPixels);
				}

				const int clampMaxPxBrightness = 192;  // To decrease overburn slightly

				// 3. Process pixels: Colorize, Desaturate and Cut by Parent Alpha
				for (int i = 0; i < W * H; i++)
				{
					int idx = 4 * i;

					if (parentBuf[idx + 3] == 0)
					{
						buffer[idx] = 0;     
						buffer[idx + 1] = 0; 
						buffer[idx + 2] = 0; 
						buffer[idx + 3] = 0; 
						continue;
					}

					// --- OVEREXPOSURE CLAMP ---
					// Soft-clip the extreme white hotspots of the raw mask BEFORE any math is executed.
					// This keeps middle-low intensities (glowing eyes, dim runes) uncorrupted and punchy,
					// while shielding the frame buffer from pure white saturation leaks.
					if (buffer[idx] > clampMaxPxBrightness) buffer[idx] = clampMaxPxBrightness;
					if (buffer[idx + 1] > clampMaxPxBrightness) buffer[idx + 1] = clampMaxPxBrightness;
					if (buffer[idx + 2] > clampMaxPxBrightness) buffer[idx + 2] = clampMaxPxBrightness;
					if (buffer[idx + 3] > clampMaxPxBrightness) buffer[idx + 3] = clampMaxPxBrightness;

					// Multiply brightmap mask by parent colors (Base * Brightmap / 255)
					const float invMaxDoomLightLevel = 1.0f / 255.0f;
					float r = (float)(buffer[idx] * parentBuf[idx]) * invMaxDoomLightLevel;
					float g = (float)(buffer[idx + 1] * parentBuf[idx + 1]) * invMaxDoomLightLevel;
					float b = (float)(buffer[idx + 2] * parentBuf[idx + 2]) * invMaxDoomLightLevel;

					// Calculate parent's alpha ratio (0.0 to 1.0)
					float alphaRatio = (float)parentBuf[idx + 3] * invMaxDoomLightLevel;

					// Calculate luminance (Rec.709) for desaturation
					float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;

					// Mixing: 95% color + 5% luma (desaturate by 5%)
					float saturationFactor = 0.95f;

					// Final color calculation with saturation adjustment
					float finalR = luma + (r - luma) * saturationFactor;
					float finalG = luma + (g - luma) * saturationFactor;
					float finalB = luma + (b - luma) * saturationFactor;

					// Apply parent alpha to color to ensure it fades out where parent is translucent
					buffer[idx] = (unsigned char)clamp<float>(finalR * alphaRatio, 0, 255);
					buffer[idx + 1] = (unsigned char)clamp<float>(finalG * alphaRatio, 0, 255);
					buffer[idx + 2] = (unsigned char)clamp<float>(finalB * alphaRatio, 0, 255);

					// Final alpha should be clamped to parent's alpha to avoid "ghosting"
					buffer[idx + 3] = (unsigned char)clamp<int>(buffer[idx + 3] * alphaRatio, 0, 255);
				}
				delete[] parentBuf;
			}
			// ------------------------------------------------------------------------

			if (remap == nullptr && !isVirtualHiResBrightmap)
			{
				CheckTrans(buffer, W * H, trans);
				isTransparent = bTranslucent;
			}
			else if (remap != nullptr)
			{
				isTransparent = 0;
				// A translated image is not conclusive for setting the texture's transparency info.
			}
		}

		// alpha texture for legacy mode
		// if (alphatrans) // left here for historic records tracking

		if (GetImage())
		{
			FContentIdBuilder builder;
			builder.id = 0;
			builder.imageID = GetImage()->GetId();
			builder.translation = max(0, translation);
			builder.expand = exx;
			result.mContentId = builder.id;
		}
		else result.mContentId = 0; // for non-image backed textures this has no meaning so leave it at 0.	

		result.mBuffer = buffer;
		result.mWidth = W;
		result.mHeight = H;

		// Only do postprocessing for image-backed textures. (i.e. not for the burn texture which can also pass through here.)
		if (GetImage() && flags & CTF_ProcessData)
		{
			if (flags & CTF_Upscale) CreateUpsampledTextureBuffer(result, !!isTransparent, checkonly);

			if (!checkonly) ProcessData(result.mBuffer, result.mWidth, result.mHeight, false);
		}
	}
	return result;
}

//===========================================================================
// 
// Dummy texture for the 0-entry.
//
//===========================================================================

bool FTexture::DetermineTranslucency()
{
		// This will calculate all we need, so just discard the result.
		CreateTexBuffer(0);
	return !!bTranslucent;
}

//===========================================================================
// 
// the default just returns an empty texture.
//
//===========================================================================

TArray<uint8_t> FTexture::Get8BitPixels(bool alphatex)
{
	TArray<uint8_t> Pixels(Width * Height, true);
	memset(Pixels.Data(), 0, Width * Height);
	return Pixels;
}

//===========================================================================
// 
//  Finds empty space around the texture. 
//  Used for sprites that got placed into a huge empty frame.
//
//===========================================================================

bool FTexture::TrimBorders(uint16_t* rect)
{

	auto texbuffer = CreateTexBuffer(0);
	int w = texbuffer.mWidth;
	int h = texbuffer.mHeight;
	auto Buffer = texbuffer.mBuffer;

	if (texbuffer.mBuffer == nullptr)
	{
		return false;
	}
	if (w != Width || h != Height)
	{
		// external Hires replacements cannot be trimmed.
		return false;
	}

	int size = w * h;
	if (size == 1)
	{
		// nothing to be done here.
		rect[0] = 0;
		rect[1] = 0;
		rect[2] = 1;
		rect[3] = 1;
		return true;
	}
	int first, last;

	for (first = 0; first < size; first++)
	{
		if (Buffer[first * 4 + 3] != 0) break;
	}
	if (first >= size)
	{
		// completely empty
		rect[0] = 0;
		rect[1] = 0;
		rect[2] = 1;
		rect[3] = 1;
		return true;
	}

	for (last = size - 1; last >= first; last--)
	{
		if (Buffer[last * 4 + 3] != 0) break;
	}

	rect[1] = first / w;
	rect[3] = 1 + last / w - rect[1];

	rect[0] = 0;
	rect[2] = w;

	unsigned char* bufferoff = Buffer + (rect[1] * w * 4);
	h = rect[3];

	for (int x = 0; x < w; x++)
	{
		for (int y = 0; y < h; y++)
		{
			if (bufferoff[(x + y * w) * 4 + 3] != 0) goto outl;
		}
		rect[0]++;
	}
outl:
	rect[2] -= rect[0];

	for (int x = w - 1; rect[2] > 1; x--)
	{
		for (int y = 0; y < h; y++)
		{
			if (bufferoff[(x + y * w) * 4 + 3] != 0)
			{
				return true;
			}
		}
		rect[2]--;
	}
	return true;
}

//===========================================================================
//
// Create a hardware texture for this texture image.
//
//===========================================================================

IHardwareTexture* FTexture::GetHardwareTexture(int translation, int scaleflags)
{
	int indexed = scaleflags & CTF_Indexed;
	if (indexed) translation = -1;
	IHardwareTexture* hwtex = SystemTextures.GetHardwareTexture(translation, scaleflags);
	if (hwtex == nullptr)
	{
		hwtex = screen->CreateHardwareTexture(indexed? 1 : 4);
		SystemTextures.AddHardwareTexture(translation, scaleflags, hwtex);
	}
	return hwtex;
}


//==========================================================================
//
// this must be copied back to textures.cpp later.
//
//==========================================================================

FWrapperTexture::FWrapperTexture(int w, int h, int bits)
{
	Width = w;
	Height = h;
	Format = bits;
	//bNoCompress = true;
	auto hwtex = screen->CreateHardwareTexture(4);
	// todo: Initialize here.
	SystemTextures.AddHardwareTexture(0, false, hwtex);
}

