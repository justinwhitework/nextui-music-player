#ifndef __PLAYLIST_M3U_H__
#define __PLAYLIST_M3U_H__

#include <stdbool.h>
#include "playlist.h"  // For PlaylistTrack

#define PLAYLISTS_DIR  SHARED_USERDATA_PATH "/music-player/playlists"
#define MAX_PLAYLISTS_CAP 16384
#define DEFAULT_MAX_PLAYLISTS 50
#define DEFAULT_PLAYLIST_SCAN_DEPTH 4
#define MAX_PLAYLIST_SCAN_DEPTH_CAP 16
#define MAX_PLAYLIST_NAME 128

typedef struct {
    char name[MAX_PLAYLIST_NAME];  // Display name (without .m3u)
    char path[512];                // Full path to .m3u file or folder
    int track_count;               // Number of tracks (playlists only)
    bool is_folder;                // true = subfolder entry
} PlaylistInfo;

// Create playlists directory if it doesn't exist
void M3U_init(void);

// List folders then playlists in a single directory (non-recursive).
int M3U_listDirectory(const char* dir, PlaylistInfo* out, int max, int max_scan_depth);

// Recursively collect all playlists up to max_scan_depth folder nesting.
int M3U_listAllPlaylists(PlaylistInfo* out, int max, int max_scan_depth);

// For Add-to-Playlist: flat list, root playlists first, then nested by path.
int M3U_listPlaylistsForPicker(PlaylistInfo* out, int max, int max_scan_depth);

// Label for picker rows: "Name (n)" or "Folder / Name (n)".
void M3U_formatPickerLabel(const PlaylistInfo* info, char* out, int out_size);

// Depth of dir relative to PLAYLISTS_DIR (0 = root).
int M3U_dirDepth(const char* dir);

// True when dir is the playlists root.
bool M3U_isPlaylistsRoot(const char* dir);

// Parent of dir into out (PLAYLISTS_DIR if already at root). Returns 0 on success.
int M3U_parentDirectory(const char* dir, char* out, int out_size);

// Create an empty .m3u in PLAYLISTS_DIR. Returns 0 on success.
int M3U_create(const char* name);

// Create an empty .m3u in dir. Returns 0 on success.
int M3U_createAt(const char* dir, const char* name);

// Delete a playlist file. Returns 0 on success.
int M3U_delete(const char* m3u_path);

// Append a track to an .m3u file. Returns 0 on success.
int M3U_addTrack(const char* m3u_path, const char* track_path, const char* display_name);

// Rewrite the .m3u file without the track at the given index. Returns 0 on success.
int M3U_removeTrack(const char* m3u_path, int index);

// Load tracks from an .m3u file into a PlaylistTrack array.
// Skips missing files. Sets *count to number loaded. Returns 0 on success.
int M3U_loadTracks(const char* m3u_path, PlaylistTrack* tracks, int max, int* count);

// Check if a track path is already in the .m3u file.
bool M3U_containsTrack(const char* m3u_path, const char* track_path);

// True when .m3u lives directly in PLAYLISTS_DIR (user playlist).
bool M3U_isRootPlaylist(const char* m3u_path);

// Parent folder name relative to PLAYLISTS_DIR (e.g. "genres"), empty for root playlists.
void M3U_getPlaylistParentFolder(const char* m3u_path, char* out, int out_size);

#endif
