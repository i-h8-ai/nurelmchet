#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include <sys/select.h>

#include <curl/curl.h>
#include <curl/websockets.h>
#include <cjson/cJSON.h>

#include "chat.h"
#include "ws.h"

#define WS_BUFSIZE (256 * 1024)

static void ws_send_json(CURL *curl, cJSON *obj) {
    char *s = cJSON_PrintUnformatted(obj);
    if (!s) return;
    fprintf(stderr, "[ws] send: %s\n", s);
    fflush(stderr);
    size_t sent;
    CURLcode rc = curl_ws_send(curl, s, strlen(s), &sent, 0, CURLWS_TEXT);
    if (rc != CURLE_OK) {
        fprintf(stderr, "[ws] send failed: %s\n", curl_easy_strerror(rc));
        fflush(stderr);
    }
    free(s);
}

static void handle_participants(ChatState *state, cJSON *root) {
    cJSON *parts = cJSON_GetObjectItem(root, "participants");
    cJSON *cnt   = cJSON_GetObjectItem(root, "participantCount");
    int count = (cnt && cJSON_IsNumber(cnt)) ? cnt->valueint : 0;

    pthread_mutex_lock(&state->lock);
    state->part_count = 0;
    if (cJSON_IsArray(parts)) {
        cJSON *p;
        cJSON_ArrayForEach(p, parts) {
            if (state->part_count >= MAX_PARTICIPANTS) break;
            Participant *pt = &state->participants[state->part_count++];
            memset(pt, 0, sizeof(*pt));
            cJSON *u    = cJSON_GetObjectItem(p, "username");
            cJSON *col  = cJSON_GetObjectItem(p, "userColor");
            cJSON *cust = cJSON_GetObjectItem(p, "useCustomColor");
            cJSON *str  = cJSON_GetObjectItem(p, "isStreamer");
            cJSON *mod  = cJSON_GetObjectItem(p, "isModerator");
            cJSON *gu   = cJSON_GetObjectItem(p, "isGuest");
            cJSON *ja   = cJSON_GetObjectItem(p, "joinedAt");
            if (u   && u->valuestring)
                strncpy(pt->username, u->valuestring, sizeof(pt->username) - 1);
            if (col && col->valuestring)
                strncpy(pt->color, col->valuestring, sizeof(pt->color) - 1);
            pt->use_custom_color = cJSON_IsTrue(cust);
            pt->is_streamer  = cJSON_IsTrue(str);
            pt->is_moderator = cJSON_IsTrue(mod);
            pt->is_guest     = cJSON_IsTrue(gu);
            pt->joined_at    = (ja && cJSON_IsNumber(ja)) ? (long long)ja->valuedouble : 0;
        }
        if (count == 0) count = state->part_count;
    }
    pthread_mutex_unlock(&state->lock);

    chat_add_system(state, "── %d participant(s) ──", count);

    pthread_mutex_lock(&state->lock);
    for (int i = 0; i < state->part_count; i++) {
        Participant *pt = &state->participants[i];
        char tags[64] = "";
        if (pt->is_streamer)  strcat(tags, " [streamer]");
        if (pt->is_moderator) strcat(tags, " [mod]");
        if (pt->is_guest)     strcat(tags, " [guest]");
        /* unlock to call chat_add_system (which re-locks) */
        pthread_mutex_unlock(&state->lock);
        chat_add_system(state, "  %s%s", pt->username, tags);
        pthread_mutex_lock(&state->lock);
    }
    pthread_mutex_unlock(&state->lock);
}

static ChatMessage parse_chat_msg(cJSON *m) {
    ChatMessage cm = {0};
    cJSON *u    = cJSON_GetObjectItem(m, "username");
    cJSON *c    = cJSON_GetObjectItem(m, "content");
    cJSON *r    = cJSON_GetObjectItem(m, "realmId");
    cJSON *col  = cJSON_GetObjectItem(m, "userColor");
    cJSON *cust = cJSON_GetObjectItem(m, "useCustomColor");
    cJSON *ts   = cJSON_GetObjectItem(m, "timestamp");
    if (u   && u->valuestring) strncpy(cm.username, u->valuestring, sizeof(cm.username) - 1);
    if (c   && c->valuestring) strncpy(cm.content,  c->valuestring, sizeof(cm.content)  - 1);
    if (r   && r->valuestring) strncpy(cm.realm,    r->valuestring, sizeof(cm.realm)    - 1);
    if (col && col->valuestring) strncpy(cm.color,  col->valuestring, sizeof(cm.color)  - 1);
    cm.use_custom_color = cJSON_IsTrue(cust);
    cm.timestamp = (ts && cJSON_IsNumber(ts)) ? (long long)ts->valuedouble : 0LL;
    return cm;
}

static void ingest_messages(ChatState *state, cJSON *msgs) {
    if (!cJSON_IsArray(msgs)) return;
    int n = cJSON_GetArraySize(msgs);
    chat_add_system(state, "── history (%d messages) ──", n);
    cJSON *m;
    cJSON_ArrayForEach(m, msgs) {
        ChatMessage cm = parse_chat_msg(m);
        chat_add_message(state, &cm);
    }
}

/* curl handle passed in so join_success can fire follow-up sends inline */
static void process_message(ChatState *state, CURL *curl, const char *json_str) {
    cJSON *root = cJSON_Parse(json_str);
    if (!root) return;

    cJSON *type_item = cJSON_GetObjectItem(root, "type");
    const char *type = cJSON_GetStringValue(type_item);
    if (!type) { cJSON_Delete(root); return; }

    /* welcome — server greets us, may precede auth_success for guests */
    if (strcmp(type, "welcome") == 0 || strcmp(type, "auth_success") == 0) {
        cJSON *un  = cJSON_GetObjectItem(root, "username");
        cJSON *uc  = cJSON_GetObjectItem(root, "userColor");
        pthread_mutex_lock(&state->lock);
        state->authenticated = true;
        if (un && un->valuestring)
            strncpy(state->username,  un->valuestring, sizeof(state->username)  - 1);
        if (uc && uc->valuestring)
            strncpy(state->userColor, uc->valuestring, sizeof(state->userColor) - 1);
        char uname[64];
        strncpy(uname, state->username, sizeof(uname) - 1);
        pthread_mutex_unlock(&state->lock);
        chat_set_status(state, "authenticated as %s", uname);
        chat_add_system(state, "✓ Authenticated as %s", uname);

    } else if (strcmp(type, "join_success") == 0) {
        pthread_mutex_lock(&state->lock);
        state->joined = true;
        char realm[64];
        strncpy(realm, state->realm, sizeof(realm) - 1);
        pthread_mutex_unlock(&state->lock);
        chat_set_status(state, "joined %s", realm);
        chat_add_system(state, "✓ Joined realm %s — loading history & participants...", realm);

        /* mirror what the JS does after join_success */
        cJSON *obj;

        obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "type", "set_chat_filter");
        cJSON_AddTrueToObject(obj, "globalChat");
        ws_send_json(curl, obj);
        cJSON_Delete(obj);

        obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "type", "get_global_history");
        ws_send_json(curl, obj);
        cJSON_Delete(obj);

        obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "type", "get_participants");
        ws_send_json(curl, obj);
        cJSON_Delete(obj);

    /* history and global_history both carry a messages array */
    } else if (strcmp(type, "global_history") == 0 || strcmp(type, "history") == 0) {
        ingest_messages(state, cJSON_GetObjectItem(root, "messages"));

    /* participants_list is the actual response type (not "participants") */
    } else if (strcmp(type, "participants_list") == 0 || strcmp(type, "participants") == 0) {
        handle_participants(state, root);

    } else if (strcmp(type, "participant_joined") == 0) {
        cJSON *p = cJSON_GetObjectItem(root, "participant");
        if (p) {
            cJSON *u = cJSON_GetObjectItem(p, "username");
            if (u && u->valuestring)
                chat_add_system(state, "→ %s joined", u->valuestring);
        }

    } else if (strcmp(type, "participant_left") == 0) {
        cJSON *u = cJSON_GetObjectItem(root, "username");
        if (u && u->valuestring)
            chat_add_system(state, "← %s left", u->valuestring);

    /* new_message is the live incoming chat type; also accept "message" as fallback */
    } else if (strcmp(type, "new_message") == 0 || strcmp(type, "message") == 0) {
        ChatMessage cm = parse_chat_msg(root);
        chat_add_message(state, &cm);

    } else if (strcmp(type, "message_deleted") == 0) {
        /* nothing to do without a full message list UI */

    } else if (strcmp(type, "error") == 0) {
        const char *fields[] = { "error", "message", "reason", "msg", "detail", NULL };
        const char *errmsg = NULL;
        for (int fi = 0; fields[fi] && !errmsg; fi++) {
            cJSON *em = cJSON_GetObjectItem(root, fields[fi]);
            if (em) errmsg = cJSON_GetStringValue(em);
        }
        char *raw = cJSON_PrintUnformatted(root);
        fprintf(stderr, "[ws] server error: %s\n", raw ? raw : json_str);
        fflush(stderr);
        chat_add_system(state, "ERROR: %s", errmsg ? errmsg : (raw ? raw : json_str));
        chat_set_status(state, "error: %s", errmsg ? errmsg : "see terminal");
        free(raw);

    } else {
        fprintf(stderr, "[ws] unhandled type=%s  raw=%.*s\n", type, 300, json_str);
        fflush(stderr);
    }

    cJSON_Delete(root);
}

static void do_send_cmd(CURL *curl, Command *cmd, ChatState *state, char *realm, const char *token) {
    cJSON *obj = cJSON_CreateObject();
    switch (cmd->type) {
        case CMD_AUTH: {
            cJSON_AddStringToObject(obj, "type", "auth");
            if (token[0])
                cJSON_AddStringToObject(obj, "token", token);
            pthread_mutex_lock(&state->lock);
            char ak[sizeof(state->apiKey)];
            strncpy(ak, state->apiKey, sizeof(ak) - 1); ak[sizeof(ak)-1] = 0;
            pthread_mutex_unlock(&state->lock);
            if (ak[0]) {
                cJSON_AddStringToObject(obj, "apiKey",  ak);
                cJSON_AddStringToObject(obj, "api_key", ak);
            }
            break;
        }
        case CMD_JOIN: {
            const char *r = cmd->arg[0] ? cmd->arg : realm;
            if (cmd->arg[0]) strncpy(realm, cmd->arg, 63);
            cJSON_AddStringToObject(obj, "type", "join");
            cJSON_AddStringToObject(obj, "realmId", r);
            break;
        }
        case CMD_GET_HISTORY:
            cJSON_AddStringToObject(obj, "type", "set_chat_filter");
            cJSON_AddTrueToObject(obj, "globalChat");
            ws_send_json(curl, obj);
            cJSON_Delete(obj);
            obj = cJSON_CreateObject();
            cJSON_AddStringToObject(obj, "type", "get_global_history");
            break;
        case CMD_GET_PARTICIPANTS:
            cJSON_AddStringToObject(obj, "type", "get_participants");
            break;
        case CMD_SEND_MESSAGE: {
            pthread_mutex_lock(&state->lock);
            char uc[8];
            strncpy(uc, state->userColor[0] ? state->userColor : "#FFFFFF", sizeof(uc) - 1);
            pthread_mutex_unlock(&state->lock);
            cJSON_AddStringToObject(obj, "type", "message");
            cJSON_AddStringToObject(obj, "content", cmd->arg);
            cJSON_AddStringToObject(obj, "userColor", uc);
            break;
        }
        default:
            break;
    }
    ws_send_json(curl, obj);
    cJSON_Delete(obj);
}

static void *ws_thread_func(void *arg) {
    ChatState *state = (ChatState *)arg;

    char *buf = malloc(WS_BUFSIZE);
    if (!buf) return NULL;

    char url[sizeof(state->ws_url)];
    char token[sizeof(state->token)];
    char apiKey[sizeof(state->apiKey)];
    char realm[sizeof(state->realm)];

    int retry_delay = 3; /* seconds, doubles each failure up to 30s */

    /* ── outer reconnect loop ─────────────────────────────────────────── */
    while (state->running) {

        /* re-read config each attempt so a token update takes effect */
        pthread_mutex_lock(&state->lock);
        strncpy(url,    state->ws_url, sizeof(url)    - 1); url[sizeof(url)-1]      = 0;
        strncpy(token,  state->token,  sizeof(token)  - 1); token[sizeof(token)-1]  = 0;
        strncpy(apiKey, state->apiKey, sizeof(apiKey) - 1); apiKey[sizeof(apiKey)-1]= 0;
        strncpy(realm,  state->realm,  sizeof(realm)  - 1); realm[sizeof(realm)-1]  = 0;
        /* flush any stale commands left from a previous session */
        state->cmd_head = state->cmd_tail = 0;
        pthread_mutex_unlock(&state->lock);

        /* auto-prepend wss:// if the user forgot the scheme */
        if (strncmp(url, "ws://", 5) != 0 && strncmp(url, "wss://", 6) != 0) {
            char tmp[sizeof(url)];
            snprintf(tmp, sizeof(tmp), "wss://%s", url);
            strncpy(url, tmp, sizeof(url) - 1);
            fprintf(stderr, "[ws] no scheme in WS_URL — prepended wss://: %s\n", url);
            fflush(stderr);
        }

        CURL *curl = curl_easy_init();
        if (!curl) {
            chat_set_status(state, "curl init failed");
            break;
        }

        /* append ?realmId=<realm> — server expects it in the URL */
        char full_url[600];
        const char *sep = strchr(url, '?') ? "&" : "?";
        snprintf(full_url, sizeof(full_url), "%s%srealmId=%s", url, sep, realm);

        curl_easy_setopt(curl, CURLOPT_URL,            full_url);
        curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY,   2L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, 60L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "nurealmschat/1.0");

        /* send API key as HTTP headers during WebSocket upgrade handshake */
        struct curl_slist *hdrs = NULL;
        if (apiKey[0]) {
            char hdr[300];
            snprintf(hdr, sizeof(hdr), "X-Api-Key: %s", apiKey);
            hdrs = curl_slist_append(hdrs, hdr);
            snprintf(hdr, sizeof(hdr), "Authorization: Bearer %s", apiKey);
            hdrs = curl_slist_append(hdrs, hdr);
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
            fprintf(stderr, "[ws] sending API key as X-Api-Key + Authorization headers\n");
            fflush(stderr);
        }

        chat_set_status(state, "connecting...");
        fprintf(stderr, "[ws] connecting to\n    [%s]\n", full_url);
        fflush(stderr);

        CURLcode rc = curl_easy_perform(curl);
        if (rc != CURLE_OK) {
            fprintf(stderr, "[ws] connect failed: %s\n", curl_easy_strerror(rc));
            fflush(stderr);
            chat_set_status(state, "connect failed: %s — retry in %ds", curl_easy_strerror(rc), retry_delay);
            chat_add_system(state, "Connection failed: %s — retrying in %ds", curl_easy_strerror(rc), retry_delay);
            curl_easy_cleanup(curl);
            for (int i = 0; i < retry_delay * 10 && state->running; i++) {
                struct timespec ts = {0, 100000000};
                nanosleep(&ts, NULL);
            }
            continue;
        }

        pthread_mutex_lock(&state->lock);
        state->connected     = true;
        state->authenticated = false;
        state->joined        = false;
        pthread_mutex_unlock(&state->lock);
        retry_delay = 3; /* reset backoff on success */
        chat_set_status(state, "connected — sending auth + join...");

        curl_socket_t sockfd = CURL_SOCKET_BAD;
        curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &sockfd);

        /* send auth then join immediately on open — matches JS client behaviour */
        {
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddStringToObject(obj, "type", "auth");
            /* include token if present */
            if (token[0])
                cJSON_AddStringToObject(obj, "token", token);
            /* include API key under both field name variants the server might expect */
            if (apiKey[0]) {
                cJSON_AddStringToObject(obj, "apiKey",  apiKey);
                cJSON_AddStringToObject(obj, "api_key", apiKey);
            }
            ws_send_json(curl, obj);
            cJSON_Delete(obj);
        }
        {
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddStringToObject(obj, "type", "join");
            cJSON_AddStringToObject(obj, "realmId", realm);
            ws_send_json(curl, obj);
            cJSON_Delete(obj);
        }

        /* ── inner recv / send loop ───────────────────────────────────── */
        bool quit = false;
        while (state->running && !quit) {

            /* drain command queue */
            pthread_mutex_lock(&state->lock);
            Command cmd;
            bool has_cmd = chat_pop_cmd(state, &cmd);
            pthread_mutex_unlock(&state->lock);

            if (has_cmd) {
                if (cmd.type == CMD_QUIT) { quit = true; break; }
                do_send_cmd(curl, &cmd, state, realm, token);
            }

            /* poll socket */
            if (sockfd == CURL_SOCKET_BAD) break;
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(sockfd, &rfds);
            struct timeval tv = {0, 50000}; /* 50ms */
            int ready = select((int)sockfd + 1, &rfds, NULL, NULL, &tv);
            if (ready < 0) break;
            if (ready == 0) continue;

            size_t nread = 0;
            const struct curl_ws_frame *frame = NULL;
            rc = curl_ws_recv(curl, buf, WS_BUFSIZE - 1, &nread, &frame);
            if (rc == CURLE_AGAIN) continue;
            if (rc != CURLE_OK) {
                fprintf(stderr, "[ws] recv error: %s\n", curl_easy_strerror(rc));
                fflush(stderr);
                chat_add_system(state, "Recv error: %s", curl_easy_strerror(rc));
                break;
            }
            if (nread == 0) continue; /* empty frame, keep going */

            buf[nread] = 0;

            if (frame && (frame->flags & CURLWS_TEXT)) {
                fprintf(stderr, "[ws] recv: %.*s\n", (int)(nread > 400 ? 400 : nread), buf);
                fflush(stderr);
                process_message(state, curl, buf);
            } else if (frame && (frame->flags & CURLWS_PING)) {
                /* reply with pong so the server doesn't drop us */
                size_t sent;
                curl_ws_send(curl, buf, nread, &sent, 0, CURLWS_PONG);
                fprintf(stderr, "[ws] ping → pong\n");
                fflush(stderr);
            } else if (frame && (frame->flags & CURLWS_CLOSE)) {
                fprintf(stderr, "[ws] server sent close frame\n");
                fflush(stderr);
                chat_add_system(state, "Server closed the connection");
                break;
            }
        }

        /* ── cleanup this connection ──────────────────────────────────── */
        pthread_mutex_lock(&state->lock);
        state->connected    = false;
        state->authenticated = false;
        state->joined        = false;
        pthread_mutex_unlock(&state->lock);

        if (hdrs) curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);

        if (quit || !state->running) break;

        chat_set_status(state, "disconnected — reconnecting in %ds...", retry_delay);
        chat_add_system(state, "Disconnected — reconnecting in %ds...", retry_delay);
        fprintf(stderr, "[ws] disconnected, reconnecting in %ds\n", retry_delay);
        fflush(stderr);

        for (int i = 0; i < retry_delay * 10 && state->running; i++) {
            struct timespec ts = {0, 100000000};
            nanosleep(&ts, NULL);
        }

        /* exponential backoff: 3 → 6 → 12 → 30s max */
        retry_delay = retry_delay * 2;
        if (retry_delay > 30) retry_delay = 30;
    }

    free(buf);
    chat_set_status(state, "disconnected");
    return NULL;
}

static pthread_t g_ws_thread;

void ws_start(ChatState *s) {
    curl_global_init(CURL_GLOBAL_ALL);
    pthread_create(&g_ws_thread, NULL, ws_thread_func, s);
}

void ws_stop(ChatState *s) {
    s->running = false;
    chat_push_cmd(s, CMD_QUIT, NULL);
    pthread_join(g_ws_thread, NULL);
    curl_global_cleanup();
}
