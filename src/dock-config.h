/*
 * crepido
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/*
 * Loads the user's Dock application order.
 *
 * @found is set TRUE when a configuration file exists and contains an
 * application list. The returned array is NULL-terminated and owned by
 * the caller; release it with g_strfreev().
 */
gchar **dock_config_load_ids(gboolean *found, GError **error);

/*
 * Saves an ordered NULL-terminated list of desktop IDs.
 */
gboolean dock_config_save_ids(
    const gchar *const *desktop_ids,
    gsize length,
    GError **error);

typedef struct {
    gchar *name;
    gchar **applications;
    gsize application_count;
} DockConfigDrawer;

/*
 * Startup snapshot of the user's configuration. All values are read from
 * one GKeyFile instance; each *_found flag distinguishes an absent setting
 * from an explicitly configured value. Initialize with {0} and clear it
 * with dock_config_state_clear() when finished.
 */
typedef struct {
    gboolean side_found;
    gboolean on_right_side;
    gboolean position_found;
    gint position_mode;
    gboolean monitor_found;
    gint monitor_index;
    gboolean monitor_manufacturer_found;
    gchar *monitor_manufacturer;
    gboolean monitor_model_found;
    gchar *monitor_model;
    gboolean icon_size_found;
    gint icon_size;
    gboolean opacity_found;
    gint opacity;
    gboolean window_indicator_found;
    gboolean show_window_indicator;
    gboolean show_hide_handle_found;
    gboolean show_hide_handle;
    gboolean shortcut_only_found;
    gboolean shortcut_only;
    gboolean minimized_window_icons_found;
    gboolean minimized_window_icons;
    gboolean minimized_title_labels_found;
    gboolean minimized_title_labels;
    gboolean minimized_group_drawer_found;
    gboolean minimized_group_drawer;
    gboolean minimized_all_workspaces_found;
    gboolean minimized_all_workspaces;
    gboolean bitmap_background_enabled_found;
    gboolean bitmap_background_enabled;
    gboolean bitmap_background_mode_found;
    gint bitmap_background_mode;
    gboolean bitmap_background_path_found;
    gchar *bitmap_background_path;
    gboolean items_found;
    gchar **items;
    gsize item_count;
    DockConfigDrawer *drawers;
    gsize drawer_count;
    GPtrArray *warnings;
} DockConfigState;

gchar **dock_config_load_items(
    gboolean *found,
    gsize *length,
    GError **error);

gboolean dock_config_load_side(
    gboolean *found,
    GError **error);

gint dock_config_load_position(
    gboolean *found,
    GError **error);

gint dock_config_load_monitor(
    gboolean *found,
    GError **error);

gchar *dock_config_load_monitor_manufacturer(
    gboolean *found,
    GError **error);

gchar *dock_config_load_monitor_model(
    gboolean *found,
    GError **error);

gint dock_config_load_icon_size(
    gboolean *found,
    GError **error);

gint dock_config_load_opacity(
    gboolean *found,
    GError **error);

gboolean dock_config_load_bitmap_background_enabled(
    gboolean *found,
    GError **error);

gint dock_config_load_bitmap_background_mode(
    gboolean *found,
    GError **error);

gchar *dock_config_load_bitmap_background_path(
    gboolean *found,
    GError **error);

gboolean dock_config_load_window_indicator(
    gboolean *found,
    GError **error);

gboolean dock_config_load_show_hide_handle(
    gboolean *found,
    GError **error);

gboolean dock_config_load_minimized_window_icons(
    gboolean *found,
    GError **error);

gboolean dock_config_load_minimized_title_labels(
    gboolean *found,
    GError **error);

gboolean dock_config_load_minimized_group_drawer(
    gboolean *found,
    GError **error);

gboolean dock_config_load_minimized_all_workspaces(
    gboolean *found,
    GError **error);

gboolean dock_config_load_shortcut_only(
    gboolean *found,
    GError **error);

DockConfigDrawer *dock_config_load_drawers(
    gsize *length,
    GError **error);

void dock_config_free_drawers(
    DockConfigDrawer *drawers,
    gsize length);

/*
 * Reads startup settings and launcher/drawer layout using one config-file
 * parse. Returns FALSE when no readable config file is present; a missing
 * file is normal and does not set @error. Per-setting errors are collected
 * in state->warnings so other valid settings can still be loaded.
 */
gboolean dock_config_load_state(
    DockConfigState *state,
    GError **error);

void dock_config_state_clear(DockConfigState *state);

gboolean dock_config_save_state(
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
    GError **error);

G_END_DECLS
