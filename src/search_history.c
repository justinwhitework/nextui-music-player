#include <unistd.h>
#include "search_history.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "defines.h"

#define HISTORY_FILE SHARED_USERDATA_PATH "/music-player/search_history.txt"
#define QUERY_MAX 256

static char history[SEARCH_HISTORY_MAX][QUERY_MAX];
static int history_count = 0;
static bool history_loaded = false;

static void trim_inplace(char* s) {
    if (!s) return;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[--len] = '\0';
    }
    char* start = s;
    while (*start == ' ') start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
}

static void save_history(void) {
    mkdir(SHARED_USERDATA_PATH "/music-player", 0755);

    FILE* f = fopen(HISTORY_FILE, "w");
    if (!f) return;

    for (int i = 0; i < history_count; i++) {
        fprintf(f, "%s\n", history[i]);
    }
    fclose(f);
}

static void load_history(void) {
    if (history_loaded) return;
    history_loaded = true;
    history_count = 0;

    FILE* f = fopen(HISTORY_FILE, "r");
    if (!f) return;

    char line[QUERY_MAX];
    while (history_count < SEARCH_HISTORY_MAX && fgets(line, sizeof(line), f)) {
        trim_inplace(line);
        if (!line[0]) continue;
        strncpy(history[history_count], line, QUERY_MAX - 1);
        history[history_count][QUERY_MAX - 1] = '\0';
        history_count++;
    }
    fclose(f);
}

void SearchHistory_init(void) {
    load_history();
}

int SearchHistory_count(void) {
    load_history();
    return history_count;
}

const char* SearchHistory_get(int index) {
    load_history();
    if (index < 0 || index >= history_count) return NULL;
    return history[index];
}

void SearchHistory_add(const char* query) {
    if (!query) return;
    load_history();

    char trimmed[QUERY_MAX];
    strncpy(trimmed, query, sizeof(trimmed) - 1);
    trimmed[sizeof(trimmed) - 1] = '\0';
    trim_inplace(trimmed);
    if (!trimmed[0]) return;

    int existing = -1;
    for (int i = 0; i < history_count; i++) {
        if (strcmp(history[i], trimmed) == 0) {
            existing = i;
            break;
        }
    }

    if (existing == 0) {
        save_history();
        return;
    }

    if (existing > 0) {
        char temp[QUERY_MAX];
        strncpy(temp, history[existing], sizeof(temp) - 1);
        for (int i = existing; i > 0; i--) {
            strncpy(history[i], history[i - 1], QUERY_MAX - 1);
            history[i][QUERY_MAX - 1] = '\0';
        }
        strncpy(history[0], temp, QUERY_MAX - 1);
        history[0][QUERY_MAX - 1] = '\0';
        save_history();
        return;
    }

    if (history_count < SEARCH_HISTORY_MAX) {
        for (int i = history_count; i > 0; i--) {
            strncpy(history[i], history[i - 1], QUERY_MAX - 1);
            history[i][QUERY_MAX - 1] = '\0';
        }
        history_count++;
    } else {
        for (int i = SEARCH_HISTORY_MAX - 1; i > 0; i--) {
            strncpy(history[i], history[i - 1], QUERY_MAX - 1);
            history[i][QUERY_MAX - 1] = '\0';
        }
    }

    strncpy(history[0], trimmed, QUERY_MAX - 1);
    history[0][QUERY_MAX - 1] = '\0';
    save_history();
}

void SearchHistory_clear(void) {
    history_count = 0;
    history[0][0] = '\0';
    history_loaded = true;
    unlink(HISTORY_FILE);
}
