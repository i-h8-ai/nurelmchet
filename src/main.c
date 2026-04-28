#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "chat.h"
#include "ws.h"
#include "gui.h"
#include "console.h"

static void load_env(ChatState *state, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "warning: cannot open %s\n", path); return; }
    char line[2200];
    while (fgets(line, sizeof(line), f)) {
        /* strip both \n and \r so Windows line-endings don't corrupt values */
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = line, *val = eq + 1;
        if (strcmp(key, "TOKEN")    == 0) strncpy(state->token,  val, sizeof(state->token)  - 1);
        if (strcmp(key, "API_KEY")  == 0) strncpy(state->apiKey, val, sizeof(state->apiKey) - 1);
        if (strcmp(key, "WS_URL")   == 0) strncpy(state->ws_url, val, sizeof(state->ws_url) - 1);
        if (strcmp(key, "REALM_ID") == 0) strncpy(state->realm,  val, sizeof(state->realm)  - 1);
    }
    fclose(f);
    fprintf(stderr, "[env] WS_URL  = [%s]\n", state->ws_url);
    fprintf(stderr, "[env] REALM   = [%s]\n", state->realm);
    fprintf(stderr, "[env] API_KEY = [%s]\n", state->apiKey[0] ? "(set)" : "(not set)");
    fflush(stderr);
}

int main(int argc, char **argv) {
    bool console_mode = false;
    const char *env_path = ".env";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--console") == 0) console_mode = true;
        else if (strcmp(argv[i], "--env") == 0 && i + 1 < argc) env_path = argv[++i];
    }

    ChatState *state = chat_create();
    load_env(state, env_path);

    if (!state->ws_url[0]) {
        fprintf(stderr, "error: WS_URL not set in %s\n", env_path);
        chat_destroy(state);
        return 1;
    }
    if (strstr(state->ws_url, "FIXME")) {
        fprintf(stderr, "error: WS_URL still contains FIXME — edit your .env\n");
        chat_destroy(state);
        return 1;
    }

    ws_start(state);

    int ret;
    if (console_mode)
        ret = console_run(state);
    else
        ret = gui_run(state);

    state->running = false;
    ws_stop(state);
    chat_destroy(state);
    return ret;
}
