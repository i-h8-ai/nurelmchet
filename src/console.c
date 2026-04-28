#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include <pthread.h>
#include "chat.h"
#include "console.h"

/* ANSI helpers */
#define RESET  "\033[0m"
#define DIM    "\033[2m"
#define BOLD   "\033[1m"
#define GREY   "\033[90m"
#define CYAN   "\033[96m"
#define GREEN  "\033[92m"
#define YELLOW "\033[93m"
#define RED    "\033[91m"

static const char *ansi_from_hex(const char *hex, char *buf, size_t bufsz) {
    if (!hex || hex[0] != '#' || (int)strlen(hex) < 7) { buf[0] = 0; return ""; }
    unsigned r, g, b;
    if (sscanf(hex + 1, "%02x%02x%02x", &r, &g, &b) != 3) { buf[0] = 0; return ""; }
    snprintf(buf, bufsz, "\033[38;2;%u;%u;%um", r, g, b);
    return buf;
}

static void print_message(const ChatMessage *msg) {
    char ts[12] = "";
    chat_fmt_ts(ts, sizeof(ts), msg->timestamp);

    char content[1024];
    chat_strip_html(content, sizeof(content), msg->content);

    if (msg->timestamp == 0) {
        /* system message */
        printf("%s  *** %s%s\n", GREY, content, RESET);
    } else {
        char color_esc[32] = "";
        if (msg->use_custom_color)
            ansi_from_hex(msg->color, color_esc, sizeof(color_esc));
        const char *realm = msg->realm[0] ? msg->realm : "";
        if (realm[0])
            printf("%s[%s]%s %s[%s]%s %s%s%s%s: %s\n",
                   GREY, ts, RESET,
                   GREY, realm, RESET,
                   color_esc, BOLD, msg->username, RESET,
                   content);
        else
            printf("%s[%s]%s %s%s%s%s: %s\n",
                   GREY, ts, RESET,
                   color_esc, BOLD, msg->username, RESET,
                   content);
    }
    fflush(stdout);
}

static ChatState *g_state;
static volatile int g_last_printed = 0;

static void *stdin_reader(void *arg) {
    (void)arg;
    printf(CYAN "Commands: auth | join [realm] | history | participants | send <msg> | quit\n" RESET);
    printf("> "); fflush(stdout);

    char line[600];
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\n")] = 0;
        if (!line[0]) { printf("> "); fflush(stdout); continue; }

        if (strcmp(line, "auth") == 0) {
            chat_push_cmd(g_state, CMD_AUTH, NULL);
        } else if (strncmp(line, "join", 4) == 0 && (line[4] == ' ' || line[4] == 0)) {
            char *r = line + 4;
            while (*r == ' ') r++;
            chat_push_cmd(g_state, CMD_JOIN, r[0] ? r : NULL);
        } else if (strcmp(line, "history") == 0 || strcmp(line, "msgs") == 0) {
            chat_push_cmd(g_state, CMD_GET_HISTORY, NULL);
        } else if (strcmp(line, "participants") == 0 || strcmp(line, "part") == 0) {
            chat_push_cmd(g_state, CMD_GET_PARTICIPANTS, NULL);
        } else if (strncmp(line, "send ", 5) == 0) {
            chat_push_cmd(g_state, CMD_SEND_MESSAGE, line + 5);
        } else if (strcmp(line, "quit") == 0 || strcmp(line, "q") == 0) {
            g_state->running = false;
            break;
        } else {
            printf("Unknown command.\n");
        }
        printf("> "); fflush(stdout);
    }
    g_state->running = false;
    return NULL;
}

int console_run(ChatState *state) {
    g_state = state;

    pthread_t reader;
    pthread_create(&reader, NULL, stdin_reader, NULL);

    struct timespec sleep_ts = {0, 50000000}; /* 50ms */
    while (state->running) {
        pthread_mutex_lock(&state->lock);
        int cur = state->msg_count;
        pthread_mutex_unlock(&state->lock);

        while (g_last_printed < cur) {
            pthread_mutex_lock(&state->lock);
            ChatMessage msg = state->messages[g_last_printed % MAX_MESSAGES];
            pthread_mutex_unlock(&state->lock);
            print_message(&msg);
            g_last_printed++;
        }
        nanosleep(&sleep_ts, NULL);
    }

    pthread_join(reader, NULL);
    return 0;
}
