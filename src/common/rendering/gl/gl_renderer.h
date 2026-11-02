// gl_renderer.h
#ifndef __GL_RENDERER_H
#define __GL_RENDERER_H

#include "v_video.h"
#include "vectors.h"
#include "matrix.h"
#include "gl_renderbuffers.h"
#include <functional>

#ifdef _MSC_VER
#pragma warning(disable:4244)
#endif

struct particle_t;
class FCanvasTexture;
class FFlatVertexBuffer;
class FSkyVertexBuffer;
class HWPortal;
class FLightBuffer;
class DPSprite;
class FGLRenderBuffers;
class FGL2DDrawer;
class SWSceneDrawer;
class HWViewpointBuffer;
struct FRenderViewpoint;

namespace OpenGLRenderer
{
	class FHardwareTexture;
	class FShaderManager;
	class FSamplerManager;
	class OpenGLFrameBuffer;
	class FPresentShaderBase;
	class FPresentShader;
	class FPresent3DCheckerShader;
	class FPresent3DColumnShader;
	class FPresent3DRowShader;
	class FShadowMapShader;

class FGLRenderer
{
public:

	OpenGLFrameBuffer *framebuffer;
	int mMirrorCount = 0;
	int mPlaneMirrorCount = 0;
	FShaderManager *mShaderManager = nullptr;
	FSamplerManager *mSamplerManager = nullptr;
	unsigned int mFBID;
	unsigned int mVAOID;
	unsigned int mStencilValue = 0;

	int mOldFBID;

	FGLRenderBuffers *mBuffers = nullptr;
	FGLRenderBuffers *mScreenBuffers = nullptr;
	FGLRenderBuffers *mSaveBuffers = nullptr;
	FPresentShader *mPresentShader = nullptr;
	FPresent3DCheckerShader *mPresent3dCheckerShader = nullptr;
	FPresent3DColumnShader *mPresent3dColumnShader = nullptr;
	FPresent3DRowShader *mPresent3dRowShader = nullptr;
	FShadowMapShader *mShadowMapShader = nullptr;

	int mLightMapID = 0;

	//FRotator mAngles;

	FGLRenderer(OpenGLFrameBuffer *fb);
	~FGLRenderer() ;

	void Initialize(int width, int height);

	void ClearBorders();

	void PresentStereo();
	void RenderScreenQuad();
	void RenderScreenQuadGL1x();
	void RenderScreenQuadGL3x();
	void PostProcessScene(int fixedcm, float flash, const std::function<void()> &afterBloomDrawEndScene2D);
	void PostProcessSceneGL1x(int fixedcm, float flash, const std::function<void()>& afterBloomDrawEndScene2D);
	void PostProcessSceneGL3x(int fixedcm, float flash, const std::function<void()>& afterBloomDrawEndScene2D);
	void AmbientOccludeScene(float m5);
	void ClearTonemapPalette();
	void BlurScene(float gameinfobluramount);
	void CopyToBackbuffer(const IntRect *bounds, bool applyGamma);
	void DrawPresentTexture(const IntRect &box, bool applyGamma);
	void DrawPresentTextureGL1x(const IntRect &box, bool applyGamma);
	void DrawPresentTextureGL3x(const IntRect &box, bool applyGamma);
	void Flush();
	void BeginFrame();

	bool StartOffscreen();
	void EndOffscreen();

	void BindToFrameBuffer(FTexture* tex);

	// GL1x/GL2x stuff
	FTextureID gl1xLight;                      // light blob texture (round white to black gradient)
	FTextureID gl1xPart1;
	FTextureID gl1xPart2;
	FTextureID mirrorgl1xTexture;
	void GetSpecialGL1xTextures();
	class FFlatVertexBuffer* mVBO = nullptr;   // Main vertex buffer
	class FSkyVertexBuffer* mSkyVBO = nullptr; // Sky vertex buffer
	bool mDrawingScene2D = false;              // flag that 2D HUD or menu is drawn on top of 3D world
	float mSky1Pos = 0.0f, mSky2Pos = 0.0f;    // Sky textures shift coordinates (scrolling sky)
	float mGlobVis = 0.0f;                     // Fixed-function globvis fog cache
	// Viewports for picture scaling and XP letterboxing:
	// Adapted from old GL_IRECT to the new unified IntRect structures of v4.14
	IntRect mScreenViewport;
	IntRect mSceneViewport;
	IntRect mOutputLetterbox;

private:

	bool QuadStereoCheckInitialRenderContextState();
	void PresentAnaglyph(bool r, bool g, bool b);
	void PresentSideBySide(int);
	void PresentTopBottom();
	void prepareInterleavedPresent(FPresentShaderBase& shader);
	void PresentColumnInterleaved();
	void PresentRowInterleaved();
	void PresentCheckerInterleaved();
	void PresentQuadStereo();

};

struct TexFilter_s
{
	int minfilter;
	int magfilter;
	bool mipmapping;
} ;


extern FGLRenderer *GLRenderer;

}
#endif
