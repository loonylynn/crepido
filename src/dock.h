/*
 * crepido
 *
 * Stage 4: ordered Dock icon model and vertical slot container.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct _Dock Dock;

typedef enum {
    DOCK_POSITION_NORMAL = 0,
    DOCK_POSITION_AUTO_RAISE_LOWER = 1,
    DOCK_POSITION_KEEP_ON_TOP = 2
} DockPositionMode;

typedef enum {
    DOCK_BITMAP_BACKGROUND_REPEAT = 0,
    DOCK_BITMAP_BACKGROUND_EXTEND = 1
} DockBitmapBackgroundMode;

Dock *dock_new(void);
void dock_free(Dock *dock);

void dock_save_configuration(Dock *dock);

gboolean dock_add_desktop_id(Dock *dock, const gchar *desktop_id);
gboolean dock_add_drawer(
    Dock *dock,
    const gchar *name,
    const gchar *const *desktop_ids,
    gsize desktop_id_count);
guint dock_get_item_count(const Dock *dock);
guint dock_get_application_count(const Dock *dock);
gchar **dock_dup_desktop_ids(const Dock *dock);
gboolean dock_is_dragging(const Dock *dock);

void dock_set_right_side(
    Dock *dock,
    gboolean on_right_side);

gboolean dock_get_right_side(
    const Dock *dock);

void dock_set_show_window_indicator(
    Dock *dock,
    gboolean enabled);

void dock_set_show_hide_handle(
    Dock *dock,
    gboolean enabled);

gboolean dock_get_show_hide_handle(
    const Dock *dock);

void dock_set_icon_size(
    Dock *dock,
    gint icon_size);

gint dock_get_icon_size(
    const Dock *dock);

void dock_set_opacity(
    Dock *dock,
    gint opacity_percent);

void dock_set_bitmap_background(
    Dock *dock,
    gboolean enabled,
    const gchar *image_path,
    DockBitmapBackgroundMode mode);

gint dock_get_opacity(
    const Dock *dock);

gboolean dock_get_show_window_indicator(
    const Dock *dock);

void dock_set_shortcut_only(
    Dock *dock,
    gboolean enabled);

gboolean dock_get_shortcut_only(
    const Dock *dock);

void dock_set_minimized_window_icons(
    Dock *dock,
    gboolean enabled);

gboolean dock_get_minimized_window_icons(
    const Dock *dock);

void dock_set_minimized_title_labels(
    Dock *dock,
    gboolean enabled);

gboolean dock_get_minimized_title_labels(
    const Dock *dock);

void dock_set_minimized_group_drawer(
    Dock *dock,
    gboolean enabled);

gboolean dock_get_minimized_group_drawer(
    const Dock *dock);

void dock_set_minimized_all_workspaces(
    Dock *dock,
    gboolean enabled);

gboolean dock_get_minimized_all_workspaces(
    const Dock *dock);

void dock_set_monitor_index(
    Dock *dock,
    gint monitor_index);

gint dock_get_monitor_index(
    const Dock *dock);

void dock_set_position_mode(
    Dock *dock,
    DockPositionMode mode);

DockPositionMode dock_get_position_mode(
    const Dock *dock);

GtkWidget *dock_get_widget(Dock *dock);

G_END_DECLS
