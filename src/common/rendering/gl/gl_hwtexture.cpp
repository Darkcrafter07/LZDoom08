/*
** gl_hwtexture.cpp
** GL texture abstraction
**
**---------------------------------------------------------------------------
** Copyright 2019 Christoph Oelckers
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

// gl_hwtexture.cpp
#include "gl_system.h"

#include "c_cvars.h"
#include "hw_material.h"

#include "gl_interface.h"
#include "hw_cvars.h"
#include "gl_debug.h"
#include "gl_renderer.h"
#include "gl_renderstate.h"
#include "gl_samplers.h"
#include "gl_hwtexture.h"

namespace OpenGLRenderer
{


TexFilter_s TexFilter[] = {
	{GL_NEAREST,					GL_NEAREST,		false},
	{GL_NEAREST_MIPMAP_NEAREST,		GL_NEAREST,		true},
	{GL_LINEAR,						GL_LINEAR,		false},
	{GL_LINEAR_MIPMAP_NEAREST,		GL_LINEAR,		true},
	{GL_LINEAR_MIPMAP_LINEAR,		GL_LINEAR,		true},
	{GL_NEAREST_MIPMAP_LINEAR,		GL_NEAREST,		true},
	{GL_LINEAR_MIPMAP_LINEAR,		GL_NEAREST,		true},
};

//===========================================================================
// 
//	Static texture data
//
//===========================================================================
unsigned int FHardwareTexture::lastbound[FHardwareTexture::MAX_TEXTURES];

//==========================================================================
//
// Performs manual CPU-side bilinear scaling to pad non-power-of-two (NPOT)
// textures into hardware-compliant square (POT) shapes for legacy GPUs.
//
//==========================================================================
static void ManualScaleImage(GLenum format, GLint w, GLint h, GLenum type, const void* input, GLint rw, GLint rh, GLenum typeout, void* output)
{
	unsigned char* src = (unsigned char*)input;
	unsigned char* dst = (unsigned char*)output;

	float x_ratio = (float)w / (float)rw;
	float y_ratio = (float)h / (float)rh;

	for (int y = 0; y < rh; y++)
	{
		float src_y_exact = (float)y * y_ratio;
		int src_y1 = (int)src_y_exact;
		int src_y2 = src_y1 + 1;
		float y_blend = src_y_exact - (float)src_y1;

		if (src_y2 >= h) { src_y2 = h - 1; src_y1 = h - 1; y_blend = 0.0f; }

		for (int x = 0; x < rw; x++)
		{
			float src_x_exact = (float)x * x_ratio;
			int src_x1 = (int)src_x_exact;
			int src_x2 = src_x1 + 1;
			float x_blend = src_x_exact - (float)src_x1;

			if (src_x2 >= w) { src_x2 = w - 1; src_x1 = w - 1; x_blend = 0.0f; }

			int idx11 = (src_y1 * w + src_x1) * 4;
			int idx12 = (src_y1 * w + src_x2) * 4;
			int idx21 = (src_y2 * w + src_x1) * 4;
			int idx22 = (src_y2 * w + src_x2) * 4;
			int dst_idx = (y * rw + x) * 4;

			for (int i = 0; i < 4; i++)
			{
				float val11 = src[idx11 + i];
				float val12 = src[idx12 + i];
				float val21 = src[idx21 + i];
				float val22 = src[idx22 + i];

				float top_avg = val11 * (1.0f - x_blend) + val12 * x_blend;
				float bottom_avg = val21 * (1.0f - x_blend) + val22 * x_blend;
				dst[dst_idx + i] = (unsigned char)(top_avg * (1.0f - y_blend) + bottom_avg * y_blend);
			}
		}
	}
}

//===========================================================================
// 
//	Loads the texture image into the hardware
//
// NOTE: For some strange reason I was unable to find the source buffer
// should be one line higher than the actual texture. I got extremely
// strange crashes deep inside the GL driver when I didn't do it!
//
//===========================================================================

unsigned int FHardwareTexture::CreateTexture(unsigned char * buffer, int w, int h, int texunit, bool mipmap, const char *name)
{
	int rh,rw;
	int texformat = GL_RGBA8;// TexFormat[gl_texture_format];
	bool deletebuffer=false;

	/*
	if (forcenocompression)
	{
		texformat = GL_RGBA8;
	}
	*/
	bool firstCall = glTexID == 0;
	if (firstCall)
	{
		glGenTextures(1, &glTexID);
	}

	int textureBinding = UINT_MAX;
	if (texunit == -1)	glGetIntegerv(GL_TEXTURE_BINDING_2D, &textureBinding);
	if (texunit > 0) glActiveTexture(GL_TEXTURE0+texunit);
	if (texunit >= 0) lastbound[texunit] = glTexID;
	glBindTexture(GL_TEXTURE_2D, glTexID);

	FGLDebug::LabelObject(GL_TEXTURE, glTexID, name);

	rw = GetTexDimension(w);
	rh = GetTexDimension(h);

	if ((gl.gl1path || gl.glnpot) && buffer != nullptr)
	//if ((gl.legacyMode || gl.gl1path || gl.glnpot) && buffer != nullptr)
	{
		if (rw != w || rh != h)
		{
			unsigned char* scaledbuffer = (unsigned char*)calloc(4, rw * (rh + 1));
			if (scaledbuffer != nullptr)
			{
				ManualScaleImage(GL_BGRA, w, h, GL_UNSIGNED_BYTE, buffer, rw, rh, GL_UNSIGNED_BYTE, scaledbuffer);
				if (deletebuffer && buffer != nullptr) free(buffer);

				deletebuffer = true;
				buffer = scaledbuffer;
			}
		}
	}
	if (glBufferID > 0)
	{
		glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
		buffer = nullptr;
	}
	else if (!buffer)
	{
		// The texture must at least be initialized if no data is present.
		mipmapped = false;
		buffer=(unsigned char *)calloc(4,rw * (rh+1));
		deletebuffer=true;
		//texheight=-h;	
	}
	else
	{
		if (rw < w || rh < h)
		{
			// The texture is larger than what the hardware can handle so scale it down.
			unsigned char * scaledbuffer=(unsigned char *)calloc(4,rw * (rh+1));
			if (scaledbuffer)
			{
				Resize(w, h, rw, rh, buffer, scaledbuffer);
				deletebuffer=true;
				buffer=scaledbuffer;
			}
		}
	}
	// store the physical size.

	int sourcetype;
	if (glTextureBytes > 0)
	{
		if (glTextureBytes < 4) glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		static const int ITypes[] = { GL_R8, GL_RG8, GL_RGB8, GL_RGBA8 };
		static const int STypes[] = { GL_RED, GL_RG, GL_BGR, GL_BGRA };

		texformat = ITypes[glTextureBytes - 1];
		sourcetype = STypes[glTextureBytes - 1];
	}
	else
	{
		sourcetype = GL_BGRA;
	}

	if (!firstCall && glBufferID > 0)
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, rw, rh, sourcetype, GL_UNSIGNED_BYTE, buffer);
	else
		glTexImage2D(GL_TEXTURE_2D, 0, texformat, rw, rh, 0, sourcetype, GL_UNSIGNED_BYTE, buffer);

	if (deletebuffer && buffer) free(buffer);
	else if (glBufferID)
	{
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	}

	if (mipmap && TexFilter[gl_texture_filter].mipmapping)
	{
		glGenerateMipmap(GL_TEXTURE_2D);
		mipmapped = true;
	}

	if (texunit > 0) glActiveTexture(GL_TEXTURE0);
	else if (texunit == -1) glBindTexture(GL_TEXTURE_2D, textureBinding);
	return glTexID;
}


//===========================================================================
// 
//
//
//===========================================================================
void FHardwareTexture::AllocateBuffer(int w, int h, int texelsize)
{
	int rw = GetTexDimension(w);
	int rh = GetTexDimension(h);
	if (texelsize < 1 || texelsize > 4) texelsize = 4;
	glTextureBytes = texelsize;
	bufferpitch = w;

	// Legacy Fixed-Pipeline hardware (OpenGL 1.x / 2.0) lacks PBO support
	// Attempting to use GL_PIXEL_UNPACK_BUFFER drops pixels data, resulting in blank black sheets.
	if (gl.legacyMode || gl.gl1path)
	{
		glBufferID = 0; // Disable PBO backing, force traditional CPU-side pointer arrays path
		return;
	}

	// ---=== ORIGINAL MODERN PROFILE SHADER PATHWAY (GL3x+) ===---
	if (rw == w || rh == h)
	{
		glGenBuffers(1, &glBufferID);
		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, glBufferID);
		glBufferData(GL_PIXEL_UNPACK_BUFFER, w * h * texelsize, nullptr, GL_STREAM_DRAW);
		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	}
}


uint8_t* FHardwareTexture::MapBuffer()
{
	if (gl.legacyMode || gl.gl1path)
	{
		return nullptr; // Forces the engine to skip raw mapped memory mutations inside legacy drawer queues
	}

	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, glBufferID);
	return (uint8_t*)glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY);
}

//===========================================================================
// 
//	Destroys the texture
//
//===========================================================================
FHardwareTexture::~FHardwareTexture() 
{ 
	if (glTexID != 0) glDeleteTextures(1, &glTexID);
	if (glBufferID != 0) glDeleteBuffers(1, &glBufferID);
}


//===========================================================================
// 
//	Binds this patch
//
//===========================================================================
unsigned int FHardwareTexture::Bind(int texunit, bool needmipmap)
{
	if (glTexID != 0)
	{
		if (lastbound[texunit] == glTexID) return glTexID;
		lastbound[texunit] = glTexID;
		if (texunit != 0) glActiveTexture(GL_TEXTURE0 + texunit);
		glBindTexture(GL_TEXTURE_2D, glTexID);
		// Check if we need mipmaps on a texture that was creted without them.
		if (needmipmap && !mipmapped && TexFilter[gl_texture_filter].mipmapping)
		{
			glGenerateMipmap(GL_TEXTURE_2D);
			mipmapped = true;
		}
		if (texunit != 0) glActiveTexture(GL_TEXTURE0);
		return glTexID;
	}
	return 0;
}

void FHardwareTexture::Unbind(int texunit)
{
	if (lastbound[texunit] != 0)
	{
		if (texunit != 0) glActiveTexture(GL_TEXTURE0+texunit);
		glBindTexture(GL_TEXTURE_2D, 0);
		if (texunit != 0) glActiveTexture(GL_TEXTURE0);
		lastbound[texunit] = 0;
	}
}

void FHardwareTexture::UnbindAll()
{
	for(int texunit = 0; texunit < 16; texunit++)
	{
		Unbind(texunit);
	}
}

//===========================================================================
// 
//	Creates a depth buffer for this texture
//
//===========================================================================

int FHardwareTexture::GetDepthBuffer(int width, int height)
{
	if (glDepthID == 0)
	{
		// [Darkcrafter07] - Context validation format enforcement
		GLenum internalFormat = GL_DEPTH24_STENCIL8;
		if (gl.legacyMode || gl.gl1path)
		{
			internalFormat = GL_DEPTH_COMPONENT24;
		}

		glGenRenderbuffers(1, &glDepthID);
		glBindRenderbuffer(GL_RENDERBUFFER, glDepthID);
		glRenderbufferStorage(GL_RENDERBUFFER, internalFormat,
			GetTexDimension(width), GetTexDimension(height));
		glBindRenderbuffer(GL_RENDERBUFFER, 0);
	}
	return glDepthID;
}


//===========================================================================
// 
//	Binds this texture's surfaces to the current framrbuffer
//
//===========================================================================

void FHardwareTexture::BindToFrameBuffer(int width, int height)
{
	width = GetTexDimension(width);
	height = GetTexDimension(height);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, glTexID, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, GetDepthBuffer(width, height));
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, GetDepthBuffer(width, height));
}


//===========================================================================
// 
//	Binds a texture to the renderer
//
//===========================================================================

bool FHardwareTexture::BindOrCreate(FTexture* tex, int texunit, int clampmode, int translation, int flags)
{
	if (gl.legacyMode)
	{
		return BindOrCreateGL1x(tex, texunit, clampmode, translation, flags);
	}
	else
	{
		return BindOrCreateGL3x(tex, texunit, clampmode, translation, flags);
	}
}

bool FHardwareTexture::BindOrCreateGL1x(FTexture* tex, int texunit, int clampmode, int translation, int flags)
{
	bool needmipmap = (clampmode <= CLAMP_XY) && !forcenofilter;

	if (!Bind(texunit, needmipmap))
	{
		if (flags & CTF_Indexed)
		{
			glTextureBytes = 1;
			forcenofilter = true;
			needmipmap = false;
		}
		int w = 0, h = 0;

		FTextureBuffer texbuffer;

		if (!tex->isHardwareCanvas())
		{
			texbuffer = tex->CreateTexBuffer(translation, flags | CTF_ProcessData);
			w = texbuffer.mWidth;
			h = texbuffer.mHeight;
		}
		else
		{
			w = tex->GetWidth();
			h = tex->GetHeight();
		}

		// Allocates texture via our POT-aligned manual resizer pipeline
		if (!CreateTexture(texbuffer.mBuffer, w, h, texunit, needmipmap, "FHardwareTexture.BindOrCreateGL1x"))
		{
			return false;
		}
	}

	if (forcenofilter && clampmode <= CLAMP_XY) clampmode += CLAMP_NOFILTER - CLAMP_NONE;

	glActiveTexture(GL_TEXTURE0 + texunit);
	glBindTexture(GL_TEXTURE_2D, glTexID);

	GLenum wrapMode = (clampmode >= CLAMP_X) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapMode);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapMode);

	GLenum filterMode = forcenofilter ? GL_NEAREST : GL_LINEAR;
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, needmipmap ? GL_LINEAR_MIPMAP_NEAREST : filterMode);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filterMode);

	glActiveTexture(GL_TEXTURE0); // Return state context pointer back to safety
	return true;
}

bool FHardwareTexture::BindOrCreateGL3x(FTexture *tex, int texunit, int clampmode, int translation, int flags)
{
	bool needmipmap = (clampmode <= CLAMP_XY) && !forcenofilter;

	// Bind it to the system.
	if (!Bind(texunit, needmipmap))
	{
		if (flags & CTF_Indexed)
		{
			glTextureBytes = 1;
			forcenofilter = true;
			needmipmap = false;
		}
		int w = 0, h = 0;

		// Create this texture

		FTextureBuffer texbuffer;

		if (!tex->isHardwareCanvas())
		{
			texbuffer = tex->CreateTexBuffer(translation, flags | CTF_ProcessData);
			w = texbuffer.mWidth;
			h = texbuffer.mHeight;
		}
		else
		{
			w = tex->GetWidth();
			h = tex->GetHeight();
		}
		if (!CreateTexture(texbuffer.mBuffer, w, h, texunit, needmipmap, "FHardwareTexture.BindOrCreate"))
		{
			// could not create texture
			return false;
		}
	}
	if (forcenofilter && clampmode <= CLAMP_XY) clampmode += CLAMP_NOFILTER - CLAMP_NONE;
	GLRenderer->mSamplerManager->Bind(texunit, clampmode, 255);
	return true;
}

}
