#define _GNU_SOURCE
#include "library_index.h"
#include "metadata_reader.h"
#include "search_fuse.h"
#include "playlist_m3u.h"
#include "playlist.h"
#include "player.h"
#include "settings.h"
#include "browser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdint.h>
#include <stdarg.h>

#include "defines.h"
#include "api.h"
#include "include/parson/parson.h"

#define LIBRARY_INDEX_FORMAT_VERSION 5
#define LIBRARY_INDEX_FORMAT_VERSION_LEGACY 4
#define MUSIC_PATH SDCARD_PATH "/Music"
#define INDEX_BIN_PATH SHARED_USERDATA_PATH "/music-player/library_index.bin"
#define INDEX_BIN_TMP_PATH SHARED_USERDATA_PATH "/music-player/library_index.bin.tmp"
#define INDEX_JSON_PATH SHARED_USERDATA_PATH "/music-player/library_index.json"
#define INDEX_LOG_PATH SHARED_USERDATA_PATH "/music-player/index.log"
#define LIBRARY_MAX_TRACKS 32768
#define LIBRARY_MAX_PLAYLISTS_INDEX 10000
#define LIBRARY_MAX_TOKENS 65536
#define TOKEN_GROW 16
#define TOKEN_INIT_CAP 256
#define TOKEN_HASH_BUCKETS 8192
#define FUZZY_MIN_TOKEN_LEN 3
#define FUZZY_RESULT_THRESHOLD 3
#define INDEX_BIN_MAGIC 0x49504D4E
#define FP_MAX_DEPTH 16
#define INDEX_FAIL_PATH SHARED_USERDATA_PATH "/music-player/index_failures"
#define INDEX_MEM_BUDGET_DEFAULT (10 * 1024 * 1024)
#define TOKEN_MAX_POSTINGS 4096
#define INDEX_BUILD_MAX_FAILURES 3
#define INDEX_BUILD_STACK_SIZE (512 * 1024)
#define BUILD_LOG_MAX_LINES 512
#define BUILD_LOG_LINE_LEN 160

typedef struct {
    char title_norm[256];
    char artist_norm[256];
    char album_norm[256];
    char genre_norm[256];
    char filename_norm[256];
} TrackNormScratch;

typedef struct {
    char path[512];
    char title[256];
    char artist[256];
    char album[256];
    char genre[256];
    AudioFormat format;
    uint64_t file_mtime;
    uint64_t file_size;
} IndexedTrack;

typedef struct {
    char path[512];
    char name[128];
    char name_norm[128];
    char parent_folder[64];
    char parent_norm[64];
    int track_count;
    bool is_root;
} IndexedPlaylist;

typedef struct {
    char token[48];
    uint16_t* track_ids;
    int track_count;
    int track_cap;
    uint16_t* playlist_ids;
    int playlist_count;
    int playlist_cap;
    bool posting_cap_logged;
} IndexToken;

typedef struct {
    uint64_t hash;
} LibraryFingerprint;

typedef struct {
    char path[512];
    uint64_t mtime;
    uint64_t size;
    bool is_playlist;
} FpEntry;

typedef struct TokenHashNode {
    int token_idx;
    struct TokenHashNode* hash_next;
    struct TokenHashNode* char_next;
} TokenHashNode;

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t fingerprint;
    uint32_t track_count;
    uint32_t playlist_count;
} IndexBinHeader;

typedef struct {
    char path[512];
    char title[256];
    char artist[256];
    char album[256];
    char genre[256];
    char filename_norm[256];
    uint32_t format;
    uint64_t mtime;
    uint64_t size;
} IndexBinTrack;

typedef struct {
    char path[512];
    char name[128];
    char name_norm[128];
    char parent_folder[64];
    char parent_norm[64];
    int32_t track_count;
    uint8_t is_root;
    uint8_t pad[3];
} IndexBinPlaylist;
#pragma pack(pop)

static pthread_mutex_t index_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool build_started = false;
static bool build_running = false;
static bool index_ready = false;
static IndexBuildState build_state = INDEX_STATE_IDLE;
static int build_failure_count = 0;
static bool token_index_degraded = false;
static bool skip_token_index = false;
static char last_failure_reason[128] = "";
static char last_ok_path[512] = "";
static int last_ok_idx = -1;
static char last_search_error[64] = "";

static IndexedTrack* tracks = NULL;
static int track_count = 0;
static IndexedPlaylist* playlists = NULL;
static int playlist_count = 0;
static IndexToken* tokens = NULL;
static int token_count = 0;
static int token_cap = 0;

static TokenHashNode* token_hash[TOKEN_HASH_BUCKETS] = {0};
static TokenHashNode* token_first_char[37] = {0};
static TokenHashNode* token_hash_nodes = NULL;
static int token_hash_node_count = 0;
static int token_hash_node_cap = 0;

static uint64_t saved_fingerprint = 0;

static char build_status[128] = "Starting...";
static char build_log_lines[BUILD_LOG_MAX_LINES][BUILD_LOG_LINE_LEN];
static int build_log_count = 0;
static int build_log_start = 0;

static void build_log_clear(void) {
    pthread_mutex_lock(&index_mutex);
    build_log_count = 0;
    build_log_start = 0;
    pthread_mutex_unlock(&index_mutex);
}

static void build_log_append(const char* msg) {
    if (!msg) return;

    pthread_mutex_lock(&index_mutex);
    int slot;
    if (build_log_count < BUILD_LOG_MAX_LINES) {
        slot = build_log_count++;
    } else {
        slot = build_log_start;
        build_log_start = (build_log_start + 1) % BUILD_LOG_MAX_LINES;
    }
    snprintf(build_log_lines[slot], BUILD_LOG_LINE_LEN, "%s", msg);
    pthread_mutex_unlock(&index_mutex);
}

static int build_log_slot(int index) {
    if (index < 0 || index >= build_log_count) return -1;
    if (build_log_count < BUILD_LOG_MAX_LINES) return index;
    return (build_log_start + index) % BUILD_LOG_MAX_LINES;
}

#define INDEX_VERBOSE_TRACKS 32
#define INDEX_VERBOSE_EVERY 512

static bool index_verbose_step(int next_track_idx) {
    return next_track_idx < INDEX_VERBOSE_TRACKS ||
           (next_track_idx & (INDEX_VERBOSE_EVERY - 1)) == 0;
}

static void index_log(const char* msg);

static void index_logf(const char* fmt, ...) {
    if (!fmt) return;
    char buf[BUILD_LOG_LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    index_log(buf);
}

static void index_log_path(const char* prefix, int idx, const char* path) {
    if (!path) return;
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char short_path[96];
    strncpy(short_path, base, sizeof(short_path) - 1);
    short_path[sizeof(short_path) - 1] = '\0';
    index_logf("%s [%d] %s", prefix ? prefix : "path", idx, short_path);
}

static void index_log_stats(const char* phase, int track_idx) {
    index_logf("rebuild_index: %s idx=%d tracks=%d tokens=%d tokcap=%d nodes=%d nodecap=%d",
                 phase ? phase : "?", track_idx, track_count, token_count,
                 token_cap, token_hash_node_count, token_hash_node_cap);
}

static void set_status(const char* msg) {
    pthread_mutex_lock(&index_mutex);
    snprintf(build_status, sizeof(build_status), "%s", msg ? msg : "");
    pthread_mutex_unlock(&index_mutex);
}

static void index_log(const char* msg) {
    if (!msg) return;
    build_log_append(msg);
    if (!Settings_getIndexLog()) return;
    mkdir(SHARED_USERDATA_PATH "/music-player", 0755);
    FILE* f = fopen(INDEX_LOG_PATH, "a");
    if (!f) return;
    fprintf(f, "%s\n", msg);
    fflush(f);
    fclose(f);
}

static void set_index_ready(bool ready) {
    pthread_mutex_lock(&index_mutex);
    index_ready = ready;
    if (ready) {
        build_state = INDEX_STATE_READY;
    }
    pthread_mutex_unlock(&index_mutex);
}

static void set_build_state(IndexBuildState state) {
    pthread_mutex_lock(&index_mutex);
    build_state = state;
    index_ready = (state == INDEX_STATE_READY);
    pthread_mutex_unlock(&index_mutex);
}

static size_t index_memory_budget_bytes(void) {
    int mb = Settings_getIndexMemMb();
    if (mb <= 0) return INDEX_MEM_BUDGET_DEFAULT;
    return (size_t)mb * 1024U * 1024U;
}

static size_t index_estimated_bytes(void) {
    size_t bytes = 0;
    if (tracks && track_count > 0) {
        bytes += (size_t)track_count * sizeof(IndexedTrack);
    }
    if (tokens && token_cap > 0) {
        bytes += (size_t)token_cap * sizeof(IndexToken);
    }
    if (token_hash_nodes && token_hash_node_cap > 0) {
        bytes += (size_t)token_hash_node_cap * sizeof(TokenHashNode);
    }
    for (int i = 0; i < token_count; i++) {
        bytes += (size_t)tokens[i].track_cap * sizeof(uint16_t);
        bytes += (size_t)tokens[i].playlist_cap * sizeof(uint16_t);
    }
    if (playlists && playlist_count > 0) {
        bytes += (size_t)playlist_count * sizeof(IndexedPlaylist);
    }
    return bytes;
}

static bool index_memory_ok(size_t extra) {
    size_t budget = index_memory_budget_bytes();
    size_t est = index_estimated_bytes() + extra;
    return est <= budget;
}

static void index_log_mem_milestone(int file_idx, int total_files) {
    if ((file_idx & (INDEX_MEM_LOG_EVERY - 1)) != 0 && file_idx != 0) return;
    index_logf("INFO mem est=%zuKB tracks=%d tokens=%d budget=%zuKB file=%d/%d",
               index_estimated_bytes() / 1024U, track_count, token_count,
               index_memory_budget_bytes() / 1024U, file_idx, total_files);
}

static bool is_stopword(const char* token) {
    static const char* const words[] = {
        "a", "an", "the", "of", "ft", "feat", "featuring",
        "official", "video", "audio", "lyrics", "lyric",
        "music", "mv", "hd", "remix", "live"
    };
    if (!token || !token[0]) return true;
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        if (strcmp(token, words[i]) == 0) return true;
    }
    return false;
}

static void fill_track_norms(const IndexedTrack* tr, TrackNormScratch* norms) {
    if (!tr || !norms) return;
    memset(norms, 0, sizeof(*norms));
    Metadata_copyNormalized(norms->title_norm, sizeof(norms->title_norm), tr->title);
    Metadata_copyNormalized(norms->artist_norm, sizeof(norms->artist_norm), tr->artist);
    Metadata_copyNormalized(norms->album_norm, sizeof(norms->album_norm), tr->album);
    Metadata_copyNormalized(norms->genre_norm, sizeof(norms->genre_norm), tr->genre);
    const char* base = strrchr(tr->path, '/');
    base = base ? base + 1 : tr->path;
    Metadata_copyNormalized(norms->filename_norm, sizeof(norms->filename_norm), base);
}

static void track_path_basename(const IndexedTrack* tr, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!tr) return;
    const char* base = strrchr(tr->path, '/');
    base = base ? base + 1 : tr->path;
    strncpy(out, base, out_size - 1);
    out[out_size - 1] = '\0';
}

static void read_failure_count_file(int* out_count) {
    if (out_count) *out_count = 0;
    FILE* f = fopen(INDEX_FAIL_PATH, "r");
    if (!f) return;
    int count = 0;
    if (fscanf(f, "%d", &count) == 1 && out_count) {
        *out_count = count;
    }
    fclose(f);
}

static void write_failure_count_file(int count) {
    mkdir(SHARED_USERDATA_PATH "/music-player", 0755);
    FILE* f = fopen(INDEX_FAIL_PATH, "w");
    if (!f) return;
    fprintf(f, "%d\n", count);
    fclose(f);
}

static void clear_failure_count_file(void) {
    unlink(INDEX_FAIL_PATH);
}

static uint64_t fnv1a_update(uint64_t h, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static uint32_t hash_token_string(const char* s) {
    return (uint32_t)fnv1a_update(14695981039346656037ULL, s, strlen(s));
}

static int fp_entry_cmp(const void* a, const void* b) {
    return strcmp(((const FpEntry*)a)->path, ((const FpEntry*)b)->path);
}

static bool skip_dirent_name(const char* name, bool include_hidden) {
    if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
        return true;
    }
    return !include_hidden && name[0] == '.';
}

static void fp_append_entry(FpEntry** entries, int* count, int* cap,
                            const char* path, uint64_t mtime, uint64_t size, bool is_playlist) {
    if (!path || !path[0] || !entries || !count || !cap) return;

    if (*count >= *cap) {
        int nc = *cap ? *cap * 2 : 4096;
        FpEntry* ne = realloc(*entries, sizeof(FpEntry) * nc);
        if (!ne) return;
        *entries = ne;
        *cap = nc;
    }

    FpEntry* e = &(*entries)[*count];
    strncpy(e->path, path, sizeof(e->path) - 1);
    e->mtime = mtime;
    e->size = size;
    e->is_playlist = is_playlist;
    (*count)++;
}

static void fp_collect_dir(const char* dir, FpEntry** entries, int* count, int* cap,
                           bool playlists_only, int depth) {
    if (depth > FP_MAX_DEPTH) return;

    DIR* d = opendir(dir);
    if (!d) return;

    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (skip_dirent_name(ent->d_name, true)) continue;

        char full[768];
        snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);

        struct stat st;
        if (lstat(full, &st) != 0) continue;
        if (S_ISLNK(st.st_mode)) continue;

        if (S_ISDIR(st.st_mode)) {
            fp_collect_dir(full, entries, count, cap, playlists_only, depth + 1);
            continue;
        }

        if (Playlist_isIndexSidecar(ent->d_name)) continue;

        bool include = false;
        if (playlists_only) {
            size_t len = strlen(ent->d_name);
            include = (len > 4 && strcasecmp(ent->d_name + len - 4, ".m3u") == 0);
        } else {
            include = Browser_isAudioFile(ent->d_name);
        }
        if (!include) continue;

        fp_append_entry(entries, count, cap, full, (uint64_t)st.st_mtime, (uint64_t)st.st_size, playlists_only);
    }
    closedir(d);
}

static void compute_fingerprint(LibraryFingerprint* fp) {
    FpEntry* entries = NULL;
    int count = 0;
    int cap = 0;

    set_status("Scanning library...");
    index_log("compute_fingerprint: collecting audio paths");

    char** paths = NULL;
    int path_count = Playlist_collectPathsEx(MUSIC_PATH, &paths, LIBRARY_MAX_TRACKS, true);
    if (path_count > 0 && paths) {
        for (int i = 0; i < path_count; i++) {
            struct stat st;
            if (stat(paths[i], &st) != 0) continue;
            fp_append_entry(&entries, &count, &cap, paths[i],
                            (uint64_t)st.st_mtime, (uint64_t)st.st_size, false);
        }
        Playlist_freePaths(paths, path_count);
    }

    fp_collect_dir(PLAYLISTS_DIR, &entries, &count, &cap, true, 0);

    if (count > 1) {
        qsort(entries, (size_t)count, sizeof(FpEntry), fp_entry_cmp);
    }

    uint64_t h = 14695981039346656037ULL;
    for (int i = 0; i < count; i++) {
        h = fnv1a_update(h, entries[i].path, strlen(entries[i].path));
        h = fnv1a_update(h, &entries[i].mtime, sizeof(entries[i].mtime));
        h = fnv1a_update(h, &entries[i].size, sizeof(entries[i].size));
    }
    free(entries);

    fp->hash = h;
}

static void fingerprint_to_string(const LibraryFingerprint* fp, char* out, int out_size) {
    snprintf(out, out_size, "v%d_%016llx",
             LIBRARY_INDEX_FORMAT_VERSION,
             (unsigned long long)fp->hash);
}

static void token_hash_clear(void) {
    memset(token_hash, 0, sizeof(token_hash));
    memset(token_first_char, 0, sizeof(token_first_char));
    free(token_hash_nodes);
    token_hash_nodes = NULL;
    token_hash_node_count = 0;
    token_hash_node_cap = 0;
}

static int token_first_char_bucket(const char* token) {
    if (!token || !token[0]) return 36;
    unsigned char c = (unsigned char)tolower((unsigned char)token[0]);
    if (c >= 'a' && c <= 'z') return c - 'a';
    if (c >= '0' && c <= '9') return 26 + (c - '0');
    return 36;
}

static bool token_node_link(int token_idx) {
    if (token_idx < 0 || token_idx >= token_count) return false;

    if (token_hash_node_count >= token_hash_node_cap) {
        int nc = token_hash_node_cap ? token_hash_node_cap * 2 : TOKEN_INIT_CAP;
        size_t extra = sizeof(TokenHashNode) * (size_t)(nc - token_hash_node_cap);
        if (!index_memory_ok(extra)) {
            token_index_degraded = true;
            skip_token_index = true;
            index_log("WARN MEM budget: skip token nodes");
            return false;
        }
        TokenHashNode* nn = realloc(token_hash_nodes, sizeof(TokenHashNode) * nc);
        if (!nn) {
            index_logf("token_index: node pool realloc failed need=%d cap=%d",
                       token_hash_node_count + 1, token_hash_node_cap);
            return false;
        }
        token_hash_nodes = nn;
        token_hash_node_cap = nc;
        index_logf("token_index: node pool grown cap=%d", token_hash_node_cap);
    }

    TokenHashNode* node = &token_hash_nodes[token_hash_node_count++];
    node->token_idx = token_idx;
    node->hash_next = NULL;
    node->char_next = NULL;

    uint32_t bucket = hash_token_string(tokens[token_idx].token) % TOKEN_HASH_BUCKETS;
    node->hash_next = token_hash[bucket];
    token_hash[bucket] = node;

    int fc = token_first_char_bucket(tokens[token_idx].token);
    node->char_next = token_first_char[fc];
    token_first_char[fc] = node;
    return true;
}

static void free_index_data(void) {
    for (int i = 0; i < token_count; i++) {
        free(tokens[i].track_ids);
        free(tokens[i].playlist_ids);
    }
    free(tokens);
    tokens = NULL;
    token_count = 0;
    token_cap = 0;

    free(tracks);
    tracks = NULL;
    track_count = 0;

    free(playlists);
    playlists = NULL;
    playlist_count = 0;

    token_hash_clear();
}

static int find_token(const char* token) {
    if (!token || !token[0]) return -1;

    uint32_t bucket = hash_token_string(token) % TOKEN_HASH_BUCKETS;
    for (TokenHashNode* n = token_hash[bucket]; n; n = n->hash_next) {
        if (strcmp(tokens[n->token_idx].token, token) == 0) {
            return n->token_idx;
        }
    }
    return -1;
}

static int ensure_token(const char* token) {
    if (!token || !token[0]) return -1;

    int idx = find_token(token);
    if (idx >= 0) return idx;

    if (token_count >= LIBRARY_MAX_TOKENS) return -1;

    if (token_count >= token_cap) {
        int nc = token_cap ? token_cap * 2 : TOKEN_INIT_CAP;
        size_t extra = sizeof(IndexToken) * (size_t)(nc - token_cap);
        if (!index_memory_ok(extra)) {
            token_index_degraded = true;
            skip_token_index = true;
            index_log("WARN MEM budget: skip new tokens");
            return -1;
        }
        IndexToken* nt = realloc(tokens, sizeof(IndexToken) * nc);
        if (!nt) {
            index_log("token_index: token table realloc failed");
            return -1;
        }
        memset(nt + token_cap, 0, sizeof(IndexToken) * (nc - token_cap));
        tokens = nt;
        token_cap = nc;
    }

    idx = token_count++;
    memset(&tokens[idx], 0, sizeof(tokens[idx]));
    strncpy(tokens[idx].token, token, sizeof(tokens[idx].token) - 1);
    if (!token_node_link(idx)) {
        token_count--;
        return -1;
    }
    return idx;
}

static void token_add_track(int token_idx, int track_id) {
    if (token_idx < 0 || track_id < 0 || track_id > 65535) return;
    if (skip_token_index) return;
    IndexToken* t = &tokens[token_idx];

    if (t->track_count >= TOKEN_MAX_POSTINGS) {
        if (!t->posting_cap_logged) {
            index_logf("WARN token cap tok=%.32s", t->token);
            t->posting_cap_logged = true;
        }
        return;
    }

    for (int i = 0; i < t->track_count; i++) {
        if (t->track_ids[i] == (uint16_t)track_id) return;
    }

    if (t->track_count >= t->track_cap) {
        int nc = t->track_cap ? t->track_cap * 2 : TOKEN_GROW;
        size_t extra = sizeof(uint16_t) * (size_t)(nc - t->track_cap);
        if (!index_memory_ok(extra)) {
            token_index_degraded = true;
            index_logf("WARN MEM budget: skip posting tok=%d track=%d", token_idx, track_id);
            return;
        }
        uint16_t* nd = realloc(t->track_ids, sizeof(uint16_t) * (size_t)nc);
        if (!nd) {
            index_logf("ERR token_index: track_ids realloc failed tok=%d track=%d",
                       token_idx, track_id);
            return;
        }
        t->track_ids = nd;
        t->track_cap = nc;
    }
    t->track_ids[t->track_count++] = (uint16_t)track_id;
}

static void token_add_playlist(int token_idx, int playlist_id) {
    if (token_idx < 0 || playlist_id < 0 || playlist_id > 65535) return;
    if (skip_token_index) return;
    IndexToken* t = &tokens[token_idx];

    for (int i = 0; i < t->playlist_count; i++) {
        if (t->playlist_ids[i] == (uint16_t)playlist_id) return;
    }

    if (t->playlist_count >= t->playlist_cap) {
        int nc = t->playlist_cap ? t->playlist_cap * 2 : TOKEN_GROW;
        size_t extra = sizeof(uint16_t) * (size_t)(nc - t->playlist_cap);
        if (!index_memory_ok(extra)) {
            index_logf("WARN MEM budget: skip playlist posting tok=%d pl=%d",
                         token_idx, playlist_id);
            return;
        }
        uint16_t* nd = realloc(t->playlist_ids, sizeof(uint16_t) * (size_t)nc);
        if (!nd) {
            index_logf("ERR token_index: playlist_ids realloc failed tok=%d pl=%d",
                       token_idx, playlist_id);
            return;
        }
        t->playlist_ids = nd;
        t->playlist_cap = nc;
    }
    t->playlist_ids[t->playlist_count++] = (uint16_t)playlist_id;
}

typedef struct {
    int id;
    bool is_track;
} TokenAddCtx;

static void add_token_cb(const char* token, void* userdata) {
    if (!token || !token[0] || is_stopword(token)) return;
    TokenAddCtx* ctx = (TokenAddCtx*)userdata;
    int ti = ensure_token(token);
    if (ti < 0) {
        token_index_degraded = true;
        return;
    }
    if (ctx->is_track) token_add_track(ti, ctx->id);
    else token_add_playlist(ti, ctx->id);
}

static void index_phrase_token(const char* norm_text, int track_id) {
    if (!norm_text || norm_text[0] == '\0') return;
    size_t len = strlen(norm_text);
    if (len < 2 || len >= sizeof(tokens[0].token)) return;

    int ti = ensure_token(norm_text);
    if (ti >= 0) token_add_track(ti, track_id);
}

static void index_track_tokens_verbose(int id) {
    if (id < 0 || id >= track_count) return;
    IndexedTrack* tr = &tracks[id];
    TrackNormScratch norms;
    fill_track_norms(tr, &norms);

    TokenAddCtx ctx = {.id = id, .is_track = true};
    int tok_before = token_count;

    index_logf("DBG tokenize [%d] title_norm=%.32s", id,
                 norms.title_norm[0] ? norms.title_norm : "(empty)");
    Metadata_foreachNormalizedToken(norms.title_norm, add_token_cb, &ctx);
    index_log_stats("after title tokens", id);

    Metadata_foreachNormalizedToken(norms.artist_norm, add_token_cb, &ctx);
    index_log_stats("after artist tokens", id);

    Metadata_foreachNormalizedToken(norms.album_norm, add_token_cb, &ctx);
    index_log_stats("after album tokens", id);

    Metadata_foreachNormalizedToken(norms.genre_norm, add_token_cb, &ctx);
    index_log_stats("after genre tokens", id);

    Metadata_foreachNormalizedToken(norms.filename_norm, add_token_cb, &ctx);
    index_log_stats("after filename tokens", id);

    index_phrase_token(norms.title_norm, id);
    index_log_stats("after phrase token", id);

    index_logf("DBG tokenize [%d] done +%d tokens", id, token_count - tok_before);
}

static void index_track_tokens(int id) {
    if (id < 0 || id >= track_count) return;
    if (skip_token_index) return;
    if (index_verbose_step(id)) {
        index_track_tokens_verbose(id);
        return;
    }

    IndexedTrack* tr = &tracks[id];
    TrackNormScratch norms;
    fill_track_norms(tr, &norms);

    TokenAddCtx ctx = {.id = id, .is_track = true};

    Metadata_foreachNormalizedToken(norms.title_norm, add_token_cb, &ctx);
    Metadata_foreachNormalizedToken(norms.artist_norm, add_token_cb, &ctx);
    Metadata_foreachNormalizedToken(norms.album_norm, add_token_cb, &ctx);
    Metadata_foreachNormalizedToken(norms.genre_norm, add_token_cb, &ctx);
    Metadata_foreachNormalizedToken(norms.filename_norm, add_token_cb, &ctx);

    index_phrase_token(norms.title_norm, id);
}

static void index_playlist_tokens(int id);

typedef struct {
    char path[512];
    uint64_t file_mtime;
    uint64_t file_size;
    long bin_offset;
    bool has_inline_metadata;
    char title[256];
    char artist[256];
    char album[256];
    char genre[256];
    char filename_norm[256];
    AudioFormat format;
} TrackCacheEntry;

static int track_cache_cmp(const void* a, const void* b) {
    return strcmp(((const TrackCacheEntry*)a)->path,
                  ((const TrackCacheEntry*)b)->path);
}

static void copy_track_fields(IndexedTrack* dst, const IndexedTrack* src) {
    if (!dst || !src) return;
    *dst = *src;
}

static void copy_track_from_bin(const IndexBinTrack* src, IndexedTrack* dst) {
    if (!src || !dst) return;
    memset(dst, 0, sizeof(*dst));
    strncpy(dst->path, src->path, sizeof(dst->path) - 1);
    strncpy(dst->title, src->title, sizeof(dst->title) - 1);
    strncpy(dst->artist, src->artist, sizeof(dst->artist) - 1);
    strncpy(dst->album, src->album, sizeof(dst->album) - 1);
    strncpy(dst->genre, src->genre, sizeof(dst->genre) - 1);
    dst->format = (AudioFormat)src->format;
    dst->file_mtime = src->mtime;
    dst->file_size = src->size;
    (void)src->filename_norm;
}

static void copy_playlist_from_bin(const IndexBinPlaylist* src, IndexedPlaylist* dst) {
    if (!src || !dst) return;
    strncpy(dst->path, src->path, sizeof(dst->path) - 1);
    strncpy(dst->name, src->name, sizeof(dst->name) - 1);
    strncpy(dst->name_norm, src->name_norm, sizeof(dst->name_norm) - 1);
    strncpy(dst->parent_folder, src->parent_folder, sizeof(dst->parent_folder) - 1);
    strncpy(dst->parent_norm, src->parent_norm, sizeof(dst->parent_norm) - 1);
    dst->track_count = src->track_count;
    dst->is_root = src->is_root != 0;
}

static uint64_t parse_fingerprint_string(const char* s) {
    if (!s || !s[0]) return 0;
    int version = 0;
    unsigned long long value = 0;
    if (sscanf(s, "v%d_%llx", &version, &value) == 2 &&
        (version == LIBRARY_INDEX_FORMAT_VERSION ||
         version == LIBRARY_INDEX_FORMAT_VERSION_LEGACY)) {
        return (uint64_t)value;
    }
    return 0;
}

static bool bin_version_supported(uint32_t version) {
    return version == LIBRARY_INDEX_FORMAT_VERSION ||
           version == LIBRARY_INDEX_FORMAT_VERSION_LEGACY;
}

static const TrackCacheEntry* track_cache_find(TrackCacheEntry* cache, int count, const char* path) {
    if (!cache || count <= 0 || !path) return NULL;

    TrackCacheEntry key = {{0}};
    strncpy(key.path, path, sizeof(key.path) - 1);
    return bsearch(&key, cache, (size_t)count, sizeof(TrackCacheEntry), track_cache_cmp);
}

static bool track_cache_load_from_bin(long offset, IndexedTrack* tr) {
    if (!tr || offset < 0) return false;

    FILE* f = fopen(INDEX_BIN_PATH, "rb");
    if (!f) return false;

    if (fseek(f, offset, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }

    IndexBinTrack bt;
    bool ok = fread(&bt, sizeof(bt), 1, f) == 1;
    fclose(f);
    if (!ok) return false;

    copy_track_from_bin(&bt, tr);
    return true;
}

static bool track_cache_apply(const TrackCacheEntry* cached, IndexedTrack* tr) {
    if (!cached || !tr) return false;

    if (cached->has_inline_metadata) {
        strncpy(tr->path, cached->path, sizeof(tr->path) - 1);
        strncpy(tr->title, cached->title, sizeof(tr->title) - 1);
        strncpy(tr->artist, cached->artist, sizeof(tr->artist) - 1);
        strncpy(tr->album, cached->album, sizeof(tr->album) - 1);
        strncpy(tr->genre, cached->genre, sizeof(tr->genre) - 1);
        tr->format = cached->format;
        tr->file_mtime = cached->file_mtime;
        tr->file_size = cached->file_size;
        return true;
    }

    if (cached->bin_offset >= 0) {
        return track_cache_load_from_bin(cached->bin_offset, tr);
    }

    return false;
}

static void load_track_from_json_object(JSON_Object* to, TrackCacheEntry* entry) {
    if (!to || !entry) return;
    memset(entry, 0, sizeof(*entry));

    const char* path = json_object_get_string(to, "path");
    if (!path || !path[0]) return;
    strncpy(entry->path, path, sizeof(entry->path) - 1);

    const char* title = json_object_get_string(to, "title");
    if (title) strncpy(entry->title, title, sizeof(entry->title) - 1);
    const char* artist = json_object_get_string(to, "artist");
    if (artist) strncpy(entry->artist, artist, sizeof(entry->artist) - 1);
    const char* album = json_object_get_string(to, "album");
    if (album) strncpy(entry->album, album, sizeof(entry->album) - 1);
    const char* genre = json_object_get_string(to, "genre");
    if (genre) strncpy(entry->genre, genre, sizeof(entry->genre) - 1);
    const char* fn = json_object_get_string(to, "filename_norm");
    if (fn) strncpy(entry->filename_norm, fn, sizeof(entry->filename_norm) - 1);
    entry->format = (AudioFormat)json_object_get_number(to, "format");
    entry->file_mtime = (uint64_t)json_object_get_number(to, "mtime");
    entry->file_size = (uint64_t)json_object_get_number(to, "size");
    entry->has_inline_metadata = true;
    entry->bin_offset = -1;
}

static void load_indexed_track_from_json(JSON_Object* to, IndexedTrack* tr) {
    if (!to || !tr) return;
    memset(tr, 0, sizeof(*tr));

    const char* path = json_object_get_string(to, "path");
    if (path) strncpy(tr->path, path, sizeof(tr->path) - 1);
    const char* title = json_object_get_string(to, "title");
    if (title) strncpy(tr->title, title, sizeof(tr->title) - 1);
    const char* artist = json_object_get_string(to, "artist");
    if (artist) strncpy(tr->artist, artist, sizeof(tr->artist) - 1);
    const char* album = json_object_get_string(to, "album");
    if (album) strncpy(tr->album, album, sizeof(tr->album) - 1);
    const char* genre = json_object_get_string(to, "genre");
    if (genre) strncpy(tr->genre, genre, sizeof(tr->genre) - 1);
    tr->format = (AudioFormat)json_object_get_number(to, "format");
    tr->file_mtime = (uint64_t)json_object_get_number(to, "mtime");
    tr->file_size = (uint64_t)json_object_get_number(to, "size");
}

static TrackCacheEntry* load_track_cache_from_bin(int* out_count) {
    if (out_count) *out_count = 0;

    FILE* f = fopen(INDEX_BIN_PATH, "rb");
    if (!f) return NULL;

    IndexBinHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 ||
        hdr.magic != INDEX_BIN_MAGIC ||
        !bin_version_supported(hdr.version)) {
        fclose(f);
        return NULL;
    }

    if (hdr.track_count == 0 || hdr.track_count > LIBRARY_MAX_TRACKS) {
        fclose(f);
        return NULL;
    }

    TrackCacheEntry* cache = calloc(hdr.track_count, sizeof(TrackCacheEntry));
    if (!cache) {
        fclose(f);
        return NULL;
    }

    int count = 0;
    for (uint32_t i = 0; i < hdr.track_count; i++) {
        long offset = ftell(f);
        IndexBinTrack bt;
        if (fread(&bt, sizeof(bt), 1, f) != 1) break;
        if (!bt.path[0]) continue;

        TrackCacheEntry* entry = &cache[count];
        memset(entry, 0, sizeof(*entry));
        strncpy(entry->path, bt.path, sizeof(entry->path) - 1);
        entry->file_mtime = bt.mtime;
        entry->file_size = bt.size;
        entry->bin_offset = offset;
        entry->has_inline_metadata = false;
        count++;
    }

    fseek(f, (long)(sizeof(IndexBinPlaylist) * hdr.playlist_count), SEEK_CUR);
    fclose(f);

    if (count <= 0) {
        free(cache);
        return NULL;
    }

    if (count > 1) {
        qsort(cache, (size_t)count, sizeof(TrackCacheEntry), track_cache_cmp);
    }

    if (out_count) *out_count = count;
    return cache;
}

static TrackCacheEntry* load_track_cache_from_json(int* out_count) {
    if (out_count) *out_count = 0;

    JSON_Value* root = json_parse_file(INDEX_JSON_PATH);
    if (!root) return NULL;

    JSON_Object* obj = json_value_get_object(root);
    JSON_Array* track_arr = obj ? json_object_get_array(obj, "tracks") : NULL;
    if (!track_arr) {
        json_value_free(root);
        return NULL;
    }

    size_t n = json_array_get_count(track_arr);
    if (n == 0) {
        json_value_free(root);
        return NULL;
    }

    TrackCacheEntry* cache = calloc(n, sizeof(TrackCacheEntry));
    if (!cache) {
        json_value_free(root);
        return NULL;
    }

    int count = 0;
    for (size_t i = 0; i < n; i++) {
        JSON_Object* to = json_array_get_object(track_arr, i);
        if (!to) continue;
        load_track_from_json_object(to, &cache[count]);
        if (!cache[count].path[0]) continue;
        count++;
    }

    json_value_free(root);

    if (count <= 0) {
        free(cache);
        return NULL;
    }

    if (count > 1) {
        qsort(cache, (size_t)count, sizeof(TrackCacheEntry), track_cache_cmp);
    }

    if (out_count) *out_count = count;
    return cache;
}

static TrackCacheEntry* load_track_cache(int* out_count) {
    TrackCacheEntry* cache = load_track_cache_from_bin(out_count);
    if (cache) return cache;
    return load_track_cache_from_json(out_count);
}

static void discard_bin_index(void) {
    unlink(INDEX_BIN_PATH);
    unlink(INDEX_BIN_TMP_PATH);
}

static bool bin_file_size_valid(FILE* f, const IndexBinHeader* hdr) {
    if (fseek(f, 0, SEEK_END) != 0) return false;
    long fsize = ftell(f);
    if (fsize < 0) return false;
    if (fseek(f, (long)sizeof(*hdr), SEEK_SET) != 0) return false;

    long expected = (long)sizeof(*hdr)
        + (long)hdr->track_count * (long)sizeof(IndexBinTrack)
        + (long)hdr->playlist_count * (long)sizeof(IndexBinPlaylist);
    return fsize >= expected;
}

static bool load_tokens_from_file(FILE* f) {
    uint32_t tok_count = 0;
    if (fread(&tok_count, sizeof(tok_count), 1, f) != 1) return false;
    if (tok_count > LIBRARY_MAX_TOKENS) return false;

    token_hash_clear();
    token_count = 0;
    token_cap = 0;
    free(tokens);
    tokens = NULL;

    if (tok_count == 0) return true;

    tokens = calloc(tok_count, sizeof(IndexToken));
    if (!tokens) return false;
    token_cap = (int)tok_count;
    token_count = (int)tok_count;

    for (uint32_t i = 0; i < tok_count; i++) {
        if (fread(tokens[i].token, sizeof(tokens[i].token), 1, f) != 1) return false;

        uint32_t tn = 0;
        if (fread(&tn, sizeof(tn), 1, f) != 1) return false;
        if (tn > LIBRARY_MAX_TRACKS) return false;
        if (tn > 0) {
            tokens[i].track_ids = calloc(tn, sizeof(uint16_t));
            if (!tokens[i].track_ids) return false;
            if (fread(tokens[i].track_ids, sizeof(uint16_t), (size_t)tn, f) != (size_t)tn) {
                return false;
            }
            tokens[i].track_count = (int)tn;
            tokens[i].track_cap = (int)tn;
        }

        uint32_t pn = 0;
        if (fread(&pn, sizeof(pn), 1, f) != 1) return false;
        if (pn > LIBRARY_MAX_PLAYLISTS_INDEX) return false;
        if (pn > 0) {
            tokens[i].playlist_ids = calloc(pn, sizeof(uint16_t));
            if (!tokens[i].playlist_ids) return false;
            if (fread(tokens[i].playlist_ids, sizeof(uint16_t), (size_t)pn, f) != (size_t)pn) {
                return false;
            }
            tokens[i].playlist_count = (int)pn;
            tokens[i].playlist_cap = (int)pn;
        }

        if (!token_node_link((int)i)) return false;
    }
    index_logf("INFO load_tokens: %u tokens", tok_count);
    return true;
}

static bool save_tokens_to_file(FILE* f) {
    if (skip_token_index || token_index_degraded) {
        uint32_t zero = 0;
        return fwrite(&zero, sizeof(zero), 1, f) == 1;
    }

    uint32_t tok_count = (uint32_t)token_count;
    if (fwrite(&tok_count, sizeof(tok_count), 1, f) != 1) return false;

    for (int i = 0; i < token_count; i++) {
        IndexToken* t = &tokens[i];
        if (fwrite(t->token, sizeof(t->token), 1, f) != 1) return false;

        uint32_t tn = (uint32_t)t->track_count;
        if (fwrite(&tn, sizeof(tn), 1, f) != 1) return false;
        if (tn > 0 &&
            fwrite(t->track_ids, sizeof(uint16_t), (size_t)tn, f) != (size_t)tn) {
            return false;
        }

        uint32_t pn = (uint32_t)t->playlist_count;
        if (fwrite(&pn, sizeof(pn), 1, f) != 1) return false;
        if (pn > 0 &&
            fwrite(t->playlist_ids, sizeof(uint16_t), (size_t)pn, f) != (size_t)pn) {
            return false;
        }
    }
    return true;
}

static bool load_bin_index(uint64_t expected_fp) {
    FILE* f = fopen(INDEX_BIN_PATH, "rb");
    if (!f) return false;

    IndexBinHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 ||
        hdr.magic != INDEX_BIN_MAGIC ||
        !bin_version_supported(hdr.version) ||
        hdr.fingerprint != expected_fp ||
        hdr.track_count > LIBRARY_MAX_TRACKS ||
        hdr.playlist_count > LIBRARY_MAX_PLAYLISTS_INDEX ||
        !bin_file_size_valid(f, &hdr)) {
        fclose(f);
        discard_bin_index();
        index_log("load_bin_index: rejected cache file");
        return false;
    }

    free_index_data();
    skip_token_index = false;
    token_index_degraded = false;
    index_log("INFO load_bin_index: loading tracks from cache");

    bool tokens_on_disk = (hdr.version == LIBRARY_INDEX_FORMAT_VERSION);

    if (hdr.track_count > 0) {
        tracks = calloc(hdr.track_count, sizeof(IndexedTrack));
        if (!tracks) {
            fclose(f);
            discard_bin_index();
            return false;
        }
        for (uint32_t i = 0; i < hdr.track_count; i++) {
            IndexBinTrack bt;
            if (fread(&bt, sizeof(bt), 1, f) != 1) {
                free_index_data();
                fclose(f);
                discard_bin_index();
                index_log("ERR load_bin_index: truncated track record");
                return false;
            }
            copy_track_from_bin(&bt, &tracks[track_count]);
            track_count++;
            if (!tokens_on_disk) {
                index_track_tokens(track_count - 1);
            }
        }
    }

    if (hdr.playlist_count > 0) {
        playlists = calloc(hdr.playlist_count, sizeof(IndexedPlaylist));
        if (!playlists) {
            free_index_data();
            fclose(f);
            discard_bin_index();
            return false;
        }
        for (uint32_t i = 0; i < hdr.playlist_count; i++) {
            IndexBinPlaylist bp;
            if (fread(&bp, sizeof(bp), 1, f) != 1) {
                free_index_data();
                fclose(f);
                discard_bin_index();
                index_log("ERR load_bin_index: truncated playlist record");
                return false;
            }
            copy_playlist_from_bin(&bp, &playlists[playlist_count++]);
        }
    }

    if (tokens_on_disk) {
        if (!load_tokens_from_file(f)) {
            free_index_data();
            fclose(f);
            discard_bin_index();
            index_log("ERR load_bin_index: token section invalid");
            return false;
        }
    } else {
        index_log("INFO load_bin_index: indexing playlist tokens");
        for (int i = 0; i < playlist_count; i++) {
            index_playlist_tokens(i);
        }
    }

    fclose(f);
    saved_fingerprint = expected_fp;
    return true;
}

static int save_bin_index(uint64_t fp) {
    mkdir(SHARED_USERDATA_PATH "/music-player", 0755);

    FILE* f = fopen(INDEX_BIN_TMP_PATH, "wb");
    if (!f) return -1;

    IndexBinHeader hdr = {
        .magic = INDEX_BIN_MAGIC,
        .version = LIBRARY_INDEX_FORMAT_VERSION,
        .fingerprint = fp,
        .track_count = (uint32_t)track_count,
        .playlist_count = (uint32_t)playlist_count
    };

    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1) {
        fclose(f);
        unlink(INDEX_BIN_TMP_PATH);
        return -1;
    }

    for (int i = 0; i < track_count; i++) {
        IndexBinTrack bt = {0};
        strncpy(bt.path, tracks[i].path, sizeof(bt.path) - 1);
        strncpy(bt.title, tracks[i].title, sizeof(bt.title) - 1);
        strncpy(bt.artist, tracks[i].artist, sizeof(bt.artist) - 1);
        strncpy(bt.album, tracks[i].album, sizeof(bt.album) - 1);
        strncpy(bt.genre, tracks[i].genre, sizeof(bt.genre) - 1);
        {
            TrackNormScratch norms;
            fill_track_norms(&tracks[i], &norms);
            strncpy(bt.filename_norm, norms.filename_norm, sizeof(bt.filename_norm) - 1);
        }
        bt.format = (uint32_t)tracks[i].format;
        bt.mtime = tracks[i].file_mtime;
        bt.size = tracks[i].file_size;
        if (fwrite(&bt, sizeof(bt), 1, f) != 1) {
            fclose(f);
            unlink(INDEX_BIN_TMP_PATH);
            return -1;
        }
    }

    for (int i = 0; i < playlist_count; i++) {
        IndexBinPlaylist bp = {0};
        strncpy(bp.path, playlists[i].path, sizeof(bp.path) - 1);
        strncpy(bp.name, playlists[i].name, sizeof(bp.name) - 1);
        strncpy(bp.name_norm, playlists[i].name_norm, sizeof(bp.name_norm) - 1);
        strncpy(bp.parent_folder, playlists[i].parent_folder, sizeof(bp.parent_folder) - 1);
        strncpy(bp.parent_norm, playlists[i].parent_norm, sizeof(bp.parent_norm) - 1);
        bp.track_count = playlists[i].track_count;
        bp.is_root = playlists[i].is_root ? 1 : 0;
        if (fwrite(&bp, sizeof(bp), 1, f) != 1) {
            fclose(f);
            unlink(INDEX_BIN_TMP_PATH);
            return -1;
        }
    }

    if (!save_tokens_to_file(f)) {
        fclose(f);
        unlink(INDEX_BIN_TMP_PATH);
        return -1;
    }

    fflush(f);
    fclose(f);

    unlink(INDEX_BIN_PATH);
    if (rename(INDEX_BIN_TMP_PATH, INDEX_BIN_PATH) != 0) {
        unlink(INDEX_BIN_TMP_PATH);
        return -1;
    }
    return 0;
}

static bool load_json_index(uint64_t expected_fp) {
    JSON_Value* root = json_parse_file(INDEX_JSON_PATH);
    if (!root) return false;

    JSON_Object* obj = json_value_get_object(root);
    const char* fp = json_object_get_string(obj, "fingerprint");
    if (parse_fingerprint_string(fp) != expected_fp) {
        json_value_free(root);
        return false;
    }

    free_index_data();

    JSON_Array* track_arr = json_object_get_array(obj, "tracks");
    if (track_arr) {
        size_t n = json_array_get_count(track_arr);
        if (n > (size_t)LIBRARY_MAX_TRACKS) n = LIBRARY_MAX_TRACKS;
        tracks = calloc(n, sizeof(IndexedTrack));
        if (!tracks) {
            json_value_free(root);
            return false;
        }
        for (size_t i = 0; i < n; i++) {
            JSON_Object* to = json_array_get_object(track_arr, i);
            if (!to) continue;
            load_indexed_track_from_json(to, &tracks[track_count]);
            track_count++;
            index_track_tokens(track_count - 1);
        }
    }

    JSON_Array* pl_arr = json_object_get_array(obj, "playlists");
    if (pl_arr) {
        size_t n = json_array_get_count(pl_arr);
        playlists = calloc(n, sizeof(IndexedPlaylist));
        if (!playlists) {
            free_index_data();
            json_value_free(root);
            return false;
        }
        for (size_t i = 0; i < n; i++) {
            JSON_Object* po = json_array_get_object(pl_arr, i);
            if (!po) continue;
            const char* path = json_object_get_string(po, "path");
            if (path) strncpy(playlists[playlist_count].path, path, sizeof(playlists[playlist_count].path) - 1);
            const char* name = json_object_get_string(po, "name");
            if (name) strncpy(playlists[playlist_count].name, name, sizeof(playlists[playlist_count].name) - 1);
            const char* nn = json_object_get_string(po, "name_norm");
            if (nn) strncpy(playlists[playlist_count].name_norm, nn, sizeof(playlists[playlist_count].name_norm) - 1);
            const char* pf = json_object_get_string(po, "parent_folder");
            if (pf) strncpy(playlists[playlist_count].parent_folder, pf, sizeof(playlists[playlist_count].parent_folder) - 1);
            const char* pn = json_object_get_string(po, "parent_norm");
            if (pn) strncpy(playlists[playlist_count].parent_norm, pn, sizeof(playlists[playlist_count].parent_norm) - 1);
            playlists[playlist_count].track_count = (int)json_object_get_number(po, "track_count");
            playlists[playlist_count].is_root = json_object_get_boolean(po, "is_root");
            playlist_count++;
        }
    }

    for (int i = 0; i < playlist_count; i++) {
        index_playlist_tokens(i);
    }
    saved_fingerprint = expected_fp;
    json_value_free(root);

    if (save_bin_index(expected_fp) == 0) {
        unlink(INDEX_JSON_PATH);
    }
    return true;
}

static void index_playlist_tokens(int id) {
    if (id < 0 || id >= playlist_count) return;
    IndexedPlaylist* pl = &playlists[id];
    TokenAddCtx ctx = {.id = id, .is_track = false};

    Metadata_foreachToken(pl->name_norm, add_token_cb, &ctx);
    if (!pl->is_root) {
        Metadata_foreachToken(pl->parent_norm, add_token_cb, &ctx);
    }
}

static bool rebuild_index(uint64_t fp) {
    index_log("INFO rebuild_index: start");
    free_index_data();
    skip_token_index = false;
    token_index_degraded = false;
    last_ok_idx = -1;
    last_ok_path[0] = '\0';
    bool hard_failure = false;

    int cache_count = 0;
    TrackCacheEntry* cache = load_track_cache(&cache_count);
    {
        char logbuf[96];
        snprintf(logbuf, sizeof(logbuf), "rebuild_index: cache %d entries", cache_count);
        index_log(logbuf);
    }

    set_status("Scanning music...");
    index_log("rebuild_index: scanning music paths");
    {
        char** paths = NULL;
        int count = Playlist_collectPathsEx(MUSIC_PATH, &paths, LIBRARY_MAX_TRACKS, true);
        {
            char logbuf[96];
            snprintf(logbuf, sizeof(logbuf), "rebuild_index: found %d paths", count);
            index_log(logbuf);
        }
        if (count > 0 && paths) {
            int tracks_cap = count < 256 ? count : 256;
            size_t track_bytes = (size_t)tracks_cap * sizeof(IndexedTrack);
            if (!index_memory_ok(track_bytes)) {
                index_log("ERR rebuild_index: tracks budget exceeded");
                hard_failure = true;
            } else {
            tracks = calloc((size_t)tracks_cap, sizeof(IndexedTrack));
            if (tracks) {
                char status[128];
                int cache_hits = 0;
                int tag_reads = 0;
                int stat_skips = 0;
                index_log("rebuild_index: reading tags begin");
                index_log_stats("tracks array ready", 0);
                for (int i = 0; i < count; i++) {
                    bool verbose = index_verbose_step(track_count);

                    if ((i & 511) == 0) {
                        snprintf(status, sizeof(status), "Indexing %d/%d", i, count);
                        set_status(status);
                        char logbuf[128];
                        snprintf(logbuf, sizeof(logbuf),
                                 "rebuild_index: tags %d/%d hits=%d reads=%d skips=%d",
                                 i, count, cache_hits, tag_reads, stat_skips);
                        index_log(logbuf);
                        index_log_mem_milestone(i, count);
                    }

                    if (verbose) {
                        index_logf("rebuild_index: loop file %d/%d", i, count);
                        index_log_path("rebuild_index: file", track_count, paths[i]);
                    }

                    struct stat st;
                    if (stat(paths[i], &st) != 0 || !S_ISREG(st.st_mode)) {
                        stat_skips++;
                        if (verbose) {
                            index_logf("rebuild_index: skip stat fail i=%d", i);
                        }
                        continue;
                    }

                    if (verbose) {
                        index_logf("rebuild_index: stat ok size=%llu", (unsigned long long)st.st_size);
                    }

                    if (track_count >= tracks_cap) {
                        if (verbose) {
                            index_logf("rebuild_index: grow tracks %d -> ?", tracks_cap);
                        }
                        int new_cap = tracks_cap * 2;
                        if (new_cap > count) new_cap = count;
                        if (new_cap <= tracks_cap) break;
                        size_t grow_bytes = (size_t)(new_cap - tracks_cap) * sizeof(IndexedTrack);
                        if (!index_memory_ok(grow_bytes)) {
                            index_logf("ERR rebuild_index: tracks OOM at i=%d", i);
                            hard_failure = true;
                            break;
                        }
                        IndexedTrack* grown = realloc(tracks, (size_t)new_cap * sizeof(IndexedTrack));
                        if (!grown) {
                            index_log("ERR rebuild_index: tracks realloc failed");
                            hard_failure = true;
                            break;
                        }
                        memset(grown + tracks_cap, 0,
                               (size_t)(new_cap - tracks_cap) * sizeof(IndexedTrack));
                        tracks = grown;
                        tracks_cap = new_cap;
                        index_logf("rebuild_index: tracks cap %d", tracks_cap);
                    }

                    IndexedTrack* tr = &tracks[track_count];
                    const TrackCacheEntry* cached = track_cache_find(cache, cache_count, paths[i]);
                    if (cached && cached->file_mtime != 0 &&
                        cached->file_mtime == (uint64_t)st.st_mtime &&
                        cached->file_size == (uint64_t)st.st_size &&
                        track_cache_apply(cached, tr)) {
                        cache_hits++;
                        if (verbose) {
                            index_logf("rebuild_index: cache hit idx=%d", track_count);
                        }
                    } else {
                        tag_reads++;
                        memset(tr, 0, sizeof(*tr));
                        strncpy(tr->path, paths[i], sizeof(tr->path) - 1);
                        tr->format = Player_detectFormat(paths[i]);

                        if (verbose) {
                            index_logf("rebuild_index: read tags fmt=%d idx=%d",
                                       (int)tr->format, track_count);
                        }

                        TrackMetadata meta;
                        memset(&meta, 0, sizeof(meta));
                        Metadata_readFromFileEx(paths[i], tr->format, &meta);

                        if (verbose) {
                            index_logf("rebuild_index: tags read title=%.40s artist=%.32s",
                                       meta.title[0] ? meta.title : "(none)",
                                       meta.artist[0] ? meta.artist : "(none)");
                        }

                        strncpy(tr->title, meta.title, sizeof(tr->title) - 1);
                        strncpy(tr->artist, meta.artist, sizeof(tr->artist) - 1);
                        strncpy(tr->album, meta.album, sizeof(tr->album) - 1);
                        strncpy(tr->genre, meta.genre, sizeof(tr->genre) - 1);

                        const char* base = strrchr(paths[i], '/');
                        base = base ? base + 1 : paths[i];
                        (void)base;

                        tr->file_mtime = (uint64_t)st.st_mtime;
                        tr->file_size = (uint64_t)st.st_size;

                        if (!meta.title[0] || strcmp(meta.title, base) == 0) {
                            index_logf("WARN tag_empty idx=%d", track_count);
                        }
                    }

                    if (verbose) {
                        index_log_stats("before tokenize", track_count);
                    }

                    track_count++;
                    index_track_tokens(track_count - 1);

                    strncpy(last_ok_path, paths[i], sizeof(last_ok_path) - 1);
                    last_ok_idx = track_count - 1;
                    {
                        char prog[128];
                        const char* slash = strrchr(last_ok_path, '/');
                        snprintf(prog, sizeof(prog), "Indexing %d/%d (%s)",
                                 i + 1, count, slash ? slash + 1 : last_ok_path);
                        set_status(prog);
                    }

                    if (verbose) {
                        index_log_stats("after tokenize", track_count - 1);
                    }
                }
                snprintf(status, sizeof(status), "Reading tags... %d/%d", count, count);
                set_status(status);
                {
                    char logbuf[128];
                    snprintf(logbuf, sizeof(logbuf),
                             "rebuild_index: tags done indexed=%d hits=%d reads=%d skips=%d cap=%d",
                             track_count, cache_hits, tag_reads, stat_skips, tracks_cap);
                    index_log(logbuf);
                }
            } else {
                index_log("ERR rebuild_index: tracks alloc failed");
                hard_failure = true;
            }
            }
            Playlist_freePaths(paths, count);
        }
    }
    free(cache);

    {
        char logbuf[96];
        snprintf(logbuf, sizeof(logbuf), "INFO rebuild_index: track tokens done count=%d", token_count);
        index_log(logbuf);
    }

    char partial[128];
    snprintf(partial, sizeof(partial), "Tracks (%d), scanning playlists...", track_count);
    set_status(partial);

    set_status("Scanning playlists...");
    index_log("rebuild_index: playlist scan begin");
    {
        int limit = Settings_getMaxPlaylists();
        char logbuf[96];
        snprintf(logbuf, sizeof(logbuf), "rebuild_index: playlist limit=%d depth=%d",
                 limit, Settings_getPlaylistScanDepth());
        index_log(logbuf);
        PlaylistInfo* infos = calloc(limit, sizeof(PlaylistInfo));
        if (!infos) {
            index_log("rebuild_index: playlist infos alloc failed");
        } else {
            int n = M3U_listAllPlaylistsEx(infos, limit, Settings_getPlaylistScanDepth(), true);
            snprintf(logbuf, sizeof(logbuf), "rebuild_index: playlist scan returned %d", n);
            index_log(logbuf);
            if (n > 0) {
                free(playlists);
                playlists = calloc(n, sizeof(IndexedPlaylist));
                playlist_count = 0;
                if (!playlists) {
                    index_log("rebuild_index: playlists alloc failed");
                } else {
                    for (int i = 0; i < n; i++) {
                        if (infos[i].is_folder) continue;

                        IndexedPlaylist* pl = &playlists[playlist_count];
                        strncpy(pl->path, infos[i].path, sizeof(pl->path) - 1);
                        strncpy(pl->name, infos[i].name, sizeof(pl->name) - 1);
                        pl->track_count = infos[i].track_count;
                        pl->is_root = M3U_isRootPlaylist(infos[i].path);

                        Metadata_copyNormalized(pl->name_norm, sizeof(pl->name_norm), infos[i].name);
                        M3U_getPlaylistParentFolder(infos[i].path, pl->parent_folder, sizeof(pl->parent_folder));
                        Metadata_copyNormalized(pl->parent_norm, sizeof(pl->parent_norm), pl->parent_folder);

                        playlist_count++;
                    }
                    snprintf(logbuf, sizeof(logbuf), "rebuild_index: indexed %d playlists", playlist_count);
                    index_log(logbuf);
                }
            }
            free(infos);
        }
    }

    index_log("rebuild_index: playlist tokens begin");
    index_log_stats("playlist tokens start", -1);
    for (int i = 0; i < playlist_count; i++) {
        if ((i & 255) == 0) {
            index_logf("rebuild_index: playlist tokens %d/%d", i, playlist_count);
            index_log_stats("playlist tokenize", i);
        }
        index_playlist_tokens(i);
    }
    {
        char logbuf[96];
        snprintf(logbuf, sizeof(logbuf), "rebuild_index: playlist tokens done token_count=%d", token_count);
        index_log(logbuf);
    }

    set_status("Saving index...");
    index_log("INFO rebuild_index: save begin");
    saved_fingerprint = fp;
    int save_rc = save_bin_index(fp);
    char done[128];
    snprintf(done, sizeof(done), "INFO rebuild_index: save rc=%d", save_rc);
    index_log(done);

    if (save_rc != 0 || hard_failure) {
        snprintf(last_failure_reason, sizeof(last_failure_reason),
                 save_rc != 0 ? "Index save failed" : "Index build incomplete");
        set_build_state(INDEX_STATE_IDLE);
        index_log("ERR rebuild_index: failed");
        return false;
    }

    if (token_index_degraded) {
        snprintf(done, sizeof(done), "Ready (%d tracks, lite search)", track_count);
    } else {
        snprintf(done, sizeof(done), "Ready (%d tracks)", track_count);
    }
    set_status(done);
    snprintf(done, sizeof(done), "INFO rebuild_index: done (%d tracks, %d playlists)",
             track_count, playlist_count);
    index_log(done);
    return true;
}

static bool build_once(bool force_rebuild) {
    M3U_init();

    LibraryFingerprint fp;
    compute_fingerprint(&fp);

    char fp_msg[96];
    snprintf(fp_msg, sizeof(fp_msg), "INFO build_thread: fingerprint %016llx",
             (unsigned long long)fp.hash);
    index_log(fp_msg);

    if (force_rebuild) {
        return rebuild_index(fp.hash);
    }

    set_status("Loading index...");
    if (load_bin_index(fp.hash)) {
        char done[128];
        snprintf(done, sizeof(done), "Ready (%d tracks)", track_count);
        set_status(done);
        index_log("INFO build_thread: loaded binary index");
        return true;
    }
    if (load_json_index(fp.hash)) {
        char done[128];
        snprintf(done, sizeof(done), "Ready (%d tracks)", track_count);
        set_status(done);
        index_log("INFO build_thread: migrated JSON index");
        return true;
    }
    return rebuild_index(fp.hash);
}

static void* supervisor_thread_func(void* arg) {
    bool force_rebuild = ((intptr_t)arg) != 0;
    PWR_pinToCores(CPU_CORE_PERFORMANCE);

    index_log(force_rebuild ? "INFO supervisor: force rebuild" : "INFO supervisor: start");

    if (!force_rebuild) {
        int persisted_failures = 0;
        read_failure_count_file(&persisted_failures);
        if (persisted_failures >= INDEX_BUILD_MAX_FAILURES) {
            snprintf(last_failure_reason, sizeof(last_failure_reason),
                     "Index failed %d times", persisted_failures);
            set_build_state(INDEX_STATE_FAILED);
            snprintf(build_status, sizeof(build_status), "Search index unavailable");
            build_running = false;
            index_log("ERR supervisor: persisted failure limit reached");
            return NULL;
        }
        build_failure_count = persisted_failures;
    } else {
        build_failure_count = 0;
        clear_failure_count_file();
    }

    bool success = false;
    for (int attempt = 1; attempt <= INDEX_BUILD_MAX_FAILURES; attempt++) {
        index_logf("INFO supervisor: attempt %d/%d", attempt, INDEX_BUILD_MAX_FAILURES);
        set_build_state(INDEX_STATE_BUILDING);
        snprintf(build_status, sizeof(build_status), "Building index (%d/%d)...",
                 attempt, INDEX_BUILD_MAX_FAILURES);

        if (build_once(force_rebuild)) {
            success = true;
            build_failure_count = 0;
            clear_failure_count_file();
            set_build_state(INDEX_STATE_READY);
            break;
        }

        build_failure_count = attempt;
        write_failure_count_file(attempt);
        snprintf(last_failure_reason, sizeof(last_failure_reason),
                 "Build failed (attempt %d)", attempt);

        if (attempt < INDEX_BUILD_MAX_FAILURES) {
            int delay_sec = attempt == 1 ? 2 : 5;
            index_logf("WARN supervisor: retry in %ds", delay_sec);
            sleep((unsigned int)delay_sec);
        }
        force_rebuild = true;
    }

    pthread_mutex_lock(&index_mutex);
    build_running = false;
    if (success) {
        index_ready = true;
        build_state = INDEX_STATE_READY;
    } else {
        index_ready = false;
        build_state = INDEX_STATE_FAILED;
        snprintf(build_status, sizeof(build_status), "Search index failed (3 attempts)");
        index_log("ERR supervisor: giving up after 3 failures");
    }
    pthread_mutex_unlock(&index_mutex);

    index_log(success ? "INFO supervisor: complete" : "ERR supervisor: failed");
    return NULL;
}

static void* build_thread_func(void* arg) {
    return supervisor_thread_func(arg);
}

static bool start_build_thread(bool force_rebuild) {
    pthread_mutex_lock(&index_mutex);
    if (build_running) {
        pthread_mutex_unlock(&index_mutex);
        return false;
    }
    build_running = true;
    index_ready = false;
    pthread_mutex_unlock(&index_mutex);

    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, INDEX_BUILD_STACK_SIZE);

    int rc = pthread_create(&thread, &attr, build_thread_func,
                            (void*)(intptr_t)(force_rebuild ? 1 : 0));
    pthread_attr_destroy(&attr);

    if (rc != 0) {
        pthread_mutex_lock(&index_mutex);
        build_running = false;
        pthread_mutex_unlock(&index_mutex);
        snprintf(build_status, sizeof(build_status), "Index build failed");
        index_log("start_build_thread: pthread_create failed");
        return false;
    }
    pthread_detach(thread);
    return true;
}

void LibraryIndex_init(void) {
    if (build_started) return;
    build_started = true;
    build_log_clear();
    snprintf(build_status, sizeof(build_status), "Starting...");
    if (Settings_getIndexLog()) unlink(INDEX_LOG_PATH);
    index_log("LibraryIndex_init");
    start_build_thread(false);
}

bool LibraryIndex_requestRebuild(void) {
    build_log_clear();
    if (Settings_getIndexLog()) unlink(INDEX_LOG_PATH);
    build_failure_count = 0;
    clear_failure_count_file();
    set_build_state(INDEX_STATE_BUILDING);
    set_status("Rebuilding index...");
    return start_build_thread(true);
}

void LibraryIndex_quit(void) {
    free_index_data();
    index_ready = false;
    build_started = false;
    build_state = INDEX_STATE_IDLE;
    build_running = false;
}

bool LibraryIndex_isReady(void) {
    pthread_mutex_lock(&index_mutex);
    bool ready = index_ready && build_state == INDEX_STATE_READY;
    pthread_mutex_unlock(&index_mutex);
    return ready;
}

bool LibraryIndex_isBuilding(void) {
    pthread_mutex_lock(&index_mutex);
    bool running = build_running;
    pthread_mutex_unlock(&index_mutex);
    return running;
}

bool LibraryIndex_isFailed(void) {
    pthread_mutex_lock(&index_mutex);
    bool failed = build_state == INDEX_STATE_FAILED;
    pthread_mutex_unlock(&index_mutex);
    return failed;
}

bool LibraryIndex_canSearch(void) {
    pthread_mutex_lock(&index_mutex);
    bool ok = index_ready && build_state == INDEX_STATE_READY && track_count > 0;
    pthread_mutex_unlock(&index_mutex);
    return ok;
}

bool LibraryIndex_isDegraded(void) {
    return token_index_degraded;
}

IndexBuildState LibraryIndex_getBuildState(void) {
    pthread_mutex_lock(&index_mutex);
    IndexBuildState state = build_state;
    pthread_mutex_unlock(&index_mutex);
    return state;
}

int LibraryIndex_getBuildFailureCount(void) {
    pthread_mutex_lock(&index_mutex);
    int count = build_failure_count;
    pthread_mutex_unlock(&index_mutex);
    return count;
}

const char* LibraryIndex_getLastSearchError(void) {
    return last_search_error[0] ? last_search_error : NULL;
}

const char* LibraryIndex_getBuildStatus(void) {
    return build_status;
}

int LibraryIndex_getBuildLogCount(void) {
    pthread_mutex_lock(&index_mutex);
    int count = build_log_count;
    pthread_mutex_unlock(&index_mutex);
    return count;
}

const char* LibraryIndex_getBuildLogLine(int index) {
    static char line_buf[BUILD_LOG_LINE_LEN];
    pthread_mutex_lock(&index_mutex);
    int slot = build_log_slot(index);
    if (slot < 0) {
        line_buf[0] = '\0';
    } else {
        strncpy(line_buf, build_log_lines[slot], sizeof(line_buf) - 1);
        line_buf[sizeof(line_buf) - 1] = '\0';
    }
    pthread_mutex_unlock(&index_mutex);
    return line_buf;
}

static void add_postings_for_token(int token_idx,
                                   bool* track_matched_token, bool* playlist_matched_token,
                                   int* track_hits, int* playlist_hits) {
    if (token_idx < 0) return;

    IndexToken* tok = &tokens[token_idx];
    for (int i = 0; i < tok->track_count; i++) {
        int id = (int)tok->track_ids[i];
        if (id < 0 || id >= track_count) continue;
        if (track_hits) track_hits[id]++;
        if (track_matched_token && !track_matched_token[id]) {
            track_matched_token[id] = true;
        }
    }
    for (int i = 0; i < tok->playlist_count; i++) {
        int id = (int)tok->playlist_ids[i];
        if (id < 0 || id >= playlist_count) continue;
        if (playlist_hits) playlist_hits[id]++;
        if (playlist_matched_token && !playlist_matched_token[id]) {
            playlist_matched_token[id] = true;
        }
    }
}

static void apply_query_token(const char* query_token, bool fuzzy,
                              bool* track_matched_token, bool* playlist_matched_token,
                              int* track_hits, int* playlist_hits) {
    if (!query_token || !query_token[0]) return;

    int exact = find_token(query_token);
    if (exact >= 0) {
        add_postings_for_token(exact, track_matched_token, playlist_matched_token,
                               track_hits, playlist_hits);
    }

    if (!fuzzy || strlen(query_token) < FUZZY_MIN_TOKEN_LEN) return;

    int max_err = SearchFuse_maxEditErrors(strlen(query_token), true);
    int bucket = token_first_char_bucket(query_token);
    for (TokenHashNode* n = token_first_char[bucket]; n; n = n->char_next) {
        if (n->token_idx == exact) continue;
        const char* candidate = tokens[n->token_idx].token;
        if (!SearchFuse_withinEditDistance(query_token, candidate, max_err)) continue;
        add_postings_for_token(n->token_idx, track_matched_token, playlist_matched_token,
                               track_hits, playlist_hits);
    }
}

static int count_marked(const bool* marks, int count) {
    int n = 0;
    for (int i = 0; i < count; i++) {
        if (marks[i]) n++;
    }
    return n;
}

static void intersect_token_hits(const int* hits, int hit_count, int required,
                               bool* selected) {
    for (int i = 0; i < hit_count; i++) {
        if (hits[i] >= required) selected[i] = true;
    }
}

static void union_token_hits(const int* hits, int hit_count, int required,
                             bool* selected) {
    for (int i = 0; i < hit_count; i++) {
        if (hits[i] >= required && !selected[i]) selected[i] = true;
    }
}

static void gather_track_candidates_fuse_scan(const char query_tokens[][48], int query_token_count,
                                              bool fuzzy, bool* track_candidate) {
    if (!track_candidate || query_token_count <= 0 || track_count <= 0) return;

    char query_phrase[512];
    query_phrase[0] = '\0';
    for (int q = 0; q < query_token_count; q++) {
        if (q > 0) strncat(query_phrase, " ", sizeof(query_phrase) - strlen(query_phrase) - 1);
        strncat(query_phrase, query_tokens[q], sizeof(query_phrase) - strlen(query_phrase) - 1);
    }

    for (int i = 0; i < track_count; i++) {
        int sc = score_track_fuse(i, query_phrase, query_tokens, query_token_count, fuzzy);
        if (sc >= SEARCH_FUSE_MIN_SCORE) {
            track_candidate[i] = true;
        }
    }
}

static void gather_track_candidates(const char query_tokens[][48], int query_token_count,
                                    bool fuzzy, bool* track_candidate) {
    if (!track_candidate || query_token_count <= 0) return;

    if (token_index_degraded || skip_token_index || token_count == 0) {
        gather_track_candidates_fuse_scan(query_tokens, query_token_count, fuzzy, track_candidate);
        return;
    }

    int* hits = calloc((size_t)track_count, sizeof(int));
    bool* matched = calloc((size_t)track_count, sizeof(bool));
    if (!hits || !matched) {
        free(hits);
        free(matched);
        return;
    }

    for (int q = 0; q < query_token_count; q++) {
        memset(hits, 0, (size_t)track_count * sizeof(int));
        memset(matched, 0, (size_t)track_count * sizeof(bool));
        apply_query_token(query_tokens[q], false, matched, NULL, hits, NULL);
        if (q == 0) {
            intersect_token_hits(hits, track_count, 1, track_candidate);
        } else {
            for (int i = 0; i < track_count; i++) {
                if (track_candidate[i] && hits[i] < 1) track_candidate[i] = false;
            }
        }
    }

    int exact_count = count_marked(track_candidate, track_count);
    if (!fuzzy || exact_count >= FUZZY_RESULT_THRESHOLD) {
        free(hits);
        free(matched);
        return;
    }

    memset(track_candidate, 0, (size_t)track_count * sizeof(bool));
    for (int q = 0; q < query_token_count; q++) {
        memset(hits, 0, (size_t)track_count * sizeof(int));
        memset(matched, 0, (size_t)track_count * sizeof(bool));
        apply_query_token(query_tokens[q], true, matched, NULL, hits, NULL);
        union_token_hits(hits, track_count, 1, track_candidate);
    }

    free(hits);
    free(matched);
}

static void gather_playlist_candidates(const char query_tokens[][48], int query_token_count,
                                       bool fuzzy, bool* playlist_candidate) {
    if (!playlist_candidate || query_token_count <= 0 || playlist_count <= 0) return;

    int* hits = calloc((size_t)playlist_count, sizeof(int));
    bool* matched = calloc((size_t)playlist_count, sizeof(bool));
    if (!hits || !matched) {
        free(hits);
        free(matched);
        return;
    }

    for (int q = 0; q < query_token_count; q++) {
        memset(hits, 0, (size_t)playlist_count * sizeof(int));
        memset(matched, 0, (size_t)playlist_count * sizeof(bool));
        apply_query_token(query_tokens[q], false, NULL, matched, NULL, hits);
        if (q == 0) {
            intersect_token_hits(hits, playlist_count, 1, playlist_candidate);
        } else {
            for (int i = 0; i < playlist_count; i++) {
                if (playlist_candidate[i] && hits[i] < 1) playlist_candidate[i] = false;
            }
        }
    }

    int exact_count = count_marked(playlist_candidate, playlist_count);
    if (!fuzzy || exact_count >= FUZZY_RESULT_THRESHOLD) {
        free(hits);
        free(matched);
        return;
    }

    memset(playlist_candidate, 0, (size_t)playlist_count * sizeof(bool));
    for (int q = 0; q < query_token_count; q++) {
        memset(hits, 0, (size_t)playlist_count * sizeof(int));
        memset(matched, 0, (size_t)playlist_count * sizeof(bool));
        apply_query_token(query_tokens[q], true, NULL, matched, NULL, hits);
        union_token_hits(hits, playlist_count, 1, playlist_candidate);
    }

    free(hits);
    free(matched);
}

static int score_track_fuse(int id, const char* query_norm,
                            const char query_tokens[][48], int query_token_count,
                            bool fuzzy) {
    if (id < 0 || id >= track_count) return 0;
    IndexedTrack* tr = &tracks[id];
    TrackNormScratch norms;
    fill_track_norms(tr, &norms);
    SearchFuseFields fields = {
        .title = norms.title_norm,
        .artist = norms.artist_norm,
        .album = norms.album_norm,
        .genre = norms.genre_norm,
        .filename = norms.filename_norm
    };
    return SearchFuse_scoreFields(&fields, query_norm, query_tokens, query_token_count, fuzzy);
}

static int score_playlist_fuse(int id, const char* query_norm,
                               const char query_tokens[][48], int query_token_count,
                               bool fuzzy, bool nested) {
    if (id < 0 || id >= playlist_count) return 0;
    IndexedPlaylist* pl = &playlists[id];
    if (nested) {
        return SearchFuse_scorePlaylist(pl->name_norm, pl->parent_norm,
                                      query_norm, query_tokens, query_token_count, fuzzy);
    }
    SearchFuseFields fields = {
        .title = pl->name_norm,
        .artist = NULL,
        .album = NULL,
        .genre = NULL,
        .filename = NULL
    };
    return SearchFuse_scoreFields(&fields, query_norm, query_tokens, query_token_count, fuzzy);
}

typedef struct {
    int id;
    int score;
    SearchResultType type;
} ScoredItem;

static int compare_scored(const void* a, const void* b) {
    const ScoredItem* sa = (const ScoredItem*)a;
    const ScoredItem* sb = (const ScoredItem*)b;
    if (sb->score != sa->score) return sb->score - sa->score;
    return sa->id - sb->id;
}

static void fill_track_row(SearchResultRow* row, int id, int score) {
    if (!row || id < 0 || id >= track_count) return;
    IndexedTrack* tr = &tracks[id];
    row->type = SEARCH_ITEM_TRACK;
    row->score = score;
    strncpy(row->path, tr->path, sizeof(row->path) - 1);
    if (tr->title[0]) {
        strncpy(row->label, tr->title, sizeof(row->label) - 1);
    } else {
        track_path_basename(tr, row->label, sizeof(row->label));
    }

    if (tr->artist[0] && tr->album[0]) {
        snprintf(row->subtitle, sizeof(row->subtitle), "%s — %s", tr->artist, tr->album);
    } else if (tr->artist[0]) {
        snprintf(row->subtitle, sizeof(row->subtitle), "%s", tr->artist);
    } else if (tr->album[0]) {
        snprintf(row->subtitle, sizeof(row->subtitle), "%s", tr->album);
    } else {
        row->subtitle[0] = '\0';
    }
    row->track_count = 1;
    row->format = tr->format;
}

static void fill_playlist_row(SearchResultRow* row, int id, int score, bool nested) {
    IndexedPlaylist* pl = &playlists[id];
    row->type = nested ? SEARCH_ITEM_NESTED_PLAYLIST : SEARCH_ITEM_USER_PLAYLIST;
    row->score = score;
    strncpy(row->path, pl->path, sizeof(row->path) - 1);
    row->track_count = pl->track_count;
    row->format = AUDIO_FORMAT_UNKNOWN;

    if (nested && pl->parent_folder[0]) {
        snprintf(row->label, sizeof(row->label), "%s / %s", pl->parent_folder, pl->name);
        snprintf(row->subtitle, sizeof(row->subtitle), "%d tracks", pl->track_count);
    } else {
        snprintf(row->label, sizeof(row->label), "%s", pl->name);
        snprintf(row->subtitle, sizeof(row->subtitle), "%d tracks", pl->track_count);
    }
}

typedef struct {
    char (*tokens)[48];
    int count;
} QueryCollectCtx;

static void collect_query_token(const char* token, void* userdata) {
    QueryCollectCtx* ctx = (QueryCollectCtx*)userdata;
    if (ctx->count >= 32) return;
    strncpy(ctx->tokens[ctx->count], token, 47);
    ctx->tokens[ctx->count][47] = '\0';
    ctx->count++;
}

bool LibraryIndex_search(const char* query, SearchResults* out) {
    last_search_error[0] = '\0';
    if (!out) {
        strncpy(last_search_error, "Invalid results", sizeof(last_search_error) - 1);
        return false;
    }
    memset(out, 0, sizeof(*out));

    if (!query || !query[0]) {
        strncpy(last_search_error, "Empty query", sizeof(last_search_error) - 1);
        return false;
    }

    if (LibraryIndex_isFailed()) {
        strncpy(last_search_error, "Index unavailable", sizeof(last_search_error) - 1);
        return false;
    }

    if (!LibraryIndex_canSearch()) {
        if (LibraryIndex_isBuilding()) {
            strncpy(last_search_error, "Index building", sizeof(last_search_error) - 1);
        } else {
            strncpy(last_search_error, "Index not ready", sizeof(last_search_error) - 1);
        }
        return false;
    }

    pthread_mutex_lock(&index_mutex);
    if (track_count == 0 && playlist_count == 0) {
        strncpy(last_search_error, "Empty index", sizeof(last_search_error) - 1);
        pthread_mutex_unlock(&index_mutex);
        return false;
    }

    char query_tokens[32][48];
    QueryCollectCtx qctx = {.tokens = query_tokens, .count = 0};

    Metadata_foreachToken(query, collect_query_token, &qctx);
    int query_token_count = qctx.count;
    if (query_token_count == 0) {
        strncpy(last_search_error, "No search terms", sizeof(last_search_error) - 1);
        pthread_mutex_unlock(&index_mutex);
        return false;
    }

    char query_phrase[512];
    Metadata_copyNormalized(query_phrase, sizeof(query_phrase), query);

    bool fuzzy_enabled = Settings_getFuzzySearch();
    bool* track_candidate = calloc((size_t)track_count, sizeof(bool));
    bool* playlist_candidate = calloc((size_t)playlist_count, sizeof(bool));
    if (!track_candidate || !playlist_candidate) {
        free(track_candidate);
        free(playlist_candidate);
        strncpy(last_search_error, "Search OOM", sizeof(last_search_error) - 1);
        index_log("ERR search OOM: candidate bitmaps");
        pthread_mutex_unlock(&index_mutex);
        return false;
    }

    gather_track_candidates(query_tokens, query_token_count, fuzzy_enabled, track_candidate);
    gather_playlist_candidates(query_tokens, query_token_count, fuzzy_enabled, playlist_candidate);

    ScoredItem* nested_items = calloc((size_t)playlist_count, sizeof(ScoredItem));
    ScoredItem* mixed_items = calloc((size_t)(track_count + playlist_count), sizeof(ScoredItem));
    int nested_n = 0;
    int mixed_n = 0;

    if (!nested_items || !mixed_items) {
        free(track_candidate);
        free(playlist_candidate);
        free(nested_items);
        free(mixed_items);
        strncpy(last_search_error, "Search OOM", sizeof(last_search_error) - 1);
        index_log("ERR search OOM: score arrays");
        pthread_mutex_unlock(&index_mutex);
        return false;
    }

    for (int i = 0; i < playlist_count; i++) {
        if (!playlist_candidate[i]) continue;
        if (playlists[i].is_root) continue;

        int sc = score_playlist_fuse(i, query_phrase, query_tokens, query_token_count,
                                     fuzzy_enabled, true);
        if (sc < SEARCH_FUSE_MIN_SCORE) continue;
        nested_items[nested_n++] = (ScoredItem){
            .id = i, .score = sc, .type = SEARCH_ITEM_NESTED_PLAYLIST
        };
    }
    qsort(nested_items, nested_n, sizeof(ScoredItem), compare_scored);

    int nested_take = nested_n < LIBRARY_SEARCH_MAX_TOP ? nested_n : LIBRARY_SEARCH_MAX_TOP;
    out->nested_count = nested_take;
    out->mixed_limit = LIBRARY_SEARCH_BASE_MIXED + (LIBRARY_SEARCH_MAX_TOP - nested_take);

    for (int i = 0; i < nested_take; i++) {
        fill_playlist_row(&out->nested[i], nested_items[i].id, nested_items[i].score, true);
    }

    for (int i = 0; i < track_count; i++) {
        if (!track_candidate[i]) continue;
        int sc = score_track_fuse(i, query_phrase, query_tokens, query_token_count, fuzzy_enabled);
        if (sc < SEARCH_FUSE_MIN_SCORE) continue;
        mixed_items[mixed_n++] = (ScoredItem){
            .id = i, .score = sc, .type = SEARCH_ITEM_TRACK
        };
    }
    for (int i = 0; i < playlist_count; i++) {
        if (!playlist_candidate[i]) continue;
        if (!playlists[i].is_root) continue;

        int sc = score_playlist_fuse(i, query_phrase, query_tokens, query_token_count,
                                     fuzzy_enabled, false);
        if (sc < SEARCH_FUSE_MIN_SCORE) continue;
        mixed_items[mixed_n++] = (ScoredItem){
            .id = i, .score = sc, .type = SEARCH_ITEM_USER_PLAYLIST
        };
    }
    qsort(mixed_items, mixed_n, sizeof(ScoredItem), compare_scored);

    int mixed_take = mixed_n < out->mixed_limit ? mixed_n : out->mixed_limit;
    out->mixed_count = mixed_take;

    for (int i = 0; i < mixed_take; i++) {
        if (mixed_items[i].type == SEARCH_ITEM_TRACK) {
            fill_track_row(&out->mixed[i], mixed_items[i].id, mixed_items[i].score);
        } else {
            fill_playlist_row(&out->mixed[i], mixed_items[i].id, mixed_items[i].score, false);
        }
    }

    bool has_results = out->nested_count > 0 || out->mixed_count > 0;

    free(track_candidate);
    free(playlist_candidate);
    free(nested_items);
    free(mixed_items);
    pthread_mutex_unlock(&index_mutex);
    return has_results;
}
