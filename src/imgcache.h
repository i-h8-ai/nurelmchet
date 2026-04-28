#pragma once
#include <stdbool.h>
#include <SDL3/SDL.h>
#include "config.h"

typedef enum { IMG_STICKER, IMG_AVATAR } ImgType;

/* Call once before any other imgcache function. base_url like "https://poop.pub" */
void imgcache_init(const AppConfig *cfg, const char *base_url);
void imgcache_shutdown(void);

/* Queue a download job if the file isn't already cached.
   name = cache key (sticker name or avatar hash).
   url_path = server-relative path like "/uploads/stickers/foo.gif". */
void imgcache_request(const char *name, const char *url_path, ImgType type);

/* Process any downloaded raw files into sized PNGs and SDL textures.
   MUST be called from the GUI/renderer thread each frame. */
void imgcache_process(SDL_Renderer *ren);

/* Retrieve a texture by name. Returns true if ready.
   tex/w/h are output params (may be NULL if you only care about readiness). */
bool imgcache_get(const char *name, ImgType type,
                  SDL_Texture **out_tex, float *out_w, float *out_h);
