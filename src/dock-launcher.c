/*
 * crepido
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "dock-launcher.h"

#include <gio/gdesktopappinfo.h>
#include <errno.h>
#include <stdio.h>
#include <gdk/gdk.h>
#include <gtk/gtk.h>

struct _DockLauncher {
    GAppInfo *app_info;
};

static gchar *
dock_launcher_override_path(
    const gchar *desktop_id)
{
    gchar *directory =
        g_build_filename(
            g_get_user_config_dir(),
            "crepido",
            "launchers",
            NULL);

    gchar *checksum =
        g_compute_checksum_for_string(
            G_CHECKSUM_SHA256,
            desktop_id,
            -1);

    gchar *filename =
        g_strdup_printf(
            "%s.desktop",
            checksum);

    gchar *path =
        g_build_filename(
            directory,
            filename,
            NULL);

    g_free(filename);
    g_free(checksum);
    g_free(directory);

    return path;
}

static GAppInfo *
find_desktop_id(const gchar *desktop_id)
{
    gchar *override_path =
        dock_launcher_override_path(desktop_id);

    if (g_file_test(
            override_path,
            G_FILE_TEST_IS_REGULAR)) {
        GDesktopAppInfo *override =
            g_desktop_app_info_new_from_filename(
                override_path);

        if (override) {
            g_free(override_path);
            return G_APP_INFO(override);
        }
    }

    g_free(override_path);

    /*
     * A launcher may also be backed by a local .desktop file that was
     * dropped onto the Dock.  Keep the absolute path as the persistent
     * launcher identifier so it can be loaded again after restart.
     */
    if (g_path_is_absolute(desktop_id) &&
        g_file_test(desktop_id, G_FILE_TEST_IS_REGULAR)) {
        GDesktopAppInfo *local =
            g_desktop_app_info_new_from_filename(desktop_id);

        if (local)
            return G_APP_INFO(local);
    }

    GDesktopAppInfo *direct =
        g_desktop_app_info_new(desktop_id);

    if (direct)
        return G_APP_INFO(direct);

    /*
     * Fallback for desktops that expose an application under an unusual
     * XDG registration path.
     */
    GList *apps = g_app_info_get_all();
    GAppInfo *found = NULL;

    for (GList *iter = apps; iter; iter = iter->next) {
        GAppInfo *app = G_APP_INFO(iter->data);

        if (!G_IS_DESKTOP_APP_INFO(app))
            continue;

        const gchar *filename =
            g_desktop_app_info_get_filename(
                G_DESKTOP_APP_INFO(app));

        if (filename) {
            gchar *basename =
                g_path_get_basename(filename);

            gboolean match =
                g_strcmp0(basename, desktop_id) == 0;

            g_free(basename);

            if (match) {
                found = g_object_ref(app);
                break;
            }
        }
    }

    g_list_free_full(
        apps,
        g_object_unref);

    return found;
}

DockLauncher *
dock_launcher_new_from_desktop_id(const gchar *desktop_id)
{
    g_return_val_if_fail(desktop_id != NULL, NULL);

    GAppInfo *app = find_desktop_id(desktop_id);
    if (!app)
        return NULL;

    DockLauncher *launcher = g_new0(DockLauncher, 1);
    launcher->app_info = app;
    return launcher;
}

DockLauncher *
dock_launcher_new_default(void)
{
    /*
     * Temporary Stage 3 demonstration launcher. Persistent Dock
     * configuration will select applications explicitly in a later stage.
     */
    static const gchar *const candidates[] = {
        "org.mate.Terminal.desktop",
        "mate-terminal.desktop",
        "org.gnome.Terminal.desktop",
        "xfce4-terminal.desktop",
        NULL
    };

    for (gsize i = 0; candidates[i]; i++) {
        DockLauncher *launcher =
            dock_launcher_new_from_desktop_id(candidates[i]);

        if (launcher)
            return launcher;
    }

    return NULL;
}

void
dock_launcher_free(DockLauncher *launcher)
{
    if (!launcher)
        return;

    g_clear_object(&launcher->app_info);
    g_free(launcher);
}

gboolean
dock_launcher_save_override(
    const gchar *desktop_id,
    const gchar *name,
    const gchar *exec,
    const gchar *icon,
    gboolean terminal,
    gboolean dbus_activatable,
    GError **error)
{
    g_return_val_if_fail(desktop_id != NULL, FALSE);
    g_return_val_if_fail(name != NULL, FALSE);
    g_return_val_if_fail(exec != NULL, FALSE);

    gchar *directory =
        g_build_filename(
            g_get_user_config_dir(),
            "crepido",
            "launchers",
            NULL);

    if (g_mkdir_with_parents(directory, 0700) != 0) {
        g_set_error(
            error,
            G_FILE_ERROR,
            g_file_error_from_errno(errno),
            "Unable to create launcher override directory '%s'",
            directory);
        g_free(directory);
        return FALSE;
    }

    GKeyFile *key_file =
        g_key_file_new();

    g_key_file_set_string(
        key_file,
        "Desktop Entry",
        "Type",
        "Application");

    g_key_file_set_string(
        key_file,
        "Desktop Entry",
        "Version",
        "1.0");

    g_key_file_set_string(
        key_file,
        "Desktop Entry",
        "Name",
        name);

    if (exec && *exec) {
        g_key_file_set_string(
            key_file,
            "Desktop Entry",
            "Exec",
            exec);
    }

    if (icon && *icon) {
        g_key_file_set_string(
            key_file,
            "Desktop Entry",
            "Icon",
            icon);
    }

    g_key_file_set_boolean(
        key_file,
        "Desktop Entry",
        "Terminal",
        terminal);

    g_key_file_set_boolean(
        key_file,
        "Desktop Entry",
        "DBusActivatable",
        dbus_activatable);

    GAppInfo *original_app =
        find_desktop_id(desktop_id);

    GDesktopAppInfo *original =
        original_app &&
        G_IS_DESKTOP_APP_INFO(original_app) ?
        G_DESKTOP_APP_INFO(original_app) :
        NULL;

    if (original) {
        gchar *startup_wm_class =
            g_desktop_app_info_get_string(
                original,
                "StartupWMClass");

        if (startup_wm_class && *startup_wm_class) {
            g_key_file_set_string(
                key_file,
                "Desktop Entry",
                "StartupWMClass",
                startup_wm_class);
        }

        g_free(startup_wm_class);

        if (g_desktop_app_info_get_boolean(
                original,
                "StartupNotify")) {
            g_key_file_set_boolean(
                key_file,
                "Desktop Entry",
                "StartupNotify",
                TRUE);
        }

        gchar *try_exec =
            g_desktop_app_info_get_string(
                original,
                "TryExec");

        if (try_exec && *try_exec) {
            g_key_file_set_string(
                key_file,
                "Desktop Entry",
                "TryExec",
                try_exec);
        }

        g_free(try_exec);
    }

    g_clear_object(&original_app);

    g_key_file_set_string(
        key_file,
        "Desktop Entry",
        "X-Crepido-DesktopId",
        desktop_id);

    gsize data_length = 0;
    GError *data_error = NULL;

    gchar *data =
        g_key_file_to_data(
            key_file,
            &data_length,
            &data_error);

    g_key_file_unref(key_file);

    if (!data) {
        if (data_error)
            g_propagate_error(error, data_error);
        g_free(directory);
        return FALSE;
    }

    gchar *checksum =
        g_compute_checksum_for_string(
            G_CHECKSUM_SHA256,
            desktop_id,
            -1);

    gchar *filename =
        g_strdup_printf(
            "%s.desktop",
            checksum);

    gchar *path =
        g_build_filename(
            directory,
            filename,
            NULL);

    gboolean ok =
        g_file_set_contents(
            path,
            data,
            (gssize)data_length,
            error);

    g_free(path);
    g_free(filename);
    g_free(checksum);
    g_free(data);
    g_free(directory);

    return ok;
}

gboolean
dock_launcher_reset_override(
    const gchar *desktop_id,
    GError **error)
{
    g_return_val_if_fail(desktop_id != NULL, FALSE);

    gchar *path =
        dock_launcher_override_path(desktop_id);

    if (!g_file_test(
            path,
            G_FILE_TEST_EXISTS)) {
        g_free(path);
        return TRUE;
    }

    gboolean ok =
        remove(path) == 0;

    if (!ok)
        g_set_error(
            error,
            G_FILE_ERROR,
            g_file_error_from_errno(errno),
            "Unable to remove launcher override '%s'",
            path);

    g_free(path);
    return ok;
}

const gchar *
dock_launcher_get_name(const DockLauncher *launcher)
{
    g_return_val_if_fail(launcher != NULL, NULL);
    return g_app_info_get_display_name(launcher->app_info);
}

GAppInfo *
dock_launcher_get_app_info(const DockLauncher *launcher)
{
    g_return_val_if_fail(launcher != NULL, NULL);
    return launcher->app_info;
}

static void
dock_launcher_child_watch(
    GPid pid,
    gint status,
    gpointer user_data)
{
    (void)status;
    (void)user_data;

    g_spawn_close_pid(pid);
}

static void
dock_launcher_pid_callback(
    GDesktopAppInfo *app_info,
    GPid pid,
    gpointer user_data)
{
    (void)app_info;
    (void)user_data;

    g_child_watch_add(
        pid,
        dock_launcher_child_watch,
        NULL);
}

gboolean
dock_launcher_launch(
    const DockLauncher *launcher,
    GtkWidget *source_widget,
    guint32 timestamp,
    GError **error)
{
    g_return_val_if_fail(launcher != NULL, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

    GdkDisplay *display =
        source_widget ?
        gtk_widget_get_display(source_widget) :
        gdk_display_get_default();

    if (!GDK_IS_DISPLAY(display))
        return g_app_info_launch(
            launcher->app_info,
            NULL,
            NULL,
            error);

    GdkScreen *screen =
        source_widget ?
        gtk_widget_get_screen(source_widget) :
        gdk_screen_get_default();

    GdkAppLaunchContext *context =
        gdk_display_get_app_launch_context(display);

    if (screen)
        gdk_app_launch_context_set_screen(
            context,
            screen);

    gdk_app_launch_context_set_timestamp(
        context,
        timestamp != 0 ?
        timestamp :
        gtk_get_current_event_time());

    gboolean launched = FALSE;

    if (G_IS_DESKTOP_APP_INFO(launcher->app_info)) {
        launched =
            g_desktop_app_info_launch_uris_as_manager(
                G_DESKTOP_APP_INFO(launcher->app_info),
                NULL,
                G_APP_LAUNCH_CONTEXT(context),
                G_SPAWN_SEARCH_PATH |
                G_SPAWN_DO_NOT_REAP_CHILD,
                NULL,
                NULL,
                dock_launcher_pid_callback,
                NULL,
                error);
    } else {
        launched =
            g_app_info_launch(
                launcher->app_info,
                NULL,
                G_APP_LAUNCH_CONTEXT(context),
                error);
    }

    g_object_unref(context);

    return launched;
}
