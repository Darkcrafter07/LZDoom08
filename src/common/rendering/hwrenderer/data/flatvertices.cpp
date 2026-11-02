/*
** hw_flatvertices.cpp
** Creates flat vertex data for hardware rendering.
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

#include "c_cvars.h"
#include "flatvertices.h"
#include "v_video.h"
#include "cmdlib.h"
#include "printf.h"
#include "hwrenderer/data/buffers.h"

#include "common/rendering/gl_load/gl_system.h"    // to get GL commands
#include "common/rendering/gl_load/gl_interface.h" // to get "if (gl.legacyMode) context

//==========================================================================
//
//
//
//==========================================================================

FFlatVertexBuffer::FFlatVertexBuffer(int width, int height, int pipelineNbr):
	mPipelineNbr(pipelineNbr)
{
	vbo_shadowdata.Resize(NUM_RESERVED);

	if (gl.legacyMode || gl.gl1path)
	{
		m_Legacy_CPU_Memory = new FFlatVertex[BUFFER_SIZE];
		DPrintf(DMSG_NOTIFY, "Using client array buffer for legacy pipeline draw loops\n");
	}

	// the first quad is reserved for handling coordinates through uniforms.
	vbo_shadowdata[0].Set(0, 0, 0, 0, 0);
	vbo_shadowdata[1].Set(1, 0, 0, 0, 0);
	vbo_shadowdata[2].Set(2, 0, 0, 0, 0);
	vbo_shadowdata[3].Set(3, 0, 0, 0, 0);

	// and the second one for the fullscreen quad used for blend overlays.
	vbo_shadowdata[4].Set(0, 0, 0, 0, 0);
	vbo_shadowdata[5].Set(0, (float)height, 0, 0, 1);
	vbo_shadowdata[6].Set((float)width, 0, 0, 1, 0);
	vbo_shadowdata[7].Set((float)width, (float)height, 0, 1, 1);

	// and this is for the postprocessing copy operation
	vbo_shadowdata[8].Set(-1.0f, -1.0f, 0, 0.0f, 0.0f);
	vbo_shadowdata[9].Set(3.0f, -1.0f, 0, 2.f, 0.0f);
	vbo_shadowdata[10].Set(-1.0f, 3.0f, 0, 0.0f, 2.f);
	vbo_shadowdata[11].Set(3.0f, 3.0f, 0, 2.f, 2.f); // Note: not used anymore

	// The next two are the stencil caps.
	vbo_shadowdata[12].Set(-32767.0f, 32767.0f, -32767.0f, 0, 0);
	vbo_shadowdata[13].Set(-32767.0f, 32767.0f, 32767.0f, 0, 0);
	vbo_shadowdata[14].Set(32767.0f, 32767.0f, 32767.0f, 0, 0);
	vbo_shadowdata[15].Set(32767.0f, 32767.0f, -32767.0f, 0, 0);

	vbo_shadowdata[16].Set(-32767.0f, -32767.0f, -32767.0f, 0, 0);
	vbo_shadowdata[17].Set(-32767.0f, -32767.0f, 32767.0f, 0, 0);
	vbo_shadowdata[18].Set(32767.0f, -32767.0f, 32767.0f, 0, 0);
	vbo_shadowdata[19].Set(32767.0f, -32767.0f, -32767.0f, 0, 0);

	mIndexBuffer = screen->CreateIndexBuffer();
	int data[4] = {};
	mIndexBuffer->SetData(4, data, BufferUsageType::Static); // On Vulkan this may not be empty, so set some dummy defaults to avoid crashes.


	for (int n = 0; n < mPipelineNbr; n++)
	{
		mVertexBufferPipeline[n] = screen->CreateVertexBuffer();

		unsigned int bytesize = BUFFER_SIZE * sizeof(FFlatVertex);
		// If legacyMode is on, downgrade vertex storage allocation flags to dynamic streams usage
		BufferUsageType targetUsage = (gl.legacyMode || gl.gl1path) ? BufferUsageType::Stream : BufferUsageType::Persistent;
		mVertexBufferPipeline[n]->SetData(bytesize, nullptr, targetUsage);

		static const FVertexBufferAttribute format[] = {
			{ 0, VATTR_VERTEX, VFmt_Float3, (int)myoffsetof(FFlatVertex, x) },
			{ 0, VATTR_TEXCOORD, VFmt_Float2, (int)myoffsetof(FFlatVertex, u) },
			{ 0, VATTR_LIGHTMAP, VFmt_Float3, (int)myoffsetof(FFlatVertex, lu) },
		};

		mVertexBufferPipeline[n]->SetFormat(1, 3, sizeof(FFlatVertex), format);
	}

	mVertexBuffer = mVertexBufferPipeline[mPipelinePos];

	mIndex = mCurIndex = NUM_RESERVED;
	mNumReserved = NUM_RESERVED;
	Copy(0, NUM_RESERVED);
}

//==========================================================================
//
//
//
//==========================================================================

FFlatVertexBuffer::~FFlatVertexBuffer()
{
	// 1. [Darkcrafter07] - Clean up our custom legacy Client Array memory grid safely
	if (m_Legacy_CPU_Memory != nullptr)
	{
		delete[] m_Legacy_CPU_Memory;
		m_Legacy_CPU_Memory = nullptr;
	}

	// 2. Loop and destroy Magnus's modern pipelines handlers
	for (int n = 0; n < mPipelineNbr; n++)
	{
		if (mVertexBufferPipeline[n] != nullptr)
		{
			delete mVertexBufferPipeline[n];
			mVertexBufferPipeline[n] = nullptr;
		}
	}

	// 3. Purge index arrays layout trackers
	if (mIndexBuffer != nullptr)
	{
		delete mIndexBuffer;
		mIndexBuffer = nullptr;
	}

	mVertexBuffer = nullptr;
}

//==========================================================================
//
//
//
//==========================================================================

void FFlatVertexBuffer::OutputResized(int width, int height)
{
	vbo_shadowdata[4].Set(0, 0, 0, 0, 0);
	vbo_shadowdata[5].Set(0, (float)height, 0, 0, 1);
	vbo_shadowdata[6].Set((float)width, 0, 0, 1, 0);
	vbo_shadowdata[7].Set((float)width, (float)height, 0, 1, 1);
	Copy(4, 4);
}

//==========================================================================
//
//
//
//==========================================================================

std::pair<FFlatVertex *, unsigned int> FFlatVertexBuffer::AllocVertices(unsigned int count)
{
	FFlatVertex *p = GetBuffer();
	auto index = mCurIndex.fetch_add(count);
	if (index + count >= BUFFER_SIZE_TO_USE)
	{
		// If a single scene needs 2'000'000 vertices there must be something very wrong. 
		I_FatalError("Out of vertex memory. Tried to allocate more than %u vertices for a single frame", index + count);
	}
	return std::make_pair(p, index);
}

//==========================================================================
//
//
//
//==========================================================================

void FFlatVertexBuffer::Copy(int start, int count)
{
	IVertexBuffer* old = mVertexBuffer;

	for (int n = 0; n < mPipelineNbr; n++)
	{
		mVertexBuffer = mVertexBufferPipeline[n];
		Map();
		memcpy(GetBuffer(start), &vbo_shadowdata[0], count * sizeof(FFlatVertex));
		Unmap();
		mVertexBuffer->Upload(start * sizeof(FFlatVertex), count * sizeof(FFlatVertex));
	}

	mVertexBuffer = old;
}

//==========================================================================
//
// GL1x/GL2x stuff
//
//==========================================================================

void FFlatVertexBuffer::BindVBO()
{
	// ---=== PATHWAY A: ORIGINAL MODERN CORE PROFILE SHADER ROUTING (GL3.3+) ===---
	if (!gl.legacyMode && !gl.gl1path)
	{
		glBindBuffer(GL_ARRAY_BUFFER, vbo_id);

		// Modern core profile attribute layout pointers binding path
		glVertexAttribPointer(0, 3, GL_FLOAT, false, sizeof(FFlatVertex), &((FFlatVertex*)NULL)->x);
		glVertexAttribPointer(1, 2, GL_FLOAT, false, sizeof(FFlatVertex), &((FFlatVertex*)NULL)->u);
		glEnableVertexAttribArray(0);
		glEnableVertexAttribArray(1);
	}
	// ---=== [Darkcrafter07] PATHWAY B: PURE FIXED-FUNCTION CLIENT ARRAYS PASS ===---
	else
	{
		// [Darkcrafter07] - HARD-WIPE MODERN BUFFER OBJECTS LOCKS!
		// We explicitly bind array buffer to 0 to instruct the legacy GPU driver
		// to pull vertices data directly from raw system RAM pointers sheets instead of VRAM!
		glBindBuffer(GL_ARRAY_BUFFER, 0);

		if (m_Legacy_CPU_Memory != nullptr)
		{
			// Force state pipeline flags configuration baseline
			glDisable(GL_LIGHTING);
			glActiveTexture(GL_TEXTURE0);
			glEnable(GL_TEXTURE_2D);

			// [Darkcrafter07] - THE MONOLITHIC HARDWARE VERTEX ENTRY COUPLING!
			// We stream floating-point bytes strictly from our secure continuous CPU memory block!
			glVertexPointer(3, GL_FLOAT, sizeof(FFlatVertex), &m_Legacy_CPU_Memory[0].x);
			glTexCoordPointer(2, GL_FLOAT, sizeof(FFlatVertex), &m_Legacy_CPU_Memory[0].u);

			glEnableClientState(GL_VERTEX_ARRAY);
			glEnableClientState(GL_TEXTURE_COORD_ARRAY);
			glDisableClientState(GL_COLOR_ARRAY);
		}
	}
}


//void FFlatVertexBuffer::RenderArray(unsigned int primtype, unsigned int offset, unsigned int count)
//{
//	glDrawArrays(primtype, offset, count);
//}

void FFlatVertexBuffer::RenderArray(unsigned int primtype, unsigned int offset, unsigned int count)
{
	// Fire standard un-indexed native primitive array rasterization loop
	glDrawArrays(primtype, offset, count);

	// [Darkcrafter07] - POST-DRAW CONTEXT SANITIZER LAYER
	if (gl.legacyMode || gl.gl1path)
	{
		// Safely drop active client array states hooks to safeguard subsequent HUD pipeline passes
		glDisableClientState(GL_VERTEX_ARRAY);
		glDisableClientState(GL_TEXTURE_COORD_ARRAY);
		glFlush();
	}
}


