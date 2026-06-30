#ifndef __TRACK_ART_H__
#define __TRACK_ART_H__

#include <stdbool.h>

struct SDL_Surface;

void TrackArt_init(void);
void TrackArt_cleanup(void);

// Cached thumbnail (size must match prior request), or NULL if not loaded yet
struct SDL_Surface* TrackArt_getThumbnail(const char* filepath, int size);

// Queue filepath for background loading (deduplicated)
void TrackArt_request(const char* filepath);

// Process one queued load per call
void TrackArt_tick(void);

void TrackArt_clearCache(void);

// True when a load is pending or in progress
bool TrackArt_hasPendingWork(void);

// Sidecar image beside audio file (track.png next to track.mp3).
bool TrackArt_findSidecarPath(const char* filepath, char* out, int out_size);

// Folder-level art in album directory (folder.png, cover.webp, etc.).
bool TrackArt_findDirArtPath(const char* dir, char* out, int out_size);

// Thumbnail for search history rows: embedded tags, then sidecar, then folder art.
struct SDL_Surface* TrackArt_getHistoryThumbnail(const char* item_path, const char* art_dir,
                                                 int size);

#endif
