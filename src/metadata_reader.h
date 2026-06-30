#ifndef __METADATA_READER_H__
#define __METADATA_READER_H__

#include <stdbool.h>

typedef struct {
    char title[256];
    char artist[256];
    char album[256];
    char genre[256];
} TrackMetadata;

// Trim, lowercase ASCII copy into out (max out_size).
void Metadata_copyNormalized(char* dest, int dest_size, const char* src);

// Invoke callback for each normalized token from text (min length 2).
typedef void (*MetadataTokenFn)(const char* token, void* userdata);
void Metadata_foreachToken(const char* text, MetadataTokenFn fn, void* userdata);

// Fill title from filename (strip extension).
void Metadata_titleFromFilename(const char* filepath, char* title, int title_size);

// Read tags from audio file; missing fields fall back to filename for title.
void Metadata_readFromFile(const char* filepath, TrackMetadata* out);

#endif
