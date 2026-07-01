#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "defines.h"
#include "api.h"
#include "module_common.h"
#include "module_search.h"
#include "module_player.h"
#include "library_index.h"
#include "playlist_m3u.h"
#include "playlist.h"
#include "keyboard.h"
#include "display_helper.h"
#include "search_history.h"
#include "ui_search.h"
#include "ui_utils.h"
#include "settings.h"
#include "track_art.h"

typedef enum {
    SEARCH_STATE_BUILDING,
    SEARCH_STATE_FAILED,
    SEARCH_STATE_QUERY,
    SEARCH_STATE_RESULTS,
    SEARCH_STATE_DETAIL,
    SEARCH_STATE_REBUILDING
} SearchState;

#define SEARCH_BUILD_HELP  56
#define SEARCH_QUERY_HELP  57
#define SEARCH_RESULTS_HELP 58
#define SEARCH_DETAIL_HELP 59

static char search_query[256] = "";
static SearchResults search_results;
static char search_toast[128] = "";
static uint32_t search_toast_time = 0;

static PlaylistTrack detail_tracks[PLAYLIST_MAX_TRACKS];
static int detail_track_count = 0;
static int detail_selected = 0;
static int detail_scroll = 0;
static char detail_title[256] = "";
static char detail_m3u_path[512] = "";
static bool detail_is_single_track = false;
static bool show_index_log = false;
static int index_log_scroll = 0;
static bool index_log_user_scrolled = false;

static void index_log_scroll_to_end(SDL_Surface* screen, int* scroll) {
    int total = LibraryIndex_getBuildLogCount();
    int lines_per_page = calc_index_build_log_lines_per_page(screen);
    int end = total - lines_per_page;
    *scroll = end > 0 ? end : 0;
}

static bool handle_index_build_input(SDL_Surface* screen, bool building) {
    if (!building) return false;

    if (PAD_justPressed(BTN_Y)) {
        show_index_log = !show_index_log;
        if (show_index_log) {
            index_log_user_scrolled = false;
            index_log_scroll_to_end(screen, &index_log_scroll);
        }
        return true;
    }

    if (!show_index_log) return false;

    if (!index_log_user_scrolled) {
        index_log_scroll_to_end(screen, &index_log_scroll);
    }

    int total = LibraryIndex_getBuildLogCount();
    int lines_per_page = calc_index_build_log_lines_per_page(screen);
    if (PAD_justRepeated(BTN_UP) && index_log_scroll > 0) {
        index_log_user_scrolled = true;
        index_log_scroll--;
        return true;
    }
    if (PAD_justRepeated(BTN_DOWN) && index_log_scroll < total - lines_per_page) {
        index_log_user_scrolled = true;
        index_log_scroll++;
        return true;
    }
    return false;
}

static bool results_index_is_header(const SearchResults* results, int idx) {
    return results && results->nested_count > 0 && idx == 0;
}

static int results_first_selectable(const SearchResults* results) {
    return (results && results->nested_count > 0) ? 1 : 0;
}

static void results_clamp_selection(const SearchResults* results, int* selected) {
    int total = search_results_total_count(results);
    int first = results_first_selectable(results);
    if (total <= first) {
        *selected = 0;
        return;
    }
    if (*selected < first) *selected = first;
    if (*selected >= total) *selected = total - 1;
}

static void results_move_selection(const SearchResults* results, int* selected, int delta) {
    int total = search_results_total_count(results);
    int first = results_first_selectable(results);
    if (total <= first) return;

    int idx = *selected;
    do {
        idx += delta;
        if (idx < first) idx = total - 1;
        if (idx >= total) idx = first;
    } while (results_index_is_header(results, idx));

    *selected = idx;
}

static void show_toast(const char* msg) {
    snprintf(search_toast, sizeof(search_toast), "%s", msg ? msg : "");
    search_toast_time = SDL_GetTicks();
}

static void open_keyboard(SDL_Surface** screen, char* out_query, int out_size) {
    DisplayHelper_prepareForExternal();
    char* result = Keyboard_open("Search library");
    PAD_poll();
    PAD_reset();
    DisplayHelper_recoverDisplay();
    {
        SDL_Surface* ns = DisplayHelper_getReinitScreen();
        if (ns) *screen = ns;
    }
    SDL_Delay(100);
    PAD_poll();
    PAD_reset();

    if (result && result[0]) {
        strncpy(out_query, result, out_size - 1);
        out_query[out_size - 1] = '\0';
    }
    if (result) free(result);
}

static bool run_search_query(const char* query, SearchState* state,
                             int* results_selected, int* results_scroll) {
    if (!query || !query[0]) return false;

    if (LibraryIndex_isFailed()) {
        show_toast("Search unavailable");
        return false;
    }
    if (LibraryIndex_isBuilding()) {
        show_toast("Index still building");
        return false;
    }
    if (!LibraryIndex_canSearch()) {
        show_toast("Search not ready");
        return false;
    }

    strncpy(search_query, query, sizeof(search_query) - 1);
    search_query[sizeof(search_query) - 1] = '\0';

    if (LibraryIndex_search(search_query, &search_results)) {
        SearchResultRow top;
        const SearchResultRow* top_ptr = NULL;
        if (search_results_top_result(&search_results, &top)) {
            top_ptr = &top;
        }
        SearchHistory_add(search_query, top_ptr);

        *results_selected = results_first_selectable(&search_results);
        *results_scroll = 0;
        *state = SEARCH_STATE_RESULTS;
        return true;
    }

    show_toast("No matches found");
    return false;
}

static void load_detail_from_row(const SearchResultRow* row) {
    detail_track_count = 0;
    detail_selected = 0;
    detail_scroll = 0;
    detail_m3u_path[0] = '\0';
    detail_is_single_track = false;

    if (!row) return;

    if (row->type == SEARCH_ITEM_TRACK) {
        detail_is_single_track = true;
        strncpy(detail_title, row->label, sizeof(detail_title) - 1);
        PlaylistTrack* tr = &detail_tracks[0];
        strncpy(tr->path, row->path, sizeof(tr->path) - 1);
        strncpy(tr->name, row->label, sizeof(tr->name) - 1);
        tr->format = row->format;
        detail_track_count = 1;
        return;
    }

    strncpy(detail_m3u_path, row->path, sizeof(detail_m3u_path) - 1);
    snprintf(detail_title, sizeof(detail_title), "%s", row->label);
    M3U_loadTracks(row->path, detail_tracks, PLAYLIST_MAX_TRACKS, &detail_track_count);
}

static void go_search_home(SearchState* state, int* home_selected, int* home_scroll) {
    *state = SEARCH_STATE_QUERY;
    *home_selected = 0;
    *home_scroll = 0;
}

static ModuleExitReason play_track_from_row(SDL_Surface** screen, const SearchResultRow* row) {
    if (!row || row->type != SEARCH_ITEM_TRACK) return MODULE_EXIT_TO_MENU;

    PlaylistTrack tracks[1];
    memset(tracks, 0, sizeof(tracks));
    strncpy(tracks[0].path, row->path, sizeof(tracks[0].path) - 1);
    strncpy(tracks[0].name, row->label, sizeof(tracks[0].name) - 1);
    tracks[0].format = row->format;

    PlayerModule_setResumePlaylistPath(NULL);
    ModuleExitReason reason = PlayerModule_runWithPlaylist(*screen, tracks, 1, 0);
    {
        SDL_Surface* ns = DisplayHelper_getReinitScreen();
        if (ns) *screen = ns;
    }
    return reason;
}

static void activate_home_row(SDL_Surface** screen, int index, SearchState* state,
                              int* home_selected, int* home_scroll,
                              int* results_selected, int* results_scroll,
                              bool* show_clear_confirm) {
    switch (search_home_item_type(index)) {
        case SEARCH_HOME_ITEM_SEARCH:
            open_keyboard(screen, search_query, sizeof(search_query));
            run_search_query(search_query, state, results_selected, results_scroll);
            break;
        case SEARCH_HOME_ITEM_HISTORY: {
            const char* q = SearchHistory_get(index - 1);
            if (q) run_search_query(q, state, results_selected, results_scroll);
            break;
        }
        case SEARCH_HOME_ITEM_CLEAR:
            *show_clear_confirm = true;
            break;
        case SEARCH_HOME_ITEM_REBUILD:
            if (LibraryIndex_requestRebuild()) {
                *state = SEARCH_STATE_REBUILDING;
            } else {
                show_toast("Index rebuild already running");
            }
            break;
    }
    (void)home_selected;
    (void)home_scroll;
}

ModuleExitReason SearchModule_run(SDL_Surface* screen) {
    Keyboard_init();
    M3U_init();
    SearchHistory_init();

    SearchState state;
    if (LibraryIndex_isFailed()) {
        state = SEARCH_STATE_FAILED;
    } else {
        state = LibraryIndex_isReady() ? SEARCH_STATE_QUERY : SEARCH_STATE_BUILDING;
    }
    int dirty = 1;
    int show_setting = 0;
    int home_selected = 0;
    int home_scroll = 0;
    int results_selected = 0;
    int results_scroll = 0;
    bool show_clear_confirm = false;

    memset(&search_results, 0, sizeof(search_results));
    search_toast[0] = '\0';
    search_query[0] = '\0';

    while (1) {
        GFX_startFrame();
        PAD_poll();

        if (show_clear_confirm) {
            if (PAD_justPressed(BTN_A)) {
                SearchHistory_clear();
                if (home_selected >= search_home_item_count()) {
                    home_selected = search_home_item_count() - 1;
                }
                if (home_selected < 0) home_selected = 0;
                show_clear_confirm = false;
                dirty = 1;
                continue;
            }
            if (PAD_justPressed(BTN_B)) {
                show_clear_confirm = false;
                dirty = 1;
                continue;
            }
            render_search_home(screen, show_setting, home_selected, home_scroll);
            render_confirmation_dialog(screen, NULL, "Clear search history?");
            GFX_flip(screen);
            GFX_sync();
            continue;
        }

        int help_state;
        switch (state) {
            case SEARCH_STATE_BUILDING: help_state = SEARCH_BUILD_HELP; break;
            case SEARCH_STATE_FAILED: help_state = SEARCH_BUILD_HELP; break;
            case SEARCH_STATE_QUERY: help_state = SEARCH_QUERY_HELP; break;
            case SEARCH_STATE_RESULTS: help_state = SEARCH_RESULTS_HELP; break;
            case SEARCH_STATE_REBUILDING: help_state = SEARCH_QUERY_HELP; break;
            default: help_state = SEARCH_DETAIL_HELP; break;
        }

        GlobalInputResult global = ModuleCommon_handleGlobalInput(screen, &show_setting, help_state);
        if (global.should_quit) return MODULE_EXIT_QUIT;
        if (global.input_consumed) {
            if (global.dirty) dirty = 1;
            GFX_sync();
            continue;
        }

        if (state == SEARCH_STATE_BUILDING || state == SEARCH_STATE_FAILED) {
            dirty = 1;
            if (handle_index_build_input(screen, true)) dirty = 1;
            if (state == SEARCH_STATE_FAILED && PAD_justPressed(BTN_A)) {
                if (LibraryIndex_requestRebuild()) {
                    state = SEARCH_STATE_REBUILDING;
                    show_index_log = false;
                    dirty = 1;
                } else {
                    show_toast("Index rebuild already running");
                }
            }
            if (LibraryIndex_isFailed()) {
                state = SEARCH_STATE_FAILED;
            } else if (LibraryIndex_isReady()) {
                state = SEARCH_STATE_QUERY;
                show_index_log = false;
                dirty = 1;
            }
            if (PAD_justPressed(BTN_B)) return MODULE_EXIT_TO_MENU;
        }
        else if (state == SEARCH_STATE_REBUILDING) {
            dirty = 1;
            if (handle_index_build_input(screen, true)) dirty = 1;
            if (LibraryIndex_isReady() && !LibraryIndex_isBuilding()) {
                state = SEARCH_STATE_QUERY;
                show_index_log = false;
                show_toast("Index rebuild complete");
                dirty = 1;
            }
            if (PAD_justPressed(BTN_B)) {
                state = SEARCH_STATE_QUERY;
                show_index_log = false;
                dirty = 1;
            }
        }
        else if (state == SEARCH_STATE_QUERY) {
            int total = search_home_item_count();
            int items_per_page = calc_list_layout(screen).items_per_page;

            if (PAD_justRepeated(BTN_UP) && total > 0) {
                home_selected = (home_selected > 0) ? home_selected - 1 : total - 1;
                dirty = 1;
            }
            else if (PAD_justRepeated(BTN_DOWN) && total > 0) {
                home_selected = (home_selected < total - 1) ? home_selected + 1 : 0;
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_LEFT)) {
                if (list_page_up(&home_selected, &home_scroll, total, items_per_page))
                    dirty = 1;
            }
            else if (PAD_justPressed(BTN_RIGHT)) {
                if (list_page_down(&home_selected, &home_scroll, total, items_per_page))
                    dirty = 1;
            }
            else if (PAD_justPressed(BTN_A) && total > 0) {
                activate_home_row(&screen, home_selected, &state, &home_selected, &home_scroll,
                                  &results_selected, &results_scroll, &show_clear_confirm);
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_Y)) {
                open_keyboard(&screen, search_query, sizeof(search_query));
                run_search_query(search_query, &state, &results_selected, &results_scroll);
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_B)) {
                return MODULE_EXIT_TO_MENU;
            }
        }
        else if (state == SEARCH_STATE_RESULTS) {
            int total = search_results_total_count(&search_results);
            int items_per_page = calc_list_layout(screen).items_per_page;

            if (PAD_justRepeated(BTN_UP) && total > 0) {
                results_move_selection(&search_results, &results_selected, -1);
                dirty = 1;
            }
            else if (PAD_justRepeated(BTN_DOWN) && total > 0) {
                results_move_selection(&search_results, &results_selected, 1);
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_LEFT)) {
                if (list_page_up(&results_selected, &results_scroll, total, items_per_page)) {
                    results_clamp_selection(&search_results, &results_selected);
                    dirty = 1;
                }
            }
            else if (PAD_justPressed(BTN_RIGHT)) {
                if (list_page_down(&results_selected, &results_scroll, total, items_per_page)) {
                    results_clamp_selection(&search_results, &results_selected);
                    dirty = 1;
                }
            }
            else if (PAD_justPressed(BTN_Y)) {
                go_search_home(&state, &home_selected, &home_scroll);
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_A) && total > 0) {
                if (!results_index_is_header(&search_results, results_selected)) {
                    SearchResultRow row;
                    if (search_result_at(&search_results, results_selected, &row)) {
                        if (row.type == SEARCH_ITEM_TRACK) {
                            ModuleExitReason reason = play_track_from_row(&screen, &row);
                            if (reason == MODULE_EXIT_QUIT) return MODULE_EXIT_QUIT;
                        } else {
                            load_detail_from_row(&row);
                            state = SEARCH_STATE_DETAIL;
                        }
                        dirty = 1;
                    }
                }
            }
            else if (PAD_justPressed(BTN_B)) {
                go_search_home(&state, &home_selected, &home_scroll);
                dirty = 1;
            }
        }
        else if (state == SEARCH_STATE_DETAIL) {
            int items_per_page = calc_list_layout(screen).items_per_page;

            if (PAD_justRepeated(BTN_UP) && detail_track_count > 0) {
                detail_selected = (detail_selected > 0) ? detail_selected - 1 : detail_track_count - 1;
                dirty = 1;
            }
            else if (PAD_justRepeated(BTN_DOWN) && detail_track_count > 0) {
                detail_selected = (detail_selected < detail_track_count - 1) ? detail_selected + 1 : 0;
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_LEFT)) {
                if (list_page_up(&detail_selected, &detail_scroll, detail_track_count, items_per_page))
                    dirty = 1;
            }
            else if (PAD_justPressed(BTN_RIGHT)) {
                if (list_page_down(&detail_selected, &detail_scroll, detail_track_count, items_per_page))
                    dirty = 1;
            }
            else if (PAD_justPressed(BTN_Y)) {
                go_search_home(&state, &home_selected, &home_scroll);
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_A) && detail_track_count > 0) {
                if (!detail_is_single_track && detail_m3u_path[0]) {
                    PlayerModule_setResumePlaylistPath(detail_m3u_path);
                } else {
                    PlayerModule_setResumePlaylistPath(NULL);
                }
                ModuleExitReason reason = PlayerModule_runWithPlaylist(
                    screen, detail_tracks, detail_track_count, detail_selected);
                if (reason == MODULE_EXIT_QUIT) return MODULE_EXIT_QUIT;
                {
                    SDL_Surface* ns = DisplayHelper_getReinitScreen();
                    if (ns) screen = ns;
                }
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_B)) {
                state = SEARCH_STATE_RESULTS;
                dirty = 1;
            }
        }

        ModuleCommon_PWR_update(&dirty, &show_setting);

        if ((state == SEARCH_STATE_QUERY || state == SEARCH_STATE_RESULTS ||
             state == SEARCH_STATE_DETAIL) &&
            Settings_getTooltipArtwork()) {
            TrackArt_tick();
            if (TrackArt_hasPendingWork()) dirty = 1;
        }

        if (dirty) {
            switch (state) {
                case SEARCH_STATE_BUILDING:
                    render_search_building(screen, show_setting, LibraryIndex_getBuildStatus(),
                                           show_index_log, index_log_scroll);
                    break;
                case SEARCH_STATE_FAILED:
                    render_search_building(screen, show_setting, LibraryIndex_getBuildStatus(),
                                           show_index_log, index_log_scroll);
                    break;
                case SEARCH_STATE_QUERY: {
                    int items_per_page = calc_list_layout(screen).items_per_page;
                    adjust_list_scroll(home_selected, &home_scroll, items_per_page);
                    render_search_home(screen, show_setting, home_selected, home_scroll);
                    break;
                }
                case SEARCH_STATE_RESULTS: {
                    int items_per_page = calc_list_layout(screen).items_per_page;
                    adjust_list_scroll(results_selected, &results_scroll, items_per_page);
                    render_search_results(screen, show_setting, search_query,
                                          &search_results, results_selected, results_scroll);
                    break;
                }
                case SEARCH_STATE_DETAIL: {
                    int items_per_page = calc_list_layout(screen).items_per_page;
                    adjust_list_scroll(detail_selected, &detail_scroll, items_per_page);
                    render_search_detail(screen, show_setting, detail_title,
                                         detail_m3u_path[0] ? detail_m3u_path : NULL,
                                         detail_tracks, detail_track_count,
                                         detail_selected, detail_scroll);
                    break;
                }
                case SEARCH_STATE_REBUILDING:
                    render_search_rebuilding(screen, show_setting, LibraryIndex_getBuildStatus(),
                                             show_index_log, index_log_scroll);
                    break;
            }

            if (show_setting) GFX_blitHardwareHints(screen, show_setting);
            render_toast(screen, search_toast, search_toast_time);
            GFX_flip(screen);
            dirty = 0;
            ModuleCommon_tickToast(search_toast, search_toast_time, &dirty);
        } else {
            GFX_sync();
        }
    }
}
