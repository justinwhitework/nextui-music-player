#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

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
#include "ui_search.h"
#include "ui_utils.h"

typedef enum {
    SEARCH_STATE_BUILDING,
    SEARCH_STATE_QUERY,
    SEARCH_STATE_SEARCHING,
    SEARCH_STATE_RESULTS,
    SEARCH_STATE_DETAIL
} SearchState;

#define SEARCH_WARN_MS     10000
#define SEARCH_TIMEOUT_MS  20000

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

static pthread_t search_thread;
static bool search_thread_active = false;
static volatile bool search_worker_done = false;
static bool search_worker_ok = false;
static SearchResults search_worker_results;
static uint32_t search_started_ticks = 0;
static bool search_warn_shown = false;
static bool search_timeout_fired = false;

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

static void finish_search_thread(void) {
    if (!search_thread_active) return;
    pthread_join(search_thread, NULL);
    search_thread_active = false;
}

static void* search_worker_func(void* arg) {
    (void)arg;
    PWR_pinToCores(CPU_CORE_EFFICIENCY);
    search_worker_ok = LibraryIndex_search(search_query, &search_worker_results);
    search_worker_done = true;
    return NULL;
}

static void begin_search(void) {
    finish_search_thread();

    search_worker_done = false;
    search_worker_ok = false;
    memset(&search_worker_results, 0, sizeof(search_worker_results));
    LibraryIndex_searchClearAbort();

    search_started_ticks = SDL_GetTicks();
    search_warn_shown = false;
    search_timeout_fired = false;

    if (pthread_create(&search_thread, NULL, search_worker_func, NULL) == 0) {
        search_thread_active = true;
    } else {
        search_worker_done = true;
        search_worker_ok = LibraryIndex_search(search_query, &search_worker_results);
    }
}

static void cancel_search(void) {
    LibraryIndex_searchAbort();
    finish_search_thread();
    LibraryIndex_searchClearAbort();
}

static void complete_search(SearchState* state, int* results_selected, int* results_scroll, int* dirty) {
    finish_search_thread();
    memcpy(&search_results, &search_worker_results, sizeof(search_results));

    if (LibraryIndex_searchTimedOut()) {
        show_toast("Search timed out");
    }

    if (search_worker_ok) {
        *results_selected = results_first_selectable(&search_results);
        *results_scroll = 0;
        *state = SEARCH_STATE_RESULTS;
    } else if (!LibraryIndex_searchTimedOut()) {
        show_toast("No matches found");
        *state = SEARCH_STATE_QUERY;
    } else if (search_results.nested_count > 0 || search_results.mixed_count > 0) {
        *results_selected = results_first_selectable(&search_results);
        *results_scroll = 0;
        *state = SEARCH_STATE_RESULTS;
    } else {
        *state = SEARCH_STATE_QUERY;
    }
    *dirty = 1;
}

static void poll_search_progress(SearchState* state, int* results_selected,
                                 int* results_scroll, int* dirty) {
    uint32_t elapsed = SDL_GetTicks() - search_started_ticks;

    if (!search_warn_shown && elapsed >= SEARCH_WARN_MS) {
        show_toast("Search is taking longer than usual");
        search_warn_shown = true;
        *dirty = 1;
    }

    if (!search_timeout_fired && elapsed >= SEARCH_TIMEOUT_MS) {
        LibraryIndex_searchAbort();
        search_timeout_fired = true;
    }

    if (search_worker_done) {
        complete_search(state, results_selected, results_scroll, dirty);
    }
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

ModuleExitReason SearchModule_run(SDL_Surface* screen) {
    Keyboard_init();
    M3U_init();

    SearchState state = LibraryIndex_isReady() ? SEARCH_STATE_QUERY : SEARCH_STATE_BUILDING;
    int dirty = 1;
    int show_setting = 0;
    int results_selected = 0;
    int results_scroll = 0;

    memset(&search_results, 0, sizeof(search_results));
    search_toast[0] = '\0';

    while (1) {
        GFX_startFrame();
        PAD_poll();

        int help_state;
        switch (state) {
            case SEARCH_STATE_BUILDING: help_state = SEARCH_BUILD_HELP; break;
            case SEARCH_STATE_QUERY: help_state = SEARCH_QUERY_HELP; break;
            case SEARCH_STATE_SEARCHING: help_state = SEARCH_QUERY_HELP; break;
            case SEARCH_STATE_RESULTS: help_state = SEARCH_RESULTS_HELP; break;
            default: help_state = SEARCH_DETAIL_HELP; break;
        }

        GlobalInputResult global = ModuleCommon_handleGlobalInput(screen, &show_setting, help_state);
        if (global.should_quit) {
            cancel_search();
            return MODULE_EXIT_QUIT;
        }
        if (global.input_consumed) {
            if (global.dirty) dirty = 1;
            GFX_sync();
            continue;
        }

        if (state == SEARCH_STATE_BUILDING) {
            dirty = 1;
            if (LibraryIndex_isReady()) {
                state = SEARCH_STATE_QUERY;
                dirty = 1;
            }
            if (PAD_justPressed(BTN_B)) return MODULE_EXIT_TO_MENU;
        }
        else if (state == SEARCH_STATE_QUERY) {
            if (PAD_justPressed(BTN_A)) {
                open_keyboard(&screen, search_query, sizeof(search_query));
                if (search_query[0]) {
                    begin_search();
                    state = SEARCH_STATE_SEARCHING;
                }
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_B)) {
                cancel_search();
                return MODULE_EXIT_TO_MENU;
            }
        }
        else if (state == SEARCH_STATE_SEARCHING) {
            dirty = 1;
            poll_search_progress(&state, &results_selected, &results_scroll, &dirty);
            if (PAD_justPressed(BTN_B)) {
                cancel_search();
                state = SEARCH_STATE_QUERY;
                dirty = 1;
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
                open_keyboard(&screen, search_query, sizeof(search_query));
                if (search_query[0]) {
                    begin_search();
                    state = SEARCH_STATE_SEARCHING;
                }
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_A) && total > 0) {
                if (!results_index_is_header(&search_results, results_selected)) {
                    SearchResultRow row;
                    if (search_result_at(&search_results, results_selected, &row)) {
                        load_detail_from_row(&row);
                        state = SEARCH_STATE_DETAIL;
                        dirty = 1;
                    }
                }
            }
            else if (PAD_justPressed(BTN_B)) {
                state = SEARCH_STATE_QUERY;
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
            else if (PAD_justPressed(BTN_A) && detail_track_count > 0) {
                if (!detail_is_single_track && detail_m3u_path[0]) {
                    PlayerModule_setResumePlaylistPath(detail_m3u_path);
                } else {
                    PlayerModule_setResumePlaylistPath(NULL);
                }
                ModuleExitReason reason = PlayerModule_runWithPlaylist(
                    screen, detail_tracks, detail_track_count, detail_selected);
                if (reason == MODULE_EXIT_QUIT) return MODULE_EXIT_QUIT;
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_B)) {
                state = SEARCH_STATE_RESULTS;
                dirty = 1;
            }
        }

        ModuleCommon_PWR_update(&dirty, &show_setting);

        if (dirty) {
            switch (state) {
                case SEARCH_STATE_BUILDING:
                    render_search_building(screen, show_setting, LibraryIndex_getBuildStatus());
                    break;
                case SEARCH_STATE_QUERY:
                    render_search_query(screen, show_setting, search_query);
                    break;
                case SEARCH_STATE_SEARCHING:
                    render_search_searching(screen, show_setting, search_query);
                    break;
                case SEARCH_STATE_RESULTS:
                    render_search_results(screen, show_setting, search_query,
                                          &search_results, results_selected, results_scroll);
                    break;
                case SEARCH_STATE_DETAIL:
                    render_search_detail(screen, show_setting, detail_title,
                                         detail_m3u_path[0] ? detail_m3u_path : NULL,
                                         detail_tracks, detail_track_count,
                                         detail_selected, detail_scroll);
                    break;
            }

            if (show_setting) GFX_blitHardwareHints(screen, show_setting);
            render_toast(screen, search_toast, search_toast_time);
            GFX_flip(screen);
            dirty = 0;
            ModuleCommon_tickToast(search_toast, search_toast_time, &dirty);
        } else if (state == SEARCH_STATE_SEARCHING) {
            poll_search_progress(&state, &results_selected, &results_scroll, &dirty);
            if (dirty) continue;
            GFX_sync();
        } else {
            GFX_sync();
        }
    }
}
