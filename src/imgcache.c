#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>

#include <curl/curl.h>
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include "imgcache.h"
#include "config.h"

/* ── Internal state ───────────────────────────────────────────────────── */

typedef enum {
    IMGST_PENDING,
    IMGST_DOWNLOADING,
    IMGST_DOWNLOADED,
    IMGST_READY,
    IMGST_FAILED,
} ImgState;

typedef struct {
    char     name[64];
    char     url_path[256];
    ImgType  type;
    ImgState state;
    /* one entry per animation frame; frame_count=1 for static images */
    SDL_Texture **frames;
    int         *delays_ms;
    int          frame_count;
    /* animation playback state — GUI thread only, no mutex needed */
    int          cur_frame;
    Uint64       last_frame_ms;
    float    w, h;
} ImgEntry;

#define IMG_CACHE_MAX 256
static ImgEntry g_entries[IMG_CACHE_MAX];
static int      g_entry_count = 0;

static AppConfig g_cfg;
static char      g_base_url[256];

#define DL_QUEUE_MAX 128
static int g_dl_queue[DL_QUEUE_MAX];
static int g_dl_head = 0, g_dl_tail = 0;

static pthread_mutex_t g_mu       = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_dl_cond  = PTHREAD_COND_INITIALIZER;
static pthread_t        g_dl_thread;
static bool             g_dl_running = false;

/* ── Filesystem helpers ───────────────────────────────────────────────── */

static void mkdirp(const char *path) {
    char tmp[512];
    strncpy(tmp, path, sizeof(tmp) - 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static bool file_exists(const char *path) {
    return access(path, F_OK) == 0;
}

/* ── Path builders ────────────────────────────────────────────────────── */

static const char *type_subdir(ImgType t) {
    return (t == IMG_STICKER) ? "stickers" : "avatars";
}

static void sized_path(char *dst, size_t dstlen,
                       const char *name, ImgType type) {
    int sz = (type == IMG_STICKER) ? g_cfg.sticker_size : g_cfg.avatar_size;
    snprintf(dst, dstlen, "%s/%s/%s_%d.png",
             g_cfg.cache_dir, type_subdir(type), name, sz);
}

static void raw_path(char *dst, size_t dstlen,
                     const char *name, ImgType type, const char *url_path) {
    const char *ext = strrchr(url_path, '.');
    if (!ext) ext = ".bin";
    snprintf(dst, dstlen, "%s/%s/raw/%s%s",
             g_cfg.cache_dir, type_subdir(type), name, ext);
}

static bool url_is_gif(const char *url_path) {
    const char *ext = strrchr(url_path, '.');
    return ext && strcasecmp(ext, ".gif") == 0;
}

/* ── Curl download ────────────────────────────────────────────────────── */

static size_t write_to_file(void *ptr, size_t size, size_t nmemb, void *ud) {
    return fwrite(ptr, size, nmemb, (FILE *)ud);
}

static bool download_file(const char *url, const char *dest) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", dest);

    FILE *f = fopen(tmp, "wb");
    if (!f) return false;

    CURL *curl = curl_easy_init();
    if (!curl) { fclose(f); remove(tmp); return false; }

    curl_easy_setopt(curl, CURLOPT_URL,            url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_to_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      f);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,      "nurealmschat/1.0");

    CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    fclose(f);

    if (rc != CURLE_OK) {
        fprintf(stderr, "[img] download failed %s: %s\n", url, curl_easy_strerror(rc));
        remove(tmp);
        return false;
    }
    if (rename(tmp, dest) != 0) {
        remove(tmp);
        return false;
    }
    return true;
}

/* ── Download thread ──────────────────────────────────────────────────── */

static void do_download(ImgEntry *e) {
    char raw[512], full_url[512], dir[512];

    raw_path(raw, sizeof(raw), e->name, e->type, e->url_path);

    snprintf(dir, sizeof(dir), "%s/%s/raw", g_cfg.cache_dir, type_subdir(e->type));
    mkdirp(dir);

    if (file_exists(raw)) {
        pthread_mutex_lock(&g_mu);
        e->state = IMGST_DOWNLOADED;
        pthread_mutex_unlock(&g_mu);
        return;
    }

    snprintf(full_url, sizeof(full_url), "%s%s", g_base_url, e->url_path);
    fprintf(stderr, "[img] downloading %s\n", full_url);

    bool ok = download_file(full_url, raw);
    pthread_mutex_lock(&g_mu);
    e->state = ok ? IMGST_DOWNLOADED : IMGST_FAILED;
    pthread_mutex_unlock(&g_mu);
}

static void *dl_thread_func(void *arg) {
    (void)arg;
    while (g_dl_running) {
        pthread_mutex_lock(&g_mu);
        while (g_dl_head == g_dl_tail && g_dl_running)
            pthread_cond_wait(&g_dl_cond, &g_mu);
        if (!g_dl_running) { pthread_mutex_unlock(&g_mu); break; }
        int idx = g_dl_queue[g_dl_head % DL_QUEUE_MAX];
        g_dl_head++;
        pthread_mutex_unlock(&g_mu);
        do_download(&g_entries[idx]);
    }
    return NULL;
}

/* ── Scale helpers ────────────────────────────────────────────────────── */

static void compute_scaled_size(int orig_w, int orig_h, int max_sz,
                                int *out_w, int *out_h) {
    int sw = orig_w, sh = orig_h;
    if (sw > max_sz || sh > max_sz) {
        if (sw > sh) { sh = sh * max_sz / sw; sw = max_sz; }
        else         { sw = sw * max_sz / sh; sh = max_sz; }
        if (sh < 1) sh = 1;
        if (sw < 1) sw = 1;
    }
    *out_w = sw; *out_h = sh;
}

/* ── Entry state helpers ──────────────────────────────────────────────── */

static void entry_set_ready(ImgEntry *e, SDL_Texture **frames,
                             int *delays_ms, int count, float w, float h) {
    pthread_mutex_lock(&g_mu);
    e->frames        = frames;
    e->delays_ms     = delays_ms;
    e->frame_count   = count;
    e->w             = w;
    e->h             = h;
    e->cur_frame     = 0;
    e->last_frame_ms = SDL_GetTicks();
    e->state         = IMGST_READY;
    pthread_mutex_unlock(&g_mu);
}

static void entry_set_failed(ImgEntry *e) {
    pthread_mutex_lock(&g_mu);
    e->state = IMGST_FAILED;
    pthread_mutex_unlock(&g_mu);
}

/* ── imgcache_process: GIF animation path ─────────────────────────────── */

static void process_gif(SDL_Renderer *ren, ImgEntry *e, const char *rp) {
    IMG_Animation *anim = IMG_LoadAnimation(rp);
    if (!anim || anim->count == 0) {
        if (anim) IMG_FreeAnimation(anim);
        entry_set_failed(e);
        return;
    }

    int max_sz = (e->type == IMG_STICKER) ? g_cfg.sticker_size : g_cfg.avatar_size;
    int sw, sh;
    compute_scaled_size(anim->w, anim->h, max_sz, &sw, &sh);

    int count            = anim->count;
    SDL_Texture **frames = calloc(count, sizeof(SDL_Texture *));
    int *delays          = calloc(count, sizeof(int));
    bool ok              = (frames && delays);

    for (int f = 0; f < count && ok; f++) {
        delays[f] = anim->delays[f] > 0 ? anim->delays[f] : 100;

        SDL_Surface *scaled = SDL_CreateSurface(sw, sh, SDL_PIXELFORMAT_RGBA32);
        if (!scaled) { ok = false; break; }
        SDL_Rect dr = {0, 0, sw, sh};
        SDL_BlitSurfaceScaled(anim->frames[f], NULL, scaled, &dr, SDL_SCALEMODE_LINEAR);
        frames[f] = SDL_CreateTextureFromSurface(ren, scaled);
        SDL_DestroySurface(scaled);
        if (!frames[f]) { ok = false; }
    }

    IMG_FreeAnimation(anim);

    if (!ok) {
        for (int f = 0; f < count; f++)
            if (frames && frames[f]) SDL_DestroyTexture(frames[f]);
        free(frames); free(delays);
        entry_set_failed(e);
        return;
    }

    entry_set_ready(e, frames, delays, count, (float)sw, (float)sh);
    fprintf(stderr, "[img] gif ready: %s (%d frames)\n", e->name, count);
}

/* ── imgcache_process: static image path ─────────────────────────────── */

static void process_static(SDL_Renderer *ren, ImgEntry *e,
                            const char *sp, const char *rp) {
    int max_sz = (e->type == IMG_STICKER) ? g_cfg.sticker_size : g_cfg.avatar_size;
    SDL_Surface *surf = NULL;

    if (file_exists(sp)) {
        surf = IMG_Load(sp);
    } else {
        if (!file_exists(rp)) return; /* raw not ready yet — try next frame */

        SDL_Surface *raw = IMG_Load(rp);
        if (!raw) {
            fprintf(stderr, "[img] IMG_Load failed %s: %s\n", rp, SDL_GetError());
            entry_set_failed(e);
            return;
        }
        int sw, sh;
        compute_scaled_size(raw->w, raw->h, max_sz, &sw, &sh);

        SDL_Surface *scaled = SDL_CreateSurface(sw, sh, SDL_PIXELFORMAT_RGBA32);
        if (!scaled) { SDL_DestroySurface(raw); entry_set_failed(e); return; }

        SDL_Rect dr = {0, 0, sw, sh};
        SDL_BlitSurfaceScaled(raw, NULL, scaled, &dr, SDL_SCALEMODE_LINEAR);
        SDL_DestroySurface(raw);

        /* cache the sized PNG for future runs */
        char sdir[512];
        snprintf(sdir, sizeof(sdir), "%s/%s", g_cfg.cache_dir, type_subdir(e->type));
        mkdirp(sdir);
        IMG_SavePNG(scaled, sp);

        surf = scaled;
    }

    if (!surf) { entry_set_failed(e); return; }

    SDL_Texture *tex = SDL_CreateTextureFromSurface(ren, surf);
    float tw = (float)surf->w, th = (float)surf->h;
    SDL_DestroySurface(surf);

    if (!tex) { entry_set_failed(e); return; }

    SDL_Texture **frames = malloc(sizeof(SDL_Texture *));
    int *delays          = calloc(1, sizeof(int));
    if (!frames || !delays) {
        free(frames); free(delays);
        SDL_DestroyTexture(tex);
        entry_set_failed(e);
        return;
    }
    frames[0] = tex;
    delays[0]  = 0;
    entry_set_ready(e, frames, delays, 1, tw, th);
}

/* ── Public API ───────────────────────────────────────────────────────── */

void imgcache_init(const AppConfig *cfg, const char *base_url) {
    g_cfg = *cfg;
    strncpy(g_base_url, base_url, sizeof(g_base_url) - 1);

    char d[512];
    snprintf(d, sizeof(d), "%s/stickers", cfg->cache_dir); mkdirp(d);
    snprintf(d, sizeof(d), "%s/avatars",  cfg->cache_dir); mkdirp(d);

    g_dl_running = true;
    pthread_create(&g_dl_thread, NULL, dl_thread_func, NULL);
}

void imgcache_shutdown(void) {
    pthread_mutex_lock(&g_mu);
    g_dl_running = false;
    pthread_cond_signal(&g_dl_cond);
    pthread_mutex_unlock(&g_mu);
    pthread_join(g_dl_thread, NULL);

    for (int i = 0; i < g_entry_count; i++) {
        ImgEntry *e = &g_entries[i];
        for (int f = 0; f < e->frame_count; f++)
            if (e->frames && e->frames[f]) SDL_DestroyTexture(e->frames[f]);
        free(e->frames);
        free(e->delays_ms);
    }
}

void imgcache_request(const char *name, const char *url_path, ImgType type) {
    if (!name || !name[0] || !url_path || !url_path[0]) return;

    bool enabled = (type == IMG_STICKER) ? g_cfg.download_stickers
                                         : g_cfg.download_avatars;

    pthread_mutex_lock(&g_mu);

    for (int i = 0; i < g_entry_count; i++) {
        if (g_entries[i].type == type && strcmp(g_entries[i].name, name) == 0) {
            pthread_mutex_unlock(&g_mu);
            return;
        }
    }

    if (g_entry_count >= IMG_CACHE_MAX) { pthread_mutex_unlock(&g_mu); return; }

    int idx = g_entry_count++;
    ImgEntry *e = &g_entries[idx];
    memset(e, 0, sizeof(*e));
    strncpy(e->name,     name,     sizeof(e->name)     - 1);
    strncpy(e->url_path, url_path, sizeof(e->url_path) - 1);
    e->type = type;

    /* GIFs are never cached as sized PNGs — check raw file or queue download */
    if (!url_is_gif(url_path)) {
        char sp[512];
        sized_path(sp, sizeof(sp), name, type);
        if (file_exists(sp)) {
            e->state = IMGST_DOWNLOADED;
            pthread_mutex_unlock(&g_mu);
            return;
        }
    }

    if (!enabled) { e->state = IMGST_FAILED; pthread_mutex_unlock(&g_mu); return; }

    e->state = IMGST_PENDING;
    g_dl_queue[g_dl_tail % DL_QUEUE_MAX] = idx;
    g_dl_tail++;
    pthread_cond_signal(&g_dl_cond);
    pthread_mutex_unlock(&g_mu);
}

/* Called from GUI thread each frame */
void imgcache_process(SDL_Renderer *ren) {
    for (int i = 0; i < g_entry_count; i++) {
        pthread_mutex_lock(&g_mu);
        ImgState st = g_entries[i].state;
        pthread_mutex_unlock(&g_mu);

        if (st != IMGST_DOWNLOADED) continue;

        ImgEntry *e = &g_entries[i];
        char sp[512], rp[512];
        sized_path(sp, sizeof(sp), e->name, e->type);
        raw_path(rp, sizeof(rp), e->name, e->type, e->url_path);

        if (url_is_gif(e->url_path)) {
            if (!file_exists(rp)) continue;
            process_gif(ren, e, rp);
        } else {
            process_static(ren, e, sp, rp);
        }
    }
}

bool imgcache_get(const char *name, ImgType type,
                  SDL_Texture **out_tex, float *out_w, float *out_h) {
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_entry_count; i++) {
        if (g_entries[i].type == type && strcmp(g_entries[i].name, name) == 0) {
            ImgEntry *e  = &g_entries[i];
            bool ready   = (e->state == IMGST_READY) && (e->frame_count > 0);
            if (ready) {
                /* advance animation frame based on wall time */
                if (e->frame_count > 1) {
                    Uint64 now   = SDL_GetTicks();
                    int    delay = e->delays_ms[e->cur_frame];
                    if ((Uint64)(now - e->last_frame_ms) >= (Uint64)delay) {
                        e->cur_frame = (e->cur_frame + 1) % e->frame_count;
                        e->last_frame_ms = now;
                    }
                }
                if (out_tex) *out_tex = e->frames[e->cur_frame];
                if (out_w)   *out_w   = e->w;
                if (out_h)   *out_h   = e->h;
            } else {
                if (out_tex) *out_tex = NULL;
                if (out_w)   *out_w   = 0;
                if (out_h)   *out_h   = 0;
            }
            pthread_mutex_unlock(&g_mu);
            return ready;
        }
    }
    pthread_mutex_unlock(&g_mu);
    if (out_tex) *out_tex = NULL;
    if (out_w)   *out_w   = 0;
    if (out_h)   *out_h   = 0;
    return false;
}
