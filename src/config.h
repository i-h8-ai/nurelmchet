#pragma once
#include <stdbool.h>

typedef struct {
    char cache_dir[512];
    int  sticker_size;
    int  avatar_size;
    int  max_messages;
    bool download_stickers;
    bool download_avatars;
} AppConfig;

void config_defaults(AppConfig *cfg);
void config_load(AppConfig *cfg, const char *path);  /* silent if file missing */
