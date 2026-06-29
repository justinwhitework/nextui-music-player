#include "playlist_art.h"
#include "image_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#define PLAYLIST_ART_CACHE_SIZE 32
#define PLAYLIST_ART_MAX_FILE (2 * 1024 * 1024)

typedef struct {
    char m3u_path[512];
    SDL_Surface* full;
    SDL_Surface* thumb;
    int thumb_size;
} PlaylistArtCacheEntry;

static PlaylistArtCacheEntry playlist_art_cache[PLAYLIST_ART_CACHE_SIZE];
static int playlist_art_cache_count = 0;

static const char* const k_sidecar_exts[] = {".png", ".webp", ".jpg", ".jpeg", NULL};

bool PlaylistArt_findPath(const char* m3u_path, char* out, int out_size) {
    if (!m3u_path || !out || out_size <= 0) return false;

    char base[512];
    strncpy(base, m3u_path, sizeof(base) - 1);
    base[sizeof(base) - 1] = '\0';

    char* dot = strrchr(base, '.');
    if (dot) *dot = '\0';

    for (int i = 0; k_sidecar_exts[i]; i++) {
        snprintf(out, out_size, "%s%s", base, k_sidecar_exts[i]);
        if (access(out, F_OK) == 0) return true;
    }
    return false;
}

static PlaylistArtCacheEntry* find_cache_entry(const char* m3u_path) {
    for (int i = 0; i < playlist_art_cache_count; i++) {
        if (strcmp(playlist_art_cache[i].m3u_path, m3u_path) == 0) {
            return &playlist_art_cache[i];
        }
    }
    return NULL;
}

static void evict_cache_index(int index) {
    if (playlist_art_cache[index].full) {
        SDL_FreeSurface(playlist_art_cache[index].full);
    }
    if (playlist_art_cache[index].thumb) {
        SDL_FreeSurface(playlist_art_cache[index].thumb);
    }
    for (int i = index; i < playlist_art_cache_count - 1; i++) {
        playlist_art_cache[i] = playlist_art_cache[i + 1];
    }
    playlist_art_cache_count--;
}

static SDL_Surface* load_image_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0 || fsize > PLAYLIST_ART_MAX_FILE) {
        fclose(f);
        return NULL;
    }

    uint8_t* data = (uint8_t*)malloc(fsize);
    if (!data) {
        fclose(f);
        return NULL;
    }
    if ((long)fread(data, 1, fsize, f) != fsize) {
        free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);

    if (!image_is_complete(data, (int)fsize)) {
        free(data);
        return NULL;
    }

    SDL_RWops* rw = SDL_RWFromConstMem(data, fsize);
    SDL_Surface* raw = NULL;
    if (rw) raw = IMG_Load_RW(rw, 1);
    free(data);
    return raw;
}

static PlaylistArtCacheEntry* ensure_cache_entry(const char* m3u_path) {
    PlaylistArtCacheEntry* existing = find_cache_entry(m3u_path);
    if (existing) return existing;

    char art_path[512];
    if (!PlaylistArt_findPath(m3u_path, art_path, sizeof(art_path))) {
        return NULL;
    }

    SDL_Surface* full = load_image_file(art_path);
    if (!full) return NULL;

    if (playlist_art_cache_count >= PLAYLIST_ART_CACHE_SIZE) {
        evict_cache_index(0);
    }

    PlaylistArtCacheEntry* entry = &playlist_art_cache[playlist_art_cache_count++];
    memset(entry, 0, sizeof(*entry));
    strncpy(entry->m3u_path, m3u_path, sizeof(entry->m3u_path) - 1);
    entry->full = full;
    return entry;
}

void PlaylistArt_init(void) {
    playlist_art_cache_count = 0;
}

void PlaylistArt_cleanup(void) {
    PlaylistArt_clearCache();
}

void PlaylistArt_clearCache(void) {
    while (playlist_art_cache_count > 0) {
        evict_cache_index(0);
    }
}

SDL_Surface* PlaylistArt_get(const char* m3u_path) {
    if (!m3u_path || !m3u_path[0]) return NULL;
    PlaylistArtCacheEntry* entry = ensure_cache_entry(m3u_path);
    return entry ? entry->full : NULL;
}

SDL_Surface* PlaylistArt_getThumbnail(const char* m3u_path, int size) {
    if (!m3u_path || !m3u_path[0] || size <= 0) return NULL;

    PlaylistArtCacheEntry* entry = ensure_cache_entry(m3u_path);
    if (!entry || !entry->full) return NULL;

    if (entry->thumb && entry->thumb_size == size) {
        return entry->thumb;
    }

    if (entry->thumb) {
        SDL_FreeSurface(entry->thumb);
        entry->thumb = NULL;
    }

    entry->thumb = image_scale_to_square(entry->full, size);
    entry->thumb_size = size;
    return entry->thumb;
}
