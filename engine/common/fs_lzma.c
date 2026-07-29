/* fs_lzma.c -- raw-LZMA1 decode wrapper (zip compression method 14, as used by
   Source/Strata packed BSP pakfiles, and 7-zip).  Kept as its own translation unit
   so the public-domain LZMA SDK's type universe (Byte/UInt32/SizeT/Bool/True/False,
   from 7zTypes.h) never leaks into the main filesystem sources.  The only symbol
   exported is FS_LZMA_DecodeRaw(); fs_zip.c calls it with plain C types. */

#include <stdlib.h>
#include "LzmaDec.h"

static void *FS_LzmaAlloc(ISzAllocPtr p, size_t size) { (void)p; return malloc(size); }
static void  FS_LzmaFree (ISzAllocPtr p, void *a)     { (void)p; free(a); }
static const ISzAlloc fs_lzma_alloc = { FS_LzmaAlloc, FS_LzmaFree };

/* Decode a raw LZMA1 stream that expands to exactly dstlen bytes.  props points at
   the LZMA property bytes (propslen of them, normally 5).  The uncompressed size is
   known from the zip central directory, so we use LZMA_FINISH_ANY and simply stop
   once dstlen bytes have been produced (the stream may or may not carry an EOS mark).
   Returns 1 on success (exactly dstlen bytes produced), 0 on any failure.
   All buffers are caller-owned. */
int FS_LZMA_DecodeRaw(unsigned char *dst, size_t dstlen,
                      const unsigned char *src, size_t srclen,
                      const unsigned char *props, unsigned propslen)
{
	SizeT destLen = dstlen;
	SizeT srcLen  = srclen;
	ELzmaStatus status;
	SRes res;

	if (!dst || !src || !props || dstlen == 0 || propslen < LZMA_PROPS_SIZE)
		return 0;

	res = LzmaDecode(dst, &destLen, src, &srcLen, props, propslen,
	                 LZMA_FINISH_ANY, &status, &fs_lzma_alloc);

	return (res == SZ_OK && destLen == dstlen);
}
