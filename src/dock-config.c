/*
 * crepido
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "dock-config.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>

#define CONFIG_GROUP "Dock"
#define CONFIG_KEY_APPLICATIONS "Applications"
#define CONFIG_KEY_ITEMS "Items"
#define CONFIG_KEY_SIDE "Side"
#define CONFIG_KEY_POSITION "Position"
#define CONFIG_KEY_MONITOR "Monitor"
#define CONFIG_KEY_MONITOR_MANUFACTURER "MonitorManufacturer"
#define CONFIG_KEY_MONITOR_MODEL "MonitorModel"
#define CONFIG_KEY_ICON_SIZE "IconSize"
#define CONFIG_KEY_OPACITY "Opacity"
#define CONFIG_KEY_BITMAP_BACKGROUND_ENABLED "BitmapBackgroundEnabled"
#define CONFIG_KEY_BITMAP_BACKGROUND_MODE "BitmapBackgroundMode"
#define CONFIG_KEY_BITMAP_BACKGROUND_PATH "BitmapBackgroundPath"
#define CONFIG_KEY_WINDOW_INDICATOR "WindowIndicator"
#define CONFIG_KEY_SHOW_HIDE_HANDLE "ShowHideHandle"
#define CONFIG_KEY_SHORTCUT_ONLY "ShortcutOnly"
#define CONFIG_KEY_MINIMIZED_WINDOW_ICONS "MinimizedWindowIcons"
#define CONFIG_KEY_MINIMIZED_TITLE_LABELS "ExperimentalTitleLabels"
#define CONFIG_KEY_MINIMIZED_GROUP_DRAWER "MinimizedGroupDrawer"
#define CONFIG_KEY_MINIMIZED_ALL_WORKSPACES "MinimizedAllWorkspaces"
#define CONFIG_DRAWER_PREFIX "Drawer"
#define CONFIG_DRAWER_NAME "Name"
#define CONFIG_DRAWER_APPLICATIONS "Applications"
#define CONFIG_FILENAME "dock.conf"

static gchar *dock_config_get_path(void);

static gchar *
dock_config_get_directory(void)
{
    return g_build_filename(
        g_get_user_config_dir(),
        "crepido",
        NULL);
}

static gboolean
dock_config_load_key_file(
    GKeyFile **key_file_out,
    GError **error)
{
    gchar *path = dock_config_get_path();
    GKeyFile *key_file = g_key_file_new();
    GError *load_error = NULL;

    if (!g_key_file_load_from_file(
            key_file,
            path,
            G_KEY_FILE_NONE,
            &load_error)) {
        g_free(path);

        if (load_error &&
            load_error->domain == G_FILE_ERROR &&
            load_error->code == G_FILE_ERROR_NOENT) {
            g_clear_error(&load_error);
            g_key_file_unref(key_file);
            return FALSE;
        }

        g_key_file_unref(key_file);

        if (load_error)
            g_propagate_error(error, load_error);

        return FALSE;
    }

    g_free(path);
    *key_file_out = key_file;
    return TRUE;
}


static gboolean
dock_config_read_error_is_missing_key(const GError *error)
{
    return error &&
        error->domain == G_KEY_FILE_ERROR &&
        error->code == G_KEY_FILE_ERROR_KEY_NOT_FOUND;
}

static gchar *
dock_config_read_string_from_key_file(
    GKeyFile *key_file,
    const gchar *key,
    gboolean *found,
    GError **error)
{
    g_return_val_if_fail(found != NULL, NULL);
    *found = FALSE;

    GError *read_error = NULL;
    gchar *value =
        g_key_file_get_string(
            key_file,
            CONFIG_GROUP,
            key,
            &read_error);

    if (read_error) {
        if (dock_config_read_error_is_missing_key(read_error))
            g_clear_error(&read_error);
        else
            g_propagate_error(error, read_error);
        return NULL;
    }

    *found = TRUE;
    return value;
}

static gint
dock_config_read_integer_from_key_file(
    GKeyFile *key_file,
    const gchar *key,
    gint default_value,
    gboolean *found,
    GError **error)
{
    g_return_val_if_fail(found != NULL, default_value);
    *found = FALSE;

    GError *read_error = NULL;
    gint value =
        g_key_file_get_integer(
            key_file,
            CONFIG_GROUP,
            key,
            &read_error);

    if (read_error) {
        if (dock_config_read_error_is_missing_key(read_error))
            g_clear_error(&read_error);
        else
            g_propagate_error(error, read_error);
        return default_value;
    }

    *found = TRUE;
    return value;
}

static gboolean
dock_config_read_boolean_from_key_file(
    GKeyFile *key_file,
    const gchar *key,
    gboolean default_value,
    gboolean *found,
    GError **error)
{
    g_return_val_if_fail(found != NULL, default_value);
    *found = FALSE;

    GError *read_error = NULL;
    gboolean value =
        g_key_file_get_boolean(
            key_file,
            CONFIG_GROUP,
            key,
            &read_error);

    if (read_error) {
        if (dock_config_read_error_is_missing_key(read_error))
            g_clear_error(&read_error);
        else
            g_propagate_error(error, read_error);
        return default_value;
    }

    *found = TRUE;
    return value;
}

static gchar *
dock_config_read_string(
    const gchar *key,
    gboolean *found,
    GError **error)
{
    g_return_val_if_fail(found != NULL, NULL);
    *found = FALSE;

    GKeyFile *key_file = NULL;
    if (!dock_config_load_key_file(&key_file, error))
        return NULL;

    gchar *value =
        dock_config_read_string_from_key_file(
            key_file,
            key,
            found,
            error);
    g_key_file_unref(key_file);
    return value;
}

static gint
dock_config_read_integer(
    const gchar *key,
    gint default_value,
    gboolean *found,
    GError **error)
{
    g_return_val_if_fail(found != NULL, default_value);
    *found = FALSE;

    GKeyFile *key_file = NULL;
    if (!dock_config_load_key_file(&key_file, error))
        return default_value;

    gint value =
        dock_config_read_integer_from_key_file(
            key_file,
            key,
            default_value,
            found,
            error);
    g_key_file_unref(key_file);
    return value;
}

static gboolean
dock_config_read_boolean(
    const gchar *key,
    gboolean default_value,
    gboolean *found,
    GError **error)
{
    g_return_val_if_fail(found != NULL, FALSE);
    *found = FALSE;

    GKeyFile *key_file = NULL;
    if (!dock_config_load_key_file(&key_file, error))
        return default_value;

    gboolean value =
        dock_config_read_boolean_from_key_file(
            key_file,
            key,
            default_value,
            found,
            error);
    g_key_file_unref(key_file);
    return value;
}

static gchar **
dock_config_make_legacy_items(
    GKeyFile *key_file,
    gboolean *found,
    gsize *length,
    GError **error)
{
    gsize app_length = 0;
    gchar **applications =
        g_key_file_get_string_list(
            key_file,
            CONFIG_GROUP,
            CONFIG_KEY_APPLICATIONS,
            &app_length,
            error);

    if (!applications)
        return NULL;

    gchar **items =
        g_new0(
            gchar *,
            app_length + 1);

    for (gsize i = 0; i < app_length; i++) {
        items[i] =
            g_strdup_printf(
                "app:%s",
                applications[i]);
    }

    g_strfreev(applications);

    *found = TRUE;
    *length = app_length;
    return items;
}


static gchar **
dock_config_read_items_from_key_file(
    GKeyFile *key_file,
    gboolean *found,
    gsize *length,
    GError **error)
{
    *found = FALSE;
    *length = 0;

    gsize item_length = 0;
    GError *read_error = NULL;
    gchar **items =
        g_key_file_get_string_list(
            key_file,
            CONFIG_GROUP,
            CONFIG_KEY_ITEMS,
            &item_length,
            &read_error);

    if (items) {
        *found = TRUE;
        *length = item_length;
        return items;
    }

    /* Older versions stored only the Applications list. */
    g_clear_error(&read_error);
    items =
        dock_config_make_legacy_items(
            key_file,
            found,
            length,
            &read_error);

    if (!items && read_error)
        g_propagate_error(error, read_error);

    return items;
}

static gchar *
dock_config_get_path(void)
{
    gchar *directory = dock_config_get_directory();
    gchar *path = g_build_filename(directory, CONFIG_FILENAME, NULL);
    g_free(directory);
    return path;
}

gchar **
dock_config_load_ids(gboolean *found, GError **error)
{
    g_return_val_if_fail(found != NULL, NULL);

    *found = FALSE;

    gchar *path = dock_config_get_path();
    GKeyFile *key_file = g_key_file_new();
    GError *load_error = NULL;

    if (!g_key_file_load_from_file(
            key_file,
            path,
            G_KEY_FILE_NONE,
            &load_error)) {
        if (load_error &&
            load_error->domain == G_FILE_ERROR &&
            load_error->code == G_FILE_ERROR_NOENT) {
            g_clear_error(&load_error);
            g_key_file_unref(key_file);
            g_free(path);
            return NULL;
        }

        if (load_error) {
            g_propagate_error(error, load_error);
            g_key_file_unref(key_file);
            g_free(path);
            return NULL;
        }

        g_key_file_unref(key_file);
        g_free(path);
        return NULL;
    }

    g_free(path);

    gsize length = 0;
    GError *read_error = NULL;

    gchar **ids =
        g_key_file_get_string_list(
            key_file,
            CONFIG_GROUP,
            CONFIG_KEY_APPLICATIONS,
            &length,
            &read_error);

    g_key_file_unref(key_file);

    if (!ids) {
        if (read_error)
            g_propagate_error(error, read_error);
        return NULL;
    }

    *found = TRUE;
    return ids;
}


gchar **
dock_config_load_items(
    gboolean *found,
    gsize *length,
    GError **error)
{
    g_return_val_if_fail(found != NULL, NULL);
    g_return_val_if_fail(length != NULL, NULL);

    *found = FALSE;
    *length = 0;

    GKeyFile *key_file = NULL;
    if (!dock_config_load_key_file(&key_file, error))
        return NULL;

    gchar **items =
        dock_config_read_items_from_key_file(
            key_file,
            found,
            length,
            error);

    g_key_file_unref(key_file);
    return items;
}

gboolean
dock_config_load_side(
    gboolean *found,
    GError **error)
{
    g_return_val_if_fail(found != NULL, TRUE);

    gchar *side =
        dock_config_read_string(
            CONFIG_KEY_SIDE,
            found,
            error);

    if (!side)
        return TRUE;

    gboolean on_right_side =
        g_ascii_strcasecmp(side, "left") != 0;

    g_free(side);
    return on_right_side;
}

gint
dock_config_load_position(
    gboolean *found,
    GError **error)
{
    g_return_val_if_fail(found != NULL, 0);

    gchar *position =
        dock_config_read_string(
            CONFIG_KEY_POSITION,
            found,
            error);

    if (!position)
        return 0;

    gint mode = 0;
    if (g_ascii_strcasecmp(position, "auto") == 0)
        mode = 1;
    else if (g_ascii_strcasecmp(position, "top") == 0)
        mode = 2;

    g_free(position);
    return mode;
}


static gchar *
dock_config_load_monitor_string(
    const gchar *key,
    gboolean *found,
    GError **error)
{
    return dock_config_read_string(key, found, error);
}

gchar *
dock_config_load_monitor_manufacturer(
    gboolean *found,
    GError **error)
{
    return dock_config_load_monitor_string(
        CONFIG_KEY_MONITOR_MANUFACTURER,
        found,
        error);
}

gchar *
dock_config_load_monitor_model(
    gboolean *found,
    GError **error)
{
    return dock_config_load_monitor_string(
        CONFIG_KEY_MONITOR_MODEL,
        found,
        error);
}

gint
dock_config_load_icon_size(
    gboolean *found,
    GError **error)
{
    g_return_val_if_fail(found != NULL, 48);

    gint icon_size =
        dock_config_read_integer(
            CONFIG_KEY_ICON_SIZE,
            48,
            found,
            error);

    return CLAMP(icon_size, 16, 64);
}

gint
dock_config_load_opacity(
    gboolean *found,
    GError **error)
{
    g_return_val_if_fail(found != NULL, 100);

    gint opacity =
        dock_config_read_integer(
            CONFIG_KEY_OPACITY,
            100,
            found,
            error);

    return CLAMP(opacity, 50, 100);
}

gboolean
dock_config_load_bitmap_background_enabled(
    gboolean *found,
    GError **error)
{
    g_return_val_if_fail(found != NULL, FALSE);

    return dock_config_read_boolean(
        CONFIG_KEY_BITMAP_BACKGROUND_ENABLED,
        FALSE,
        found,
        error);
}

gint
dock_config_load_bitmap_background_mode(
    gboolean *found,
    GError **error)
{
    g_return_val_if_fail(found != NULL, 0);

    gint mode =
        dock_config_read_integer(
            CONFIG_KEY_BITMAP_BACKGROUND_MODE,
            0,
            found,
            error);

    return mode == 1 ? 1 : 0;
}

gchar *
dock_config_load_bitmap_background_path(
    gboolean *found,
    GError **error)
{
    g_return_val_if_fail(found != NULL, NULL);

    gchar *path =
        dock_config_read_string(
            CONFIG_KEY_BITMAP_BACKGROUND_PATH,
            found,
            error);

    if (path && *path)
        return path;

    g_free(path);
    *found = FALSE;
    return NULL;
}

gint
dock_config_load_monitor(
    gboolean *found,
    GError **error)
{
    g_return_val_if_fail(found != NULL, -1);

    return dock_config_read_integer(
        CONFIG_KEY_MONITOR,
        -1,
        found,
        error);
}

gboolean
dock_config_load_minimized_title_labels(
    gboolean *found,
    GError **error)
{
    g_return_val_if_fail(found != NULL, FALSE);

    return dock_config_read_boolean(
        CONFIG_KEY_MINIMIZED_TITLE_LABELS,
        FALSE,
        found,
        error);
}

gboolean
dock_config_load_minimized_group_drawer(
    gboolean *found,
    GError **error)
{
    g_return_val_if_fail(found != NULL, FALSE);

    return dock_config_read_boolean(
        CONFIG_KEY_MINIMIZED_GROUP_DRAWER,
        FALSE,
        found,
        error);
}

gboolean
dock_config_load_minimized_all_workspaces(
    gboolean *found,
    GError **error)
{
    g_return_val_if_fail(found != NULL, FALSE);

    return dock_config_read_boolean(
        CONFIG_KEY_MINIMIZED_ALL_WORKSPACES,
        FALSE,
        found,
        error);
}

gboolean
dock_config_load_shortcut_only(
    gboolean *found,
    GError **error)
{
    g_return_val_if_fail(found != NULL, FALSE);

    return dock_config_read_boolean(
        CONFIG_KEY_SHORTCUT_ONLY,
        FALSE,
        found,
        error);
}

gboolean
dock_config_load_minimized_window_icons(
    gboolean *found,
    GError **error)
{
    g_return_val_if_fail(found != NULL, FALSE);

    return dock_config_read_boolean(
        CONFIG_KEY_MINIMIZED_WINDOW_ICONS,
        FALSE,
        found,
        error);
}

gboolean
dock_config_load_window_indicator(
    gboolean *found,
    GError **error)
{
    g_return_val_if_fail(found != NULL, TRUE);

    return dock_config_read_boolean(
        CONFIG_KEY_WINDOW_INDICATOR,
        TRUE,
        found,
        error);
}

gboolean
dock_config_load_show_hide_handle(
    gboolean *found,
    GError **error)
{
    g_return_val_if_fail(found != NULL, TRUE);

    return dock_config_read_boolean(
        CONFIG_KEY_SHOW_HIDE_HANDLE,
        TRUE,
        found,
        error);
}

static DockConfigDrawer *
dock_config_read_drawers_from_key_file(
    GKeyFile *key_file,
    gsize *length)
{
    g_return_val_if_fail(length != NULL, NULL);
    *length = 0;

    gsize group_count = 0;
    gchar **groups =
        g_key_file_get_groups(
            key_file,
            &group_count);

    GPtrArray *drawers =
        g_ptr_array_new();

    /*
     * Drawer groups are numbered, so loading order is deterministic.
     */
    for (gsize i = 0; i < group_count; i++) {
        const gchar *group = groups[i];

        if (!g_str_has_prefix(
                group,
                CONFIG_DRAWER_PREFIX))
            continue;

        gchar *end = NULL;
        g_ascii_strtoull(
            group + strlen(CONFIG_DRAWER_PREFIX),
            &end,
            10);

        if (!end || *end != '\0')
            continue;

        gchar *name =
            g_key_file_get_string(
                key_file,
                group,
                CONFIG_DRAWER_NAME,
                NULL);

        gsize app_length = 0;
        gchar **applications =
            g_key_file_get_string_list(
                key_file,
                group,
                CONFIG_DRAWER_APPLICATIONS,
                &app_length,
                NULL);

        if (!name)
            name = g_strdup(group);

        DockConfigDrawer *drawer =
            g_new0(DockConfigDrawer, 1);
        drawer->name = name;
        drawer->applications = applications;
        drawer->application_count = app_length;
        g_ptr_array_add(drawers, drawer);
    }

    g_strfreev(groups);
    *length = drawers->len;

    if (drawers->len == 0) {
        g_ptr_array_free(drawers, TRUE);
        return NULL;
    }

    DockConfigDrawer *result =
        g_new0(DockConfigDrawer, drawers->len);

    for (guint i = 0; i < drawers->len; i++) {
        DockConfigDrawer *source =
            g_ptr_array_index(drawers, i);
        result[i] = *source;
        g_free(source);
    }

    g_ptr_array_free(drawers, TRUE);
    return result;
}

DockConfigDrawer *
dock_config_load_drawers(
    gsize *length,
    GError **error)
{
    g_return_val_if_fail(length != NULL, NULL);
    *length = 0;

    GKeyFile *key_file = NULL;
    if (!dock_config_load_key_file(&key_file, error))
        return NULL;

    DockConfigDrawer *drawers =
        dock_config_read_drawers_from_key_file(key_file, length);
    g_key_file_unref(key_file);
    return drawers;
}

void
dock_config_free_drawers(
    DockConfigDrawer *drawers,
    gsize length)
{
    if (!drawers)
        return;

    for (gsize i = 0; i < length; i++) {
        g_free(drawers[i].name);
        g_strfreev(drawers[i].applications);
    }

    g_free(drawers);
}

static void
dock_config_state_record_error(
    DockConfigState *state,
    const gchar *setting,
    GError *error)
{
    if (!error)
        return;

    g_ptr_array_add(
        state->warnings,
        g_strdup_printf("%s: %s", setting, error->message));
    g_error_free(error);
}

static gchar *
dock_config_state_read_string(
    DockConfigState *state,
    GKeyFile *key_file,
    const gchar *key,
    gboolean *found)
{
    GError *error = NULL;
    gchar *value =
        dock_config_read_string_from_key_file(
            key_file,
            key,
            found,
            &error);
    dock_config_state_record_error(state, key, error);
    return value;
}

static gint
dock_config_state_read_integer(
    DockConfigState *state,
    GKeyFile *key_file,
    const gchar *key,
    gint default_value,
    gboolean *found)
{
    GError *error = NULL;
    gint value =
        dock_config_read_integer_from_key_file(
            key_file,
            key,
            default_value,
            found,
            &error);
    dock_config_state_record_error(state, key, error);
    return value;
}

static gboolean
dock_config_state_read_boolean(
    DockConfigState *state,
    GKeyFile *key_file,
    const gchar *key,
    gboolean default_value,
    gboolean *found)
{
    GError *error = NULL;
    gboolean value =
        dock_config_read_boolean_from_key_file(
            key_file,
            key,
            default_value,
            found,
            &error);
    dock_config_state_record_error(state, key, error);
    return value;
}

gboolean
dock_config_load_state(
    DockConfigState *state,
    GError **error)
{
    g_return_val_if_fail(state != NULL, FALSE);

    /* The caller supplies an empty, zero-initialized snapshot. */
    memset(state, 0, sizeof(*state));
    state->warnings = g_ptr_array_new_with_free_func(g_free);
    state->on_right_side = TRUE;
    state->monitor_index = -1;
    state->icon_size = 48;
    state->opacity = 100;
    state->show_window_indicator = TRUE;
    state->show_hide_handle = TRUE;

    GKeyFile *key_file = NULL;
    if (!dock_config_load_key_file(&key_file, error))
        return FALSE;

    state->monitor_index =
        dock_config_state_read_integer(
            state, key_file, CONFIG_KEY_MONITOR, -1,
            &state->monitor_found);

    state->monitor_manufacturer =
        dock_config_state_read_string(
            state, key_file, CONFIG_KEY_MONITOR_MANUFACTURER,
            &state->monitor_manufacturer_found);

    state->monitor_model =
        dock_config_state_read_string(
            state, key_file, CONFIG_KEY_MONITOR_MODEL,
            &state->monitor_model_found);

    gint icon_size =
        dock_config_state_read_integer(
            state, key_file, CONFIG_KEY_ICON_SIZE, 48,
            &state->icon_size_found);
    state->icon_size = CLAMP(icon_size, 16, 64);

    gint opacity =
        dock_config_state_read_integer(
            state, key_file, CONFIG_KEY_OPACITY, 100,
            &state->opacity_found);
    state->opacity = CLAMP(opacity, 50, 100);

    gchar *side =
        dock_config_state_read_string(
            state, key_file, CONFIG_KEY_SIDE, &state->side_found);
    if (side) {
        state->on_right_side =
            g_ascii_strcasecmp(side, "left") != 0;
        g_free(side);
    }

    gchar *position =
        dock_config_state_read_string(
            state, key_file, CONFIG_KEY_POSITION,
            &state->position_found);
    if (position) {
        if (g_ascii_strcasecmp(position, "auto") == 0)
            state->position_mode = 1;
        else if (g_ascii_strcasecmp(position, "top") == 0)
            state->position_mode = 2;
        g_free(position);
    }

    state->show_window_indicator =
        dock_config_state_read_boolean(
            state, key_file, CONFIG_KEY_WINDOW_INDICATOR, TRUE,
            &state->window_indicator_found);

    state->show_hide_handle =
        dock_config_state_read_boolean(
            state, key_file, CONFIG_KEY_SHOW_HIDE_HANDLE, TRUE,
            &state->show_hide_handle_found);

    state->shortcut_only =
        dock_config_state_read_boolean(
            state, key_file, CONFIG_KEY_SHORTCUT_ONLY, FALSE,
            &state->shortcut_only_found);

    state->minimized_window_icons =
        dock_config_state_read_boolean(
            state, key_file, CONFIG_KEY_MINIMIZED_WINDOW_ICONS, FALSE,
            &state->minimized_window_icons_found);

    state->minimized_title_labels =
        dock_config_state_read_boolean(
            state, key_file, CONFIG_KEY_MINIMIZED_TITLE_LABELS, FALSE,
            &state->minimized_title_labels_found);

    state->minimized_group_drawer =
        dock_config_state_read_boolean(
            state, key_file, CONFIG_KEY_MINIMIZED_GROUP_DRAWER, FALSE,
            &state->minimized_group_drawer_found);

    state->minimized_all_workspaces =
        dock_config_state_read_boolean(
            state, key_file, CONFIG_KEY_MINIMIZED_ALL_WORKSPACES, FALSE,
            &state->minimized_all_workspaces_found);

    state->bitmap_background_enabled =
        dock_config_state_read_boolean(
            state, key_file, CONFIG_KEY_BITMAP_BACKGROUND_ENABLED, FALSE,
            &state->bitmap_background_enabled_found);

    gint bitmap_mode =
        dock_config_state_read_integer(
            state, key_file, CONFIG_KEY_BITMAP_BACKGROUND_MODE, 0,
            &state->bitmap_background_mode_found);
    state->bitmap_background_mode = bitmap_mode == 1 ? 1 : 0;

    state->bitmap_background_path =
        dock_config_state_read_string(
            state, key_file, CONFIG_KEY_BITMAP_BACKGROUND_PATH,
            &state->bitmap_background_path_found);
    if (!state->bitmap_background_path ||
        !*state->bitmap_background_path) {
        g_clear_pointer(&state->bitmap_background_path, g_free);
        state->bitmap_background_path_found = FALSE;
    }

    GError *items_error = NULL;
    state->items =
        dock_config_read_items_from_key_file(
            key_file,
            &state->items_found,
            &state->item_count,
            &items_error);
    dock_config_state_record_error(
        state, "Items/Applications", items_error);

    state->drawers =
        dock_config_read_drawers_from_key_file(
            key_file, &state->drawer_count);

    g_key_file_unref(key_file);
    return TRUE;
}

void
dock_config_state_clear(DockConfigState *state)
{
    if (!state)
        return;

    g_strfreev(state->items);
    dock_config_free_drawers(state->drawers, state->drawer_count);
    g_free(state->monitor_manufacturer);
    g_free(state->monitor_model);
    g_free(state->bitmap_background_path);
    g_clear_pointer(&state->warnings, g_ptr_array_unref);
    memset(state, 0, sizeof(*state));
}

gboolean
dock_config_save_state(
    const gchar *const *items,
    gsize item_count,
    const DockConfigDrawer *drawers,
    gsize drawer_count,
    gboolean on_right_side,
    gint position_mode,
    gint monitor_index,
    const gchar *monitor_manufacturer,
    const gchar *monitor_model,
    gint icon_size,
    gint opacity_percent,
    gboolean show_window_indicator,
    gboolean show_hide_handle,
    gboolean shortcut_only,
    gboolean minimized_window_icons,
    gboolean minimized_title_labels,
    gboolean minimized_group_drawer,
    gboolean minimized_all_workspaces,
    gboolean bitmap_background_enabled,
    gint bitmap_background_mode,
    const gchar *bitmap_background_path,
    GError **error)
{
    g_return_val_if_fail(
        items != NULL || item_count == 0,
        FALSE);

    g_return_val_if_fail(
        drawers != NULL || drawer_count == 0,
        FALSE);

    gchar *directory =
        dock_config_get_directory();

    if (g_mkdir_with_parents(
            directory,
            0700) != 0) {
        g_set_error(
            error,
            G_FILE_ERROR,
            g_file_error_from_errno(errno),
            "Unable to create Dock configuration directory '%s'",
            directory);
        g_free(directory);
        return FALSE;
    }

    gchar *path =
        g_build_filename(
            directory,
            CONFIG_FILENAME,
            NULL);

    GKeyFile *key_file =
        g_key_file_new();

    g_key_file_set_string_list(
        key_file,
        CONFIG_GROUP,
        CONFIG_KEY_ITEMS,
        items,
        item_count);

    g_key_file_set_string(
        key_file,
        CONFIG_GROUP,
        CONFIG_KEY_SIDE,
        on_right_side ? "right" : "left");

    g_key_file_set_string(
        key_file,
        CONFIG_GROUP,
        CONFIG_KEY_POSITION,
        position_mode == 1 ?
            "auto" :
            position_mode == 2 ?
            "top" :
            "normal");

    g_key_file_set_integer(
        key_file,
        CONFIG_GROUP,
        CONFIG_KEY_MONITOR,
        monitor_index);

    if (monitor_manufacturer &&
        *monitor_manufacturer) {
        g_key_file_set_string(
            key_file,
            CONFIG_GROUP,
            CONFIG_KEY_MONITOR_MANUFACTURER,
            monitor_manufacturer);
    } else {
        g_key_file_remove_key(
            key_file,
            CONFIG_GROUP,
            CONFIG_KEY_MONITOR_MANUFACTURER,
            NULL);
    }

    if (monitor_model &&
        *monitor_model) {
        g_key_file_set_string(
            key_file,
            CONFIG_GROUP,
            CONFIG_KEY_MONITOR_MODEL,
            monitor_model);
    } else {
        g_key_file_remove_key(
            key_file,
            CONFIG_GROUP,
            CONFIG_KEY_MONITOR_MODEL,
            NULL);
    }

    g_key_file_set_integer(
        key_file,
        CONFIG_GROUP,
        CONFIG_KEY_ICON_SIZE,
        CLAMP(icon_size, 16, 64));

    g_key_file_set_integer(
        key_file,
        CONFIG_GROUP,
        CONFIG_KEY_OPACITY,
        CLAMP(opacity_percent, 50, 100));

    g_key_file_set_boolean(
        key_file,
        CONFIG_GROUP,
        CONFIG_KEY_WINDOW_INDICATOR,
        show_window_indicator);

    g_key_file_set_boolean(
        key_file,
        CONFIG_GROUP,
        CONFIG_KEY_SHOW_HIDE_HANDLE,
        show_hide_handle);

    g_key_file_set_boolean(
        key_file,
        CONFIG_GROUP,
        CONFIG_KEY_SHORTCUT_ONLY,
        shortcut_only);

    g_key_file_set_boolean(
        key_file,
        CONFIG_GROUP,
        CONFIG_KEY_MINIMIZED_WINDOW_ICONS,
        minimized_window_icons);

    g_key_file_set_boolean(
        key_file,
        CONFIG_GROUP,
        CONFIG_KEY_MINIMIZED_TITLE_LABELS,
        minimized_title_labels);

    g_key_file_set_boolean(
        key_file,
        CONFIG_GROUP,
        CONFIG_KEY_MINIMIZED_GROUP_DRAWER,
        minimized_group_drawer);

    g_key_file_set_boolean(
        key_file,
        CONFIG_GROUP,
        CONFIG_KEY_MINIMIZED_ALL_WORKSPACES,
        minimized_all_workspaces);

    g_key_file_set_boolean(
        key_file,
        CONFIG_GROUP,
        CONFIG_KEY_BITMAP_BACKGROUND_ENABLED,
        bitmap_background_enabled);

    g_key_file_set_integer(
        key_file,
        CONFIG_GROUP,
        CONFIG_KEY_BITMAP_BACKGROUND_MODE,
        bitmap_background_mode == 1 ? 1 : 0);

    if (bitmap_background_path && *bitmap_background_path) {
        g_key_file_set_string(
            key_file,
            CONFIG_GROUP,
            CONFIG_KEY_BITMAP_BACKGROUND_PATH,
            bitmap_background_path);
    } else {
        g_key_file_remove_key(
            key_file,
            CONFIG_GROUP,
            CONFIG_KEY_BITMAP_BACKGROUND_PATH,
            NULL);
    }

    GPtrArray *applications =
        g_ptr_array_new_with_free_func(g_free);

    for (gsize i = 0; i < item_count; i++) {
        if (g_str_has_prefix(
                items[i],
                "app:")) {
            g_ptr_array_add(
                applications,
                g_strdup(
                    items[i] + 4));
        }
    }

    g_ptr_array_add(
        applications,
        NULL);

    g_key_file_set_string_list(
        key_file,
        CONFIG_GROUP,
        CONFIG_KEY_APPLICATIONS,
        (const gchar *const *)applications->pdata,
        applications->len - 1);

    g_ptr_array_free(
        applications,
        TRUE);

    /*
     * Remove previously stored Drawer groups before writing the current
     * ordered Drawer list.
     */
    gsize old_group_count = 0;
    gchar **old_groups =
        g_key_file_get_groups(
            key_file,
            &old_group_count);

    for (gsize i = 0;
         i < old_group_count;
         i++) {
        if (g_str_has_prefix(
                old_groups[i],
                CONFIG_DRAWER_PREFIX)) {
            g_key_file_remove_group(
                key_file,
                old_groups[i],
                NULL);
        }
    }

    g_strfreev(old_groups);

    for (gsize i = 0;
         i < drawer_count;
         i++) {
        gchar *group =
            g_strdup_printf(
                "%s%zu",
                CONFIG_DRAWER_PREFIX,
                i);

        g_key_file_set_string(
            key_file,
            group,
            CONFIG_DRAWER_NAME,
            drawers[i].name ?
            drawers[i].name :
            "Drawer");

        if (drawers[i].application_count > 0) {
            g_key_file_set_string_list(
                key_file,
                group,
                CONFIG_DRAWER_APPLICATIONS,
                (const gchar *const *)drawers[i].applications,
                drawers[i].application_count);
        }

        g_free(group);
    }

    gsize data_length = 0;
    GError *data_error = NULL;

    gchar *data =
        g_key_file_to_data(
            key_file,
            &data_length,
            &data_error);

    g_key_file_unref(key_file);
    g_free(directory);

    if (!data) {
        g_free(path);

        if (data_error)
            g_propagate_error(error, data_error);

        return FALSE;
    }

    GError *write_error = NULL;

    gboolean ok =
        g_file_set_contents(
            path,
            data,
            (gssize)data_length,
            &write_error);

    g_free(data);
    g_free(path);

    if (!ok && write_error)
        g_propagate_error(error, write_error);

    return ok;
}

gboolean
dock_config_save_ids(
    const gchar *const *desktop_ids,
    gsize length,
    GError **error)
{
    g_return_val_if_fail(
        desktop_ids != NULL || length == 0,
        FALSE);

    gchar *directory = dock_config_get_directory();

    if (g_mkdir_with_parents(directory, 0700) != 0) {
        g_set_error(
            error,
            G_FILE_ERROR,
            g_file_error_from_errno(errno),
            "Unable to create Dock configuration directory '%s'",
            directory);
        g_free(directory);
        return FALSE;
    }

    gchar *path =
        g_build_filename(
            directory,
            CONFIG_FILENAME,
            NULL);

    GKeyFile *key_file =
        g_key_file_new();

    g_key_file_set_string_list(
        key_file,
        CONFIG_GROUP,
        CONFIG_KEY_APPLICATIONS,
        desktop_ids,
        length);

    gsize data_length = 0;
    GError *data_error = NULL;

    gchar *data =
        g_key_file_to_data(
            key_file,
            &data_length,
            &data_error);

    g_key_file_unref(key_file);
    g_free(directory);

    if (!data) {
        g_free(path);

        if (data_error)
            g_propagate_error(error, data_error);

        return FALSE;
    }

    GError *write_error = NULL;

    gboolean ok =
        g_file_set_contents(
            path,
            data,
            (gssize)data_length,
            &write_error);

    g_free(data);
    g_free(path);

    if (!ok && write_error)
        g_propagate_error(error, write_error);

    return ok;
}
