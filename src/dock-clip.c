/*
 * crepido
 *
 * Small Window Maker-inspired workspace Clip for MATE/Marco on X11.
 * Uses EWMH root properties and client messages rather than maintaining a
 * second, independent workspace state.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "dock-clip.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <gdk/gdkx.h>

#include <string.h>

struct _DockClip {
    Dock *dock;
    GtkWidget *window;
    GtkWidget *button;
    GtkWidget *workspace_image;
    GtkWidget *workspace_label;
    GtkWidget *workspace_menu;
    GdkDisplay *gdk_display;
    GdkScreen *screen;
    Display *display;
    Window root;
    Atom atom_current_desktop;
    Atom atom_number_of_desktops;
    Atom atom_desktop_names;
    Atom atom_utf8_string;
    guint refresh_id;
    guint current_desktop;
    guint number_of_desktops;
    gulong monitor_added_handler;
    gulong monitor_removed_handler;
    gulong monitors_changed_handler;
    gulong screen_size_changed_handler;
    gint last_x;
    gint last_y;
    gint last_tile_size;
    gint last_icon_size;
    gint last_opacity;
    gint last_position_mode;
};

static gboolean dock_clip_refresh(gpointer user_data);
static void dock_clip_show_workspace_menu(DockClip *clip);
static void dock_clip_close_workspace_menu(DockClip *clip);
static void dock_clip_sync_dock_settings(DockClip *clip);

static gboolean
dock_clip_read_cardinal(
    DockClip *clip,
    Atom property,
    guint32 *value)
{
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char *data = NULL;

    int status = XGetWindowProperty(
        clip->display,
        clip->root,
        property,
        0L,
        1L,
        False,
        XA_CARDINAL,
        &actual_type,
        &actual_format,
        &item_count,
        &bytes_after,
        &data);

    if (status != Success ||
        actual_type != XA_CARDINAL ||
        actual_format != 32 ||
        item_count < 1 ||
        !data) {
        if (data)
            XFree(data);
        return FALSE;
    }

    /* Xlib represents format-32 property data as longs, including on LP64. */
    *value = (guint32)((unsigned long *)data)[0];
    XFree(data);
    return TRUE;
}

static gchar **
dock_clip_read_workspace_names(
    DockClip *clip,
    guint count)
{
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char *data = NULL;

    gchar **names = g_new0(gchar *, (gsize)count + 1);

    int status = XGetWindowProperty(
        clip->display,
        clip->root,
        clip->atom_desktop_names,
        0L,
        16384L,
        False,
        clip->atom_utf8_string,
        &actual_type,
        &actual_format,
        &item_count,
        &bytes_after,
        &data);

    if (status != Success ||
        actual_type != clip->atom_utf8_string ||
        actual_format != 8 ||
        !data) {
        if (data)
            XFree(data);
        return names;
    }

    gsize offset = 0;
    for (guint i = 0; i < count && offset < item_count; i++) {
        gsize length = 0;
        while (offset + length < item_count &&
               data[offset + length] != '\0') {
            length++;
        }

        if (length > 0) {
            names[i] = g_utf8_make_valid(
                (const gchar *)data + offset,
                (gssize)length);
        } else {
            names[i] = g_strdup("");
        }

        offset += length;
        if (offset < item_count && data[offset] == '\0')
            offset++;
    }

    XFree(data);
    return names;
}

static void
dock_clip_update_display(DockClip *clip)
{
    guint32 current = clip->current_desktop;
    guint32 count = clip->number_of_desktops;

    dock_clip_read_cardinal(
        clip,
        clip->atom_current_desktop,
        &current);

    if (!dock_clip_read_cardinal(
            clip,
            clip->atom_number_of_desktops,
            &count) || count == 0) {
        count = MAX(clip->number_of_desktops, 1U);
    }

    /* Keep menu size bounded if a broken window manager publishes nonsense. */
    count = MIN(count, 256U);
    current = MIN(current, count - 1);

    /* If the workspace changed outside this menu (for example through MATE's
     * Workspace Switcher or a keyboard shortcut), dismiss the stale menu.
     * Its radio checks are a snapshot and should not remain visible out of date. */
    if ((current != clip->current_desktop ||
         count != clip->number_of_desktops) &&
        clip->workspace_menu) {
        dock_clip_close_workspace_menu(clip);
    }

    clip->current_desktop = current;
    clip->number_of_desktops = count;

    gchar *label = g_strdup_printf("%u", current + 1);

    if (g_strcmp0(
            gtk_label_get_text(GTK_LABEL(clip->workspace_label)),
            label) != 0) {
        gtk_label_set_text(
            GTK_LABEL(clip->workspace_label),
            label);
    }
    g_free(label);

    gchar **names = dock_clip_read_workspace_names(clip, count);
    const gchar *name = names ? names[current] : NULL;
    gchar *tooltip = NULL;

    if (name && *name) {
        tooltip = g_strdup_printf(
            "%s (workspace %u of %u)\nClick to choose a workspace",
            name,
            current + 1,
            count);
    } else {
        tooltip = g_strdup_printf(
            "Workspace %u of %u\nClick to choose a workspace",
            current + 1,
            count);
    }

    gtk_widget_set_tooltip_text(clip->button, tooltip);
    g_free(tooltip);
    g_strfreev(names);
}

static gboolean
dock_clip_refresh(gpointer user_data)
{
    DockClip *clip = user_data;
    dock_clip_sync_dock_settings(clip);
    dock_clip_update_display(clip);
    return G_SOURCE_CONTINUE;
}

static void
dock_clip_request_workspace(
    DockClip *clip,
    guint workspace)
{
    if (!clip || workspace >= clip->number_of_desktops ||
        workspace == clip->current_desktop)
        return;

    XEvent event;
    memset(&event, 0, sizeof(event));
    event.xclient.type = ClientMessage;
    event.xclient.window = clip->root;
    event.xclient.message_type = clip->atom_current_desktop;
    event.xclient.format = 32;
    event.xclient.data.l[0] = (long)workspace;
    event.xclient.data.l[1] = (long)gtk_get_current_event_time();
    event.xclient.data.l[2] = 0;
    event.xclient.data.l[3] = 0;
    event.xclient.data.l[4] = 0;

    XSendEvent(
        clip->display,
        clip->root,
        False,
        SubstructureRedirectMask | SubstructureNotifyMask,
        &event);
    XFlush(clip->display);
}

static void
dock_clip_workspace_activated(
    GtkMenuItem *menu_item,
    gpointer user_data)
{
    DockClip *clip = user_data;

    if (!clip ||
        !GTK_IS_CHECK_MENU_ITEM(menu_item) ||
        !gtk_check_menu_item_get_active(
            GTK_CHECK_MENU_ITEM(menu_item)) ||
        !clip->workspace_menu)
        return;

    /* Ignore callbacks from an old popup that is being dismissed. In
     * particular, changing radio-group state while the menu is closing must
     * never send an unsolicited request back to workspace 1. */
    GtkWidget *menu =
        gtk_widget_get_parent(GTK_WIDGET(menu_item));
    if (!GTK_IS_MENU(menu) || menu != clip->workspace_menu)
        return;

    guint stored = GPOINTER_TO_UINT(
        g_object_get_data(
            G_OBJECT(menu_item),
            "dock-clip-workspace-index"));
    if (stored == 0)
        return;

    guint32 current = 0;
    guint32 count = 0;

    /* Read the live EWMH state directly. Do not call
     * dock_clip_update_display() here: it can close a stale menu during this
     * callback, after which an outdated item could still issue a request. */
    if (!dock_clip_read_cardinal(
            clip,
            clip->atom_current_desktop,
            &current) ||
        !dock_clip_read_cardinal(
            clip,
            clip->atom_number_of_desktops,
            &count) ||
        count == 0)
        return;

    count = MIN(count, 256U);
    current = MIN(current, count - 1);
    clip->current_desktop = current;
    clip->number_of_desktops = count;

    guint target = stored - 1;
    if (target >= count || target == current)
        return;

    dock_clip_request_workspace(clip, target);
}

static void
dock_clip_close_workspace_menu(DockClip *clip)
{
    if (!clip || !clip->workspace_menu)
        return;

    GtkWidget *menu = clip->workspace_menu;
    clip->workspace_menu = NULL;

    /* Mark the intentional close so selection-done does not destroy the same
     * menu recursively while gtk_menu_popdown() is processing deactivation. */
    g_object_set_data(
        G_OBJECT(menu),
        "dock-clip-close-requested",
        GINT_TO_POINTER(1));
    gtk_menu_popdown(GTK_MENU(menu));
    gtk_widget_destroy(menu);
}

static void
dock_clip_menu_selection_done(
    GtkWidget *menu,
    gpointer user_data)
{
    DockClip *clip = user_data;

    if (g_object_get_data(
            G_OBJECT(menu),
            "dock-clip-close-requested"))
        return;

    if (clip && clip->workspace_menu == menu)
        clip->workspace_menu = NULL;

    gtk_widget_destroy(menu);
}

static void
dock_clip_show_workspace_menu(DockClip *clip)
{
    dock_clip_update_display(clip);

    /* A second click toggles the current menu instead of creating a second
     * popup, which can otherwise look glitchy when switching workspaces. */
    if (clip->workspace_menu) {
        dock_clip_close_workspace_menu(clip);
        return;
    }

    guint count = clip->number_of_desktops;
    guint current = clip->current_desktop;
    gchar **names = dock_clip_read_workspace_names(clip, count);
    GtkWidget *menu = gtk_menu_new();
    clip->workspace_menu = menu;
    GSList *group = NULL;

    for (guint i = 0; i < count; i++) {
        gchar *label;
        if (names && names[i] && *names[i]) {
            label = g_strdup_printf(
                "%u — %s",
                i + 1,
                names[i]);
        } else {
            label = g_strdup_printf("Workspace %u", i + 1);
        }

        GtkWidget *item = gtk_radio_menu_item_new_with_label(
            group,
            label);
        g_free(label);
        group = gtk_radio_menu_item_get_group(
            GTK_RADIO_MENU_ITEM(item));

        gtk_check_menu_item_set_active(
            GTK_CHECK_MENU_ITEM(item),
            i == current);
        g_object_set_data(
            G_OBJECT(item),
            "dock-clip-workspace-index",
            GUINT_TO_POINTER(i + 1));
        g_signal_connect(
            item,
            "activate",
            G_CALLBACK(dock_clip_workspace_activated),
            clip);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }

    g_strfreev(names);
    g_signal_connect(
        menu,
        "selection-done",
        G_CALLBACK(dock_clip_menu_selection_done),
        clip);

    gtk_widget_show_all(menu);
    gtk_menu_popup_at_widget(
        GTK_MENU(menu),
        clip->button,
        GDK_GRAVITY_SOUTH_WEST,
        GDK_GRAVITY_NORTH_WEST,
        NULL);
}

static void
dock_clip_button_clicked(
    GtkButton *button,
    gpointer user_data)
{
    (void)button;
    dock_clip_show_workspace_menu(user_data);
}

static gboolean
dock_clip_button_press(
    GtkWidget *widget,
    GdkEventButton *event,
    gpointer user_data)
{
    (void)widget;
    DockClip *clip = user_data;

    if (event->button != 3)
        return FALSE;

    dock_clip_show_workspace_menu(clip);
    return TRUE;
}

static gint
dock_clip_get_tile_size(DockClip *clip)
{
    gint icon_size = dock_get_icon_size(clip->dock);
    return MAX(icon_size + 16, (icon_size * 64 + 24) / 48);
}

static void
dock_clip_sync_dock_settings(DockClip *clip)
{
    if (!clip || !clip->dock || !clip->window)
        return;

    GtkWidget *dock_widget = dock_get_widget(clip->dock);
    GtkWidget *dock_toplevel =
        dock_widget ? gtk_widget_get_toplevel(dock_widget) : NULL;
    GdkDisplay *display =
        dock_toplevel ? gtk_widget_get_display(dock_toplevel) : clip->gdk_display;
    if (!display)
        return;

    gint monitor_count = gdk_display_get_n_monitors(display);
    gint monitor_index = dock_get_monitor_index(clip->dock);
    GdkMonitor *monitor = NULL;

    if (monitor_index >= 0 && monitor_index < monitor_count)
        monitor = gdk_display_get_monitor(display, monitor_index);
    if (!monitor)
        monitor = gdk_display_get_primary_monitor(display);
    if (!monitor)
        return;

    GdkRectangle geometry;
    gdk_monitor_get_geometry(monitor, &geometry);

    gint tile_size = dock_clip_get_tile_size(clip);
    gint x = geometry.x;
    /* If the Dock is on the left edge, sit immediately beside it instead
     * of overlapping the Dock's first tile. Otherwise use the monitor's
     * upper-left corner, like Window Maker's Clip. */
    if (!dock_get_right_side(clip->dock))
        x += tile_size;
    gint y = geometry.y;

    if (clip->last_x != x || clip->last_y != y) {
        gtk_window_move(GTK_WINDOW(clip->window), x, y);
        clip->last_x = x;
        clip->last_y = y;
    }

    if (clip->last_tile_size != tile_size) {
        gtk_widget_set_size_request(clip->window, tile_size, tile_size);
        gtk_widget_set_size_request(clip->button, tile_size, tile_size);
        gtk_window_resize(GTK_WINDOW(clip->window), tile_size, tile_size);
        clip->last_tile_size = tile_size;
    }

    gint icon_size = dock_get_icon_size(clip->dock);
    if (clip->last_icon_size != icon_size) {
        gtk_image_set_pixel_size(GTK_IMAGE(clip->workspace_image), icon_size);
        clip->last_icon_size = icon_size;
    }

    gint opacity = dock_get_opacity(clip->dock);
    if (clip->last_opacity != opacity) {
        gtk_widget_set_opacity(clip->window, opacity / 100.0);
        clip->last_opacity = opacity;
    }

    DockPositionMode mode = dock_get_position_mode(clip->dock);
    if (clip->last_position_mode != (gint)mode) {
        /* The Clip follows Crepido's own stacking policy rather than always
         * staying above application windows. */
        gtk_window_set_keep_above(
            GTK_WINDOW(clip->window),
            mode == DOCK_POSITION_KEEP_ON_TOP);
        gtk_window_set_keep_below(
            GTK_WINDOW(clip->window),
            mode == DOCK_POSITION_NORMAL);
        clip->last_position_mode = (gint)mode;
    }
}

static void
dock_clip_monitor_changed(
    GdkDisplay *display,
    GdkMonitor *monitor,
    gpointer user_data)
{
    (void)display;
    (void)monitor;
    dock_clip_sync_dock_settings(user_data);
}

static void
dock_clip_screen_changed(
    GdkScreen *screen,
    gpointer user_data)
{
    (void)screen;
    dock_clip_sync_dock_settings(user_data);
}

DockClip *
dock_clip_new(Dock *dock)
{
    g_return_val_if_fail(dock != NULL, NULL);

    GdkDisplay *gdk_display = gdk_display_get_default();
    if (!gdk_display || !GDK_IS_X11_DISPLAY(gdk_display))
        return NULL;

    DockClip *clip = g_new0(DockClip, 1);
    clip->dock = dock;
    clip->last_x = G_MININT;
    clip->last_y = G_MININT;
    clip->last_tile_size = G_MININT;
    clip->last_icon_size = G_MININT;
    clip->last_opacity = G_MININT;
    clip->last_position_mode = G_MININT;
    clip->current_desktop = 0;
    clip->number_of_desktops = 1;
    clip->gdk_display = g_object_ref(gdk_display);
    clip->screen = g_object_ref(gdk_display_get_default_screen(gdk_display));
    clip->display = gdk_x11_display_get_xdisplay(gdk_display);
    clip->root = DefaultRootWindow(clip->display);

    clip->atom_current_desktop = XInternAtom(
        clip->display, "_NET_CURRENT_DESKTOP", False);
    clip->atom_number_of_desktops = XInternAtom(
        clip->display, "_NET_NUMBER_OF_DESKTOPS", False);
    clip->atom_desktop_names = XInternAtom(
        clip->display, "_NET_DESKTOP_NAMES", False);
    clip->atom_utf8_string = XInternAtom(
        clip->display, "UTF8_STRING", False);

    clip->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(clip->window), "Crepido Clip");
    gtk_window_set_decorated(GTK_WINDOW(clip->window), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(clip->window), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(clip->window), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(clip->window), TRUE);
    gtk_window_set_accept_focus(GTK_WINDOW(clip->window), FALSE);
    gtk_window_set_focus_on_map(GTK_WINDOW(clip->window), FALSE);
    gtk_window_set_type_hint(
        GTK_WINDOW(clip->window),
        GDK_WINDOW_TYPE_HINT_DOCK);
    gtk_window_stick(GTK_WINDOW(clip->window));

    clip->button = gtk_button_new();
    gtk_style_context_add_class(
        gtk_widget_get_style_context(clip->button),
        "crepido-bitmap-block");
    gtk_button_set_relief(GTK_BUTTON(clip->button), GTK_RELIEF_NORMAL);
    gtk_widget_set_can_focus(clip->button, FALSE);
    gtk_widget_set_focus_on_click(clip->button, FALSE);
    gtk_widget_add_events(clip->button, GDK_BUTTON_PRESS_MASK);

    GtkWidget *overlay = gtk_overlay_new();
    clip->workspace_image = gtk_image_new_from_icon_name(
        "mail-attachment",
        GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(
        GTK_IMAGE(clip->workspace_image),
        dock_get_icon_size(dock));
    gtk_container_add(
        GTK_CONTAINER(overlay),
        clip->workspace_image);

    /* Show the active workspace as a small themed badge, like Crepido's
     * window-count overlays, instead of a second line of text in the tile. */
    clip->workspace_label = gtk_label_new("1");
    gtk_widget_set_halign(clip->workspace_label, GTK_ALIGN_START);
    gtk_widget_set_valign(clip->workspace_label, GTK_ALIGN_END);
    gtk_widget_set_margin_start(clip->workspace_label, 2);
    gtk_widget_set_margin_bottom(clip->workspace_label, 1);
    gtk_overlay_add_overlay(
        GTK_OVERLAY(overlay),
        clip->workspace_label);

    gtk_container_add(GTK_CONTAINER(clip->button), overlay);
    gtk_container_add(GTK_CONTAINER(clip->window), clip->button);

    g_signal_connect(
        clip->button,
        "clicked",
        G_CALLBACK(dock_clip_button_clicked),
        clip);
    g_signal_connect(
        clip->button,
        "button-press-event",
        G_CALLBACK(dock_clip_button_press),
        clip);

    clip->monitor_added_handler = g_signal_connect(
        clip->gdk_display,
        "monitor-added",
        G_CALLBACK(dock_clip_monitor_changed),
        clip);
    clip->monitor_removed_handler = g_signal_connect(
        clip->gdk_display,
        "monitor-removed",
        G_CALLBACK(dock_clip_monitor_changed),
        clip);
    clip->monitors_changed_handler = g_signal_connect(
        clip->screen,
        "monitors-changed",
        G_CALLBACK(dock_clip_screen_changed),
        clip);
    clip->screen_size_changed_handler = g_signal_connect(
        clip->screen,
        "size-changed",
        G_CALLBACK(dock_clip_screen_changed),
        clip);

    gtk_widget_show_all(clip->window);
    dock_clip_sync_dock_settings(clip);
    dock_clip_update_display(clip);
    clip->refresh_id = g_timeout_add(250, dock_clip_refresh, clip);

    return clip;
}

void
dock_clip_free(DockClip *clip)
{
    if (!clip)
        return;

    if (clip->refresh_id) {
        g_source_remove(clip->refresh_id);
        clip->refresh_id = 0;
    }

    if (clip->gdk_display) {
        if (clip->monitor_added_handler)
            g_signal_handler_disconnect(
                clip->gdk_display,
                clip->monitor_added_handler);
        if (clip->monitor_removed_handler)
            g_signal_handler_disconnect(
                clip->gdk_display,
                clip->monitor_removed_handler);
    }

    if (clip->screen) {
        if (clip->monitors_changed_handler)
            g_signal_handler_disconnect(
                clip->screen,
                clip->monitors_changed_handler);
        if (clip->screen_size_changed_handler)
            g_signal_handler_disconnect(
                clip->screen,
                clip->screen_size_changed_handler);
    }

    /* Ensure no popup retains a callback pointer to the Clip during shutdown. */
    dock_clip_close_workspace_menu(clip);

    if (clip->window)
        gtk_widget_destroy(clip->window);

    g_clear_object(&clip->screen);
    g_clear_object(&clip->gdk_display);
    g_free(clip);
}
