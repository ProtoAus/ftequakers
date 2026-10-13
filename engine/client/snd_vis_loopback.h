// Main-thread, default render endpoint only. No render client or capture endpoint is opened.
#ifndef SND_VIS_LOOPBACK_H
#define SND_VIS_LOOPBACK_H
#include "snd_vis_pcm.h"
#if defined(_WIN32) && !defined(SERVERONLY)
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#endif
#define VISLB_RING 32768
static struct
{
	float ring[VISLB_RING];
	unsigned long long total;
	unsigned int generation, packets;
	int rate, active;
	double retry, checktime, lastdata, zerotime;
#if defined(_WIN32) && !defined(SERVERONLY)
	IMMDeviceEnumerator *enumerator;
	IMMDevice *device;
	IAudioClient *client;
	IAudioCaptureClient *capture;
	WAVEFORMATEX *format;
	LPWSTR id;
	int couninit, bytes, floating;
#endif
} vislb;

static void VISLB_Clear(void)
{
	vislb.total = 0;
	vislb.generation++;
	memset(vislb.ring, 0, sizeof(vislb.ring));
}
static inline unsigned int VISLB_Append(const unsigned char *data, unsigned int frames, int channels, int bytes, int floating, int silent)
{
	unsigned int i, start = frames > VISLB_RING ? frames - VISLB_RING : 0;
	for (i = start; i < frames; i++)
		vislb.ring[(vislb.total+i)&(VISLB_RING-1)] = silent ? 0 :
			SNDVIS_PCMMono(data + (size_t)i*channels*bytes, channels, bytes, floating);
	vislb.total += frames;
	return frames - start;
}
static void VISLB_Close(void)
{
#if defined(_WIN32) && !defined(SERVERONLY)
	if (vislb.client) vislb.client->lpVtbl->Stop(vislb.client);
	if (vislb.capture) vislb.capture->lpVtbl->Release(vislb.capture);
	if (vislb.client) vislb.client->lpVtbl->Release(vislb.client);
	if (vislb.device) vislb.device->lpVtbl->Release(vislb.device);
	if (vislb.enumerator) vislb.enumerator->lpVtbl->Release(vislb.enumerator);
	CoTaskMemFree(vislb.format);
	CoTaskMemFree(vislb.id);
	vislb.capture = NULL; vislb.client = NULL; vislb.device = NULL;
	vislb.enumerator = NULL; vislb.format = NULL; vislb.id = NULL;
	if (vislb.couninit) CoUninitialize();
	vislb.couninit = 0;
#endif
	vislb.active = 0;
	VISLB_Clear();
}
#if defined(_WIN32) && !defined(SERVERONLY)
static const GUID vislb_clsid = {0xBCDE0395,0xE52F,0x467C,{0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E}};
static const GUID vislb_enum_iid = {0xA95664D2,0x9614,0x4F35,{0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6}};
static const GUID vislb_client_iid = {0x1CB9AD4C,0xDBFA,0x4c32,{0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2}};
static const GUID vislb_capture_iid = {0xC8ADBD64,0xE71E,0x48a0,{0xA4,0xDE,0x18,0x5C,0x39,0x5C,0xD3,0x17}};
static int VISLB_Open(double now)
{
	WAVEFORMATEX *f;
	int tag;
	HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
	if (SUCCEEDED(hr)) vislb.couninit = 1;
	else if (hr != RPC_E_CHANGED_MODE) goto fail;
	if (FAILED(CoCreateInstance(&vislb_clsid, NULL, CLSCTX_ALL, &vislb_enum_iid, (void**)&vislb.enumerator))) goto fail;
	if (FAILED(vislb.enumerator->lpVtbl->GetDefaultAudioEndpoint(vislb.enumerator, eRender, eConsole, &vislb.device))) goto fail;
	if (FAILED(vislb.device->lpVtbl->GetId(vislb.device, &vislb.id))) goto fail;
	if (FAILED(vislb.device->lpVtbl->Activate(vislb.device, &vislb_client_iid, CLSCTX_ALL, NULL, (void**)&vislb.client))) goto fail;
	if (FAILED(vislb.client->lpVtbl->GetMixFormat(vislb.client, &vislb.format))) goto fail;
	f = vislb.format;
	tag = f->wFormatTag;
	if (tag == WAVE_FORMAT_EXTENSIBLE)
	{
		static const unsigned char tail[12] = {0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
		WAVEFORMATEXTENSIBLE *ext = (WAVEFORMATEXTENSIBLE*)f;
		if (f->cbSize < 22 || memcmp((const unsigned char*)&ext->SubFormat+4, tail, 12)) goto fail;
		tag = ext->SubFormat.Data1;
		if (!ext->Samples.wValidBitsPerSample || ext->Samples.wValidBitsPerSample > f->wBitsPerSample) goto fail;
	}
	vislb.floating = tag == WAVE_FORMAT_IEEE_FLOAT;
	if (tag != WAVE_FORMAT_PCM && !vislb.floating) goto fail;
	if (vislb.floating ? (f->wBitsPerSample != 32 && f->wBitsPerSample != 64) :
		(f->wBitsPerSample != 8 && f->wBitsPerSample != 16 && f->wBitsPerSample != 24 && f->wBitsPerSample != 32)) goto fail;
	vislb.bytes = f->wBitsPerSample/8;
	if (!f->nChannels || f->nChannels > 32 || f->nBlockAlign != f->nChannels*vislb.bytes ||
		f->nSamplesPerSec < 8000 || f->nSamplesPerSec > 192000) goto fail;
	if (FAILED(vislb.client->lpVtbl->Initialize(vislb.client, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 1000000, 0, f, NULL))) goto fail;
	if (FAILED(vislb.client->lpVtbl->GetService(vislb.client, &vislb_capture_iid, (void**)&vislb.capture))) goto fail;
	if (FAILED(vislb.client->lpVtbl->Start(vislb.client))) goto fail;
	VISLB_Clear();
	vislb.rate = f->nSamplesPerSec;
	vislb.active = 1;
	vislb.lastdata = vislb.zerotime = now;
	vislb.checktime = now + 2;
	return 1;
fail:
	VISLB_Close();
	vislb.retry = now + 2;
	return 0;
}
#endif
static int VISLB_Poll(double now)
{
#if defined(_WIN32) && !defined(SERVERONLY)
	UINT32 frames, size;
	DWORD flags;
	BYTE *data;
	unsigned int i, packets, converted = 0;
	int got = 0;
	if (!vislb.active && (now < vislb.retry || !VISLB_Open(now))) return 0;
	if (now >= vislb.checktime)
	{
		IMMDevice *device = NULL;
		LPWSTR id = NULL;
		int same = 0;
		if (SUCCEEDED(vislb.enumerator->lpVtbl->GetDefaultAudioEndpoint(vislb.enumerator, eRender, eConsole, &device)))
		{
			if (SUCCEEDED(device->lpVtbl->GetId(device, &id))) same = !wcscmp(id, vislb.id);
			CoTaskMemFree(id);
			device->lpVtbl->Release(device);
		}
		if (!same) goto fail;
		vislb.checktime = now + 2;
	}
	// Bound both packet count and conversion work, even after a long frame hitch.
	for (packets = 0; packets < 64; packets++)
	{
		if (FAILED(vislb.capture->lpVtbl->GetNextPacketSize(vislb.capture, &size))) goto fail;
		if (!size) break;
		if (FAILED(vislb.capture->lpVtbl->GetBuffer(vislb.capture, &data, &frames, &flags, NULL, NULL))) goto fail;
		if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) VISLB_Clear();
		converted += VISLB_Append(data, frames, vislb.format->nChannels, vislb.bytes, vislb.floating, flags & AUDCLNT_BUFFERFLAGS_SILENT);
		vislb.packets++;
		got = 1;
		if (FAILED(vislb.capture->lpVtbl->ReleaseBuffer(vislb.capture, frames))) goto fail;
		if (converted >= VISLB_RING) break;
	}
	if (got) vislb.lastdata = vislb.zerotime = now;
	else if (now - vislb.lastdata > 0.1)
	{
		// Some Windows versions stop issuing loopback packets when the endpoint is idle.
		double count = (now - vislb.zerotime)*vislb.rate;
		unsigned int n = count > VISLB_RING ? VISLB_RING : count > 0 ? (unsigned int)count : 0;
		for (i = 0; i < n; i++) vislb.ring[(vislb.total+i)&(VISLB_RING-1)] = 0;
		vislb.total += n;
		if (n) vislb.zerotime = now;
	}
	return 1;
fail:
	VISLB_Close();
	vislb.retry = now + 2;
#else
	(void)now;
#endif
	return 0;
}
#endif
