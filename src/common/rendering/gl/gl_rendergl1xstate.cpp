// gl_rendergl1xstate.cpp
#include "gl_system.h"
#include "gl_interface.h"
#include "hw_cvars.h"
#include "flatvertices.h"
#include "gl_shader.h"
#include "gl_renderer.h"
#include "gl_renderstate.h"
#include "hw_lightbuffer.h"
#include "hw_bonebuffer.h"
#include "gl_renderbuffers.h"
#include "gl_hwtexture.h"
#include "gl_buffers.h"
#include "hw_clock.h"
#include "hwrenderer/data/hw_viewpointbuffer.h"

#include "playsim/actor.h"


namespace OpenGLRenderer
{
	//==========================================================================
	//
	// State apply method for GL1x/GL2x
	//
	//==========================================================================

	// Call these deep in the code
	void ApplyGL1xStateBridge(FRenderState& baseState, AActor* actor)
	{
		static_cast<FGLRenderState&>(baseState).ApplyGL1x(actor);
	}

	// Not the same as "ApplyFixedGL1xFunction"!
	// It's rather made to toggle between fixed GL pipeline states.
	void FGLRenderState::ApplyGL1x(AActor *actor)
	{
		// Bind vertex buffers dynamically if the engine has them queued
		if (mVertexBuffer != mCurrentVertexBuffer)
		{
			if (mVertexBuffer == nullptr)
			{
				glBindBuffer(GL_ARRAY_BUFFER, 0);
			}
			else
			{
				static_cast<GLVertexBuffer*>(mVertexBuffer)->Bind(mVertexOffsets);
			}
			mCurrentVertexBuffer = mVertexBuffer;
		}

		if (mIndexBuffer != mCurrentIndexBuffer)
		{
			if (mIndexBuffer == nullptr)
			{
				glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
			}
			else
			{
				static_cast<GLIndexBuffer*>(mIndexBuffer)->Bind();
			}
			mCurrentIndexBuffer = mIndexBuffer;
		}

		// Replicate internal state dirty flags flush for Alpha and Blending environments
		if (mRenderStyle != stRenderStyle)
		{
			ApplyBlendMode();
			stRenderStyle = mRenderStyle;
		}

		if (mBias.mChanged)
		{
			if (mBias.mFactor == 0 && mBias.mUnits == 0)
			{
				glDisable(GL_POLYGON_OFFSET_FILL);
			}
			else
			{
				glEnable(GL_POLYGON_OFFSET_FILL);
			}
			glPolygonOffset(mBias.mFactor, mBias.mUnits);
			mBias.mChanged = false;
		}

		// Explicit synchronization flush for material texture sheets inside GL1.x pipeline
		if (mMaterial.mChanged)
		{
			ApplyMaterial(mMaterial.mMaterial, mMaterial.mClampMode, mMaterial.mTranslation, mMaterial.mOverrideShader);
			mMaterial.mChanged = false;
		}

		// Call "ApplyFixedGL1xFunction" in gl_20.cpp
		ApplyFixedGL1xFunction(actor);
	}

} // namespace OpenGLRenderer


// ==========================================================================
//
// [Darkcrafter07] - GL1x/GL2x blending and alpha baseline functionality.
//
// ==========================================================================

void FRenderState::BlendGL1xFunc(int src, int dst)
{
	glBlendFunc(src, dst);
}

void FRenderState::AlphaGL1xFunc(int func, float thresh)
{
	if (func == GL_GREATER) mAlphaThreshold = thresh;
	else mAlphaThreshold = thresh - 0.001f;

	glAlphaFunc(func, thresh);
}

void FRenderState::BlendGL1xEquation(int eq)
{
	glBlendEquation(eq);
}

void FRenderState::SetFixedGL1xColormap(int cm)
{
	// Force track active palette (CM_LITE, CM_TORCH, etc.) 
	// inside our restored fixed-function cache variables slot.
	mColormapState = cm;
}


