#ifndef __SEARCH_HISTORY_H__
#define __SEARCH_HISTORY_H__

#include <stdbool.h>
#include "library_index.h"

#define SEARCH_HISTORY_MAX 10

typedef enum {
    SEARCH_HISTORY_ART_NONE = 0,
    SEARCH_HISTORY_ART_TRACK,
    SEARCH_HISTORY_ART_PLAYLIST
} SearchHistoryArtType;

typedef struct {
    char query[256];
    char item_path[512];
    char art_dir[512];
    SearchHistoryArtType art_type;
} SearchHistoryEntry;

void SearchHistory_init(void);
int SearchHistory_count(void);
const char* SearchHistory_get(int index);
bool SearchHistory_getEntry(int index, SearchHistoryEntry* out);

// top_result may be NULL (no art stored). Adds/moves query to front of history.
void SearchHistory_add(const char* query, const SearchResultRow* top_result);
void SearchHistory_clear(void);

#endif
