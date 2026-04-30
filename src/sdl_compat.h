/* SDL2 ↔ SDL3 compatibility shim — included when USE_SDL2 is defined.
   Maps SDL3-style API calls in the codebase to their SDL2 equivalents. */
#pragma once

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>

/* ── Letter keysym names: SDL3 uppercase, SDL2 lowercase ── */
#define SDLK_V  SDLK_v
#define SDLK_C  SDLK_c

/* ── Event type constants ── */
#define SDL_EVENT_QUIT               SDL_QUIT
#define SDL_EVENT_KEY_DOWN           SDL_KEYDOWN
#define SDL_EVENT_MOUSE_BUTTON_DOWN  SDL_MOUSEBUTTONDOWN
#define SDL_EVENT_MOUSE_BUTTON_UP    SDL_MOUSEBUTTONUP
#define SDL_EVENT_MOUSE_MOTION       SDL_MOUSEMOTION
#define SDL_EVENT_MOUSE_WHEEL        SDL_MOUSEWHEEL
#define SDL_EVENT_TEXT_INPUT         SDL_TEXTINPUT

/* ── Key modifier ── */
#define SDL_KMOD_CTRL  KMOD_CTRL

/* ── Scale mode constant (value ignored — SDL_BlitScaled has no mode arg) ── */
#define SDL_SCALEMODE_LINEAR  1

/* ── SDL_Init: int 0=ok in SDL2, bool in SDL3 ── */
static inline bool compat_SDL_Init(Uint32 flags) { return SDL_Init(flags) == 0; }
#define SDL_Init  compat_SDL_Init

/* ── TTF_Init: int 0=ok in SDL2, bool in SDL3 ── */
static inline bool compat_TTF_Init(void) { return TTF_Init() == 0; }
#define TTF_Init  compat_TTF_Init

/* ── TTF_OpenFont: float ptsize in SDL3, int in SDL2 ── */
static inline TTF_Font *compat_TTF_OpenFont(const char *file, float sz) {
    return TTF_OpenFont(file, (int)sz);
}
#define TTF_OpenFont  compat_TTF_OpenFont

/* ── TTF_GetStringSize → TTF_SizeUTF8 (drops length param) ── */
static inline bool compat_TTF_GetStringSize(TTF_Font *f, const char *t,
                                             size_t l, int *w, int *h) {
    (void)l; return TTF_SizeUTF8(f, t, w, h) == 0;
}
#define TTF_GetStringSize  compat_TTF_GetStringSize

/* ── TTF render: SDL3 has an extra 'length' param after text ── */
#define TTF_RenderText_Blended(f,t,l,c)            TTF_RenderUTF8_Blended(f,t,c)
#define TTF_RenderText_Blended_Wrapped(f,t,l,c,w)  TTF_RenderUTF8_Blended_Wrapped(f,t,c,w)

/* ── SDL_CreateWindow: SDL2 takes x,y position args ── */
static inline SDL_Window *compat_SDL_CreateWindow(const char *title,
                                                   int w, int h, Uint32 flags) {
    return SDL_CreateWindow(title,
                            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                            w, h, flags);
}
#define SDL_CreateWindow  compat_SDL_CreateWindow

/* ── SDL_CreateRenderer: SDL2 uses driver index (-1=auto) + flags ── */
static inline SDL_Renderer *compat_SDL_CreateRenderer(SDL_Window *win,
                                                       const char *name) {
    (void)name;
    return SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
}
#define SDL_CreateRenderer  compat_SDL_CreateRenderer

/* ── VSync ── */
#define SDL_SetRenderVSync(r,v)  SDL_RenderSetVSync(r,v)

/* ── Renderer output size ── */
#define SDL_GetRenderOutputSize(r,w,h)  SDL_GetRendererOutputSize(r,w,h)

/* ── Mouse state: SDL2 returns int coords ── */
static inline Uint32 compat_SDL_GetMouseState(float *x, float *y) {
    int ix = 0, iy = 0;
    Uint32 s = SDL_GetMouseState(&ix, &iy);
    if (x) *x = (float)ix;
    if (y) *y = (float)iy;
    return s;
}
#define SDL_GetMouseState  compat_SDL_GetMouseState

/* ── SDL_RenderFillRect: SDL2 has the …F suffix ── */
#define SDL_RenderFillRect(r,fr)  SDL_RenderFillRectF(r,fr)

/* ── Clip rect ── */
#define SDL_SetRenderClipRect(r,rc)  SDL_RenderSetClipRect(r,rc)

/* ── SDL_RenderTexture: SDL3 takes SDL_FRect* src; SDL2 RenderCopyF needs SDL_Rect* ── */
static inline int compat_SDL_RenderTexture(SDL_Renderer *r, SDL_Texture *t,
                                            const SDL_FRect *src,
                                            const SDL_FRect *dst) {
    if (src) {
        SDL_Rect isrc = {(int)src->x, (int)src->y, (int)src->w, (int)src->h};
        return SDL_RenderCopyF(r, t, &isrc, dst);
    }
    return SDL_RenderCopyF(r, t, NULL, dst);
}
#define SDL_RenderTexture  compat_SDL_RenderTexture

/* ── SDL_GetTextureSize: SDL2 QueryTexture returns int w/h ── */
static inline bool compat_SDL_GetTextureSize(SDL_Texture *t, float *w, float *h) {
    int iw = 0, ih = 0;
    if (SDL_QueryTexture(t, NULL, NULL, &iw, &ih) == 0) {
        if (w) *w = (float)iw;
        if (h) *h = (float)ih;
        return true;
    }
    return false;
}
#define SDL_GetTextureSize  compat_SDL_GetTextureSize

/* ── Surface create/destroy ── */
static inline SDL_Surface *compat_SDL_CreateSurface(int w, int h,
                                                     SDL_PixelFormatEnum fmt) {
    return SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, fmt);
}
#define SDL_CreateSurface   compat_SDL_CreateSurface
#define SDL_DestroySurface  SDL_FreeSurface

/* ── SDL_BlitSurfaceScaled: SDL2 SDL_BlitScaled has no scale-mode arg ── */
static inline int compat_SDL_BlitSurfaceScaled(SDL_Surface *src,
                                                const SDL_Rect *srcrect,
                                                SDL_Surface *dst,
                                                SDL_Rect *dstrect, int mode) {
    (void)mode;
    return SDL_BlitScaled(src, srcrect, dst, dstrect);
}
#define SDL_BlitSurfaceScaled  compat_SDL_BlitSurfaceScaled

/* ── SDL_GetTicks: Uint32 in SDL2, Uint64 in SDL3 ── */
static inline Uint64 compat_SDL_GetTicks(void) { return (Uint64)SDL_GetTicks(); }
#define SDL_GetTicks  compat_SDL_GetTicks

/* ── Text input: SDL2 takes no window arg ── */
static inline void compat_SDL_StartTextInput(SDL_Window *w) {
    (void)w; SDL_StartTextInput();
}
static inline void compat_SDL_StopTextInput(SDL_Window *w) {
    (void)w; SDL_StopTextInput();
}
#define SDL_StartTextInput  compat_SDL_StartTextInput
#define SDL_StopTextInput   compat_SDL_StopTextInput
