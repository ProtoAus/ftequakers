/*
 * BLAKE2b-256 (RFC 7693), exposed as an FTE hashfunc_t.
 *
 * Derived from the BLAKE2 reference implementation by Samuel Neves, which is released
 * under CC0 / public domain -- GPLv2-compatible. (sha2.c here is LGPL-from-Libgcrypt,
 * so a third-party hash in common/ is already precedent.)
 *
 * WHY THIS EXISTS: the Quakers content-distribution tree names every object by its
 * BLAKE2b-256 hash (objects/<hh>/<hash>), and the manifest declares
 * "hash_algo": "blake2b-256". The engine's in-game updater has to reproduce those hashes
 * byte-for-byte or nothing verifies. The hash can never be swapped for one FTE already
 * has: object names *are* the hashes, so changing it renames all ~4,100 R2 objects and
 * forces a 6.3 GB re-download for every player.
 *
 * MUST MATCH RustCrypto's Blake2b<U32> (launcher/src/hashing.rs) exactly. That is
 * BLAKE2b with the parameter block's digest_length field set to 32 -- i.e.
 * h[0] ^= 0x01010020 -- and NOT a BLAKE2b-512 digest truncated to 32 bytes. The two
 * differ from the first compression, so getting it wrong makes every single file fail
 * verification forever, which on the wire looks exactly like a corrupt download.
 *
 * Endianness is done with explicit byte-shuffling load64/store64 rather than FTE's
 * LittleU64, because journal replay hashes staged objects from the tail of
 * COM_InitFilesystem(), and the runtime byte-order setup is not guaranteed to have run.
 */

#include "quakedef.h"

#define BLAKE2B_BLOCKBYTES	128
#define BLAKE2B_OUTBYTES	32

typedef struct
{
	quint64_t h[8];
	quint64_t t[2];					/*message byte counter, 128bit*/
	qbyte buf[BLAKE2B_BLOCKBYTES];
	size_t buflen;
} BLAKE2B_CONTEXT;

static const quint64_t blake2b_IV[8] =
{
	0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull,
	0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
	0x510e527fade682d1ull, 0x9b05688c2b3e6c1full,
	0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull
};

static const qbyte blake2b_sigma[12][16] =
{
	{  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
	{ 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 },
	{ 11,  8, 12,  0,  5,  2, 15, 13, 10, 14,  3,  6,  7,  1,  9,  4 },
	{  7,  9,  3,  1, 13, 12, 11, 14,  2,  6,  5, 10,  4,  0, 15,  8 },
	{  9,  0,  5,  7,  2,  4, 10, 15, 14,  1, 11, 12,  6,  8,  3, 13 },
	{  2, 12,  6, 10,  0, 11,  8,  3,  4, 13,  7,  5, 15, 14,  1,  9 },
	{ 12,  5,  1, 15, 14, 13,  4, 10,  0,  7,  6,  3,  9,  2,  8, 11 },
	{ 13, 11,  7, 14, 12,  1,  3,  9,  5,  0, 15,  4,  8,  6,  2, 10 },
	{  6, 15, 14,  9, 11,  3,  0,  8, 12,  2, 13,  7,  1,  4, 10,  5 },
	{ 10,  2,  8,  4,  7,  6,  1,  5, 15, 11,  9, 14,  3, 12, 13,  0 },
	{  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
	{ 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 }
};

static quint64_t blake2b_load64(const qbyte *src)
{
	return	((quint64_t)src[0] <<  0) | ((quint64_t)src[1] <<  8) |
			((quint64_t)src[2] << 16) | ((quint64_t)src[3] << 24) |
			((quint64_t)src[4] << 32) | ((quint64_t)src[5] << 40) |
			((quint64_t)src[6] << 48) | ((quint64_t)src[7] << 56);
}
static void blake2b_store64(qbyte *dst, quint64_t w)
{
	dst[0] = (qbyte)(w >>  0);	dst[1] = (qbyte)(w >>  8);
	dst[2] = (qbyte)(w >> 16);	dst[3] = (qbyte)(w >> 24);
	dst[4] = (qbyte)(w >> 32);	dst[5] = (qbyte)(w >> 40);
	dst[6] = (qbyte)(w >> 48);	dst[7] = (qbyte)(w >> 56);
}
#define blake2b_rotr64(w,c) (((w) >> (c)) | ((w) << (64-(c))))

#define G(r,i,a,b,c,d)								\
	do {											\
		a = a + b + m[blake2b_sigma[r][2*i+0]];		\
		d = blake2b_rotr64(d ^ a, 32);				\
		c = c + d;									\
		b = blake2b_rotr64(b ^ c, 24);				\
		a = a + b + m[blake2b_sigma[r][2*i+1]];		\
		d = blake2b_rotr64(d ^ a, 16);				\
		c = c + d;									\
		b = blake2b_rotr64(b ^ c, 63);				\
	} while(0)

#define ROUND(r)									\
	do {											\
		G(r,0,v[ 0],v[ 4],v[ 8],v[12]);				\
		G(r,1,v[ 1],v[ 5],v[ 9],v[13]);				\
		G(r,2,v[ 2],v[ 6],v[10],v[14]);				\
		G(r,3,v[ 3],v[ 7],v[11],v[15]);				\
		G(r,4,v[ 0],v[ 5],v[10],v[15]);				\
		G(r,5,v[ 1],v[ 6],v[11],v[12]);				\
		G(r,6,v[ 2],v[ 7],v[ 8],v[13]);				\
		G(r,7,v[ 3],v[ 4],v[ 9],v[14]);				\
	} while(0)

static void blake2b_compress(BLAKE2B_CONTEXT *S, const qbyte *block, qboolean last)
{
	quint64_t m[16];
	quint64_t v[16];
	int i;

	for (i = 0; i < 16; i++)
		m[i] = blake2b_load64(block + i*sizeof(m[i]));
	for (i = 0; i < 8; i++)
		v[i] = S->h[i];
	for (i = 0; i < 8; i++)
		v[8+i] = blake2b_IV[i];

	v[12] ^= S->t[0];
	v[13] ^= S->t[1];
	if (last)
		v[14] = ~v[14];	/*f[0]; there is no tree mode here so f[1] stays 0*/

	ROUND( 0); ROUND( 1); ROUND( 2); ROUND( 3);
	ROUND( 4); ROUND( 5); ROUND( 6); ROUND( 7);
	ROUND( 8); ROUND( 9); ROUND(10); ROUND(11);

	for (i = 0; i < 8; i++)
		S->h[i] ^= v[i] ^ v[8+i];
}

/*count the bytes we are about to feed into a compression. the 128bit counter must be
  bumped BEFORE compressing, and it counts bytes, not blocks.*/
static void blake2b_increment(BLAKE2B_CONTEXT *S, quint64_t inc)
{
	S->t[0] += inc;
	if (S->t[0] < inc)
		S->t[1]++;
}

static void blake2b_256_init(void *context)
{
	BLAKE2B_CONTEXT *S = context;
	int i;
	for (i = 0; i < 8; i++)
		S->h[i] = blake2b_IV[i];
	/*the parameter block, xored in whole. unkeyed sequential BLAKE2b leaves every field
	  0 except digest_length(1) and fanout/depth(1 each), so only the first word differs
	  from the IV: 0x01010000 | outlen. THIS is what makes blake2b-256 != truncated -512.*/
	S->h[0] ^= 0x01010000ull ^ (quint64_t)BLAKE2B_OUTBYTES;
	S->t[0] = S->t[1] = 0;
	S->buflen = 0;
}

static void blake2b_256_process(void *context, const void *data, size_t datasize)
{
	BLAKE2B_CONTEXT *S = context;
	const qbyte *in = data;

	if (!datasize)
		return;

	/*strictly greater-than: a block is only compressed once we know more data follows it,
	  because the final block must be compressed with the last-block flag set. Buffering
	  exactly 128 bytes and flushing eagerly would hash a full-block message wrong.*/
	if (S->buflen + datasize > BLAKE2B_BLOCKBYTES)
	{
		size_t fill = BLAKE2B_BLOCKBYTES - S->buflen;
		memcpy(S->buf + S->buflen, in, fill);
		blake2b_increment(S, BLAKE2B_BLOCKBYTES);
		blake2b_compress(S, S->buf, false);
		S->buflen = 0;
		in += fill;
		datasize -= fill;

		while (datasize > BLAKE2B_BLOCKBYTES)
		{
			blake2b_increment(S, BLAKE2B_BLOCKBYTES);
			blake2b_compress(S, in, false);
			in += BLAKE2B_BLOCKBYTES;
			datasize -= BLAKE2B_BLOCKBYTES;
		}
	}
	memcpy(S->buf + S->buflen, in, datasize);
	S->buflen += datasize;
}

static void blake2b_256_finish(qbyte *digest, void *context)
{
	BLAKE2B_CONTEXT *S = context;
	int i;

	blake2b_increment(S, S->buflen);
	memset(S->buf + S->buflen, 0, BLAKE2B_BLOCKBYTES - S->buflen);
	blake2b_compress(S, S->buf, true);

	for (i = 0; i < BLAKE2B_OUTBYTES/8; i++)
		blake2b_store64(digest + i*8, S->h[i]);
}

hashfunc_t hash_blake2b_256 =
{
	BLAKE2B_OUTBYTES,
	sizeof(BLAKE2B_CONTEXT),
	blake2b_256_init,
	blake2b_256_process,
	blake2b_256_finish
};

#undef G
#undef ROUND
