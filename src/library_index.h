#ifndef __LIBRARY_INDEX_H__
#define __LIBRARY_INDEX_H__

#include <stdbool.h>
#include "player.h"

#define LIBRARY_SEARCH_MAX_TOP 5
#define LIBRARY_SEARCH_BASE_MIXED 25
#define LIBRARY_SEARCH_MAX_MIXED 30

typedef enum {
    SEARCH_ITEM_NESTED_PLAYLIST,
    SEARCH_ITEM_TRACK,
    SEARCH_ITEM_USER_PLAYLIST
} SearchResultType;

typedef struct {
    SearchResultType type;
    char label[256];
    char path[512];
    char subtitle[256];
    int track_count;
    AudioFormat format;
    int score;
} SearchResultRow;

typedef struct {
    int nested_count;
    int mixed_count;
    int mixed_limit;
    SearchResultRow nested[LIBRARY_SEARCH_MAX_TOP];
    SearchResultRow mixed[LIBRARY_SEARCH_MAX_MIXED];
} SearchResults;

void LibraryIndex_init(void);
void LibraryIndex_quit(void);

bool LibraryIndex_isReady(void);
bool LibraryIndex_isBuilding(void);
const char* LibraryIndex_getBuildStatus(void);

int LibraryIndex_getBuildLogCount(void);
const char* LibraryIndex_getBuildLogLine(int index);

bool LibraryIndex_search(const char* query, SearchResults* out);

// Force a full index rebuild in the background. Returns false if already building.
bool LibraryIndex_requestRebuild(void);

#endif
