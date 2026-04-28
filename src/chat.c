#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "chat.h"

ChatState *chat_create(void) {
    ChatState *s = calloc(1, sizeof(ChatState));
    s->running = true;
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->cmd_cond, NULL);
    return s;
}

void chat_destroy(ChatState *s) {
    pthread_mutex_destroy(&s->lock);
    pthread_cond_destroy(&s->cmd_cond);
    free(s);
}

void chat_add_message(ChatState *s, const ChatMessage *msg) {
    pthread_mutex_lock(&s->lock);
    int idx = s->msg_count % MAX_MESSAGES;
    s->messages[idx] = *msg;
    s->msg_count++;
    s->new_messages = true;
    pthread_mutex_unlock(&s->lock);
}

void chat_add_system(ChatState *s, const char *fmt, ...) {
    ChatMessage msg = {0};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg.content, sizeof(msg.content), fmt, ap);
    va_end(ap);
    strncpy(msg.username, "system", sizeof(msg.username) - 1);
    msg.timestamp = 0; /* marks as system */
    chat_add_message(s, &msg);
}

void chat_set_status(ChatState *s, const char *fmt, ...) {
    pthread_mutex_lock(&s->lock);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->status, sizeof(s->status), fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&s->lock);
}

void chat_push_cmd(ChatState *s, CommandType t, const char *arg) {
    pthread_mutex_lock(&s->lock);
    int next = (s->cmd_tail + 1) % CMD_QUEUE_SIZE;
    if (next != s->cmd_head) {
        s->cmd_queue[s->cmd_tail].type = t;
        if (arg)
            strncpy(s->cmd_queue[s->cmd_tail].arg, arg, sizeof(s->cmd_queue[0].arg) - 1);
        else
            s->cmd_queue[s->cmd_tail].arg[0] = 0;
        s->cmd_tail = next;
        pthread_cond_signal(&s->cmd_cond);
    }
    pthread_mutex_unlock(&s->lock);
}

bool chat_pop_cmd(ChatState *s, Command *out) {
    if (s->cmd_head == s->cmd_tail) return false;
    *out = s->cmd_queue[s->cmd_head];
    s->cmd_head = (s->cmd_head + 1) % CMD_QUEUE_SIZE;
    return true;
}

void chat_strip_html(char *dst, size_t dstlen, const char *src) {
    size_t di = 0;
    const char *p = src;
    while (*p && di < dstlen - 1) {
        if (*p == '&') {
            if (strncmp(p, "&amp;",  5) == 0) { dst[di++] = '&'; p += 5; continue; }
            if (strncmp(p, "&lt;",  4) == 0)  { dst[di++] = '<'; p += 4; continue; }
            if (strncmp(p, "&gt;",  4) == 0)  { dst[di++] = '>'; p += 4; continue; }
            if (strncmp(p, "&nbsp;",6) == 0)  { dst[di++] = ' '; p += 6; continue; }
        }
        if (*p == '<') {
            /* replace <br> variants with space */
            if (strncasecmp(p, "<br>",   4) == 0 ||
                strncasecmp(p, "<br/>",  5) == 0 ||
                strncasecmp(p, "<br />", 6) == 0) {
                dst[di++] = ' ';
            }
            while (*p && *p != '>') p++;
            if (*p == '>') p++;
            continue;
        }
        dst[di++] = *p++;
    }
    dst[di] = 0;
}

void chat_fmt_ts(char *dst, size_t dstlen, long long ms) {
    if (ms <= 0) { dst[0] = 0; return; }
    time_t t = (time_t)(ms / 1000);
    struct tm *tm = localtime(&t);
    strftime(dst, dstlen, "%H:%M", tm);
}
