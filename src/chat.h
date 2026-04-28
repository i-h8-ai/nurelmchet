#pragma once
#include <pthread.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stddef.h>

#define MAX_MESSAGES     300
#define MAX_PARTICIPANTS 100
#define CMD_QUEUE_SIZE   16

typedef struct {
    char username[64];
    char content[1024];
    char realm[64];
    char color[8];        /* #RRGGBB or empty */
    long long timestamp;  /* ms epoch, 0 = system msg */
    bool use_custom_color;
} ChatMessage;

typedef struct {
    char username[64];
    char color[8];
    bool use_custom_color;
    bool is_streamer, is_moderator, is_guest;
    long long joined_at;
} Participant;

typedef enum {
    CMD_AUTH = 0,
    CMD_JOIN,
    CMD_GET_HISTORY,
    CMD_GET_PARTICIPANTS,
    CMD_SEND_MESSAGE,
    CMD_QUIT,
} CommandType;

typedef struct {
    CommandType type;
    char arg[512];
} Command;

typedef struct {
    ChatMessage  messages[MAX_MESSAGES];
    int          msg_count;   /* total ever added; ring index = msg_count % MAX_MESSAGES */

    Participant  participants[MAX_PARTICIPANTS];
    int          part_count;

    Command      cmd_queue[CMD_QUEUE_SIZE];
    int          cmd_head, cmd_tail;

    bool connected, authenticated, joined, running;
    char status[256];
    char realm[64];
    char username[64];
    char userColor[8];   /* #RRGGBB received from auth_success */
    char token[2048];
    char apiKey[256];
    char ws_url[256];

    pthread_mutex_t lock;
    pthread_cond_t  cmd_cond;
    bool new_messages;
} ChatState;

ChatState *chat_create(void);
void       chat_destroy(ChatState *s);

void chat_add_message(ChatState *s, const ChatMessage *msg);
void chat_add_system(ChatState *s, const char *fmt, ...);
void chat_set_status(ChatState *s, const char *fmt, ...);
void chat_push_cmd(ChatState *s, CommandType t, const char *arg);
bool chat_pop_cmd(ChatState *s, Command *out);

void chat_strip_html(char *dst, size_t dstlen, const char *src);
void chat_fmt_ts(char *dst, size_t dstlen, long long ms);
