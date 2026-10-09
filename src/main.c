/*
 * crepido
 *
 * Stage 4: ordered multi-icon Dock.
 *
 * This project is an independent implementation. Crepido follows
 * a Dock-oriented desktop design reference and does not depend on another window manager at runtime.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <gtk/gtk.h>
#include "dock.h"
#include "dock-config.h"
#include "dock-monitor.h"
#include "dock-clip.h"

#define DOCK_TILE_SIZE 64

static GtkWidget *dock_window = NULL;
static DockClip *workspace_clip = NULL;

static gint
find_monitor_by_identity(
    GtkWidget *widget,
    const gchar *manufacturer,
    const gchar *model,
    gint fallback_index)
{
    if (!widget)
        return fallback_index;

    GdkDisplay *display =
        gtk_widget_get_display(widget);

    if (!display)
        return fallback_index;

    gint monitor_count =
        gdk_display_get_n_monitors(display);

    DockMonitorIdentity *identities =
        g_new0(
            DockMonitorIdentity,
            (gsize)MAX(0, monitor_count));

    for (gint i = 0; i < monitor_count; i++) {
        GdkMonitor *monitor =
            gdk_display_get_monitor(
                display,
                i);

        if (!monitor)
            continue;

        identities[i].manufacturer =
            gdk_monitor_get_manufacturer(monitor);
        identities[i].model =
            gdk_monitor_get_model(monitor);
    }

    /*
     * Prefer the persisted slot when it still identifies the expected
     * monitor. This matters when two connected displays report identical
     * manufacturer/model strings. If the saved slot no longer matches,
     * search for the identity before falling back to the saved index.
     */
    gint selected_index =
        dock_monitor_find_identity_index(
            manufacturer,
            model,
            identities,
            (gsize)MAX(0, monitor_count),
            fallback_index);

    g_free(identities);

    return selected_index;
}

static void
activate(GtkApplication *app, gpointer user_data)
{
    (void)user_data;

    /*
     * GApplication is unique, so a second manual launch or another
     * autostart invocation reuses the existing Dock instead of creating
     * a second Dock window.
     */
    if (dock_window) {
        gtk_window_present(GTK_WINDOW(dock_window));
        return;
    }

    GtkWidget *window = gtk_application_window_new(app);

    dock_window = window;
    g_object_add_weak_pointer(
        G_OBJECT(window),
        (gpointer *)&dock_window);

    gtk_window_set_title(GTK_WINDOW(window), "Crepido GTK3");
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(window), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(window), TRUE);
    /*
     * Mark the main toplevel as an X11 dock window. This matches the
     * Crepido's X11 dock role while the explicit monitor
     * positioning below keeps it anchored to the selected screen edge.
     */
    gtk_window_set_type_hint(
        GTK_WINDOW(window),
        GDK_WINDOW_TYPE_HINT_DOCK);
    gtk_window_set_keep_below(
        GTK_WINDOW(window),
        TRUE);
    gtk_window_set_accept_focus(
        GTK_WINDOW(window),
        FALSE);
    gtk_window_set_focus_on_map(
        GTK_WINDOW(window),
        FALSE);

    Dock *dock = dock_new();

    DockConfigState config = {0};
    GError *config_error = NULL;
    if (!dock_config_load_state(&config, &config_error) &&
        config_error) {
        g_warning(
            "Unable to load Dock configuration: %s",
            config_error->message);
        g_clear_error(&config_error);
    }

    for (guint i = 0;
         config.warnings && i < config.warnings->len;
         i++) {
        g_warning(
            "Unable to load Dock setting: %s",
            (const gchar *)g_ptr_array_index(config.warnings, i));
    }

    gint configured_monitor =
        find_monitor_by_identity(
            window,
            config.monitor_manufacturer_found ?
                config.monitor_manufacturer :
                NULL,
            config.monitor_model_found ?
                config.monitor_model :
                NULL,
            config.monitor_index);

    dock_set_monitor_index(dock, configured_monitor);

    if (config.icon_size_found)
        dock_set_icon_size(dock, config.icon_size);

    dock_set_right_side(dock, config.on_right_side);
    dock_set_position_mode(
        dock,
        (DockPositionMode)config.position_mode);

    if (config.opacity_found)
        dock_set_opacity(dock, config.opacity);

    if (config.bitmap_background_enabled_found ||
        config.bitmap_background_path_found ||
        config.bitmap_background_mode_found) {
        dock_set_bitmap_background(
            dock,
            config.bitmap_background_enabled,
            config.bitmap_background_path,
            config.bitmap_background_mode ==
                DOCK_BITMAP_BACKGROUND_EXTEND ?
                    DOCK_BITMAP_BACKGROUND_EXTEND :
                    DOCK_BITMAP_BACKGROUND_REPEAT);
    }

    if (config.window_indicator_found)
        dock_set_show_window_indicator(
            dock,
            config.show_window_indicator);

    dock_set_show_hide_handle(dock, config.show_hide_handle);

    gboolean shortcut_only_found = config.shortcut_only_found;
    gboolean shortcut_only = config.shortcut_only;
    gboolean minimized_window_icons_found =
        config.minimized_window_icons_found;
    gboolean minimized_window_icons = config.minimized_window_icons;
    gboolean minimized_title_labels_found =
        config.minimized_title_labels_found;
    gboolean minimized_title_labels = config.minimized_title_labels;
    gboolean minimized_group_drawer_found =
        config.minimized_group_drawer_found;
    gboolean minimized_group_drawer = config.minimized_group_drawer;
    gboolean minimized_all_workspaces_found =
        config.minimized_all_workspaces_found;
    gboolean minimized_all_workspaces = config.minimized_all_workspaces;

    gboolean config_found = config.items_found;
    gsize configured_item_count = config.item_count;
    gchar **configured_items = config.items;
    gsize configured_drawer_count = config.drawer_count;
    DockConfigDrawer *configured_drawers = config.drawers;

    static const gchar *const default_ids[] = {
        "org.mate.Terminal.desktop",
        "mate-terminal.desktop",
        "caja.desktop",
        "org.gnome.Nautilus.desktop",
        "firefox.desktop",
        "org.mozilla.firefox.desktop",
        NULL
    };

    if (config_found) {
        for (gsize i = 0;
             i < configured_item_count;
             i++) {
            const gchar *item =
                configured_items[i];

            if (g_str_has_prefix(item, "app:")) {
                dock_add_desktop_id(
                    dock,
                    item + 4);
                continue;
            }

            if (g_str_has_prefix(item, "drawer:")) {
                gchar *parse_end = NULL;

                guint64 drawer_index =
                    g_ascii_strtoull(
                        item + 7,
                        &parse_end,
                        10);

                if (parse_end &&
                    *parse_end == '\0' &&
                    drawer_index < configured_drawer_count) {
                    DockConfigDrawer *drawer =
                        &configured_drawers[drawer_index];

                    dock_add_drawer(
                        dock,
                        drawer->name,
                        (const gchar *const *)
                            drawer->applications,
                        drawer->application_count);
                }
            }
        }
    } else {
        for (gsize i = 0;
             default_ids[i];
             i++) {
            dock_add_desktop_id(
                dock,
                default_ids[i]);
        }
    }

    /*
     * A damaged configuration must not leave the Dock without at least one
     * normal application shortcut.
     */
    if (dock_get_application_count(dock) == 0) {
        config_found = FALSE;

        for (gsize i = 0;
             default_ids[i];
             i++) {
            dock_add_desktop_id(
                dock,
                default_ids[i]);
        }
    }

    if (!config_found &&
        dock_get_application_count(dock) > 0) {
        dock_save_configuration(dock);
    }

    if (dock_get_item_count(dock) == 0) {
        g_warning("No supported desktop applications were found");
    }

    gtk_container_add(
        GTK_CONTAINER(window),
        dock_get_widget(dock));

    gtk_widget_show_all(window);


    dock_set_opacity(
        dock,
        dock_get_opacity(dock));

    dock_set_position_mode(
        dock,
        dock_get_position_mode(dock));

    if (shortcut_only_found)
        dock_set_shortcut_only(
            dock,
            shortcut_only);

    if (minimized_window_icons_found)
        dock_set_minimized_window_icons(
            dock,
            minimized_window_icons);

    if (minimized_title_labels_found)
        dock_set_minimized_title_labels(
            dock,
            minimized_title_labels);

    if (minimized_group_drawer_found)
        dock_set_minimized_group_drawer(
            dock,
            minimized_group_drawer);

    if (minimized_all_workspaces_found)
        dock_set_minimized_all_workspaces(
            dock,
            minimized_all_workspaces);

    /*
     * Apply the configured monitor now that the Dock toplevel exists.
     * dock_set_monitor_index() keeps this monitor assignment when the
     * Dock side is swapped.
     */
    dock_set_monitor_index(
        dock,
        dock_get_monitor_index(dock));

    /* The Clip uses the same selected monitor, opacity and stacking mode. */
    workspace_clip = dock_clip_new(dock);

    /*
     * The Dock stays alive for the application's lifetime. It is freed
     * by the application shutdown path below.
     */
    dock_config_state_clear(&config);

    g_object_set_data_full(
        G_OBJECT(window), "dock-model", dock, (GDestroyNotify)dock_free);
}

/*
 * Release the Dock model before destroying its application window. The Dock
 * owns cleanup of its child widget and X11 resources, so its widget must
 * still be valid while dock_free() runs.
 */
static void
shutdown_dock(void)
{
    if (!dock_window)
        return;

    if (workspace_clip) {
        dock_clip_free(workspace_clip);
        workspace_clip = NULL;
    }

    GtkWidget *window = g_object_ref(dock_window);
    Dock *dock =
        g_object_steal_data(
            G_OBJECT(window),
            "dock-model");

    if (dock)
        dock_free(dock);

    gtk_widget_destroy(window);
    g_object_unref(window);
}

int
main(int argc, char **argv)
{
    GtkApplication *app = gtk_application_new(
        "org.loonylynn.crepido",
        G_APPLICATION_DEFAULT_FLAGS);

    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    shutdown_dock();
    g_object_unref(app);
    return status;
}
