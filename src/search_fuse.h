#ifndef __SEARCH_FUSE_H__
#define __SEARCH_FUSE_H__

#include <stdbool.h>
#include <stddef.h>

#define SEARCH_FUSE_PATTERN_MAX 32
#define SEARCH_FUSE_TOKEN_MAX   48
#define SEARCH_FUSE_MIN_SCORE   45

typedef struct {
    const char* title;
    const char* artist;
    const char* album;
    const char* genre;
    const char* filename;
} SearchFuseFields;

int SearchFuse_maxEditErrors(size_t pattern_len, bool fuzzy);

bool SearchFuse_withinEditDistance(const char* a, const char* b, int max_err);

int SearchFuse_matchScore(const char* text, const char* pattern, bool fuzzy);

int SearchFuse_scoreFields(const SearchFuseFields* fields,
                           const char* query_norm,
                           const char query_tokens[][SEARCH_FUSE_TOKEN_MAX],
                           int query_token_count,
                           bool fuzzy);

int SearchFuse_scorePlaylist(const char* name_norm, const char* parent_norm,
                             const char* query_norm,
                             const char query_tokens[][SEARCH_FUSE_TOKEN_MAX],
                             int query_token_count,
                             bool fuzzy);

#endif
