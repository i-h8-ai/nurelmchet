#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "config.h"

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)*(e - 1))) e--;
    *e = 0;
    return s;
}

void config_defaults(AppConfig *cfg) {
    const char *home = getenv("HOME");
    if (home)
        snprintf(cfg->cache_dir, sizeof(cfg->cache_dir),
                 "%s/.cache/nurealmschat", home);
    else
        snprintf(cfg->cache_dir, sizeof(cfg->cache_dir), ".cache/nurealmschat");
    cfg->sticker_size         = 120;
    cfg->avatar_size          = 120;
    cfg->max_messages         = 20;
    cfg->download_stickers    = true;
    cfg->download_avatars     = true;
}

void config_load(AppConfig *cfg, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[640];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(line);
        char *val = trim(eq + 1);
        if (!key[0] || key[0] == '#') continue;
        if (strcmp(key, "cache_dir") == 0)
            strncpy(cfg->cache_dir, val, sizeof(cfg->cache_dir) - 1);
        else if (strcmp(key, "sticker_size") == 0)
            cfg->sticker_size = atoi(val);
        else if (strcmp(key, "avatar_size") == 0)
            cfg->avatar_size = atoi(val);
        else if (strcmp(key, "max_messages") == 0)
            cfg->max_messages = atoi(val);
        else if (strcmp(key, "download_missing_stickers") == 0)
            cfg->download_stickers = (val[0]=='1'||val[0]=='t'||val[0]=='y');
        else if (strcmp(key, "download_missing_avatars") == 0)
            cfg->download_avatars  = (val[0]=='1'||val[0]=='t'||val[0]=='y');
    }
    fclose(f);
    if (cfg->max_messages < 1)   cfg->max_messages = 1;
    if (cfg->max_messages > 100) cfg->max_messages = 100;
    if (cfg->sticker_size < 16)  cfg->sticker_size = 16;
    if (cfg->avatar_size  < 16)  cfg->avatar_size  = 16;
}
