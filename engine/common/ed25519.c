/*
FTESurf Patch 417 -- Ed25519 (RFC 8032), for the client's run receipt.

WHY THE ENGINE CARRIES ITS OWN.  The receipt is signed on the PLAYER's machine
with a key that never leaves it, so the primitive has to exist wherever the
client runs.  The engine's TLS backends cannot supply it: gnutls can sign and is
Linux-only here, SChannel (net_ssl_winsspi.c) cannot sign at all, and Windows
CNG has no Ed25519.  Two OS backends would be more code than one portable file,
and neither would run in a headless test.

WHAT IS AND IS NOT HERE.  Group arithmetic only: SHA-512 is the engine's own
(common/sha2.c, via CalcHash), so this file is the curve and nothing else.  The
arrangement is the compact "tweetnacl" one -- sixteen 16-bit limbs in a 64-bit
int, schoolbook multiply, constant-time conditional swap -- chosen because it is
short enough that somebody can read all of it, which ref10 is not.  It signs
once per run, so speed is irrelevant.

HOW IT IS CHECKED, because "I wrote a curve implementation" is worth nothing on
its own.  tools/ed25519check.py drives `ed25519_selftest` and compares against
python-cryptography's implementation on random keys and messages, both
directions: keys derived here must match theirs from the same seed, signatures
made here must verify there, and signatures made there must verify here.  A
wrong curve constant fails every one of those on the first vector.

SIDE CHANNELS: the conditional swap and the compare are constant-time; the
field arithmetic is not branch-free in `unpackneg`'s two square-root tests,
which run on a PUBLIC key and a PUBLIC signature.  Nothing here processes a
secret under a data-dependent branch except the scalar clamp, which is the
standard one.  This is a game signing its own replays, not a smartcard.
*/

#include "quakedef.h"

typedef qint64_t ed_i64;
typedef ed_i64 gf[16];

static const gf
	gf0,
	gf1 = {1},
	ed_D  = {0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
	         0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203},
	ed_D2 = {0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
	         0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406},
	ed_X  = {0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
	         0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169},
	ed_Y  = {0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
	         0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666},
	ed_I  = {0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
	         0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83};

/* The group order, little-endian bytes: 2^252 + 27742317777372353535851937790883648493.
   Signed rather than unsigned so every product in modL stays in signed
   arithmetic -- the values there never exceed about 2^20, so nothing wraps and
   the reduction reads as the arithmetic it is. */
static const ed_i64 ed_L[32] = {
	0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
	0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0x10};

static void ed_set(gf r, const gf a)
{
	int i;
	for (i = 0; i < 16; i++)
		r[i] = a[i];
}

static void ed_car(gf o)
{
	int i;
	ed_i64 c;
	for (i = 0; i < 16; i++)
	{
		o[i] += (ed_i64)1 << 16;
		c = o[i] >> 16;
		o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
		o[i] -= c << 16;
	}
}

/*constant-time swap of p and q when b is 1*/
static void ed_sel(gf p, gf q, int b)
{
	int i;
	ed_i64 t, c = ~(b - 1);
	for (i = 0; i < 16; i++)
	{
		t = c & (p[i] ^ q[i]);
		p[i] ^= t;
		q[i] ^= t;
	}
}

static void ed_pack25519(qbyte *o, const gf n)
{
	int i, j, b;
	gf m, t;
	ed_set(t, n);
	ed_car(t);
	ed_car(t);
	ed_car(t);
	for (j = 0; j < 2; j++)
	{
		m[0] = t[0] - 0xffed;
		for (i = 1; i < 15; i++)
		{
			m[i] = t[i] - 0xffff - ((m[i-1] >> 16) & 1);
			m[i-1] &= 0xffff;
		}
		m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
		b = (m[15] >> 16) & 1;
		m[14] &= 0xffff;
		ed_sel(t, m, 1 - b);
	}
	for (i = 0; i < 16; i++)
	{
		o[2*i]   = t[i] & 0xff;
		o[2*i+1] = (t[i] >> 8) & 0xff;
	}
}

/*0 when equal, -1 otherwise, without branching on the contents*/
static int ed_vn(const qbyte *x, const qbyte *y, int n)
{
	int i;
	unsigned int d = 0;
	for (i = 0; i < n; i++)
		d |= x[i] ^ y[i];
	return (1 & ((d - 1) >> 8)) - 1;
}

static int ed_neq(const gf a, const gf b)
{
	qbyte c[32], d[32];
	ed_pack25519(c, a);
	ed_pack25519(d, b);
	return ed_vn(c, d, 32);
}

static qbyte ed_par(const gf a)
{
	qbyte d[32];
	ed_pack25519(d, a);
	return d[0] & 1;
}

static void ed_unpack25519(gf o, const qbyte *n)
{
	int i;
	for (i = 0; i < 16; i++)
		o[i] = n[2*i] + ((ed_i64)n[2*i+1] << 8);
	o[15] &= 0x7fff;
}

static void ed_A(gf o, const gf a, const gf b)
{
	int i;
	for (i = 0; i < 16; i++)
		o[i] = a[i] + b[i];
}

static void ed_Z(gf o, const gf a, const gf b)
{
	int i;
	for (i = 0; i < 16; i++)
		o[i] = a[i] - b[i];
}

static void ed_M(gf o, const gf a, const gf b)
{
	int i, j;
	ed_i64 t[31];
	for (i = 0; i < 31; i++)
		t[i] = 0;
	for (i = 0; i < 16; i++)
		for (j = 0; j < 16; j++)
			t[i+j] += a[i] * b[j];
	for (i = 0; i < 15; i++)
		t[i] += 38 * t[i+16];
	for (i = 0; i < 16; i++)
		o[i] = t[i];
	ed_car(o);
	ed_car(o);
}

static void ed_S(gf o, const gf a)
{
	ed_M(o, a, a);
}

static void ed_inv(gf o, const gf i)
{
	gf c;
	int a;
	ed_set(c, i);
	for (a = 253; a >= 0; a--)
	{
		ed_S(c, c);
		if (a != 2 && a != 4)
			ed_M(c, c, i);
	}
	ed_set(o, c);
}

static void ed_pow2523(gf o, const gf i)
{
	gf c;
	int a;
	ed_set(c, i);
	for (a = 250; a >= 0; a--)
	{
		ed_S(c, c);
		if (a != 1)
			ed_M(c, c, i);
	}
	ed_set(o, c);
}

/*extended coordinates: p = (x, y, z, t)*/
static void ed_add(gf p[4], gf q[4])
{
	gf a, b, c, d, t, e, f, g, h;
	ed_Z(a, p[1], p[0]);
	ed_Z(t, q[1], q[0]);
	ed_M(a, a, t);
	ed_A(b, p[0], p[1]);
	ed_A(t, q[0], q[1]);
	ed_M(b, b, t);
	ed_M(c, p[3], q[3]);
	ed_M(c, c, ed_D2);
	ed_M(d, p[2], q[2]);
	ed_A(d, d, d);
	ed_Z(e, b, a);
	ed_Z(f, d, c);
	ed_A(g, d, c);
	ed_A(h, b, a);
	ed_M(p[0], e, f);
	ed_M(p[1], h, g);
	ed_M(p[2], g, f);
	ed_M(p[3], e, h);
}

static void ed_cswap(gf p[4], gf q[4], qbyte b)
{
	int i;
	for (i = 0; i < 4; i++)
		ed_sel(p[i], q[i], b);
}

static void ed_packpoint(qbyte *r, gf p[4])
{
	gf tx, ty, zi;
	ed_inv(zi, p[2]);
	ed_M(tx, p[0], zi);
	ed_M(ty, p[1], zi);
	ed_pack25519(r, ty);
	r[31] ^= ed_par(tx) << 7;
}

static void ed_scalarmult(gf p[4], gf q[4], const qbyte *s)
{
	int i;
	ed_set(p[0], gf0);
	ed_set(p[1], gf1);
	ed_set(p[2], gf1);
	ed_set(p[3], gf0);
	for (i = 255; i >= 0; i--)
	{
		qbyte b = (s[i/8] >> (i & 7)) & 1;
		ed_cswap(p, q, b);
		ed_add(q, p);
		ed_add(p, p);
		ed_cswap(p, q, b);
	}
}

static void ed_scalarbase(gf p[4], const qbyte *s)
{
	gf q[4];
	ed_set(q[0], ed_X);
	ed_set(q[1], ed_Y);
	ed_set(q[2], gf1);
	ed_M(q[3], ed_X, ed_Y);
	ed_scalarmult(p, q, s);
}

static void ed_modL(qbyte *r, ed_i64 x[64])
{
	ed_i64 carry;
	int i, j;
	for (i = 63; i >= 32; i--)
	{
		carry = 0;
		for (j = i - 32; j < i - 12; j++)
		{
			x[j] += carry - 16 * x[i] * ed_L[j - (i - 32)];
			carry = (x[j] + 128) >> 8;
			x[j] -= carry << 8;
		}
		x[j] += carry;
		x[i] = 0;
	}
	carry = 0;
	for (j = 0; j < 32; j++)
	{
		x[j] += carry - (x[31] >> 4) * ed_L[j];
		carry = x[j] >> 8;
		x[j] &= 255;
	}
	for (j = 0; j < 32; j++)
		x[j] -= carry * ed_L[j];
	for (i = 0; i < 32; i++)
	{
		x[i+1] += x[i] >> 8;
		r[i] = x[i] & 255;
	}
}

static void ed_reduce(qbyte *r)
{
	ed_i64 x[64];
	int i;
	for (i = 0; i < 64; i++)
		x[i] = (ed_i64)r[i];
	for (i = 0; i < 64; i++)
		r[i] = 0;
	ed_modL(r, x);
}

static void ed_sha512_2(qbyte digest[64], const qbyte *a, size_t alen,
                        const qbyte *b, size_t blen, const qbyte *c, size_t clen)
{
	/*three parts because every hash this file needs is a concatenation and
	  building the join in a heap block would mean a malloc per signature on a
	  path that must not fail.*/
	void *ctx = alloca(hash_sha2_512.contextsize);
	hash_sha2_512.init(ctx);
	if (alen)
		hash_sha2_512.process(ctx, a, alen);
	if (blen)
		hash_sha2_512.process(ctx, b, blen);
	if (clen)
		hash_sha2_512.process(ctx, c, clen);
	hash_sha2_512.terminate(digest, ctx);
}

/*
  The public API.  `sk` is the 64-byte NaCl layout -- seed(32) || pubkey(32) --
  because that is what every other implementation calls a secret key, and a file
  written in some private layout would be a file only this engine can read.
*/
void Ed25519_FromSeed(qbyte pk[32], qbyte sk[64], const qbyte seed[32])
{
	qbyte d[64];
	gf p[4];
	int i;

	ed_sha512_2(d, seed, 32, NULL, 0, NULL, 0);
	d[0] &= 248;
	d[31] &= 127;
	d[31] |= 64;

	ed_scalarbase(p, d);
	ed_packpoint(pk, p);

	for (i = 0; i < 32; i++)
		sk[i] = seed[i];
	for (i = 0; i < 32; i++)
		sk[32+i] = pk[i];
}

void Ed25519_Sign(qbyte sig[64], const qbyte *m, size_t mlen, const qbyte sk[64])
{
	qbyte d[64], h[64], r[64];
	ed_i64 x[64];
	gf p[4];
	int i, j;

	ed_sha512_2(d, sk, 32, NULL, 0, NULL, 0);
	d[0] &= 248;
	d[31] &= 127;
	d[31] |= 64;

	ed_sha512_2(r, d + 32, 32, m, mlen, NULL, 0);
	ed_reduce(r);
	ed_scalarbase(p, r);
	ed_packpoint(sig, p);

	ed_sha512_2(h, sig, 32, sk + 32, 32, m, mlen);
	ed_reduce(h);

	for (i = 0; i < 64; i++)
		x[i] = 0;
	for (i = 0; i < 32; i++)
		x[i] = (ed_i64)r[i];
	for (i = 0; i < 32; i++)
		for (j = 0; j < 32; j++)
			x[i+j] += (ed_i64)h[i] * (ed_i64)d[j];
	ed_modL(sig + 32, x);
}

/*
  IS THIS POINT IN THE SMALL SUBGROUP?  [8]A is the identity exactly when A has
  order 1, 2, 4 or 8.

  WHY IT IS CHECKED HERE RATHER THAN LEFT TO THE CALLER, which is what most
  Ed25519 code does: this engine's caller treats the PUBLIC KEY AS AN IDENTITY.
  A signature under a small-order key verifies for anybody -- [h]A is the
  identity whatever h is, so the check collapses to R == [s]B, which anyone can
  satisfy by choosing r and setting s = r.  Under the ordinary reading (one key,
  one signer) that is a curiosity; under "this key is who you are on the board"
  it is a name eight people can wear at once, and nobody has to break anything
  to do it.  Three doublings is the whole cost.

  Computed rather than tabulated on purpose.  The eight encodings are a constant
  that has to be right, and a wrong byte in a blacklist fails silently in the
  direction that accepts.
*/
static int ed_small_order(gf p[4])
{
	gf q[4];
	qbyte packed[32];
	static const qbyte identity[32] = {1};
	int i;

	for (i = 0; i < 4; i++)
		ed_set(q[i], p[i]);
	ed_add(q, q);
	ed_add(q, q);
	ed_add(q, q);
	ed_packpoint(packed, q);
	return !memcmp(packed, identity, sizeof(packed));
}

static int ed_unpackneg(gf r[4], const qbyte p[32])
{
	gf t, chk, num, den, den2, den4, den6;
	qbyte canon[32];

	ed_set(r[2], gf1);
	ed_unpack25519(r[1], p);

	/*
	  THE ENCODING MUST BE THE CANONICAL ONE, and this matters here for the same
	  reason the subgroup test above does.  ed_unpack25519 masks the sign bit and
	  says nothing about y >= 2^255-19, so for the nineteen smallest y values
	  there are TWO 64-character spellings of one key -- and anything that keys a
	  player on the hex text (a board, a "same key across runs" join) would see
	  them as two people, or one person as two.  Re-pack and compare: a canonical
	  encoding is its own round trip.
	*/
	ed_pack25519(canon, r[1]);
	canon[31] |= p[31] & 0x80;
	if (memcmp(canon, p, sizeof(canon)))
		return -1;
	ed_S(num, r[1]);
	ed_M(den, num, ed_D);
	ed_Z(num, num, r[2]);
	ed_A(den, r[2], den);

	ed_S(den2, den);
	ed_S(den4, den2);
	ed_M(den6, den4, den2);
	ed_M(t, den6, num);
	ed_M(t, t, den);

	ed_pow2523(t, t);
	ed_M(t, t, num);
	ed_M(t, t, den);
	ed_M(t, t, den);
	ed_M(r[0], t, den);

	ed_S(chk, r[0]);
	ed_M(chk, chk, den);
	if (ed_neq(chk, num))
		ed_M(r[0], r[0], ed_I);

	ed_S(chk, r[0]);
	ed_M(chk, chk, den);
	if (ed_neq(chk, num))
		return -1;		/*not a point on the curve*/

	if (ed_par(r[0]) == (p[31] >> 7))
		ed_Z(r[0], gf0, r[0]);

	ed_M(r[3], r[0], r[1]);
	return 0;
}

qboolean Ed25519_Verify(const qbyte sig[64], const qbyte *m, size_t mlen, const qbyte pk[32])
{
	qbyte h[64], t[32];
	gf p[4], q[4];
	int i;

	/*
	  s < L, IN FULL, AND THE THREE-BIT VERSION WAS NOT ENOUGH.  The usual
	  `sig[63] & 224` only refuses s >= 2^253, and L is about 2^252 -- so for
	  every honest signature there is a second accepted encoding, (R, s+L),
	  which verifies identically because B has order L.  This engine's offline
	  verifier (tools/ed25519.py) does the full comparison, so the two would
	  have accepted DIFFERENT SETS of signatures, and no random cross-check
	  could ever have found it: no honestly-produced s is anywhere near L.
	*/
	for (i = 31; i >= 0; i--)
	{
		if (sig[32+i] < (qbyte)ed_L[i])
			break;				/*strictly less: reduced*/
		if (sig[32+i] > (qbyte)ed_L[i])
			return false;		/*strictly greater: not reduced*/
	}
	if (i < 0)
		return false;			/*exactly L*/

	if (ed_unpackneg(q, pk))
		return false;
	/*
	  A SMALL-ORDER KEY VERIFIES EVERYTHING, and this caller treats the public
	  key as a player's identity -- so it would be a name anybody can wear.  See
	  ed_small_order.
	*/
	if (ed_small_order(q))
		return false;

	ed_sha512_2(h, sig, 32, pk, 32, m, mlen);
	ed_reduce(h);

	ed_scalarmult(p, q, h);
	ed_scalarbase(q, sig + 32);
	ed_add(p, q);
	ed_packpoint(t, p);

	return ed_vn(sig, t, 32) == 0;
}
