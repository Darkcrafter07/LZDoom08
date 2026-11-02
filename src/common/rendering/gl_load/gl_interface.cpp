/*
** gl_interface.cpp
** OpenGL system interface
**
**---------------------------------------------------------------------------
** Copyright 2005-2019 Christoph Oelckers
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
*/

#include "gl_system.h"
#include "engineerrors.h"
#include "tarray.h"
#include "basics.h"
#include "m_argv.h"
#include "version.h"
#include "v_video.h"
#include "printf.h"
#include "gl_interface.h"
#include "c_cvars.h"
#include "r_data/r_translate.h"
#include "common/rendering/gl_load/gl_interface.h"
#include "common/rendering/hwrenderer/data/hw_cvars.h" // for "gl_usegl1mode"

static TArray<FString>  m_Extensions;
RenderContext gl;
static double realglversion;
static bool bindless;

CVAR(Bool, gl_riskymodernpath, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

EXTERN_CVAR (Int, vid_preferbackend)

EXTERN_CVAR(Bool, gl_legacy_mode) // declared in gl_20.cpp

//==========================================================================
//
// 
//
//==========================================================================

static void CollectExtensions()
{
	const char *extension;

	int max = 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &max);

	if (max == 0)
	{
		// Try old method to collect extensions
		const char *supported = (char *)glGetString(GL_EXTENSIONS);

		if (nullptr != supported)
		{
			char *extensions = new char[strlen(supported) + 1];
			strcpy(extensions, supported);

			char *extension = strtok(extensions, " ");

			while (extension)
			{
				m_Extensions.Push(FString(extension));
				extension = strtok(nullptr, " ");
			}

			delete[] extensions;
		}
	}
	else
	{
		// Use modern method to collect extensions
		for (int i = 0; i < max; i++)
		{
			extension = (const char*)glGetStringi(GL_EXTENSIONS, i);
			m_Extensions.Push(FString(extension));
		}
	}
}

//==========================================================================
//
// 
//
//==========================================================================

static bool CheckExtension(const char *ext)
{
	for (unsigned int i = 0; i < m_Extensions.Size(); ++i)
	{
		if (m_Extensions[i].CompareNoCase(ext) == 0) return true;
	}

	return false;
}



//==========================================================================
//
// 
//
//==========================================================================

static void InitContext()
{
	gl.flags=0;
}

//==========================================================================
//
// 
//
//==========================================================================

#define FUDGE_FUNC(name, ext) 	if (_ptrc_##name == NULL) _ptrc_##name = _ptrc_##name##ext;


void gl_LoadExtensions()
{
	int v = 0;

	InitContext();
	CollectExtensions();

	// CRITICAL MASTER FIX: Query baseline string properties IMMEDIATELY 
	// after context allocation to guarantee that string pointers are never NULL!
	const char* glversion = (const char*)glGetString(GL_VERSION);
	gl.vendorstring = (char*)glGetString(GL_VENDOR);
	gl.modelstring = (char*)glGetString(GL_RENDERER);
	gl.es = false;

	const char* version = Args->CheckValue("-glversion");
	realglversion = strtod(glversion, NULL);

	bool forceGL11Param = Args->CheckParm("-forcegl1dot1") != 0;

	if (version == NULL)
	{
		if (gl_usegl1mode)
		{
			version = "1";
			Printf("OpenGL1 compat mode enabled via lzdoom.ini/launcher.\n");
		}
		else
		{
			version = glversion;
		}
	}
	else
	{
		double v1 = strtod(version, NULL);
		if (v1 >= 3.0 && v1 < 3.3 && !gl_usegl1mode && !forceGL11Param)
		{
			v1 = 3.3;	// promote '3' to 3.3.
			version = "3.3";
		}
		if (realglversion < v1) version = glversion;
		else Printf("Emulating OpenGL v %s\n", version);
	}

	float gl_version = (float)strtod(version, NULL) + 0.01f;

	// ==============================================================================
	// [Darkcrafter07] - THE MULTI-TIERED LEGACY HARDWARE PROFILE LADDER
	// ==============================================================================
	if (gl_version < 3.3f || gl_usegl1mode || forceGL11Param)
	{
		// Stage 1: Enforce the core legacy flags hierarchy
		gl.legacyMode = true;
		gl.gl1path = true;

		gl.lightmethod = LM_LEGACY;
		gl.buffermethod = BM_LEGACY; // Force enforce client arrays / immediate pipelines
		gl.glslversion = 0.0f;       // Shaders stay completely disabled
		gl.flags |= RFL_NO_CLIP_PLANES; // Enforce CPU-side mathematical geometry clipping

		if (forceGL11Param || gl_usegl1mode)
		{
			gl_version = 1.1f;
		}

		// Stage 2: Flexible capability check between pure GL 1.1 and GL 1.3+ extensions
		const char* extensions = (const char*)glGetString(GL_EXTENSIONS);
		bool hasCombineSupport = false;

		if (extensions != nullptr)
		{
			hasCombineSupport = (gl_version >= 1.3f) || (strstr(extensions, "GL_ARB_texture_env_combine") != nullptr);
		}

		if (!hasCombineSupport || forceGL11Param || gl_usegl1mode)
		{
			gl.gl1_v1dot1 = true; // Fallback to basic 1.1 blending passes
			Printf("Falling back to OpenGL 1.1 mode (NO GL1.3 support)\n");
		}
		else
		{
			gl.gl1_v1dot1 = false; // Hardware natively processes 1.3+ multi-texturing
			Printf("OpenGL 1.3+ texture combination extensions detected\n");
		}

		// Stage 3: Hard-lock uniform metrics to safe zeros to stop glBufferData crashes!
		gl.maxuniforms = 0;
		gl.maxuniformblock = 0;
		gl.uniformblockalignment = 0;
		gl.glslversion = 0.0f;

		Printf("GL1 npot - to reduce blurriness, go vid-options -> txt hires upscale\n");
	}


	// ---=== ORIGINAL FACTORY MODERN PROFILE (GL 3.3+) ===---
	else
	{
		gl.gl1path = false;
		gl.gl1_v1dot1 = false;
		gl.legacyMode = false;
		gl.lightmethod = LM_DEFERRED;
		gl.buffermethod = BM_DEFERRED;

		gl.glslversion = strtod((char*)glGetString(GL_SHADING_LANGUAGE_VERSION), NULL) + 0.01f;

		if (gl_version < 4.f)
		{
#ifdef _WIN32
			// Protected query safely checks verified non-null vendorstring variables
			if (gl.vendorstring != nullptr && strstr(gl.vendorstring, "ATI Tech") != nullptr)
			{
				gl.flags |= RFL_NO_CLIP_PLANES;
			}
#endif
			gl.glslversion = 3.31f;
		}
		else if (gl_version >= 4.5f)
		{
			gl.flags |= RFL_SHADER_STORAGE_BUFFER | RFL_BUFFER_STORAGE;
			glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &v);
			if (v == 0) gl.flags &= ~RFL_SHADER_STORAGE_BUFFER;
		}

		if (gl_version >= 4.3f || CheckExtension("GL_ARB_invalidate_subdata")) gl.flags |= RFL_INVALIDATE_BUFFER;
		if (gl_version >= 4.3f || CheckExtension("GL_KHR_debug")) gl.flags |= RFL_DEBUG;

		glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_COMPONENTS, &v);
		gl.maxuniforms = v;
		glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &v);
		gl.maxuniformblock = min(65536, v);
		glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &v);
		gl.uniformblockalignment = v;
	}

	if (CheckExtension("GL_ARB_texture_compression")) gl.flags |= RFL_TEXTURE_COMPRESSION;
	if (CheckExtension("GL_EXT_texture_compression_s3tc")) gl.flags |= RFL_TEXTURE_COMPRESSION_S3TC;

	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &gl.max_texturesize);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

	bindless = CheckExtension("GL_ARB_bindless_texture");

	// ==============================================================================
	// STAGE 4: THE MONOLITHIC ARB/EXT FBO FUDGE REDUNDANCY ENGINE
	// ==============================================================================
	if (gl.legacyMode)
	{
		FUDGE_FUNC(glGenerateMipmap, EXT);
		FUDGE_FUNC(glGenFramebuffers, EXT);
		FUDGE_FUNC(glBindFramebuffer, EXT);
		FUDGE_FUNC(glDeleteFramebuffers, EXT);
		FUDGE_FUNC(glFramebufferTexture2D, EXT);
		FUDGE_FUNC(glFramebufferRenderbuffer, EXT);
		FUDGE_FUNC(glGenRenderbuffers, EXT);
		FUDGE_FUNC(glDeleteRenderbuffers, EXT);
		FUDGE_FUNC(glRenderbufferStorage, EXT);
		FUDGE_FUNC(glBindRenderbuffer, EXT);
		FUDGE_FUNC(glCheckFramebufferStatus, EXT);
	}

	// Sync local cvars registry links
	FBaseCVar* legacyCVar = cvarMap.CheckKey(FName("gl_legacy_mode")) ? cvarMap[FName("gl_legacy_mode")] : nullptr;
	if (legacyCVar != nullptr)
	{
		UCVarValue value;
		value.Bool = gl.legacyMode;
		legacyCVar->ForceSet(value, CVAR_Bool);
	}
	else
	{
		gl_legacy_mode = !!gl.legacyMode;
	}
}

//==========================================================================
//
// 
//
//==========================================================================

void gl_PrintStartupLog()
{
	int v = 0;
	// [Darkcrafter07] - Protection for raw GL1 context where GL_CONTEXT_PROFILE_MASK is undefined
	if (!gl.legacyMode)
	{
		glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &v);
	}

	Printf("GL_VENDOR: %s\n", glGetString(GL_VENDOR));
	Printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));
	Printf("GL_VERSION: %s (%s profile)\n", glGetString(GL_VERSION), (!gl.legacyMode && (v & GL_CONTEXT_CORE_PROFILE_BIT)) ? "Core" : "Compatibility");

	// Safe query for GLSL version to prevent crash on pure GL1 cards
	const char* glslVersion = (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
	Printf("GL_SHADING_LANGUAGE_VERSION: %s\n", glslVersion ? glslVersion : "None (Fixed-Function)");

	Printf(PRINT_LOG, "GL_EXTENSIONS:");
	for (unsigned i = 0; i < m_Extensions.Size(); i++)
	{
		Printf(PRINT_LOG, " %s", m_Extensions[i].GetChars());
	}

	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &v);
	Printf("\nMax. texture size: %d\n", v);
	glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &v);
	Printf("Max. texture units: %d\n", v);

	// GL_MAX_VARYING_FLOATS doesn't exist in pure GL1.1/1.3 drivers
	if (!gl.legacyMode)
	{
		glGetIntegerv(GL_MAX_VARYING_FLOATS, &v);
		Printf("Max. varying: %d\n", v);
	}
	else
	{
		Printf("Max. varying: 0 (No shaders)\n");
	}

	if (gl.flags & RFL_SHADER_STORAGE_BUFFER)
	{
		glGetIntegerv(GL_MAX_COMBINED_SHADER_STORAGE_BLOCKS, &v);
		Printf("Max. combined shader storage blocks: %d\n", v);
		glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &v);
		Printf("Max. vertex shader storage blocks: %d\n", v);
	}
	else if (!gl.legacyMode) // Uniform blocks require modern contexts
	{
		glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &v);
		Printf("Max. uniform block size: %d\n", v);
		glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &v);
		Printf("Uniform block alignment: %d\n", v);
	}

	// [Darkcrafter07] - Adapted legacy alpha-texture remapping for the new GPalette system
	// For shader-less, the special alphatexture translation must be changed to actually set the alpha, because it won't get translated by a shader.
	if (gl.legacyMode)
	{
		// Safely retrieve the translation from the new manager using the v4.14 API
		FRemapTable* remap = GPalette.GetTranslation(TRANSLATION_Standard, 8); // 8 is STD_Grayscale / alphatexture slot
		if (remap != nullptr)
		{
			// Create a temporary copy to modify and push back into GPalette safely
			FRemapTable modifiedRemap = *remap;
			for (int i = 0; i < 256; i++)
			{
				modifiedRemap.Remap[i] = i;
				modifiedRemap.Palette[i] = PalEntry(i, 255, 255, 255);
			}
			// Hard update the transaction table inside the global palette cache
			GPalette.UpdateTranslation(TRANSLATION(TRANSLATION_Standard, 8), &modifiedRemap);
		}
	}
}

void setGlVersion(double glv)
{
	realglversion = glv;
}

std::pair<double, bool> gl_getInfo()
{
	// gl_ARB_bindless_texture is the closest we can get to determine Vulkan support from OpenGL.
	// This isn't foolproof because Intel doesn't support it but for NVidia and AMD support of this extension means Vulkan support.
	return std::make_pair(realglversion, bindless);
}
