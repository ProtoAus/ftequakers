// gcc -std=c99 -O2 -Wall -Wextra -Werror tools/test_snd_vis_session.c -lole32 -luuid -lm -o test_snd_vis_session.exe
// Play external audio on the default render endpoint. Exit77 means no positive
// input: NOT MEASURED. Only this process's silent render session is muted.
// No owner settings, endpoint/session identifiers, microphone or raw audio.
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <math.h>
#include <stdio.h>
#ifndef PRODUCTION_HEADER
#define PRODUCTION_HEADER "../engine/client/snd_vis_loopback.h"
#endif
#include PRODUCTION_HEADER

static double Capture(IMMDevice *device, const GUID *session, int *packets)
{
	IAudioClient *client = NULL;
	IAudioCaptureClient *cap = NULL;
	WAVEFORMATEX *format = NULL;
	UINT32 size = 0, frames = 0, i;
	DWORD flags, start;
	BYTE *data;
	double sum = 0;
	unsigned count = 0;
	int floating, bytes;
	*packets = 0;
	if (FAILED(IMMDevice_Activate(device, &IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&client))) goto done;
	if (FAILED(IAudioClient_GetMixFormat(client, &format))) goto done;
	if (FAILED(IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 1000000, 0, format, session))) goto done;
	if (FAILED(IAudioClient_GetService(client, &IID_IAudioCaptureClient, (void**)&cap))) goto done;
	if (FAILED(IAudioClient_Start(client))) goto done;
	floating = format->wFormatTag == 3 || (format->wFormatTag == 0xfffe && ((WAVEFORMATEXTENSIBLE*)format)->SubFormat.Data1 == 3);
	bytes = format->wBitsPerSample / 8;
	start = GetTickCount();
	while ((DWORD)(GetTickCount() - start) < 700)
	{
		if (FAILED(IAudioCaptureClient_GetNextPacketSize(cap, &size))) break;
		while (size)
		{
			if (FAILED(IAudioCaptureClient_GetBuffer(cap, &data, &frames, &flags, NULL, NULL))) goto done;
			++*packets;
			for (i = 0; i < frames; ++i)
			{
				double f = 0;
				if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
					f = SNDVIS_PCMMono(data + (size_t)i * format->nBlockAlign, format->nChannels, bytes, floating);
				sum += f*f;
				++count;
			}
			IAudioCaptureClient_ReleaseBuffer(cap, frames);
			if (FAILED(IAudioCaptureClient_GetNextPacketSize(cap, &size))) goto done;
		}
		Sleep(10);
	}
 done:
	if (cap) IAudioCaptureClient_Release(cap);
	if (client) { IAudioClient_Stop(client); IAudioClient_Release(client); }
	if (format) CoTaskMemFree(format);
	return count ? sqrt(sum/count) : 0;
}

static double ProductionCapture(void)
{
	DWORD until = GetTickCount() + 700;
	double sum = 0;
	unsigned long long i, count;
	while (GetTickCount() < until)
	{
		VISLB_Poll(GetTickCount64() / 1000.0);
		Sleep(5);
	}
	count = vislb.total < VISLB_RING ? vislb.total : VISLB_RING;
	for (i = 0; i < count; ++i) sum += vislb.ring[i] * vislb.ring[i];
	printf("production active %d packets %u frames %llu rms %.6f\n",
		vislb.active, vislb.packets, vislb.total, count ? sqrt(sum/count) : 0);
	VISLB_Close();
	return count ? sqrt(sum/count) : 0;
}

int main(void)
{
	IMMDeviceEnumerator *enumerator = NULL;
	IMMDevice *device = NULL;
	IAudioClient *render = NULL;
	IAudioRenderClient *buffer = NULL;
	ISimpleAudioVolume *volume = NULL;
	WAVEFORMATEX *format = NULL;
	GUID independent;
	UINT32 frames = 0;
	BYTE *data;
	double baseline = 0, coupled = 0, separate = 0, production = 0;
	int packets = 0, result = 2;
	float gain = -1;
	BOOL muted = FALSE;
	if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) return 2;
	if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void**)&enumerator))) goto done;
	if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &device))) goto done;
	if (FAILED(IMMDevice_Activate(device, &IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&render))) goto done;
	if (FAILED(IAudioClient_GetMixFormat(render, &format))) goto done;
	if (FAILED(IAudioClient_Initialize(render, AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, format, NULL))) goto done;
	if (FAILED(IAudioClient_GetService(render, &IID_ISimpleAudioVolume, (void**)&volume))) goto done;
	if (FAILED(IAudioClient_GetService(render, &IID_IAudioRenderClient, (void**)&buffer))) goto done;
	if (FAILED(IAudioClient_GetBufferSize(render, &frames))) goto done;
	if (FAILED(IAudioRenderClient_GetBuffer(buffer, frames, &data))) goto done;
	IAudioRenderClient_ReleaseBuffer(buffer, frames, AUDCLNT_BUFFERFLAGS_SILENT);
	if (FAILED(IAudioClient_Start(render))) goto done;
	ISimpleAudioVolume_SetMasterVolume(volume, 1, NULL);
	ISimpleAudioVolume_SetMute(volume, FALSE, NULL);
	baseline = Capture(device, NULL, &packets);
	printf("baseline packets %d rms %.6f\n", packets, baseline);
	ISimpleAudioVolume_SetMasterVolume(volume, 0, NULL);
	ISimpleAudioVolume_SetMute(volume, TRUE, NULL);
	coupled = Capture(device, NULL, &packets);
	printf("muted-default packets %d rms %.6f\n", packets, coupled);
	if (FAILED(CoCreateGuid(&independent))) goto done;
	separate = Capture(device, &independent, &packets);
	printf("muted-independent packets %d rms %.6f\n", packets, separate);
	production = ProductionCapture();
	ISimpleAudioVolume_GetMasterVolume(volume, &gain);
	ISimpleAudioVolume_GetMute(volume, &muted);
	printf("own render STILL gain %.4f mute %d\n", gain, !!muted);
	if (baseline < 0.0001 || separate < 0.0001) result = 77;
	else result = coupled == 0 && production > 0.0001 && gain == 0 && muted ? 0 : 1;
 done:
	if (volume) ISimpleAudioVolume_Release(volume);
	if (buffer) IAudioRenderClient_Release(buffer);
	if (render) { IAudioClient_Stop(render); IAudioClient_Release(render); }
	if (format) CoTaskMemFree(format);
	if (device) IMMDevice_Release(device);
	if (enumerator) IMMDeviceEnumerator_Release(enumerator);
	CoUninitialize();
	return result;
}
