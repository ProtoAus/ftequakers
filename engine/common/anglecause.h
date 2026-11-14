#ifndef FTE_ANGLECAUSE_H
#define FTE_ANGLECAUSE_H
#include <string.h>
#include <stdio.h>

#define AC_MARKER_MAX 192
#define AC_WIRE_MAX 13

typedef struct {
	char nonce[33], op[21], basis[21], fmt[8], hex[AC_WIRE_MAX*2+1];
} anglecause_t;

static inline int AC_Decimal(const char *s)
{
	size_t n = strlen(s), i;
	if (!n || n > 20 || (n > 1 && *s == '0')) return 0;
	for (i = 0; i < n; i++) if (s[i] < '0' || s[i] > '9') return 0;
	return n < 20 || strcmp(s, "18446744073709551615") <= 0;
}
static inline int AC_U32(const char *s)
{
	return AC_Decimal(s) && (strlen(s) < 10 || (strlen(s) == 10 && strcmp(s, "4294967295") <= 0));
}
static inline int AC_Hex(const char *s, size_t n)
{
	size_t i;
	if (strlen(s) != n) return 0;
	for (i = 0; i < n; i++)
		if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
	return 1;
}
static inline int AC_Nibble(char c)
{
	return c <= '9' ? c-'0' : c-'a'+10;
}
static inline int AC_WireHex(const char *hex, int anglesize)
{
	int i, hi, lo;
	if (anglesize != 1 && anglesize != 2 && anglesize != 4) return 0;
	if (!AC_Hex(hex, 2*(1+3*anglesize))) return 0;
	if (anglesize == 4)
		for (i = 0; i < 3; i++)
		{
			hi = AC_Nibble(hex[8+i*8])*16 + AC_Nibble(hex[9+i*8]);
			lo = AC_Nibble(hex[6+i*8])*16 + AC_Nibble(hex[7+i*8]);
			if ((hi & 127) == 127 && (lo & 128)) return 0;
		}
	return 1;
}
static inline const char *AC_Format(int anglesize)
{
	return anglesize == 1 ? "qwabs1n" : anglesize == 2 ? "qwabs2n" : anglesize == 4 ? "qwabs4n" : "";
}
static inline void AC_Encode(char *hex, const unsigned char *bytes, size_t n)
{
	static const char digits[] = "0123456789abcdef";
	size_t i;
	for (i = 0; i < n; i++) { hex[i*2] = digits[bytes[i]>>4]; hex[i*2+1] = digits[bytes[i]&15]; }
	hex[n*2] = 0;
}
static inline int AC_Parse(const char *text, anglecause_t *a, int anglesize, int setangle)
{
	char canonical[AC_MARKER_MAX], first[3];
	unsigned char service = (unsigned char)setangle;
	if (!*AC_Format(anglesize) || strlen(text) >= sizeof(canonical)) return 0;
	if (sscanf(text, "//ac1 %32s %20s %20s %7s %26s", a->nonce, a->op, a->basis, a->fmt, a->hex) != 5) return 0;
	if (!AC_Hex(a->nonce, 32) || !AC_Decimal(a->op) || !strcmp(a->op, "0") || !AC_U32(a->basis) ||
		strcmp(a->fmt, AC_Format(anglesize)) || !AC_WireHex(a->hex, anglesize)) return 0;
	AC_Encode(first, &service, 1);
	if (strncmp(a->hex, first, 2)) return 0;
	snprintf(canonical, sizeof(canonical), "//ac1 %s %s %s %s %s\n", a->nonce, a->op, a->basis, a->fmt, a->hex);
	return !strcmp(text, canonical);
}
static inline int AC_Matches(const anglecause_t *a, const unsigned char *wire, size_t size, int anglesize)
{
	char hex[AC_WIRE_MAX*2+1];
	size_t n = 1+3*anglesize;
	if (!*AC_Format(anglesize) || size < n || strcmp(a->fmt, AC_Format(anglesize))) return 0;
	AC_Encode(hex, wire, n);
	return !strcmp(hex, a->hex);
}
// Legacy metadata admission checks the physical span before line normalization.
// Only a terminal LF or CRLF is removed; hidden NUL/internal CR is not canonical.
static inline int AC_PhysicalLine(const char *line, size_t size, size_t *content)
{
	if (memchr(line, 0, size)) return 0;
	if (size && line[size-1] == '\n')
	{
		size--;
		if (size && line[size-1] == '\r') size--;
	}
	if (memchr(line, '\r', size) || memchr(line, '\n', size)) return 0;
	*content = size;
	return 1;
}
// Known optional REC row: it never installs physics state.
static inline int AC_Record(const char *line, size_t size, int setangle)
{
	char text[256], pk[11], mt[11], row[11], marker[AC_MARKER_MAX], canonical[256];
	anglecause_t a;
	int anglesize;
	size_t canonical_size;
	if (size >= sizeof(text) || memchr(line, 0, size)) return 0;
	memcpy(text, line, size); text[size] = 0;
	if (sscanf(text, "angleop %10s %10s %10s %32s %20s %20s %7s %26s", pk, mt, row,
		a.nonce, a.op, a.basis, a.fmt, a.hex) != 8 || !AC_U32(pk) || !AC_U32(mt) || !AC_U32(row)) return 0;
	anglesize = !strcmp(a.fmt, "qwabs1n") ? 1 : !strcmp(a.fmt, "qwabs2n") ? 2 : !strcmp(a.fmt, "qwabs4n") ? 4 : 0;
	snprintf(marker, sizeof(marker), "//ac1 %s %s %s %s %s\n", a.nonce, a.op, a.basis, a.fmt, a.hex);
	if (!AC_Parse(marker, &a, anglesize, setangle)) return 0;
	// Engine snprintf aliases a void, always-terminated wrapper. Every field is
	// bounded above, so this canonical row fits without depending on its return.
	snprintf(canonical, sizeof(canonical), "angleop %s %s %s %s %s %s %s %s", pk, mt, row,
		a.nonce, a.op, a.basis, a.fmt, a.hex);
	canonical_size = strlen(canonical);
	if (size && text[size-1] == '\n') size--;
	return canonical_size == size && !memcmp(line, canonical, size);
}
#endif
