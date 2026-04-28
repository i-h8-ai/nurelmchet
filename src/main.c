#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include <curl/curl.h>

#include "chat.h"
#include "config.h"
#include "imgcache.h"
#include "ws.h"
#include "gui.h"
#include "console.h"

static void load_env(ChatState *state, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "warning: cannot open %s\n", path); return; }
    char line[2200];
    while (fgets(line, sizeof(line), f)) {
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
        if (strcmp(key, "BASE_URL") == 0) strncpy(state->base_url, val, sizeof(state->base_url) - 1);
    }
    fclose(f);
    fprintf(stderr, "[env] WS_URL  = [%s]\n", state->ws_url);
    fprintf(stderr, "[env] REALM   = [%s]\n", state->realm);
    fprintf(stderr, "[env] API_KEY = [%s]\n", state->apiKey[0] ? "(set)" : "(not set)");
    fflush(stderr);
}

/* Derive https://host from wss://host/path */
static void derive_base_url(ChatState *state) {
    if (state->base_url[0]) return;  /* already set via .env */
    const char *u = state->ws_url;
    const char *host = u;
    if (strncmp(u, "wss://", 6) == 0)      host = u + 6;
    else if (strncmp(u, "ws://", 5) == 0)  host = u + 5;
    const char *slash = strchr(host, '/');
    if (slash)
        snprintf(state->base_url, sizeof(state->base_url),
                 "https://%.*s", (int)(slash - host), host);
    else
        snprintf(state->base_url, sizeof(state->base_url), "https://%s", host);
    fprintf(stderr, "[env] BASE_URL = [%s] (derived)\n", state->base_url);
    fflush(stderr);
}

int main(int argc, char **argv) {
    bool console_mode = false;
    const char *env_path    = ".env";
    const char *config_path = "nurealmschat.conf";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--console") == 0)
            console_mode = true;
        else if (strcmp(argv[i], "--env") == 0 && i + 1 < argc)
            env_path = argv[++i];
        else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc)
            config_path = argv[++i];
    }

    AppConfig cfg;
    config_defaults(&cfg);
    config_load(&cfg, config_path);

    ChatState *state = chat_create();
    load_env(state, env_path);

    if (!state->ws_url[0]) {
        fprintf(stderr, "error: WS_URL not set in %s\n", env_path);
        chat_destroy(state); return 1;
    }
    if (strstr(state->ws_url, "FIXME")) {
        fprintf(stderr, "error: WS_URL still contains FIXME — edit your .env\n");
        chat_destroy(state); return 1;
    }

    derive_base_url(state);
    state->max_messages = cfg.max_messages;

    curl_global_init(CURL_GLOBAL_ALL);
    imgcache_init(&cfg, state->base_url);
    ws_start(state);

    int ret;
    if (console_mode)
        ret = console_run(state);
    else
        ret = gui_run(state, &cfg);

    state->running = false;
    ws_stop(state);
    imgcache_shutdown();
    curl_global_cleanup();
    chat_destroy(state);
    return ret;
}
