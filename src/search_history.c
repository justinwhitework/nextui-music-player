#include <unistd.h>
#include "search_history.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "defines.h"

#define HISTORY_FILE SHARED_USERDATA_PATH "/music-player/search_history.txt"
#define LINE_MAX 1024

static SearchHistoryEntry history[SEARCH_HISTORY_MAX];
static int history_count = 0;
static bool history_loaded = false;

static void trim_inplace(char* s) {
    if (!s) return;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\n' || s[len - 1] == '\r' || s[len - 1] == '\t')) {
        s[--len] = '\0';
    }
    char* start = s;
    while (*start == ' ') start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
}

static void parent_dir(const char* path, char* out, int out_size) {
    if (!out || out_size <= 0) return;
    out[0] = '\0';
    if (!path || !path[0]) return;

    strncpy(out, path, out_size - 1);
    out[out_size - 1] = '\0';
    char* slash = strrchr(out, '/');
    if (slash) *slash = '\0';
}

static void entry_from_result(const SearchResultRow* row, SearchHistoryEntry* entry) {
    if (!row || !entry) return;

    strncpy(entry->item_path, row->path, sizeof(entry->item_path) - 1);
    entry->item_path[sizeof(entry->item_path) - 1] = '\0';
    parent_dir(row->path, entry->art_dir, sizeof(entry->art_dir));

    if (row->type == SEARCH_ITEM_TRACK) {
        entry->art_type = SEARCH_HISTORY_ART_TRACK;
    } else {
        entry->art_type = SEARCH_HISTORY_ART_PLAYLIST;
    }
}

static void clear_entry_art(SearchHistoryEntry* entry) {
    if (!entry) return;
    entry->item_path[0] = '\0';
    entry->art_dir[0] = '\0';
    entry->art_type = SEARCH_HISTORY_ART_NONE;
}

static void save_history(void) {
    mkdir(SHARED_USERDATA_PATH "/music-player", 0755);

    FILE* f = fopen(HISTORY_FILE, "w");
    if (!f) return;

    for (int i = 0; i < history_count; i++) {
        const SearchHistoryEntry* e = &history[i];
        if (e->art_type == SEARCH_HISTORY_ART_NONE || !e->item_path[0]) {
            fprintf(f, "%s\n", e->query);
            continue;
        }

        const char* type = (e->art_type == SEARCH_HISTORY_ART_TRACK) ? "T" : "P";
        fprintf(f, "%s\t%s\t%s\t%s\n", e->query, type, e->item_path, e->art_dir);
    }
    fclose(f);
}

static SearchHistoryArtType parse_art_type(const char* s) {
    if (!s || !s[0]) return SEARCH_HISTORY_ART_NONE;
    if (s[0] == 'T' && s[1] == '\0') return SEARCH_HISTORY_ART_TRACK;
    if (s[0] == 'P' && s[1] == '\0') return SEARCH_HISTORY_ART_PLAYLIST;
    return SEARCH_HISTORY_ART_NONE;
}

static void load_history(void) {
    if (history_loaded) return;
    history_loaded = true;
    history_count = 0;

    FILE* f = fopen(HISTORY_FILE, "r");
    if (!f) return;

    char line[LINE_MAX];
    while (history_count < SEARCH_HISTORY_MAX && fgets(line, sizeof(line), f)) {
        trim_inplace(line);
        if (!line[0]) continue;

        SearchHistoryEntry* e = &history[history_count];
        memset(e, 0, sizeof(*e));

        char* tab = strchr(line, '\t');
        if (!tab) {
            strncpy(e->query, line, sizeof(e->query) - 1);
            history_count++;
            continue;
        }

        *tab = '\0';
        strncpy(e->query, line, sizeof(e->query) - 1);
        e->query[sizeof(e->query) - 1] = '\0';

        char* type_str = tab + 1;
        char* path_tab = strchr(type_str, '\t');
        if (!path_tab) {
            history_count++;
            continue;
        }
        *path_tab = '\0';

        char* item_path = path_tab + 1;
        char* dir_tab = strchr(item_path, '\t');
        if (dir_tab) {
            *dir_tab = '\0';
            strncpy(e->art_dir, dir_tab + 1, sizeof(e->art_dir) - 1);
            e->art_dir[sizeof(e->art_dir) - 1] = '\0';
            trim_inplace(e->art_dir);
        }

        strncpy(e->item_path, item_path, sizeof(e->item_path) - 1);
        e->item_path[sizeof(e->item_path) - 1] = '\0';
        trim_inplace(e->item_path);
        e->art_type = parse_art_type(type_str);

        if (e->art_type != SEARCH_HISTORY_ART_NONE && !e->art_dir[0]) {
            parent_dir(e->item_path, e->art_dir, sizeof(e->art_dir));
        }

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
    return history[index].query;
}

bool SearchHistory_getEntry(int index, SearchHistoryEntry* out) {
    load_history();
    if (!out || index < 0 || index >= history_count) return false;
    *out = history[index];
    return true;
}

static void shift_history_down(int from, int to) {
    for (int i = from; i > to; i--) {
        history[i] = history[i - 1];
    }
}

void SearchHistory_add(const char* query, const SearchResultRow* top_result) {
    if (!query) return;
    load_history();

    char trimmed[256];
    strncpy(trimmed, query, sizeof(trimmed) - 1);
    trimmed[sizeof(trimmed) - 1] = '\0';
    trim_inplace(trimmed);
    if (!trimmed[0]) return;

    SearchHistoryEntry new_entry = {0};
    strncpy(new_entry.query, trimmed, sizeof(new_entry.query) - 1);
    if (top_result) {
        entry_from_result(top_result, &new_entry);
    }

    int existing = -1;
    for (int i = 0; i < history_count; i++) {
        if (strcmp(history[i].query, trimmed) == 0) {
            existing = i;
            break;
        }
    }

    if (existing == 0) {
        if (top_result) {
            entry_from_result(top_result, &history[0]);
        }
        save_history();
        return;
    }

    if (existing > 0) {
        SearchHistoryEntry temp = history[existing];
        if (top_result) {
            entry_from_result(top_result, &temp);
        }
        shift_history_down(existing, 0);
        history[0] = temp;
        save_history();
        return;
    }

    if (history_count < SEARCH_HISTORY_MAX) {
        shift_history_down(history_count, 0);
        history_count++;
    } else {
        shift_history_down(SEARCH_HISTORY_MAX - 1, 0);
    }

    history[0] = new_entry;
    save_history();
}

void SearchHistory_clear(void) {
    history_count = 0;
    history[0].query[0] = '\0';
    clear_entry_art(&history[0]);
    history_loaded = true;
    unlink(HISTORY_FILE);
}
