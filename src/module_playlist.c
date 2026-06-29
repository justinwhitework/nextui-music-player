#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "defines.h"
#include "api.h"
#include "module_common.h"
#include "module_playlist.h"
#include "module_player.h"
#include "playlist_m3u.h"
#include "playlist.h"
#include "keyboard.h"
#include "display_helper.h"
#include "ui_playlist.h"
#include "ui_main.h"
#include "ui_utils.h"
#include "ui_album_art.h"
#include "track_art.h"
#include "settings.h"

typedef enum {
    PLAYLIST_INTERNAL_LIST,
    PLAYLIST_INTERNAL_DETAIL
} PlaylistInternalState;

static PlaylistInfo* playlists = NULL;
static int playlist_count = 0;
static char list_dir_path[512];
static int list_selected = 0;
static int list_scroll = 0;

static PlaylistTrack detail_tracks[PLAYLIST_MAX_TRACKS];
static int detail_track_count = 0;
static int detail_selected = 0;
static int detail_scroll = 0;
static int current_playlist_index = -1;

static char playlist_toast_message[128] = "";
static uint32_t playlist_toast_time = 0;

static bool show_confirm = false;
static char confirm_name[256] = "";
static int confirm_action = 0;
static int confirm_target = -1;

#define PLAYLIST_LIST_HELP_STATE   50
#define PLAYLIST_DETAIL_HELP_STATE 51

static void get_list_dir_title(char* out, int out_size) {
    if (M3U_isPlaylistsRoot(list_dir_path)) {
        snprintf(out, out_size, "Playlists");
        return;
    }
    const char* name = strrchr(list_dir_path, '/');
    snprintf(out, out_size, "%s", name ? name + 1 : list_dir_path);
}

static void refresh_playlists(void) {
    if (playlists) {
        free(playlists);
        playlists = NULL;
    }

    int limit = Settings_getMaxPlaylists();
    playlists = calloc(limit, sizeof(PlaylistInfo));
    if (!playlists) {
        playlist_count = 0;
        return;
    }

    playlist_count = M3U_listDirectory(
        list_dir_path, playlists, limit, Settings_getPlaylistScanDepth());
}

static void refresh_detail(void) {
    if (current_playlist_index < 0 || current_playlist_index >= playlist_count) return;
    if (playlists[current_playlist_index].is_folder) return;
    M3U_loadTracks(playlists[current_playlist_index].path, detail_tracks, PLAYLIST_MAX_TRACKS, &detail_track_count);
}

static void show_toast(const char* msg) {
    snprintf(playlist_toast_message, sizeof(playlist_toast_message), "%s", msg);
    playlist_toast_time = SDL_GetTicks();
}

static void navigate_up_dir(void) {
    char parent[512];
    if (M3U_parentDirectory(list_dir_path, parent, sizeof(parent)) != 0) return;
    strncpy(list_dir_path, parent, sizeof(list_dir_path) - 1);
    list_dir_path[sizeof(list_dir_path) - 1] = '\0';
    refresh_playlists();
    list_selected = 0;
    list_scroll = 0;
}

ModuleExitReason PlaylistModule_run(SDL_Surface* screen) {
    M3U_init();
    Keyboard_init();
    snprintf(list_dir_path, sizeof(list_dir_path), "%s", PLAYLISTS_DIR);
    refresh_playlists();

    PlaylistInternalState state = PLAYLIST_INTERNAL_LIST;
    int dirty = 1;
    int show_setting = 0;

    while (1) {
        GFX_startFrame();
        PAD_poll();

        if (show_confirm) {
            if (PAD_justPressed(BTN_A)) {
                if (confirm_action == 0) {
                    int idx = confirm_target;
                    if (idx >= 0 && idx < playlist_count && !playlists[idx].is_folder) {
                        M3U_delete(playlists[idx].path);
                        refresh_playlists();
                        if (list_selected >= playlist_count) list_selected = playlist_count - 1;
                        if (list_selected < 0) list_selected = 0;
                        show_toast("Playlist deleted");
                    }
                } else if (confirm_action == 1) {
                    int idx = confirm_target;
                    if (current_playlist_index >= 0 && current_playlist_index < playlist_count) {
                        M3U_removeTrack(playlists[current_playlist_index].path, idx);
                        refresh_detail();
                        playlists[current_playlist_index].track_count = detail_track_count;
                        if (detail_selected >= detail_track_count) detail_selected = detail_track_count - 1;
                        if (detail_selected < 0) detail_selected = 0;
                        show_toast("Track removed");
                    }
                }
                show_confirm = false;
                dirty = 1;
                continue;
            }
            if (PAD_justPressed(BTN_B)) {
                show_confirm = false;
                dirty = 1;
                continue;
            }
            const char* confirm_title = (confirm_action == 0) ? "Delete Playlist?" : "Remove Track?";
            render_confirmation_dialog(screen, confirm_name, confirm_title);
            GFX_flip(screen);
            GFX_sync();
            continue;
        }

        int app_state_for_help = (state == PLAYLIST_INTERNAL_LIST) ? PLAYLIST_LIST_HELP_STATE : PLAYLIST_DETAIL_HELP_STATE;
        GlobalInputResult global = ModuleCommon_handleGlobalInput(screen, &show_setting, app_state_for_help);
        if (global.should_quit) {
            free(playlists);
            playlists = NULL;
            return MODULE_EXIT_QUIT;
        }
        if (global.input_consumed) {
            if (global.dirty) dirty = 1;
            GFX_sync();
            continue;
        }

        if (state == PLAYLIST_INTERNAL_LIST) {
            int total_items = playlist_count;
            int items_per_page = calc_list_layout(screen).items_per_page;

            if (PAD_justPressed(BTN_B)) {
                GFX_clearLayers(LAYER_SCROLLTEXT);
                if (!M3U_isPlaylistsRoot(list_dir_path)) {
                    navigate_up_dir();
                    dirty = 1;
                } else {
                    free(playlists);
                    playlists = NULL;
                    return MODULE_EXIT_TO_MENU;
                }
            }
            else if (total_items > 0 && PAD_justRepeated(BTN_UP)) {
                list_selected = (list_selected > 0) ? list_selected - 1 : total_items - 1;
                dirty = 1;
            }
            else if (total_items > 0 && PAD_justRepeated(BTN_DOWN)) {
                list_selected = (list_selected < total_items - 1) ? list_selected + 1 : 0;
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_LEFT)) {
                list_page_up(&list_selected, &list_scroll, playlist_count, items_per_page);
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_RIGHT)) {
                list_page_down(&list_selected, &list_scroll, playlist_count, items_per_page);
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_A)) {
                if (list_selected >= 0 && list_selected < playlist_count) {
                    if (playlists[list_selected].is_folder) {
                        strncpy(list_dir_path, playlists[list_selected].path, sizeof(list_dir_path) - 1);
                        list_dir_path[sizeof(list_dir_path) - 1] = '\0';
                        refresh_playlists();
                        list_selected = 0;
                        list_scroll = 0;
                        GFX_clearLayers(LAYER_SCROLLTEXT);
                        dirty = 1;
                    } else {
                        current_playlist_index = list_selected;
                        refresh_detail();
                        detail_selected = 0;
                        detail_scroll = 0;
                        state = PLAYLIST_INTERNAL_DETAIL;
                        GFX_clearLayers(LAYER_SCROLLTEXT);
                        dirty = 1;
                    }
                }
            }
            else if (PAD_justPressed(BTN_Y)) {
                DisplayHelper_prepareForExternal();
                char* name = Keyboard_open("Playlist name");
                PAD_poll(); PAD_reset();
                DisplayHelper_recoverDisplay();
                {
                    SDL_Surface* ns = DisplayHelper_getReinitScreen();
                    if (ns) screen = ns;
                }
                if (name && name[0]) {
                    if (M3U_createAt(list_dir_path, name) == 0) {
                        show_toast("Playlist created");
                        refresh_playlists();
                    } else {
                        show_toast("Already exists");
                    }
                    free(name);
                } else if (name) {
                    free(name);
                }
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_X)) {
                if (list_selected >= 0 && list_selected < playlist_count &&
                    !playlists[list_selected].is_folder) {
                    snprintf(confirm_name, sizeof(confirm_name), "%s", playlists[list_selected].name);
                    confirm_action = 0;
                    confirm_target = list_selected;
                    show_confirm = true;
                    GFX_clearLayers(LAYER_SCROLLTEXT);
                    dirty = 1;
                }
            }

            if (playlist_list_needs_scroll_refresh()) {
                playlist_list_animate_scroll();
            }
            if (playlist_list_scroll_needs_render()) dirty = 1;

        } else if (state == PLAYLIST_INTERNAL_DETAIL) {
            int total_items = detail_track_count;
            int items_per_page = calc_list_layout(screen).items_per_page;

            if (PAD_justPressed(BTN_B)) {
                GFX_clearLayers(LAYER_SCROLLTEXT);
                cleanup_album_art_background();
                refresh_playlists();
                state = PLAYLIST_INTERNAL_LIST;
                dirty = 1;
            }
            else if (total_items > 0 && PAD_justRepeated(BTN_UP)) {
                detail_selected = (detail_selected > 0) ? detail_selected - 1 : total_items - 1;
                dirty = 1;
            }
            else if (total_items > 0 && PAD_justRepeated(BTN_DOWN)) {
                detail_selected = (detail_selected < total_items - 1) ? detail_selected + 1 : 0;
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_LEFT)) {
                list_page_up(&detail_selected, &detail_scroll, detail_track_count, items_per_page);
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_RIGHT)) {
                list_page_down(&detail_selected, &detail_scroll, detail_track_count, items_per_page);
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_A)) {
                if (detail_track_count > 0) {
                    GFX_clearLayers(LAYER_SCROLLTEXT);
                    PlayerModule_setResumePlaylistPath(playlists[current_playlist_index].path);
                    PlayerModule_runWithPlaylist(screen, detail_tracks, detail_track_count, detail_selected);
                    PlayerModule_setResumePlaylistPath(NULL);
                    refresh_detail();
                    if (detail_selected >= detail_track_count) detail_selected = detail_track_count - 1;
                    if (detail_selected < 0) detail_selected = 0;
                    dirty = 1;
                }
            }
            else if (PAD_justPressed(BTN_X)) {
                if (detail_selected >= 0 && detail_selected < detail_track_count) {
                    snprintf(confirm_name, sizeof(confirm_name), "%s", detail_tracks[detail_selected].name);
                    confirm_action = 1;
                    confirm_target = detail_selected;
                    show_confirm = true;
                    GFX_clearLayers(LAYER_SCROLLTEXT);
                    dirty = 1;
                }
            }

            if (playlist_list_needs_scroll_refresh()) {
                playlist_list_animate_scroll();
            }
            if (playlist_list_scroll_needs_render()) dirty = 1;

            if (Settings_getTooltipArtwork()) {
                TrackArt_tick();
                if (TrackArt_hasPendingWork()) dirty = 1;
            }
        }

        ModuleCommon_PWR_update(&dirty, &show_setting);

        if (dirty) {
            if (state == PLAYLIST_INTERNAL_DETAIL &&
                (current_playlist_index < 0 || current_playlist_index >= playlist_count ||
                 playlists[current_playlist_index].is_folder)) {
                cleanup_album_art_background();
                state = PLAYLIST_INTERNAL_LIST;
            }

            if (state == PLAYLIST_INTERNAL_LIST) {
                int items_per_page = calc_list_layout(screen).items_per_page;
                adjust_list_scroll(list_selected, &list_scroll, items_per_page);
                char dir_title[128];
                get_list_dir_title(dir_title, sizeof(dir_title));
                render_playlist_list(screen, show_setting, dir_title,
                                     playlists, playlist_count, list_selected, list_scroll);
            } else {
                int items_per_page = calc_list_layout(screen).items_per_page;
                adjust_list_scroll(detail_selected, &detail_scroll, items_per_page);
                render_playlist_detail(screen, show_setting, playlists[current_playlist_index].name,
                                       playlists[current_playlist_index].path,
                                       detail_tracks, detail_track_count, detail_selected, detail_scroll);
            }

            render_toast(screen, playlist_toast_message, playlist_toast_time);

            if (show_setting) {
                GFX_blitHardwareHints(screen, show_setting);
            }

            GFX_flip(screen);
            dirty = 0;

            ModuleCommon_tickToast(playlist_toast_message, playlist_toast_time, &dirty);
        } else {
            GFX_sync();
        }
    }
}
