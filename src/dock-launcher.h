/*
 * crepido
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gio/gio.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct _DockLauncher DockLauncher;

DockLauncher *dock_launcher_new_from_desktop_id(const gchar *desktop_id);
DockLauncher *dock_launcher_new_default(void);
void dock_launcher_free(DockLauncher *launcher);

const gchar *dock_launcher_get_name(const DockLauncher *launcher);
GAppInfo *dock_launcher_get_app_info(const DockLauncher *launcher);

gboolean dock_launcher_save_override(
    const gchar *desktop_id,
    const gchar *name,
    const gchar *exec,
    const gchar *icon,
    gboolean terminal,
    gboolean dbus_activatable,
    GError **error);

gboolean dock_launcher_reset_override(
    const gchar *desktop_id,
    GError **error);

gboolean dock_launcher_launch(
    const DockLauncher *launcher,
    GtkWidget *source_widget,
    guint32 timestamp,
    GError **error);

G_END_DECLS
