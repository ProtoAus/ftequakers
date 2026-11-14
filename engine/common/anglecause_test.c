// Standalone codec falsifier: no game, socket, journal or physical input.
#include "anglecause.h"
#include <assert.h>
int main(void)
{
	const char *nonce = "00000000000000000000000000000000";
	unsigned char wire[13] = {10, 0, 64, 128};
	char marker[AC_MARKER_MAX], hex[27], row[256], span[256];
	anglecause_t a;
	size_t n, content;
	int size;
	assert(AC_Decimal("18446744073709551615"));
	assert(!AC_Decimal("18446744073709551616"));
	assert(!AC_Decimal("01"));
	assert(!AC_Decimal("-1"));
	assert(AC_U32("4294967295"));
	assert(!AC_U32("4294967296"));
	assert(!AC_WireHex("0a0000807f0000000000000000", 4)); // +inf
	assert(!AC_WireHex("0a0100807f0000000000000000", 4)); // NaN
	assert(AC_WireHex("0a0000803f0000000000000000", 4));
	for (size = 1; size <= 4; size *= 2)
	{
		AC_Encode(hex, wire, 1+3*size);
		snprintf(marker, sizeof(marker), "//ac1 %s 1 7 %s %s\n", nonce, AC_Format(size), hex);
		assert(AC_Parse(marker, &a, size, 10));
		assert(AC_Matches(&a, wire, 1+3*size, size));
		assert(!AC_Matches(&a, wire, 3*size, size));
		wire[1] ^= 1;
		assert(!AC_Matches(&a, wire, 1+3*size, size));
		wire[1] ^= 1;
		assert(!AC_Parse(marker, &a, size, 11));
		assert(!AC_Parse(marker, &a, 3, 10));
		snprintf(row, sizeof(row), "angleop 1 2 3 %s 1 7 %s %s\n", nonce, AC_Format(size), hex);
		n = strlen(row);
		assert(AC_Record(row, n, 10));
		assert(AC_Record(row, n-1, 10)); // sized canonical span need not be NUL-terminated
		assert(!AC_Record(row, n+1, 10)); // supplied NUL is not part of a row
		assert(AC_PhysicalLine(row, n, &content) && content == n-1);
		assert(AC_PhysicalLine(row, n-1, &content) && content == n-1);

		memcpy(span, row, n-1);
		span[n-1] = 0; span[n] = 'x'; span[n+1] = '\n';
		assert(!AC_Record(span, n+2, 10));
		assert(!AC_PhysicalLine(span, n+2, &content));

		memcpy(span, row, 4);
		span[4] = '\r';
		memcpy(span+5, row+4, n-4);
		assert(!AC_Record(span, n+1, 10));
		assert(!AC_PhysicalLine(span, n+1, &content));

		memcpy(span, row, n-1);
		span[n-1] = '\r'; span[n] = '\n';
		assert(AC_PhysicalLine(span, n+1, &content) && content == n-1);
		assert(AC_Record(span, content, 10)); // legacy CRLF admission retains full-span validation
		assert(!AC_Record(span, n+1, 10)); // raw canonical codec is LF/bare only
		span[n-1] = ' ';
		assert(!AC_Record(span, n+1, 10));
		assert(!AC_Record(row, n-2, 10));
		strcat(row, "extra");
		assert(!AC_Record(row, strlen(row), 10));
		strcat(marker, "extra");
		assert(!AC_Parse(marker, &a, size, 10));
	}
	snprintf(marker, sizeof(marker), "//ac1 %s 0 7 qwabs1n 0a004080\n", nonce);
	assert(!AC_Parse(marker, &a, 1, 10));
	snprintf(marker, sizeof(marker), "//ac1 %s 1 4294967296 qwabs1n 0a004080\n", nonce);
	assert(!AC_Parse(marker, &a, 1, 10));
	snprintf(marker, sizeof(marker), "//ac1 %s 1 7 qwabs1n 0A004080\n", nonce);
	assert(!AC_Parse(marker, &a, 1, 10));
	assert(AC_PhysicalLine("anglecause 1\r\n", sizeof("anglecause 1\r\n")-1, &content) && content == 12);
	assert(!AC_PhysicalLine("angleca\ruse 1\n", sizeof("angleca\ruse 1\n")-1, &content));
	assert(!AC_PhysicalLine("anglecause 1\0x\n", sizeof("anglecause 1\0x\n")-1, &content));
	assert(!AC_PhysicalLine("anglecause 1\r\r\n", sizeof("anglecause 1\r\r\n")-1, &content));
	assert(!AC_PhysicalLine("anglecause 1\r", sizeof("anglecause 1\r")-1, &content));
	puts("anglecause codec PASS");
	return 0;
}
