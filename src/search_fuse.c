#include "search_fuse.h"
#include <string.h>
#include <ctype.h>

#define FUSE_THRESHOLD 0.4f

int SearchFuse_maxEditErrors(size_t pattern_len, bool fuzzy) {
    if (!fuzzy || pattern_len == 0) return 0;
    if (pattern_len > SEARCH_FUSE_PATTERN_MAX) pattern_len = SEARCH_FUSE_PATTERN_MAX;
    int err = (int)(pattern_len * FUSE_THRESHOLD);
    if (err < 1 && pattern_len >= 4) err = 1;
    if (err > 3) err = 3;
    return err;
}

bool SearchFuse_withinEditDistance(const char* a, const char* b, int max_err) {
    if (!a || !b) return false;
    if (max_err < 0) return false;
    if (strcmp(a, b) == 0) return true;

    size_t la = strlen(a);
    size_t lb = strlen(b);
    if (la > 48 || lb > 48) return false;

    if ((la > lb ? la - lb : lb - la) > (size_t)max_err) return false;

    size_t cols = lb + 1;
    int prev[49];
    int curr[49];
    if (cols > sizeof(prev) / sizeof(prev[0])) return false;

    for (size_t j = 0; j < cols; j++) prev[j] = (int)j;

    for (size_t i = 1; i <= la; i++) {
        curr[0] = (int)i;
        int row_min = curr[0];
        for (size_t j = 1; j <= lb; j++) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            int del = prev[j] + 1;
            int ins = curr[j - 1] + 1;
            int sub = prev[j - 1] + cost;
            int v = del;
            if (ins < v) v = ins;
            if (sub < v) v = sub;
            curr[j] = v;
            if (v < row_min) row_min = v;
        }
        if (row_min > max_err) return false;
        memcpy(prev, curr, cols * sizeof(int));
    }
    return prev[lb] <= max_err;
}

static int field_length_bonus(const char* text) {
    size_t len = text ? strlen(text) : 0;
    if (len == 0) return 0;
    if (len <= 12) return 12;
    if (len <= 24) return 8;
    if (len <= 48) return 4;
    return 0;
}

static int score_from_distance(size_t pattern_len, int distance) {
    if (pattern_len == 0) return 0;
    if (distance <= 0) return 100;
    int pct = (int)(100 - ((distance * 100) / (int)pattern_len));
    if (pct < 0) pct = 0;
    return pct;
}

static int best_fuzzy_window_score(const char* text, const char* pattern, size_t plen, int max_err) {
    size_t tlen = strlen(text);
    if (tlen == 0) return 0;

    int best = 0;
    for (size_t start = 0; start < tlen; start++) {
        size_t remain = tlen - start;
        size_t win = plen + (size_t)max_err;
        if (win > remain) win = remain;
        if (win == 0) break;

        char window[SEARCH_FUSE_PATTERN_MAX + 4];
        size_t copy = win;
        if (copy >= sizeof(window)) copy = sizeof(window) - 1;
        memcpy(window, text + start, copy);
        window[copy] = '\0';

        if (SearchFuse_withinEditDistance(window, pattern, max_err)) {
            int dist = 0;
            size_t wlen = strlen(window);
            size_t cmp = plen < wlen ? plen : wlen;
            for (size_t i = 0; i < cmp; i++) {
                if (pattern[i] != window[i]) dist++;
            }
            if (plen > wlen) dist += (int)(plen - wlen);
            else if (wlen > plen) dist += (int)(wlen - plen);
            int s = score_from_distance(plen, dist);
            if (s > best) best = s;
            if (best >= 95) return best;
        }
        if (win < plen / 2) break;
    }
    return best;
}

int SearchFuse_matchScore(const char* text, const char* pattern, bool fuzzy) {
    if (!text || !pattern || !pattern[0] || !text[0]) return 0;

    char pat[SEARCH_FUSE_PATTERN_MAX + 1];
    strncpy(pat, pattern, SEARCH_FUSE_PATTERN_MAX);
    pat[SEARCH_FUSE_PATTERN_MAX] = '\0';
    size_t plen = strlen(pat);
    if (plen < 2) return 0;

    const char* hit = strstr(text, pat);
    if (hit) {
        int score = (hit == text) ? 100 : 92;
        return score + field_length_bonus(text);
    }

    if (strncmp(text, pat, plen) == 0) {
        return 96 + field_length_bonus(text);
    }

    if (!fuzzy) return 0;

    int max_err = SearchFuse_maxEditErrors(plen, true);
    return best_fuzzy_window_score(text, pat, plen, max_err);
}

static int weighted_field_score(const char* field, int weight, const char* query_norm, bool fuzzy) {
    if (!field || !field[0] || weight <= 0) return 0;
    int raw = SearchFuse_matchScore(field, query_norm, fuzzy);
    if (raw <= 0) return 0;
    int bonus = field_length_bonus(field);
    int combined = raw + bonus;
    if (combined > 100) combined = 100;
    return combined * weight;
}

static int best_token_field_score(const SearchFuseFields* fields, const char* token, bool fuzzy) {
    int best = 0;
    const char* vals[] = {fields->title, fields->artist, fields->album, fields->genre, fields->filename};
    for (int i = 0; i < 5; i++) {
        if (!vals[i] || !vals[i][0]) continue;
        int s = SearchFuse_matchScore(vals[i], token, fuzzy);
        if (s > best) best = s;
    }
    return best;
}

int SearchFuse_scoreFields(const SearchFuseFields* fields,
                           const char* query_norm,
                           const char query_tokens[][SEARCH_FUSE_TOKEN_MAX],
                           int query_token_count,
                           bool fuzzy) {
    if (!fields || !query_norm || !query_norm[0]) return 0;

    const int weights[] = {35, 35, 15, 5, 10};
    const char* vals[] = {fields->title, fields->artist, fields->album, fields->genre, fields->filename};

    int phrase_sum = 0;
    int phrase_weight = 0;
    for (int i = 0; i < 5; i++) {
        phrase_sum += weighted_field_score(vals[i], weights[i], query_norm, fuzzy);
        phrase_weight += weights[i];
    }
    int phrase_score = phrase_weight > 0 ? phrase_sum / phrase_weight : 0;

    if (query_token_count <= 0) return phrase_score;

    int token_sum = 0;
    int token_hits = 0;
    for (int t = 0; t < query_token_count; t++) {
        if (!query_tokens[t][0]) continue;
        int best = best_token_field_score(fields, query_tokens[t], fuzzy);
        if (best > 0) {
            token_sum += best;
            token_hits++;
        }
    }

    if (token_hits == 0) return phrase_score;

    int token_avg = token_sum / token_hits;
    int min_required = fuzzy ? 1 : query_token_count;
    if (token_hits < min_required) {
        if (!fuzzy) return 0;
        token_avg = (token_avg * token_hits) / query_token_count;
    }

    if (query_token_count == 1) return (phrase_score > token_avg) ? phrase_score : token_avg;
    return (phrase_score * 35 + token_avg * 65) / 100;
}

int SearchFuse_scorePlaylist(const char* name_norm, const char* parent_norm,
                             const char* query_norm,
                             const char query_tokens[][SEARCH_FUSE_TOKEN_MAX],
                             int query_token_count,
                             bool fuzzy) {
    SearchFuseFields fields = {
        .title = name_norm,
        .artist = parent_norm,
        .album = NULL,
        .genre = NULL,
        .filename = NULL
    };
    return SearchFuse_scoreFields(&fields, query_norm, query_tokens, query_token_count, fuzzy);
}
