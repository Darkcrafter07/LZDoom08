// gl_interface.h
#ifndef R_RENDER
#define R_RENDER

enum GLCompat
{
	CMPT_GL2,
	CMPT_GL2_SHADER,
	CMPT_GL3,
	CMPT_GL4
};

enum ELightMethod
{
	LM_LEGACY = 0,		// placeholder for legacy mode (textured lights), should not be checked anywhere in the code!
	LM_DEFERRED = 1,	// calculate lights up front in a separate pass
	LM_DIRECT = 2,		// calculate lights on the fly along with the render data
};

enum EBufferMethod
{
	BM_LEGACY = 0,		// placeholder for legacy mode (client arrays), should not be checked anywhere in the code!
	BM_DEFERRED = 1,	// use a temporarily mapped buffer, for GL 3.x core profile
	BM_PERSISTENT = 2	// use a persistently mapped buffer
};


struct RenderContext
{
	unsigned int flags;
	unsigned int maxuniforms;
	unsigned int maxuniformblock;
	unsigned int uniformblockalignment;
	int lightmethod;
	int buffermethod;
	float glslversion;
	int max_texturesize;
	char* vendorstring;
	char* modelstring;
	bool legacyMode; // this one applies to both GL1x and GL2x, as GL2x is same but wrapped via shader based frame buffer
	bool gl1path;            // this one is OpenGL1.3
	bool gl1_v1dot1 = false; // this one is OpenGL1.1 as a fork of GL1.3, e.g. built on top!
	bool glnpot = false;     // OpenGL non-power-of-two, false means npot support is ENABLED!
	bool es;

	int MaxLights() const
	{
		return maxuniforms >= 2048 ? 128 : 64;
	}
};

extern RenderContext gl;

#endif

