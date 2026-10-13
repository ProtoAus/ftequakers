// WASAPI interleaved samples are little-endian; extensible PCM valid bits are left-aligned.
#ifndef SND_VIS_PCM_H
#define SND_VIS_PCM_H
#include <math.h>
#include <string.h>
static float SNDVIS_PCMMono(const unsigned char *p, int channels, int bytes, int floating)
{
	double sum = 0;
	int c, b;
	for (c = 0; c < channels; c++, p += bytes)
	{
		double f;
		if (floating)
		{
			if (bytes == 4) { float x; memcpy(&x, p, 4); f = x; }
			else { double x; memcpy(&x, p, 8); f = x; }
		}
		else if (bytes == 1)
			f = ((int)*p - 128) / 128.0;
		else
		{
			unsigned int u = 0;
			for (b = 0; b < bytes; b++) u |= (unsigned int)p[b] << (8*b);
			f = (double)u;
			if (p[bytes-1] & 128) f -= ldexp(1.0, bytes*8);
			f = ldexp(f, 1-bytes*8);
		}
		if (!isfinite(f)) f = 0;
		if (f > 1) f = 1;
		if (f < -1) f = -1;
		sum += f;
	}
	return channels ? sum/channels : 0;
}
#endif
