/*
** gl_renderer.cpp
** Renderer interface
**
**---------------------------------------------------------------------------
** Copyright 2005-2020 Christoph Oelckers
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
#include "files.h"
#include "v_video.h"
#include "m_png.h"
#include "filesystem.h"
#include "i_time.h"
#include "cmdlib.h"
#include "version.h"
#include "gl_interface.h"
#include "gl_framebuffer.h"
#include "hw_cvars.h"
#include "gl_debug.h"
#include "gl_renderer.h"
#include "gl_renderstate.h"
#include "gl_renderbuffers.h"
#include "gl_shaderprogram.h"
#include "flatvertices.h"
#include "gl_samplers.h"
#include "hw_lightbuffer.h"
#include "r_videoscale.h"
#include "model.h"
#include "gl_postprocessstate.h"
#include "gl_buffers.h"
#include "texturemanager.h"
#include "hw_skydome.h"

EXTERN_CVAR(Int, screenblocks)

namespace OpenGLRenderer
{

//===========================================================================
// 
// Renderer interface
//
//===========================================================================

//-----------------------------------------------------------------------------
//
// Initialize
//
//-----------------------------------------------------------------------------

FGLRenderer::FGLRenderer(OpenGLFrameBuffer* fb)
{
	framebuffer = fb;

	// GL1x/GL2x vars
	mVBO = nullptr;
	mSkyVBO = nullptr;
	mVAOID = 0;
}

void FGLRenderer::Initialize(int width, int height)
{
	mScreenBuffers = new FGLRenderBuffers();
	mSaveBuffers = new FGLRenderBuffers();
	mBuffers = mScreenBuffers;

	// Allocate frame shaders strictly for the modern core profile path
	if (!gl.legacyMode)
	{
		mPresentShader = new FPresentShader();
		mPresent3dCheckerShader = new FPresent3DCheckerShader();
		mPresent3dColumnShader = new FPresent3DColumnShader();
		mPresent3dRowShader = new FPresent3DRowShader();
		mShadowMapShader = new FShadowMapShader();

		glGenVertexArrays(1, &mVAOID);
		glBindVertexArray(mVAOID);
		FGLDebug::LabelObject(GL_VERTEX_ARRAY, mVAOID, "FGLRenderer.mVAOID");
	}
	else
	{
		// Fixed-Function path baseline initialization
		mPresentShader = nullptr;
		mPresent3dCheckerShader = nullptr;
		mPresent3dColumnShader = nullptr;
		mPresent3dRowShader = nullptr;
		mShadowMapShader = nullptr;
		mVAOID = 0;
	}

	mFBID = 0;
	mOldFBID = 0;

	// Reallocate dedicated fixed-function GL1x/GL2x polygonal storage meshes
	if (gl.legacyMode)
	{
		mVBO = new FFlatVertexBuffer(width, height);
		mSkyVBO = new FSkyVertexBuffer();

		// Force register our legacy array mapping buffer onto the active rendering thread context
		gl_RenderState.SetVertexBuffer(mVBO);
	}

	mShaderManager = new FShaderManager;
	mSamplerManager = new FSamplerManager;

	GetSpecialGL1xTextures();
}

//void FGLRenderer::Initialize(int width, int height)
//{
//	mScreenBuffers = new FGLRenderBuffers();
//	mSaveBuffers = new FGLRenderBuffers();
//	mBuffers = mScreenBuffers;
//
//	// [Darkcrafter07] - THE SHADER PRESENTATION EMULATION BREAKTHROUGH!
//	// We force allocate the primary screen quad layout shaders inside gl.legacyMode!
//	// This is ultra-lightweight and natively supported by any OpenGL 2.0 Windows XP driver.
//	mPresentShader = new FPresentShader();
//
//	if (!gl.legacyMode)
//	{
//		mPresent3dCheckerShader = new FPresent3DCheckerShader();
//		mPresent3dColumnShader = new FPresent3DColumnShader();
//		mPresent3dRowShader = new FPresent3DRowShader();
//		mShadowMapShader = new FShadowMapShader();
//
//		glGenVertexArrays(1, &mVAOID);
//		glBindVertexArray(mVAOID);
//		FGLDebug::LabelObject(GL_VERTEX_ARRAY, mVAOID, "FGLRenderer.mVAOID");
//	}
//	else
//	{
//		// Fixed-Function path shadow/3D allocations stay disabled to block drifts
//		mPresent3dCheckerShader = nullptr;
//		mPresent3dColumnShader = nullptr;
//		mPresent3dRowShader = nullptr;
//		mShadowMapShader = nullptr;
//		mVAOID = 0;
//	}
//
//	mFBID = 0;
//	mOldFBID = 0;
//
//	// Reallocate dedicated fixed-function GL1x/GL2x polygonal storage meshes
//	if (gl.legacyMode)
//	{
//		mVBO = new FFlatVertexBuffer(width, height);
//		mSkyVBO = new FSkyVertexBuffer();
//
//		// Force register our legacy array mapping buffer onto the active rendering thread context
//		gl_RenderState.SetVertexBuffer(mVBO);
//	}
//
//	mShaderManager = new FShaderManager;
//	mSamplerManager = new FSamplerManager;
//
//	GetSpecialGL1xTextures();
//}

FGLRenderer::~FGLRenderer()
{
	FlushModels();
	TexMan.FlushAll();

	if (mShaderManager != nullptr) delete mShaderManager;
	if (mSamplerManager != nullptr) delete mSamplerManager;
	if (mFBID != 0) glDeleteFramebuffers(1, &mFBID);

	if (mVAOID != 0)
	{
		glBindVertexArray(0);
		glDeleteVertexArrays(1, &mVAOID);
	}

	// Clean legacy GL1x/GL2x fixed-function memory buckets safely
	if (mVBO != nullptr) delete mVBO;
	if (mSkyVBO != nullptr) delete mSkyVBO;

	if (mBuffers) delete mBuffers;
	if (mSaveBuffers) delete mSaveBuffers;
	if (mPresentShader) delete mPresentShader;
	if (mPresent3dCheckerShader) delete mPresent3dCheckerShader;
	if (mPresent3dColumnShader) delete mPresent3dColumnShader;
	if (mPresent3dRowShader) delete mPresent3dRowShader;
	if (mShadowMapShader) delete mShadowMapShader;
}

void FGLRenderer::GetSpecialGL1xTextures()
{
	if (gl.legacyMode)
	{
		// Also needs to be done on game restart but IDK how yet?
		gl1xLight = TexMan.CheckForTexture("glstuff/gl1xlight.png", ETextureType::MiscPatch);
		gl1xPart1 = TexMan.CheckForTexture("glstuff/gl1xpart1.png", ETextureType::MiscPatch);
		gl1xPart2 = TexMan.CheckForTexture("glstuff/gl1xpart2.png", ETextureType::MiscPatch);
		mirrorgl1xTexture = TexMan.CheckForTexture("glstuff/mirrorgl1x.png", ETextureType::MiscPatch);
	}
}

//===========================================================================
// 
//
//
//===========================================================================

bool FGLRenderer::StartOffscreen()
{
	bool firstBind = (mFBID == 0);
	if (mFBID == 0)
		glGenFramebuffers(1, &mFBID);
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &mOldFBID);
	glBindFramebuffer(GL_FRAMEBUFFER, mFBID);
	if (firstBind)
		FGLDebug::LabelObject(GL_FRAMEBUFFER, mFBID, "OffscreenFB");
	return true;
}

//===========================================================================
// 
//
//
//===========================================================================

void FGLRenderer::EndOffscreen()
{
	glBindFramebuffer(GL_FRAMEBUFFER, mOldFBID); 
}

//===========================================================================
//
//
//
//===========================================================================

void FGLRenderer::BindToFrameBuffer(FTexture *tex)
{
	auto BaseLayer = static_cast<FHardwareTexture*>(tex->GetHardwareTexture(0, 0));
	// must create the hardware texture first
	BaseLayer->BindOrCreate(tex, 0, 0, 0, 0);
	FHardwareTexture::Unbind(0);
	gl_RenderState.ClearLastMaterial();
	BaseLayer->BindToFrameBuffer(tex->GetWidth(), tex->GetHeight());
}

//===========================================================================
//
//
//
//===========================================================================

void FGLRenderer::BeginFrame()
{
	mScreenBuffers->Setup(screen->mScreenViewport.width, screen->mScreenViewport.height, screen->mSceneViewport.width, screen->mSceneViewport.height);
	mSaveBuffers->Setup(SAVEPICWIDTH, SAVEPICHEIGHT, SAVEPICWIDTH, SAVEPICHEIGHT);
}

}
