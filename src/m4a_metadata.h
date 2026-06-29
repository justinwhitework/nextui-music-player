#ifndef __M4A_METADATA_H__
#define __M4A_METADATA_H__

#include <stddef.h>
#include <stdint.h>

struct SDL_Surface;

// Load embedded cover art from an M4A/AAC file. Caller must SDL_FreeSurface the result.
struct SDL_Surface* M4A_extractCoverArt(const char* filepath);

#endif
