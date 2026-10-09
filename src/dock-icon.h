/*
 * crepido
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

#include "dock-launcher.h"

G_BEGIN_DECLS

GtkWidget *dock_icon_button_new(DockLauncher *launcher);
GtkWidget *dock_icon_button_new_drag_preview(DockLauncher *launcher);
void dock_icon_button_set_icon_size(
    GtkWidget *button,
    gint icon_size);
void dock_icon_button_set_launcher(
    GtkWidget *button,
    DockLauncher *launcher);

void dock_icon_button_set_instance_count(GtkWidget *button, guint count);

void dock_icon_button_set_launching(
    GtkWidget *button,
    gboolean launching);

G_END_DECLS
