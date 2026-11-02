/*
** gl_framebuffer.cpp
** Implementation of the non-hardware specific parts of the
** OpenGL frame buffer
**
**---------------------------------------------------------------------------
** Copyright 2010-2020 Christoph Oelckers
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
#include "v_video.h"
#include "m_png.h"

#include "i_time.h"

#include "gl_interface.h"
#include "gl_framebuffer.h"
#include "gl_renderer.h"
#include "gl_renderbuffers.h"
#include "gl_samplers.h"
#include "hw_clock.h"
#include "hw_vrmodes.h"
#include "hw_skydome.h"
#include "hw_viewpointbuffer.h"
#include "hw_lightbuffer.h"
#include "hw_bonebuffer.h"
#include "gl_shaderprogram.h"
#include "gl_debug.h"
#include "r_videoscale.h"
#include "gl_buffers.h"
#include "gl_postprocessstate.h"
#include "v_draw.h"
#include "printf.h"
#include "gl_hwtexture.h"

#include "flatvertices.h"
#include "hw_cvars.h"

#include "d_player.h"      // players profiles
#include "doomstat.h"      // indices arrays matrix
#include "g_levellocals.h" // for "&level"
#include "hwrenderer/scene/hw_drawinfo.h"

EXTERN_CVAR (Bool, vid_vsync)
EXTERN_CVAR(Int, gl_tonemap)
EXTERN_CVAR(Bool, cl_capfps)
EXTERN_CVAR(Int, gl_pipeline_depth);

void gl_LoadExtensions();
void gl_PrintStartupLog();

extern bool vid_hdr_active;

namespace OpenGLRenderer
{
	FGLRenderer *GLRenderer;

//==========================================================================
//
//
//
//==========================================================================

OpenGLFrameBuffer::OpenGLFrameBuffer(void *hMonitor, bool fullscreen) : 
	Super(hMonitor, fullscreen) 
{
	// SetVSync needs to be at the very top to workaround a bug in Nvidia's OpenGL driver.
	// If wglSwapIntervalEXT is called after glBindFramebuffer in a frame the setting is not changed!
	Super::SetVSync(vid_vsync);
	FHardwareTexture::InitGlobalState();

	// Make sure all global variables tracking OpenGL context state are reset..
	gl_RenderState.Reset();

	GLRenderer = nullptr;
}

OpenGLFrameBuffer::~OpenGLFrameBuffer()
{
	PPResource::ResetAll();

	if (mVertexData != nullptr) delete mVertexData;
	if (mSkyData != nullptr) delete mSkyData;
	if (mViewpoints != nullptr) delete mViewpoints;
	if (mLights != nullptr) delete mLights;
	if (mBones != nullptr) delete mBones;
	mShadowMap.Reset();

	if (GLRenderer)
	{
		delete GLRenderer;
		GLRenderer = nullptr;
	}
}

//==========================================================================
//
// Initializes the GL renderer
//
//==========================================================================

void OpenGLFrameBuffer::InitializeState()
{
	static bool first = true;

	if (first)
	{
		if (ogl_LoadFunctions() == ogl_LOAD_FAILED)
		{
			I_FatalError("Failed to load OpenGL functions.");
		}
	}

	gl_LoadExtensions();

	mPipelineNbr = clamp(*gl_pipeline_depth, 1, HW_MAX_PIPELINE_BUFFERS); // здесь
	mPipelineType = gl_pipeline_depth > 0;

	// ---=== [Darkcrafter07] STAGE 1: SYSTEM CAPABILITIES SHIELD ===---
	// Safely moves state flags to the framebuffer object.
	// If running under gl.legacyMode, we zero out dynamic blocks offsets alignments
	// to prevent static_cast<GLDataBuffer*> BindBase() routines from locking the driver!
	if (!gl.legacyMode)
	{
		hwcaps = gl.flags;
		glslversion = gl.glslversion;
		uniformblockalignment = gl.uniformblockalignment;
		maxuniformblock = gl.maxuniformblock;
		vendorstring = gl.vendorstring;
	}
	else
	{
		hwcaps = 0;
		glslversion = 0;
		uniformblockalignment = 0;
		maxuniformblock = 0;
		vendorstring = nullptr;
	}

	if (first)
	{
		first = false;
		gl_PrintStartupLog();
	}

	// ---=== STAGE 2: FIXED HARDWARE RASTERIZATION STATES REGISTER ===---
	glDepthFunc(GL_LESS);
	glEnable(GL_DITHER);
	glDisable(GL_CULL_FACE);
	glDisable(GL_POLYGON_OFFSET_FILL);
	glEnable(GL_POLYGON_OFFSET_LINE);
	glEnable(GL_BLEND);
	glEnable(GL_DEPTH_CLAMP);
	glDisable(GL_DEPTH_TEST);

	if (gl.legacyMode)
	{
		glEnable(GL_TEXTURE_2D);
	}

	glDisable(GL_LINE_SMOOTH);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClearDepth(1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	// Re-calculates and maps factory screen boundaries matrices into 4.14 structures
	SetViewportRects(nullptr);

	// ---=== STAGE 3: ALLOCATING STORAGE AND CONTEXTS CONTAINERS ===---
	mVertexData = new FFlatVertexBuffer(GetWidth(), GetHeight()); // здесь
	mSkyData = new FSkyVertexBuffer;
	mViewpoints = new HWViewpointBuffer(screen->mPipelineNbr);
	mLights = new FLightBuffer(screen->mPipelineNbr);
	mBones = new BoneBuffer(screen->mPipelineNbr);

	GLRenderer = new FGLRenderer(this);
	GLRenderer->Initialize(GetWidth(), GetHeight());

	// ---=== [Darkcrafter07] STAGE 4: SHADERLESS PIPELINE ALIGNMENT SYNCHRONIZER ===---
	if (!gl.legacyMode)
	{
		// Shaders-bound UBO descriptors are only bound if Core Profile is active
		static_cast<GLDataBuffer*>(mLights->GetBuffer())->BindBase();
		static_cast<GLDataBuffer*>(mBones->GetBuffer())->BindBase();
	}
	else
	{
		// [Darkcrafter07] - EMBEDDED 3.88b Begin2D/SetOutputViewport RECONSTRUCTION!
		// Force calibrate and allocate pipeline texture attachments under legacy modes
		// strictly AFTER the GLRenderer pointer instance becomes valid in RAM!
		if (GLRenderer != nullptr && GLRenderer->mBuffers != nullptr)
		{
			GLRenderer->mBuffers->Setup(GetWidth(), GetHeight(), GetWidth(), GetHeight());
		}

		// Force direct hardware framebuffers context redirection to pipeline unit 0
		typedef void (APIENTRY* PFNGLBINDFRAMEBUFFERPROC) (unsigned int target, unsigned int framebuffer);
		static PFNGLBINDFRAMEBUFFERPROC p_glBindFramebuffer = (PFNGLBINDFRAMEBUFFERPROC)wglGetProcAddress("glBindFramebuffer");
		if (p_glBindFramebuffer != nullptr)
		{
			p_glBindFramebuffer(0x8D40, 0); // Hard-reset back to screen backbuffer context
		}

		// Configure standard 2D fixed-function initial projection viewport bounds
		glViewport(0, 0, GetWidth(), GetHeight());
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		glOrtho(0, GetWidth(), GetHeight(), 0, -1.0f, 1.0f);
		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();
	}

	mDebug = std::make_unique<FGLDebug>();
	mDebug->Update();
}

//==========================================================================
//
// Updates the screen
//
//==========================================================================

void OpenGLFrameBuffer::Update()
{
	twoD.Reset();
	Flush3D.Reset();

	Flush3D.Clock();
	GLRenderer->Flush();
	Flush3D.Unclock();

	Swap();
	Super::Update();
}

void OpenGLFrameBuffer::CopyScreenToBuffer(int width, int height, uint8_t* scr)
{
	IntRect bounds;
	bounds.left = 0;
	bounds.top = 0;
	bounds.width = width;
	bounds.height = height;
	GLRenderer->CopyToBackbuffer(&bounds, false);

	// strictly speaking not needed as the glReadPixels should block until the scene is rendered, but this is to safeguard against shitty drivers
	glFinish();
	glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, scr);
}

//===========================================================================
//
// Camera texture rendering
//
//===========================================================================

void OpenGLFrameBuffer::RenderTextureView(FCanvasTexture* tex, std::function<void(IntRect&)> renderFunc)
{
	if (tex == nullptr) return;

	if (gl.legacyMode)
	{
		glUseProgram(0);

		if ((uintptr_t)&level > 0x10000)
		{
			// Leverage GZ4x static factory pipeline manager
			// straight from hw_entrypoint.cpp! Pushes level context and active r_viewpoint variables cleanly.
			auto di = HWDrawInfo::StartDrawInfo(&level, nullptr, r_viewpoint, nullptr);

			if (di != nullptr)
			{
				// Extract target camera actor directly from the verified players statics array
				AActor* gl1xCameraActor = players[consoleplayer].camera;
				if (gl1xCameraActor == nullptr)
				{
					gl1xCameraActor = players[consoleplayer].mo;
				}

				// Pull active field of view degrees from our verified r_viewpoint overlay tracker
				float currentFovDegrees = (float)r_viewpoint.FieldOfView.Degrees();

				// Route to glCopyTexSubImage2D hardware capture pass
				di->RenderTextureViewGL1x(tex, gl1xCameraActor, currentFovDegrees);

				// Clean up the allocated stack frame descriptors page natively via the engine destructor
				di->EndDrawInfo();
			}
		}

		this->camtexcount++; // Sync the frame textures counter register for Swap() optimization
		return; // Legacy fixed-function capture complete, abort core shader paths.
	}

	if (GLRenderer != nullptr)
	{
		GLRenderer->StartOffscreen();
		GLRenderer->BindToFrameBuffer(tex);

		IntRect bounds;
		bounds.left = bounds.top = 0;
		bounds.width = FHardwareTexture::GetTexDimension(tex->GetWidth());
		bounds.height = FHardwareTexture::GetTexDimension(tex->GetHeight());

		renderFunc(bounds);
		GLRenderer->EndOffscreen();

		tex->SetUpdated(true);
		static_cast<OpenGLFrameBuffer*>(screen)->camtexcount++;
	}
}

//===========================================================================
//
// 
//
//===========================================================================

const char* OpenGLFrameBuffer::DeviceName() const 
{
	return gl.modelstring;
}

//==========================================================================
//
// Swap the buffers
//
//==========================================================================

CVAR(Bool, gl_finishbeforeswap, false, CVAR_ARCHIVE|CVAR_GLOBALCONFIG);

//==========================================================================
//
// Finishes the active graphics queue rendering operations and swaps frame buffers.
// Completely bypasses modern multi-buffered pipeline wait syncs inside gl.legacyMode.
// Declared as a native member of class OpenGLFrameBuffer (gl_framebuffer.cpp).
//
//==========================================================================
void OpenGLFrameBuffer::Swap()
{
	bool swapbefore = gl_finishbeforeswap && camtexcount == 0;
	Finish.Reset();
	Finish.Clock();

	// ---=== [Darkcrafter07] PATHWAY A: LEGACY FIXED-PIPELINE SWAP OVERRIDE ===---
	if (gl.legacyMode)
	{
		// 1. Force hardware pipeline queue completion to ensure pure OpenGL 1.1 layout alignment
		glFlush();
		glFinish();

		// 2. Bound framework frame rates frame limiters ticks
		FPSLimit();

		// 3. Execute pure platform-specific double buffer flip straight down to gl_sysfb.cpp!
		SwapBuffers();

		// Final hardware wipe queue lock
		glFinish();

		// 4. CLEANUP RETRO REGISTERS AFTER PAGE FLIP
		Finish.Unclock();
		camtexcount = 0;
		FHardwareTexture::UnbindAll();
		gl_RenderState.ClearLastMaterial();
		mDebug->Update();
		return; // Page swapped natively on WinXP, completely skip pipeline sync mutations!
	}

	// ---=== PATHWAY B: ORIGINAL MODERN SHADER PATH (GL3x+) ===---
	if (gl_pipeline_depth < 1)
	{
		if (swapbefore) glFinish();
		FPSLimit();
		SwapBuffers();
		if (!swapbefore) glFinish();
	}
	else
	{
		mVertexData->DropSync();

		FPSLimit();
		SwapBuffers();

		mVertexData->NextPipelineBuffer();
		mVertexData->WaitSync();
	}

	Finish.Unclock();
	camtexcount = 0;
	FHardwareTexture::UnbindAll();
	gl_RenderState.ClearLastMaterial();
	mDebug->Update();
}

//==========================================================================
//
// Enable/disable vertical sync
//
//==========================================================================

void OpenGLFrameBuffer::SetVSync(bool vsync)
{
	// Switch to the default frame buffer because some drivers associate the vsync state with the bound FB object.
	GLint oldDrawFramebufferBinding = 0, oldReadFramebufferBinding = 0;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &oldDrawFramebufferBinding);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldReadFramebufferBinding);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

	Super::SetVSync(vsync);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, oldDrawFramebufferBinding);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, oldReadFramebufferBinding);
}

//===========================================================================
//
//
//===========================================================================

void OpenGLFrameBuffer::SetTextureFilterMode()
{
	if (GLRenderer != nullptr && GLRenderer->mSamplerManager != nullptr) GLRenderer->mSamplerManager->SetTextureFilterMode();
}

IHardwareTexture *OpenGLFrameBuffer::CreateHardwareTexture(int numchannels) 
{ 
	return new FHardwareTexture(numchannels);
}

void OpenGLFrameBuffer::PrecacheMaterial(FMaterial *mat, int translation)
{
	if (mat->Source()->GetUseType() == ETextureType::SWCanvas) return;

	int numLayers = mat->NumLayers();
	MaterialLayerInfo* layer;
	auto base = static_cast<FHardwareTexture*>(mat->GetLayer(0, translation, &layer));

	if (base->BindOrCreate(layer->layerTexture, 0, CLAMP_NONE, translation, layer->scaleFlags))
	{
		for (int i = 1; i < numLayers; i++)
		{
			auto systex = static_cast<FHardwareTexture*>(mat->GetLayer(i, 0, &layer));
			systex->BindOrCreate(layer->layerTexture, i, CLAMP_NONE, 0, layer->scaleFlags);
		}
	}
	// unbind everything. 
	FHardwareTexture::UnbindAll();
	gl_RenderState.ClearLastMaterial();
}

IVertexBuffer *OpenGLFrameBuffer::CreateVertexBuffer()
{ 
	return new GLVertexBuffer; 
}

IIndexBuffer *OpenGLFrameBuffer::CreateIndexBuffer()
{ 
	return new GLIndexBuffer; 
}

IDataBuffer *OpenGLFrameBuffer::CreateDataBuffer(int bindingpoint, bool ssbo, bool needsresize)
{
	return new GLDataBuffer(bindingpoint, ssbo);
}

void OpenGLFrameBuffer::BlurScene(float amount)
{
	GLRenderer->BlurScene(amount);
}

void OpenGLFrameBuffer::InitLightmap(int LMTextureSize, int LMTextureCount, TArray<uint16_t>& LMTextureData)
{
	if (LMTextureData.Size() > 0)
	{
		GLint activeTex = 0;
		glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTex);
		glActiveTexture(GL_TEXTURE0 + 17);

		if (GLRenderer->mLightMapID == 0)
			glGenTextures(1, (GLuint*)&GLRenderer->mLightMapID);

		glBindTexture(GL_TEXTURE_2D_ARRAY, GLRenderer->mLightMapID);
		glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGB16F, LMTextureSize, LMTextureSize, LMTextureCount, 0, GL_RGB, GL_HALF_FLOAT, &LMTextureData[0]);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glGenerateMipmap(GL_TEXTURE_2D_ARRAY);

		glActiveTexture(activeTex);

		LMTextureData.Reset(); // We no longer need this, release the memory
	}
}

//void OpenGLFrameBuffer::SetViewportRects(IntRect *bounds)
//{
//	Super::SetViewportRects(bounds);
//	if (!bounds)
//	{
//		auto vrmode = VRMode::GetVRMode(true);
//		vrmode->AdjustViewport(this);
//	}
//}

void OpenGLFrameBuffer::SetViewportRects(IntRect* bounds)
{
	// 1. Invoke the default baseline layout matrix structures calculation
	Super::SetViewportRects(bounds);

	// ==============================================================================
	// [Darkcrafter07] - THE ULTIMATE LEGACY VIEWPORT ANCHOR LOCK!
	// ==============================================================================
	// CRITICAL FIX: If running under gl.legacyMode / gl.gl1path, custom viewport rescaling 
	// (gl_scale_viewport) distorts coordinates, causing fixed-function primitives 
	// to completely drift out of the frustum boundaries, resulting in a black screen!
	// We force-restore un-scaled standard proportions natively on the spot!
	if (gl.legacyMode || gl.gl1path)
	{
		int screenWidth = GetWidth();
		int screenHeight = GetHeight();

		// Enforce clean 1:1 pixel boundaries matching our exact fixed glOrtho requirements
		mScreenViewport.left = 0;
		mScreenViewport.top = 0;
		mScreenViewport.width = screenWidth;
		mScreenViewport.height = screenHeight;

		// The 3D scene viewport under legacy pipelines aligns straight with screen pixels bounds
		mSceneViewport = mScreenViewport;

		mOutputLetterbox = mScreenViewport;
		mGameScreenWidth = screenWidth;
		mGameScreenHeight = screenHeight;
		return; // Legacy protection applied successfully, bypass any fuzzy VR/Scale adjustments!
	}

	// ---=== ORIGINAL MODERN PROFILE DISPATCHER PATH (GL3x+ / Vulkan) ===---
	if (!bounds)
	{
		auto vrmode = VRMode::GetVRMode(true);
		if (vrmode != nullptr)
		{
			vrmode->AdjustViewport(this);
		}
	}
}

void OpenGLFrameBuffer::UpdatePalette()
{
	if (GLRenderer)
		GLRenderer->ClearTonemapPalette();
}

FRenderState* OpenGLFrameBuffer::RenderState()
{
	return &gl_RenderState;
}

void OpenGLFrameBuffer::AmbientOccludeScene(float m5)
{
	gl_RenderState.EnableDrawBuffers(1);
	GLRenderer->AmbientOccludeScene(m5);
	glViewport(screen->mSceneViewport.left, mSceneViewport.top, mSceneViewport.width, mSceneViewport.height);
	GLRenderer->mBuffers->BindSceneFB(true);
	gl_RenderState.EnableDrawBuffers(gl_RenderState.GetPassDrawBufferCount());
	gl_RenderState.Apply();
}

void OpenGLFrameBuffer::FirstEye()
{
	GLRenderer->mBuffers->CurrentEye() = 0;  // always begin at zero, in case eye count changed
}

void OpenGLFrameBuffer::NextEye(int eyecount)
{
	GLRenderer->mBuffers->NextEye(eyecount);
}

// If used wrong, draws only menues, player sprites and HUD but not the 3D world
void OpenGLFrameBuffer::SetSceneRenderTarget(bool useSSAO)
{
	GLRenderer->mBuffers->BindSceneFB(useSSAO);
}

void OpenGLFrameBuffer::UpdateShadowMap()
{
	if (mShadowMap.PerformUpdate())
	{
		FGLDebug::PushGroup("ShadowMap");

		FGLPostProcessState savedState;

		static_cast<GLDataBuffer*>(screen->mShadowMap.mLightList)->BindBase();
		static_cast<GLDataBuffer*>(screen->mShadowMap.mNodesBuffer)->BindBase();
		static_cast<GLDataBuffer*>(screen->mShadowMap.mLinesBuffer)->BindBase();

		GLRenderer->mBuffers->BindShadowMapFB();

		GLRenderer->mShadowMapShader->Bind();
		GLRenderer->mShadowMapShader->Uniforms->ShadowmapQuality = gl_shadowmap_quality;
		GLRenderer->mShadowMapShader->Uniforms->NodesCount = screen->mShadowMap.NodesCount();
		GLRenderer->mShadowMapShader->Uniforms.SetData();
		static_cast<GLDataBuffer*>(GLRenderer->mShadowMapShader->Uniforms.GetBuffer())->BindBase();

		glViewport(0, 0, gl_shadowmap_quality, 1024);
		GLRenderer->RenderScreenQuad();

		const auto& viewport = screen->mScreenViewport;
		glViewport(viewport.left, viewport.top, viewport.width, viewport.height);

		GLRenderer->mBuffers->BindShadowMapTexture(16);
		FGLDebug::PopGroup();
		screen->mShadowMap.FinishUpdate();
	}
}

void OpenGLFrameBuffer::WaitForCommands(bool finish)
{
	glFinish();
}

void OpenGLFrameBuffer::SetSaveBuffers(bool yes)
{
	if (!GLRenderer) return;
	if (yes) GLRenderer->mBuffers = GLRenderer->mSaveBuffers;
	else GLRenderer->mBuffers = GLRenderer->mScreenBuffers;
}

//===========================================================================
//
// CAUSES BLACK SCREEN IF USED INCORRECTLY
//
//===========================================================================

void OpenGLFrameBuffer::BeginFrame()
{
	SetViewportRects(nullptr);
	mViewpoints->Clear();
	if (GLRenderer != nullptr)
		GLRenderer->BeginFrame();
}

//===========================================================================
// 
//	Takes a screenshot
//
//===========================================================================

TArray<uint8_t> OpenGLFrameBuffer::GetScreenshotBuffer(int &pitch, ESSType &color_type, float &gamma)
{
	const auto &viewport = mOutputLetterbox;

	// Grab what is in the back buffer.
	// We cannot rely on SCREENWIDTH/HEIGHT here because the output may have been scaled.
	TArray<uint8_t> pixels;
	pixels.Resize(viewport.width * viewport.height * 3);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(viewport.left, viewport.top, viewport.width, viewport.height, GL_RGB, GL_UNSIGNED_BYTE, &pixels[0]);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);

	// Copy to screenshot buffer:
	int w = SCREENWIDTH;
	int h = SCREENHEIGHT;

	TArray<uint8_t> ScreenshotBuffer(w * h * 3, true);

	float rcpWidth = 1.0f / w;
	float rcpHeight = 1.0f / h;
	for (int y = 0; y < h; y++)
	{
		for (int x = 0; x < w; x++)
		{
			float u = (x + 0.5f) * rcpWidth;
			float v = (y + 0.5f) * rcpHeight;
			int sx = u * viewport.width;
			int sy = v * viewport.height;
			int sindex = (sx + sy * viewport.width) * 3;
			int dindex = (x + (h - y - 1) * w) * 3;
			ScreenshotBuffer[dindex] = pixels[sindex];
			ScreenshotBuffer[dindex + 1] = pixels[sindex + 1];
			ScreenshotBuffer[dindex + 2] = pixels[sindex + 2];
		}
	}

	pitch = w * 3;
	color_type = SS_RGB;

	// Screenshot should not use gamma correction if it was already applied to rendered image
	gamma = 1;
	if (vid_hdr_active && vid_fullscreen)
		gamma *= 2.2f;
	return ScreenshotBuffer;
}

//===========================================================================
// 
// 2D drawing
//
//===========================================================================

void OpenGLFrameBuffer::Draw2D()
{
	if (GLRenderer != nullptr)
	{
		GLRenderer->mBuffers->BindCurrentFB();
		::Draw2D(twod, gl_RenderState);
	}
}

void OpenGLFrameBuffer::PostProcessScene(bool swscene, int fixedcm, float flash, const std::function<void()> &afterBloomDrawEndScene2D)
{
	if (!swscene) GLRenderer->mBuffers->BlitSceneToTexture(); // Copy the resulting scene to the current post process texture
	GLRenderer->PostProcessScene(fixedcm, flash, afterBloomDrawEndScene2D);
}

bool OpenGLFrameBuffer::CompileNextShader()
{
	return GLRenderer->mShaderManager->CompileNextShader();
}

//==========================================================================
//
// OpenGLFrameBuffer :: WipeStartScreen
//
// Called before the current screen has started rendering. This needs to
// save what was drawn the previous frame so that it can be animated into
// what gets drawn this frame.
//
//==========================================================================

FTexture *OpenGLFrameBuffer::WipeStartScreen()
{
	const auto &viewport = screen->mScreenViewport;

	auto tex = new FWrapperTexture(viewport.width, viewport.height, 1);
	tex->GetSystemTexture()->CreateTexture(nullptr, viewport.width, viewport.height, 0, false, "WipeStartScreen");
	glFinish();
	static_cast<FHardwareTexture*>(tex->GetSystemTexture())->Bind(0, false);

	GLRenderer->mBuffers->BindCurrentFB();
	glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, viewport.left, viewport.top, viewport.width, viewport.height);
	return tex;
}

//==========================================================================
//
// OpenGLFrameBuffer :: WipeEndScreen
//
// The screen we want to animate to has just been drawn.
//
//==========================================================================

FTexture *OpenGLFrameBuffer::WipeEndScreen()
{
	GLRenderer->Flush();
	const auto &viewport = screen->mScreenViewport;
	auto tex = new FWrapperTexture(viewport.width, viewport.height, 1);
	tex->GetSystemTexture()->CreateTexture(NULL, viewport.width, viewport.height, 0, false, "WipeEndScreen");
	glFinish();
	static_cast<FHardwareTexture*>(tex->GetSystemTexture())->Bind(0, false);
	GLRenderer->mBuffers->BindCurrentFB();
	glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, viewport.left, viewport.top, viewport.width, viewport.height);
	return tex;
}

}
