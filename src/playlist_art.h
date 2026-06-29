#ifndef __PLAYLIST_ART_H__
#define __PLAYLIST_ART_H__

#include <stdbool.h>

struct SDL_Surface;

void PlaylistArt_init(void);
void PlaylistArt_cleanup(void);

// Find sidecar image beside .m3u (same basename): .png, .webp, .jpg
bool PlaylistArt_findPath(const char* m3u_path, char* out, int out_size);

// Full-size artwork surface (cached), or NULL
struct SDL_Surface* PlaylistArt_get(const char* m3u_path);

// Pre-scaled thumbnail for list rows, or NULL
struct SDL_Surface* PlaylistArt_getThumbnail(const char* m3u_path, int size);

void PlaylistArt_clearCache(void);

#endif
