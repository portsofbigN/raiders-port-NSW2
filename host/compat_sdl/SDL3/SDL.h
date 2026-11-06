// Minimal stand-in for SDL3's audio API, used by TotK Recomp's audio_mixer.c: the functions it calls, implemented on
// WASAPI in compat_sdl/sdl_wasapi.c (one stereo float stream to the default output device, pulled by a callback).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SDLCALL __cdecl
#define SDL_INIT_AUDIO 0x00000010u
#define SDL_AUDIO_F32 0x8120u
#define SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK ((SDL_AudioDeviceID)0xFFFFFFFFu)

typedef uint32_t SDL_AudioDeviceID;
typedef int SDL_AudioFormat;
typedef struct SDL_AudioStream SDL_AudioStream;
typedef struct { SDL_AudioFormat format; int channels; int freq; } SDL_AudioSpec;
typedef void(SDLCALL *SDL_AudioStreamCallback)(void *userdata, SDL_AudioStream *stream, int additional_amount,
                                               int total_amount);

bool SDL_InitSubSystem(uint32_t flags);
void SDL_QuitSubSystem(uint32_t flags);
const char *SDL_GetError(void);
SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID d, const SDL_AudioSpec *s, SDL_AudioStreamCallback cb, void *u);
bool SDL_PutAudioStreamData(SDL_AudioStream *s, const void *b, int n);
bool SDL_ResumeAudioStreamDevice(SDL_AudioStream *s);
void SDL_DestroyAudioStream(SDL_AudioStream *s);
SDL_AudioDeviceID SDL_GetAudioStreamDevice(SDL_AudioStream *s);
const char *SDL_GetAudioDeviceName(SDL_AudioDeviceID d);
bool SDL_GetAudioDeviceFormat(SDL_AudioDeviceID d, SDL_AudioSpec *s, int *frames);
