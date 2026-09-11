//========= ASTC runtime recompression feature (added on top of Valve TOGL) =====//
//
// astc_texcompress.h
//
// Purpose
// -------
// MANDATORY texture compression: every texture format TOGL uploads --
// uncompressed color formats (ARGB/RGBA/luminance/single/dual-channel,
// normalized or float) AND already-block-compressed S3TC/DXT1/DXT3/DXT5
// textures (decompressed first, then re-encoded) -- is converted to ASTC
// at upload time. This is not opt-in: WriteTexels() in cglmtex.cpp always
// attempts it for every eligible format; only formats that are physically
// impossible to store as ASTC on the GPU (render targets, multisampled
// surfaces, depth/stencil) are skipped, because OpenGL has no such thing
// as "render to a compressed texture" -- that's a hardware/API limitation,
// not a policy choice.
//
// ASTC has two wire-compatible "profiles" that use the *same* block layout
// and the *same* GL internal-format enums (GL_COMPRESSED_RGBA_ASTC_*_KHR) --
// the difference is entirely in how the 128-bit blocks are produced and in
// which decode profile the GPU/driver is told to use:
//
//   * LDR (Low Dynamic Range)  - values are clamped/normalized to [0,1] and
//                                decode as regular UNORM colors. Used for
//                                every normalized/byte format (_A8R8G8B8,
//                                _X8R8G8B8, _R8G8B8, _R5G6B5, _A1R5G5B5,
//                                _A4R4G4B4, _Q8W8V8U8,
//                                _A16B16G16R16, _L8, _A8L8, _A8, _L16, ...)
//                                and for decompressed DXT1/DXT3/DXT5 source
//                                data.
//
//   * HDR (High Dynamic Range) - values may exceed 1.0 and decode as floats.
//                                Used for every floating point / half-float
//                                format (_A16B16G16R16F, _A32B32G32R32F,
//                                _R32F, _R16F, _G16R16F, _G32R32F).
//
// This module decides LDR vs HDR automatically from the source D3DFORMAT,
// and hands the actual block encoding off to ARM's reference "astcenc"
// library (Apache-2.0, https://github.com/ARM-software/astc-encoder;
// packaged on Debian/Ubuntu as `libastcenc-dev`).
//
// If astcenc isn't compiled in (HAVE_ASTCENC not defined -- e.g. the dev
// package isn't installed), ASTC_CompressTexture() safely returns false and
// the caller falls back to the original uncompressed/DXT upload path. This
// is the ONLY case where a texture doesn't end up as ASTC -- it is a build
// environment fallback, not a runtime switch: there is no ConVar to turn
// mandatory compression off.
//
//===============================================================================

#ifndef ASTC_TEXCOMPRESS_H
#define ASTC_TEXCOMPRESS_H

#pragma once

#include <stdint.h>
#include "tier1/convar.h"	// needed for the extern ConVar declarations below (callers use .GetBool()/.GetInt()/.GetString())

// ---------------------------------------------------------------------------
// GL enums for ASTC (from KHR_texture_compression_astc_ldr / _hdr).
// Defined here defensively in case the local GL header set predates them --
// remove the #ifndef guards if your gl headers already provide these.
// ---------------------------------------------------------------------------
#ifndef GL_COMPRESSED_RGBA_ASTC_4x4_KHR
#define GL_COMPRESSED_RGBA_ASTC_4x4_KHR    0x93B0
#define GL_COMPRESSED_RGBA_ASTC_5x4_KHR    0x93B1
#define GL_COMPRESSED_RGBA_ASTC_5x5_KHR    0x93B2
#define GL_COMPRESSED_RGBA_ASTC_6x5_KHR    0x93B3
#define GL_COMPRESSED_RGBA_ASTC_6x6_KHR    0x93B4
#define GL_COMPRESSED_RGBA_ASTC_8x5_KHR    0x93B5
#define GL_COMPRESSED_RGBA_ASTC_8x6_KHR    0x93B6
#define GL_COMPRESSED_RGBA_ASTC_8x8_KHR    0x93B7
#define GL_COMPRESSED_RGBA_ASTC_10x5_KHR   0x93B8
#define GL_COMPRESSED_RGBA_ASTC_10x6_KHR   0x93B9
#define GL_COMPRESSED_RGBA_ASTC_10x8_KHR   0x93BA
#define GL_COMPRESSED_RGBA_ASTC_10x10_KHR  0x93BB
#define GL_COMPRESSED_RGBA_ASTC_12x10_KHR  0x93BC
#define GL_COMPRESSED_RGBA_ASTC_12x12_KHR  0x93BD
// sRGB variants (LDR-only; there is no sRGB+HDR combination in the spec)
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4_KHR   0x93D0
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_5x4_KHR   0x93D1
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_5x5_KHR   0x93D2
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x5_KHR   0x93D3
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x6_KHR   0x93D4
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x5_KHR   0x93D5
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x6_KHR   0x93D6
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x8_KHR   0x93D7
#endif

// Note: HDR profile textures are uploaded with the *same* enums above --
// KHR_texture_compression_astc_hdr re-uses the LDR tokens. The driver knows
// it is HDR data purely because the astcenc encode profile that produced the
// bitstream (ASTCENC_PRF_HDR) is different; there is no separate GL enum.

enum EASTCProfile
{
	kASTCProfile_LDR = 0,	// clamped [0,1], UNORM decode
	kASTCProfile_HDR = 1,	// unclamped, FP16 decode (needs GL_KHR_texture_compression_astc_hdr)
};

struct ASTCEncodeResult
{
	void*		m_pData;			// caller must free with ASTC_FreeResult()
	uint32_t	m_nDataSize;		// size in bytes of m_pData
	uint32_t	m_glInternalFormat;	// GL_COMPRESSED_RGBA_ASTC_WxH_KHR to pass to glCompressedTexImage2D
	int			m_blockW;
	int			m_blockH;
	EASTCProfile m_profile;			// LDR or HDR -- informational, also implied by m_glInternalFormat + profile used to encode
};

// Returns true if this D3DFORMAT is an uncompressed color format this
// feature can transcode straight to ASTC (any plain ARGB/RGBA/luminance/
// single/dual-channel layout, normalized or float). Covers essentially
// every non-block-compressed, non-depth color format TOGL uploads.
// fmt is passed as int here so this header doesn't need d3d9types.h; the
// .cpp does the real D3DFORMAT switch.
bool ASTC_IsEligibleFormat( int d3dFormat );

// Returns true if this D3DFORMAT is an already block-compressed S3TC/DXT
// format (_DXT1, _DXT3, _DXT5). These are handled by a separate path that
// decompresses the block data to RGBA8 first (see decompress.h) and then
// re-encodes it as ASTC LDR -- DXT is always 8-bit/normalized, never HDR.
bool ASTC_IsDXTFormat( int d3dFormat );

// Returns true if the format's payload is floating point / half-float and
// therefore must be encoded with the ASTC HDR profile instead of LDR.
bool ASTC_IsHDRFormat( int d3dFormat );

// Encodes an RGBA (or RGBA-compatible) source image into ASTC blocks.
//   srcData        - tightly packed source texels, top row first
//   width, height  - texel dimensions (need not be a multiple of the block size;
//                    astcenc pads internally)
//   srcGLFormat    - GL_RGBA, GL_BGRA, GL_RGB, ... describing srcData's channel order
//   srcGLType      - GL_UNSIGNED_BYTE, GL_UNSIGNED_INT_8_8_8_8_REV, GL_HALF_FLOAT_ARB, GL_FLOAT, ...
//   isHDR          - from ASTC_IsHDRFormat()
//   blockW, blockH - ASTC block footprint, e.g. 4,4 (highest quality/least
//                    compression) up to 12,12 (most compression). 4x4 and
//                    6x6 are good general purpose defaults for LDR/HDR
//                    respectively.
//   qualityPreset  - 0=fastest .. 100=thorough (maps to ASTCENC_PRE_FASTEST..THOROUGH)
//   outResult      - filled in on success
// Returns false (and leaves *outResult untouched) if astcenc isn't compiled
// in (HAVE_ASTCENC not defined), the format isn't supported, or encoding
// failed for any reason -- callers must fall back to the normal uncompressed
// upload path in that case.
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
	ASTCEncodeResult* outResult );

// Same as ASTC_CompressTexture(), but the source is DXT1/DXT3/DXT5-
// compressed block data (dxtD3DFormat must satisfy ASTC_IsDXTFormat()).
// Internally decompresses to RGBA8 (via decompress.h) then encodes ASTC
// LDR. width/height must each be a multiple of 4 (standard DXT block
// requirement) -- returns false otherwise so the caller can fall back to
// uploading the original DXT bytes unchanged.
bool ASTC_CompressDXTToASTC(
	const void* dxtSrcData,
	int width,
	int height,
	int dxtD3DFormat,
	int qualityPreset,
	ASTCEncodeResult* outResult );

void ASTC_FreeResult( ASTCEncodeResult* result );

// Exact byte size of one source texel for (glFormat, glType) -- 0 if this
// module doesn't recognize the combination. Used to walk between Z-layers
// of a volume (GL_TEXTURE_3D) texture for the sliced-3D functions below.
uint32_t ASTC_GetSrcBytesPerTexel( unsigned int glFormat, unsigned int glType );

// GL_TEXTURE_3D (volume texture) support via
// GL_KHR_texture_compression_astc_sliced_3d: each Z-layer is compressed as
// an independent standard 2D ASTC image (same block dims/internal format as
// the 2D path) and the results are concatenated -- that's what "sliced 3d"
// means, no true 3D block mode is needed. srcData must be `depth` full
// 2D images of (width x height) back to back, in whatever (srcGLFormat,
// srcGLType) layout ASTC_CompressTexture() accepts.
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
	ASTCEncodeResult* outResult );

// Same as ASTC_CompressTexture3DSliced(), but the source is `depth` DXT1/3/5
// -compressed 2D layers back to back (a block-compressed volume texture).
bool ASTC_CompressDXTToASTC3DSliced(
	const void* dxtSrcData,
	int width,
	int height,
	int depth,
	int dxtD3DFormat,
	int qualityPreset,
	ASTCEncodeResult* outResult );

// Reads gl_astc_block_ldr / gl_astc_block_hdr and parses them into block
// dimensions for the given profile. Used by the WriteTexels upload hook.
void ASTC_GetConfiguredBlockSize( bool isHDR, int* outW, int* outH );

// Convars (defined in astc_texcompress.cpp). Declared extern here so any
// other translation unit -- cglmtex.cpp included -- can reference them
// after including this header; without this each .cpp only sees its own
// copy and the linker/compiler has no idea these ConVars exist.
//
// NOTE: ASTC compression is now MANDATORY for every eligible format --
// there is deliberately no "master enable" ConVar to opt out of it anymore.
// gl_astc_debug_disable exists purely as a debug/bisect escape hatch
// (defaults OFF) for tracking down a driver or content bug; it is not meant
// to be shipped set to 1.
extern ConVar gl_astc_debug_disable;	// 0/1, default 0 -- debug-only kill switch, do not ship set to 1
extern ConVar gl_astc_block_ldr;		// "4x4" / "5x5" / "6x6" / "8x8" ... - block size used for LDR (8-bit) sources
extern ConVar gl_astc_block_hdr;		// "4x4" / "5x5" / "6x6" / "8x8" ... - block size used for HDR (float) sources
extern ConVar gl_astc_quality;			// 0-100 - encoder quality/speed tradeoff

#endif // ASTC_TEXCOMPRESS_H
