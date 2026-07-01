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

#define LIBRARY_INDEX_FORMAT_VERSION 4
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
#define INDEX_BUILD_STACK_SIZE (512 * 1024)
#define BUILD_LOG_MAX_LINES 512
#define BUILD_LOG_LINE_LEN 160

typedef struct {
    char path[512];
    char title[256];
    char artist[256];
    char album[256];
    char genre[256];
    char title_norm[256];
    char artist_norm[256];
    char album_norm[256];
    char genre_norm[256];
    char filename_norm[256];
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
    int* track_ids;
    int track_count;
    int track_cap;
    int* playlist_ids;
    int playlist_count;
    int playlist_cap;
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
    struct TokenHashNode* next;
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

#define INDEX_VERBOSE_TRACKS 64
#define INDEX_VERBOSE_EVERY 32

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
    pthread_mutex_unlock(&index_mutex);
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

static bool token_hash_node_push(int token_idx, TokenHashNode*** head) {
    if (token_idx < 0 || token_idx >= token_count || !head) return false;

    if (token_hash_node_count >= token_hash_node_cap) {
        int nc = token_hash_node_cap ? token_hash_node_cap * 2 : TOKEN_INIT_CAP;
        TokenHashNode* nn = realloc(token_hash_nodes, sizeof(TokenHashNode) * nc);
        if (!nn) {
            index_log("token_index: hash node realloc failed");
            return false;
        }
        token_hash_nodes = nn;
        token_hash_node_cap = nc;
    }

    TokenHashNode* node = &token_hash_nodes[token_hash_node_count++];
    node->token_idx = token_idx;
    node->next = *head;
    *head = node;
    return true;
}

static void token_first_char_insert(int token_idx) {
    int bucket = token_first_char_bucket(tokens[token_idx].token);
    token_hash_node_push(token_idx, &token_first_char[bucket]);
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

static void token_hash_insert(int token_idx) {
    if (token_idx < 0 || token_idx >= token_count) return;
    uint32_t bucket = hash_token_string(tokens[token_idx].token) % TOKEN_HASH_BUCKETS;
    token_hash_node_push(token_idx, &token_hash[bucket]);
    token_first_char_insert(token_idx);
}

static int find_token(const char* token) {
    if (!token || !token[0]) return -1;

    uint32_t bucket = hash_token_string(token) % TOKEN_HASH_BUCKETS;
    for (TokenHashNode* n = token_hash[bucket]; n; n = n->next) {
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
    token_hash_insert(idx);
    return idx;
}

static void token_add_track(int token_idx, int track_id) {
    if (token_idx < 0 || track_id < 0) return;
    IndexToken* t = &tokens[token_idx];

    for (int i = 0; i < t->track_count; i++) {
        if (t->track_ids[i] == track_id) return;
    }

    if (t->track_count >= t->track_cap) {
        int nc = t->track_cap ? t->track_cap * 2 : TOKEN_GROW;
        int* nd = realloc(t->track_ids, sizeof(int) * nc);
        if (!nd) {
            index_logf("token_index: track_ids realloc failed tok=%d track=%d",
                       token_idx, track_id);
            return;
        }
        t->track_ids = nd;
        t->track_cap = nc;
    }
    t->track_ids[t->track_count++] = track_id;
}

static void token_add_playlist(int token_idx, int playlist_id) {
    if (token_idx < 0 || playlist_id < 0) return;
    IndexToken* t = &tokens[token_idx];

    for (int i = 0; i < t->playlist_count; i++) {
        if (t->playlist_ids[i] == playlist_id) return;
    }

    if (t->playlist_count >= t->playlist_cap) {
        int nc = t->playlist_cap ? t->playlist_cap * 2 : TOKEN_GROW;
        int* nd = realloc(t->playlist_ids, sizeof(int) * nc);
        if (!nd) return;
        t->playlist_ids = nd;
        t->playlist_cap = nc;
    }
    t->playlist_ids[t->playlist_count++] = playlist_id;
}

typedef struct {
    int id;
    bool is_track;
} TokenAddCtx;

static void add_token_cb(const char* token, void* userdata) {
    TokenAddCtx* ctx = (TokenAddCtx*)userdata;
    int ti = ensure_token(token);
    if (ti < 0) return;
    if (ctx->is_track) token_add_track(ti, ctx->id);
    else token_add_playlist(ti, ctx->id);
}

static void index_track_norms(IndexedTrack* tr) {
    if (!tr) return;
    Metadata_copyNormalized(tr->title_norm, sizeof(tr->title_norm), tr->title);
    Metadata_copyNormalized(tr->artist_norm, sizeof(tr->artist_norm), tr->artist);
    Metadata_copyNormalized(tr->album_norm, sizeof(tr->album_norm), tr->album);
    Metadata_copyNormalized(tr->genre_norm, sizeof(tr->genre_norm), tr->genre);
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

    TokenAddCtx ctx = {.id = id, .is_track = true};
    int tok_before = token_count;

    index_logf("tokenize [%d] title_norm=%.32s", id,
                 tr->title_norm[0] ? tr->title_norm : "(empty)");
    Metadata_foreachNormalizedToken(tr->title_norm, add_token_cb, &ctx);
    index_log_stats("after title tokens", id);

    Metadata_foreachNormalizedToken(tr->artist_norm, add_token_cb, &ctx);
    Metadata_foreachNormalizedToken(tr->album_norm, add_token_cb, &ctx);
    Metadata_foreachNormalizedToken(tr->genre_norm, add_token_cb, &ctx);
    Metadata_foreachNormalizedToken(tr->filename_norm, add_token_cb, &ctx);
    index_phrase_token(tr->title_norm, id);

    index_logf("tokenize [%d] done +%d tokens", id, token_count - tok_before);
}

static void index_track_tokens(int id) {
    if (id < 0 || id >= track_count) return;
    if (index_verbose_step(id)) {
        index_track_tokens_verbose(id);
        return;
    }

    IndexedTrack* tr = &tracks[id];

    TokenAddCtx ctx = {.id = id, .is_track = true};

    Metadata_foreachNormalizedToken(tr->title_norm, add_token_cb, &ctx);
    Metadata_foreachNormalizedToken(tr->artist_norm, add_token_cb, &ctx);
    Metadata_foreachNormalizedToken(tr->album_norm, add_token_cb, &ctx);
    Metadata_foreachNormalizedToken(tr->genre_norm, add_token_cb, &ctx);
    Metadata_foreachNormalizedToken(tr->filename_norm, add_token_cb, &ctx);

    index_phrase_token(tr->title_norm, id);
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
    strncpy(dst->path, src->path, sizeof(dst->path) - 1);
    strncpy(dst->title, src->title, sizeof(dst->title) - 1);
    strncpy(dst->artist, src->artist, sizeof(dst->artist) - 1);
    strncpy(dst->album, src->album, sizeof(dst->album) - 1);
    strncpy(dst->genre, src->genre, sizeof(dst->genre) - 1);
    strncpy(dst->filename_norm, src->filename_norm, sizeof(dst->filename_norm) - 1);
    dst->format = (AudioFormat)src->format;
    dst->file_mtime = src->mtime;
    dst->file_size = src->size;
    index_track_norms(dst);
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
        version == LIBRARY_INDEX_FORMAT_VERSION) {
        return (uint64_t)value;
    }
    return 0;
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
        strncpy(tr->filename_norm, cached->filename_norm, sizeof(tr->filename_norm) - 1);
        tr->format = cached->format;
        tr->file_mtime = cached->file_mtime;
        tr->file_size = cached->file_size;
        index_track_norms(tr);
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
    const char* fn = json_object_get_string(to, "filename_norm");
    if (fn) strncpy(tr->filename_norm, fn, sizeof(tr->filename_norm) - 1);
    tr->format = (AudioFormat)json_object_get_number(to, "format");
    tr->file_mtime = (uint64_t)json_object_get_number(to, "mtime");
    tr->file_size = (uint64_t)json_object_get_number(to, "size");
    index_track_norms(tr);
}

static TrackCacheEntry* load_track_cache_from_bin(int* out_count) {
    if (out_count) *out_count = 0;

    FILE* f = fopen(INDEX_BIN_PATH, "rb");
    if (!f) return NULL;

    IndexBinHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 ||
        hdr.magic != INDEX_BIN_MAGIC ||
        hdr.version != LIBRARY_INDEX_FORMAT_VERSION) {
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

static bool load_bin_index(uint64_t expected_fp) {
    FILE* f = fopen(INDEX_BIN_PATH, "rb");
    if (!f) return false;

    IndexBinHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 ||
        hdr.magic != INDEX_BIN_MAGIC ||
        hdr.version != LIBRARY_INDEX_FORMAT_VERSION ||
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
    index_log("load_bin_index: loading tracks from cache");

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
                index_log("load_bin_index: truncated track record");
                return false;
            }
            copy_track_from_bin(&bt, &tracks[track_count]);
            track_count++;
            index_track_tokens(track_count - 1);
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
                index_log("load_bin_index: truncated playlist record");
                return false;
            }
            copy_playlist_from_bin(&bp, &playlists[playlist_count++]);
        }
    }

    fclose(f);
    index_log("load_bin_index: indexing playlist tokens");
    for (int i = 0; i < playlist_count; i++) {
        index_playlist_tokens(i);
    }
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
        strncpy(bt.filename_norm, tracks[i].filename_norm, sizeof(bt.filename_norm) - 1);
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

static void rebuild_index(uint64_t fp) {
    index_log("rebuild_index: start");
    free_index_data();

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
                        snprintf(status, sizeof(status), "Reading tags... %d/%d", i, count);
                        set_status(status);
                        char logbuf[128];
                        snprintf(logbuf, sizeof(logbuf),
                                 "rebuild_index: tags %d/%d hits=%d reads=%d skips=%d",
                                 i, count, cache_hits, tag_reads, stat_skips);
                        index_log(logbuf);
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
                        IndexedTrack* grown = realloc(tracks, (size_t)new_cap * sizeof(IndexedTrack));
                        if (!grown) {
                            index_log("rebuild_index: tracks realloc failed");
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
                        Metadata_copyNormalized(tr->filename_norm, sizeof(tr->filename_norm), base);
                        index_track_norms(tr);

                        tr->file_mtime = (uint64_t)st.st_mtime;
                        tr->file_size = (uint64_t)st.st_size;
                    }

                    if (verbose) {
                        index_log_stats("before tokenize", track_count);
                    }

                    track_count++;
                    index_track_tokens(track_count - 1);

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
                index_log("rebuild_index: tracks alloc failed");
            }
            Playlist_freePaths(paths, count);
        }
    }
    free(cache);

    {
        char logbuf[96];
        snprintf(logbuf, sizeof(logbuf), "rebuild_index: track tokens done count=%d", token_count);
        index_log(logbuf);
    }
    set_index_ready(true);
    index_log("rebuild_index: search enabled");

    char partial[128];
    snprintf(partial, sizeof(partial), "Ready tracks (%d), scanning playlists...", track_count);
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
    index_log("rebuild_index: save begin");
    saved_fingerprint = fp;
    int save_rc = save_bin_index(fp);
    char done[128];
    snprintf(done, sizeof(done), "rebuild_index: save rc=%d", save_rc);
    index_log(done);
    snprintf(done, sizeof(done), "Ready (%d tracks)", track_count);
    set_status(done);
    snprintf(done, sizeof(done), "rebuild_index: done (%d tracks, %d playlists)", track_count, playlist_count);
    index_log(done);
}

static void* build_thread_func(void* arg) {
    bool force_rebuild = ((intptr_t)arg) != 0;
    PWR_pinToCores(CPU_CORE_PERFORMANCE);

    index_log(force_rebuild ? "build_thread: force rebuild" : "build_thread: start");

    M3U_init();

    LibraryFingerprint fp;
    compute_fingerprint(&fp);

    char fp_msg[96];
    snprintf(fp_msg, sizeof(fp_msg), "build_thread: fingerprint %016llx", (unsigned long long)fp.hash);
    index_log(fp_msg);

    if (force_rebuild) {
        rebuild_index(fp.hash);
    } else {
        set_status("Loading index...");
        if (load_bin_index(fp.hash)) {
            char done[128];
            snprintf(done, sizeof(done), "Ready (%d tracks)", track_count);
            set_status(done);
            index_log("build_thread: loaded binary index");
        } else if (load_json_index(fp.hash)) {
            char done[128];
            snprintf(done, sizeof(done), "Ready (%d tracks)", track_count);
            set_status(done);
            index_log("build_thread: migrated JSON index");
        } else {
            rebuild_index(fp.hash);
        }
    }

    pthread_mutex_lock(&index_mutex);
    index_ready = true;
    build_running = false;
    pthread_mutex_unlock(&index_mutex);
    index_log("build_thread: complete");
    return NULL;
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
    set_status("Rebuilding index...");
    return start_build_thread(true);
}

void LibraryIndex_quit(void) {
    free_index_data();
    index_ready = false;
    build_started = false;
}

bool LibraryIndex_isReady(void) {
    pthread_mutex_lock(&index_mutex);
    bool ready = index_ready;
    pthread_mutex_unlock(&index_mutex);
    return ready;
}

bool LibraryIndex_isBuilding(void) {
    pthread_mutex_lock(&index_mutex);
    bool running = build_running;
    pthread_mutex_unlock(&index_mutex);
    return running;
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
        int id = tok->track_ids[i];
        if (id < 0 || id >= track_count) continue;
        if (track_hits) track_hits[id]++;
        if (track_matched_token && !track_matched_token[id]) {
            track_matched_token[id] = true;
        }
    }
    for (int i = 0; i < tok->playlist_count; i++) {
        int id = tok->playlist_ids[i];
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
    for (TokenHashNode* n = token_first_char[bucket]; n; n = n->next) {
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

static void gather_track_candidates(const char query_tokens[][48], int query_token_count,
                                    bool fuzzy, bool* track_candidate) {
    if (!track_candidate || query_token_count <= 0) return;

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
    SearchFuseFields fields = {
        .title = tr->title_norm,
        .artist = tr->artist_norm,
        .album = tr->album_norm,
        .genre = tr->genre_norm,
        .filename = tr->filename_norm
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
    IndexedTrack* tr = &tracks[id];
    row->type = SEARCH_ITEM_TRACK;
    row->score = score;
    strncpy(row->path, tr->path, sizeof(row->path) - 1);
    strncpy(row->label, tr->title[0] ? tr->title : tr->filename_norm, sizeof(row->label) - 1);

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
    if (!out || !query) return false;
    memset(out, 0, sizeof(*out));

    if (!LibraryIndex_isReady()) return false;

    pthread_mutex_lock(&index_mutex);
    if (track_count == 0 && playlist_count == 0) {
        pthread_mutex_unlock(&index_mutex);
        return false;
    }

    char query_tokens[32][48];
    QueryCollectCtx qctx = {.tokens = query_tokens, .count = 0};

    Metadata_foreachToken(query, collect_query_token, &qctx);
    int query_token_count = qctx.count;
    if (query_token_count == 0) {
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
