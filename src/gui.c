#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include "chat.h"
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

/* ── Dynamic layout (updated each frame from real window size) ────────── */
static int g_win_w = 1100;
static int g_win_h = 720;

static inline int cmsg_h(void)    { return g_win_h - TITLE_H - BTNBAR_H - INPUT_H; }
static inline int cinput_y(void)  { return g_win_h - INPUT_H; }

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

static const SDL_Color C_BTN_AUTH  = COL(0x20,0x8C,0x56,0xFF);
static const SDL_Color C_BTN_JOIN  = COL(0x20,0x76,0xB0,0xFF);
static const SDL_Color C_BTN_HIST  = COL(0x7C,0x3A,0xAA,0xFF);
static const SDL_Color C_BTN_PART  = COL(0xC0,0x6E,0x14,0xFF);
static const SDL_Color C_BTN_SEND  = COL(0x20,0x8C,0x56,0xFF);
static const SDL_Color C_BTN_HOVER = COL(0xFF,0xFF,0xFF,0x28);

/* ── Types ────────────────────────────────────────────────────────────── */
typedef struct {
    SDL_FRect   rect;
    SDL_Color   base_color;
    const char *label;
    CommandType cmd;
    bool        hovered;
} Button;

typedef struct {
    SDL_Texture *tex;
    float        w, h;
} MsgTex;

/* ── Globals ──────────────────────────────────────────────────────────── */
static SDL_Window   *g_win      = NULL;
static SDL_Renderer *g_ren      = NULL;
static TTF_Font     *g_font     = NULL;
static int           g_line_h   = 0;
static MsgTex        g_msg_tex[MAX_MESSAGES];
static int           g_last_textured = 0;
static int           g_scroll   = 0;

static char   g_input[INPUT_MAXLEN + 1] = {0};
static int    g_input_len = 0;

static Uint64 g_cursor_toggle_ms = 0;
static bool   g_cursor_visible   = true;

static bool   g_sb_dragging    = false;
static bool   g_sb_hovered     = false;

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

static SDL_Texture *make_text_tex(const char *text, SDL_Color color, float *out_w, float *out_h) {
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

static void draw_text(SDL_Texture *tex, float x, float y, float w, float h) {
    if (!tex) return;
    SDL_FRect dst = {x, y, w, h};
    SDL_RenderTexture(g_ren, tex, NULL, &dst);
}

static void render_text_at(const char *text, float x, float y, SDL_Color c) {
    float w, h;
    SDL_Texture *tex = make_text_tex(text, c, &w, &h);
    if (tex) { draw_text(tex, x, y, w, h); SDL_DestroyTexture(tex); }
}

/* ── Message texture cache ────────────────────────────────────────────── */

static void format_msg_line(char *dst, size_t dstlen, const ChatMessage *msg) {
    char content[1024];
    chat_strip_html(content, sizeof(content), msg->content);

    if (msg->timestamp == 0) {
        snprintf(dst, dstlen, "  %s", content);
        return;
    }
    char ts[10];
    chat_fmt_ts(ts, sizeof(ts), msg->timestamp);
    if (msg->realm[0])
        snprintf(dst, dstlen, "[%s] [%s] %s: %s", ts, msg->realm, msg->username, content);
    else
        snprintf(dst, dstlen, "[%s] %s: %s", ts, msg->username, content);
}

static void create_msg_tex(int idx, const ChatMessage *msg) {
    if (g_msg_tex[idx].tex) {
        SDL_DestroyTexture(g_msg_tex[idx].tex);
        g_msg_tex[idx].tex = NULL;
    }
    char line[1300];
    format_msg_line(line, sizeof(line), msg);

    SDL_Color color;
    if (msg->timestamp == 0)
        color = C_SYSTEM;
    else if (msg->use_custom_color && msg->color[0] == '#')
        color = parse_hex_color(msg->color);
    else
        color = C_TEXT;

    float w, h;
    g_msg_tex[idx].tex = make_text_tex(line, color, &w, &h);
    g_msg_tex[idx].w   = w;
    g_msg_tex[idx].h   = h;
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

    float btn_w = 130.0f, btn_h = 30.0f;
    float gap   = 10.0f;
    float total  = NUM_TOP_BTNS * btn_w + (NUM_TOP_BTNS - 1) * gap;
    float start_x = ((float)g_win_w - total) / 2.0f;
    float btn_y  = TITLE_H + (BTNBAR_H - btn_h) / 2.0f;

    for (int i = 0; i < NUM_TOP_BTNS; i++) {
        g_top_btns[i].rect       = (SDL_FRect){start_x + i * (btn_w + gap), btn_y, btn_w, btn_h};
        g_top_btns[i].base_color = cols[i];
        g_top_btns[i].label      = labels[i];
        g_top_btns[i].cmd        = cmds[i];
        g_top_btns[i].hovered    = false;
    }

    float sbw = 80.0f, sbh = 30.0f;
    g_send_btn.rect       = (SDL_FRect){(float)g_win_w - sbw - 10.0f, cinput_y() + (INPUT_H - sbh) / 2.0f, sbw, sbh};
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
        float tx = b->rect.x + (b->rect.w - tw) / 2.0f;
        float ty = b->rect.y + (b->rect.h - th) / 2.0f;
        draw_text(tex, tx, ty, tw, th);
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
    g_sb_hovered = (mx >= g_win_w - SCROLLBAR_W && my >= MSG_Y && my < cinput_y());
}

static void handle_button_click(float mx, float my, ChatState *state) {
    for (int i = 0; i < NUM_TOP_BTNS; i++) {
        if (pt_in_rect(mx, my, &g_top_btns[i].rect)) {
            CommandType cmd = g_top_btns[i].cmd;
            if (cmd == CMD_JOIN) {
                pthread_mutex_lock(&state->lock);
                char realm[64];
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
        g_input[0]   = 0;
        g_input_len  = 0;
    }
}

/* scrollbar thumb geometry helpers */
static void sb_thumb(int total, int visible, float *out_y, float *out_h) {
    float track_h = (float)cmsg_h();
    float th = (total > 0) ? ((float)visible / total * track_h) : track_h;
    if (th < 20) th = 20;
    if (th > track_h) th = track_h;
    int max_scroll = (total > visible) ? (total - visible) : 0;
    float frac = (max_scroll > 0) ? (float)(max_scroll - g_scroll) / max_scroll : 1.0f;
    float ty = MSG_Y + frac * (track_h - th);
    if (out_y) *out_y = ty;
    if (out_h) *out_h = th;
}

static void sb_click_scroll(float my, int total, int visible) {
    float track_h = (float)cmsg_h();
    float frac = (my - MSG_Y) / track_h;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    int max_scroll = (total > visible) ? (total - visible) : 0;
    /* frac 0 = top = max scroll; frac 1 = bottom = 0 */
    g_scroll = (int)((1.0f - frac) * max_scroll);
    if (g_scroll < 0)          g_scroll = 0;
    if (g_scroll > max_scroll) g_scroll = max_scroll;
}

/* ── Drawing ──────────────────────────────────────────────────────────── */

static void draw_title(ChatState *state) {
    fill_rect(0, 0, (float)g_win_w, TITLE_H, C_TITLE);

    pthread_mutex_lock(&state->lock);
    char status[256] = {0};
    char realm[64]   = {0};
    strncpy(status, state->status, sizeof(status) - 1);
    strncpy(realm,  state->realm,  sizeof(realm)  - 1);
    bool conn  = state->connected;
    bool auth  = state->authenticated;
    bool join  = state->joined;
    pthread_mutex_unlock(&state->lock);

    char title[512];
    snprintf(title, sizeof(title), "NurealmS  |  %s  |  %s",
             realm[0] ? realm : "(no realm)",
             status[0] ? status : "idle");

    SDL_Color indicator = conn ? (auth ? (join ? C_BTN_AUTH : C_BTN_JOIN) : C_BTN_HIST) : C_DIM;
    fill_rect(0, 0, 4, TITLE_H, indicator);
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

    int visible = (g_line_h > 0) ? mh / g_line_h : 1;

    /* clamp scroll */
    int max_scroll = (total > visible) ? (total - visible) : 0;
    if (g_scroll > max_scroll) g_scroll = max_scroll;
    if (g_scroll < 0)          g_scroll = 0;

    int first = total - visible - g_scroll;
    if (first < 0) first = 0;
    int last  = total - g_scroll;

    /* clip to message area, leave room for scrollbar */
    SDL_Rect clip = {0, MSG_Y, g_win_w - SCROLLBAR_W, mh};
    SDL_SetRenderClipRect(g_ren, &clip);

    float y = (float)MSG_Y + 4.0f;
    for (int i = first; i < last; i++) {
        int idx = i % MAX_MESSAGES;
        if (g_msg_tex[idx].tex) {
            float tw = g_msg_tex[idx].w;
            float th = g_msg_tex[idx].h;
            float avail_w = (float)(g_win_w - SCROLLBAR_W - MSG_PAD_X * 2);
            SDL_FRect src = {0, 0, (tw > avail_w) ? avail_w : tw, th};
            SDL_FRect dst = {MSG_PAD_X, y, src.w, th};
            SDL_RenderTexture(g_ren, g_msg_tex[idx].tex, &src, &dst);
        }
        y += g_line_h;
    }

    SDL_SetRenderClipRect(g_ren, NULL);

    /* ── scrollbar ── */
    float sb_x = (float)(g_win_w - SCROLLBAR_W);
    fill_rect(sb_x, (float)MSG_Y, SCROLLBAR_W, (float)mh, C_SCROLLBAR);

    if (total > visible) {
        float th_y, th_h;
        sb_thumb(total, visible, &th_y, &th_h);
        SDL_Color thumb_col = (g_sb_hovered || g_sb_dragging) ? C_SCROLLHOV : C_SCROLLTHUMB;
        fill_rect(sb_x + 1, th_y, (float)(SCROLLBAR_W - 2), th_h, thumb_col);
    }

    /* separator lines */
    set_color(C_SEPLINE);
    SDL_FRect top_sep = {0, (float)(MSG_Y - 1),    (float)g_win_w, 1};
    SDL_FRect bot_sep = {0, (float)cinput_y(), (float)g_win_w, 1};
    SDL_RenderFillRect(g_ren, &top_sep);
    SDL_RenderFillRect(g_ren, &bot_sep);
}

static void draw_input(void) {
    int iy = cinput_y();
    fill_rect(0, (float)iy, (float)g_win_w, INPUT_H, C_INPUTBG);

    float label_x = 8;
    float mid_y   = (float)iy + INPUT_H / 2.0f;
    render_text_at("MSG >", label_x, mid_y - g_line_h / 2.0f, C_DIM);

    float field_x = label_x + 56;
    float field_w = g_send_btn.rect.x - field_x - 10;
    float field_h = (float)INPUT_H - 12;
    float field_y = (float)iy + 6;

    fill_rect(field_x, field_y, field_w, field_h, C_INPUTFLD);

    set_color(C_INPUTBDR);
    SDL_FRect bdr[] = {
        {field_x,               field_y,               field_w, 2},
        {field_x,               field_y + field_h - 2, field_w, 2},
        {field_x,               field_y,               2,       field_h},
        {field_x + field_w - 2, field_y,               2,       field_h},
    };
    for (int i = 0; i < 4; i++) SDL_RenderFillRect(g_ren, &bdr[i]);

    float tx0    = field_x + 6;
    float ty0    = field_y + (field_h - g_line_h) / 2.0f;
    float t_avail = field_w - 12;

    SDL_Rect clip = {(int)field_x + 2, (int)field_y + 2,
                     (int)field_w - 4, (int)field_h - 4};
    SDL_SetRenderClipRect(g_ren, &clip);

    float tw = 0;
    if (g_input_len > 0) {
        float th;
        SDL_Texture *tex = make_text_tex(g_input, C_TEXT, &tw, &th);
        if (tex) {
            float draw_x = tx0;
            if (tw > t_avail)
                draw_x = tx0 - (tw - t_avail);
            draw_text(tex, draw_x, ty0, tw, th);
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

int gui_run(ChatState *state) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (!TTF_Init()) {
        fprintf(stderr, "TTF_Init: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    g_font = TTF_OpenFont(FONT_PATH, FONT_SIZE);
    if (!g_font) {
        fprintf(stderr, "TTF_OpenFont: %s\n", SDL_GetError());
        TTF_Quit(); SDL_Quit();
        return 1;
    }

    {
        int tw, th;
        TTF_GetStringSize(g_font, "Ag", 0, &tw, &th);
        g_line_h = th + LINE_PAD;
    }

    /* fit window to display — leave a margin so it doesn't fill the screen */
    int init_w = 1000, init_h = 680;
    {
        SDL_DisplayID disp = SDL_GetPrimaryDisplay();
        const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(disp);
        if (dm && dm->w > 0 && dm->h > 0) {
            init_w = (int)(dm->w * 0.82f);
            init_h = (int)(dm->h * 0.82f);
            if (init_w > 1200) init_w = 1200;
            if (init_h > 800)  init_h = 800;
            /* ensure there's always room for the input bar */
            int min_h = TITLE_H + BTNBAR_H + g_line_h * 4 + INPUT_H + 10;
            if (init_h < min_h) init_h = min_h;
        }
    }
    g_win_w = init_w;
    g_win_h = init_h;

    g_win = SDL_CreateWindow("NurealmS Chat", init_w, init_h, SDL_WINDOW_RESIZABLE);
    if (!g_win) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        TTF_CloseFont(g_font); TTF_Quit(); SDL_Quit();
        return 1;
    }

    g_ren = SDL_CreateRenderer(g_win, NULL);
    if (!g_ren) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(g_win); TTF_CloseFont(g_font); TTF_Quit(); SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(g_ren, 1);
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);

    layout_buttons();
    SDL_StartTextInput(g_win);
    memset(g_msg_tex, 0, sizeof(g_msg_tex));

    int prev_w = init_w, prev_h = init_h;

    bool running = true;
    while (running) {
        /* update live window size */
        SDL_GetRenderOutputSize(g_ren, &g_win_w, &g_win_h);
        if (g_win_w != prev_w || g_win_h != prev_h) {
            layout_buttons();
            prev_w = g_win_w;
            prev_h = g_win_h;
        }

        float mx = 0, my = 0;
        SDL_GetMouseState(&mx, &my);
        update_hover(mx, my);

        /* scrollbar drag */
        if (g_sb_dragging) {
            pthread_mutex_lock(&state->lock);
            int total = state->msg_count;
            pthread_mutex_unlock(&state->lock);
            int visible = (g_line_h > 0) ? cmsg_h() / g_line_h : 1;
            sb_click_scroll(my, total, visible);
        }

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_EVENT_QUIT:
                    running = false;
                    break;

                case SDL_EVENT_MOUSE_MOTION:
                    update_hover(ev.motion.x, ev.motion.y);
                    break;

                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    if (ev.button.button == SDL_BUTTON_LEFT) {
                        float bx = ev.button.x, by = ev.button.y;
                        if (bx >= g_win_w - SCROLLBAR_W && by >= MSG_Y && by < cinput_y()) {
                            /* click on scrollbar */
                            g_sb_dragging = true;
                            pthread_mutex_lock(&state->lock);
                            int total = state->msg_count;
                            pthread_mutex_unlock(&state->lock);
                            int visible = (g_line_h > 0) ? cmsg_h() / g_line_h : 1;
                            sb_click_scroll(by, total, visible);
                        } else {
                            handle_button_click(bx, by, state);
                        }
                    }
                    break;

                case SDL_EVENT_MOUSE_BUTTON_UP:
                    if (ev.button.button == SDL_BUTTON_LEFT)
                        g_sb_dragging = false;
                    break;

                case SDL_EVENT_MOUSE_WHEEL:
                    g_scroll += (int)(-ev.wheel.y * 3);
                    if (g_scroll < 0) g_scroll = 0;
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

                case SDL_EVENT_KEY_DOWN:
                    switch (ev.key.key) {
                        case SDLK_RETURN:
                        case SDLK_KP_ENTER:
                            if (g_input_len > 0) {
                                chat_push_cmd(state, CMD_SEND_MESSAGE, g_input);
                                g_input[0]  = 0;
                                g_input_len = 0;
                            }
                            break;
                        case SDLK_BACKSPACE:
                            if (g_input_len > 0) {
                                g_input_len--;
                                while (g_input_len > 0 && (g_input[g_input_len] & 0xC0) == 0x80)
                                    g_input_len--;
                                g_input[g_input_len] = 0;
                            }
                            break;
                        case SDLK_ESCAPE:
                            g_input[0]  = 0;
                            g_input_len = 0;
                            break;
                        case SDLK_V:
                            if (ev.key.mod & SDL_KMOD_CTRL) {
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
                        case SDLK_END:
                            g_scroll = 0;
                            break;
                        case SDLK_HOME: {
                            pthread_mutex_lock(&state->lock);
                            int total = state->msg_count;
                            pthread_mutex_unlock(&state->lock);
                            int visible = (g_line_h > 0) ? cmsg_h() / g_line_h : 1;
                            g_scroll = (total > visible) ? total - visible : 0;
                            break;
                        }
                        default: break;
                    }
                    break;

                default: break;
            }
        }

        update_msg_textures(state);

        {
            pthread_mutex_lock(&state->lock);
            bool nm = state->new_messages;
            if (nm) state->new_messages = false;
            pthread_mutex_unlock(&state->lock);
            if (nm && g_scroll <= 3) g_scroll = 0;
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
    for (int i = 0; i < MAX_MESSAGES; i++) {
        if (g_msg_tex[i].tex) SDL_DestroyTexture(g_msg_tex[i].tex);
    }
    TTF_CloseFont(g_font);
    SDL_DestroyRenderer(g_ren);
    SDL_DestroyWindow(g_win);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
