// basics.h
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <algorithm>

#define MAXWIDTH 12000
#define MAXHEIGHT 5000

//
// fixed point, 32bit as 16.16.
//
#define FRACBITS						16
#define FRACUNIT						(1<<FRACBITS)

typedef int32_t							fixed_t;

#define FIXED_MAX						(signed)(0x7fffffff)
#define FIXED_MIN						(signed)(0x80000000)

// the last remnants of tables.h
#define ANGLE_90		(0x40000000)
#define ANGLE_180		(0x80000000)
#define ANGLE_MAX		(0xffffffff)

typedef uint32_t			angle_t;

#if defined(__GNUC__)
// With versions of GCC newer than 4.2, it appears it was determined that the
// cost of an unaligned pointer on PPC was high enough to add padding to the
// end of packed structs.  For whatever reason __packed__ and pragma pack are
// handled differently in this regard. Note that this only needs to be applied
// to types which are used in arrays or sizeof is needed. This also prevents
// code from taking references to the struct members.
#define FORCE_PACKED __attribute__((__packed__))
#else
#define FORCE_PACKED
#endif

#ifdef __GNUC__
#define GCCPRINTF(stri,firstargi)		__attribute__((format(printf,stri,firstargi)))
#define GCCFORMAT(stri)					__attribute__((format(printf,stri,0)))
#define GCCNOWARN						__attribute__((unused))
#else
#define GCCPRINTF(a,b)
#define GCCFORMAT(a)
#define GCCNOWARN
#endif

#ifndef MAKE_ID
#ifndef __BIG_ENDIAN__
#define MAKE_ID(a,b,c,d)	((uint32_t)((a)|((b)<<8)|((c)<<16)|((d)<<24)))
#else
#define MAKE_ID(a,b,c,d)	((uint32_t)((d)|((c)<<8)|((b)<<16)|((a)<<24)))
#endif
#endif

using INTBOOL = int;
using BITFIELD = uint32_t;


// always use our own definition for consistency.
#ifdef M_PI
#undef M_PI
#endif

const double M_PI = 3.14159265358979323846;	// matches value in gcc v2 math.h

using std::min;
using std::max;
//using std::clamp;

template<typename T>
T clamp(T val, T minval, T maxval)
{
    return std::max<T>(std::min<T>(val, maxval), minval);
}

//==========================================================================
//
// LERP (linear interpolation)
//
// Standard linear interpolation: returns start + t*(end - start)
//
//==========================================================================
#ifdef lerp
#undef lerp
#endif

static inline float lerp(float start, float end, float t)
{
	return start + t * (end - start);
}

//==========================================================================
//
// ArrayLength (C-style array).
//
// Get the length of a C-style array.
// This is safer than hardcoding sizes.
//
//==========================================================================
#ifdef ArrayLength
#undef ArrayLength
#endif

template<typename T, size_t N>
inline size_t ArrayLength(const T(&)[N])
{
	return N;
}

//==========================================================================
//
// Newton-Raphson Division - "divnr" 
//
// Portable C++ implementation with zero external runtime dependencies.
// Extracts float exponent bits manually to ensure guaranteed convergence.
//
//==========================================================================

#ifdef divnr
#undef divnr
#endif

template<typename T, size_t Iterations = 2>
inline T divnr(const T numerator, const T denominator)
{
	// Prevent hardware failure or NaN explosions on dead zero boundaries
	if (denominator == static_cast<T>(0))
	{
		return static_cast<T>(0);
	}

	// Double-check configuration layer for float types only
	// (Standard structural dynamic lights use plain 32-bit floats)
	float denomF = static_cast<float>(denominator);
	uint32_t bits = *(uint32_t*)&denomF;

	// Extract raw sign and absolute magnitude to filter out signs
	uint32_t signBit = bits & 0x80000000;
	uint32_t absBits = bits & 0x7FFFFFFF;

	// Prevent subnormal floats or near-zero underflows from corrupting registers
	if (absBits < 0x00800000)
	{
		// Fallback for extreme subnormal minimum scales
		return numerator * (static_cast<T>(1) / denominator);
	}

	// Extract the 8-bit IEEE 754 exponent offset matrix baseline
	int32_t rawExponent = static_cast<int32_t>((absBits & 0x7F800000) >> 23) - 127;

	// Re-compile the mantissa into pure normalized [0.5, 1.0) float space
	// 0x3F000000 forces the exponent to exactly 126 (-1 offset), representing 0.5f
	uint32_t normalizedBits = (absBits & 0x007FFFFF) | 0x3F000000;
	float normalizedDenom = *(float*)&normalizedBits;

	// Linear Chebyshev baseline guess layout mapped tightly inside [0.5, 1.0)
	float x = 2.8235294f - 1.8823530f * normalizedDenom;

	// Perform Compile-Time Unrolled Newton-Raphson Iterations
	// Quadratic convergence: 2 iterations fully restore ~16-24 bits precision!
	for (size_t i = 0; i < Iterations; ++i)
	{
		x = x * (2.0f - normalizedDenom * x);
	}

	// Re-scale the reciprocal back to the original exponent matrix space
	// We adjust the integer bit offset manually instead of calling ldexp
	int32_t targetExponent = 127 - rawExponent - 1;

	float finalReciprocal = 0.0f;
	if (targetExponent > 0 && targetExponent < 255)
	{
		// Safe fast-path: Shift the computed bits straight into exponent registry
		uint32_t xBits = *(uint32_t*)&x;
		uint32_t finalBits = (xBits & 0x007FFFFF) | (static_cast<uint32_t>(targetExponent) << 23) | signBit;
		finalReciprocal = *(float*)&finalBits;
	}
	else
	{
		// Fallback override path for radical extreme scales
		finalReciprocal = 1.0f / denominator;
	}

	// Final single multiplication to return (numerator * (1 / denominator))
	return numerator * static_cast<T>(finalReciprocal);
}
