/*
 * Minimal SDL2 declarations used by Conduit.
 *
 * Build with -DUSE_SYSTEM_SDL to use the real <SDL2/SDL.h> (CI / device
 * toolchains). Without it, these ABI-compatible declarations let Conduit
 * compile on machines that only have the SDL2 runtime library installed.
 * Everything here mirrors SDL 2.0.x public headers, which are ABI-stable.
 */
#pragma once

#ifdef USE_SYSTEM_SDL
#include <SDL2/SDL.h>
#else

#include <stdint.h>

typedef uint8_t Uint8;
typedef int16_t Sint16;
typedef uint16_t Uint16;
typedef int32_t Sint32;
typedef uint32_t Uint32;
typedef enum { SDL_FALSE = 0, SDL_TRUE = 1 } SDL_bool;

#define SDL_INIT_AUDIO          0x00000010u
#define SDL_INIT_VIDEO          0x00000020u
#define SDL_INIT_JOYSTICK       0x00000200u
#define SDL_INIT_GAMECONTROLLER 0x00002000u
#define SDL_INIT_EVENTS         0x00004000u

/* audio */
typedef Uint16 SDL_AudioFormat;
#define AUDIO_F32SYS 0x8120
typedef void (*SDL_AudioCallback)(void *userdata, Uint8 *stream, int len);
typedef struct SDL_AudioSpec {
    int freq;
    SDL_AudioFormat format;
    Uint8 channels;
    Uint8 silence;
    Uint16 samples;
    Uint16 padding;
    Uint32 size;
    SDL_AudioCallback callback;
    void *userdata;
} SDL_AudioSpec;
typedef Uint32 SDL_AudioDeviceID;

int SDL_Init(Uint32 flags);
void SDL_Quit(void);
const char *SDL_GetError(void);
SDL_bool SDL_SetHint(const char *name, const char *value);
Uint32 SDL_GetTicks(void);
void SDL_Delay(Uint32 ms);

int SDL_GetNumAudioDevices(int iscapture);
const char *SDL_GetAudioDeviceName(int index, int iscapture);
const char *SDL_GetCurrentAudioDriver(void);
SDL_AudioDeviceID SDL_OpenAudioDevice(const char *device, int iscapture,
                                      const SDL_AudioSpec *desired,
                                      SDL_AudioSpec *obtained,
                                      int allowed_changes);
void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on);
void SDL_LockAudioDevice(SDL_AudioDeviceID dev);
void SDL_UnlockAudioDevice(SDL_AudioDeviceID dev);
void SDL_CloseAudioDevice(SDL_AudioDeviceID dev);

/* video */
typedef struct SDL_Window SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture SDL_Texture;
typedef struct SDL_Rect { int x, y, w, h; } SDL_Rect;
#define SDL_WINDOWPOS_UNDEFINED 0x1FFF0000
#define SDL_WINDOW_FULLSCREEN_DESKTOP 0x00001001u
#define SDL_WINDOW_SHOWN 0x00000004u
#define SDL_RENDERER_SOFTWARE 0x1u
#define SDL_RENDERER_ACCELERATED 0x2u
#define SDL_RENDERER_PRESENTVSYNC 0x4u
#define SDL_PIXELFORMAT_ARGB8888 0x16362004u
#define SDL_TEXTUREACCESS_STATIC 0
typedef enum { SDL_BLENDMODE_NONE = 0, SDL_BLENDMODE_BLEND = 1 } SDL_BlendMode;

SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags);
void SDL_DestroyWindow(SDL_Window *w);
SDL_Renderer *SDL_CreateRenderer(SDL_Window *w, int index, Uint32 flags);
void SDL_DestroyRenderer(SDL_Renderer *r);
int SDL_RenderSetLogicalSize(SDL_Renderer *r, int w, int h);
int SDL_SetRenderDrawColor(SDL_Renderer *r, Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca);
int SDL_SetRenderDrawBlendMode(SDL_Renderer *r, SDL_BlendMode m);
int SDL_RenderClear(SDL_Renderer *r);
int SDL_RenderFillRect(SDL_Renderer *r, const SDL_Rect *rect);
int SDL_RenderDrawRect(SDL_Renderer *r, const SDL_Rect *rect);
int SDL_RenderDrawLine(SDL_Renderer *r, int x1, int y1, int x2, int y2);
int SDL_RenderCopy(SDL_Renderer *r, SDL_Texture *t, const SDL_Rect *src, const SDL_Rect *dst);
void SDL_RenderPresent(SDL_Renderer *r);
SDL_Texture *SDL_CreateTexture(SDL_Renderer *r, Uint32 format, int access, int w, int h);
int SDL_UpdateTexture(SDL_Texture *t, const SDL_Rect *rect, const void *pixels, int pitch);
int SDL_SetTextureBlendMode(SDL_Texture *t, SDL_BlendMode m);
int SDL_SetTextureColorMod(SDL_Texture *t, Uint8 r, Uint8 g, Uint8 b);
int SDL_SetTextureAlphaMod(SDL_Texture *t, Uint8 a);
void SDL_DestroyTexture(SDL_Texture *t);
int SDL_ShowCursor(int toggle);
int SDL_RenderReadPixels(SDL_Renderer *r, const SDL_Rect *rect, Uint32 format, void *pixels, int pitch);

/* input */
typedef struct _SDL_Joystick SDL_Joystick;
typedef struct _SDL_GameController SDL_GameController;
int SDL_NumJoysticks(void);
SDL_bool SDL_IsGameController(int joystick_index);
SDL_GameController *SDL_GameControllerOpen(int joystick_index);
SDL_Joystick *SDL_JoystickOpen(int device_index);
const char *SDL_JoystickNameForIndex(int device_index);

enum {
    SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X,
    SDL_CONTROLLER_BUTTON_Y, SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_GUIDE,
    SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_LEFTSTICK,
    SDL_CONTROLLER_BUTTON_RIGHTSTICK, SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
    SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, SDL_CONTROLLER_BUTTON_DPAD_UP,
    SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT,
    SDL_CONTROLLER_BUTTON_DPAD_RIGHT
};
enum { SDL_CONTROLLER_AXIS_TRIGGERLEFT = 4, SDL_CONTROLLER_AXIS_TRIGGERRIGHT = 5 };

#define SDLK_BACKSPACE 8
#define SDLK_TAB 9
#define SDLK_RETURN 13
#define SDLK_ESCAPE 27
#define SDLK_SPACE 32
#define SDLK_RIGHT 0x4000004F
#define SDLK_LEFT 0x40000050
#define SDLK_DOWN 0x40000051
#define SDLK_UP 0x40000052
#define SDLK_RSHIFT 0x400000E5
#define SDLK_LSHIFT 0x400000E1
#define SDLK_LCTRL 0x400000E0

enum {
    SDL_QUIT = 0x100,
    SDL_KEYDOWN = 0x300, SDL_KEYUP = 0x301,
    SDL_JOYAXISMOTION = 0x600, SDL_JOYHATMOTION = 0x602,
    SDL_JOYBUTTONDOWN = 0x603, SDL_JOYBUTTONUP = 0x604,
    SDL_JOYDEVICEADDED = 0x605,
    SDL_CONTROLLERAXISMOTION = 0x650, SDL_CONTROLLERBUTTONDOWN = 0x651,
    SDL_CONTROLLERBUTTONUP = 0x652, SDL_CONTROLLERDEVICEADDED = 0x653,
    SDL_AUDIODEVICEADDED = 0x1100, SDL_AUDIODEVICEREMOVED = 0x1101
};
#define SDL_HAT_UP 1
#define SDL_HAT_RIGHT 2
#define SDL_HAT_DOWN 4
#define SDL_HAT_LEFT 8

typedef struct { Sint32 scancode; Sint32 sym; Uint16 mod; Uint32 unused; } SDL_Keysym;
typedef struct { Uint32 type, timestamp, windowID; Uint8 state, repeat, p2, p3; SDL_Keysym keysym; } SDL_KeyboardEvent;
typedef struct { Uint32 type, timestamp; Sint32 which; Uint8 button, state, p1, p2; } SDL_ButtonEventX;
typedef struct { Uint32 type, timestamp; Sint32 which; Uint8 axis, p1, p2, p3; Sint16 value; Uint16 p4; } SDL_AxisEventX;
typedef struct { Uint32 type, timestamp; Sint32 which; Uint8 hat, value, p1, p2; } SDL_JoyHatEvent;
typedef struct { Uint32 type, timestamp; Uint32 which; Uint8 iscapture, p1, p2, p3; } SDL_AudioDeviceEvent;
typedef union SDL_Event {
    Uint32 type;
    SDL_KeyboardEvent key;
    SDL_ButtonEventX cbutton;   /* controller button */
    SDL_ButtonEventX jbutton;   /* joystick button */
    SDL_AxisEventX caxis;
    SDL_AxisEventX jaxis;
    SDL_JoyHatEvent jhat;
    SDL_AudioDeviceEvent adevice;
    Uint8 padding[56];
} SDL_Event;
int SDL_PollEvent(SDL_Event *e);

#endif
