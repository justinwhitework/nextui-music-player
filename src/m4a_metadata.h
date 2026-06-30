#ifndef __M4A_METADATA_H__
#define __M4A_METADATA_H__

#include <stddef.h>
#include <stdint.h>

struct SDL_Surface;

typedef struct {
    char title[256];
    char artist[256];
    char album[256];
    char genre[256];
} M4ATags;

// Load embedded cover art from an M4A/AAC file. Caller must SDL_FreeSurface the result.
struct SDL_Surface* M4A_extractCoverArt(const char* filepath);

// Read text tags from M4A/MP4. Returns 0 on success (file opened), -1 on failure.
int M4A_readTags(const char* filepath, M4ATags* out);

#endif
