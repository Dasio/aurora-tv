#include "server.popup.h"
#include "server.context_menu.h"

#include "app.h"
#include "backend/pcmanager.h"
#include "ui/ui_input.h"

#include "lvgl.h"
#include "lvgl/util/lv_app_utils.h"
#include "lvgl/font/material_icons_regular_symbols.h"
#include "lvgl/theme/lv_theme_moonlight.h"
#include "lvgl/theme/lv_theme_moonlight_colors.h"
#include "lv_gridview.h"

#include "util/i18n.h"

#include <SDL.h>
#include <string.h>

/* Compact cards sized to the host count, not a full-screen cover grid. */
#define SERVER_TILE_W LV_DPX(150)
#define SERVER_TILE_H LV_DPX(118)
#define SERVER_GRID_GAP LV_DPX(12)
#define SERVER_GRID_PAD LV_DPX(6)
#define SERVER_GRID_MAX_COLS 4
/** Visible rows in the popup viewport (scroll for more). */
#define SERVER_GRID_VISIBLE_ROWS 2

typedef struct server_popup_entry_t {
    uuidstr_t id;
    const char *hostname;
    SERVER_STATE state;
    bool selected;
    bool streaming;
} server_popup_entry_t;

typedef struct server_tile_holder_t {
    lv_obj_t *icon;
    lv_obj_t *title;
    lv_obj_t *status;
} server_tile_holder_t;

typedef struct server_popup_t {
    lv_obj_t *msgbox;
    lv_obj_t *grid;
    lv_group_t *group;
    launcher_fragment_t *launcher;
    server_popup_entry_t *entries;
    int entry_count;
    int col_count;
    /* Back fires KEY and CANCEL for one press; close once. */
    bool closing;
} server_popup_t;

static void server_popup_select_index(server_popup_t *popup, int index);

static void server_popup_close(server_popup_t *popup) {
    if (popup == NULL || popup->closing) {
        return;
    }
    popup->closing = true;
    lv_msgbox_close_async(popup->msgbox);
}

static const char *server_entry_icon(const server_popup_entry_t *entry) {
    if (entry == NULL) {
        return MAT_SYMBOL_WARNING;
    }
    switch (entry->state.code) {
        case SERVER_STATE_NONE:
        case SERVER_STATE_QUERYING:
            return MAT_SYMBOL_TV;
        case SERVER_STATE_AVAILABLE:
            return entry->streaming ? MAT_SYMBOL_ONDEMAND_VIDEO : MAT_SYMBOL_TV;
        case SERVER_STATE_NOT_PAIRED:
            return MAT_SYMBOL_LOCK;
        case SERVER_STATE_ERROR:
        case SERVER_STATE_OFFLINE:
            return MAT_SYMBOL_WARNING;
        default:
            return MAT_SYMBOL_TV;
    }
}

static const char *server_entry_status(const server_popup_entry_t *entry) {
    switch (entry->state.code) {
        case SERVER_STATE_AVAILABLE:
            return entry->streaming ? locstr("Playing") : locstr("Online");
        case SERVER_STATE_NOT_PAIRED:
            return locstr("Not paired");
        case SERVER_STATE_ERROR:
        case SERVER_STATE_OFFLINE:
            return locstr("Offline");
        default:
            return locstr("Connecting...");
    }
}

static int server_popup_col_count(int entry_count) {
    if (entry_count <= 1) {
        return 1;
    }
    return entry_count < SERVER_GRID_MAX_COLS ? entry_count : SERVER_GRID_MAX_COLS;
}

static lv_coord_t server_popup_grid_width(int col_count) {
    return col_count * SERVER_TILE_W + (col_count - 1) * SERVER_GRID_GAP + 2 * SERVER_GRID_PAD;
}

static void server_popup_update_grid(server_popup_t *popup) {
    if (popup == NULL || popup->grid == NULL) {
        return;
    }
    const int col_count = server_popup_col_count(popup->entry_count);
    const int row_count = (popup->entry_count + col_count - 1) / col_count;
    const int visible_rows = row_count < SERVER_GRID_VISIBLE_ROWS ? row_count : SERVER_GRID_VISIBLE_ROWS;
    popup->col_count = col_count;
    lv_obj_set_size(popup->grid, server_popup_grid_width(col_count),
                    visible_rows * SERVER_TILE_H + (visible_rows - 1) * SERVER_GRID_GAP + 2 * SERVER_GRID_PAD);
    lv_gridview_set_config(popup->grid, col_count, SERVER_TILE_H, LV_GRID_ALIGN_CENTER, LV_GRID_ALIGN_CENTER);
}

static void server_tile_deselected(lv_event_t *e) {
    lv_obj_clear_state(lv_event_get_target(e), LV_STATE_FOCUS_KEY);
}

static void server_tile_holder_delete_cb(lv_event_t *e) {
    SDL_free(lv_event_get_user_data(e));
}

static lv_obj_t *server_tile_create(server_popup_t *popup, lv_obj_t *parent) {
    (void) popup;
    static const lv_style_prop_t trans_props[] = {
            LV_STYLE_BG_COLOR, LV_STYLE_OUTLINE_OPA, LV_STYLE_TRANSFORM_WIDTH, LV_STYLE_TRANSFORM_HEIGHT, 0,
    };
    static lv_style_transition_dsc_t trans;
    static bool trans_ready = false;
    if (!trans_ready) {
        lv_style_transition_dsc_init(&trans, trans_props, lv_anim_path_ease_out, 120, 0, NULL);
        trans_ready = true;
    }

    lv_obj_t *item = lv_obj_create(parent);
    lv_obj_remove_style_all(item);
    lv_obj_add_flag(item, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(item, SERVER_TILE_W, SERVER_TILE_H);
    lv_obj_set_style_radius(item, LV_DPX(16), 0);
    lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(item, ml_color_hex(ML_COLOR_SURFACE_ALT), 0);
    lv_obj_set_style_border_width(item, LV_DPX(1), 0);
    lv_obj_set_style_border_color(item, ml_color_hex(ML_COLOR_BORDER), 0);
    lv_obj_set_style_pad_all(item, LV_DPX(8), 0);
    lv_obj_set_style_pad_row(item, LV_DPX(2), 0);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_transition(item, &trans, 0);
    lv_obj_set_style_bg_color(item, ml_color_hex(ML_COLOR_SURFACE_HI), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(item, LV_DPX(3), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_pad(item, LV_DPX(2), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_color(item, ml_color_hex(ML_COLOR_FOCUS), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_opa(item, LV_OPA_COVER, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_transform_width(item, LV_DPX(3), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_transform_height(item, LV_DPX(3), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_color(item, ml_color_hex(ML_COLOR_SURFACE_HI), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(item, LV_DPX(2), LV_STATE_CHECKED);
    lv_obj_set_style_border_color(item, ml_color_hex(ML_COLOR_PRIMARY), LV_STATE_CHECKED);

    lv_obj_t *icon = lv_label_create(item);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_font(icon, lv_theme_moonlight_get_iconfont_large(item), 0);
    lv_obj_set_style_text_color(icon, ml_color_hex(ML_COLOR_TEXT), 0);

    lv_obj_t *title = lv_label_create(item);
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_style_text_color(title, ml_color_hex(ML_COLOR_TEXT), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *status = lv_label_create(item);
    lv_obj_set_width(status, LV_PCT(100));
    lv_obj_set_style_text_font(status, lv_theme_get_font_small(item), 0);
    lv_obj_set_style_text_color(status, ml_color_hex(ML_COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(status, LV_LABEL_LONG_DOT);
    lv_obj_clear_flag(status, LV_OBJ_FLAG_CLICKABLE);

    server_tile_holder_t *holder = SDL_malloc(sizeof(server_tile_holder_t));
    holder->icon = icon;
    holder->title = title;
    holder->status = status;
    lv_obj_set_user_data(item, holder);
    lv_obj_add_event_cb(item, server_tile_deselected, LV_EVENT_DEFOCUSED, NULL);
    lv_obj_add_event_cb(item, server_tile_holder_delete_cb, LV_EVENT_DELETE, holder);
    return item;
}

static void server_tile_bind(server_popup_t *popup, lv_obj_t *item, int position) {
    if (popup == NULL || item == NULL || position < 0 || position >= popup->entry_count) {
        return;
    }
    server_tile_holder_t *holder = lv_obj_get_user_data(item);
    if (holder == NULL) {
        return;
    }
    const server_popup_entry_t *entry = &popup->entries[position];
    lv_label_set_text(holder->title, entry->hostname);
    lv_label_set_text_static(holder->icon, server_entry_icon(entry));
    lv_label_set_text(holder->status, server_entry_status(entry));
    lv_obj_set_style_text_color(holder->icon, ml_color_hex(entry->selected ? ML_COLOR_PRIMARY : ML_COLOR_TEXT), 0);
    if (entry->selected) {
        lv_obj_add_state(item, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(item, LV_STATE_CHECKED);
    }
}

static int server_adapter_count(lv_obj_t *grid, void *data) {
    (void) grid;
    server_popup_t *popup = data;
    return popup != NULL ? popup->entry_count : 0;
}

static lv_obj_t *server_adapter_create_view(lv_obj_t *parent) {
    server_popup_t *popup = lv_obj_get_user_data(parent);
    return server_tile_create(popup, parent);
}

static void server_adapter_bind_view(lv_obj_t *grid, lv_obj_t *item_view, void *data, int position) {
    (void) grid;
    server_tile_bind(data, item_view, position);
}

static const lv_gridview_adapter_t server_grid_adapter = {
        .item_count = server_adapter_count,
        .create_view = server_adapter_create_view,
        .bind_view = server_adapter_bind_view,
};

static void server_grid_focus_with_key(lv_obj_t *grid, int idx) {
    lv_gridview_focus(grid, idx);
    lv_obj_t *item = lv_gridview_get_item_view(grid, idx);
    if (item != NULL) {
        lv_obj_add_state(item, LV_STATE_FOCUS_KEY);
    }
}

static int server_popup_selected_index(const server_popup_t *popup) {
    if (popup == NULL) {
        return 0;
    }
    for (int i = 0; i < popup->entry_count; i++) {
        if (popup->entries[i].selected) {
            return i;
        }
    }
    return 0;
}

static void server_popup_select_index(server_popup_t *popup, int index) {
    if (popup == NULL || popup->launcher == NULL || index < 0 || index >= popup->entry_count) {
        return;
    }
    uuidstr_t snapshot = popup->entries[index].id;
    server_popup_close(popup);
    launcher_select_server(popup->launcher, &snapshot);
}

static void server_popup_item_click(lv_event_t *e) {
    server_popup_t *popup = lv_event_get_user_data(e);
    lv_obj_t *target = lv_event_get_target(e);
    if (popup == NULL || popup->grid == NULL || lv_obj_get_parent(target) != popup->grid) {
        return;
    }
    int index = lv_gridview_get_item_data_index(popup->grid, target);
    if (index >= 0) {
        server_popup_select_index(popup, index);
    }
}

static void server_popup_item_longpress(lv_event_t *e) {
    server_popup_t *popup = lv_event_get_user_data(e);
    lv_obj_t *target = lv_event_get_target(e);
    if (popup == NULL || popup->grid == NULL || lv_obj_get_parent(target) != popup->grid) {
        return;
    }
    int index = lv_gridview_get_item_data_index(popup->grid, target);
    if (index < 0 || index >= popup->entry_count) {
        return;
    }
    uuidstr_t snapshot = popup->entries[index].id;
    server_popup_close(popup);
    lv_fragment_t *fragment = lv_fragment_create(&server_menu_class, &snapshot);
    lv_obj_t *menu = lv_fragment_create_obj(fragment, NULL);
    lv_obj_add_event_cb(menu, ui_cb_destroy_fragment, LV_EVENT_DELETE, fragment);
}

static void server_popup_grid_focus_enter(lv_event_t *e) {
    if (lv_event_get_target(e) != lv_event_get_current_target(e)) {
        return;
    }
    server_popup_t *popup = lv_event_get_user_data(e);
    if (popup == NULL || popup->grid == NULL) {
        return;
    }
    int idx = server_popup_selected_index(popup);
    server_grid_focus_with_key(popup->grid, idx);
}

static void server_popup_focus_close_btn(server_popup_t *popup) {
    lv_obj_t *btns = lv_msgbox_get_btns(popup->msgbox);
    if (btns == NULL) {
        return;
    }
    if (popup->grid != NULL) {
        lv_gridview_focus(popup->grid, -1);
    }
    lv_group_focus_obj(btns);
    lv_btnmatrix_set_selected_btn(btns, 0);
    lv_obj_add_state(btns, LV_STATE_FOCUS_KEY);
}

/*
 * The popup owns its navigation (the theme's msgbox handlers are off via USER_4):
 * Down past the last card reaches Close, Up from Close returns to the cards, Back closes.
 * The grid runs with key focus clamp off, so a key it cannot move on reaches us.
 */
static void server_popup_key_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) {
        return;
    }
    server_popup_t *popup = lv_event_get_user_data(e);
    lv_obj_t *target = lv_event_get_target(e);
    switch (lv_event_get_key(e)) {
        case LV_KEY_ESC:
            server_popup_close(popup);
            break;
        case LV_KEY_DOWN:
            if (popup->grid != NULL && target == popup->grid) {
                server_popup_focus_close_btn(popup);
            }
            break;
        case LV_KEY_UP:
            if (popup->grid != NULL && target == lv_msgbox_get_btns(popup->msgbox)) {
                lv_obj_clear_state(target, LV_STATE_FOCUS_KEY);
                lv_group_focus_obj(popup->grid);
                server_grid_focus_with_key(popup->grid, server_popup_selected_index(popup));
            }
            break;
        default:
            break;
    }
}

static void server_popup_cancel_cb(lv_event_t *e) {
    server_popup_close(lv_event_get_user_data(e));
}

static void server_popup_deleted(lv_event_t *e) {
    server_popup_t *popup = lv_event_get_user_data(e);
    if (popup->group != NULL) {
        app_input_remove_modal_group(&popup->launcher->global->ui.input, popup->group);
        lv_group_del(popup->group);
        popup->group = NULL;
    }
    SDL_free(popup->entries);
    SDL_free(popup);
}

static void server_popup_msgbox_cb(lv_event_t *e) {
    server_popup_t *popup = lv_event_get_user_data(e);
    lv_obj_t *msgbox = lv_event_get_current_target(e);
    if (msgbox != popup->msgbox) {
        return;
    }
    if (lv_msgbox_get_active_btn(msgbox) == 0) {
        server_popup_close(popup);
    }
}

void server_popup_open(launcher_fragment_t *controller) {
    if (!controller) {
        return;
    }

    int count = 0;
    for (const pclist_t *cur = pcmanager_servers(pcmanager); cur != NULL; cur = cur->next) {
        if (cur->server != NULL) {
            count++;
        }
    }

    server_popup_t *popup = SDL_malloc(sizeof(server_popup_t));
    memset(popup, 0, sizeof(*popup));
    popup->launcher = controller;
    popup->entry_count = count;

    if (count > 0) {
        popup->entries = SDL_malloc((size_t) count * sizeof(server_popup_entry_t));
        int i = 0;
        for (const pclist_t *cur = pcmanager_servers(pcmanager); cur != NULL; cur = cur->next) {
            if (cur->server == NULL) {
                continue;
            }
            popup->entries[i].id = cur->id;
            popup->entries[i].hostname = cur->server->hostname;
            popup->entries[i].state = cur->state;
            popup->entries[i].selected = cur->selected;
            popup->entries[i].streaming = cur->server->currentGame != 0;
            i++;
        }
    }

    static const char *btn_texts[] = {translatable("Close"), ""};
    lv_obj_t *msgbox = lv_msgbox_create_i18n(NULL, locstr("Select server"), NULL, btn_texts, false);
    popup->msgbox = msgbox;
    lv_obj_set_user_data(msgbox, popup);
    lv_obj_add_flag(msgbox, LV_OBJ_FLAG_USER_4);
    lv_obj_add_event_cb(msgbox, server_popup_deleted, LV_EVENT_DELETE, popup);
    lv_obj_add_event_cb(msgbox, server_popup_msgbox_cb, LV_EVENT_VALUE_CHANGED, popup);
    lv_obj_add_event_cb(msgbox, server_popup_key_cb, LV_EVENT_KEY, popup);
    /* Fit the cards: the theme's 40-60% msgbox width is meant for text dialogs. */
    lv_obj_set_style_max_width(msgbox, LV_PCT(90), 0);
    lv_obj_set_style_min_width(msgbox, LV_DPX(360), 0);
    lv_obj_set_width(msgbox, count > 0 ? server_popup_grid_width(server_popup_col_count(count)) + LV_DPX(56)
                                       : LV_DPX(420));

    lv_obj_t *content = lv_msgbox_get_content(msgbox);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(content, lv_dpx(4), 0);
    lv_obj_set_style_pad_gap(content, lv_dpx(8), 0);

    if (count == 0) {
        lv_obj_t *empty = lv_label_create(content);
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(empty, LV_PCT(100));
        lv_label_set_text_static(empty,
                                 locstr("No paired computers yet. Use the \"+\" button on the top bar to add one."));
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    } else {
        lv_obj_t *grid = popup->grid = lv_gridview_create(content);
        lv_obj_set_user_data(grid, popup);
        lv_obj_add_flag(grid, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_scroll_dir(grid, LV_DIR_VER);
        lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLL_WITH_ARROW);
        lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_style_pad_all(grid, SERVER_GRID_PAD, 0);
        lv_obj_set_style_pad_row(grid, SERVER_GRID_GAP, 0);
        lv_obj_set_style_pad_column(grid, SERVER_GRID_GAP, 0);
        lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_opa(grid, LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(grid, 0, 0);
        lv_obj_set_style_anim_time(grid, 220, 0);
        lv_gridview_set_key_focus_clamp(grid, false);
        lv_gridview_set_adapter(grid, &server_grid_adapter);

        server_popup_update_grid(popup);
        lv_gridview_set_data(grid, popup);

        lv_obj_add_event_cb(grid, server_popup_item_click, LV_EVENT_SHORT_CLICKED, popup);
        lv_obj_add_event_cb(grid, server_popup_item_longpress, LV_EVENT_LONG_PRESSED, popup);
        lv_obj_add_event_cb(grid, server_popup_grid_focus_enter, LV_EVENT_FOCUSED, popup);
        lv_obj_add_event_cb(grid, server_popup_key_cb, LV_EVENT_KEY, popup);
        lv_obj_add_event_cb(grid, server_popup_cancel_cb, LV_EVENT_CANCEL, popup);
    }

    popup->group = lv_group_create();
    lv_group_set_wrap(popup->group, false);
    if (popup->grid != NULL) {
        lv_group_add_obj(popup->group, popup->grid);
    }
    /* Close stays reachable with the remote (and is the only target when no host is paired). */
    lv_obj_t *btns = lv_msgbox_get_btns(msgbox);
    if (btns != NULL) {
        lv_group_add_obj(popup->group, btns);
        lv_obj_add_event_cb(btns, server_popup_key_cb, LV_EVENT_KEY, popup);
        lv_obj_add_event_cb(btns, server_popup_cancel_cb, LV_EVENT_CANCEL, popup);
    }
    /* Modal only: setting it as the app group too left input on a deleted group after close. */
    app_input_push_modal_group(&controller->global->ui.input, popup->group);
    if (popup->grid != NULL) {
        server_grid_focus_with_key(popup->grid, server_popup_selected_index(popup));
    }

    lv_obj_center(msgbox);
}
