//========= ASTC runtime recompression feature (added on top of Valve TOGL) =====//
//
// astc_texcompress.cpp
//
// See astc_texcompress.h for the feature overview. This file:
//   1. classifies D3DFORMATs into "eligible for ASTC" / "LDR" / "HDR"
//   2. converts whatever GL (format,type) the caller has in memory into the
//      plain byte/half/float RGBA buffer astcenc wants
//   3. calls into ARM's astcenc (if HAVE_ASTCENC is compiled in) to produce
//      real ASTC blocks, choosing ASTCENC_PRF_LDR vs ASTCENC_PRF_HDR
//
//===============================================================================

#include "astc_texcompress.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// Real D3DFMT_* / D3DFORMAT enum, and the DXT block decompressors used for
// the "already compressed -> decompress -> re-encode as ASTC" path.
#include "togles/rendermechanism.h"
#include "decompress.h"

#if defined( HAVE_ASTCENC )
	#include <astcenc.h>
#endif

// ---------------------------------------------------------------------------
// ConVars (declared extern in astc_texcompress.h so cglmtex.cpp etc. can see them)
// ---------------------------------------------------------------------------

ConVar gl_astc_debug_disable( "gl_astc_debug_disable", "0", FCVAR_ARCHIVE,
	"Debug-only kill switch for the mandatory ASTC texture compression "
	"feature. Leave at 0. Only set to 1 temporarily to bisect a suspected "
	"ASTC-related rendering bug -- every eligible texture format is "
	"compressed to ASTC by default, this is not a normal opt-out." );

ConVar gl_astc_block_ldr( "gl_astc_block_ldr", "4x4", FCVAR_ARCHIVE,
	"ASTC block footprint used for 8/16-bit normalized color sources. "
	"Smaller blocks (4x4) = higher quality/bigger; larger blocks (8x8) = "
	"more compression/smaller." );

ConVar gl_astc_block_hdr( "gl_astc_block_hdr", "6x6", FCVAR_ARCHIVE,
	"ASTC block footprint used for floating point / half-float (HDR) "
	"sources." );

ConVar gl_astc_quality( "gl_astc_quality", "60", FCVAR_ARCHIVE,
	"ASTC encoder quality/speed tradeoff, 0 (fastest) - 100 (thorough)." );

// ---------------------------------------------------------------------------
// Format classification
//
// Every plain (non-block-compressed) color format TOGL's g_formatDescTable
// (cglmtex.cpp) actually maps to a GL upload is eligible -- verified against
// that table entry-by-entry, including exactly which packed GL type each one
// uses, so BuildRGBABuffer() below can unpack every one of them correctly
// instead of assuming a generic byte-per-component layout. D3DFMT_L16,
// D3DFMT_G16R16, D3DFMT_G16R16F, D3DFMT_G32R32F and D3DFMT_R16F are
// deliberately NOT listed: they have no row in g_formatDescTable in this
// tree (grep it before re-adding them) so a texture can never actually
// arrive in one of those formats here.
//
// DXT1/3/5 are handled separately (ASTC_IsDXTFormat / ASTC_CompressDXTToASTC)
// since they arrive already block-compressed and need a decompress-then-
// encode step instead.
//
// Formats that are NOT here on purpose, because ASTC fundamentally cannot
// represent them (these stay uncompressed no matter what):
//   - depth/stencil formats (D3DFMT_D24S8 and friends) -- ASTC has no
//     depth/stencil block mode, and GPUs can't sample/blend these as color
//     anyway.
//   - any format on a render target or multisampled surface -- OpenGL has
//     no "render into a compressed texture" capability on any driver; that
//     check lives in cglmtex.cpp's WriteTexels() alongside the format check
//     here, not in this function.
// ---------------------------------------------------------------------------

bool ASTC_IsEligibleFormat( int d3dFormat )
{
	switch ( d3dFormat )
	{
		// GL_BGRA / GL_UNSIGNED_INT_8_8_8_8_REV (32-bit packed)
		case D3DFMT_A8R8G8B8:
		case D3DFMT_X8R8G8B8:
		case D3DFMT_Q8W8V8U8:
		// NOTE: D3DFMT_A2R10G10B10 / D3DFMT_A2B10G10R10 were removed from
		// this list -- they aren't declared in this tree's imageformat.h at
		// all (build error: "undeclared identifier"), and in practice these
		// two formats are almost always used as HDR accumulation/bloom
		// render targets in Source, which are already excluded above by the
		// kGLMTexRenderable check in cglmtex.cpp. If your engine build does
		// use one of these as a static (non-render-target) texture, add the
		// case back here -- ASTC_GetSrcBytesPerTexel()/BuildLDRTexel() below
		// already handle GL_UNSIGNED_INT_10_10_10_2 correctly, only the
		// D3DFMT_* symbols were the problem.
		// GL_BGRA / GL_UNSIGNED_SHORT_4_4_4_4_REV (16-bit packed)
		case D3DFMT_A4R4G4B4:
		// GL_BGRA / GL_UNSIGNED_SHORT_1_5_5_5_REV (16-bit packed)
		case D3DFMT_A1R5G5B5:
		case D3DFMT_X1R5G5B5:
		// GL_RGB / GL_UNSIGNED_SHORT_5_6_5 (16-bit packed, non-REV, no alpha)
		case D3DFMT_R5G6B5:
		// GL_LUMINANCE / GL_LUMINANCE_ALPHA / GL_ALPHA / GL_BGR, all plain GL_UNSIGNED_BYTE
		case D3DFMT_L8:
		case D3DFMT_A8L8:
		case D3DFMT_A8:
		case D3DFMT_R8G8B8:
		// GL_RGBA / GL_UNSIGNED_SHORT (64bpp normalized, non-packed)
		case D3DFMT_A16B16G16R16:
		// floating point (HDR -- see ASTC_IsHDRFormat)
		case D3DFMT_A16B16G16R16F:
		case D3DFMT_A32B32G32R32F:
		case D3DFMT_R32F:
			return true;
		default:
			return false;
	}
}

bool ASTC_IsDXTFormat( int d3dFormat )
{
	switch ( d3dFormat )
	{
		case D3DFMT_DXT1:
		case D3DFMT_DXT3:
		case D3DFMT_DXT5:
			return true;
		default:
			return false;
	}
}

bool ASTC_IsHDRFormat( int d3dFormat )
{
	switch ( d3dFormat )
	{
		case D3DFMT_A16B16G16R16F:
		case D3DFMT_A32B32G32R32F:
		case D3DFMT_R32F:
			return true;
		default:
			return false;	// everything else is a normalized ("byte-ish") format -> LDR
	}
}

// Parse "4x4" / "6x6" / "8x8" style convar strings into two ints.
static void ParseBlockSize( const char* str, int* outW, int* outH )
{
	int w = 4, h = 4;
	if ( str && sscanf( str, "%dx%d", &w, &h ) != 2 )
	{
		w = 4; h = 4;
	}
	*outW = w;
	*outH = h;
}

static uint32_t GLBlockSizeToInternalFormat( int blockW, int blockH )
{
	struct Entry { int w, h; uint32_t enumVal; };
	static const Entry table[] =
	{
		{ 4,  4,  GL_COMPRESSED_RGBA_ASTC_4x4_KHR   },
		{ 5,  4,  GL_COMPRESSED_RGBA_ASTC_5x4_KHR   },
		{ 5,  5,  GL_COMPRESSED_RGBA_ASTC_5x5_KHR   },
		{ 6,  5,  GL_COMPRESSED_RGBA_ASTC_6x5_KHR   },
		{ 6,  6,  GL_COMPRESSED_RGBA_ASTC_6x6_KHR   },
		{ 8,  5,  GL_COMPRESSED_RGBA_ASTC_8x5_KHR   },
		{ 8,  6,  GL_COMPRESSED_RGBA_ASTC_8x6_KHR   },
		{ 8,  8,  GL_COMPRESSED_RGBA_ASTC_8x8_KHR   },
		{ 10, 5,  GL_COMPRESSED_RGBA_ASTC_10x5_KHR  },
		{ 10, 6,  GL_COMPRESSED_RGBA_ASTC_10x6_KHR  },
		{ 10, 8,  GL_COMPRESSED_RGBA_ASTC_10x8_KHR  },
		{ 10, 10, GL_COMPRESSED_RGBA_ASTC_10x10_KHR },
		{ 12, 10, GL_COMPRESSED_RGBA_ASTC_12x10_KHR },
		{ 12, 12, GL_COMPRESSED_RGBA_ASTC_12x12_KHR },
	};
	for ( const Entry& e : table )
	{
		if ( e.w == blockW && e.h == blockH )
			return e.enumVal;
	}
	return GL_COMPRESSED_RGBA_ASTC_4x4_KHR;	// safe fallback
}

// ---------------------------------------------------------------------------
// Source pixel normalization: whatever (glFormat, glType) the caller has,
// produce a tightly packed RGBA buffer astcenc can consume directly --
// U8 RGBA8 for LDR, F32 RGBA for HDR. Every (glFormat, glType) pair handled
// here was verified against the real GL columns of g_formatDescTable in
// cglmtex.cpp -- including the packed 16-bit/32-bit types (_REV and non-REV)
// -- rather than assumed, since guessing wrong here silently corrupts the
// texture instead of failing loudly.
// ---------------------------------------------------------------------------
// Minimal local copies of the GL enums we branch on, in case gl.h isn't
// pulled in ahead of this file in some build configs. Defined unconditionally
// (not gated by HAVE_ASTCENC) because ASTC_GetSrcBytesPerTexel() below needs
// them regardless of whether astcenc itself is compiled in.
#ifndef GL_UNSIGNED_BYTE
#define GL_UNSIGNED_BYTE                 0x1401
#endif
#ifndef GL_UNSIGNED_SHORT
#define GL_UNSIGNED_SHORT                0x1403
#endif
#ifndef GL_FLOAT
#define GL_FLOAT                         0x1406
#endif
#ifndef GL_RED
#define GL_RED                           0x1903
#endif
#ifndef GL_ALPHA
#define GL_ALPHA                         0x1906
#endif
#ifndef GL_RGB
#define GL_RGB                           0x1907
#endif
#ifndef GL_RGBA
#define GL_RGBA                          0x1908
#endif
#ifndef GL_LUMINANCE
#define GL_LUMINANCE                     0x1909
#endif
#ifndef GL_LUMINANCE_ALPHA
#define GL_LUMINANCE_ALPHA               0x190A
#endif
#ifndef GL_BGR
#define GL_BGR                           0x80E0
#endif
#ifndef GL_BGRA
#define GL_BGRA                          0x80E1
#endif
#ifndef GL_UNSIGNED_SHORT_5_6_5
#define GL_UNSIGNED_SHORT_5_6_5          0x8363
#endif
#ifndef GL_UNSIGNED_SHORT_4_4_4_4_REV
#define GL_UNSIGNED_SHORT_4_4_4_4_REV    0x8365
#endif
#ifndef GL_UNSIGNED_SHORT_1_5_5_5_REV
#define GL_UNSIGNED_SHORT_1_5_5_5_REV    0x8366
#endif
#ifndef GL_UNSIGNED_INT_8_8_8_8_REV
#define GL_UNSIGNED_INT_8_8_8_8_REV      0x8367
#endif
#ifndef GL_UNSIGNED_INT_10_10_10_2
#define GL_UNSIGNED_INT_10_10_10_2       0x8036
#endif
#ifndef GL_HALF_FLOAT_ARB
#define GL_HALF_FLOAT_ARB                0x140B
#endif

#if defined( HAVE_ASTCENC )

static float HalfToFloat( uint16_t h )
{
	uint32_t sign = (h & 0x8000u) << 16;
	uint32_t exp  = (h >> 10) & 0x1F;
	uint32_t mant = h & 0x3FF;
	uint32_t bits;
	if ( exp == 0 )
	{
		if ( mant == 0 ) { bits = sign; }
		else
		{
			// subnormal
			exp = 1;
			while ( !(mant & 0x400) ) { mant <<= 1; exp--; }
			mant &= 0x3FF;
			bits = sign | ((exp + 112) << 23) | (mant << 13);
		}
	}
	else if ( exp == 31 )
	{
		bits = sign | 0x7F800000u | (mant << 13);
	}
	else
	{
		bits = sign | ((exp + 112) << 23) | (mant << 13);
	}
	float f;
	memcpy( &f, &bits, sizeof(f) );
	return f;
}

// Scale an n-bit channel value up to 8 bits (e.g. a 5-bit blue in R5G6B5).
static inline uint8_t ScaleToU8( uint32_t value, int bits )
{
	if ( bits >= 8 ) return (uint8_t)( value >> (bits - 8) );
	uint32_t maxVal = (1u << bits) - 1u;
	return (uint8_t)( (value * 255u) / maxVal );
}

#endif // HAVE_ASTCENC

// Exact byte size of one source texel for (glFormat, glType) -- used both by
// BuildRGBABuffer() below and by the 3D "sliced" path in cglmtex.cpp to walk
// from one Z-layer of a volume texture to the next. Returns 0 for a
// combination this module doesn't know how to handle (caller must treat
// that as "can't compress, fall back").
uint32_t ASTC_GetSrcBytesPerTexel( unsigned int glFormat, unsigned int glType )
{
	switch ( glType )
	{
		case GL_UNSIGNED_INT_8_8_8_8_REV:
		case GL_UNSIGNED_INT_10_10_10_2:
			return 4;
		case GL_UNSIGNED_SHORT_4_4_4_4_REV:
		case GL_UNSIGNED_SHORT_1_5_5_5_REV:
		case GL_UNSIGNED_SHORT_5_6_5:
			return 2;
		case GL_UNSIGNED_BYTE:
			switch ( glFormat )
			{
				case GL_LUMINANCE: case GL_ALPHA:	return 1;
				case GL_LUMINANCE_ALPHA:			return 2;
				case GL_BGR: case GL_RGB:			return 3;
				case GL_RGBA: case GL_BGRA:			return 4;
				default: return 0;
			}
		case GL_UNSIGNED_SHORT:	// A16B16G16R16, always 4-component in this tree
			return 8;
		case GL_HALF_FLOAT_ARB:	// A16B16G16R16F, always 4-component in this tree
			return 8;
		case GL_FLOAT:
			switch ( glFormat )
			{
				case GL_RED: return 4;				// R32F
				case GL_RGBA: return 16;			// A32B32G32R32F
				default: return 0;
			}
		default:
			return 0;
	}
}

#if defined( HAVE_ASTCENC )

// Expands one texel at src (in whatever (glFormat,glType) layout) into
// 8-bit r,g,b,a. LDR path only -- see BuildHDRTexel() for the float path.
static inline void BuildLDRTexel( const uint8_t* src, unsigned int glFormat, unsigned int glType,
								   uint8_t* outR, uint8_t* outG, uint8_t* outB, uint8_t* outA )
{
	uint8_t r = 0, g = 0, b = 0, a = 255;

	switch ( glType )
	{
		case GL_UNSIGNED_INT_8_8_8_8_REV:
		{
			uint32_t v = *(const uint32_t*)src;
			// byte order in memory is B,G,R,A for GL_BGRA + _REV (A8R8G8B8,
			// X8R8G8B8, Q8W8V8U8 all use this).
			b = (uint8_t)( v        & 0xFF);
			g = (uint8_t)((v >>  8) & 0xFF);
			r = (uint8_t)((v >> 16) & 0xFF);
			a = (uint8_t)((v >> 24) & 0xFF);
			break;
		}
		case GL_UNSIGNED_INT_10_10_10_2:
		{
			uint32_t v = *(const uint32_t*)src;
			uint32_t c0 = (v >> 22) & 0x3FF;	// top 10 bits
			uint32_t c1 = (v >> 12) & 0x3FF;
			uint32_t c2 = (v >>  2) & 0x3FF;
			uint32_t c3 = v & 0x3;				// bottom 2 bits (alpha)
			if ( glFormat == GL_BGRA )	{ b = ScaleToU8(c0,10); g = ScaleToU8(c1,10); r = ScaleToU8(c2,10); }
			else						{ r = ScaleToU8(c0,10); g = ScaleToU8(c1,10); b = ScaleToU8(c2,10); }	// GL_RGBA
			a = ScaleToU8( c3, 2 );
			break;
		}
		case GL_UNSIGNED_SHORT_4_4_4_4_REV:
		{
			uint16_t v = *(const uint16_t*)src;
			// GL_BGRA + _REV: MSB->LSB is A,R,G,B nibbles.
			b = ScaleToU8( (v      ) & 0xF, 4 );
			g = ScaleToU8( (v >> 4 ) & 0xF, 4 );
			r = ScaleToU8( (v >> 8 ) & 0xF, 4 );
			a = ScaleToU8( (v >> 12) & 0xF, 4 );
			break;
		}
		case GL_UNSIGNED_SHORT_1_5_5_5_REV:
		{
			uint16_t v = *(const uint16_t*)src;
			// GL_BGRA + _REV: MSB->LSB is A1,R5,G5,B5.
			b = ScaleToU8( (v      ) & 0x1F, 5 );
			g = ScaleToU8( (v >> 5 ) & 0x1F, 5 );
			r = ScaleToU8( (v >> 10) & 0x1F, 5 );
			a = ( (v >> 15) & 0x1 ) ? 255 : 0;
			break;
		}
		case GL_UNSIGNED_SHORT_5_6_5:
		{
			uint16_t v = *(const uint16_t*)src;
			// GL_RGB, non-REV: MSB->LSB is R5,G6,B5. No alpha -> opaque.
			b = ScaleToU8( (v      ) & 0x1F, 5 );
			g = ScaleToU8( (v >> 5 ) & 0x3F, 6 );
			r = ScaleToU8( (v >> 11) & 0x1F, 5 );
			a = 255;
			break;
		}
		case GL_UNSIGNED_BYTE:
		{
			switch ( glFormat )
			{
				case GL_LUMINANCE:			r = g = b = src[0]; a = 255; break;
				case GL_ALPHA:				r = g = b = 0; a = src[0]; break;
				case GL_LUMINANCE_ALPHA:	r = g = b = src[0]; a = src[1]; break;
				case GL_BGR:				b = src[0]; g = src[1]; r = src[2]; a = 255; break;
				case GL_RGB:				r = src[0]; g = src[1]; b = src[2]; a = 255; break;
				case GL_BGRA:				b = src[0]; g = src[1]; r = src[2]; a = src[3]; break;
				case GL_RGBA:	default:	r = src[0]; g = src[1]; b = src[2]; a = src[3]; break;
			}
			break;
		}
		case GL_UNSIGNED_SHORT:
		{
			// A16B16G16R16, GL_RGBA, non-packed 4x16-bit. ASTC LDR blocks are
			// 8-bit precision internally regardless, so downscale by taking
			// the high byte of each 16-bit channel.
			const uint16_t* s16 = (const uint16_t*)src;
			r = (uint8_t)(s16[0] >> 8); g = (uint8_t)(s16[1] >> 8);
			b = (uint8_t)(s16[2] >> 8); a = (uint8_t)(s16[3] >> 8);
			break;
		}
	}

	*outR = r; *outG = g; *outB = b; *outA = a;
}

// Expands srcData into a freshly malloc'd RGBA buffer, either 8-bit (LDR) or
// 32-bit float (HDR) per channel. Caller frees with free().
static void* BuildRGBABuffer( const void* srcData, int width, int height,
							   unsigned int glFormat, unsigned int glType,
							   bool isHDR )
{
	const int nPixels = width * height;

	if ( !isHDR )
	{
		uint8_t* dst = (uint8_t*)malloc( (size_t)nPixels * 4 );
		const uint8_t* src8 = (const uint8_t*)srcData;
		uint32_t texelSize = ASTC_GetSrcBytesPerTexel( glFormat, glType );
		if ( texelSize == 0 ) { free( dst ); return nullptr; }	// unknown/unsupported combo -- caller falls back

		for ( int i = 0; i < nPixels; ++i )
		{
			BuildLDRTexel( src8 + (size_t)i * texelSize, glFormat, glType,
						   &dst[i*4+0], &dst[i*4+1], &dst[i*4+2], &dst[i*4+3] );
		}
		return dst;
	}
	else
	{
		float* dst = (float*)malloc( (size_t)nPixels * 4 * sizeof(float) );

		if ( glType == GL_HALF_FLOAT_ARB )		// A16B16G16R16F, 4-component
		{
			const uint16_t* src16 = (const uint16_t*)srcData;
			for ( int i = 0; i < nPixels; ++i )
			{
				dst[i*4+0] = HalfToFloat( src16[i*4+0] );
				dst[i*4+1] = HalfToFloat( src16[i*4+1] );
				dst[i*4+2] = HalfToFloat( src16[i*4+2] );
				dst[i*4+3] = HalfToFloat( src16[i*4+3] );
			}
		}
		else if ( glFormat == GL_RED )			// R32F, 1-component -> replicate to RGB, A=1
		{
			const float* src32 = (const float*)srcData;
			for ( int i = 0; i < nPixels; ++i )
			{
				dst[i*4+0] = dst[i*4+1] = dst[i*4+2] = src32[i];
				dst[i*4+3] = 1.0f;
			}
		}
		else									// A32B32G32R32F, 4-component GL_FLOAT
		{
			memcpy( dst, srcData, (size_t)nPixels * 4 * sizeof(float) );
		}
		return dst;
	}
}

#endif // HAVE_ASTCENC

// ---------------------------------------------------------------------------
// Main entry point
// ---------------------------------------------------------------------------
bool ASTC_CompressTexture(
	const void* srcData,
	int width,
	int height,
	unsigned int srcGLFormat,
	unsigned int srcGLType,
	bool isHDR,
	int blockW,
	int blockH,
	int qualityPreset,
	ASTCEncodeResult* outResult )
{
#if !defined( HAVE_ASTCENC )
	// astc-encoder isn't vendored/enabled in this build -- caller must fall
	// back to the normal uncompressed upload path. See astc_texcompress.h.
	(void)srcData; (void)width; (void)height; (void)srcGLFormat; (void)srcGLType;
	(void)isHDR; (void)blockW; (void)blockH; (void)qualityPreset; (void)outResult;
	return false;
#else
	if ( !srcData || width <= 0 || height <= 0 || !outResult )
		return false;

	void* rgba = BuildRGBABuffer( srcData, width, height, srcGLFormat, srcGLType, isHDR );
	if ( !rgba )
		return false;

	astcenc_profile profile = isHDR ? ASTCENC_PRF_HDR : ASTCENC_PRF_LDR;

	float quality = ASTCENC_PRE_MEDIUM;
	if ( qualityPreset <= 10 )      quality = ASTCENC_PRE_FASTEST;
	else if ( qualityPreset <= 35 ) quality = ASTCENC_PRE_FAST;
	else if ( qualityPreset <= 65 ) quality = ASTCENC_PRE_MEDIUM;
	else if ( qualityPreset <= 90 ) quality = ASTCENC_PRE_THOROUGH;
	else                            quality = ASTCENC_PRE_EXHAUSTIVE;

	astcenc_config config;
	astcenc_error status = astcenc_config_init(
		profile, blockW, blockH, 1, quality, 0, &config );
	if ( status != ASTCENC_SUCCESS )
	{
		free( rgba );
		return false;
	}

	astcenc_context* context = nullptr;
	status = astcenc_context_alloc( &config, 1 /*thread count*/, &context );
	if ( status != ASTCENC_SUCCESS )
	{
		free( rgba );
		return false;
	}

	astcenc_image image;
	image.dim_x = width;
	image.dim_y = height;
	image.dim_z = 1;
	image.data_type = isHDR ? ASTCENC_TYPE_F32 : ASTCENC_TYPE_U8;
	uint8_t* sliceArray[1] = { (uint8_t*)rgba };
	image.data = (void**)sliceArray;

	astcenc_swizzle swizzle { ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A };

	int xBlocks = (width  + blockW - 1) / blockW;
	int yBlocks = (height + blockH - 1) / blockH;
	size_t compSize = (size_t)xBlocks * yBlocks * 16; // ASTC blocks are always 16 bytes

	uint8_t* compData = (uint8_t*)malloc( compSize );
	if ( !compData )
	{
		astcenc_context_free( context );
		free( rgba );
		return false;
	}

	status = astcenc_compress_image( context, &image, &swizzle, compData, compSize, 0 );

	astcenc_context_free( context );
	free( rgba );

	if ( status != ASTCENC_SUCCESS )
	{
		free( compData );
		return false;
	}

	outResult->m_pData = compData;
	outResult->m_nDataSize = (uint32_t)compSize;
	outResult->m_glInternalFormat = GLBlockSizeToInternalFormat( blockW, blockH );
	outResult->m_blockW = blockW;
	outResult->m_blockH = blockH;
	outResult->m_profile = isHDR ? kASTCProfile_HDR : kASTCProfile_LDR;
	return true;
#endif
}

void ASTC_FreeResult( ASTCEncodeResult* result )
{
	if ( result && result->m_pData )
	{
		free( result->m_pData );
		result->m_pData = nullptr;
		result->m_nDataSize = 0;
	}
}

// Convenience used by the WriteTexels hook: picks the configured block size
// for the given HDR-ness straight from the convars.
void ASTC_GetConfiguredBlockSize( bool isHDR, int* outW, int* outH )
{
	ParseBlockSize( isHDR ? gl_astc_block_hdr.GetString() : gl_astc_block_ldr.GetString(), outW, outH );
}

// ---------------------------------------------------------------------------
// DXT1/DXT3/DXT5 -> RGBA8 -> ASTC LDR
//
// decompress.h's DecompressBlockDXTn() functions decompress ONE 4x4 block
// into a caller-owned RGBA8 image buffer (packed r|g<<8|b<<16|a<<24 per
// texel -- see decompress.c's PackRGBA -- which is exactly tightly-packed
// GL_RGBA/GL_UNSIGNED_BYTE byte order in memory). This loops every block of
// the source image and calls the right decompressor for the format, then
// hands the fully-decompressed image to the normal ASTC_CompressTexture().
// ---------------------------------------------------------------------------
#if defined( HAVE_ASTCENC )
static uint32_t* DecompressDXTToRGBA( const uint8_t* src, int width, int height, int dxtD3DFormat )
{
	uint32_t* image = (uint32_t*)malloc( (size_t)width * height * sizeof(uint32_t) );
	if ( !image )
		return nullptr;

	int blockBytes = ( dxtD3DFormat == D3DFMT_DXT1 ) ? 8 : 16;	// DXT1 = 8 bytes/block, DXT3/DXT5 = 16 bytes/block
	int blocksWide = width / 4;
	int simpleAlpha = 0, complexAlpha = 0;

	for ( int by = 0; by < height; by += 4 )
	{
		for ( int bx = 0; bx < width; bx += 4 )
		{
			int blockIndex = (by / 4) * blocksWide + (bx / 4);
			const uint8_t* blockPtr = src + (size_t)blockIndex * blockBytes;

			switch ( dxtD3DFormat )
			{
				case D3DFMT_DXT1:
					DecompressBlockDXT1( bx, by, width, blockPtr, 0, &simpleAlpha, &complexAlpha, image );
					break;
				case D3DFMT_DXT3:
					DecompressBlockDXT3( bx, by, width, blockPtr, 0, &simpleAlpha, &complexAlpha, image );
					break;
				case D3DFMT_DXT5:
					DecompressBlockDXT5( bx, by, width, blockPtr, 0, &simpleAlpha, &complexAlpha, image );
					break;
			}
		}
	}

	return image;
}
#endif // HAVE_ASTCENC

bool ASTC_CompressDXTToASTC(
	const void* dxtSrcData,
	int width,
	int height,
	int dxtD3DFormat,
	int qualityPreset,
	ASTCEncodeResult* outResult )
{
#if !defined( HAVE_ASTCENC )
	(void)dxtSrcData; (void)width; (void)height; (void)dxtD3DFormat; (void)qualityPreset; (void)outResult;
	return false;
#else
	if ( !dxtSrcData || !outResult || !ASTC_IsDXTFormat( dxtD3DFormat ) )
		return false;

	// DXT is defined in 4x4 blocks -- refuse anything ragged rather than
	// risk an out-of-bounds decompress write. Mip levels this small are a
	// tiny minority of a texture's total data and just stay DXT-compressed
	// (still compressed on the GPU, just not re-encoded to ASTC).
	if ( (width % 4) != 0 || (height % 4) != 0 || width <= 0 || height <= 0 )
		return false;

	uint32_t* rgba = DecompressDXTToRGBA( (const uint8_t*)dxtSrcData, width, height, dxtD3DFormat );
	if ( !rgba )
		return false;

	int blockW, blockH;
	ASTC_GetConfiguredBlockSize( false /*DXT source is always LDR*/, &blockW, &blockH );

	bool ok = ASTC_CompressTexture( rgba, width, height, GL_RGBA, GL_UNSIGNED_BYTE,
									 false /*isHDR*/, blockW, blockH, qualityPreset, outResult );
	free( rgba );
	return ok;
#endif
}

// ---------------------------------------------------------------------------
// GL_TEXTURE_3D (volume textures) via GL_KHR_texture_compression_astc_sliced_3d
//
// That extension does NOT require real 3D ASTC block modes -- it reuses the
// existing 2D GL_COMPRESSED_RGBA_ASTC_WxH_KHR tokens with glCompressedTex-
// Image3D(), and simply requires each Z-layer to be an independent, complete
// 2D ASTC-compressed image, concatenated back to back in upload order. So
// this just runs the normal 2D encoder once per Z-layer and concatenates the
// results -- no separate 3D encoder path needed.
// ---------------------------------------------------------------------------
bool ASTC_CompressTexture3DSliced(
	const void* srcData,
	int width,
	int height,
	int depth,
	unsigned int srcGLFormat,
	unsigned int srcGLType,
	bool isHDR,
	int blockW,
	int blockH,
	int qualityPreset,
	ASTCEncodeResult* outResult )
{
#if !defined( HAVE_ASTCENC )
	(void)srcData; (void)width; (void)height; (void)depth; (void)srcGLFormat; (void)srcGLType;
	(void)isHDR; (void)blockW; (void)blockH; (void)qualityPreset; (void)outResult;
	return false;
#else
	if ( !srcData || width <= 0 || height <= 0 || depth <= 0 || !outResult )
		return false;

	uint32_t texelSize = ASTC_GetSrcBytesPerTexel( srcGLFormat, srcGLType );
	if ( texelSize == 0 )
		return false;	// unknown (format,type) combo -- caller falls back

	int xBlocks = (width  + blockW - 1) / blockW;
	int yBlocks = (height + blockH - 1) / blockH;
	size_t perSliceCompSize = (size_t)xBlocks * yBlocks * 16;	// ASTC blocks are always 16 bytes
	size_t totalCompSize = perSliceCompSize * (size_t)depth;

	uint8_t* allSlices = (uint8_t*)malloc( totalCompSize );
	if ( !allSlices )
		return false;

	const uint8_t* srcBase = (const uint8_t*)srcData;
	size_t sliceByteStride = (size_t)width * height * texelSize;

	for ( int z = 0; z < depth; ++z )
	{
		ASTCEncodeResult sliceResult;
		bool ok = ASTC_CompressTexture( srcBase + (size_t)z * sliceByteStride, width, height,
										 srcGLFormat, srcGLType, isHDR, blockW, blockH,
										 qualityPreset, &sliceResult );
		if ( !ok || sliceResult.m_nDataSize != perSliceCompSize )
		{
			if ( ok ) ASTC_FreeResult( &sliceResult );
			free( allSlices );
			return false;
		}
		memcpy( allSlices + (size_t)z * perSliceCompSize, sliceResult.m_pData, perSliceCompSize );
		ASTC_FreeResult( &sliceResult );
	}

	outResult->m_pData = allSlices;
	outResult->m_nDataSize = (uint32_t)totalCompSize;
	outResult->m_glInternalFormat = GLBlockSizeToInternalFormat( blockW, blockH );
	outResult->m_blockW = blockW;
	outResult->m_blockH = blockH;
	outResult->m_profile = isHDR ? kASTCProfile_HDR : kASTCProfile_LDR;
	return true;
#endif
}

// Same idea as ASTC_CompressTexture3DSliced(), but the source is a volume
// texture whose Z-layers are each an independent DXT1/3/5-compressed 2D
// image (decompress each layer, then encode+concatenate as above).
bool ASTC_CompressDXTToASTC3DSliced(
	const void* dxtSrcData,
	int width,
	int height,
	int depth,
	int dxtD3DFormat,
	int qualityPreset,
	ASTCEncodeResult* outResult )
{
#if !defined( HAVE_ASTCENC )
	(void)dxtSrcData; (void)width; (void)height; (void)depth; (void)dxtD3DFormat;
	(void)qualityPreset; (void)outResult;
	return false;
#else
	if ( !dxtSrcData || !outResult || !ASTC_IsDXTFormat( dxtD3DFormat ) )
		return false;
	if ( (width % 4) != 0 || (height % 4) != 0 || width <= 0 || height <= 0 || depth <= 0 )
		return false;

	int blockW, blockH;
	ASTC_GetConfiguredBlockSize( false, &blockW, &blockH );

	int xBlocks = (width  + blockW - 1) / blockW;
	int yBlocks = (height + blockH - 1) / blockH;
	size_t perSliceCompSize = (size_t)xBlocks * yBlocks * 16;
	size_t totalCompSize = perSliceCompSize * (size_t)depth;

	uint8_t* allSlices = (uint8_t*)malloc( totalCompSize );
	if ( !allSlices )
		return false;

	int blockBytes = ( dxtD3DFormat == D3DFMT_DXT1 ) ? 8 : 16;
	size_t dxtBytesPerSlice = (size_t)(width / 4) * (height / 4) * blockBytes;
	const uint8_t* srcBase = (const uint8_t*)dxtSrcData;

	for ( int z = 0; z < depth; ++z )
	{
		ASTCEncodeResult sliceResult;
		bool ok = ASTC_CompressDXTToASTC( srcBase + (size_t)z * dxtBytesPerSlice, width, height,
										   dxtD3DFormat, qualityPreset, &sliceResult );
		if ( !ok || sliceResult.m_nDataSize != perSliceCompSize )
		{
			if ( ok ) ASTC_FreeResult( &sliceResult );
			free( allSlices );
			return false;
		}
		memcpy( allSlices + (size_t)z * perSliceCompSize, sliceResult.m_pData, perSliceCompSize );
		ASTC_FreeResult( &sliceResult );
	}

	outResult->m_pData = allSlices;
	outResult->m_nDataSize = (uint32_t)totalCompSize;
	outResult->m_glInternalFormat = GLBlockSizeToInternalFormat( blockW, blockH );
	outResult->m_blockW = blockW;
	outResult->m_blockH = blockH;
	outResult->m_profile = kASTCProfile_LDR;
	return true;
#endif
}
