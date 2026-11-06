// The part of SDL3's audio API that TotK Recomp's audio_mixer.c uses, on WASAPI (Windows' native audio): one stereo
// float stream to the default output device, pulled by the mixer's callback. A thread waits for the device's buffer
// event, asks the callback for exactly the free space, and hands it to the device. Shared mode with the system's
// sample rate conversion, so 48 kHz plays on any device; when the device goes away (headphones unplugged, another
// default chosen) the stream reopens on the new default.
#define COBJMACROS
#include <initguid.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "host.h"

DEFINE_GUID(NX2_CLSID_MMDeviceEnumerator, 0xbcde0395, 0xe52f, 0x467c, 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e);
DEFINE_GUID(NX2_IID_IMMDeviceEnumerator, 0xa95664d2, 0x9614, 0x4f35, 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6);
DEFINE_GUID(NX2_IID_IAudioClient, 0x1cb9ad4c, 0xdbfa, 0x4c32, 0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2);
DEFINE_GUID(NX2_IID_IAudioRenderClient, 0xf294acfc, 0x3146, 0x4483, 0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2);
DEFINE_GUID(NX2_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

struct SDL_AudioStream {
    SDL_AudioStreamCallback cb;
    void *user;
    int rate;
    HANDLE thread, event, started;
    volatile LONG quit, running;
    int period_frames;
    float *dst;            // inside the device buffer while the callback runs
    int dst_left;          // its free bytes
};

static const char *last_error = "no error";
static char device_name[64] = "Windows default output";

bool SDL_InitSubSystem(uint32_t flags) { (void)flags; return true; }
void SDL_QuitSubSystem(uint32_t flags) { (void)flags; }
const char *SDL_GetError(void) { return last_error; }

// Opens the default device; 0 on failure. Out: client, render client, buffer frames.
static int open_device(SDL_AudioStream *s, IAudioClient **ac, IAudioRenderClient **rc, UINT32 *frames) {
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    *ac = NULL, *rc = NULL;
    if (FAILED(CoCreateInstance(&NX2_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &NX2_IID_IMMDeviceEnumerator, (void **)&en)))
        return last_error = "no device enumerator", 0;
    HRESULT hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    IMMDeviceEnumerator_Release(en);
    if (FAILED(hr)) return last_error = "no default output device", 0;
    hr = IMMDevice_Activate(dev, &NX2_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)ac);
    IMMDevice_Release(dev);
    if (FAILED(hr)) return last_error = "the output device cannot be activated", 0;
    WAVEFORMATEXTENSIBLE wf = {0};
    wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wf.Format.nChannels = 2;
    wf.Format.nSamplesPerSec = (DWORD)s->rate;
    wf.Format.wBitsPerSample = 32;
    wf.Format.nBlockAlign = 8;
    wf.Format.nAvgBytesPerSec = (DWORD)s->rate * 8;
    wf.Format.cbSize = sizeof wf - sizeof wf.Format;
    wf.Samples.wValidBitsPerSample = 32;
    wf.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    wf.SubFormat = NX2_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    REFERENCE_TIME period = 0, minp = 0;
    IAudioClient_GetDevicePeriod(*ac, &period, &minp);
    REFERENCE_TIME buffer = 400000;   // 40 ms: room for the mixer's pacing
    hr = IAudioClient_Initialize(*ac, AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                     AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                 buffer, 0, (WAVEFORMATEX *)&wf, NULL);
    if (FAILED(hr)) {
        IAudioClient_Release(*ac), *ac = NULL;
        return last_error = "the output device refused 48 kHz stereo float", 0;
    }
    IAudioClient_SetEventHandle(*ac, s->event);
    IAudioClient_GetBufferSize(*ac, frames);
    if (FAILED(IAudioClient_GetService(*ac, &NX2_IID_IAudioRenderClient, (void **)rc))) {
        IAudioClient_Release(*ac), *ac = NULL;
        return last_error = "no render client", 0;
    }
    s->period_frames = (int)(period * s->rate / 10000000);
    if (s->period_frames <= 0) s->period_frames = s->rate / 100;
    return 1;
}

static DWORD WINAPI audio_thread(LPVOID arg) {
    SDL_AudioStream *s = arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    HANDLE task = NULL;
    {   // MMCSS "Pro Audio" scheduling when available
        typedef HANDLE(WINAPI * AvSet)(LPCWSTR, LPDWORD);
        HMODULE av = LoadLibraryA("avrt.dll");
        AvSet set = av ? (AvSet)(void *)GetProcAddress(av, "AvSetMmThreadCharacteristicsW") : NULL;
        DWORD idx = 0;
        if (set) task = set(L"Pro Audio", &idx);
        if (!task) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    }
    IAudioClient *ac = NULL;
    IAudioRenderClient *rc = NULL;
    UINT32 frames = 0;
    int ok = open_device(s, &ac, &rc, &frames);
    InterlockedExchange(&s->running, ok ? 1 : -1);
    SetEvent(s->started);
    if (!ok) return 0;
    int playing = 0;
    while (!s->quit) {
        if (!playing && s->running == 2) {
            IAudioClient_Start(ac);
            playing = 1;
        }
        WaitForSingleObject(s->event, 100);
        if (!playing) continue;
        UINT32 pad = 0;
        HRESULT hr = IAudioClient_GetCurrentPadding(ac, &pad);
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING) {   // the device went away
            IAudioRenderClient_Release(rc), IAudioClient_Release(ac);
            rt_log("audio: output device lost; reopening the default one");
            while (!s->quit && !open_device(s, &ac, &rc, &frames)) Sleep(500);
            if (s->quit) return 0;
            IAudioClient_Start(ac);
            continue;
        }
        if (FAILED(hr) || pad >= frames) continue;
        UINT32 avail = frames - pad;
        BYTE *buf = NULL;
        if (FAILED(IAudioRenderClient_GetBuffer(rc, avail, &buf))) continue;
        memset(buf, 0, (size_t)avail * 8);
        s->dst = (float *)buf, s->dst_left = (int)avail * 8;
        s->cb(s->user, s, (int)avail * 8, (int)avail * 8);
        s->dst = NULL;
        IAudioRenderClient_ReleaseBuffer(rc, avail, 0);
    }
    if (playing) IAudioClient_Stop(ac);
    IAudioRenderClient_Release(rc);
    IAudioClient_Release(ac);
    CoUninitialize();
    return 0;
}

SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID d, const SDL_AudioSpec *spec, SDL_AudioStreamCallback cb,
                                           void *user) {
    (void)d;
    if (!spec || spec->format != SDL_AUDIO_F32 || spec->channels != 2 || !cb) return last_error = "unsupported format", NULL;
    SDL_AudioStream *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->cb = cb, s->user = user, s->rate = spec->freq;
    s->event = CreateEventA(NULL, FALSE, FALSE, NULL);
    s->started = CreateEventA(NULL, TRUE, FALSE, NULL);
    s->thread = CreateThread(NULL, 0, audio_thread, s, 0, NULL);
    WaitForSingleObject(s->started, 5000);
    if (s->running != 1) {
        InterlockedExchange(&s->quit, 1);
        SetEvent(s->event);
        WaitForSingleObject(s->thread, 2000);
        CloseHandle(s->thread), CloseHandle(s->event), CloseHandle(s->started);
        free(s);
        return NULL;
    }
    return s;
}

bool SDL_PutAudioStreamData(SDL_AudioStream *s, const void *b, int n) {   // from the callback: into the device buffer
    if (!s || !s->dst || n > s->dst_left) return false;
    memcpy(s->dst, b, (size_t)n);
    s->dst = (float *)((uint8_t *)s->dst + n);
    s->dst_left -= n;
    return true;
}

bool SDL_ResumeAudioStreamDevice(SDL_AudioStream *s) {
    if (!s) return false;
    InterlockedExchange(&s->running, 2);
    SetEvent(s->event);
    return true;
}

void SDL_DestroyAudioStream(SDL_AudioStream *s) {
    if (!s) return;
    InterlockedExchange(&s->quit, 1);
    SetEvent(s->event);
    WaitForSingleObject(s->thread, 2000);
    CloseHandle(s->thread), CloseHandle(s->event), CloseHandle(s->started);
    free(s);
}

static SDL_AudioStream *last_stream;   // SDL_GetAudioStreamDevice's answer refers to it
SDL_AudioDeviceID SDL_GetAudioStreamDevice(SDL_AudioStream *s) {
    last_stream = s;
    return 1;
}
const char *SDL_GetAudioDeviceName(SDL_AudioDeviceID d) { (void)d; return device_name; }
bool SDL_GetAudioDeviceFormat(SDL_AudioDeviceID d, SDL_AudioSpec *spec, int *frames) {
    (void)d;
    if (!last_stream) return false;
    if (spec) spec->format = SDL_AUDIO_F32, spec->channels = 2, spec->freq = last_stream->rate;
    if (frames) *frames = last_stream->period_frames;
    return true;
}
