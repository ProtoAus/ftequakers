// Main-thread, default render endpoint only. No render client or capture endpoint is opened (Linux: the default
// output's monitor source, never an input device; Patch 1682).
#ifndef SND_VIS_LOOPBACK_H
#define SND_VIS_LOOPBACK_H
#include "snd_vis_pcm.h"
#if defined(_WIN32) && !defined(SERVERONLY)
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#elif defined(__linux__) && defined(AUDIO_PULSE) && !defined(SERVERONLY)
#define VISLB_PULSE	//FTESurf Patch 1682: see the block above VISLB_Open
#endif
#define VISLB_RING 32768
static struct
{
	float ring[VISLB_RING];
	unsigned long long total;
	unsigned int generation, packets;
	int rate, active;
	double retry, checktime, lastdata, zerotime;
	double backoff, lastuse, opened;	//FTESurf Patch 1682: failed opens wait 2 s doubling to 60 (back to 2 once a
					//capture has stayed up 10 s since `opened`); lastuse closes an unasked capture
#if defined(_WIN32) && !defined(SERVERONLY)
	IMMDeviceEnumerator *enumerator;
	IMMDevice *device;
	IAudioClient *client;
	IAudioCaptureClient *capture;
	WAVEFORMATEX *format;
	LPWSTR id;
	int couninit, bytes, floating;
#endif
#ifdef VISLB_PULSE
	struct pa_mainloop *ml;
	struct pa_context *ctx;
	struct pa_stream *st;
	int ready;
	double deadline;
#endif
} vislb;

#ifdef VISLB_PULSE
/*FTESurf Patch 1682: Linux desktop audio is the PulseAudio/PipeWire monitor of the default output (@DEFAULT_MONITOR@),
  read on the main thread through a pa_mainloop pumped without blocking: no thread, nothing played or recorded, and a
  stream whose source is not a *.monitor (a server that took the name for a microphone) is closed before any sample.*/
typedef struct pa_mainloop pa_mainloop;
typedef struct pa_mainloop_api pa_mainloop_api;
typedef struct pa_context pa_context;
typedef struct pa_stream pa_stream;
typedef struct {int format; uint32_t rate; uint8_t channels;} vislb_paspec_t;	//pa_sample_spec
typedef struct {uint32_t maxlength, tlength, prebuf, minreq, fragsize;} vislb_paattr_t;	//pa_buffer_attr
#define VISLB_PA_RATE		48000
#define VISLB_PA_FLOAT32LE	5	//pa_sample_format_t
#define VISLB_PA_CTX_READY	4	//pa_context_state_t: FAILED 5, TERMINATED 6
#define VISLB_PA_ST_READY	2	//pa_stream_state_t: FAILED 3, TERMINATED 4
static struct
{
	pa_mainloop *(*mainloop_new)(void);
	pa_mainloop_api *(*mainloop_get_api)(pa_mainloop *m);
	int (*mainloop_iterate)(pa_mainloop *m, int block, int *retval);
	void (*mainloop_free)(pa_mainloop *m);
	pa_context *(*context_new)(pa_mainloop_api *api, const char *name);
	int (*context_connect)(pa_context *c, const char *server, int flags, const void *spawn);
	int (*context_get_state)(pa_context *c);
	void (*context_disconnect)(pa_context *c);
	void (*context_unref)(pa_context *c);
	pa_stream *(*stream_new)(pa_context *c, const char *name, const vislb_paspec_t *ss, const void *map);
	int (*stream_connect_record)(pa_stream *s, const char *dev, const vislb_paattr_t *attr, int flags);
	int (*stream_get_state)(pa_stream *s);
	const char *(*stream_get_device_name)(pa_stream *s);
	int (*stream_peek)(pa_stream *s, const void **data, size_t *nbytes);
	int (*stream_drop)(pa_stream *s);
	int (*stream_disconnect)(pa_stream *s);
	void (*stream_unref)(pa_stream *s);
	dllhandle_t *lib;
	int tried;
} vislb_pa;
static int VISLB_PulseLoad(void)
{
	dllfunction_t funcs[] = {
		{(void**)&vislb_pa.mainloop_new, "pa_mainloop_new"},
		{(void**)&vislb_pa.mainloop_get_api, "pa_mainloop_get_api"},
		{(void**)&vislb_pa.mainloop_iterate, "pa_mainloop_iterate"},
		{(void**)&vislb_pa.mainloop_free, "pa_mainloop_free"},
		{(void**)&vislb_pa.context_new, "pa_context_new"},
		{(void**)&vislb_pa.context_connect, "pa_context_connect"},
		{(void**)&vislb_pa.context_get_state, "pa_context_get_state"},
		{(void**)&vislb_pa.context_disconnect, "pa_context_disconnect"},
		{(void**)&vislb_pa.context_unref, "pa_context_unref"},
		{(void**)&vislb_pa.stream_new, "pa_stream_new"},
		{(void**)&vislb_pa.stream_connect_record, "pa_stream_connect_record"},
		{(void**)&vislb_pa.stream_get_state, "pa_stream_get_state"},
		{(void**)&vislb_pa.stream_get_device_name, "pa_stream_get_device_name"},
		{(void**)&vislb_pa.stream_peek, "pa_stream_peek"},
		{(void**)&vislb_pa.stream_drop, "pa_stream_drop"},
		{(void**)&vislb_pa.stream_disconnect, "pa_stream_disconnect"},
		{(void**)&vislb_pa.stream_unref, "pa_stream_unref"},
		{NULL, NULL}};
	if (!vislb_pa.tried)
	{
		vislb_pa.tried = 1;
		vislb_pa.lib = Sys_LoadLibrary("libpulse.so.0", funcs);
	}
	return vislb_pa.lib != NULL;
}
#endif

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
#ifdef VISLB_PULSE
	if (vislb.st) { vislb_pa.stream_disconnect(vislb.st); vislb_pa.stream_unref(vislb.st); }
	if (vislb.ctx) { vislb_pa.context_disconnect(vislb.ctx); vislb_pa.context_unref(vislb.ctx); }
	if (vislb.ml) vislb_pa.mainloop_free(vislb.ml);
	vislb.st = NULL; vislb.ctx = NULL; vislb.ml = NULL;
	vislb.ready = 0;
#endif
	vislb.active = 0;
	VISLB_Clear();
}
#if (defined(_WIN32) && !defined(SERVERONLY)) || defined(VISLB_PULSE)
//FTESurf Patch 1682: desktop audio is on by default now, so a machine where it cannot open (no sound server, a WASAPI
//without loopback) must not pay the synchronous open every few seconds for the whole session.
static void VISLB_Fail(double now)
{
	VISLB_Close();
	vislb.backoff = vislb.backoff < 2 ? 2 : (vislb.backoff >= 30 ? 60 : vislb.backoff * 2);
	vislb.retry = now + vislb.backoff;
}
#endif
#if defined(_WIN32) && !defined(SERVERONLY)
static const GUID vislb_clsid = {0xBCDE0395,0xE52F,0x467C,{0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E}};
static const GUID vislb_enum_iid = {0xA95664D2,0x9614,0x4F35,{0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6}};
static const GUID vislb_client_iid = {0x1CB9AD4C,0xDBFA,0x4c32,{0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2}};
static const GUID vislb_capture_iid = {0xC8ADBD64,0xE71E,0x48a0,{0xA4,0xDE,0x18,0x5C,0x39,0x5C,0xD3,0x17}};
static int VISLB_Open(double now)
{
	WAVEFORMATEX *f;
	GUID session;
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
	// Do not inherit mute/volume from the game's rendering session.
	if (FAILED(CoCreateGuid(&session))) goto fail;
	if (FAILED(vislb.client->lpVtbl->Initialize(vislb.client, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 1000000, 0, f, &session))) goto fail;
	if (FAILED(vislb.client->lpVtbl->GetService(vislb.client, &vislb_capture_iid, (void**)&vislb.capture))) goto fail;
	if (FAILED(vislb.client->lpVtbl->Start(vislb.client))) goto fail;
	VISLB_Clear();
	vislb.rate = f->nSamplesPerSec;
	vislb.active = 1;
	vislb.lastdata = vislb.zerotime = vislb.opened = now;
	vislb.checktime = now + 2;
	return 1;
fail:
	VISLB_Fail(now);
	return 0;
}
#elif defined(VISLB_PULSE)
//Starts the asynchronous connect; VISLB_Poll walks it to a ready stream (or fails it after 3 s).
static int VISLB_Open(double now)
{
	if (!VISLB_PulseLoad()) goto fail;
	if (!(vislb.ml = vislb_pa.mainloop_new())) goto fail;
	if (!(vislb.ctx = vislb_pa.context_new(vislb_pa.mainloop_get_api(vislb.ml), "World of Surf visualiser"))) goto fail;
	if (vislb_pa.context_connect(vislb.ctx, NULL, 1/*PA_CONTEXT_NOAUTOSPAWN*/, NULL) < 0) goto fail;
	VISLB_Clear();
	vislb.rate = VISLB_PA_RATE;
	vislb.active = 1;
	vislb.ready = 0;
	vislb.deadline = now + 3;
	vislb.lastdata = vislb.zerotime = now;
	return 1;
fail:
	VISLB_Fail(now);
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
	if (now - vislb.opened > 10) vislb.backoff = 0;	//up 10 s: not on packets, an idle endpoint sends none
	return 1;
fail:
	VISLB_Fail(now);
#elif defined(VISLB_PULSE)
	const void *data;
	size_t bytes, len;
	unsigned int i, packets, converted = 0;
	int k, r = 0, got = 0;
	const char *dev;
	if (!vislb.active && (now < vislb.retry || !VISLB_Open(now))) return 0;
	for (k = 0; k < 32 && (r = vislb_pa.mainloop_iterate(vislb.ml, 0, NULL)) > 0; k++)
		;	//never blocks; bounded like the packet loop below
	if (r < 0) goto fail;
	if (!vislb.st)
	{
		k = vislb_pa.context_get_state(vislb.ctx);
		if (k > VISLB_PA_CTX_READY || (k != VISLB_PA_CTX_READY && now > vislb.deadline)) goto fail;
		if (k == VISLB_PA_CTX_READY)
		{
			vislb_paspec_t ss = {VISLB_PA_FLOAT32LE, VISLB_PA_RATE, 2};
			//0.5 s held at most (stale audio is dropped, not replayed after a hitch), 10 ms fragments
			vislb_paattr_t ba = {VISLB_PA_RATE*8/2, (uint32_t)-1, (uint32_t)-1, (uint32_t)-1, VISLB_PA_RATE*8/100};
			if (!(vislb.st = vislb_pa.stream_new(vislb.ctx, "visualiser (desktop audio)", &ss, NULL))) goto fail;
			//No DONT_MOVE: PipeWire then keeps the stream on the old output when the default changes. A move that
			//lands on a microphone (PulseAudio, the source unplugged) is caught by the name check below, which runs
			//after the iterate that delivers the move and before any sample is read.
			if (vislb_pa.stream_connect_record(vislb.st, "@DEFAULT_MONITOR@", &ba, 0x2000/*ADJUST_LATENCY*/) < 0) goto fail;
		}
		return 0;
	}
	k = vislb_pa.stream_get_state(vislb.st);
	if (k > VISLB_PA_ST_READY || (k != VISLB_PA_ST_READY && (vislb.ready || now > vislb.deadline))) goto fail;
	if (k != VISLB_PA_ST_READY) return 0;
	//every poll, not once: the name follows the stream if anything still moves it
	dev = vislb_pa.stream_get_device_name(vislb.st);
	len = dev ? strlen(dev) : 0;
	if (len < 8 || strcmp(dev + len - 8, ".monitor"))
	{
		Con_DPrintf("snd_vis: desktop audio refused source \"%s\" (not a monitor)\n", dev ? dev : "");
		goto fail;
	}
	if (!vislb.ready)
	{
		vislb.ready = 1;
		vislb.lastdata = vislb.zerotime = vislb.opened = now;
	}
	for (packets = 0; packets < 64; packets++)
	{
		if (vislb_pa.stream_peek(vislb.st, &data, &bytes) < 0) goto fail;
		if (!bytes) break;	//nothing buffered (data NULL too)
		//a NULL with bytes is a hole in the stream: that much silence
		converted += VISLB_Append(data ? (const unsigned char*)data : (const unsigned char*)"", (unsigned int)(bytes/8), 2, 4, 1, !data);
		vislb_pa.stream_drop(vislb.st);
		vislb.packets++;
		got = 1;
		if (converted >= VISLB_RING) break;
	}
	if (got) vislb.lastdata = vislb.zerotime = now;
	else if (now - vislb.lastdata > 0.1)
	{	//a suspended sink sends nothing: keep the clock moving on silence, as the WASAPI path does
		double count = (now - vislb.zerotime)*vislb.rate;
		unsigned int n = count > VISLB_RING ? VISLB_RING : count > 0 ? (unsigned int)count : 0;
		for (i = 0; i < n; i++) vislb.ring[(vislb.total+i)&(VISLB_RING-1)] = 0;
		vislb.total += n;
		if (n) vislb.zerotime = now;
	}
	if (now - vislb.opened > 10) vislb.backoff = 0;	//up 10 s: not on packets, an idle endpoint sends none
	return 1;
fail:
	VISLB_Fail(now);
#else
	(void)now;
#endif
	return 0;
}
#endif
