#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <time.h>

#ifdef USE_SDL2
#include "sdl_compat.h"
#else
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#endif

#include "chat.h"
#include "config.h"
#include "imgcache.h"
#include "gui.h"

/* ── Fixed layout constants ───────────────────────────────────────────── */
#define TITLE_H      30
#define BTNBAR_H     44
#define INPUT_H      52
#define MSG_Y        (TITLE_H + BTNBAR_H)
#define SCROLLBAR_W  10
#define FONT_PATH    "/usr/share/fonts/noto/NotoSansMono-Regular.ttf"
#define FONT_SIZE    14.0f
#define LINE_PAD     3
#define MSG_PAD_X    8
#define INPUT_MAXLEN 480

/* ── Dynamic layout (updated each frame) ─────────────────────────────── */
static int g_win_w = 1000;
static int g_win_h = 680;

static inline int cmsg_h(void)   { return g_win_h - TITLE_H - BTNBAR_H - INPUT_H; }
static inline int cinput_y(void) { return g_win_h - INPUT_H; }

/* ── Colors ───────────────────────────────────────────────────────────── */
#ifndef COL
#define COL(r,g,b,a) ((SDL_Color){r,g,b,a})
#endif
static const SDL_Color C_BG         = COL(0x1E,0x1E,0x2E,0xFF);
static const SDL_Color C_TITLE      = COL(0x28,0x28,0x3E,0xFF);
static const SDL_Color C_BTNBAR     = COL(0x22,0x22,0x32,0xFF);
static const SDL_Color C_MSGBG      = COL(0x1A,0x1A,0x2A,0xFF);
static const SDL_Color C_INPUTBG    = COL(0x22,0x22,0x32,0xFF);
static const SDL_Color C_INPUTFLD   = COL(0x30,0x30,0x48,0xFF);
static const SDL_Color C_TEXT       = COL(0xCD,0xD6,0xF4,0xFF);
static const SDL_Color C_DIM        = COL(0x6C,0x70,0x86,0xFF);
static const SDL_Color C_SYSTEM     = COL(0x74,0x74,0x8E,0xFF);
static const SDL_Color C_CURSOR     = COL(0x00,0xFF,0x88,0xFF);
static const SDL_Color C_INPUTBDR   = COL(0x45,0x85,0x88,0xFF);
static const SDL_Color C_SEPLINE    = COL(0x33,0x33,0x50,0xFF);
static const SDL_Color C_SCROLLBAR  = COL(0x25,0x25,0x38,0xFF);
static const SDL_Color C_SCROLLTHUMB= COL(0x55,0x55,0x80,0xFF);
static const SDL_Color C_SCROLLHOV  = COL(0x70,0x70,0xA8,0xFF);
static const SDL_Color C_BTN_AUTH   = COL(0x20,0x8C,0x56,0xFF);
static const SDL_Color C_BTN_JOIN   = COL(0x20,0x76,0xB0,0xFF);
static const SDL_Color C_BTN_HIST   = COL(0x7C,0x3A,0xAA,0xFF);
static const SDL_Color C_BTN_PART   = COL(0xC0,0x6E,0x14,0xFF);
static const SDL_Color C_BTN_SEND   = COL(0x20,0x8C,0x56,0xFF);
static const SDL_Color C_BTN_HOVER  = COL(0xFF,0xFF,0xFF,0x28);

/* ── Types ────────────────────────────────────────────────────────────── */
typedef struct {
    SDL_FRect   rect;
    SDL_Color   base_color;
    const char *label;
    CommandType cmd;
    bool        hovered;
} Button;

/* Per-message render data */
typedef struct {
    SDL_Texture *text_tex;
    float        text_w, text_h;
    /* sticker/avatar keys for imgcache lookup */
    char         sticker_names[MAX_STICKERS_PER_MSG][64];
    int          sticker_count;
    char         avatar_key[128];   /* "" if none */
    /* pre-computed total pixel height for this slot */
    float        total_h;
} MsgTex;

/* ── Globals ──────────────────────────────────────────────────────────── */
static SDL_Window   *g_win       = NULL;
static SDL_Renderer *g_ren       = NULL;
static TTF_Font     *g_font      = NULL;
static int           g_line_h    = 0;
static MsgTex        g_msg_tex[MAX_MESSAGES];
static int           g_last_textured = 0;

/* Pixel-based scroll: 0 = newest at bottom, positive = scrolled toward older */
static float g_scroll_px = 0.0f;

/* Wrap width used when last rendering text textures.
   When window width changes significantly we rebuild all text textures. */
static int g_wrap_w = 0;

static char   g_input[INPUT_MAXLEN + 1] = {0};
static int    g_input_len = 0;

static Uint64 g_cursor_toggle_ms = 0;
static bool   g_cursor_visible   = true;

static bool   g_sb_dragging  = false;
static bool   g_sb_hovered   = false;

static AppConfig g_cfg;

#ifdef ENABLE_TEXT_SELECT
/* Ring-buffer index of the selected message, -1 = none */
static int g_sel_msg_idx = -1;

/* Visible message positions recorded each draw frame for hit-testing */
typedef struct { float y0, y1; int ring_idx; } VisMsg;
static VisMsg g_vis_msgs[MAX_MESSAGES];
static int    g_vis_count = 0;
#endif

/* ── Timestamp-sorted display ─────────────────────────────────────────── */
/* g_sort_pos[j] = offset from oldest (0..count-1) for the j-th drawn msg */
static int       g_sort_pos[MAX_MESSAGES];
static long long g_sort_ts[MAX_MESSAGES];   /* timestamps indexed by offset */

static int cmp_msg_order(const void *a, const void *b, void *ctx) {
    long long *ts = (long long *)ctx;
    int pa = *(const int *)a, pb = *(const int *)b;
    long long ta = ts[pa], tb = ts[pb];
    /* system messages (ts=0) stay in insertion order before real messages */
    if (ta == tb) return pa - pb;
    if (ta == 0)  return -1;
    if (tb == 0)  return  1;
    return (ta < tb) ? -1 : 1;
}

/* ── Helpers ──────────────────────────────────────────────────────────── */

static SDL_Color parse_hex_color(const char *hex) {
    SDL_Color c = C_TEXT;
    if (!hex || hex[0] != '#' || (int)strlen(hex) < 7) return c;
    unsigned r, g, b;
    if (sscanf(hex + 1, "%02x%02x%02x", &r, &g, &b) == 3) {
        c.r = (Uint8)r; c.g = (Uint8)g; c.b = (Uint8)b; c.a = 0xFF;
    }
    return c;
}

static void set_color(SDL_Color c) {
    SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, c.a);
}

static void fill_rect(float x, float y, float w, float h, SDL_Color c) {
    set_color(c);
    SDL_FRect r = {x, y, w, h};
    SDL_RenderFillRect(g_ren, &r);
}

static SDL_Texture *make_text_tex(const char *text, SDL_Color color,
                                  float *out_w, float *out_h) {
    if (!text || !text[0]) return NULL;
    SDL_Surface *surf = TTF_RenderText_Blended(g_font, text, 0, color);
    if (!surf) return NULL;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(g_ren, surf);
    SDL_DestroySurface(surf);
    if (!tex) return NULL;
    float w = 0, h = 0;
    SDL_GetTextureSize(tex, &w, &h);
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return tex;
}

static void draw_tex(SDL_Texture *tex, float x, float y, float w, float h) {
    if (!tex) return;
    SDL_FRect dst = {x, y, w, h};
    SDL_RenderTexture(g_ren, tex, NULL, &dst);
}

static void render_text_at(const char *text, float x, float y, SDL_Color c) {
    float w, h;
    SDL_Texture *tex = make_text_tex(text, c, &w, &h);
    if (tex) { draw_tex(tex, x, y, w, h); SDL_DestroyTexture(tex); }
}

/* Strip :word: sticker codes from content when the sticker is being shown visually */
static void strip_sticker_codes(char *dst, size_t dstlen, const char *src) {
    size_t d = 0;
    for (const char *s = src; *s && d + 1 < dstlen; ) {
        if (*s == ':') {
            const char *end = strchr(s + 1, ':');
            if (end && end > s + 1) {
                bool word = true;
                for (const char *p = s + 1; p < end; p++)
                    if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-')
                        { word = false; break; }
                if (word) { s = end + 1; continue; }
            }
        }
        dst[d++] = *s++;
    }
    /* trim trailing spaces */
    while (d > 0 && dst[d - 1] == ' ') d--;
    dst[d] = 0;
}

/* ── Message texture cache ────────────────────────────────────────────── */

static void format_msg_line(char *dst, size_t dstlen,
                             const ChatMessage *msg) {
    char raw[1024];
    chat_strip_html(raw, sizeof(raw), msg->content);

    if (msg->timestamp == 0) {
        snprintf(dst, dstlen, "  %s", raw);
        return;
    }

    char content[1024];
    if (msg->sticker_count > 0)
        strip_sticker_codes(content, sizeof(content), raw);
    else
        strncpy(content, raw, sizeof(content) - 1);

    char ts[10];
    chat_fmt_ts(ts, sizeof(ts), msg->timestamp);
    if (msg->realm[0])
        snprintf(dst, dstlen, "[%s] [%s] %s: %s", ts, msg->realm, msg->username, content);
    else
        snprintf(dst, dstlen, "[%s] %s: %s", ts, msg->username, content);
}

static float msg_total_h(const ChatMessage *msg) {
    float text_part = (float)g_line_h;
    /* if avatar is taller than one line, expand the text row */
    if (msg->avatar_url[0] && g_cfg.avatar_size > g_line_h)
        text_part = (float)g_cfg.avatar_size;
    /* each sticker gets its own row below the text */
    float sticker_part = 0;
    if (msg->sticker_count > 0)
        sticker_part = (float)(msg->sticker_count * (g_cfg.sticker_size + 4));
    return text_part + sticker_part + (msg->sticker_count > 0 ? 4.0f : 0.0f);
}

/* Extract avatar cache key from URL path */
static void avatar_key_from_url(char *dst, size_t dstlen, const char *url) {
    const char *slash = strrchr(url, '/');
    const char *name  = slash ? slash + 1 : url;
    strncpy(dst, name, dstlen - 1);
    char *dot = strrchr(dst, '.');
    if (dot) *dot = 0;
}

static void create_msg_tex(int idx, const ChatMessage *msg) {
    MsgTex *mt = &g_msg_tex[idx];
    if (mt->text_tex) { SDL_DestroyTexture(mt->text_tex); mt->text_tex = NULL; }

    char line[1400];
    format_msg_line(line, sizeof(line), msg);

    SDL_Color color;
    if (msg->timestamp == 0)
        color = C_SYSTEM;
    else if (msg->use_custom_color && msg->color[0] == '#')
        color = parse_hex_color(msg->color);
    else
        color = C_TEXT;

    /* wrapped rendering — wrap at current window width minus reserved columns */
    int wrap = g_wrap_w > 0 ? g_wrap_w : (g_win_w - MSG_PAD_X * 2 - SCROLLBAR_W);
    if (wrap < 100) wrap = 100;

    SDL_Surface *surf = NULL;
    if (line[0])
        surf = TTF_RenderText_Blended_Wrapped(g_font, line, 0, color, wrap);
    if (surf) {
        mt->text_tex = SDL_CreateTextureFromSurface(g_ren, surf);
        mt->text_w   = (float)surf->w;
        mt->text_h   = (float)surf->h;
        SDL_DestroySurface(surf);
    } else {
        mt->text_w = mt->text_h = 0;
    }

    /* sticker keys */
    mt->sticker_count = msg->sticker_count;
    for (int i = 0; i < msg->sticker_count; i++)
        strncpy(mt->sticker_names[i], msg->stickers[i].name,
                sizeof(mt->sticker_names[i]) - 1);

    /* avatar key */
    mt->avatar_key[0] = 0;
    if (msg->avatar_url[0])
        avatar_key_from_url(mt->avatar_key, sizeof(mt->avatar_key), msg->avatar_url);

    /* use actual rendered text height (may be multiple wrapped lines) */
    float text_part = mt->text_h > 0 ? mt->text_h : (float)g_line_h;
    if (msg->avatar_url[0] && g_cfg.avatar_size > (int)text_part)
        text_part = (float)g_cfg.avatar_size;
    float sticker_part = msg->sticker_count > 0
        ? (float)(msg->sticker_count * (g_cfg.sticker_size + 4)) + 4.0f : 0.0f;
    mt->total_h = text_part + sticker_part;
    if (mt->total_h < g_line_h) mt->total_h = (float)g_line_h;
}

/* Destroy all text textures so they're rebuilt at the new wrap width */
static void invalidate_msg_textures(void) {
    for (int i = 0; i < MAX_MESSAGES; i++) {
        if (g_msg_tex[i].text_tex) {
            SDL_DestroyTexture(g_msg_tex[i].text_tex);
            g_msg_tex[i].text_tex = NULL;
        }
        g_msg_tex[i].total_h = (float)g_line_h;
    }
    g_last_textured = 0;
}

static void update_msg_textures(ChatState *state) {
    pthread_mutex_lock(&state->lock);
    int cur = state->msg_count;
    pthread_mutex_unlock(&state->lock);

    while (g_last_textured < cur) {
        int idx = g_last_textured % MAX_MESSAGES;
        pthread_mutex_lock(&state->lock);
        ChatMessage msg = state->messages[idx];
        pthread_mutex_unlock(&state->lock);
        create_msg_tex(idx, &msg);
        g_last_textured++;
    }
}

/* ── Buttons ──────────────────────────────────────────────────────────── */

#define NUM_TOP_BTNS 4
static Button g_top_btns[NUM_TOP_BTNS];
static Button g_send_btn;

static void layout_buttons(void) {
    const char *labels[] = { "Auth", "Join Room", "Get Msgs", "Participants" };
    SDL_Color   cols[]   = { C_BTN_AUTH, C_BTN_JOIN, C_BTN_HIST, C_BTN_PART };
    CommandType cmds[]   = { CMD_AUTH, CMD_JOIN, CMD_GET_HISTORY, CMD_GET_PARTICIPANTS };

    float btn_w = 130.0f, btn_h = 30.0f, gap = 10.0f;
    float total_bw = NUM_TOP_BTNS * btn_w + (NUM_TOP_BTNS - 1) * gap;
    float sx = ((float)g_win_w - total_bw) / 2.0f;
    float by = TITLE_H + (BTNBAR_H - btn_h) / 2.0f;

    for (int i = 0; i < NUM_TOP_BTNS; i++) {
        g_top_btns[i].rect       = (SDL_FRect){sx + i * (btn_w + gap), by, btn_w, btn_h};
        g_top_btns[i].base_color = cols[i];
        g_top_btns[i].label      = labels[i];
        g_top_btns[i].cmd        = cmds[i];
        g_top_btns[i].hovered    = false;
    }
    float sbw = 80.0f, sbh = 30.0f;
    g_send_btn.rect  = (SDL_FRect){(float)g_win_w - sbw - 10.0f,
                                   cinput_y() + (INPUT_H - sbh) / 2.0f, sbw, sbh};
    g_send_btn.base_color = C_BTN_SEND;
    g_send_btn.label      = "Send";
    g_send_btn.cmd        = CMD_SEND_MESSAGE;
    g_send_btn.hovered    = false;
}

static void draw_button(Button *b) {
    fill_rect(b->rect.x, b->rect.y, b->rect.w, b->rect.h, b->base_color);
    if (b->hovered)
        fill_rect(b->rect.x, b->rect.y, b->rect.w, b->rect.h, C_BTN_HOVER);
    float tw, th;
    SDL_Texture *tex = make_text_tex(b->label, C_TEXT, &tw, &th);
    if (tex) {
        draw_tex(tex, b->rect.x + (b->rect.w - tw) / 2.0f,
                      b->rect.y + (b->rect.h - th) / 2.0f, tw, th);
        SDL_DestroyTexture(tex);
    }
}

static bool pt_in_rect(float px, float py, const SDL_FRect *r) {
    return px >= r->x && px < r->x + r->w && py >= r->y && py < r->y + r->h;
}

static void update_hover(float mx, float my) {
    for (int i = 0; i < NUM_TOP_BTNS; i++)
        g_top_btns[i].hovered = pt_in_rect(mx, my, &g_top_btns[i].rect);
    g_send_btn.hovered = pt_in_rect(mx, my, &g_send_btn.rect);
    g_sb_hovered = (mx >= g_win_w - SCROLLBAR_W &&
                    my >= MSG_Y && my < cinput_y());
}

static void handle_button_click(float mx, float my, ChatState *state) {
    for (int i = 0; i < NUM_TOP_BTNS; i++) {
        if (pt_in_rect(mx, my, &g_top_btns[i].rect)) {
            CommandType cmd = g_top_btns[i].cmd;
            if (cmd == CMD_JOIN) {
                pthread_mutex_lock(&state->lock);
                char realm[64] = {0};
                strncpy(realm, state->realm, sizeof(realm) - 1);
                pthread_mutex_unlock(&state->lock);
                chat_push_cmd(state, CMD_JOIN, realm);
            } else {
                chat_push_cmd(state, cmd, NULL);
            }
            return;
        }
    }
    if (pt_in_rect(mx, my, &g_send_btn.rect) && g_input_len > 0) {
        chat_push_cmd(state, CMD_SEND_MESSAGE, g_input);
        g_input[0] = 0; g_input_len = 0;
    }
}

/* ── Pixel-based scroll helpers ───────────────────────────────────────── */

static float total_content_px(int msg_count) {
    int lim    = (g_cfg.max_messages > 0) ? g_cfg.max_messages : MAX_MESSAGES;
    int oldest = (msg_count > lim) ? (msg_count - lim) : 0;
    float px = 0;
    for (int i = oldest; i < msg_count; i++)
        px += g_msg_tex[i % MAX_MESSAGES].total_h;
    return px;
}

static void clamp_scroll(int msg_count) {
    float total = total_content_px(msg_count);
    float max_s = total - (float)cmsg_h();
    if (max_s < 0) max_s = 0;
    if (g_scroll_px < 0)      g_scroll_px = 0;
    if (g_scroll_px > max_s)  g_scroll_px = max_s;
}

static void sb_set_scroll_from_mouse(float my, int msg_count) {
    float track_h = (float)cmsg_h();
    float frac = (my - MSG_Y) / track_h;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    float total   = total_content_px(msg_count);
    float max_s   = total - (float)cmsg_h();
    if (max_s <= 0) { g_scroll_px = 0; return; }
    /* frac 0 = top of track = oldest = max scroll */
    g_scroll_px = (1.0f - frac) * max_s;
}

/* ── Drawing ──────────────────────────────────────────────────────────── */

static void draw_title(ChatState *state) {
    fill_rect(0, 0, (float)g_win_w, TITLE_H, C_TITLE);
    pthread_mutex_lock(&state->lock);
    char status[256] = {0}, realm[64] = {0};
    strncpy(status, state->status, sizeof(status) - 1);
    strncpy(realm,  state->realm,  sizeof(realm)  - 1);
    bool conn = state->connected, auth = state->authenticated, join = state->joined;
    pthread_mutex_unlock(&state->lock);

    char title[512];
    snprintf(title, sizeof(title), "NurealmS  |  %s  |  %s",
             realm[0] ? realm : "(no realm)",
             status[0] ? status : "idle");

    SDL_Color ind = conn ? (auth ? (join ? C_BTN_AUTH : C_BTN_JOIN) : C_BTN_HIST) : C_DIM;
    fill_rect(0, 0, 4, TITLE_H, ind);
    render_text_at(title, 10, (TITLE_H - g_line_h) / 2.0f, C_TEXT);
}

static void draw_btnbar(void) {
    fill_rect(0, TITLE_H, (float)g_win_w, BTNBAR_H, C_BTNBAR);
    for (int i = 0; i < NUM_TOP_BTNS; i++) draw_button(&g_top_btns[i]);
}

static void draw_messages(ChatState *state) {
    int mh = cmsg_h();
    fill_rect(0, MSG_Y, (float)g_win_w, (float)mh, C_MSGBG);

    pthread_mutex_lock(&state->lock);
    int total = state->msg_count;
    pthread_mutex_unlock(&state->lock);

    clamp_scroll(total);

    int lim    = (g_cfg.max_messages > 0) ? g_cfg.max_messages : MAX_MESSAGES;
    int oldest = (total > lim) ? (total - lim) : 0;
    int count  = total - oldest;

    /* build timestamp-sorted draw order */
    for (int j = 0; j < count; j++) g_sort_pos[j] = j;
    pthread_mutex_lock(&state->lock);
    for (int j = 0; j < count; j++)
        g_sort_ts[j] = state->messages[(oldest + j) % MAX_MESSAGES].timestamp;
    pthread_mutex_unlock(&state->lock);
    qsort_r(g_sort_pos, count, sizeof(int), cmp_msg_order, g_sort_ts);

    float total_px = total_content_px(total);
    float vp_top   = total_px - (float)mh - g_scroll_px;
    if (vp_top < 0) vp_top = 0;

    /* clip to message area (leave scrollbar column) */
    SDL_Rect clip = {0, MSG_Y, g_win_w - SCROLLBAR_W, mh};
    SDL_SetRenderClipRect(g_ren, &clip);

#ifdef ENABLE_TEXT_SELECT
    g_vis_count = 0;
#endif

    float cum_px = 0;
    for (int j = 0; j < count; j++) {
        int idx = (oldest + g_sort_pos[j]) % MAX_MESSAGES;
        MsgTex *mt = &g_msg_tex[idx];
        float msg_h = mt->total_h;

        if (cum_px + msg_h <= vp_top) { cum_px += msg_h; continue; }
        if (cum_px >= vp_top + mh)     break;

        float screen_y = (float)MSG_Y + (cum_px - vp_top);

#ifdef ENABLE_TEXT_SELECT
        /* record for hit-testing */
        if (g_vis_count < MAX_MESSAGES) {
            g_vis_msgs[g_vis_count++] = (VisMsg){screen_y, screen_y + msg_h, idx};
        }
        /* selection highlight */
        if (idx == g_sel_msg_idx) {
            SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
            fill_rect(0, screen_y, (float)(g_win_w - SCROLLBAR_W), msg_h,
                      (SDL_Color){0x45, 0x85, 0x88, 0x55});
        }
#endif

        /* ── avatar ── */
        float text_x = (float)MSG_PAD_X;
        if (mt->avatar_key[0]) {
            SDL_Texture *av_tex = NULL; float av_w = 0, av_h = 0;
            imgcache_get(mt->avatar_key, IMG_AVATAR, &av_tex, &av_w, &av_h);
            if (av_tex) {
                float av_sz = (float)g_cfg.avatar_size;
                SDL_FRect av_dst = {(float)MSG_PAD_X, screen_y, av_sz, av_sz};
                SDL_RenderTexture(g_ren, av_tex, NULL, &av_dst);
            }
            text_x = (float)(MSG_PAD_X + g_cfg.avatar_size + 4);
        }

        /* ── text line ── */
        float text_part_h = mt->avatar_key[0] && g_cfg.avatar_size > g_line_h
                            ? (float)g_cfg.avatar_size : (float)g_line_h;
        float ty = screen_y + (text_part_h - mt->text_h) / 2.0f;
        float avail_w = (float)(g_win_w - SCROLLBAR_W) - text_x - MSG_PAD_X;

        if (mt->text_tex) {
            float tw = mt->text_w > avail_w ? avail_w : mt->text_w;
            SDL_FRect src = {0, 0, tw, mt->text_h};
            SDL_FRect dst = {text_x, ty, tw, mt->text_h};
            SDL_RenderTexture(g_ren, mt->text_tex, &src, &dst);
        }

        /* ── stickers ── */
        float sti_y = screen_y + text_part_h + 4.0f;
        for (int si = 0; si < mt->sticker_count; si++) {
            SDL_Texture *st = NULL; float sw = 0, sh = 0;
            imgcache_get(mt->sticker_names[si], IMG_STICKER, &st, &sw, &sh);
            if (st) {
                SDL_FRect dst = {text_x, sti_y, sw, sh};
                SDL_RenderTexture(g_ren, st, NULL, &dst);
            }
            sti_y += (float)(g_cfg.sticker_size + 4);
        }

        cum_px += msg_h;
    }

    SDL_SetRenderClipRect(g_ren, NULL);

    /* ── scrollbar ── */
    float sb_x = (float)(g_win_w - SCROLLBAR_W);
    fill_rect(sb_x, (float)MSG_Y, SCROLLBAR_W, (float)mh, C_SCROLLBAR);

    if (total_px > (float)mh) {
        float thumb_frac_h = (float)mh / total_px;
        float thumb_h = thumb_frac_h * mh;
        if (thumb_h < 20) thumb_h = 20;
        float max_s = total_px - (float)mh;
        float frac_pos = (max_s > 0) ? (max_s - g_scroll_px) / max_s : 1.0f;
        float thumb_y = (float)MSG_Y + frac_pos * ((float)mh - thumb_h);
        SDL_Color tc = (g_sb_hovered || g_sb_dragging) ? C_SCROLLHOV : C_SCROLLTHUMB;
        fill_rect(sb_x + 1, thumb_y, (float)(SCROLLBAR_W - 2), thumb_h, tc);
    }

    /* separator lines */
    set_color(C_SEPLINE);
    SDL_FRect top_sep = {0, (float)(MSG_Y - 1), (float)g_win_w, 1};
    SDL_FRect bot_sep = {0, (float)cinput_y(),  (float)g_win_w, 1};
    SDL_RenderFillRect(g_ren, &top_sep);
    SDL_RenderFillRect(g_ren, &bot_sep);
}

static void draw_input(void) {
    int iy = cinput_y();
    fill_rect(0, (float)iy, (float)g_win_w, INPUT_H, C_INPUTBG);

    render_text_at("MSG >", 8, (float)iy + (INPUT_H - g_line_h) / 2.0f, C_DIM);

    float field_x = 64.0f;
    float field_w = g_send_btn.rect.x - field_x - 10.0f;
    float field_h = (float)INPUT_H - 12.0f;
    float field_y = (float)iy + 6.0f;

    fill_rect(field_x, field_y, field_w, field_h, C_INPUTFLD);
    set_color(C_INPUTBDR);
    SDL_FRect bdr[] = {
        {field_x,               field_y,               field_w, 2},
        {field_x,               field_y + field_h - 2, field_w, 2},
        {field_x,               field_y,               2,       field_h},
        {field_x + field_w - 2, field_y,               2,       field_h},
    };
    for (int i = 0; i < 4; i++) SDL_RenderFillRect(g_ren, &bdr[i]);

    float tx0    = field_x + 6.0f;
    float ty0    = field_y + (field_h - (float)g_line_h) / 2.0f;
    float t_avail = field_w - 12.0f;

    SDL_Rect clip = {(int)field_x + 2, (int)field_y + 2,
                     (int)field_w - 4, (int)field_h - 4};
    SDL_SetRenderClipRect(g_ren, &clip);

    float tw = 0;
    if (g_input_len > 0) {
        float th;
        SDL_Texture *tex = make_text_tex(g_input, C_TEXT, &tw, &th);
        if (tex) {
            float dx = tx0;
            if (tw > t_avail) dx = tx0 - (tw - t_avail);
            draw_tex(tex, dx, ty0, tw, th);
            SDL_DestroyTexture(tex);
        }
    } else {
        render_text_at("Type a message and press Enter...", tx0, ty0, C_DIM);
    }

    Uint64 now_ms = SDL_GetTicks();
    if (now_ms - g_cursor_toggle_ms >= 500) {
        g_cursor_visible   = !g_cursor_visible;
        g_cursor_toggle_ms = now_ms;
    }
    if (g_cursor_visible) {
        float cx = tx0 + tw;
        if (cx > field_x + field_w - 4) cx = field_x + field_w - 4;
        SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
        fill_rect(cx, ty0, 10, (float)g_line_h, C_CURSOR);
    }

    SDL_SetRenderClipRect(g_ren, NULL);
    draw_button(&g_send_btn);
}

/* ── Main entry ───────────────────────────────────────────────────────── */

int gui_run(ChatState *state, const AppConfig *cfg) {
    g_cfg = *cfg;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
    }
    if (!TTF_Init()) {
        fprintf(stderr, "TTF_Init: %s\n", SDL_GetError()); SDL_Quit(); return 1;
    }
    g_font = TTF_OpenFont(FONT_PATH, FONT_SIZE);
    if (!g_font) {
        fprintf(stderr, "TTF_OpenFont: %s\n", SDL_GetError());
        TTF_Quit(); SDL_Quit(); return 1;
    }
    { int tw, th; TTF_GetStringSize(g_font, "Ag", 0, &tw, &th); g_line_h = th + LINE_PAD; }

    int init_w = 1000, init_h = 680;
    {
#ifdef USE_SDL2
        SDL_DisplayMode dm;
        if (SDL_GetCurrentDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) {
            init_w = (int)(dm.w * 0.82f); if (init_w > 1200) init_w = 1200;
            init_h = (int)(dm.h * 0.82f); if (init_h > 800)  init_h = 800;
            int min_h = TITLE_H + BTNBAR_H + g_line_h * 4 + INPUT_H + 10;
            if (init_h < min_h) init_h = min_h;
        }
#else
        SDL_DisplayID disp = SDL_GetPrimaryDisplay();
        const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(disp);
        if (dm && dm->w > 0 && dm->h > 0) {
            init_w = (int)(dm->w * 0.82f); if (init_w > 1200) init_w = 1200;
            init_h = (int)(dm->h * 0.82f); if (init_h > 800)  init_h = 800;
            int min_h = TITLE_H + BTNBAR_H + g_line_h * 4 + INPUT_H + 10;
            if (init_h < min_h) init_h = min_h;
        }
#endif
    }
    g_win_w = init_w; g_win_h = init_h;

    g_win = SDL_CreateWindow("NurealmS Chat", init_w, init_h, SDL_WINDOW_RESIZABLE);
    if (!g_win) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        TTF_CloseFont(g_font); TTF_Quit(); SDL_Quit(); return 1;
    }
    g_ren = SDL_CreateRenderer(g_win, NULL);
    if (!g_ren) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(g_win); TTF_CloseFont(g_font); TTF_Quit(); SDL_Quit(); return 1;
    }
    SDL_SetRenderVSync(g_ren, 1);
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);

    layout_buttons();
    g_wrap_w = init_w - MSG_PAD_X * 2 - SCROLLBAR_W;
    SDL_StartTextInput(g_win);
    memset(g_msg_tex, 0, sizeof(g_msg_tex));

    int prev_w = init_w, prev_h = init_h;
    bool running = true;

    while (running) {
        SDL_GetRenderOutputSize(g_ren, &g_win_w, &g_win_h);
        if (g_win_w != prev_w || g_win_h != prev_h) {
            layout_buttons();
            int new_wrap = g_win_w - MSG_PAD_X * 2 - SCROLLBAR_W;
            if (abs(new_wrap - g_wrap_w) > 20) {
                g_wrap_w = new_wrap;
                invalidate_msg_textures();
            }
            prev_w = g_win_w; prev_h = g_win_h;
        }

        float mx = 0, my = 0;
        SDL_GetMouseState(&mx, &my);
        update_hover(mx, my);

        if (g_sb_dragging) {
            pthread_mutex_lock(&state->lock);
            int total = state->msg_count;
            pthread_mutex_unlock(&state->lock);
            sb_set_scroll_from_mouse(my, total);
        }

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_EVENT_QUIT: running = false; break;

                case SDL_EVENT_MOUSE_MOTION:
                    update_hover(ev.motion.x, ev.motion.y);
                    break;

                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    if (ev.button.button == SDL_BUTTON_LEFT) {
                        float bx = ev.button.x, by = ev.button.y;
                        if (bx >= g_win_w - SCROLLBAR_W &&
                            by >= MSG_Y && by < cinput_y()) {
                            g_sb_dragging = true;
                            pthread_mutex_lock(&state->lock);
                            int total = state->msg_count;
                            pthread_mutex_unlock(&state->lock);
                            sb_set_scroll_from_mouse(by, total);
#ifdef ENABLE_TEXT_SELECT
                        } else if (bx < g_win_w - SCROLLBAR_W &&
                                   by >= MSG_Y && by < cinput_y()) {
                            /* click in message area: select that message */
                            int hit = -1;
                            for (int vi = 0; vi < g_vis_count; vi++) {
                                if (by >= g_vis_msgs[vi].y0 && by < g_vis_msgs[vi].y1) {
                                    hit = g_vis_msgs[vi].ring_idx; break;
                                }
                            }
                            g_sel_msg_idx = hit;
#endif
                        } else {
                            handle_button_click(bx, by, state);
                        }
                    }
                    break;

                case SDL_EVENT_MOUSE_BUTTON_UP:
                    if (ev.button.button == SDL_BUTTON_LEFT) g_sb_dragging = false;
                    break;

                case SDL_EVENT_MOUSE_WHEEL:
                    g_scroll_px += -ev.wheel.y * g_line_h * 3;
                    break;

                case SDL_EVENT_TEXT_INPUT:
                    if (g_input_len < INPUT_MAXLEN) {
                        size_t add = strlen(ev.text.text);
                        if (g_input_len + (int)add <= INPUT_MAXLEN) {
                            memcpy(g_input + g_input_len, ev.text.text, add);
                            g_input_len += (int)add;
                            g_input[g_input_len] = 0;
                        }
                    }
                    break;

                case SDL_EVENT_KEY_DOWN: {
#ifdef USE_SDL2
                    SDL_Keycode key_sym = ev.key.keysym.sym;
                    int         key_mod = ev.key.keysym.mod;
#else
                    SDL_Keycode key_sym = ev.key.key;
                    int         key_mod = (int)ev.key.mod;
#endif
                    switch (key_sym) {
                        case SDLK_RETURN: case SDLK_KP_ENTER:
                            if (g_input_len > 0) {
                                chat_push_cmd(state, CMD_SEND_MESSAGE, g_input);
                                g_input[0] = 0; g_input_len = 0;
                            }
                            break;
                        case SDLK_BACKSPACE:
                            if (g_input_len > 0) {
                                g_input_len--;
                                while (g_input_len > 0 &&
                                       (g_input[g_input_len] & 0xC0) == 0x80)
                                    g_input_len--;
                                g_input[g_input_len] = 0;
                            }
                            break;
                        case SDLK_ESCAPE:
                            g_input[0] = 0; g_input_len = 0;
#ifdef ENABLE_TEXT_SELECT
                            g_sel_msg_idx = -1;
#endif
                            break;
                        case SDLK_C:
#ifdef ENABLE_TEXT_SELECT
                            if ((key_mod & SDL_KMOD_CTRL) && g_sel_msg_idx >= 0) {
                                pthread_mutex_lock(&state->lock);
                                ChatMessage msg = state->messages[g_sel_msg_idx];
                                pthread_mutex_unlock(&state->lock);
                                char line[1400];
                                format_msg_line(line, sizeof(line), &msg);
                                SDL_SetClipboardText(line);
                            }
#endif
                            break;
                        case SDLK_V:
                            if (key_mod & SDL_KMOD_CTRL) {
                                char *clip = SDL_GetClipboardText();
                                if (clip) {
                                    int add = (int)strlen(clip);
                                    if (g_input_len + add > INPUT_MAXLEN)
                                        add = INPUT_MAXLEN - g_input_len;
                                    if (add > 0) {
                                        memcpy(g_input + g_input_len, clip, add);
                                        g_input_len += add;
                                        g_input[g_input_len] = 0;
                                    }
                                    SDL_free(clip);
                                }
                            }
                            break;
                        case SDLK_END:  g_scroll_px = 0; break;
                        case SDLK_HOME: {
                            pthread_mutex_lock(&state->lock);
                            int total = state->msg_count;
                            pthread_mutex_unlock(&state->lock);
                            float tp = total_content_px(total);
                            g_scroll_px = tp - (float)cmsg_h();
                            break;
                        }
                        default: break;
                    }
                    break;
                }

                default: break;
            }
        }

        update_msg_textures(state);
        imgcache_process(g_ren);

        {
            pthread_mutex_lock(&state->lock);
            bool nm = state->new_messages;
            if (nm) state->new_messages = false;
            pthread_mutex_unlock(&state->lock);
            if (nm) g_scroll_px = 0;
        }

        set_color(C_BG);
        SDL_RenderClear(g_ren);
        draw_title(state);
        draw_btnbar();
        draw_messages(state);
        draw_input();
        SDL_RenderPresent(g_ren);
    }

    SDL_StopTextInput(g_win);
    for (int i = 0; i < MAX_MESSAGES; i++)
        if (g_msg_tex[i].text_tex) SDL_DestroyTexture(g_msg_tex[i].text_tex);
    TTF_CloseFont(g_font);
    SDL_DestroyRenderer(g_ren);
    SDL_DestroyWindow(g_win);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
