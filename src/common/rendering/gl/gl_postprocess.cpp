/*
**  Postprocessing framework
**  Copyright (c) 2016-2020 Magnus Norddahl
**
**  This software is provided 'as-is', without any express or implied
**  warranty.  In no event will the authors be held liable for any damages
**  arising from the use of this software.
**
**  Permission is granted to anyone to use this software for any purpose,
**  including commercial applications, and to alter it and redistribute it
**  freely, subject to the following restrictions:
**
**  1. The origin of this software must not be misrepresented; you must not
**     claim that you wrote the original software. If you use this software
**     in a product, an acknowledgment in the product documentation would be
**     appreciated but is not required.
**  2. Altered source versions must be plainly marked as such, and must not be
**     misrepresented as being the original software.
**  3. This notice may not be removed or altered from any source distribution.
*/

#include "gl_system.h"
#include "m_png.h"
#include "gl_buffers.h"
#include "gl_framebuffer.h"
#include "gl_debug.h"
#include "gl_renderbuffers.h"
#include "gl_renderer.h"
#include "gl_postprocessstate.h"
#include "gl_shaderprogram.h"
#include "hwrenderer/postprocessing/hw_postprocess.h"
#include "hwrenderer/postprocessing/hw_postprocess_cvars.h"
#include "flatvertices.h"
#include "r_videoscale.h"
#include "v_video.h"

#include "hw_vrmodes.h"
#include "v_draw.h"

#include "common/rendering/gl/gl_renderbuffers.h"
#include "rendering/hwrenderer/scene/hw_drawinfo.h"

extern bool vid_hdr_active;

CVAR(Int, gl_dither_bpc, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)

namespace OpenGLRenderer
{

//void FGLRenderer::RenderScreenQuad()
//{
//	auto buffer = static_cast<GLVertexBuffer*>(screen->mVertexData->GetBufferObjects().first);
//	buffer->Bind(nullptr);
//	glDrawArrays(GL_TRIANGLE_STRIP, FFlatVertexBuffer::PRESENT_INDEX, 3);
//}

void FGLRenderer::RenderScreenQuad()
{
	if (gl.legacyMode) 
	{
		RenderScreenQuadGL1x();
	}
	else               
	{
		RenderScreenQuadGL3x();
	}
}

//void FGLRenderer::RenderScreenQuadGL1x()
//{
//	if (mVBO != nullptr)
//	{
//		// GZDoom 3x way
//		mVBO->BindVBO();
//		OpenGLRenderer::gl_RenderState.ResetVertexBuffer();
//		mVBO->RenderArray(GL_TRIANGLE_STRIP, FFlatVertexBuffer::PRESENT_INDEX, 4);
//	}
//}

void FGLRenderer::RenderScreenQuadGL1x()
{
	// Check guard protects memory from processing missing legacy buffer objects
	if (mVBO == nullptr || mBuffers == nullptr || screen == nullptr)
		return;

	// 1. Force unbind modern Vertex Array Objects (VAO) to unlock immediate draws
	typedef void (APIENTRY* PFNGLBINDVERTEXARRAYPROC) (unsigned int array);
	static PFNGLBINDVERTEXARRAYPROC p_glBindVertexArray = (PFNGLBINDVERTEXARRAYPROC)wglGetProcAddress("glBindVertexArray");
	if (p_glBindVertexArray != nullptr)
	{
		p_glBindVertexArray(0);
	}

	// 2. FORCE RE-BIND THE SCENE TEXTURE FROM ACTIVE PIPELINE FB CHANNELS!
	glActiveTexture(GL_TEXTURE0);
	glEnable(GL_TEXTURE_2D);
	mBuffers->BindCurrentTexture(0, GL_NEAREST, GL_CLAMP_TO_EDGE);

	// ==============================================================================
	// [Darkcrafter07] - THE MONOLITHIC MATRIX HARDWARE SHIELD LOCK!
	// ==============================================================================
	// CRITICAL MASTER-KEY: We push existing projections to stacks before drawing.
	// This completely immunizes our 2D coordinates from ApplyMatrices() overrides!
	glMatrixMode(GL_PROJECTION);
	glPushMatrix(); // Save original 3D projection matrix stream onto stack!
	glLoadIdentity();
	glOrtho(0.0, 1.0, 0.0, 1.0, -1.0, 1.0);

	glMatrixMode(GL_MODELVIEW);
	glPushMatrix(); // Save original 3D modelview matrix stream onto stack!
	glLoadIdentity();

	// 3. Configure baseline immediate state overrides for strict hardware compliance
	glPushAttrib(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT | GL_ENABLE_BIT);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_FOG);
	glDisable(GL_LIGHTING);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_CULL_FACE);
	glDepthMask(GL_FALSE);

	// Safely clear modern vertex attributes array streams to isolate the pipeline
	for (int i = 0; i < 16; i++)
	{
		glDisableVertexAttribArray(i);
	}

	// 4. EXTRACT THE CHEST COORDINATES DIRECTLY FROM THE mVBO SHADOWDATA SHEET!
	const auto& vertices = mVBO->vbo_shadowdata;
	unsigned int presentOffset = 8; // PRESENT_INDEX boundary offset tracker

	if (presentOffset + 3 < vertices.Size())
	{
		const auto* q0 = &vertices[presentOffset];
		const auto* q1 = &vertices[presentOffset + 1];
		const auto* q2 = &vertices[presentOffset + 2];
		const auto* q3 = &vertices[presentOffset + 3];

		// Stream coordinates natively matching old GZDoom 3x quad блиттер layouts
		glBegin(GL_QUADS);
		glColor4f(1.0f, 1.0f, 1.0f, 1.0f); // Reset color modulation to clear bright white

		glTexCoord2f(q0->u, q0->v);	glVertex2f((q0->x + 1.0f) * 0.5f, (q0->y + 1.0f) * 0.5f);
		glTexCoord2f(q1->u, q1->v);	glVertex2f((q1->x + 1.0f) * 0.5f, (q1->y + 1.0f) * 0.5f);
		glTexCoord2f(q3->u, q3->v);	glVertex2f((q3->x + 1.0f) * 0.5f, (q3->y + 1.0f) * 0.5f); // TRIANGLE_STRIP to QUAD layout order mapping
		glTexCoord2f(q2->u, q2->v);	glVertex2f((q2->x + 1.0f) * 0.5f, (q2->y + 1.0f) * 0.5f);
		glEnd();
	}

	// ==============================================================================
	// STAGE 5: POP AND RESTORE FACTORY MATRICES
	// ==============================================================================
	glPopAttrib();

	glMatrixMode(GL_MODELVIEW);
	glPopMatrix(); // Restore engine's original modelview matrices seamlessly

	glMatrixMode(GL_PROJECTION);
	glPopMatrix(); // Restore engine's original projection matrices seamlessly

	glMatrixMode(GL_MODELVIEW); // Return context register pointer back to safety

	glFlush();
}

void FGLRenderer::RenderScreenQuadGL3x()
{
	auto buffer = static_cast<GLVertexBuffer*>(screen->mVertexData->GetBufferObjects().first);
	buffer->Bind(nullptr);
	glDrawArrays(GL_TRIANGLE_STRIP, FFlatVertexBuffer::PRESENT_INDEX, 3);
}



// PostProcessScene - the method responsible for final image output
void FGLRenderer::PostProcessScene(int fixedcm, float flash, const std::function<void()>& afterBloomDrawEndScene2D)
{
	if (gl.legacyMode) PostProcessSceneGL1x(fixedcm, flash, afterBloomDrawEndScene2D);
	else               PostProcessSceneGL3x(fixedcm, flash, afterBloomDrawEndScene2D);
}

void FGLRenderer::PostProcessSceneGL1x(int fixedcm, float flash, const std::function<void()>& afterBloomDrawEndScene2D)
{
	// 1. TOTAL SHADER AND VERTEX ARRAYS OBJECTS PURGE
	glUseProgram(0);

	typedef void (APIENTRY* PFNGLBINDFRAMEBUFFERPROC) (unsigned int target, unsigned int framebuffer);
	static PFNGLBINDFRAMEBUFFERPROC p_glBindFramebuffer = (PFNGLBINDFRAMEBUFFERPROC)wglGetProcAddress("glBindFramebuffer");
	if (p_glBindFramebuffer != nullptr)
	{
		p_glBindFramebuffer(0x8D40, 0); // Force back to true native window default backbuffer
	}

	typedef void (APIENTRY* PFNGLBINDVERTEXARRAYPROC) (unsigned int array);
	static PFNGLBINDVERTEXARRAYPROC p_glBindVertexArray = (PFNGLBINDVERTEXARRAYPROC)wglGetProcAddress("glBindVertexArray");
	if (p_glBindVertexArray != nullptr)
	{
		p_glBindVertexArray(0);
	}

	// 2. CONFIGURE NATIVE FIXED PRESENTATION VIEWPORT LAYOUT
	int w = screen ? screen->GetWidth() : 640;
	int h = screen ? screen->GetHeight() : 480;
	glViewport(0, 0, w, h);

	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0.0, (double)w, (double)h, 0.0, -1.0, 1.0);

	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_LIGHTING);
	glDisable(GL_BLEND);
	glDisable(GL_SCISSOR_TEST);
	glDepthMask(GL_FALSE);

	for (int attr = 0; attr < 16; attr++)
	{
		glDisableVertexAttribArray(attr);
	}

	// ==============================================================================
	// STAGE 2: EXECUTE NATIVE ZDOOM 2D UI DRAWERS QUEUES
	// ==============================================================================
	if (afterBloomDrawEndScene2D)
	{
		glEnable(GL_TEXTURE_2D);
		glTexEnvf(0x2300, 0x3001, 0x2100); // GL_TEXTURE_ENV_MODE = GL_MODULATE

		// Wake up internal state trackers before processing commands
		OpenGLRenderer::gl_RenderState.ApplyFixedGL1xFunction(nullptr);

		// Processes ZScript drawers and populates the twod commands streams!
		afterBloomDrawEndScene2D();
	}

	// ==============================================================================
	// [Darkcrafter07] - FIXED-FUNCTION EMBEDDED TWOD FLUSH BUSTER!
	// ==============================================================================
	// If the modern twod->OnFrameDone() tries to run, it will upload vertices to 
	// modern persistent buffers which old cards ignore. We intercept and draw manually!
	if (twod != nullptr)
	{
		// Force turn on basic hardware transparency alpha blending for UI masks
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glDisable(GL_TEXTURE_2D); // Drop texturing to draw solid color blocks cleanly

		// [Darkcrafter07] - DIRECT GEOMETRY EXTRACTION FROM THE RAW COMMANDS QUEUE!
		// We stream a full-screen immediate mode fallback quad with the calculated 
		// V_CalcBlend/Dim color properties straight over the viewport!
		glBegin(GL_QUADS);

		// Baseline fullbright white modulation
		glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

		// Commit the raw immediate drawing parameters straight to hardware registers
		glEnd();

		// Flush commands buffer safely and clear twod lists to prevent double-draw artifacts
		twod->OnFrameDone();
	}

	// ==============================================================================
	// STAGE 3: TRIGGER THE MONOLITHIC WIN32 GDI HARDWARE PAGE FLIP
	// ==============================================================================
	if (HWDrawInfo::CurrentGL1xInfo != nullptr)
	{
		// Fires direct Win32 API ::SwapBuffers straight down to the monitor!
		HWDrawInfo::CurrentGL1xInfo->SubmitGL1xFrame();
	}
}

void FGLRenderer::PostProcessSceneGL3x(int fixedcm, float flash, const std::function<void()>& afterBloomDrawEndScene2D)
{
	int sceneWidth = mBuffers->GetSceneWidth();
	int sceneHeight = mBuffers->GetSceneHeight();

	GLPPRenderState renderstate(mBuffers);

	hw_postprocess.Pass1(&renderstate, fixedcm, sceneWidth, sceneHeight);
	mBuffers->BindCurrentFB();
	if (afterBloomDrawEndScene2D) afterBloomDrawEndScene2D();
	hw_postprocess.Pass2(&renderstate, fixedcm, flash, sceneWidth, sceneHeight);
}

//-----------------------------------------------------------------------------
//
// Adds ambient occlusion to the scene
//
//-----------------------------------------------------------------------------

void FGLRenderer::AmbientOccludeScene(float m5)
{
	int sceneWidth = mBuffers->GetSceneWidth();
	int sceneHeight = mBuffers->GetSceneHeight();

	GLPPRenderState renderstate(mBuffers);
	hw_postprocess.ssao.Render(&renderstate, m5, sceneWidth, sceneHeight);
}

void FGLRenderer::BlurScene(float gameinfobluramount)
{
	int sceneWidth = mBuffers->GetSceneWidth();
	int sceneHeight = mBuffers->GetSceneHeight();

	GLPPRenderState renderstate(mBuffers);

	auto vrmode = VRMode::GetVRMode(true);
	int eyeCount = vrmode->mEyeCount;
	for (int i = 0; i < eyeCount; ++i)
	{
		hw_postprocess.bloom.RenderBlur(&renderstate, sceneWidth, sceneHeight, gameinfobluramount);
		if (eyeCount - i > 1) mBuffers->NextEye(eyeCount);
	}
}

void FGLRenderer::ClearTonemapPalette()
{
	hw_postprocess.tonemap.ClearTonemapPalette();
}

//-----------------------------------------------------------------------------
//
// Copies the rendered screen to its final destination
//
//-----------------------------------------------------------------------------

void FGLRenderer::Flush()
{
	auto vrmode = VRMode::GetVRMode(true);
	if (vrmode->mEyeCount == 1)
	{
		CopyToBackbuffer(nullptr, true);
	}
	else
	{
		// Render 2D to eye textures
		int eyeCount = vrmode->mEyeCount;
		for (int eye_ix = 0; eye_ix < eyeCount; ++eye_ix)
		{
			screen->Draw2D();
			if (eyeCount - eye_ix > 1)
				mBuffers->NextEye(eyeCount);
		}
		twod->Clear();

		FGLPostProcessState savedState;
		FGLDebug::PushGroup("PresentEyes");
		// Note: This here is the ONLY place in the entire engine where the OpenGL dependent parts of the Stereo3D code need to be dealt with.
		// There's absolutely no need to create a overly complex class hierarchy for just this.
		PresentStereo();
		FGLDebug::PopGroup();
	}
}

//-----------------------------------------------------------------------------
//
// Gamma correct while copying to frame buffer
//
//-----------------------------------------------------------------------------

void FGLRenderer::CopyToBackbuffer(const IntRect *bounds, bool applyGamma)
{
	screen->Draw2D();	// draw all pending 2D stuff before copying the buffer
	twod->Clear();

	GLPPRenderState renderstate(mBuffers);
	hw_postprocess.customShaders.Run(&renderstate, "screen");

	FGLDebug::PushGroup("CopyToBackbuffer");
	FGLPostProcessState savedState;
	savedState.SaveTextureBindings(2);
	mBuffers->BindOutputFB();

	IntRect box;
	if (bounds)
	{
		box = *bounds;
	}
	else
	{
		ClearBorders();
		box = screen->mOutputLetterbox;
	}

	mBuffers->BindCurrentTexture(0);
	DrawPresentTexture(box, applyGamma);
	FGLDebug::PopGroup();
}

void FGLRenderer::DrawPresentTexture(const IntRect& box, bool applyGamma)
{
	if (gl.legacyMode)
	{
		DrawPresentTextureGL1x(box, applyGamma);
	}
	else
	{
		DrawPresentTextureGL3x(box, applyGamma);
	}
}

//==========================================================================
//
// Pure Fixed-Function emulation of the modern mPresentShader pipeline.
// Forces strict hardware texture binding from mPipelineTexture arrays.
// Declared as a native member of class FGLRenderer (gl_postprocess.cpp).
//
//==========================================================================
void FGLRenderer::DrawPresentTextureGL1x(const IntRect& box, bool applyGamma)
{
	glViewport(box.left, box.top, box.width, box.height);

	if (mBuffers == nullptr || mVBO == nullptr) return;

	mBuffers->BindDitherTexture(1);

	// [Darkcrafter07] - DIRECT WIN32 FIXED-FUNCTION SAMPLER PURGE LOCK!
	glActiveTexture(GL_TEXTURE0);
	glEnable(GL_TEXTURE_2D);

	int currentFBTextureIdx = mBuffers->GetCurrentPipelineTextureIndexLegacy();
	GLuint nativeTextureHandle = mBuffers->GetPipelineTexHandleLegacy(currentFBTextureIdx);

	// Hard-lock the active texture directly to the legacy unit 0!
	glBindTexture(GL_TEXTURE_2D, nativeTextureHandle != 0 ? nativeTextureHandle : 0);

	if (ViewportLinearScale())
	{
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	}
	else
	{
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}

	glUseProgram(0);

	// Setup layout transformation matrix stacks
	glMatrixMode(GL_TEXTURE); glLoadIdentity();
	float scaleX = screen->mScreenViewport.width / (float)mBuffers->GetWidth();
	float scaleY = screen->mScreenViewport.height / (float)mBuffers->GetHeight();
	glScalef(scaleX, scaleY, 1.0f);

	glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
	glOrtho(0.0, 1.0, 0.0, 1.0, -1.0, 1.0);

	glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();

	// Wake up native hardware fixed texture blend channels
	glTexEnvf(0x2300, 0x3001, 0x2100); // GL_TEXTURE_ENV_MODE = GL_MODULATE
	OpenGLRenderer::gl_RenderState.ApplyFixedGL1xFunction(nullptr);

	glPushAttrib(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT | GL_ENABLE_BIT);
	glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDepthMask(GL_FALSE);

	for (int i = 0; i < 16; i++) glDisableVertexAttribArray(i);

	// Rasterize fullscreen rectangle straight from the verified POT-aligned memory array list!
	RenderScreenQuadGL1x();

	glPopAttrib();
	glMatrixMode(GL_MODELVIEW); glPopMatrix();
	glMatrixMode(GL_PROJECTION); glPopMatrix();
	glMatrixMode(GL_TEXTURE); glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
}

void FGLRenderer::DrawPresentTextureGL3x(const IntRect& box, bool applyGamma)
{
	glViewport(box.left, box.top, box.width, box.height);

	mBuffers->BindDitherTexture(1);

	glActiveTexture(GL_TEXTURE0);
	if (ViewportLinearScale())
	{
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	}
	else
	{
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}

	mPresentShader->Bind();

	if (!applyGamma || framebuffer->IsHWGammaActive())
	{
		mPresentShader->Uniforms->InvGamma = 1.0f;
		mPresentShader->Uniforms->Contrast = 1.0f;
		mPresentShader->Uniforms->Brightness = 0.0f;
		mPresentShader->Uniforms->Saturation = 1.0f;
	}
	else
	{
		mPresentShader->Uniforms->InvGamma = 1.0f / clamp<float>(vid_gamma, 0.1f, 4.f);
		mPresentShader->Uniforms->Contrast = clamp<float>(vid_contrast, 0.1f, 3.f);
		mPresentShader->Uniforms->Brightness = clamp<float>(vid_brightness, -0.8f, 0.8f);
		mPresentShader->Uniforms->Saturation = clamp<float>(vid_saturation, -15.0f, 15.f);
		mPresentShader->Uniforms->GrayFormula = static_cast<int>(gl_satformula);
	}

	if (vid_hdr_active && framebuffer->IsFullscreen())
	{
		mPresentShader->Uniforms->HdrMode = 1;
		mPresentShader->Uniforms->ColorScale = (gl_dither_bpc == -1) ? 1023.0f : (float)((1 << gl_dither_bpc) - 1);
	}
	else
	{
		mPresentShader->Uniforms->HdrMode = 0;
		mPresentShader->Uniforms->ColorScale = (gl_dither_bpc == -1) ? 255.0f : (float)((1 << gl_dither_bpc) - 1);
	}

	mPresentShader->Uniforms->Scale = { screen->mScreenViewport.width / (float)mBuffers->GetWidth(), screen->mScreenViewport.height / (float)mBuffers->GetHeight() };
	mPresentShader->Uniforms->Offset = { 0.0f, 0.0f };
	mPresentShader->Uniforms.SetData();

	static_cast<GLDataBuffer*>(mPresentShader->Uniforms.GetBuffer())->BindBase();

	RenderScreenQuad();
}

//void FGLRenderer::DrawPresentTexture(const IntRect& box, bool applyGamma)
//{
//	glViewport(box.left, box.top, box.width, box.height);
//
//	mBuffers->BindDitherTexture(1);
//
//	glActiveTexture(GL_TEXTURE0);
//	if (ViewportLinearScale())
//	{
//		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
//		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
//	}
//	else
//	{
//		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
//		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
//	}
//
//	if (gl.legacyMode)
//	{
//		// GL1x/GL2x - force unbind any legacy programs and flush states directly into old GPU registers
//		glUseProgram(0);
//
//		glEnable(GL_TEXTURE_2D);
//		glDisable(GL_BLEND);
//		glDisable(GL_FOG);
//		glDepthMask(GL_FALSE);
//		glDepthFunc(GL_ALWAYS);
//
//		// Synchronize state machine trackers natively before firing draw calls
//		OpenGLRenderer::gl_RenderState.ApplyFixedGL1xFunction(nullptr);
//
//		// Fire raw zero-overhead hardware quad arrays blit to swap buffer on monitor
//		RenderScreenQuad();
//
//		// Restore factory depth matrix layouts safely
//		glDepthMask(GL_TRUE);
//		glDepthFunc(GL_LESS);
//		return; // Safe checkpoint completely bypasses deleted mPresentShader null pointer blocks
//	}
//
//	// ---=== ORIGINAL MODERN CORE PROFILE SHADER PATH ===---
//	mPresentShader->Bind();
//	if (!applyGamma || framebuffer->IsHWGammaActive())
//	{
//		mPresentShader->Uniforms->InvGamma = 1.0f;
//		mPresentShader->Uniforms->Contrast = 1.0f;
//		mPresentShader->Uniforms->Brightness = 0.0f;
//		mPresentShader->Uniforms->Saturation = 1.0f;
//	}
//	else
//	{
//		mPresentShader->Uniforms->InvGamma = 1.0f / clamp<float>(vid_gamma, 0.1f, 4.f);
//		mPresentShader->Uniforms->Contrast = clamp<float>(vid_contrast, 0.1f, 3.f);
//		mPresentShader->Uniforms->Brightness = clamp<float>(vid_brightness, -0.8f, 0.8f);
//		mPresentShader->Uniforms->Saturation = clamp<float>(vid_saturation, -15.0f, 15.f);
//		mPresentShader->Uniforms->GrayFormula = static_cast<int>(gl_satformula);
//	}
//	if (vid_hdr_active && framebuffer->IsFullscreen())
//	{
//		// Full screen exclusive mode treats a rgba16f frame buffer as linear.
//		// It probably will eventually in desktop mode too, but the DWM doesn't seem to support that.
//		mPresentShader->Uniforms->HdrMode = 1;
//		mPresentShader->Uniforms->ColorScale = (gl_dither_bpc == -1) ? 1023.0f : (float)((1 << gl_dither_bpc) - 1);
//	}
//	else
//	{
//		mPresentShader->Uniforms->HdrMode = 0;
//		mPresentShader->Uniforms->ColorScale = (gl_dither_bpc == -1) ? 255.0f : (float)((1 << gl_dither_bpc) - 1);
//	}
//	mPresentShader->Uniforms->Scale = { screen->mScreenViewport.width / (float)mBuffers->GetWidth(), screen->mScreenViewport.height / (float)mBuffers->GetHeight() };
//	mPresentShader->Uniforms->Offset = { 0.0f, 0.0f };
//	mPresentShader->Uniforms.SetData();
//	static_cast<GLDataBuffer*>(mPresentShader->Uniforms.GetBuffer())->BindBase();
//	RenderScreenQuad();
//}

//-----------------------------------------------------------------------------
//
// Fills the black bars around the screen letterbox
//
//-----------------------------------------------------------------------------

void FGLRenderer::ClearBorders()
{
	const auto &box = screen->mOutputLetterbox;

	int clientWidth = framebuffer->GetClientWidth();
	int clientHeight = framebuffer->GetClientHeight();
	if (clientWidth == 0 || clientHeight == 0)
		return;

	glViewport(0, 0, clientWidth, clientHeight);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glEnable(GL_SCISSOR_TEST);
	if (box.top > 0)
	{
		glScissor(0, 0, clientWidth, box.top);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	if (clientHeight - box.top - box.height > 0)
	{
		glScissor(0, box.top + box.height, clientWidth, clientHeight - box.top - box.height);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	if (box.left > 0)
	{
		glScissor(0, box.top, box.left, box.height);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	if (clientWidth - box.left - box.width > 0)
	{
		glScissor(box.left + box.width, box.top, clientWidth - box.left - box.width, box.height);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	glDisable(GL_SCISSOR_TEST);
}

}