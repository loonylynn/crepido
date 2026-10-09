/*
 * crepido
 *
 * X11 application discovery and activation for Dock launchers.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gtk/gtk.h>
#include <X11/Xlib.h>

G_BEGIN_DECLS

typedef struct _DockX11 DockX11;
typedef struct _DockLauncher DockLauncher;

DockX11 *dock_x11_new(GtkWidget *container);
void dock_x11_free(DockX11 *x11);

Window dock_x11_get_active_window(DockX11 *x11);

gboolean dock_x11_activate_button(
    DockX11 *x11,
    GtkWidget *button,
    guint32 timestamp,
    Window active_before_click);

void dock_x11_refresh_button(
    DockX11 *x11,
    GtkWidget *button);

void dock_x11_set_window_indicator_enabled(
    DockX11 *x11,
    gboolean enabled);

void dock_x11_set_shortcut_only(
    DockX11 *x11,
    gboolean enabled);

void dock_x11_set_minimized_window_icons(
    DockX11 *x11,
    gboolean enabled);

void dock_x11_set_minimized_title_labels(
    DockX11 *x11,
    gboolean enabled);

void dock_x11_set_minimized_group_drawer(
    DockX11 *x11,
    gboolean enabled);

void dock_x11_set_minimized_all_workspaces(
    DockX11 *x11,
    gboolean enabled);

void dock_x11_set_icon_size(
    DockX11 *x11,
    gint icon_size);

void dock_x11_set_opacity(
    DockX11 *x11,
    gint opacity_percent);

void dock_x11_close_group_for_ancestor(
    DockX11 *x11,
    GtkWidget *ancestor);

void dock_x11_close_group_popup(
    DockX11 *x11);

void dock_x11_set_group_popup_suppressed(
    DockX11 *x11,
    gboolean suppressed);

gint dock_x11_prepare_group_popup_for_press(
    DockX11 *x11,
    GtkWidget *button);

gboolean dock_x11_append_window_menu(
    DockX11 *x11,
    GtkWidget *button,
    GtkMenuShell *menu_shell);

gboolean dock_x11_group_popup_point_inside(
    DockX11 *x11,
    GtkWidget *ancestor,
    gint root_x,
    gint root_y);

G_END_DECLS
